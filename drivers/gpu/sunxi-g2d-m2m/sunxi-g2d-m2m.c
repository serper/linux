// SPDX-License-Identifier: GPL-2.0
//
// Allwinner T113-S3 G2D V4L2 mem2mem driver
// Based on fillrect v1.0.0 STABLE - DIRECT mode only
//
// Author: Sergio + AI Assistant

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/interconnect.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#define DRV_NAME "sunxi-g2d-m2m"
#define DRV_VERSION "1.0.0"

/* Enable hardware (for testing, disable to use CPU memcpy) */
static bool enable_hw = true;
module_param(enable_hw, bool, 0644);
MODULE_PARM_DESC(enable_hw, "Enable hardware acceleration (1=HW, 0=CPU memcpy)");

/* G2D register definitions (from fillrect v1.0.0) */
#include "sunxi-g2d-regs.h"

/* ===== Hardware structures ===== */

struct sunxi_g2d_dev {
	struct device *dev;
	void __iomem *mmio;
	resource_size_t mmio_size;
	void __iomem *ccu;	/* T113 CCU for clocks */
	
	struct clk *clk_bus;
	struct clk *clk_mod;
	struct clk *clk_mbus;
	struct reset_control *rst;
	struct icc_path *mbus;	/* MBUS interconnect */
	
	int irq;
	
	struct v4l2_device v4l2_dev;
	struct v4l2_m2m_dev *m2m_dev;
	struct video_device vfd;
	
	struct mutex dev_mutex;	/* Protects m2m context */
};

struct sunxi_g2d_ctx {
	struct v4l2_fh fh;
	struct sunxi_g2d_dev *g2d;
	
	/* Format configuration */
	struct v4l2_pix_format out_fmt;	/* OUTPUT (source) */
	struct v4l2_pix_format cap_fmt;	/* CAPTURE (dest) */
};

/* Supported pixel format */
static const struct v4l2_fmtdesc g2d_formats[] = {
	{
		.description = "32-bit XRGB 8-8-8-8",
		.pixelformat = V4L2_PIX_FMT_XRGB32,
		.flags = 0,
	},
};

#define NUM_FORMATS ARRAY_SIZE(g2d_formats)

/* ===== Register access helpers ===== */

static inline void g2d_write(struct sunxi_g2d_dev *g2d, u32 reg, u32 val)
{
	iowrite32(val, g2d->mmio + reg);
}

static inline u32 g2d_read(struct sunxi_g2d_dev *g2d, u32 reg)
{
	return ioread32(g2d->mmio + reg);
}

/* ===== V4L2 format helpers ===== */

static const struct v4l2_fmtdesc *find_format(u32 pixelformat)
{
	unsigned int i;
	
	for (i = 0; i < NUM_FORMATS; i++) {
		if (g2d_formats[i].pixelformat == pixelformat)
			return &g2d_formats[i];
	}
	
	return NULL;
}

static void g2d_get_default_format(struct v4l2_pix_format *pix)
{
	pix->width = 64;
	pix->height = 64;
	pix->pixelformat = V4L2_PIX_FMT_XRGB32;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = pix->width * 4;
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
}

/* ===== V4L2 queue operations ===== */

static int g2d_queue_setup(struct vb2_queue *vq,
			   unsigned int *nbuffers, unsigned int *nplanes,
			   unsigned int sizes[], struct device *alloc_devs[])
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *pix;
	
	pr_info("%s: type=%d nbuffers=%u\n", __func__, vq->type, *nbuffers);
	
	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix = &ctx->out_fmt;
	else
		pix = &ctx->cap_fmt;
	
	if (*nplanes) {
		if (sizes[0] < pix->sizeimage)
			return -EINVAL;
	} else {
		*nplanes = 1;
		sizes[0] = pix->sizeimage;
	}
	
	return 0;
}

