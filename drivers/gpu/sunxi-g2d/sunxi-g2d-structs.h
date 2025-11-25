// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Allwinner G2D Register Structures
 * 
 * Copyright (C) 2025 Sergio Perez
 *
 * Hardware register structures for G2D IP (T113/D1 and similar SoCs).
 * Based on Allwinner BSP driver structures.
 * 
 * These structures map directly to hardware register blocks and are used for:
 * 1. Direct register access via MMIO
 * 2. RCQ (Register Command Queue) block construction
 * 3. Type-safe register field manipulation
 *
 * All structures use __packed and __aligned(4) to ensure correct memory layout.
 * Unions allow both bitfield access (.bits.field) and raw access (.dwval).
 */

#ifndef __SUNXI_G2D_STRUCTS_H__
#define __SUNXI_G2D_STRUCTS_H__

#include <linux/types.h>

/* ============================================================================
 * G2D Enhanced Enumerations (BSP V2)
 * Readable constants for hardware configuration
 * Note: g2d_alpha_mode is now in UAPI header (sunxi_g2d.h)
 * ============================================================================ */

/**
 * enum g2d_blt_flags - Blit operation flags
 * Bitflags for controlling blit operations (alpha, color key, transforms)
 */
typedef enum {
	G2D_BLT_NONE          = 0x00000000,
	G2D_BLT_PIXEL_ALPHA   = 0x00000001,
	G2D_BLT_PLANE_ALPHA   = 0x00000002,
	G2D_BLT_MULTI_ALPHA   = 0x00000004,
	G2D_BLT_SRC_COLORKEY  = 0x00000008,
	G2D_BLT_DST_COLORKEY  = 0x00000010,
	G2D_BLT_FLIP_HORIZONTAL = 0x00000020,
	G2D_BLT_FLIP_VERTICAL = 0x00000040,
	G2D_BLT_ROTATE90      = 0x00000080,
	G2D_BLT_ROTATE180     = 0x00000100,
	G2D_BLT_ROTATE270     = 0x00000200,
	G2D_BLT_MIRROR45      = 0x00000400,
	G2D_BLT_MIRROR135     = 0x00000800,
} g2d_blt_flags;

/**
 * enum g2d_fillrect_flags - Fillrect operation flags
 */
typedef enum {
	G2D_FIL_NONE        = 0x00000000,
	G2D_FIL_PIXEL_ALPHA = 0x00000001,
	G2D_FIL_PLANE_ALPHA = 0x00000002,
	G2D_FIL_MULTI_ALPHA = 0x00000004,
} g2d_fillrect_flags;

/**
 * enum g2d_blt_flags_h - Extended blit flags (ROP and rotation)
 */
typedef enum {
	G2D_BLT_NONE_0      = 0x0,
	G2D_BLT_BLACKNESS   = 0x1,
	G2D_BLT_NOTMERGEPEN = 0x2,
	G2D_BLT_MASKNOTPEN  = 0x3,
	G2D_BLT_NOTCOPYPEN  = 0x4,
	G2D_BLT_MASKPENNOT  = 0x5,
	G2D_BLT_NOT         = 0x6,
	G2D_BLT_XORPEN      = 0x7,
	G2D_BLT_NOTMASKPEN  = 0x8,
	G2D_BLT_MASKPEN     = 0x9,
	G2D_BLT_NOTXORPEN   = 0xa,
	G2D_BLT_NOP         = 0xb,
	G2D_BLT_MERGENOTPEN = 0xc,
	G2D_BLT_COPYPEN     = 0xd,
	G2D_BLT_MERGEPENNOT = 0xe,
	G2D_BLT_MERGEPEN    = 0xf,
	G2D_BLT_WHITENESS   = 0x000000ff,
	G2D_ROT_90          = 0x00000100,
	G2D_ROT_180         = 0x00000200,
	G2D_ROT_270         = 0x00000300,
	G2D_ROT_H           = 0x00001000,
	G2D_ROT_V           = 0x00002000,
	G2D_SM_DTLR_1       = 0x10000000,
} g2d_blt_flags_h;

/**
 * enum g2d_fmt_enh - Enhanced pixel format codes
 * Hardware format codes for V0 (video layer) and UI layers.
 * Note: Formats >= 0x20 are invalid for UI channels (video layer only)
 */
typedef enum {
	G2D_FORMAT_ARGB8888 = 0,
	G2D_FORMAT_ABGR8888 = 1,
	G2D_FORMAT_RGBA8888 = 2,
	G2D_FORMAT_BGRA8888 = 3,
	G2D_FORMAT_XRGB8888 = 4,
	G2D_FORMAT_XBGR8888 = 5,
	G2D_FORMAT_RGBX8888 = 6,
	G2D_FORMAT_BGRX8888 = 7,
	G2D_FORMAT_RGB888 = 8,
	G2D_FORMAT_BGR888 = 9,
	G2D_FORMAT_RGB565 = 10,
	G2D_FORMAT_BGR565 = 11,
	G2D_FORMAT_ARGB4444 = 12,
	G2D_FORMAT_ABGR4444 = 13,
	G2D_FORMAT_RGBA4444 = 14,
	G2D_FORMAT_BGRA4444 = 15,
	G2D_FORMAT_ARGB1555 = 16,
	G2D_FORMAT_ABGR1555 = 17,
	G2D_FORMAT_RGBA5551 = 18,
	G2D_FORMAT_BGRA5551 = 19,
	G2D_FORMAT_ARGB2101010 = 20,
	G2D_FORMAT_ABGR2101010 = 21,
	G2D_FORMAT_RGBA1010102 = 22,
	G2D_FORMAT_BGRA1010102 = 23,
	/* Invalid for UI channels - video layer only */
	G2D_FORMAT_IYUV422_V0Y1U0Y0 = 0x20,
	G2D_FORMAT_IYUV422_Y1V0Y0U0 = 0x21,
	G2D_FORMAT_IYUV422_U0Y1V0Y0 = 0x22,
	G2D_FORMAT_IYUV422_Y1U0Y0V0 = 0x23,
	G2D_FORMAT_YUV422UVC_V1U1V0U0 = 0x24,
	G2D_FORMAT_YUV422UVC_U1V1U0V0 = 0x25,
	G2D_FORMAT_YUV422_PLANAR = 0x26,
	G2D_FORMAT_YUV420UVC_V1U1V0U0 = 0x28,
	G2D_FORMAT_YUV420UVC_U1V1U0V0 = 0x29,
	G2D_FORMAT_YUV420_PLANAR = 0x2a,
	G2D_FORMAT_YUV411UVC_V1U1V0U0 = 0x2c,
	G2D_FORMAT_YUV411UVC_U1V1U0V0 = 0x2d,
	G2D_FORMAT_YUV411_PLANAR = 0x2e,
	G2D_FORMAT_Y8 = 0x30,
	/* YUV 10bit formats */
	G2D_FORMAT_YVU10_P010 = 0x34,
	G2D_FORMAT_YVU10_P210 = 0x36,
	G2D_FORMAT_YVU10_444 = 0x38,
	G2D_FORMAT_YUV10_444 = 0x39,
} g2d_fmt_enh;

/**
 * enum g2d_rop3_cmd_flag - ROP3 (Raster Operation) commands
 * Standard ROP3 operations for bitblt
 */
typedef enum {
	G2D_ROP3_BLACKNESS    = 0x00,
	G2D_ROP3_NOTSRCERASE  = 0x11,
	G2D_ROP3_NOTSRCCOPY   = 0x33,
	G2D_ROP3_SRCERASE     = 0x44,
	G2D_ROP3_DSTINVERT    = 0x55,
	G2D_ROP3_PATINVERT    = 0x5A,
	G2D_ROP3_SRCINVERT    = 0x66,
	G2D_ROP3_SRCAND       = 0x88,
	G2D_ROP3_MERGEPAINT   = 0xBB,
	G2D_ROP3_MERGECOPY    = 0xC0,
	G2D_ROP3_SRCCOPY      = 0xCC,
	G2D_ROP3_SRCPAINT     = 0xEE,
	G2D_ROP3_PATCOPY      = 0xF0,
	G2D_ROP3_PATPAINT     = 0xFB,
	G2D_ROP3_WHITENESS    = 0xFF,
} g2d_rop3_cmd_flag;

/**
 * enum g2d_color_gmt - Color gamut / color space standards
 * @G2D_BT601: ITU-R BT.601 (SD video)
 * @G2D_BT709: ITU-R BT.709 (HD video)
 * @G2D_BT2020: ITU-R BT.2020 (UHD video)
 */
