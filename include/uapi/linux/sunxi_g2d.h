/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Allwinner G2D UAPI
 * Copyright (C) 2025 Sergio Perez
 */

#ifndef _UAPI_SUNXI_G2D_H
#define _UAPI_SUNXI_G2D_H

#include <linux/types.h>

/* G2D pixel formats */
enum g2d_pixel_format {
	G2D_FMT_ARGB8888 = 0,
	G2D_FMT_ABGR8888 = 1,
	G2D_FMT_RGBA8888 = 2,
	G2D_FMT_BGRA8888 = 3,
	G2D_FMT_XRGB8888 = 4,
	G2D_FMT_XBGR8888 = 5,
	G2D_FMT_RGBX8888 = 6,
	G2D_FMT_BGRX8888 = 7,
	G2D_FMT_RGB888 = 8,
	G2D_FMT_BGR888 = 9,
	G2D_FMT_RGB565 = 10,
	G2D_FMT_BGR565 = 11,
	G2D_FMT_ARGB4444 = 12,
	G2D_FMT_ABGR4444 = 13,
	G2D_FMT_RGBA4444 = 14,
	G2D_FMT_BGRA4444 = 15,
	G2D_FMT_ARGB1555 = 16,
	G2D_FMT_ABGR1555 = 17,
	G2D_FMT_RGBA5551 = 18,
	G2D_FMT_BGRA5551 = 19,
	G2D_FMT_YUV422_I_YVYU = 20,
	G2D_FMT_YUV422_I_YUYV = 21,
	G2D_FMT_YUV422_I_UYVY = 22,
	G2D_FMT_YUV422_I_VYUY = 23,
	G2D_FMT_YUV422_SP_UVUV = 24,
	G2D_FMT_YUV422_SP_VUVU = 25,
	G2D_FMT_YUV422_P = 26,
	G2D_FMT_YUV420_SP_UVUV = 27,
	G2D_FMT_YUV420_SP_VUVU = 28,
	G2D_FMT_YUV420_P = 29,
	G2D_FMT_YUV411_SP_UVUV = 30,
	G2D_FMT_YUV411_SP_VUVU = 31,
	G2D_FMT_YUV411_P = 32,
	G2D_FMT_8BPP_MONO = 33,
	G2D_FMT_4BPP_MONO = 34,
	G2D_FMT_2BPP_MONO = 35,
	G2D_FMT_1BPP_MONO = 36,
	/* New explicit planar 4:2:0 variant with V then U plane order (YV12) */
	G2D_FMT_YUV420_P_VU = 37,
};

/* CSC Adjustment Structure */
struct g2d_csc_adjust {
	__u32 mode;        /* G2D_CSC_YUV2RGB_601, _709, _2020... */

	/* Brightness: offset in RGB space (-128..+128) */
	__s16 brightness;  /* 0 = neutral, +n = brighter, -n = darker */

	/* Contrast and Saturation as percentage * 100 (Q8.8-ish) */
	__u16 contrast;    /* 100 = neutral, 50 = half, 200 = double */
	__u16 saturation;  /* 100 = neutral, 0 = B/N, 150 = 1.5x */

	__u32 flags;       /* Reserved for future use (gamma, hue, etc.) */
};

#define G2D_CSC_F_ENABLE  (1u << 0)  /* CSC forzado */

/* Alpha modes for per-image control */
enum g2d_alpha_mode {
	G2D_PIXEL_ALPHA = 0,	/* Use per-pixel alpha from image */
	G2D_GLOBAL_ALPHA = 1,	/* Use global_alpha value */
	G2D_MIXER_ALPHA = 2,	/* Multiply pixel and global alpha */
};

/* Premultiplication modes for alpha channel handling */
enum g2d_premul_mode {
	G2D_PREMUL_NONE = 0,	/* Non-premultiplied alpha (straight alpha) */
	G2D_PREMUL_ALPHA = 1,	/* Premultiplied alpha (color *= alpha) */
};

/* Color space for YUV to RGB conversion
 * Only relevant for YUV formats (>= 0x20).
 * YUV formats mapping:
 * - G2D_FMT_YUV420_SP_UVUV = NV12 (Y + interleaved U,V)
 * - G2D_FMT_YUV420_SP_VUVU = NV21 (Y + interleaved V,U)
 * - G2D_FMT_YUV420_P       = I420 (Y + U + V planes)
 * - G2D_FMT_YUV420_P_VU    = YV12 (Y + V + U planes)
 * Determines which matrix is used for color space conversion.
 */