static int g2d_buf_prepare(struct vb2_buffer *vb)
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format *pix;
	
	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type))
		pix = &ctx->out_fmt;
	else
		pix = &ctx->cap_fmt;
	
	if (vb2_plane_size(vb, 0) < pix->sizeimage)
		return -EINVAL;
	
	vb2_set_plane_payload(vb, 0, pix->sizeimage);
	
	return 0;
}

static void g2d_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int g2d_start_streaming(struct vb2_queue *q, unsigned int count)
{
	pr_info("%s: type=%d count=%u\n", __func__, q->type, count);
	return 0;
}

static void g2d_stop_streaming(struct vb2_queue *q)
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;
	
	pr_info("%s: type=%d\n", __func__, q->type);
	
	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops g2d_qops = {
	.queue_setup	 = g2d_queue_setup,
	.buf_prepare	 = g2d_buf_prepare,
	.buf_queue	 = g2d_buf_queue,
	.start_streaming = g2d_start_streaming,
	.stop_streaming	 = g2d_stop_streaming,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
};

static int g2d_queue_init(void *priv, struct vb2_queue *src_vq,
			  struct vb2_queue *dst_vq)
{
	struct sunxi_g2d_ctx *ctx = priv;
	int ret;
	
	/* Source queue (OUTPUT) */
	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &g2d_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->g2d->dev_mutex;
	src_vq->dev = ctx->g2d->dev;
	
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;
	
	/* Destination queue (CAPTURE) */
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &g2d_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->g2d->dev_mutex;
	dst_vq->dev = ctx->g2d->dev;
	
	return vb2_queue_init(dst_vq);
}

/* ===== Hardware operations (from fillrect v1.0.0) ===== */

static irqreturn_t g2d_irq_handler(int irq, void *data)
{
	struct sunxi_g2d_dev *g2d = data;
	struct sunxi_g2d_ctx *ctx;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	u32 status;
	
	status = g2d_read(g2d, G2D_MIXER_INT);
	
	pr_info("%s: status=0x%08x\n", __func__, status);
	
	/* Check if it's our interrupt */
	if (!(status & G2D_MIXER_INT_IRQ_PENDING))
		return IRQ_NONE;
	
	/* Clear interrupt */
	g2d_write(g2d, G2D_MIXER_INT, G2D_MIXER_INT_IRQ_PENDING);
	
	/* Get current job */
	ctx = v4l2_m2m_get_curr_priv(g2d->m2m_dev);
	if (!ctx) {
		dev_err(g2d->dev, "No context in IRQ handler\n");
		return IRQ_HANDLED;
	}
	
	src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	
	if (src_buf && dst_buf) {
		dst_buf->vb2_buf.timestamp = src_buf->vb2_buf.timestamp;
		dst_buf->timecode = src_buf->timecode;
		dst_buf->field = src_buf->field;
		dst_buf->flags = src_buf->flags;
		
		v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);
	}
	
	v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);
	
	return IRQ_HANDLED;
}

static void g2d_device_run(void *priv)
{
	struct sunxi_g2d_ctx *ctx = priv;
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	dma_addr_t src_dma, dst_dma;
	u32 src_w, src_h, dst_w, dst_h;
	u32 src_pitch, dst_pitch;
	u32 size_word;
	
	pr_info("%s: entered, enable_hw=%d\n", __func__, enable_hw);
	
	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	
	if (!src_buf || !dst_buf) {
		dev_err(g2d->dev, "Missing source or destination buffer\n");
		return;
	}
	
	pr_info("%s: buffers OK, returning immediately\n", __func__);
	
	/* TEMP: Don't touch hardware at all */
	v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);
	v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);
	return;
	
