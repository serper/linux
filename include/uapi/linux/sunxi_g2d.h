/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Allwinner G2D User API
 * Copyright (C) 2025 Sergio Perez
 *
 * This header defines the user-space API for the Allwinner G2D 2D graphics accelerator.
 * Designed for easy integration with applications, including Rust bindings via bindgen.
 */

#ifndef _UAPI_SUNXI_G2D_H
#define _UAPI_SUNXI_G2D_H

#include <linux/types.h>

/*
 * ============================================================================
 * PIXEL FORMATS
 * ============================================================================
 */

/**
 * enum g2d_pixel_format - Pixel format identifiers for G2D buffers
 *
 * RGB Formats (0-19):
 *   - 8888: 32-bit RGBA/ARGB variants with full alpha
 *   - 888:  24-bit RGB variants without alpha
 *   - 565:  16-bit RGB (5 red, 6 green, 5 blue)
 *   - 4444: 16-bit RGBA with 4-bit alpha
 *   - 1555: 16-bit RGB with 1-bit alpha
 *
 * YUV Formats (20-32, 37):
 *   - I: Interleaved (packed)
 *   - SP: Semi-planar (Y plane + interleaved UV/VU plane)
 *   - P: Planar (separate Y, U, V planes)
 *   - 422: 4:2:2 chroma subsampling
 *   - 420: 4:2:0 chroma subsampling
 *   - 411: 4:1:1 chroma subsampling
 *
 * Monochrome Formats (33-36):
 *   - 1, 2, 4, 8 bits per pixel grayscale
 */
enum g2d_pixel_format {
	/* 32-bit RGB formats with alpha */
	G2D_FMT_ARGB8888 = 0,
	G2D_FMT_ABGR8888 = 1,
	G2D_FMT_RGBA8888 = 2,
	G2D_FMT_BGRA8888 = 3,
	
	/* 32-bit RGB formats without alpha (X = unused) */
	G2D_FMT_XRGB8888 = 4,
	G2D_FMT_XBGR8888 = 5,
	G2D_FMT_RGBX8888 = 6,
	G2D_FMT_BGRX8888 = 7,
	
	/* 24-bit RGB formats */
	G2D_FMT_RGB888 = 8,
	G2D_FMT_BGR888 = 9,
	
	/* 16-bit RGB formats */
	G2D_FMT_RGB565 = 10,
	G2D_FMT_BGR565 = 11,
	
	/* 16-bit RGB formats with 4-bit alpha */
	G2D_FMT_ARGB4444 = 12,
	G2D_FMT_ABGR4444 = 13,
	G2D_FMT_RGBA4444 = 14,
	G2D_FMT_BGRA4444 = 15,
	
	/* 16-bit RGB formats with 1-bit alpha */
	G2D_FMT_ARGB1555 = 16,
	G2D_FMT_ABGR1555 = 17,
	G2D_FMT_RGBA5551 = 18,
	G2D_FMT_BGRA5551 = 19,
	
	/* YUV 422 interleaved formats */
	G2D_FMT_YUV422_I_YVYU = 20,
	G2D_FMT_YUV422_I_YUYV = 21,
	G2D_FMT_YUV422_I_UYVY = 22,
	G2D_FMT_YUV422_I_VYUY = 23,
	
	/* YUV 422 semi-planar formats */
	G2D_FMT_YUV422_SP_UVUV = 24,
	G2D_FMT_YUV422_SP_VUVU = 25,
	
	/* YUV 422 planar format */
	G2D_FMT_YUV422_P = 26,
	
	/* YUV 420 semi-planar formats (NV12/NV21) */
	G2D_FMT_YUV420_SP_UVUV = 27,  /* NV12 */
	G2D_FMT_YUV420_SP_VUVU = 28,  /* NV21 */
	
	/* YUV 420 planar formats (I420/YV12) */
	G2D_FMT_YUV420_P = 29,        /* I420 (Y + U + V) */
	
	/* YUV 411 semi-planar formats */
	G2D_FMT_YUV411_SP_UVUV = 30,
	G2D_FMT_YUV411_SP_VUVU = 31,
	
	/* YUV 411 planar format */
	G2D_FMT_YUV411_P = 32,
	
	/* Monochrome formats */
	G2D_FMT_8BPP_MONO = 33,
	G2D_FMT_4BPP_MONO = 34,
	G2D_FMT_2BPP_MONO = 35,
	G2D_FMT_1BPP_MONO = 36,
	
