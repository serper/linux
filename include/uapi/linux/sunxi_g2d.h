/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Allwinner G2D UAPI
 * Copyright (C) 2025 Sergio Peralta
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
};

/* Alpha modes for per-image control */
enum g2d_alpha_mode {
	G2D_PIXEL_ALPHA = 0,	/* Use per-pixel alpha from image */
	G2D_GLOBAL_ALPHA = 1,	/* Use global_alpha value */
	G2D_MIXER_ALPHA = 2,	/* Multiply pixel and global alpha */
};

/* G2D buffer description */
struct g2d_buf {
	__u32 width;
	__u32 height;
	__u32 format;		/* enum g2d_pixel_format */
	__u32 stride[3];	/* bytes per line for each plane */
	
	/* Buffer can be specified by DMA-BUF fd OR physical address */
	__s32 dma_fd;		/* DMA-BUF file descriptor, or -1 */
	__u64 paddr[3];		/* Physical addresses for up to 3 planes */
	
	__u32 crop_x;		/* Crop rectangle (optional) */
	__u32 crop_y;
	__u32 crop_w;
	__u32 crop_h;
	
	/* Alpha control (for alpha blending operations) */
	__u8 alpha;		/* Global alpha value for this buffer (0-255) */
	__u8 alpha_mode;	/* enum g2d_alpha_mode */
	__u16 _pad;		/* Padding for alignment to 4-byte boundary */
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
 * Alpha blending is automatic when:
 * - src.alpha_mode != G2D_PIXEL_ALPHA (uses src.alpha value), OR
 * - dst.alpha_mode != G2D_PIXEL_ALPHA (uses dst.alpha value), OR
 * - Source format has alpha channel and pixel alpha is non-opaque
 * 
 * If out.dma_fd == -1: in-place operation (dst is both input background and output)
 * If out.dma_fd >= 0: three-buffer operation (dst=background, src=foreground, out=result)
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
	__u32 _reserved;	/* Reserved for future use */
	
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

/* G2D buffer allocation request (TODO: Not yet implemented) */
struct g2d_alloc_buffer {
	__u64 size;		/* IN: Buffer size in bytes */
	__s32 dma_fd;		/* OUT: DMA-BUF file descriptor */
	__u32 flags;		/* Reserved for future use */
};

/* Selftest helper for userspace to create a kernel-backed fence that will
 * be signalled after a timeout. Used for isolating fence lifecycle bugs
 * without touching hardware.
 */
struct g2d_selftest {
    __s32 timeout_ms;   /* IN: delay in milliseconds before signalling */
    __s32 fence_fd;     /* OUT: returned fence fd */
};

/* IOCTLs */
#define G2D_IOC_MAGIC		'G'

#define G2D_IOC_GET_VERSION	_IOR(G2D_IOC_MAGIC, 0, struct g2d_version)
#define G2D_IOC_BLIT		_IOWR(G2D_IOC_MAGIC, 1, struct g2d_blit)
#define G2D_IOC_FILLRECT	_IOWR(G2D_IOC_MAGIC, 2, struct g2d_fillrect)
#define G2D_IOC_SYNC		_IOW(G2D_IOC_MAGIC, 3, __s32)  /* Wait on fence */
#define G2D_IOC_ALLOC_BUFFER	_IOWR(G2D_IOC_MAGIC, 4, struct g2d_alloc_buffer)
/* Deprecated ioctls - kept for compatibility */
#define G2D_IOC_ALPHA_BLEND	_IOWR(G2D_IOC_MAGIC, 5, struct g2d_blit)  /* Use G2D_IOC_BLIT instead */
#define G2D_IOC_SELFTEST_FENCE _IOWR(G2D_IOC_MAGIC, 6, struct g2d_selftest)
#define G2D_IOC_FILLRECT_RCQ	_IOWR(G2D_IOC_MAGIC, 7, struct g2d_fillrect)  /* Use G2D_IOC_FILLRECT instead */

/* Buffer read/write operations for userspace manipulation */
struct g2d_buffer_rw {
	__s32 dma_fd;		/* DMA-BUF fd from G2D_IOC_ALLOC_BUFFER */
	__u64 offset;		/* Offset in bytes from buffer start */
	__u64 size;		/* Number of bytes to read/write */
	__u64 user_ptr;		/* Userspace buffer pointer (void __user *) */
};

#define G2D_IOC_WRITE_BUFFER	_IOW(G2D_IOC_MAGIC, 8, struct g2d_buffer_rw)
#define G2D_IOC_READ_BUFFER	_IOR(G2D_IOC_MAGIC, 9, struct g2d_buffer_rw)

#endif /* _UAPI_SUNXI_G2D_H */
