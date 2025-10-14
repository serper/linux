// SPDX-License-Identifier: GPL-2.0
//
// Minimal V4L2 mem2mem driver skeleton for Allwinner G2D
// Features MVP: XRGB8888 OUTPUT -> XRGB8888 CAPTURE, scale if WxH differ
// Author: tú+yomismo

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define DRV_NAME "sunxi-g2d-m2m"

// ========= DT binding =========
// Placeholder compatibles; ajusta según tu DTS/BSP
static const struct of_device_id sunxi_g2d_of_match[] = {
	{ .compatible = "allwinner,t113-g2d" }, // TODO: ajusta al compatible real
	{ .compatible = "allwinner,sunxi-g2d" },
	{}
};
MODULE_DEVICE_TABLE(of, sunxi_g2d_of_match);

// ========= HW regs base =========
// TODO(G2D): define aquí offsets de registros, bits de IRQ, start, cfg pipes, scale,
// src/dst stride, formatos, etc. copiados/adaptados del BSP/Tina.
#define G2D_REG_SIZE   0x10000

struct sunxi_g2d_fmt {
	u32 fourcc;
	u8  depth;      // bits per pixel
	u8  num_planes; // 1 por ahora
};

static const struct sunxi_g2d_fmt g_formats[] = {
	{ .fourcc = V4L2_PIX_FMT_XRGB32, .depth = 32, .num_planes = 1 }, // XRGB8888
};

static const struct sunxi_g2d_fmt *find_fmt(u32 fourcc)
{
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(g_formats); i++)
		if (g_formats[i].fourcc == fourcc)
			return &g_formats[i];
	return NULL;
}

struct sunxi_g2d_ctx;

struct sunxi_g2d_dev {
	struct device *dev;
	void __iomem *mmio;
	struct clk *clk;
	struct reset_control *rst;
	int irq;

	struct v4l2_device v4l2_dev;
	struct video_device vfd;
	struct v4l2_m2m_dev *m2m_dev;

	struct mutex dev_mutex; // serializa open/release
	spinlock_t irqlock;

	// job state
};

struct sunxi_g2d_ctx {
	struct sunxi_g2d_dev *g2d;
	struct v4l2_fh fh;

	// formats
	struct v4l2_pix_format out_fmt; // OUTPUT
	struct v4l2_pix_format cap_fmt; // CAPTURE

	// vb2 queues
	struct vb2_queue out_q;
	struct vb2_queue cap_q;

	// param job actual (calculado al submit)
	bool needs_scale;
};

// ========== VB2 ops ==========
static int qbuf_queue_setup(struct vb2_queue *q,
			    unsigned int *nbufs, unsigned int *nplanes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(q);
	struct v4l2_pix_format *pf =
		(q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) ? &ctx->out_fmt : &ctx->cap_fmt;

	unsigned int size = pf->sizeimage;
	if (!size)
		return -EINVAL;

	if (*nplanes) {
		if (sizes[0] < size)
			return -EINVAL;
	} else {
		*nplanes = 1;
		sizes[0] = size;
	}
	return 0;
}

static int qbuf_buf_prepare(struct vb2_buffer *vb)
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format *pf =
		(vb->vb2_queue->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) ? &ctx->out_fmt : &ctx->cap_fmt;

	unsigned long size = pf->sizeimage;
	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void qbuf_buf_queue(struct vb2_buffer *vb)
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct sunxi_g2d_dev *g2d = ctx->g2d;

	// Cola M2M estándar: pasa el buffer a v4l2_m2m
	if (vb->vb2_queue->type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vb);
	else
		v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vb);

	// Intenta arrancar job si hay OUT+CAP encolados
	v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
}

static const struct vb2_ops qbuf_qops = {
	.queue_setup    = qbuf_queue_setup,
	.buf_prepare    = qbuf_buf_prepare,
	.buf_queue      = qbuf_buf_queue,
	.wait_prepare   = vb2_ops_wait_prepare,
	.wait_finish    = vb2_ops_wait_finish,
};