typedef enum {
	G2D_BT601 = 0,
	G2D_BT709 = 1,
	G2D_BT2020 = 2,
} g2d_color_gmt;

/* ============================================================================
 * TOP Register Block (offset 0x0000)
 * Controls global clocking, reset, and RCQ operation
 * ============================================================================ */

/**
 * union g2d_sclk_gate - Source clock gating control
 * @dwval: Direct 32-bit register access
 * @bits.mixer_sclk_gate: Enable MIXER module source clock (1=enabled)
 * @bits.rot_sclk_gate: Enable ROT module source clock (1=enabled)
 *
 * Controls source clocks for MIXER and ROT (rotator) modules.
 * Both bits should be set to 1 for normal operation.
 */
union g2d_sclk_gate {
	u32 dwval;
	struct {
		u32 mixer_sclk_gate:1;
		u32 rot_sclk_gate:1;
		u32 res0:30;
	} bits;
};

/**
 * union g2d_hclk_gate - AHB clock gating control
 * @dwval: Direct 32-bit register access
 * @bits.mixer_hclk_gate: Enable MIXER module AHB clock (1=enabled)
 * @bits.rot_hclk_gate: Enable ROT module AHB clock (1=enabled)
 *
 * Controls AHB bus clocks for MIXER and ROT modules.
 * Both bits should be set to 1 for normal operation.
 */
union g2d_hclk_gate {
	u32 dwval;
	struct {
		u32 mixer_hclk_gate:1;
		u32 rot_hclk_gate:1;
		u32 res0:30;
	} bits;
};

/**
 * union g2d_ahb_reset - AHB reset control
 * @dwval: Direct 32-bit register access
 * @bits.mixer_ahb_rst: MIXER module reset (0=reset, 1=deassert)
 * @bits.rot_ahb_rst: ROT module reset (0=reset, 1=deassert)
 *
 * Reset control for MIXER and ROT modules.
 * Write 0 to reset, then 1 to release from reset.
 */
union g2d_ahb_reset {
	u32 dwval;
	struct {
		u32 mixer_ahb_rst:1;
		u32 rot_ahb_rst:1;
		u32 res0:30;
	} bits;
};

/**
 * union g2d_sclk_div - Source clock divider
 * @dwval: Direct 32-bit register access
 * @bits.mixer_sclk_div: MIXER clock divider (actual_div = value + 1)
 * @bits.rot_sclk_div: ROT clock divider (actual_div = value + 1)
 *
 * Divides source clocks for MIXER and ROT modules.
 * Actual divider = register_value + 1.
 */
union g2d_sclk_div {
	u32 dwval;
	struct {
		u32 mixer_sclk_div:4;
		u32 rot_sclk_div:4;
		u32 res0:24;
	} bits;
};

/**
 * union g2d_hw_version - Hardware IP version identification
 * @dwval: Direct 32-bit register access
 * @bits.gsu_no: Number of GSU (Graphics Scaler Unit) instances
 * @bits.vsu_no: Number of VSU (Video Scaler Unit) instances
 * @bits.rtmx_no: RT-WB Mixer availability (1=present)
 * @bits.rot_no: Rotator availability (1=present)
 * @bits.ip_version: IP version number (e.g., 0x0110 for v1.1.0)
 *
 * Read-only register identifying G2D IP capabilities and version.
 * T113 typically reports ip_version=0x0110.
 * Note: Renamed from g2d_version to avoid conflict with UAPI struct.
 */
union g2d_hw_version {
	u32 dwval;
	struct {
		u32 gsu_no:2;
		u32 vsu_no:2;
		u32 rtmx_no:1;
		u32 res0:3;
		u32 rot_no:1;
		u32 res1:7;
		u32 ip_version:16;
	} bits;
};

/**
 * union g2d_rcq_irq_ctl - RCQ interrupt control
 * @dwval: Direct 32-bit register access
 * @bits.rcq_sel: RCQ mode select (implementation specific)
 * @bits.task_end_irq_en: Enable task completion interrupt
 * @bits.rcq_cfg_finish_irq_en: Enable RCQ configuration finish interrupt
 *
 * Controls RCQ-related interrupts.
 * Set task_end_irq_en=1 to get IRQ when RCQ task completes.
 */
union g2d_rcq_irq_ctl {
	u32 dwval;
	struct {
		u32 rcq_sel:1;
		u32 res0:3;
		u32 task_end_irq_en:1;
		u32 res1:1;
		u32 rcq_cfg_finish_irq_en:1;
		u32 res2:25;
	} bits;
};

/**
 * union g2d_rcq_status - RCQ status flags
 * @dwval: Direct 32-bit register access
 * @bits.task_end_irq: Task completion interrupt flag (write 1 to clear)
 * @bits.cfg_finish_irq: Config finish interrupt flag (write 1 to clear)
 * @bits.frame_cnt: Current frame counter
 *
 * Read RCQ status and interrupt flags.
 * Write 1 to interrupt bits to clear them.
 */
union g2d_rcq_status {
	u32 dwval;
	struct {
		u32 task_end_irq:1;
		u32 res0:1;
		u32 cfg_finish_irq:1;
		u32 res1:5;
		u32 frame_cnt:8;
		u32 res2:16;
	} bits;
};

/**
 * union g2d_rcq_ctrl - RCQ control register
 * @dwval: Direct 32-bit register access
 * @bits.update: Trigger RCQ header fetch (write 1 to start)
 * @bits.en: Enable RCQ (T113 v2 specific, bit 4)
 *
 * Controls RCQ operation. Write update=1 to trigger DMA fetch of RCQ header.
 * T113 v2 requires en=1 to enable RCQ operation.
 */
union g2d_rcq_ctrl {
	u32 dwval;
	struct {
		u32 update:1;
		u32 res0:3;
		u32 en:1;
		u32 res1:27;
	} bits;
};

/**
 * union g2d_rcq_header_len - RCQ header length
 * @dwval: Direct 32-bit register access
 * @bits.rcq_header_len: Number of register entries in RCQ header
 *
 * Specifies how many register write entries are in the RCQ header.
 * Each entry is 8 bytes (address + value pair).
 */
union g2d_rcq_header_len {
	u32 dwval;
	struct {
		u32 rcq_header_len:16;
		u32 res0:16;
	} bits;
};

/**
 * struct g2d_top_reg - Complete TOP register block
 * @sclk_gate: Source clock gating (0x00)
 * @hclk_gate: AHB clock gating (0x04)
 * @ahb_rst: AHB reset control (0x08)
 * @sclk_div: Source clock dividers (0x0C)
 * @version: IP version identification (0x10)
 * @res0: Reserved registers (0x14-0x1C)
 * @rcq_irq_ctl: RCQ interrupt control (0x20)
 * @rcq_status: RCQ status flags (0x24)
 * @rcq_ctrl: RCQ control (0x28)
 * @rcq_header_low_addr: RCQ header DMA address low 32-bits (0x2C)
 * @rcq_header_high_addr: RCQ header DMA address high 32-bits (0x30)
 * @rcq_header_len: RCQ header entry count (0x34)
 *
 * Complete TOP register block starting at offset 0x0000.
 * Maps to hardware registers 0x0000-0x0037.
 * Size: 0x38 bytes (14 registers).
 */
struct g2d_top_reg {
	/* 0x00 */
	union g2d_sclk_gate sclk_gate;
	union g2d_hclk_gate hclk_gate;
	union g2d_ahb_reset ahb_rst;
	union g2d_sclk_div sclk_div;
	/* 0x10 */
	union g2d_hw_version version;
	u32 res0[3];
	/* 0x20 */
	union g2d_rcq_irq_ctl rcq_irq_ctl;
	union g2d_rcq_status rcq_status;
	union g2d_rcq_ctrl rcq_ctrl;
	u32 rcq_header_low_addr;
	/* 0x30 */
	u32 rcq_header_high_addr;
	union g2d_rcq_header_len rcq_header_len;
} __packed __aligned(4);

/* ============================================================================
 * MIXER Global Register Block (offset 0x0100)
 * Controls MIXER module operation and interrupts
 * ============================================================================ */

/**
 * union g2d_mixer_ctrl - MIXER control register
 * @dwval: Direct 32-bit register access
 * @bits.scan_order: Scan order mode (0=progressive, 1=interlaced top, 2=interlaced bottom)
 * @bits.bist_en: Built-in self-test enable
 * @bits.start: Start MIXER operation (write 1 to trigger)
 *
 * Main MIXER control. Write start=1 to begin processing.
 * start bit auto-clears when operation completes.
 */
