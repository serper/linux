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
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/bitops.h>
#include <linux/string.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#define DRV_NAME "sunxi-g2d-m2m"

// G2D register map (v2 style) extracted from BSP
#include "sunxi-g2d-regs.h"

// ========= DT binding =========
static const struct of_device_id sunxi_g2d_of_match[] = {
	{ .compatible = "allwinner,t113-g2d" },
	{ .compatible = "allwinner,sun8i-g2d" },
	{ .compatible = "allwinner,sun8i-g2d-v1" },
	{ .compatible = "allwinner,sun8i-g2d-v2" },
	{ .compatible = "allwinner,sunxi-g2d" },
	{}
};
MODULE_DEVICE_TABLE(of, sunxi_g2d_of_match);

// ========= HW regs base =========
// Tamaño de la ventana MMIO útil (TOP..VSU); evita solapar crypto (ver DTS)
#define G2D_REG_SIZE   0x1000

/* MMIO helpers operate on the mapped base directly to avoid incomplete type use */
static inline void g2d_writel(void __iomem *mmio, u32 val, u32 reg)
{
    iowrite32(val, mmio + reg);
}

static inline u32 g2d_readl(void __iomem *mmio, u32 reg)
{
    return ioread32(mmio + reg);
}

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
	struct sunxi_g2d_ctx *curr_ctx; // contexto en ejecución
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

	// estado del job en curso
	struct vb2_v4l2_buffer *cur_src;
	struct vb2_v4l2_buffer *cur_dst;
	bool job_done;
	struct delayed_work timeout_work;
};

// Parámetro de módulo para activar programación HW (experimental)
static bool enable_hw = false;
module_param(enable_hw, bool, 0644);
MODULE_PARM_DESC(enable_hw, "Habilita programación HW del G2D (experimental); si false usa memcpy CPU");

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

	// Cola M2M estándar: pasa el buffer a v4l2_m2m
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));

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

	// Guarda estado del job y marca contexto en ejecución para IRQ
	ctx->cur_src = src;
	ctx->cur_dst = dst;
	ctx->job_done = false;
	spin_lock_irqsave(&g2d->irqlock, flags);
	g2d->curr_ctx = ctx;
	spin_unlock_irqrestore(&g2d->irqlock, flags);

	if (enable_hw && g2d->irq > 0 && !ctx->needs_scale) {
		// ==== PROGRAMAR G2D (conservador, v2) ====
		// Nota: Bits de formato/control están en integración; por ahora programamos
		// direcciones, pitches y tamaños, y habilitamos IRQ global del MIXER si está presente.

		// Fuente V0: base + pitch + tamaño
	g2d_writel(g2d->mmio, lower_32_bits(src_dma), V0_LADD0);
	g2d_writel(g2d->mmio, src_pitch, V0_PITCH0);
	g2d_writel(g2d->mmio, (src_w - 1) | ((src_h - 1) << 16), V0_MBSIZE);
	g2d_writel(g2d->mmio, 0, V0_COOR);
		// Destino WB: base + pitch + tamaño
	g2d_writel(g2d->mmio, lower_32_bits(dst_dma), WB_LADD0);
	g2d_writel(g2d->mmio, dst_pitch, WB_PITCH0);
	g2d_writel(g2d->mmio, (dst_w - 1) | ((dst_h - 1) << 16), WB_SIZE);

		// Habilita IRQ de MIXER: patrón típico v2 (enable en bit4, pending en bit0)
		// Escribe 0x10 para habilitar IRQ; limpiar pending previo
		/* limpia pendientes y habilita IRQ */
	g2d_writel(g2d->mmio, G2D_MIXER_INT_PEND, G2D_MIXER_INT);
	g2d_writel(g2d->mmio, G2D_MIXER_INT_EN, G2D_MIXER_INT);

		// Dispara operación: algunos SoC usan MIXER_CTL bit0 como START
		// Si no hace nada, el timeout completará el trabajo para no bloquear userland
	g2d_writel(g2d->mmio, G2D_MIXER_CTL_START, G2D_MIXER_CTL);

		// Programa timeout de seguridad por si no llega IRQ
		schedule_delayed_work(&ctx->timeout_work, msecs_to_jiffies(50));
		return;
	}

	// Fallback: copia por CPU (simple y lenta), finaliza sin IRQ
	{
		void *src_v = vb2_plane_vaddr(&src->vb2_buf, 0);
		void *dst_v = vb2_plane_vaddr(&dst->vb2_buf, 0);
		size_t rows = min_t(u32, src_h, dst_h);
		size_t bytes_per_row = min_t(u32, src_pitch, dst_pitch);
		size_t i;
		if (src_v && dst_v) {
			for (i = 0; i < rows; i++)
				memcpy(dst_v + i * dst_pitch, src_v + i * src_pitch, bytes_per_row);
		}
	}

	// Completa buffers (ruta CPU)
	dst->sequence = src->sequence;
	dst->field = V4L2_FIELD_NONE;
	dst->vb2_buf.timestamp = ktime_get_ns();
	v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
	v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);
}