#if 0  /* DISABLED FOR NOW */
	/* Get DMA addresses */
	src_dma = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	dst_dma = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	
	src_w = ctx->out_fmt.width;
	src_h = ctx->out_fmt.height;
	dst_w = ctx->cap_fmt.width;
	dst_h = ctx->cap_fmt.height;
	
	src_pitch = ctx->out_fmt.bytesperline;
	dst_pitch = ctx->cap_fmt.bytesperline;
	
	/* CPU memcpy fallback for testing */
	if (!enable_hw) {
		void *src_vaddr = vb2_plane_vaddr(&src_buf->vb2_buf, 0);
		void *dst_vaddr = vb2_plane_vaddr(&dst_buf->vb2_buf, 0);
		
		if (src_vaddr && dst_vaddr) {
			memcpy(dst_vaddr, src_vaddr, ctx->cap_fmt.sizeimage);
			
			dst_buf->vb2_buf.timestamp = src_buf->vb2_buf.timestamp;
			dst_buf->field = src_buf->field;
			
			v4l2_m2m_buf_done(v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx),
					  VB2_BUF_STATE_DONE);
			v4l2_m2m_buf_done(v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx),
					  VB2_BUF_STATE_DONE);
			v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);
		}
		return;
	}
	
	/* === Hardware blit (fillrect v1.0.0 proven sequence) === */
	
	size_word = ((src_w - 1) & 0xFFFF) | (((src_h - 1) & 0xFFFF) << 16);
	
	/* 1. Reset G2D */
	g2d_write(g2d, G2D_AHB_RESET, 0x0);
	g2d_write(g2d, G2D_AHB_RESET, 0x3);
	wmb();
	
	/* 2. V0 Layer (source) */
	g2d_write(g2d, V0_LADD0, lower_32_bits(src_dma));
	g2d_write(g2d, V0_HADD, (u32)((src_dma >> 24) & 0xFF));
	g2d_write(g2d, V0_PITCH0, src_pitch);
	g2d_write(g2d, V0_MBSIZE, size_word);
	g2d_write(g2d, V0_SIZE, size_word);
	g2d_write(g2d, V0_COOR, 0);
	g2d_write(g2d, V0_ATTCTL, V0_ATTCTL_EN | (0x04 << V0_ATTCTL_FMT_SHIFT)); /* XRGB8888 */
	
	/* 3. BLD (Blender) */
	{
		u32 bld_en = g2d_read(g2d, BLD_EN_CTL);
		bld_en |= BLD_PIPE0_EN;
		g2d_write(g2d, BLD_EN_CTL, bld_en);
	}
	g2d_write(g2d, BLD_PREMUL_CTL, 0x0);
	g2d_write(g2d, BLD_CH_ISIZE0, size_word);
	g2d_write(g2d, BLD_CH_OFFSET0, 0x0);
	g2d_write(g2d, BLD_OUT_SIZE, size_word);
	{
		u32 bld_out_color = g2d_read(g2d, BLD_OUT_COLOR);
		bld_out_color &= ~BIT(1);  /* RGB mode */
		g2d_write(g2d, BLD_OUT_COLOR, bld_out_color);
	}
	g2d_write(g2d, BLD_CTL, 0x0);
	
	/* 4. ROP */
	g2d_write(g2d, ROP_CTL, 0xF0);      /* SRC pass-through */
	g2d_write(g2d, ROP_INDEX0, 0x61080); /* COPYPEN */
	
	/* 5. WB (Writeback) */
	g2d_write(g2d, WB_LADD0, lower_32_bits(dst_dma));
	g2d_write(g2d, WB_HADD0, (u32)((dst_dma >> 24) & 0xFF));
	g2d_write(g2d, WB_PITCH0, dst_pitch);
	g2d_write(g2d, WB_SIZE, size_word);
	g2d_write(g2d, BLD_SIZE, size_word);  /* BSP writes this after WB */
	g2d_write(g2d, WB_ATT, 0x04 << WB_ATT_FMT_SHIFT); /* XRGB8888 */
	wmb();
	
	/* 6. MIXER Control */
	g2d_write(g2d, G2D_MIXER_INT, 0x0);
	g2d_write(g2d, G2D_MIXER_CTL, 0x0);
	wmb();
	
	/* Clear pending and enable IRQ */
	g2d_write(g2d, G2D_MIXER_INT, G2D_MIXER_INT_IRQ_PENDING | G2D_MIXER_INT_FINISH_IRQ_EN);
	wmb();
	pr_info("%s: MIXER_INT=0x%08x after enable\n", __func__, g2d_read(g2d, G2D_MIXER_INT));
	
	/* 7. START */
	{
		u32 mixer_ctl = g2d_read(g2d, G2D_MIXER_CTL);
		pr_info("%s: Starting with MIXER_CTL=0x%08x\n", __func__, mixer_ctl);
		g2d_write(g2d, G2D_MIXER_CTL, mixer_ctl | G2D_MIXER_CTL_START);
		pr_info("%s: MIXER_CTL=0x%08x after START\n", __func__, g2d_read(g2d, G2D_MIXER_CTL));
	}
	
	pr_info("%s: job started\n", __func__);
	pr_info("%s: DEBUG: skipping polling, returning immediately for test\n", __func__);
	
	/* TEMP: Skip everything, just return to see if we can even get here */
	v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_DONE);
	v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);