	/* YUV 420 planar with reversed plane order */
	G2D_FMT_YUV420_P_VU = 37,     /* YV12 (Y + V + U) */
};

/*
 * ============================================================================
 * COLOR SPACE AND CONVERSION
 * ============================================================================
 */

/**
 * enum g2d_color_space - Color space for YUV to RGB conversion
 *
 * Only relevant for YUV formats (>= G2D_FMT_YUV422_I_YVYU).
 * Determines which conversion matrix is used.
 */
enum g2d_color_space {
	G2D_COLOR_SPACE_BT601 = 0,   /* BT.601 (SD video, SDTV) */
	G2D_COLOR_SPACE_BT709 = 1,   /* BT.709 (HD video, HDTV) */
	G2D_COLOR_SPACE_BT2020 = 2,  /* BT.2020 (UHD video) */
};

/**
 * struct g2d_csc_adjust - Color Space Conversion adjustment parameters
 *
 * @mode:        Color space mode (G2D_COLOR_SPACE_*)
 * @brightness:  Brightness offset in RGB space (-128 to +128, 0 = neutral)
 * @contrast:    Contrast multiplier (100 = neutral, 50 = half, 200 = double)
 * @saturation:  Saturation multiplier (100 = neutral, 0 = grayscale, 150 = 1.5x)
 * @flags:       Control flags (G2D_CSC_F_*)
 */
struct g2d_csc_adjust {
	__u32 mode;
	__s16 brightness;
	__u16 contrast;
	__u16 saturation;
	__u32 flags;
};

/* CSC flags */
#define G2D_CSC_F_ENABLE  (1u << 0)  /* Force CSC conversion */

/*
 * ============================================================================
 * ALPHA BLENDING AND PREMULTIPLICATION
 * ============================================================================
 */

/**
 * enum g2d_alpha_mode - Per-image alpha control mode
 */
enum g2d_alpha_mode {
	G2D_PIXEL_ALPHA = 0,   /* Use per-pixel alpha from image */
	G2D_GLOBAL_ALPHA = 1,  /* Use global_alpha value */
	G2D_MIXER_ALPHA = 2,   /* Multiply pixel alpha with global alpha */
};

/**
 * enum g2d_premul_mode - Alpha premultiplication mode
 */
enum g2d_premul_mode {
	G2D_PREMUL_NONE = 0,   /* Straight alpha (non-premultiplied) */
	G2D_PREMUL_ALPHA = 1,  /* Premultiplied alpha (RGB *= A) */
};

/**
 * enum g2d_bld_mode - Porter-Duff blending modes
 *
 * Defines how source and destination are combined during alpha blending.
 * 
 * IMPORTANT: Hardware quirk compensation is applied internally. Applications
 * should use standard Porter-Duff semantics (e.g., G2D_BLD_SRCOVER for
 * "source over destination").
 */
enum g2d_bld_mode {
	G2D_BLD_CLEAR = 0,      /* Clear: 0 */
	G2D_BLD_COPY = 1,       /* Copy source: Src */
	G2D_BLD_DST = 2,        /* Keep destination: Dst */
	G2D_BLD_DSTOVER = 3,    /* Destination Over: Dst + Src*(1-Ad) */
	G2D_BLD_SRCOVER = 4,    /* Source Over: Src + Dst*(1-As) [DEFAULT] */
	G2D_BLD_DSTIN = 5,      /* Destination In: Dst*As */
	G2D_BLD_SRCIN = 6,      /* Source In: Src*Ad */
	G2D_BLD_DSTOUT = 7,     /* Destination Out: Dst*(1-As) */
	G2D_BLD_SRCOUT = 8,     /* Source Out: Src*(1-Ad) */
	G2D_BLD_DSTATOP = 9,    /* Destination Atop: Dst*As + Src*(1-Ad) */
	G2D_BLD_SRCATOP = 10,   /* Source Atop: Src*Ad + Dst*(1-As) */
	G2D_BLD_XOR = 11,       /* XOR: Src*(1-Ad) + Dst*(1-As) */
};

/*
 * ============================================================================
 * COLOR KEYING (CHROMA KEY)
 * ============================================================================
 */

/**
 * enum g2d_color_key_mode - Color keying transparency mode
 */
enum g2d_color_key_mode {
	G2D_CK_MODE_INSIDE_TRANSPARENT = 0,   /* RGB in [min,max] → transparent */
	G2D_CK_MODE_OUTSIDE_TRANSPARENT = 1,  /* RGB outside [min,max] → transparent */
};