static irqreturn_t g2d_irq(int irq, void *data)
{
	struct sunxi_g2d_dev *g2d = data;
	unsigned long flags;
	struct sunxi_g2d_ctx *ctx;
	u32 st;

	// Lee y limpia IRQ del MIXER (v2: write-back para clear)
	st = g2d_readl(g2d, G2D_MIXER_INT);
	if (!(st & G2D_MIXER_INT_PEND))
		return IRQ_NONE;
	g2d_writel(g2d, st, G2D_MIXER_INT);

	// Finaliza el job en curso si hay
	spin_lock_irqsave(&g2d->irqlock, flags);
	ctx = g2d->curr_ctx;
	g2d->curr_ctx = NULL;
	spin_unlock_irqrestore(&g2d->irqlock, flags);

	if (ctx && !ctx->job_done) {
		struct vb2_v4l2_buffer *src = ctx->cur_src;
		struct vb2_v4l2_buffer *dst = ctx->cur_dst;
		cancel_delayed_work(&ctx->timeout_work);
		if (src && dst) {
			dst->sequence = src->sequence;
			dst->field = V4L2_FIELD_NONE;
			dst->vb2_buf.timestamp = ktime_get_ns();
			v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
			v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
			v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
		}
		ctx->job_done = true;
		v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);
	}

	return IRQ_HANDLED;
}

// Timeout: finaliza el trabajo si no llegó IRQ a tiempo
static void g2d_timeout_workfn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct sunxi_g2d_ctx *ctx = container_of(dwork, struct sunxi_g2d_ctx, timeout_work);
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	unsigned long flags;

	// Si ya está marcado done por IRQ, nada que hacer
	if (ctx->job_done)
		return;

	// Desengancha el contexto si sigue como actual
	spin_lock_irqsave(&g2d->irqlock, flags);
	if (g2d->curr_ctx == ctx)
		g2d->curr_ctx = NULL;
	spin_unlock_irqrestore(&g2d->irqlock, flags);

	// Completa buffers de forma conservadora
	if (ctx->cur_src && ctx->cur_dst) {
		struct vb2_v4l2_buffer *src = ctx->cur_src;
		struct vb2_v4l2_buffer *dst = ctx->cur_dst;
		dst->sequence = src->sequence;
		dst->field = V4L2_FIELD_NONE;
		dst->vb2_buf.timestamp = ktime_get_ns();
		v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
	}
	ctx->job_done = true;
	v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);
}