union g2d_mixer_ctrl {
	u32 dwval;
	struct {
		u32 res0:4;
		u32 scan_order:2;
		u32 res1:2;
		u32 bist_en:1;
		u32 res2:22;
		u32 start:1;
	} bits;
};

/**
 * union g2d_mixer_interrupt - MIXER interrupt control/status
 * @dwval: Direct 32-bit register access
 * @bits.mixer_irq: Interrupt pending flag (write 1 to clear)
 * @bits.finish_irq_en: Enable finish interrupt (1=enabled)
 *
 * MIXER interrupt control and status.
 * Set finish_irq_en=1 to get IRQ when MIXER completes.
 * Write mixer_irq=1 to clear pending interrupt.
 */
union g2d_mixer_interrupt {
	u32 dwval;
	struct {
		u32 mixer_irq:1;
		u32 res0:3;
		u32 finish_irq_en:1;
		u32 res1:27;
	} bits;
};

/**
 * struct g2d_mixer_glb_reg - MIXER global register block
 * @mixer_ctrl: MIXER control register (0x00 relative to G2D_MIXER)
 * @mixer_interrupt: Interrupt control/status (0x04)
 *
 * MIXER global control registers starting at offset 0x0100.
 * Size: 8 bytes (2 registers).
 * Note: Additional MIXER registers (fillcolor, size, etc.) follow but are
 * not part of this minimal global block in the BSP structure.
 */
struct g2d_mixer_glb_reg {
	union g2d_mixer_ctrl mixer_ctrl;
	union g2d_mixer_interrupt mixer_interrupt;
} __packed __aligned(4);

/* ============================================================================
 * BLD (Blender) Register Block (offset 0x0400)
 * Controls layer blending, alpha compositing, and color space conversion
 * ============================================================================ */

/**
 * union g2d_mixer_bld_en_ctrl - BLD pipe enable control
 * @dwval: Direct 32-bit register access
 * @bits.p0_fcen: Pipe0 fill color enable (1=use fill color, 0=use layer data)
 * @bits.p1_fcen: Pipe1 fill color enable (1=use fill color, 0=use layer data)
 * @bits.p0_en: Pipe0 enable (1=enabled)
 * @bits.p1_en: Pipe1 enable (1=enabled)
 *
 * Controls which pipes are enabled and whether they use layer data or fill color.
 * Typical alpha blending: p0_en=1, p1_en=1, p0_fcen=0, p1_fcen=0
 */
union g2d_mixer_bld_en_ctrl {
	u32 dwval;
	struct {
		u32 p0_fcen:1;
		u32 p1_fcen:1;
		u32 res0:6;
		u32 p0_en:1;
		u32 p1_en:1;
		u32 res1:22;
	} bits;
};

/**
 * union g2d_mixer_bld_mem_size - BLD memory/channel size
 * @dwval: Direct 32-bit register access
 * @bits.width: Width in pixels (actual = value + 1)
 * @bits.height: Height in pixels (actual = value + 1)
 *
 * Specifies input channel size for blending.
 * Hardware expects value-1, so for 80x80 write 79x79.
 */
union g2d_mixer_bld_mem_size {
	u32 dwval;
	struct {
		u32 width:13;
		u32 res0:3;
		u32 height:13;
		u32 res1:3;
	} bits;
};

/**
 * union g2d_mixer_bld_mem_coor - BLD memory/channel coordinate offset
 * @dwval: Direct 32-bit register access
 * @bits.xcoor: X coordinate offset in pixels
 * @bits.ycoor: Y coordinate offset in pixels
 *
 * Specifies coordinate offset for input channel positioning within blend area.
 * Typically 0,0 for simple overlays.
 */
union g2d_mixer_bld_mem_coor {
	u32 dwval;
	struct {
		u32 xcoor:16;
		u32 ycoor:16;
	} bits;
};

/**
 * union g2d_mixer_bld_premulti_ctrl - BLD premultiplication control
 * @dwval: Direct 32-bit register access
 * @bits.p0_alpha_mode: Pipe0 alpha premultiplication mode
 * @bits.p1_alpha_mode: Pipe1 alpha premultiplication mode
 *
 * Controls whether pipe data is premultiplied by alpha.
 * 0 = non-premultiplied, 1 = premultiplied
 */
union g2d_mixer_bld_premulti_ctrl {
	u32 dwval;
	struct {
		u32 p0_alpha_mode:1;
		u32 p1_alpha_mode:1;
		u32 res0:30;
	} bits;
};

/**
 * union g2d_mixer_bld_ctrl - BLD blend mode control (Porter-Duff)
 * @dwval: Direct 32-bit register access
 * @bits.blend_pfs: Pixel blend factor for source
 * @bits.blend_pfd: Pixel blend factor for destination
 * @bits.blend_afs: Alpha blend factor for source
 * @bits.blend_afd: Alpha blend factor for destination
 *
 * Controls Porter-Duff blend equation factors.
 * Common modes from BSP:
 * - SRCOVER: 0x03010301 (source over destination, standard alpha blend)
 * - COPY:    0x00010001 (source only, no blending)
 * - CLEAR:   0x00000000 (clear destination)
 *
 * Blend equation: OUT = SRC * Fs + DST * Fd
 * where Fs/Fd are determined by these 4-bit factor fields.
 */
union g2d_mixer_bld_ctrl {
	u32 dwval;
	struct {
		u32 blend_pfs:4;  /* Pixel factor source (bits 0-3) */
		u32 res0:4;
		u32 blend_pfd:4;  /* Pixel factor dest (bits 8-11) */
		u32 res1:4;
		u32 blend_afs:4;  /* Alpha factor source (bits 16-19) */
		u32 res2:4;
		u32 blend_afd:4;  /* Alpha factor dest (bits 24-27) */
		u32 res3:4;
	} bits;
};

/**
 * union g2d_mixer_bld_color_key - BLD color key control
 * @dwval: Direct 32-bit register access
 * @bits.key0_en: Color key 0 enable
 * @bits.key0_match_dir: Match direction (0=inside, 1=outside range)
 *
 * Controls color keying (chroma key) for transparency.
 */
union g2d_mixer_bld_color_key {
	u32 dwval;
	struct {
		u32 key0_en:1;
		u32 key0_match_dir:2;
		u32 res0:29;
	} bits;
};

/**
 * union g2d_mixer_bld_color_key_cfg - Color key channel configuration
 * @dwval: Direct 32-bit register access
 * @bits.key0b_match: Match blue/Y channel
 * @bits.key0g_match: Match green/U channel
 * @bits.key0y_match: Match red/V channel
 *
 * Selects which color channels participate in color key matching.
 */
union g2d_mixer_bld_color_key_cfg {
	u32 dwval;
	struct {
		u32 key0b_match:1;
		u32 key0g_match:1;
		u32 key0y_match:1;
		u32 res0:29;
	} bits;
};

/**
 * union g2d_mixer_bld_color_key_max - Color key maximum values
 * @dwval: Direct 32-bit register access
 * @bits.max_b: Maximum blue/Y value
 * @bits.max_g: Maximum green/U value
 * @bits.max_r: Maximum red/V value
 *
 * Upper bound of color key range for each channel.
 */
union g2d_mixer_bld_color_key_max {
	u32 dwval;
	struct {
		u32 max_b:8;
		u32 max_g:8;
		u32 max_r:8;
		u32 res0:8;
	} bits;
};

/**
 * union g2d_mixer_bld_color_key_min - Color key minimum values
 * @dwval: Direct 32-bit register access
 * @bits.min_b: Minimum blue/Y value
 * @bits.min_g: Minimum green/U value
 * @bits.min_r: Minimum red/V value
 *
 * Lower bound of color key range for each channel.
 */
union g2d_mixer_bld_color_key_min {
	u32 dwval;
	struct {
		u32 min_b:8;
		u32 min_g:8;
		u32 min_r:8;
		u32 res0:8;
	} bits;
};

/**
 * union g2d_mixer_bld_output_color - BLD output color mode
 * @dwval: Direct 32-bit register access
 * @bits.premul_en: Output premultiplication enable
 * @bits.alpha_mode: Output alpha mode
 *
 * Controls output color format and alpha handling.
 */
union g2d_mixer_bld_output_color {
	u32 dwval;
	struct {
		u32 premul_en:1;
		u32 alpha_mode:1;
		u32 res0:30;
	} bits;
};