#endif  /* End of disabled hardware code */
}

static int g2d_job_ready(void *priv)
{
	struct sunxi_g2d_ctx *ctx = priv;
	
	if (v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) > 0 &&
	    v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx) > 0) {
		pr_info("%s: job is ready\n", __func__);
		return 1;
	}
	
	return 0;
}

static const struct v4l2_m2m_ops g2d_m2m_ops = {
	.device_run	= g2d_device_run,
	.job_ready	= g2d_job_ready,
};

/* ===== V4L2 file operations ===== */

static int g2d_open(struct file *file)
{
	struct sunxi_g2d_dev *g2d = video_drvdata(file);
	struct sunxi_g2d_ctx *ctx;
	int ret;
	
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	
	ctx->g2d = g2d;
	v4l2_fh_init(&ctx->fh, &g2d->vfd);
	file->private_data = &ctx->fh;
	
	/* Initialize formats to defaults */
	g2d_get_default_format(&ctx->out_fmt);
	g2d_get_default_format(&ctx->cap_fmt);
	
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(g2d->m2m_dev, ctx, g2d_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_free;
	}
	
	v4l2_fh_add(&ctx->fh, file);
	
	return 0;

err_free:
	kfree(ctx);
	return ret;
}

static int g2d_release(struct file *file)
{
	struct sunxi_g2d_ctx *ctx = container_of(file->private_data,
						  struct sunxi_g2d_ctx, fh);
	
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	kfree(ctx);
	
	return 0;
}

static const struct v4l2_file_operations g2d_fops = {
	.owner		= THIS_MODULE,
	.open		= g2d_open,
	.release	= g2d_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

/* ===== V4L2 ioctl operations ===== */

static int g2d_querycap(struct file *file, void *priv,
			struct v4l2_capability *cap)
{
	pr_info("%s\n", __func__);
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Allwinner G2D", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", DRV_NAME);
	
	return 0;
}

static int g2d_enum_fmt(struct file *file, void *priv,
			struct v4l2_fmtdesc *f)
{
	pr_info("%s: index=%d\n", __func__, f->index);
	if (f->index >= NUM_FORMATS)
		return -EINVAL;
	
	*f = g2d_formats[f->index];
	f->index = f->index;
	f->type = f->type;
	
	return 0;
}

static int g2d_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = file->private_data;
	
	pr_info("%s: type=%d\n", __func__, f->type);
	
	if (V4L2_TYPE_IS_OUTPUT(f->type))
		f->fmt.pix = ctx->out_fmt;
	else
		f->fmt.pix = ctx->cap_fmt;
	
	return 0;
}