enum g2d_color_space {
	G2D_COLOR_SPACE_BT601 = 0,	/* BT.601 (SD video, SDTV) */
	G2D_COLOR_SPACE_BT709 = 1,	/* BT.709 (HD video, HDTV) */
	G2D_COLOR_SPACE_BT2020 = 2,	/* BT.2020 (UHD video) */
};

/* Porter-Duff blending modes
 * These define how source and destination are combined during alpha blending.
 * Only used when alpha blending is active.
 * 
 * HARDWARE QUIRK COMPENSATION:
 * The G2D hardware has V0 and WB registers with INVERTED semantic roles:
 * - V0 (labeled "source") acts as DESTINATION in blend equation
 * - WB (labeled "destination") acts as SOURCE in blend equation
 * 
 * To compensate, these enum values are SWAPPED so that:
 * - User requests G2D_BLD_SRCOVER → hardware executes with DSTOVER register values
 * - Result: correct "src OVER dst" visual behavior
 * 
 * This swap is internal to the driver. Applications use standard Porter-Duff semantics.
 */
enum g2d_bld_mode {
	G2D_BLD_CLEAR = 0,	/* Clear: 0 */
	G2D_BLD_COPY = 1,	/* Copy source: Src */
	G2D_BLD_DST = 2,	/* Keep destination: Dst */
	G2D_BLD_SRCOVER = 4,	/* Source Over: Src + Dst*(1-As) [DEFAULT] - HW uses DSTOVER value */
	G2D_BLD_DSTOVER = 3,	/* Destination Over: Dst + Src*(1-Ad) - HW uses SRCOVER value */
	G2D_BLD_SRCIN = 6,	/* Source In: Src*Ad - HW uses DSTIN value */
	G2D_BLD_DSTIN = 5,	/* Destination In: Dst*As - HW uses SRCIN value */
	G2D_BLD_SRCOUT = 8,	/* Source Out: Src*(1-Ad) - HW uses DSTOUT value */
	G2D_BLD_DSTOUT = 7,	/* Destination Out: Dst*(1-As) - HW uses SRCOUT value */
	G2D_BLD_SRCATOP = 10,	/* Source Atop: Src*Ad + Dst*(1-As) - HW uses DSTATOP value */
	G2D_BLD_DSTATOP = 9,	/* Destination Atop: Dst*As + Src*(1-Ad) - HW uses SRCATOP value */
	G2D_BLD_XOR = 11,	/* XOR: Src*(1-Ad) + Dst*(1-As) */
};

/* G2D buffer description 
 * Note: Fields are ordered to minimize padding with natural alignment.
 * __u64 fields first, then __u32, then smaller types.
 */
struct g2d_buf {
	__u32 width;
	__u32 height;
	__u32 format;		/* enum g2d_pixel_format */
	__u32 stride[3];	/* bytes per line for each plane */
	
	__u32 crop_x;		/* Crop rectangle (optional) */
	__u32 crop_y;
	__u32 crop_w;
	__u32 crop_h;
	
	/* Buffer can be specified by DMA-BUF fd OR physical address */
	__s32 dma_fd;		/* DMA-BUF file descriptor, or -1 */
	
	/* Alpha control (for alpha blending operations) */
	__u8 alpha;		/* Global alpha value for this buffer (0-255) */
	__u8 alpha_mode;	/* enum g2d_alpha_mode */
	__u8 premul_mode;	/* enum g2d_premul_mode - alpha premultiplication */
	__u8 color_space;	/* enum g2d_color_space - YUV→RGB conversion (YUV formats only) */
};

/* G2D blit operation - UNIFIED operation for all blitting needs
 * 
 * This single operation handles:
 * - Simple copy (no flags)
 * - Scaling (when dst_w/dst_h differ from src crop size)
 * - Alpha blending (via src/dst alpha_mode and alpha values)
 * - Rotation/flip (via flags, cannot combine with scaling)
 * 
 * Hardware limitation: Cannot do scaling + rotation simultaneously.
 * If both are needed, perform in two passes.
 * 
 * Alpha blending is automatically enabled when ANY of these conditions are met:
 * - out.dma_fd >= 0 (three-buffer operation always uses alpha blending)
 * - src.alpha_mode == G2D_GLOBAL_ALPHA (uses src.alpha value)
 * - src.alpha_mode == G2D_MIXER_ALPHA (multiply pixel and src.alpha)
 * - dst.alpha_mode == G2D_GLOBAL_ALPHA (uses dst.alpha value)
 * - dst.alpha_mode == G2D_MIXER_ALPHA (multiply pixel and dst.alpha)
 * - src.alpha_mode == G2D_PIXEL_ALPHA AND src format has alpha (ARGB8888/ABGR8888)
 * - Legacy G2D_BLIT_FLAG_ALPHA_BLEND flag is set (deprecated)
 * 
 * Buffer modes:
 * - If out.dma_fd == -1: in-place operation (dst is both input background and output)
 * - If out.dma_fd >= 0: three-buffer operation (dst=background, src=foreground, out=result)
 *                       dst buffer is preserved (not modified)
 */