/**
 * union g2d_mixer_bld_cs_ctrl - BLD color space conversion control
 * @dwval: Direct 32-bit register access
 * @bits.cs0_en: CSC0 enable (pipe0 input conversion)
 * @bits.cs1_en: CSC1 enable (pipe1 input conversion)
 * @bits.cs2_en: CSC2 enable (output conversion)
 *
 * Enables color space conversion matrices for RGB<->YUV conversions.
 */
union g2d_mixer_bld_cs_ctrl {
	u32 dwval;
	struct {
		u32 cs0_en:1;
		u32 cs1_en:1;
		u32 cs2_en:1;
		u32 res0:29;
	} bits;
};

/**
 * union g2d_mixer_bld_cs_coeff - CSC coefficient register
 * @dwval: Direct 32-bit register access
 * @bits.coeff: Signed 13-bit coefficient (sign + 12-bit fraction)
 *
 * Color space conversion matrix coefficient.
 * Format: S12.0 (signed 13-bit integer representing fixed-point value).
 */
union g2d_mixer_bld_cs_coeff {
	u32 dwval;
	struct {
		u32 coeff:13;
		u32 res0:19;
	} bits;
};

/**
 * union g2d_mixer_bld_cs_const - CSC constant/offset register
 * @dwval: Direct 32-bit register access
 * @bits.const_val: Signed 20-bit constant offset
 *
 * Color space conversion constant/offset added after matrix multiplication.
 * Format: S19.0 (signed 20-bit integer).
 */
union g2d_mixer_bld_cs_const {
	u32 dwval;
	struct {
		u32 const_val:20;  /* Renamed from 'const' to avoid C keyword */
		u32 res0:12;
	} bits;
};

/* ============================================================================
 * ROP (Raster Operation) Registers - Part of BLD block
 * Define before struct g2d_mixer_bld_reg to avoid forward declaration issues
 * ============================================================================ */

/**
 * union g2d_mixer_rop_ctrl - ROP control register
 * @dwval: Direct 32-bit register access
 * @bits.type: ROP type selection
 * @bits.blue_bypass_en: Bypass blue channel ROP
 * @bits.green_bypass_en: Bypass green channel ROP
 * @bits.red_bypass_en: Bypass red channel ROP
 * @bits.alpha_bypass_en: Bypass alpha channel ROP
 * @bits.blue_ch_sel: Blue channel source select (2-bit)
 * @bits.green_ch_sel: Green channel source select (2-bit)
 * @bits.red_ch_sel: Red channel source select (2-bit)
 * @bits.alpha_ch_sel: Alpha channel source select (2-bit)
 *
 * Controls raster operations (bitwise logic operations) on pixel data.
 * Can apply different operations per channel.
 */
union g2d_mixer_rop_ctrl {
	u32 dwval;
	struct {
		u32 type:1;
		u32 res0:3;
		u32 blue_bypass_en:1;
		u32 green_bypass_en:1;
		u32 red_bypass_en:1;
		u32 alpha_bypass_en:1;
		u32 blue_ch_sel:2;
		u32 green_ch_sel:2;
		u32 red_ch_sel:2;
		u32 alpha_ch_sel:2;
		u32 res1:16;
	} bits;
};

/**
 * union g2d_mixer_rop_ch3_index0 - ROP channel 3 index configuration
 * @dwval: Direct 32-bit register access
 * @bits.index0node0-7: ROP truth table nodes
 * @bits.ch0ign_en: Channel 0 ignore enable
 * @bits.ch1ign_en: Channel 1 ignore enable
 * @bits.ch2ign_en: Channel 2 ignore enable
 *
 * Configures ROP truth table for complex bitwise operations.
 * Allows programmable boolean logic functions.
 */
union g2d_mixer_rop_ch3_index0 {
	u32 dwval;
	struct {
		u32 index0node0:3;
		u32 index0node1:1;
		u32 index0node2:1;
		u32 index0node3:1;
		u32 index0node4:4;
		u32 index0node5:1;
		u32 index0node6:4;
		u32 index0node7:1;
		u32 ch0ign_en:1;
		u32 ch1ign_en:1;
		u32 ch2ign_en:1;
		u32 res0:13;
	} bits;
};

/**
 * struct g2d_mixer_bld_reg - Complete BLD register block
 * @bld_en_ctrl: Pipe enable control (0x00)
 * @res0: Reserved (0x04-0x0C)
 * @bld_fill_color: Fill colors for pipes (0x10-0x14)
 * @res1: Reserved (0x18-0x1C)
 * @mem_size: Input channel sizes [pipe0, pipe1] (0x20-0x24)
 * @res2: Reserved (0x28-0x2C)
 * @mem_coor: Input channel offsets [pipe0, pipe1] (0x30-0x34)
 * @res3: Reserved (0x38-0x3C)
 * @premulti_ctrl: Premultiplication control (0x40)
 * @bld_backgroud_color: Background fill color (0x44)
 * @out_size: Output size (0x48)
 * @bld_ctrl: Blend mode control - Porter-Duff factors (0x4C)
 * @color_key: Color key enable/config (0x50)
 * @color_key_cfg: Color key channel select (0x54)
 * @color_key_max: Color key max values (0x58)
 * @color_key_min: Color key min values (0x5C)
 * @out_color: Output color mode (0x60)
 * @res4: Reserved (0x64-0x7C)
 * @rop_ctrl: ROP control (0x80)
 * @ch3_index0: ROP channel 3 index 0 (0x84)
 * @ch3_index1: ROP channel 3 index 1 (0x88)
 * @res5: Reserved (0x8C-0xFC)
 * @cs_ctrl: Color space conversion control (0x100)
 * @res6: Reserved (0x104-0x10C)
 * @csc0_coeff0_reg0-2: CSC0 matrix row 0 coefficients (0x110-0x118)
 * @csc0_const0: CSC0 row 0 constant (0x11C)
 * @csc0_coeff1_reg0-2: CSC0 matrix row 1 coefficients (0x120-0x128)
 * @csc0_const1: CSC0 row 1 constant (0x12C)
 * @csc0_coeff2_reg0-2: CSC0 matrix row 2 coefficients (0x130-0x138)
 * @csc0_const2: CSC0 row 2 constant (0x13C)
 * @csc1_coeff0_reg0-2: CSC1 matrix row 0 coefficients (0x140-0x148)
 * @csc1_const0: CSC1 row 0 constant (0x14C)
 * @csc1_coeff1_reg0-2: CSC1 matrix row 1 coefficients (0x150-0x158)
 * @csc1_const1: CSC1 row 1 constant (0x15C)
 * @csc1_coeff2_reg0-2: CSC1 matrix row 2 coefficients (0x160-0x168)
 * @csc1_const2: CSC1 row 2 constant (0x16C)
 * @csc2_coeff0_reg0-2: CSC2 matrix row 0 coefficients (0x170-0x178)
 * @csc2_const0: CSC2 row 0 constant (0x17C)
 * @csc2_coeff1_reg0-2: CSC2 matrix row 1 coefficients (0x180-0x188)
 * @csc2_const1: CSC2 row 1 constant (0x18C)
 * @csc2_coeff2_reg0-2: CSC2 matrix row 2 coefficients (0x190-0x198)
 * @csc2_const2: CSC2 row 2 constant (0x19C)
 *
 * Complete BLD register block starting at offset 0x0400.
 * Size: 0x1A0 bytes (104 registers).
 *
 * Key registers for alpha blending:
 * - bld_en_ctrl: Enable pipes and select data source
 * - mem_size[]: Set input sizes for each pipe
 * - mem_coor[]: Position layers within blend area
 * - bld_ctrl: Porter-Duff blend mode (e.g., 0x03010301 for SRCOVER)
 * - out_size: Output dimensions
 * - premulti_ctrl: Alpha premultiplication mode
 */