static int g2d_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;
	const struct v4l2_fmtdesc *fmt;
	
	fmt = find_format(pix->pixelformat);
	if (!fmt) {
		pix->pixelformat = V4L2_PIX_FMT_XRGB32;
		fmt = find_format(pix->pixelformat);
	}
	
	/* Clamp dimensions */
	pix->width = clamp(pix->width, 8U, 4096U);
	pix->height = clamp(pix->height, 8U, 4096U);
	
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = pix->width * 4;
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
	
	return 0;
}

static int g2d_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = file->private_data;
	int ret;
	
	pr_info("%s: type=%d\n", __func__, f->type);
	
	ret = g2d_try_fmt(file, priv, f);
	if (ret) {
		pr_info("%s: try_fmt failed: %d\n", __func__, ret);
		return ret;
	}
	
	pr_info("%s: saving format %dx%d\n", __func__, f->fmt.pix.width, f->fmt.pix.height);
	
	if (V4L2_TYPE_IS_OUTPUT(f->type))
		ctx->out_fmt = f->fmt.pix;
	else
		ctx->cap_fmt = f->fmt.pix;
	
	pr_info("%s: OK\n", __func__);
	return 0;
}

static const struct v4l2_ioctl_ops g2d_ioctl_ops = {
	.vidioc_querycap		= g2d_querycap,
	.vidioc_enum_fmt_vid_cap	= g2d_enum_fmt,
	.vidioc_enum_fmt_vid_out	= g2d_enum_fmt,
	.vidioc_g_fmt_vid_cap		= g2d_g_fmt,
	.vidioc_g_fmt_vid_out		= g2d_g_fmt,
	.vidioc_try_fmt_vid_cap		= g2d_try_fmt,
	.vidioc_try_fmt_vid_out		= g2d_try_fmt,
	.vidioc_s_fmt_vid_cap		= g2d_s_fmt,
	.vidioc_s_fmt_vid_out		= g2d_s_fmt,
	
	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,
	
	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,
};

/* ===== Platform driver ===== */