// ========== M2M device_run / job_complete ==========
static void g2d_device_run(void *priv)
{
	struct sunxi_g2d_ctx *ctx = priv;
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	unsigned long flags;

	// Saca un par OUT+CAP
	struct vb2_v4l2_buffer *src, *dst;
	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!src || !dst) {
		// nada que hacer
		return;
	}

	// Calcula si hay que escalar
	ctx->needs_scale = (ctx->out_fmt.width  != ctx->cap_fmt.width) ||
			   (ctx->out_fmt.height != ctx->cap_fmt.height);

	// Direcciones físicas (CMA)
	dma_addr_t src_dma = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	dma_addr_t dst_dma = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);

	u32 src_w = ctx->out_fmt.width;
	u32 src_h = ctx->out_fmt.height;
	u32 dst_w = ctx->cap_fmt.width;
	u32 dst_h = ctx->cap_fmt.height;
	u32 src_pitch = ctx->out_fmt.bytesperline;
	u32 dst_pitch = ctx->cap_fmt.bytesperline;

	// ==== PROGRAMAR G2D ====
	// 1) Enciende reloj/reset si hace falta por job
	// 2) Escribe src base addr, dst base addr, strides, tamaños
	// 3) Configura operación: BLIT o SCALE (coeficientes) XRGB8888
	// 4) Dispara job y habilita IRQ de "frame done"
	// TODO(G2D): programar registros concretos
	// iowrite32(..., g2d->mmio + REG_SRC_ADDR);
	// iowrite32(..., g2d->mmio + REG_DST_ADDR);
	// iowrite32(..., g2d->mmio + REG_CTRL);
	// iowrite32(..., g2d->mmio + REG_START);

	// Para el MVP, simulamos “done” inmediato (sin IRQ) para encajar con userspace
	// === ELIMINA esta simulación cuando programes IRQ ===
	dst->sequence = src->sequence;
	dst->field = V4L2_FIELD_NONE;
	dst->vb2_buf.timestamp = ktime_get_ns();
	v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
	// Lanza siguiente si hay
	v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
}

static irqreturn_t g2d_irq(int irq, void *data)
{
	struct sunxi_g2d_dev *g2d = data;
	unsigned long flags;

	// TODO(G2D): leer status, limpiar IRQ, finalizar job actual:
	// v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);

	return IRQ_HANDLED;
}

static const struct v4l2_m2m_ops g2d_m2m_ops = {
	.device_run = g2d_device_run,
};