struct g2d_mixer_bld_reg {
	/* 0x00 */
	union g2d_mixer_bld_en_ctrl bld_en_ctrl;
	u32 res0[3];
	/* 0x10 */
	u32 bld_fill_color[2];
	u32 res1[2];
	/* 0x20 */
	union g2d_mixer_bld_mem_size mem_size[2];
	u32 res2[2];
	/* 0x30 */
	union g2d_mixer_bld_mem_coor mem_coor[2];
	u32 res3[2];
	/* 0x40 */
	union g2d_mixer_bld_premulti_ctrl premulti_ctrl;
	u32 bld_backgroud_color;
	union g2d_mixer_bld_mem_size out_size;
	union g2d_mixer_bld_ctrl bld_ctrl;
	/* 0x50 */
	union g2d_mixer_bld_color_key color_key;
	union g2d_mixer_bld_color_key_cfg color_key_cfg;
	union g2d_mixer_bld_color_key_max color_key_max;
	union g2d_mixer_bld_color_key_min color_key_min;
	/* 0x60 */
	union g2d_mixer_bld_output_color out_color;
	u32 res4[7];
	/* 0x80 */
	union g2d_mixer_rop_ctrl rop_ctrl;
	union g2d_mixer_rop_ch3_index0 ch3_index0;
	union g2d_mixer_rop_ch3_index0 ch3_index1;
	u32 res5[29];
	/* 0x100 */
	union g2d_mixer_bld_cs_ctrl cs_ctrl;
	u32 res6[3];
	/* 0x110 - CSC0 (pipe0 input color space conversion) */
	union g2d_mixer_bld_cs_coeff csc0_coeff0_reg0;
	union g2d_mixer_bld_cs_coeff csc0_coeff0_reg1;
	union g2d_mixer_bld_cs_coeff csc0_coeff0_reg2;
	union g2d_mixer_bld_cs_const csc0_const0;
	/* 0x120 */
	union g2d_mixer_bld_cs_coeff csc0_coeff1_reg0;
	union g2d_mixer_bld_cs_coeff csc0_coeff1_reg1;
	union g2d_mixer_bld_cs_coeff csc0_coeff1_reg2;
	union g2d_mixer_bld_cs_const csc0_const1;
	/* 0x130 */
	union g2d_mixer_bld_cs_coeff csc0_coeff2_reg0;
	union g2d_mixer_bld_cs_coeff csc0_coeff2_reg1;
	union g2d_mixer_bld_cs_coeff csc0_coeff2_reg2;
	union g2d_mixer_bld_cs_const csc0_const2;
	/* 0x140 - CSC1 (pipe1 input color space conversion) */
	union g2d_mixer_bld_cs_coeff csc1_coeff0_reg0;
	union g2d_mixer_bld_cs_coeff csc1_coeff0_reg1;
	union g2d_mixer_bld_cs_coeff csc1_coeff0_reg2;
	union g2d_mixer_bld_cs_const csc1_const0;
	/* 0x150 */
	union g2d_mixer_bld_cs_coeff csc1_coeff1_reg0;
	union g2d_mixer_bld_cs_coeff csc1_coeff1_reg1;
	union g2d_mixer_bld_cs_coeff csc1_coeff1_reg2;
	union g2d_mixer_bld_cs_const csc1_const1;
	/* 0x160 */
	union g2d_mixer_bld_cs_coeff csc1_coeff2_reg0;
	union g2d_mixer_bld_cs_coeff csc1_coeff2_reg1;
	union g2d_mixer_bld_cs_coeff csc1_coeff2_reg2;
	union g2d_mixer_bld_cs_const csc1_const2;
	/* 0x170 - CSC2 (output color space conversion) */
	union g2d_mixer_bld_cs_coeff csc2_coeff0_reg0;
	union g2d_mixer_bld_cs_coeff csc2_coeff0_reg1;
	union g2d_mixer_bld_cs_coeff csc2_coeff0_reg2;
	union g2d_mixer_bld_cs_const csc2_const0;
	/* 0x180 */
	union g2d_mixer_bld_cs_coeff csc2_coeff1_reg0;
	union g2d_mixer_bld_cs_coeff csc2_coeff1_reg1;
	union g2d_mixer_bld_cs_coeff csc2_coeff1_reg2;
	union g2d_mixer_bld_cs_const csc2_const1;
	/* 0x190 */
	union g2d_mixer_bld_cs_coeff csc2_coeff2_reg0;
	union g2d_mixer_bld_cs_coeff csc2_coeff2_reg1;
	union g2d_mixer_bld_cs_coeff csc2_coeff2_reg2;
	union g2d_mixer_bld_cs_const csc2_const2;
} __packed __aligned(4);

/* ============================================================================
 * OVL (Overlay) Layer Registers - Common unions
 * Shared between Video (V0) and UI (UI0/UI1/UI2) layers
 * ============================================================================ */

/**
 * union g2d_mixer_ovl_mem - Layer memory/frame size
 * @dwval: Direct 32-bit register access
 * @bits.lay_width: Layer width in pixels (actual = value + 1)
 * @bits.lay_height: Layer height in pixels (actual = value + 1)
 *
 * Specifies the full frame size of the layer buffer.
 * Hardware expects value-1, so for 80x80 write 79x79.
 */
union g2d_mixer_ovl_mem {
	u32 dwval;
	struct {
		u32 lay_width:13;
		u32 res0:3;
		u32 lay_height:13;
		u32 res1:3;
	} bits;
};

/**
 * union g2d_mixer_ovl_mem_coor - Layer memory coordinate offset
 * @dwval: Direct 32-bit register access
 * @bits.lay_xcoor: X coordinate offset in layer buffer
 * @bits.lay_ycoor: Y coordinate offset in layer buffer
 *
 * Specifies which portion of the layer buffer to use.
 * Typically 0,0 to use from buffer origin.
 */
union g2d_mixer_ovl_mem_coor {
	u32 dwval;
	struct {
		u32 lay_xcoor:16;
		u32 lay_ycoor:16;
	} bits;
};

/**
 * union g2d_mixer_ovl_winsize - Layer window/output size
 * @dwval: Direct 32-bit register access
 * @bits.width: Output width in pixels (actual = value + 1)
 * @bits.height: Output height in pixels (actual = value + 1)
 *
 * Specifies the cropped/scaled output size of the layer.
 * Can differ from lay_width/lay_height for cropping.
 * Hardware expects value-1.
 */
union g2d_mixer_ovl_winsize {
	u32 dwval;
	struct {
		u32 width:13;
		u32 res0:3;
		u32 height:13;
		u32 res1:3;
	} bits;
};

/* ============================================================================
 * Video Layer (V0) Register Block (offset 0x0800)
 * Supports YUV and RGB formats with 3-plane addressing
 * ============================================================================ */

/**
 * union g2d_mixer_ovl_attr - Video layer attributes
 * @dwval: Direct 32-bit register access
 * @bits.lay_en: Layer enable (1=enabled)
 * @bits.alpha_mode: Alpha blending mode
 *   - 0: Use pixel alpha from buffer
 *   - 1: Use global alpha only
 *   - 2: Multiply pixel alpha * global alpha
 * @bits.lay_fillcolor_en: Use fill color instead of memory (1=fill color mode)
 * @bits.lay_fbfmt: Frame buffer format (6-bit format code)
 *   - 0x00: ARGB8888
 *   - 0x04: XRGB8888
 *   - 0x0A: RGB565
 *   - YUV formats: see format enum
 * @bits.lay_premul_ctl: Premultiplication control
 *   - 0: Non-premultiplied alpha
 *   - 1: Premultiplied alpha
 * @bits.lay_glbalpha: Global alpha value (0-255)
 *
 * Main control register for video layer configuration.
 */
union g2d_mixer_ovl_attr {
	u32 dwval;
	struct {
		u32 lay_en:1;
		u32 alpha_mode:2;
		u32 res0:1;
		u32 lay_fillcolor_en:1;
		u32 res1:3;
		u32 lay_fbfmt:6;
		u32 res2:2;
		u32 lay_premul_ctl:2;
		u32 res3:6;
		u32 lay_glbalpha:8;
	} bits;
};

/**
 * union g2d_mixer_ovl_mem_high_addr - Video layer high address bits
 * @dwval: Direct 32-bit register access
 * @bits.lay_y_hadd: High 8 bits of Y/RGB plane address
 * @bits.lay_u_hadd: High 8 bits of U/Cb plane address
 * @bits.lay_v_hadd: High 8 bits of V/Cr plane address
 *
 * Upper 8 bits of 40-bit physical addresses for 3-plane YUV formats.
 * For RGB formats, only lay_y_hadd is used.
 */
union g2d_mixer_ovl_mem_high_addr {
	u32 dwval;
	struct {
		u32 lay_y_hadd:8;
		u32 lay_u_hadd:8;
		u32 lay_v_hadd:8;
		u32 res0:8;
	} bits;
};

/**
 * union g2d_mixer_ovl_down_sample - Layer downsampling control
 * @dwval: Direct 32-bit register access
 * @bits.M: Downsampling M factor (numerator)
 * @bits.N: Downsampling N factor (denominator)
 *
 * Controls downsampling ratio = M/N.
 * Used for chroma subsampling in YUV formats.
 * Typically N=2, M=1 for 4:2:0, or N=M=1 for no downsampling.
 */