/*
 * ============================================================================
 * RASTER OPERATIONS (ROP3)
 * ============================================================================
 */

/**
 * enum g2d_rop3_mode - Ternary raster operation codes
 *
 * Standard ROP3 codes for mask operations.
 * S = Source, D = Destination, P = Pattern
 */
enum g2d_rop3_mode {
	G2D_ROP3_BLACKNESS = 0x00,      /* 0 */
	G2D_ROP3_NOTSRCERASE = 0x11,    /* ~(S | D) */
	G2D_ROP3_NOTSRCCOPY = 0x33,     /* ~S */
	G2D_ROP3_SRCERASE = 0x44,       /* S & ~D */
	G2D_ROP3_DSTINVERT = 0x55,      /* ~D */
	G2D_ROP3_PATINVERT = 0x5A,      /* P ^ D */
	G2D_ROP3_SRCINVERT = 0x66,      /* S ^ D */
	G2D_ROP3_SRCAND = 0x88,         /* S & D */
	G2D_ROP3_MERGEPAINT = 0xBB,     /* ~S | D */
	G2D_ROP3_MERGECOPY = 0xC0,      /* S & P */
	G2D_ROP3_SRCCOPY = 0xCC,        /* S */
	G2D_ROP3_SRCPAINT = 0xEE,       /* S | D */
	G2D_ROP3_PATCOPY = 0xF0,        /* P */
	G2D_ROP3_WHITENESS = 0xFF,      /* 1 */
};

/*
 * ============================================================================
 * BUFFER STRUCTURES
 * ============================================================================
 */

/**
 * struct g2d_buf - G2D buffer description
 *
 * @width:        Buffer width in pixels
 * @height:       Buffer height in pixels
 * @format:       Pixel format (enum g2d_pixel_format)
 * @stride:       Bytes per line for each plane (up to 3 planes)
 * @crop_x:       Crop rectangle X offset (0 = no crop)
 * @crop_y:       Crop rectangle Y offset
 * @crop_w:       Crop rectangle width (0 = full width)
 * @crop_h:       Crop rectangle height (0 = full height)
 * @dma_fd:       DMA-BUF file descriptor (-1 for physical address mode)
 * @alpha:        Global alpha value (0-255, 255 = opaque)
 * @alpha_mode:   Alpha control mode (enum g2d_alpha_mode)
 * @premul_mode:  Alpha premultiplication (enum g2d_premul_mode)
 * @color_space:  YUV color space (enum g2d_color_space, YUV formats only)
 *
 * Note: Fields are ordered to minimize padding with natural alignment.
 */
struct g2d_buf {
	__u32 width;
	__u32 height;
	__u32 format;
	__u32 stride[3];
	
	__u32 crop_x;
	__u32 crop_y;
	__u32 crop_w;
	__u32 crop_h;
	
	__s32 dma_fd;
	
	__u8 alpha;
	__u8 alpha_mode;
	__u8 premul_mode;
	__u8 color_space;
};

/*
 * ============================================================================
 * OPERATION FLAGS
 * ============================================================================
 */

/* Transformation flags (rotation and flipping) */
#define G2D_BLIT_FLAG_ROTATE_90   (1u << 1)
#define G2D_BLIT_FLAG_ROTATE_180  (1u << 2)
#define G2D_BLIT_FLAG_ROTATE_270  (1u << 3)
#define G2D_BLIT_FLAG_FLIP_H      (1u << 4)
#define G2D_BLIT_FLAG_FLIP_V      (1u << 5)

/* Generic operation flags */
#define G2D_FLAG_TILE_REPEAT      (1u << 8)  /* Tile source to fill destination */

/* Deprecated flags (kept for API compatibility) */
#define G2D_BLIT_FLAG_ALPHA_BLEND (1u << 0)  /* Deprecated: auto-detected */
#define G2D_BLIT_FLAG_ASYNC       (1u << 6)  /* Deprecated: always async */

/*
 * ============================================================================
 * BLIT OPERATION
 * ============================================================================
 */