// ========== IOCTLs ==========
static int g2d_try_fmt(struct file *filp, void *priv, struct v4l2_format *f)
{
	const struct sunxi_g2d_fmt *fmt = find_fmt(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	// clamp tamaños
	if (!f->fmt.pix.width)  f->fmt.pix.width  = 16;
	if (!f->fmt.pix.height) f->fmt.pix.height = 16;

	// obliga a alineación si quieres (p.ej. múltiplos de 2)
	f->fmt.pix.bytesperline = (f->fmt.pix.width * (fmt->depth/8));
	f->fmt.pix.sizeimage = f->fmt.pix.bytesperline * f->fmt.pix.height;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	return 0;
}

static int g2d_s_fmt_out(struct file *filp, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = priv;
	int ret = g2d_try_fmt(filp, priv, f);
	if (ret) return ret;
	ctx->out_fmt = f->fmt.pix;
	return 0;
}

static int g2d_s_fmt_cap(struct file *filp, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = priv;
	int ret = g2d_try_fmt(filp, priv, f);
	if (ret) return ret;
	ctx->cap_fmt = f->fmt.pix;
	return 0;
}

static int g2d_reqbufs(struct file *filp, void *priv, struct v4l2_requestbuffers *rb)
{
	return 0; // vb2 ioctl helpers harán el trabajo real
}

static const struct v4l2_ioctl_ops g2d_ioctl_ops = {
	.vidioc_querycap                = v4l2_m2m_ioctl_querycap,
	.vidioc_enum_fmt_vid_cap        = v4l2_ioctl_enum_fmt_vid_cap,   // opcional: limita a XRGB8888
	.vidioc_enum_fmt_vid_out        = v4l2_ioctl_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out           = v4l2_m2m_ioctl_g_fmt_vid_out,
	.vidioc_s_fmt_vid_out           = g2d_s_fmt_out,
	.vidioc_try_fmt_vid_out         = g2d_try_fmt,
	.vidioc_g_fmt_vid_cap           = v4l2_m2m_ioctl_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap           = g2d_s_fmt_cap,
	.vidioc_try_fmt_vid_cap         = g2d_try_fmt,

	.vidioc_reqbufs                 = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf                = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf                    = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf                   = v4l2_m2m_ioctl_dqbuf,

	.vidioc_streamon                = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff               = v4l2_m2m_ioctl_streamoff,
};

// ========== File ops / vdev ==========
static int g2d_open(struct file *filp)
{
	struct sunxi_g2d_dev *g2d = video_drvdata(filp);
	struct sunxi_g2d_ctx *ctx;
	int ret;

	mutex_lock(&g2d->dev_mutex);

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) { ret = -ENOMEM; goto err; }

	ctx->g2d = g2d;
	v4l2_fh_init(&ctx->fh, &g2d->vfd);
	filp->private_data = &ctx->fh;

	// formatos por defecto
	ctx->out_fmt.pixelformat = V4L2_PIX_FMT_XRGB32;
	ctx->out_fmt.width = 640; ctx->out_fmt.height = 360;
	ctx->out_fmt.bytesperline = 640*4; ctx->out_fmt.sizeimage = 640*360*4;

	ctx->cap_fmt.pixelformat = V4L2_PIX_FMT_XRGB32;
	ctx->cap_fmt.width = 320; ctx->cap_fmt.height = 180;
	ctx->cap_fmt.bytesperline = 320*4; ctx->cap_fmt.sizeimage = 320*180*4;

	// vb2 queues
	ctx->out_q.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ctx->out_q.io_modes = VB2_MMAP;
	ctx->out_q.drv_priv = ctx;
	ctx->out_q.buf_struct_size = sizeof(struct vb2_v4l2_buffer);
	ctx->out_q.ops = &qbuf_qops;
	ctx->out_q.mem_ops = &vb2_dma_contig_memops;
	ctx->out_q.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	ctx->out_q.lock = &g2d->dev_mutex;
	ctx->out_q.dev = g2d->dev;
	ret = vb2_queue_init(&ctx->out_q); if (ret) goto err_fh;

	ctx->cap_q = ctx->out_q;
	ctx->cap_q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = vb2_queue_init(&ctx->cap_q); if (ret) goto err_fh;

	// m2m context
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(g2d->m2m_dev, ctx, g2d_device_run);
	if (IS_ERR(ctx->fh.m2m_ctx)) { ret = PTR_ERR(ctx->fh.m2m_ctx); goto err_fh; }

	v4l2_fh_add(&ctx->fh);
	mutex_unlock(&g2d->dev_mutex);
	return 0;

err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
err:
	mutex_unlock(&g2d->dev_mutex);
	return ret;
}

static int g2d_release(struct file *filp)
{
	struct sunxi_g2d_dev *g2d = video_drvdata(filp);
	struct v4l2_fh *fh = filp->private_data;
	struct sunxi_g2d_ctx *ctx = container_of(fh, struct sunxi_g2d_ctx, fh);

	mutex_lock(&g2d->dev_mutex);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&g2d->dev_mutex);
	return 0;
}

static const struct v4l2_file_operations g2d_fops = {
	.owner          = THIS_MODULE,
	.open           = g2d_open,
	.release        = g2d_release,
	.unlocked_ioctl = video_ioctl2,
	.poll           = v4l2_m2m_fop_poll,
	.mmap           = v4l2_m2m_fop_mmap,
};