static int sunxi_g2d_probe(struct platform_device *pdev)
{
	struct sunxi_g2d_dev *g2d;
	struct resource *res;
	struct video_device *vfd;
	int ret;
	
	g2d = devm_kzalloc(&pdev->dev, sizeof(*g2d), GFP_KERNEL);
	if (!g2d)
		return -ENOMEM;
	
	g2d->dev = &pdev->dev;
	platform_set_drvdata(pdev, g2d);
	mutex_init(&g2d->dev_mutex);
	
	/* Map G2D registers */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	g2d->mmio = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(g2d->mmio))
		return PTR_ERR(g2d->mmio);
	g2d->mmio_size = resource_size(res);
	
	dev_info(&pdev->dev, "G2D MMIO base=0x%08llx size=0x%llx\n",
		 (u64)res->start, (u64)g2d->mmio_size);
	
	/* Map CCU for T113 clock control */
	g2d->ccu = ioremap(0x02001000, 0x1000);
	if (!g2d->ccu) {
		dev_err(&pdev->dev, "Failed to map CCU\n");
		return -ENOMEM;
	}
	
	/* Get clocks */
	g2d->clk_bus = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(g2d->clk_bus)) {
		/* Try alternative name */
		g2d->clk_bus = devm_clk_get(&pdev->dev, "bus_g2d");
		if (IS_ERR(g2d->clk_bus)) {
			dev_err(&pdev->dev, "Failed to get bus clock\n");
			ret = PTR_ERR(g2d->clk_bus);
			goto err_unmap_ccu;
		}
	}
	
	g2d->clk_mod = devm_clk_get(&pdev->dev, "g2d");
	if (IS_ERR(g2d->clk_mod)) {
		dev_err(&pdev->dev, "Failed to get module clock\n");
		ret = PTR_ERR(g2d->clk_mod);
		goto err_unmap_ccu;
	}
	
	g2d->clk_mbus = devm_clk_get(&pdev->dev, "mbus_g2d");
	if (IS_ERR(g2d->clk_mbus)) {
		dev_err(&pdev->dev, "Failed to get mbus clock\n");
		ret = PTR_ERR(g2d->clk_mbus);
		goto err_unmap_ccu;
	}
	
	/* Enable clocks */
	ret = clk_prepare_enable(g2d->clk_bus);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable bus clock\n");
		goto err_unmap_ccu;
	}
	
	ret = clk_prepare_enable(g2d->clk_mod);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable module clock\n");
		goto err_disable_bus;
	}
	
	ret = clk_prepare_enable(g2d->clk_mbus);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable mbus clock\n");
		goto err_disable_mod;
	}
	
	dev_info(&pdev->dev, "Clocks: bus=%lu mod=%lu mbus=%lu Hz\n",
		 clk_get_rate(g2d->clk_bus),
		 clk_get_rate(g2d->clk_mod),
		 clk_get_rate(g2d->clk_mbus));
	
	/* Get reset */
	g2d->rst = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(g2d->rst)) {
		dev_err(&pdev->dev, "Failed to get reset\n");
		ret = PTR_ERR(g2d->rst);
		goto err_disable_mbus;
	}
	
	ret = reset_control_deassert(g2d->rst);
	if (ret) {
		dev_err(&pdev->dev, "Failed to deassert reset\n");
		goto err_disable_mbus;
	}
	
	/* Configure DMA (from fillrect v1.0.0) */
	ret = of_dma_configure(&pdev->dev, pdev->dev.of_node, true);
	if (ret)
		dev_warn(&pdev->dev, "of_dma_configure failed: %d\n", ret);
	
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask\n");
		goto err_reset;
	}
	
	/* MBUS interconnect */
	g2d->mbus = devm_of_icc_get(&pdev->dev, "dma-mem");
	if (IS_ERR(g2d->mbus)) {
		dev_err(&pdev->dev, "Failed to get MBUS interconnect\n");
		ret = PTR_ERR(g2d->mbus);
		goto err_reset;
	}
	
	ret = icc_set_bw(g2d->mbus, 300000, 600000);
	if (ret) {
		dev_err(&pdev->dev, "Failed to set MBUS bandwidth\n");
		goto err_reset;
	}
	
	/* T113 CCU initialization (from fillrect v1.0.0) */
	writel(0, g2d->ccu + G2D_BGR_REG);
	udelay(10);
	writel(G2D_BGR_RST, g2d->ccu + G2D_BGR_REG);
	udelay(100);
	writel(G2D_BGR_RST | G2D_BGR_GATING, g2d->ccu + G2D_BGR_REG);
	udelay(100);
	
	writel(G2D_CLK_GATING | (0x1 << G2D_CLK_SRC_SEL_SHIFT),
	       g2d->ccu + G2D_CLK_REG);
	udelay(100);
	
	dev_info(&pdev->dev, "CCU: BGR=0x%08x CLK=0x%08x\n",
		 readl(g2d->ccu + G2D_BGR_REG),
		 readl(g2d->ccu + G2D_CLK_REG));
	
	/* Configure TOP gates (Legacy mode for DIRECT) */
	g2d_write(g2d, G2D_SCLK_GATE, 0x3);
	g2d_write(g2d, G2D_HCLK_GATE, 0x3);
	g2d_write(g2d, G2D_AHB_RESET, 0x3);
	udelay(10);
	
	/* Configure RCQ_IRQ_CTL for DIRECT mode */
	g2d_write(g2d, G2D_RCQ_IRQ_CTL, G2D_RCQ_IRQ_SEL);
	dev_info(&pdev->dev, "RCQ_IRQ_CTL=0x%08x (DIRECT mode)\n",
		 g2d_read(g2d, G2D_RCQ_IRQ_CTL));
	
	/* Initialize MIXER_INT */
	g2d_write(g2d, G2D_MIXER_INT, G2D_MIXER_INT_IRQ_PENDING);
	g2d_write(g2d, G2D_MIXER_INT, G2D_MIXER_INT_FINISH_IRQ_EN);
	
	/* Get IRQ */
	g2d->irq = platform_get_irq(pdev, 0);
	if (g2d->irq < 0) {
		dev_err(&pdev->dev, "Failed to get IRQ\n");
		ret = g2d->irq;
		goto err_reset;
	}
	
	ret = devm_request_irq(&pdev->dev, g2d->irq, g2d_irq_handler,
			       0, dev_name(&pdev->dev), g2d);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ\n");
		goto err_reset;
	}
	
	dev_info(&pdev->dev, "IRQ %d registered\n", g2d->irq);
	
	/* V4L2 device */
	ret = v4l2_device_register(&pdev->dev, &g2d->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register V4L2 device\n");
		goto err_reset;
	}
	
	/* M2M device */
	g2d->m2m_dev = v4l2_m2m_init(&g2d_m2m_ops);
	if (IS_ERR(g2d->m2m_dev)) {
		dev_err(&pdev->dev, "Failed to init M2M device\n");
		ret = PTR_ERR(g2d->m2m_dev);
		goto err_v4l2;
	}
	
	/* Video device */
	vfd = &g2d->vfd;
	vfd->fops = &g2d_fops;
	vfd->ioctl_ops = &g2d_ioctl_ops;
	vfd->minor = -1;
	vfd->release = video_device_release_empty;
	vfd->vfl_dir = VFL_DIR_M2M;
	vfd->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	vfd->v4l2_dev = &g2d->v4l2_dev;
	vfd->lock = &g2d->dev_mutex;
	
	snprintf(vfd->name, sizeof(vfd->name), "%s", DRV_NAME);
	video_set_drvdata(vfd, g2d);
	
	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register video device\n");
		goto err_m2m;
	}
	
	dev_info(&pdev->dev, "Registered as %s\n", video_device_node_name(vfd));
	dev_info(&pdev->dev, "G2D V4L2 M2M driver v" DRV_VERSION " loaded\n");
	
	return 0;