/**
 * struct g2d_blit - Unified blit operation
 *
 * This structure handles all blitting operations including:
 * - Simple copy (no flags)
 * - Scaling (when dst_w/dst_h differ from src crop size)
 * - Alpha blending (via src/dst alpha settings)
 * - Rotation/flip (via flags, cannot combine with scaling)
 *
 * Buffer modes:
 * - If out.dma_fd == -1: In-place (dst is both input and output)
 * - If out.dma_fd >= 0: Three-buffer (dst=background, src=foreground, out=result)
 *
 * Alpha blending is enabled when:
 * - out.dma_fd >= 0 (three-buffer mode)
 * - src.alpha_mode == G2D_GLOBAL_ALPHA or G2D_MIXER_ALPHA
 * - dst.alpha_mode == G2D_GLOBAL_ALPHA or G2D_MIXER_ALPHA
 * - src.alpha_mode == G2D_PIXEL_ALPHA and src format has alpha channel
 *
 * Hardware limitations:
 * - Cannot combine scaling and rotation (use two passes)
 *
 * @src:                Source/foreground buffer
 * @dst:                Destination/background buffer
 * @out:                Output buffer (set dma_fd=-1 for in-place)
 * @dst_x:              Destination X coordinate
 * @dst_y:              Destination Y coordinate
 * @dst_w:              Destination width (for scaling)
 * @dst_h:              Destination height (for scaling)
 * @flags:              Operation flags (G2D_BLIT_FLAG_*)
 * @bld_mode:           Blending mode (enum g2d_bld_mode)
 * @color_key_enable:   1 = enable color keying, 0 = disable
 * @color_key_mode:     Color keying mode (enum g2d_color_key_mode)
 * @color_key_min:      Minimum RGB value (0xRRGGBB)
 * @color_key_max:      Maximum RGB value (0xRRGGBB)
 * @fence_fd_in:        Input fence to wait on (-1 = none)
 * @fence_fd_out:       Output fence signaled when done (filled by kernel)
 */
struct g2d_blit {
	struct g2d_buf src;
	struct g2d_buf dst;
	struct g2d_buf out;
	
	__u32 dst_x;
	__u32 dst_y;
	__u32 dst_w;
	__u32 dst_h;
	
	__u32 flags;
	__u32 bld_mode;
	
	__u32 color_key_enable;
	__u32 color_key_mode;
	__u32 color_key_min;
	__u32 color_key_max;
	
	__s32 fence_fd_in;
	__s32 fence_fd_out;
};

/*
 * ============================================================================
 * FILL RECTANGLE OPERATION
 * ============================================================================
 */

/**
 * struct g2d_fillrect - Fill rectangle with solid color
 *
 * @dst:           Destination buffer
 * @dst_x:         Rectangle X coordinate
 * @dst_y:         Rectangle Y coordinate
 * @dst_w:         Rectangle width
 * @dst_h:         Rectangle height
 * @color:         Fill color in format specified by color_format
 * @color_format:  Format of color value (enum g2d_pixel_format)
 * @global_alpha:  Global alpha (0-255, 0 defaults to 255)
 * @fence_fd_in:   Input fence (-1 = none)
 * @fence_fd_out:  Output fence (filled by kernel)
 */
struct g2d_fillrect {
	struct g2d_buf dst;
	
	__u32 dst_x;
	__u32 dst_y;
	__u32 dst_w;
	__u32 dst_h;
	
	__u32 color;
	__u32 color_format;
	__u32 global_alpha;
	
	__s32 fence_fd_in;
	__s32 fence_fd_out;
};

/*
 * ============================================================================
 * UNIFIED COMMAND INTERFACE
 * ============================================================================
 */

/**
 * enum g2d_cmd_type - Command types for unified interface
 */
enum g2d_cmd_type {
	G2D_CMD_COPY = 1,
	G2D_CMD_SCALE = 2,
	G2D_CMD_BLEND = 3,
	G2D_CMD_ROTATE = 4,
	G2D_CMD_MASK = 5,
	G2D_CMD_FILLRECT = 6,
};

/**
 * struct g2d_cmd - Unified command structure
 *
 * This is a flexible command structure that can represent different operations.
 * The cmd_type field determines which buffers and parameters are used.
 */
struct g2d_cmd {
	__u32 cmd_type;
	
	struct g2d_buf src;
	struct g2d_buf dst;
	struct g2d_buf out;
	struct g2d_buf mask;
	struct g2d_buf ptn;
	
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
			__u32 global_alpha;
		} fillrect;
		
		struct {
			__u32 bld_mode;
			__u32 color_key_enable;
			__u32 color_key_mode;
			__u32 color_key_min;
			__u32 color_key_max;
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
			__u32 alpha_enable;
			__u32 back_flag;
			__u32 fore_flag;
		} mask;
	} params;
};