// ========== Probe / Remove ==========
static int sunxi_g2d_probe(struct platform_device *pdev)
{
	struct sunxi_g2d_dev *g2d;
	struct resource *res;
	int ret;

	g2d = devm_kzalloc(&pdev->dev, sizeof(*g2d), GFP_KERNEL);
	if (!g2d) return -ENOMEM;

	g2d->dev = &pdev->dev;
	mutex_init(&g2d->dev_mutex);
	spin_lock_init(&g2d->irqlock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	g2d->mmio = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(g2d->mmio)) return PTR_ERR(g2d->mmio);

	/* Use named clock + optional reset following common sunxi patterns */
	g2d->clk = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(g2d->clk)) {
		dev_warn(&pdev->dev, "bus clock not found, trying fallback\n");
		g2d->clk = devm_clk_get(&pdev->dev, NULL);
	}
	if (!IS_ERR(g2d->clk)) {
		ret = clk_prepare_enable(g2d->clk);
		if (ret) goto err_clk;
	} else {
		dev_warn(&pdev->dev, "no clock available for g2d, continuing\n");
	}

	/* reset may be optional on some boards */
	g2d->rst = devm_reset_control_get_optional_exclusive(&pdev->dev, "bus");
	if (IS_ERR(g2d->rst)) {
		if (PTR_ERR(g2d->rst) == -EPROBE_DEFER)
			goto err_clk;
		g2d->rst = NULL;
	}
	if (g2d->rst)
		reset_control_deassert(g2d->rst);

	g2d->irq = platform_get_irq(pdev, 0);
	if (g2d->irq > 0) {
		ret = devm_request_irq(&pdev->dev, g2d->irq, g2d_irq, 0, DRV_NAME, g2d);
		if (ret) goto err_clk;
	}

	ret = v4l2_device_register(&pdev->dev, &g2d->v4l2_dev);
	if (ret) goto err_clk;

	g2d->m2m_dev = v4l2_m2m_init(&g2d_m2m_ops);
	if (IS_ERR(g2d->m2m_dev)) { ret = PTR_ERR(g2d->m2m_dev); goto err_v4l2; }

	strscpy(g2d->vfd.name, DRV_NAME, sizeof(g2d->vfd.name));
	g2d->vfd.v4l2_dev = &g2d->v4l2_dev;
	g2d->vfd.fops = &g2d_fops;
	g2d->vfd.ioctl_ops = &g2d_ioctl_ops;
	g2d->vfd.lock = &g2d->dev_mutex;
	g2d->vfd.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	video_set_drvdata(&g2d->vfd, g2d);

	ret = video_register_device(&g2d->vfd, VFL_TYPE_VIDEO, -1);
	if (ret) goto err_m2m;

	dev_info(&pdev->dev, "sunxi G2D mem2mem registered as /dev/video%d\n",
		 g2d->vfd.minor);

	return 0;

err_m2m:
	v4l2_m2m_release(g2d->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&g2d->v4l2_dev);
err_clk:
	if (!IS_ERR(g2d->rst))
		reset_control_assert(g2d->rst);
	clk_disable_unprepare(g2d->clk);
	return ret;
}

static int sunxi_g2d_remove(struct platform_device *pdev)
{
	struct sunxi_g2d_dev *g2d = platform_get_drvdata(pdev);

	video_unregister_device(&g2d->vfd);
	v4l2_m2m_release(g2d->m2m_dev);
	v4l2_device_unregister(&g2d->v4l2_dev);
	if (!IS_ERR(g2d->rst))
		reset_control_assert(g2d->rst);
	clk_disable_unprepare(g2d->clk);
	return 0;
}

static struct platform_driver sunxi_g2d_driver = {
	.probe  = sunxi_g2d_probe,
	.remove = sunxi_g2d_remove,
	.driver = {
		.name           = DRV_NAME,
		.of_match_table = sunxi_g2d_of_match,
	},
};

module_platform_driver(sunxi_g2d_driver);

MODULE_DESCRIPTION("Allwinner Sunxi G2D V4L2 mem2mem (skeleton)");
MODULE_AUTHOR("Sergio Perez <sergio@pereznus.es>");
MODULE_LICENSE("GPL");
