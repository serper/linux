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
 * Determines which matrix is used for color space conversion.
 */
enum g2d_color_space {
	G2D_COLOR_SPACE_BT601 = 0,	/* BT.601 (SD video, SDTV) */
	G2D_COLOR_SPACE_BT709 = 1,	/* BT.709 (HD video, HDTV) */
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
	G2D_BLD_XOR = 11,	/* XOR: Src*(1-Ad) + Dst*(1-As) - symmetric, no swap needed */
};

/* G2D buffer description 
 * Note: Fields are ordered to minimize padding in packed structures.
 * __u64 fields first, then __u32, then smaller types.
 */
struct g2d_buf {
	/* Physical addresses must be first for proper alignment even with packed */
	__u64 paddr[3];		/* Physical addresses for up to 3 planes */
	
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
} __attribute__((packed));

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
} __attribute__((packed));

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
} __attribute__((packed));

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

/* IOCTLs */
#define G2D_IOC_MAGIC		'G'

#define G2D_IOC_GET_VERSION	_IOR(G2D_IOC_MAGIC, 0, struct g2d_version)
#define G2D_IOC_UNIFIED	_IOWR(G2D_IOC_MAGIC, 1, struct g2d_blit)
#define G2D_IOC_FILLRECT	_IOWR(G2D_IOC_MAGIC, 2, struct g2d_fillrect)
#define G2D_IOC_SYNC		_IOW(G2D_IOC_MAGIC, 3, __s32)  /* Wait on fence */
#define G2D_IOC_ALLOC_BUFFER	_IOWR(G2D_IOC_MAGIC, 4, struct g2d_alloc_buffer)
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

/* ============================================================================
 * NEW SPECIALIZED COMMAND API - RECOMMENDED FOR NEW CODE
 * ============================================================================
 * 
 * This new API provides explicit, specialized commands for each operation type.
 * Each command does exactly one thing, making the API clearer and easier to use.
 * 
 * Benefits over unified G2D_IOC_UNIFIED:
 * - Explicit: No hidden automatic behavior detection
 * - Simple: Each command has clear, predictable behavior  
 * - Maintainable: Easier to implement and debug
 * - Flexible: Complex operations built from simple primitives
 * 
 * For complex operations (e.g., scale+rotate+blend), chain multiple commands.
 * Use fence_fd_out from one command as fence_fd_in for the next.
 */

/* Command types for specialized operations */
enum g2d_cmd_type {
	G2D_CMD_COPY = 0,	/* Simple copy/blit (no scaling, no blending, no rotation) */
	G2D_CMD_SCALE = 1,	/* Scaling only using VSU (no blending, no rotation) */
	G2D_CMD_BLEND = 2,	/* Alpha blending (src + dst → out, with optional scaling) */
	G2D_CMD_ROTATE = 3,	/* Rotation/flip using ROT block (no scaling, no blending) */
	G2D_CMD_MASK = 4,	/* Color keying / chromakey (make colors transparent) */
};

/* Specialized command structure - uses union to save space */
struct g2d_cmd {
	__u32 cmd_type;		/* enum g2d_cmd_type */
	
	struct g2d_buf src;	/* Source buffer (all commands) */
	struct g2d_buf dst;	/* Destination buffer (all commands) */
	struct g2d_buf out;	/* Output buffer (only BLEND and optionally MASK) */
	
	__u32 dst_x;		/* Destination position/size (all commands) */
	__u32 dst_y;
	__u32 dst_w;
	__u32 dst_h;
	
	/* Command-specific parameters */
	union {
		/* G2D_CMD_COPY: no extra params, uses dst_x/y/w/h for position */
		struct {
			__u32 reserved;
		} copy;
		
		/* G2D_CMD_SCALE: uses dst_w/h as target size, VSU scales automatically */
		struct {
			__u32 reserved;
		} scale;
		
		/* G2D_CMD_BLEND: alpha blending with Porter-Duff modes
		 * Alpha values come from src.alpha and dst.alpha in g2d_buf.
		 * Can optionally scale src to dst_w/h during blend.
		 */
		struct {
			__u8 bld_mode;	/* enum g2d_bld_mode (Porter-Duff) */
			__u8 reserved[3];
		} blend;
		
		/* G2D_CMD_ROTATE: rotation and flipping using ROT block
		 * Cannot be combined with scaling (hardware limitation).
		 * For rotate+scale: use G2D_CMD_SCALE first, then G2D_CMD_ROTATE.
		 */
		struct {
			__u32 angle;	/* Rotation angle: 0, 90, 180, or 270 degrees */
			__u32 flip_h;	/* Horizontal flip: 0=no, 1=yes */
			__u32 flip_v;	/* Vertical flip: 0=no, 1=yes */
		} rotate;
		
		/* G2D_CMD_MASK: color keying / chromakey
		 * Makes colors within range transparent (or keeps only those colors).
		 * Useful for green screen effects, transparency masking, etc.
		 */
		struct {
			__u32 color_key_min;	/* Minimum RGB value (0xRRGGBB) */
			__u32 color_key_max;	/* Maximum RGB value (0xRRGGBB) */
			__u8  color_key_mode;	/* 0=inside range transparent, 1=outside transparent */
			__u8  reserved[3];
		} mask;
	} params;
	
	__s32 fence_fd_in;	/* Wait on this fence before operation, or -1 */
	__s32 fence_fd_out;	/* OUT: fence that signals when operation completes */
} __attribute__((packed));

#define G2D_IOC_CMD		_IOWR(G2D_IOC_MAGIC, 10, struct g2d_cmd)

#endif /* _UAPI_SUNXI_G2D_H */