struct g2d_blit {
	struct g2d_buf src;	/* Source/foreground image */
	struct g2d_buf dst;	/* Destination/background image */
	struct g2d_buf out;	/* Output buffer (optional: -1 for in-place) */
	
	__u32 dst_x;		/* Destination position */
	__u32 dst_y;
	__u32 dst_w;		/* Destination size (for scaling) */
	__u32 dst_h;
	
	__u32 flags;		/* G2D_BLIT_FLAG_* */
	__u32 bld_mode;		/* enum g2d_bld_mode (Porter-Duff blend mode) */
	
	/* Chroma key (color keying) support */
	__u32 color_key_enable;		/* 1 = enable color keying, 0 = disable */
	__u32 color_key_mode;		/* 0 = match inside range (make transparent),
					   1 = match outside range (keep only key color) */
	__u32 color_key_min;		/* Minimum RGB value (0xRRGGBB format) */
	__u32 color_key_max;		/* Maximum RGB value (0xRRGGBB format) */
	
	/* Sync fence support */
	__s32 fence_fd_in;	/* Wait on this fence before blit, or -1 */
	__s32 fence_fd_out;	/* OUT: fence that signals when done */
};

/* Blit flags 
 * Note: ALPHA_BLEND flag is deprecated - alpha blending is now automatic
 * based on src/dst alpha_mode and alpha values in g2d_buf structures.
 */
#define G2D_BLIT_FLAG_ROTATE_90		(1 << 1)
#define G2D_BLIT_FLAG_ROTATE_180	(1 << 2)
#define G2D_BLIT_FLAG_ROTATE_270	(1 << 3)
#define G2D_BLIT_FLAG_FLIP_H		(1 << 4)
#define G2D_BLIT_FLAG_FLIP_V		(1 << 5)

/* Deprecated flags - kept for compatibility but ignored */
#define G2D_BLIT_FLAG_ALPHA_BLEND	(1 << 0)  /* Deprecated: auto-detected */
#define G2D_BLIT_FLAG_ASYNC		(1 << 6)  /* Deprecated: always async */

/* G2D fillrect operation 
 * 
 * Fills a rectangular region with a solid color.
 * Can optionally perform alpha blending with existing content if dst buffer
 * format has alpha channel.
 */
struct g2d_fillrect {
	struct g2d_buf dst;
	
	__u32 dst_x;
	__u32 dst_y;
	__u32 dst_w;
	__u32 dst_h;
	
	__u32 color;		/* Color in format specified by color_format */
	__u32 color_format;	/* enum g2d_pixel_format - format of color value */
	
	__s32 fence_fd_in;
	__s32 fence_fd_out;	/* OUT */
};

/* G2D version info */
struct g2d_version {
	__u32 hw_version;	/* Hardware IP version register */
	__u32 driver_major;
	__u32 driver_minor;
	__u32 driver_patchlevel;
};

/* G2D buffer allocation request */
struct g2d_alloc_buffer {
	__u64 size;		/* IN: Buffer size in bytes */
	__s32 dma_fd;		/* OUT: DMA-BUF file descriptor */
	__u32 flags; 		/* IN: Allocation flags (see G2D_ALLOC_F_*) */
};

/* g2d_alloc_buffer.flags
 *
 * By default, the driver uses its module parameter g2d_alloc_mode to decide
 * the allocation backend. These flags allow userspace to request a specific
 * backend per-allocation without changing the module-wide default.
 *
 * - G2D_ALLOC_F_CONTIGUOUS: Require a single, IOVA-contiguous DMA segment.
 *   This is mandatory for G2D linear surfaces on T113 to avoid hardware DMA
 *   past the first SG entry. If set, the driver will choose a backend that
 *   yields a single contiguous segment (e.g., dma_alloc_coherent if available).
 *
 * - G2D_ALLOC_F_COHERENT: Prefer a cache-coherent mapping for CPU access.
 *   When combined with CONTIGUOUS, this selects the coherent/IOMMU backend
 *   (dma_alloc_coherent + dma_get_sgtable_attrs) when possible.
 */