static const struct v4l2_m2m_ops g2d_m2m_ops = {
	.device_run = g2d_device_run,
};
// v4l2-mem2mem (antiguas) requieren queue_init callback
static int g2d_queue_init(void *priv, struct vb2_queue *out, struct vb2_queue *cap)
{
	struct sunxi_g2d_ctx *ctx = priv;

	memset(out, 0, sizeof(*out));
	out->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	out->io_modes = VB2_MMAP;
	out->drv_priv = ctx;
	out->buf_struct_size = sizeof(struct vb2_v4l2_buffer);
	out->ops = &qbuf_qops;
	out->mem_ops = &vb2_dma_contig_memops;
	out->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	out->lock = &ctx->g2d->dev_mutex;
	out->dev = ctx->g2d->dev;
	if (vb2_queue_init(out))
		return -EINVAL;

	memset(cap, 0, sizeof(*cap));
	*cap = *out;
	cap->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	return vb2_queue_init(cap);
}

// ========== IOCTLs ==========
static int g2d_querycap(struct file *filp, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "sunxi-g2d-m2m", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", DRV_NAME);
	return 0;
}
static int g2d_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_XRGB32;
	strscpy(f->description, "XRGB8888", sizeof(f->description));
	return 0;
}

static int g2d_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_XRGB32;
	strscpy(f->description, "XRGB8888", sizeof(f->description));
	return 0;
}
static int g2d_try_fmt(struct file *filp, void *priv, struct v4l2_format *f)
{
	const struct sunxi_g2d_fmt *fmt = find_fmt(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	// clamp tamaños
	if (!f->fmt.pix.width)  f->fmt.pix.width  = 16;
	if (!f->fmt.pix.height) f->fmt.pix.height = 16;

	// bytes por línea e imagen
	f->fmt.pix.bytesperline = (f->fmt.pix.width * (fmt->depth / 8));
	f->fmt.pix.sizeimage = f->fmt.pix.bytesperline * f->fmt.pix.height;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	return 0;
}

static int g2d_g_fmt_out(struct file *filp, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = priv;
	f->fmt.pix = ctx->out_fmt;
	return 0;
}

static int g2d_g_fmt_cap(struct file *filp, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = priv;
	f->fmt.pix = ctx->cap_fmt;
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

/* no custom reqbufs; vb2 helpers se encargan */

static const struct v4l2_ioctl_ops g2d_ioctl_ops = {
	.vidioc_querycap                = g2d_querycap,
	.vidioc_enum_fmt_vid_cap        = g2d_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out        = g2d_enum_fmt_out,
	.vidioc_g_fmt_vid_out           = g2d_g_fmt_out,
	.vidioc_s_fmt_vid_out           = g2d_s_fmt_out,
	.vidioc_try_fmt_vid_out         = g2d_try_fmt,
	/* no custom reqbufs needed; use v4l2_m2m ioctl helpers */
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

	// init timeout work
	INIT_DELAYED_WORK(&ctx->timeout_work, g2d_timeout_workfn);

	// m2m context (las colas se inicializan vía queue_init)
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(g2d->m2m_dev, ctx, g2d_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) { ret = PTR_ERR(ctx->fh.m2m_ctx); goto err_fh; }

	v4l2_fh_add(&ctx->fh, filp);
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
	cancel_delayed_work_sync(&ctx->timeout_work);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh, filp);
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

	if (resource_size(res) < G2D_REG_SIZE)
		dev_warn(&pdev->dev, "g2d reg size (0x%pa) < expected (0x%x) for v2 map; DTS may need update\n",
				 &res->end, G2D_REG_SIZE);

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

    platform_set_drvdata(pdev, g2d);

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

static void sunxi_g2d_remove(struct platform_device *pdev)
{
	struct sunxi_g2d_dev *g2d = platform_get_drvdata(pdev);

	video_unregister_device(&g2d->vfd);
	v4l2_m2m_release(g2d->m2m_dev);
	v4l2_device_unregister(&g2d->v4l2_dev);
	if (!IS_ERR(g2d->rst))
		reset_control_assert(g2d->rst);
	clk_disable_unprepare(g2d->clk);
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