union g2d_mixer_ovl_down_sample {
	u32 dwval;
	struct {
		u32 M:14;
		u32 res0:2;
		u32 N:14;
		u32 res1:2;
	} bits;
};

/**
 * struct g2d_mixer_ovl_v_reg - Complete Video layer register block
 * @ovl_attr: Layer attributes and format (0x00)
 * @ovl_mem: Frame buffer size (0x04)
 * @ovl_mem_coor: Coordinate offset in buffer (0x08)
 * @ovl_mem_pitch0: Y/RGB plane pitch in bytes (0x0C)
 * @ovl_mem_pitch1: U/Cb plane pitch in bytes (0x10)
 * @ovl_mem_pitch2: V/Cr plane pitch in bytes (0x14)
 * @ovl_mem_low_addr0: Y/RGB plane address low 32-bits (0x18)
 * @ovl_mem_low_addr1: U/Cb plane address low 32-bits (0x1C)
 * @ovl_mem_low_addr2: V/Cr plane address low 32-bits (0x20)
 * @ovl_fill_color: Fill color for fillcolor mode (0x24)
 * @ovl_mem_high_addr: High 8 bits of 3-plane addresses (0x28)
 * @ovl_winsize: Output window size (0x2C)
 * @hor_down_sample0: Horizontal downsampling for plane 0 (0x30)
 * @hor_down_sample1: Horizontal downsampling for plane 1 (0x34)
 * @ver_down_sample0: Vertical downsampling for plane 0 (0x38)
 * @ver_down_sample1: Vertical downsampling for plane 1 (0x3C)
 *
 * Complete V0 layer register block starting at offset 0x0800.
 * Size: 0x40 bytes (16 registers).
 *
 * Supports both RGB and YUV formats:
 * - RGB: Use ovl_mem_low_addr0 + ovl_mem_pitch0 only
 * - YUV420/422: Use all 3 address/pitch pairs
 */
struct g2d_mixer_ovl_v_reg {
	/* 0x00 */
	union g2d_mixer_ovl_attr ovl_attr;
	union g2d_mixer_ovl_mem ovl_mem;
	union g2d_mixer_ovl_mem_coor ovl_mem_coor;
	u32 ovl_mem_pitch0;
	/* 0x10 */
	u32 ovl_mem_pitch1;
	u32 ovl_mem_pitch2;
	u32 ovl_mem_low_addr0;
	u32 ovl_mem_low_addr1;
	/* 0x20 */
	u32 ovl_mem_low_addr2;
	u32 ovl_fill_color;
	union g2d_mixer_ovl_mem_high_addr ovl_mem_high_addr;
	union g2d_mixer_ovl_winsize ovl_winsize;
	/* 0x30 */
	union g2d_mixer_ovl_down_sample hor_down_sample0;
	union g2d_mixer_ovl_down_sample hor_down_sample1;
	union g2d_mixer_ovl_down_sample ver_down_sample0;
	union g2d_mixer_ovl_down_sample ver_down_sample1;
} __packed __aligned(4);

/* ============================================================================
 * UI Layer (UI0/UI1/UI2) Register Blocks
 * UI0: offset 0x1000, UI1: offset 0x1800, UI2: offset 0x2000
 * RGB-only layers with single-plane addressing
 * ============================================================================ */

/**
 * union g2d_mixer_ovl_u_attr - UI layer attributes
 * @dwval: Direct 32-bit register access
 * @bits.lay_en: Layer enable (1=enabled)
 * @bits.alpha_mode: Alpha blending mode
 *   - 0: Use pixel alpha from buffer
 *   - 1: Use global alpha only (layer alpha)
 *   - 2: Multiply pixel alpha * global alpha
 * @bits.lay_fillcolor_en: Use fill color instead of memory (1=fill color mode)
 * @bits.lay_fbfmt: Frame buffer format (5-bit format code for UI)
 *   - 0x00: ARGB8888
 *   - 0x01: ABGR8888 (byte-swapped)
 *   - 0x04: XRGB8888
 *   - 0x0A: RGB565
 * @bits.lay_premul_ctl: Premultiplication control
 *   - 0: Non-premultiplied alpha
 *   - 1: Premultiplied alpha
 * @bits.lay_glbalpha: Global alpha value (0-255)
 *
 * UI layer control register. Similar to video layer but:
 * - 5-bit format field (not 6-bit)
 * - RGB formats only (no YUV)
 * - Single plane addressing
 */
union g2d_mixer_ovl_u_attr {
	u32 dwval;
	struct {
		u32 lay_en:1;
		u32 alpha_mode:2;
		u32 res0:1;
		u32 lay_fillcolor_en:1;
		u32 res1:3;
		u32 lay_fbfmt:5;     /* 5 bits for UI, not 6 */
		u32 res2:3;
		u32 lay_premul_ctl:2;
		u32 res3:6;
		u32 lay_glbalpha:8;
	} bits;
};

/**
 * struct g2d_mixer_ovl_u_reg - Complete UI layer register block
 * @ovl_attr: Layer attributes and format (0x00)
 * @ovl_mem: Frame buffer size (0x04)
 * @ovl_mem_coor: Coordinate offset in buffer (0x08)
 * @ovl_mem_pitch0: Line pitch in bytes (0x0C)
 * @ovl_mem_low_addr0: Buffer address low 32-bits (0x10)
 * @ovl_fill_color: Fill color for fillcolor mode (0x14)
 * @ovl_mem_high_addr: High 8 bits of buffer address (0x18)
 * @ovl_winsize: Output window size (0x1C)
 *
 * Complete UI layer register block.
 * UI0: offset 0x1000, UI1: offset 0x1800, UI2: offset 0x2000.
 * Size: 0x20 bytes (8 registers).
 *
 * Simpler than video layer:
 * - Single plane only (RGB formats)
 * - No downsampling controls
 * - Smaller register count
 */
struct g2d_mixer_ovl_u_reg {
	/* 0x00 */
	union g2d_mixer_ovl_u_attr ovl_attr;
	union g2d_mixer_ovl_mem ovl_mem;
	union g2d_mixer_ovl_mem_coor ovl_mem_coor;
	u32 ovl_mem_pitch0;
	/* 0x10 */
	u32 ovl_mem_low_addr0;
	u32 ovl_fill_color;
	u32 ovl_mem_high_addr;   /* Simple u32, not union for UI layers */
	union g2d_mixer_ovl_winsize ovl_winsize;
} __packed __aligned(4);

/* ============================================================================
 * WB (Writeback) Register Block (offset 0x3000)
 * Controls output to memory (destination buffer)
 * ============================================================================ */

/**
 * union g2d_mixer_wb_attr - Writeback attributes
 * @dwval: Direct 32-bit register access
 * @bits.fmt: Output format (6-bit format code)
 *   - 0x00: ARGB8888
 *   - 0x04: XRGB8888
 *   - 0x0A: RGB565
 * @bits.round_en: Rounding enable for format conversion
 *
 * Controls writeback output format and processing.
 */
union g2d_mixer_wb_attr {
	u32 dwval;
	struct {
		u32 en:1;         /* Writeback enable (bit 0) */
		u32 res0:5;
		u32 fmt:6;
		u32 res1:2;
		u32 round_en:1;
		u32 res2:17;
	} bits;
};

/**
 * union g2d_mixer_wb_data_size - Writeback data size
 * @dwval: Direct 32-bit register access
 * @bits.width: Output width in pixels (actual = value + 1)
 * @bits.height: Output height in pixels (actual = value + 1)
 *
 * Specifies writeback output dimensions.
 * Hardware expects value-1.
 */
union g2d_mixer_wb_data_size {
	u32 dwval;
	struct {
		u32 width:13;
		u32 res0:3;
		u32 height:13;
		u32 res1:3;
	} bits;
};

/**
 * union g2d_mixer_wb_crop_coor - Writeback crop coordinate
 * @dwval: Direct 32-bit register access
 * @bits.xcoor: X coordinate (0-8191)
 * @bits.ycoor: Y coordinate (0-8191)
 */
union g2d_mixer_wb_crop_coor {
	u32 dwval;
	struct {
		u32 xcoor:16;
		u32 ycoor:16;
	} bits;
};

/**
 * struct g2d_mixer_write_back_reg - Complete Writeback register block
 * @wb_attr: Attribute control (enable, format)
 * @data_size: Output size (width, height)
 * @pitch0/1/2: Line stride for each plane
 * @laddr0/1/2: Low 32-bits of DMA address for each plane
 * @haddr0/1/2: High 32-bits of DMA address for each plane
 * @wb_crop_coor: Crop coordinate (X,Y) for output position
 *
 * For RGB formats, only pitch0/laddr0/haddr0 are used.
 * For YUV formats, all 3 plane addresses/pitches are used.
 */