/*
 * ============================================================================
 * TASK BATCHING
 * ============================================================================
 */

/**
 * enum g2d_task_cmd - Task management commands
 */
enum g2d_task_cmd {
	G2D_TASK_CREATE = 1,
	G2D_TASK_ADD = 2,
	G2D_TASK_RUN = 3,
	G2D_TASK_DEL = 4,
};

/**
 * struct g2d_task_req - Task request for batch operations
 *
 * @task_cmd:      Task command (enum g2d_task_cmd)
 * @task_id:       Task identifier (input/output)
 * @step:          Command to add (for G2D_TASK_ADD)
 * @fence_fd_out:  Output fence (for G2D_TASK_RUN)
 */
struct g2d_task_req {
	__u32 task_cmd;
	__u32 task_id;
	struct g2d_cmd step;
	__s32 fence_fd_out;
};

/*
 * ============================================================================
 * BUFFER MANAGEMENT
 * ============================================================================
 */

/* Buffer allocation flags */
#define G2D_ALLOC_F_CONTIGUOUS  (1u << 0)  /* Require IOVA-contiguous DMA segment */
#define G2D_ALLOC_F_COHERENT    (1u << 1)  /* Prefer cache-coherent mapping */

/**
 * struct g2d_alloc_buffer - Buffer allocation request
 *
 * @size:    Buffer size in bytes (input)
 * @dma_fd:  DMA-BUF file descriptor (output)
 * @flags:   Allocation flags (G2D_ALLOC_F_*)
 */
struct g2d_alloc_buffer {
	__u64 size;
	__s32 dma_fd;
	__u32 flags;
};

/**
 * struct g2d_buffer_rw - Buffer read/write request
 *
 * @dma_fd:    DMA-BUF file descriptor
 * @offset:    Offset in buffer
 * @size:      Number of bytes to transfer
 * @user_ptr:  Userspace buffer pointer
 */
struct g2d_buffer_rw {
	__s32 dma_fd;
	__u64 offset;
	__u64 size;
	__u64 user_ptr;
};

/*
 * ============================================================================
 * VERSION AND DEBUGGING
 * ============================================================================
 */

/**
 * struct g2d_version - G2D version information
 *
 * @hw_version:         Hardware IP version register
 * @driver_major:       Driver major version
 * @driver_minor:       Driver minor version
 * @driver_patchlevel:  Driver patch level
 */
struct g2d_version {
	__u32 hw_version;
	__u32 driver_major;
	__u32 driver_minor;
	__u32 driver_patchlevel;
};

/**
 * struct g2d_selftest - Self-test fence creation
 *
 * @timeout_ms:  Delay before signaling (milliseconds)
 * @fence_fd:    Returned fence file descriptor (output)
 */
struct g2d_selftest {
	__s32 timeout_ms;
	__s32 fence_fd;
};

/*
 * ============================================================================
 * IOCTL DEFINITIONS
 * ============================================================================
 */

#define G2D_IOC_MAGIC  'G'

#define G2D_IOC_GET_VERSION     _IOR(G2D_IOC_MAGIC, 0x01, struct g2d_version)
#define G2D_IOC_CMD             _IOWR(G2D_IOC_MAGIC, 0x02, struct g2d_cmd)
#define G2D_IOC_ALLOC_BUFFER    _IOWR(G2D_IOC_MAGIC, 0x03, struct g2d_alloc_buffer)
#define G2D_IOC_SELFTEST_FENCE  _IOWR(G2D_IOC_MAGIC, 0x04, struct g2d_selftest)
#define G2D_IOC_WRITE_BUFFER    _IOW(G2D_IOC_MAGIC, 0x05, struct g2d_buffer_rw)
#define G2D_IOC_READ_BUFFER     _IOR(G2D_IOC_MAGIC, 0x06, struct g2d_buffer_rw)
#define G2D_IOC_SYNC            _IOW(G2D_IOC_MAGIC, 0x07, int)
#define G2D_IOC_TASK            _IOWR(G2D_IOC_MAGIC, 0x08, struct g2d_task_req)
#define G2D_IOC_SET_CSC_ADJUST  _IOW(G2D_IOC_MAGIC, 0x50, struct g2d_csc_adjust)
#define G2D_IOC_GET_CSC_ADJUST  _IOR(G2D_IOC_MAGIC, 0x51, struct g2d_csc_adjust)

#endif /* _UAPI_SUNXI_G2D_H */