err_m2m:
	v4l2_m2m_release(g2d->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&g2d->v4l2_dev);
err_reset:
	reset_control_assert(g2d->rst);
err_disable_mbus:
	clk_disable_unprepare(g2d->clk_mbus);
err_disable_mod:
	clk_disable_unprepare(g2d->clk_mod);
err_disable_bus:
	clk_disable_unprepare(g2d->clk_bus);
err_unmap_ccu:
	if (g2d->ccu)
		iounmap(g2d->ccu);
	return ret;
}

static void sunxi_g2d_remove(struct platform_device *pdev)
{
	struct sunxi_g2d_dev *g2d = platform_get_drvdata(pdev);
	
	video_unregister_device(&g2d->vfd);
	v4l2_m2m_release(g2d->m2m_dev);
	v4l2_device_unregister(&g2d->v4l2_dev);
	
	reset_control_assert(g2d->rst);
	clk_disable_unprepare(g2d->clk_mbus);
	clk_disable_unprepare(g2d->clk_mod);
	clk_disable_unprepare(g2d->clk_bus);
	
	if (g2d->ccu)
		iounmap(g2d->ccu);
}

static const struct of_device_id sunxi_g2d_dt_match[] = {
	{ .compatible = "allwinner,t113-g2d" },
	{ }
};
MODULE_DEVICE_TABLE(of, sunxi_g2d_dt_match);

static struct platform_driver sunxi_g2d_driver = {
	.probe	= sunxi_g2d_probe,
	.remove	= sunxi_g2d_remove,
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= sunxi_g2d_dt_match,
	},
};

module_platform_driver(sunxi_g2d_driver);

MODULE_DESCRIPTION("Allwinner T113-S3 G2D V4L2 M2M driver");
MODULE_AUTHOR("Sergio + AI Assistant");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