struct g2d_mixer_write_back_reg {
	/* 0x00 */
	union g2d_mixer_wb_attr wb_attr;
	union g2d_mixer_wb_data_size data_size;
	u32 pitch0;
	u32 pitch1;
	/* 0x10 */
	u32 pitch2;
	u32 laddr0;
	u32 haddr0;
	u32 laddr1;
	/* 0x20 */
	u32 haddr1;
	u32 laddr2;
	u32 haddr2;
	union g2d_mixer_wb_crop_coor wb_crop_coor;
} __packed __aligned(4);

/* ============================================================================
 * VS (Video Scaler) Register Block (offset 0x8000)
 * Hardware scaling engine with programmable filter coefficients
 * ============================================================================ */

/**
 * union g2d_mixer_vs_ctrl - Video scaler control
 * @dwval: Direct 32-bit register access
 * @bits.en: Scaler enable (1=enabled)
 * @bits.coef_access_sel: Coefficient access select
 * @bits.filter_type: Filter type selection
 * @bits.core_rst: Core reset
 * @bits.bist_en: Built-in self test enable
 *
 * Main control register for video scaler.
 */
union g2d_mixer_vs_ctrl {
	u32 dwval;
	struct {
		u32 en:1;
		u32 res0:7;
		u32 coef_access_sel:1;
		u32 res1:7;
		u32 filter_type:1;
		u32 res2:13;
		u32 core_rst:1;
		u32 bist_en:1;
	} bits;
};

/**
 * union g2d_mixer_vs_out_size - Video scaler output size
 * @dwval: Direct 32-bit register access
 * @bits.out_width: Output width in pixels (actual = value + 1)
 * @bits.out_height: Output height in pixels (actual = value + 1)
 *
 * Specifies scaled output dimensions.
 */
union g2d_mixer_vs_out_size {
	u32 dwval;
	struct {
		u32 out_width:13;
		u32 res0:3;
		u32 out_height:13;
		u32 res1:3;
	} bits;
};

/**
 * union g2d_mixer_vs_glb_alpha - Video scaler global alpha
 * @dwval: Direct 32-bit register access
 * @bits.glb_alpha: Global alpha value (0-255)
 *
 * Global alpha applied to scaled output.
 */
union g2d_mixer_vs_glb_alpha {
	u32 dwval;
	struct {
		u32 glb_alpha:8;
		u32 res0:24;
	} bits;
};

/**
 * union g2d_mixer_vs_ch_size - Video scaler channel size
 * @dwval: Direct 32-bit register access
 * @bits.y_width: Channel width in pixels (actual = value + 1)
 * @bits.y_height: Channel height in pixels (actual = value + 1)
 *
 * Specifies input channel size for Y or Chroma.
 */
union g2d_mixer_vs_ch_size {
	u32 dwval;
	struct {
		u32 y_width:13;
		u32 res0:3;
		u32 y_height:13;
		u32 res1:3;
	} bits;
};

/**
 * union g2d_mixer_vs_step - Video scaler phase step
 * @dwval: Direct 32-bit register access
 * @bits.frac: Fractional part (19 bits)
 * @bits.integer: Integer part (4 bits)
 *
 * Controls scaling phase step (ratio).
 * Format: 4.19 fixed-point (4 integer bits, 19 fractional bits).
 * Step = input_size / output_size
 */
union g2d_mixer_vs_step {
	u32 dwval;
	struct {
		u32 res0:1;
		u32 frac:19;
		u32 integer:4;
		u32 res1:8;
	} bits;
};

/**
 * union g2d_mixer_vs_filter_coeff - Video scaler filter coefficient
 * @dwval: Direct 32-bit register access
 * @bits.coff0: Coefficient 0 (signed 8-bit)
 * @bits.coff1: Coefficient 1 (signed 8-bit)
 * @bits.coff2: Coefficient 2 (signed 8-bit)
 * @bits.coff3: Coefficient 3 (signed 8-bit)
 *
 * Four filter tap coefficients packed in one register.
 * Each coefficient is a signed 8-bit value.
 */
union g2d_mixer_vs_filter_coeff {
	u32 dwval;
	struct {
		u32 coff0:8;
		u32 coff1:8;
		u32 coff2:8;
		u32 coff3:8;
	} bits;
};

/**
 * struct g2d_mixer_video_scaler_reg - Complete Video Scaler register block
 * @vs_ctrl: Scaler control (0x00)
 * @res0: Reserved (0x04-0x3C)
 * @out_size: Output size (0x40)
 * @glb_alpha: Global alpha (0x44)
 * @res1: Reserved (0x48-0x7C)
 * @y_ch_size: Y channel input size (0x80)
 * @res2: Reserved (0x84)
 * @y_hor_step: Y horizontal phase step (0x88)
 * @y_ver_step: Y vertical phase step (0x8C)
 * @y_hor_phase: Y horizontal initial phase (0x90)
 * @res3: Reserved (0x94)
 * @y_ver_phase: Y vertical initial phase (0x98)
 * @res4: Reserved (0x9C-0xBC)
 * @c_ch_size: Chroma channel input size (0xC0)
 * @res5: Reserved (0xC4)
 * @c_hor_step: Chroma horizontal phase step (0xC8)
 * @c_ver_step: Chroma vertical phase step (0xCC)
 * @c_hor_phase: Chroma horizontal initial phase (0xD0)
 * @res6: Reserved (0xD4)
 * @c_ver_phase: Chroma vertical initial phase (0xD8)
 * @res7: Reserved (0xDC-0x1FC)
 * @vs_y_ch_hor_filter_coef: Y horizontal filter coefficients [32] (0x200-0x27C)
 * @res8: Reserved (0x280-0x2FC)
 * @vs_y_ch_ver_filter_coef: Y vertical filter coefficients [32] (0x300-0x37C)
 * @res9: Reserved (0x380-0x3FC)
 * @vs_c_ch_hor_filter_coef: Chroma horizontal filter coefficients [32] (0x400-0x47C)
 *
 * Complete VS register block starting at offset 0x8000.
 * Size: 0x480 bytes (includes large coefficient tables).
 *
 * Key concepts:
 * - Y and Chroma channels have separate scaling controls
 * - Phase step controls scaling ratio (input/output)
 * - 32 coefficient sets for polyphase filtering
 * - Each coefficient set has 4 taps (coff0-3)
 */
struct g2d_mixer_video_scaler_reg {
	/* 0x00 */
	union g2d_mixer_vs_ctrl vs_ctrl;
	u32 res0[15];
	/* 0x40 */
	union g2d_mixer_vs_out_size out_size;
	union g2d_mixer_vs_glb_alpha glb_alpha;
	u32 res1[14];
	/* 0x80 */
	union g2d_mixer_vs_ch_size y_ch_size;
	u32 res2;
	union g2d_mixer_vs_step y_hor_step;
	union g2d_mixer_vs_step y_ver_step;
	/* 0x90 */
	union g2d_mixer_vs_step y_hor_phase;
	u32 res3;
	union g2d_mixer_vs_step y_ver_phase;
	u32 res4[9];
	/* 0xC0 */
	union g2d_mixer_vs_ch_size c_ch_size;
	u32 res5;
	union g2d_mixer_vs_step c_hor_step;
	union g2d_mixer_vs_step c_ver_step;
	/* 0xD0 */
	union g2d_mixer_vs_step c_hor_phase;
	u32 res6;
	union g2d_mixer_vs_step c_ver_phase;
	u32 res7[73];
	/* 0x200 */
	union g2d_mixer_vs_filter_coeff vs_y_ch_hor_filter_coef[32];
	u32 res8[32];
	/* 0x300 */
	union g2d_mixer_vs_filter_coeff vs_y_ch_ver_filter_coef[32];
	u32 res9[32];
	/* 0x400 */
	union g2d_mixer_vs_filter_coeff vs_c_ch_hor_filter_coef[32];
} __packed __aligned(4);

/* ============================================================================
 * ROT (Rotator) Register Block (offset 0x28000)
 * Hardware rotation and flip engine - independent from MIXER pipeline
 * ============================================================================ */