#define G2D_ALLOC_F_CONTIGUOUS		(1u << 0)
#define G2D_ALLOC_F_COHERENT		(1u << 1)

/* Selftest helper for userspace to create a kernel-backed fence that will
 * be signalled after a timeout. Used for isolating fence lifecycle bugs
 * without touching hardware.
 */
struct g2d_selftest {
    __s32 timeout_ms;   /* IN: delay in milliseconds before signalling */
    __s32 fence_fd;     /* OUT: returned fence fd */
};

/* Command types for G2D_IOC_CMD */
enum g2d_cmd_type {
	G2D_CMD_COPY = 1,
	G2D_CMD_SCALE = 2,
	G2D_CMD_BLEND = 3,
	G2D_CMD_ROTATE = 4,
	G2D_CMD_MASK = 5,
	G2D_CMD_FILLRECT = 6,
};

enum g2d_task_cmd {
	G2D_TASK_CREATE = 1,
	G2D_TASK_ADD = 2,
	G2D_TASK_RUN = 3,
	G2D_TASK_DEL = 4,
};

/* Unified command structure */
struct g2d_cmd {
	__u32 cmd_type;	/* enum g2d_cmd_type */
	
	struct g2d_buf src;
	struct g2d_buf dst;
	struct g2d_buf out;
	
	__u32 dst_x;
	__u32 dst_y;
	__u32 dst_w;
	__u32 dst_h;
	
	__u32 flags;
	
	__s32 fence_fd_in;
	__s32 fence_fd_out;
	
	union {
		struct {
			__u32 color;
		} fillrect;
		struct {
			__u32 bld_mode;
		} blend;
		struct {
			__u32 angle;
			__u32 flip_h;
			__u32 flip_v;
		} rotate;
		struct {
			__u32 color;
			__u32 color_key_mode;
			__u32 color_key_min;
			__u32 color_key_max;
	} mask;
	} params;
};

/* Task request structure for G2D_IOC_TASK */
struct g2d_task_req {
	__u32 task_cmd;          /* enum g2d_task_cmd */
	__u32 task_id;           /* IN/OUT: task identifier */
	struct g2d_cmd step;     /* Used only with G2D_TASK_ADD */
	__s32 fence_fd_out;      /* OUT: fence for G2D_TASK_RUN */
};

/* Buffer read/write request */
struct g2d_buffer_rw {
	__u32 dma_fd;
	__u64 offset;
	__u64 size;
	__u64 user_ptr; /* Pointer to userspace buffer */
};

/* IOCTLs */
#define G2D_IOC_MAGIC		'G'

#define G2D_IOC_GET_VERSION		_IOR(G2D_IOC_MAGIC, 0x01, struct g2d_version)
#define G2D_IOC_CMD				_IOWR(G2D_IOC_MAGIC, 0x02, struct g2d_cmd)
#define G2D_IOC_ALLOC_BUFFER	_IOWR(G2D_IOC_MAGIC, 0x03, struct g2d_alloc_buffer)
#define G2D_IOC_SELFTEST_FENCE 	_IOWR(G2D_IOC_MAGIC, 0x04, struct g2d_selftest)
#define G2D_IOC_WRITE_BUFFER	_IOW(G2D_IOC_MAGIC, 0x05, struct g2d_buffer_rw)
#define G2D_IOC_READ_BUFFER		_IOR(G2D_IOC_MAGIC, 0x06, struct g2d_buffer_rw)
#define G2D_IOC_SYNC			_IOW(G2D_IOC_MAGIC, 0x07, int)
#define G2D_IOC_TASK			_IOWR(G2D_IOC_MAGIC, 0x08, struct g2d_task_req)

/* New CSC Adjustment IOCTLs */
#define G2D_IOC_SET_CSC_ADJUST   _IOW(G2D_IOC_MAGIC, 0x50, struct g2d_csc_adjust)
#define G2D_IOC_GET_CSC_ADJUST   _IOR(G2D_IOC_MAGIC, 0x51, struct g2d_csc_adjust)

#endif /* _UAPI_SUNXI_G2D_H */