/**
 * union g2d_rot_ctrl - Rotator control register
 * @dwval: Direct 32-bit register access
 * @bits.mode_sel: Operation mode select
 *   - 0: Rotation mode
 *   - 1: Other modes (implementation specific)
 * @bits.degree: Rotation angle
 *   - 0: 0 degrees (no rotation)
 *   - 1: 90 degrees clockwise
 *   - 2: 180 degrees
 *   - 3: 270 degrees clockwise
 * @bits.vflip_en: Vertical flip enable (mirror on X-axis)
 * @bits.hflip_en: Horizontal flip enable (mirror on Y-axis)
 * @bits.bist_en: Built-in self test enable
 * @bits.start: Start rotation operation (write 1 to trigger)
 *
 * Main control register for rotator.
 * Can combine rotation + flips for all 8 possible orientations.
 */
union g2d_rot_ctrl {
	u32 dwval;
	struct {
		u32 mode_sel:2;
		u32 res0:2;
		u32 degree:2;
		u32 vflip_en:1;
		u32 hflip_en:1;
		u32 res1:22;
		u32 bist_en:1;
		u32 start:1;
	} bits;
};

/**
 * union g2d_rot_interrupt - Rotator interrupt control/status
 * @dwval: Direct 32-bit register access
 * @bits.rot_irq: Rotation interrupt pending flag (write 1 to clear)
 * @bits.finish_irq: Finish interrupt enable (1=enabled)
 *
 * Interrupt control and status for rotator completion.
 */
union g2d_rot_interrupt {
	u32 dwval;
	struct {
		u32 rot_irq:1;
		u32 res0:15;
		u32 finish_irq:1;
		u32 res1:15;
	} bits;
};

/**
 * union g2d_rot_time_ctrl - Rotator timeout control
 * @dwval: Direct 32-bit register access
 * @bits.timeout_st: Timeout status flag
 * @bits.timeout_rst_en: Timeout reset enable
 * @bits.timeout_rst: Timeout reset trigger
 *
 * Controls timeout detection and recovery for rotator.
 */
union g2d_rot_time_ctrl {
	u32 dwval;
	struct {
		u32 timeout_st:1;
		u32 res0:29;
		u32 timeout_rst_en:1;
		u32 timeout_rst:1;
	} bits;
};

/**
 * union g2d_rot_in_fmt - Rotator input format
 * @dwval: Direct 32-bit register access
 * @bits.fmt: Input pixel format (6-bit format code)
 *   - 0x00: ARGB8888
 *   - 0x04: XRGB8888
 *   - 0x0A: RGB565
 *   - YUV formats also supported
 *
 * Specifies input buffer pixel format.
 */
union g2d_rot_in_fmt {
	u32 dwval;
	struct {
		u32 fmt:6;
		u32 res0:26;
	} bits;
};

/**
 * union g2d_rot_size - Rotator image size
 * @dwval: Direct 32-bit register access
 * @bits.width: Image width in pixels (actual = value + 1)
 * @bits.height: Image height in pixels (actual = value + 1)
 *
 * Specifies image dimensions.
 * Note: For 90/270 degree rotation, output size swaps width/height.
 */
union g2d_rot_size {
	u32 dwval;
	struct {
		u32 width:13;
		u32 res0:3;
		u32 height:13;
		u32 res1:3;
	} bits;
};

/**
 * union g2d_rot_rand_ctrl - Rotator randomization control
 * @dwval: Direct 32-bit register access
 * @bits.rand_en: Randomization enable
 * @bits.mode: Randomization mode
 * @bits.seed: Random seed value (24-bit)
 *
 * Controls data randomization feature (if supported).
 * Implementation specific - may be used for DRAM efficiency.
 */
union g2d_rot_rand_ctrl {
	u32 dwval;
	struct {
		u32 rand_en:1;
		u32 res0:3;
		u32 mode:2;
		u32 res1:2;
		u32 seed:24;
	} bits;
};

/**
 * union g2d_rot_rand_clk - Rotator randomization clock control
 * @dwval: Direct 32-bit register access
 * @neg_num: Negative number count (16-bit)
 * @pos_num: Positive number count (16-bit)
 *
 * Controls randomization timing parameters.
 */
union g2d_rot_rand_clk {
	u32 dwval;
	struct {
		u32 neg_num:16;
		u32 pos_num:16;
	} bits;  /* Added bits struct for consistency */
};

/**
 * struct g2d_rot_reg - Complete Rotator register block
 * @rot_ctrl: Rotation control (0x00)
 * @rot_int: Interrupt control/status (0x04)
 * @time_ctrl: Timeout control (0x08)
 * @res0: Reserved (0x0C-0x1C)
 * @infmt: Input format (0x20)
 * @insize: Input size (0x24)
 * @res1: Reserved (0x28-0x2C)
 * @pitch0: Input plane 0 pitch in bytes (0x30)
 * @pitch1: Input plane 1 pitch in bytes (0x34)
 * @pitch2: Input plane 2 pitch in bytes (0x38)
 * @res2: Reserved (0x3C)
 * @laddr0: Input plane 0 address low 32-bits (0x40)
 * @haddr0: Input plane 0 address high 8-bits (0x44)
 * @laddr1: Input plane 1 address low 32-bits (0x48)
 * @haddr1: Input plane 1 address high 8-bits (0x4C)
 * @laddr2: Input plane 2 address low 32-bits (0x50)
 * @haddr2: Input plane 2 address high 8-bits (0x54)
 * @res3: Reserved (0x58-0x80)
 * @outsize: Output size (0x84)
 * @res4: Reserved (0x88-0x8C)
 * @out_pitch0: Output plane 0 pitch in bytes (0x90)
 * @out_pitch1: Output plane 1 pitch in bytes (0x94)
 * @out_pitch2: Output plane 2 pitch in bytes (0x98)
 * @res5: Reserved (0x9C)
 * @out_laddr0: Output plane 0 address low 32-bits (0xA0)
 * @out_haddr0: Output plane 0 address high 8-bits (0xA4)
 * @out_laddr1: Output plane 1 address low 32-bits (0xA8)
 * @out_haddr1: Output plane 1 address high 8-bits (0xAC)
 * @out_laddr2: Output plane 2 address low 32-bits (0xB0)
 * @out_haddr2: Output plane 2 address high 8-bits (0xB4)
 * @rand_in_ctrl: Input randomization control (0xB8)
 * @rand_in_clk: Input randomization clock (0xBC)
 * @rand_out_ctrl: Output randomization control (0xC0)
 * @rand_out_clk: Output randomization clock (0xC4)
 *
 * Complete ROT register block starting at offset 0x28000.
 * Size: 0xC8 bytes (50 registers).
 *
 * Rotator operates independently from MIXER pipeline:
 * - Dedicated input/output buffers
 * - Supports 0/90/180/270 degree rotation
 * - Can combine rotation with H/V flips
 * - Separate interrupt from MIXER
 *
 * Note: For 90/270 degree rotation:
 *   outsize.width = insize.height
 *   outsize.height = insize.width
 *
 * Typical usage:
 * 1. Configure input: format, size, addresses
 * 2. Configure output: size, addresses
 * 3. Set rotation: degree, flip flags
 * 4. Enable interrupt
 * 5. Write start=1 to rot_ctrl
 * 6. Wait for interrupt
 */
struct g2d_rot_reg {
	/* 0x00 */
	union g2d_rot_ctrl rot_ctrl;
	union g2d_rot_interrupt rot_int;
	union g2d_rot_time_ctrl time_ctrl;
	u32 res0[5];
	/* 0x20 */
	union g2d_rot_in_fmt infmt;
	union g2d_rot_size insize;
	u32 res1[2];
	/* 0x30 */
	u32 pitch0;
	u32 pitch1;
	u32 pitch2;
	u32 res2;
	/* 0x40 */
	u32 laddr0;
	u32 haddr0;
	u32 laddr1;
	u32 haddr1;
	/* 0x50 */
	u32 laddr2;
	u32 haddr2;
	u32 res3[11];
	/* 0x84 */
	union g2d_rot_size outsize;
	u32 res4[2];
	/* 0x90 */
	u32 out_pitch0;
	u32 out_pitch1;
	u32 out_pitch2;
	u32 res5;
	/* 0xA0 */
	u32 out_laddr0;
	u32 out_haddr0;
	u32 out_laddr1;
	u32 out_haddr1;
	/* 0xB0 */
	u32 out_laddr2;
	u32 out_haddr2;
	union g2d_rot_rand_ctrl rand_in_ctrl;
	union g2d_rot_rand_clk rand_in_clk;
	/* 0xC0 */
	union g2d_rot_rand_ctrl rand_out_ctrl;
	union g2d_rot_rand_clk rand_out_clk;
} __packed __aligned(4);

#endif /* __SUNXI_G2D_STRUCTS_H__ */
