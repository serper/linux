// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * sunxi-g2d-rcq-builders.c - RCQ block builder functions
 * 
 * Copyright (C) 2025 Sergio Perez
 * 
 * Modular helper functions that build pre-configured RCQ register blocks.
 * Each function allocates and returns a configured block ready to be added
 * to an RCQ frame, promoting code reuse across different operations (fillrect,
 * bitblt, composite, etc.).
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <uapi/linux/sunxi_g2d.h>
#include "sunxi-g2d-rcq.h"
#include "sunxi-g2d-regs.h"
#include "sunxi-g2d-structs.h"
#include "sunxi-g2d-scaler-coeffs.h"
#include "sunxi-g2d-csc-tables.h"

/*
 * Get BLD_CTL value for Porter-Duff blending mode
 * 
 * BLD_CTL format: [31:24]=pipe3 [23:16]=pipe2 [15:8]=pipe1 [7:0]=pipe0
 */
static u32 g2d_rcq_get_bld_mode(u32 mode)
{
	switch (mode) {
	case G2D_BLD_CLEAR: /* 0 */
		return 0x00000000;
	case G2D_BLD_COPY: /* 1 */
		return 0x00010001;
	case G2D_BLD_DST: /* 2 */
		return 0x01000100;
	case G2D_BLD_SRCOVER: /* 4 */
		return 0x03010301;
	case G2D_BLD_DSTOVER: /* 3 */
		return 0x01030103;
	case G2D_BLD_SRCIN: /* 6 */
		return 0x00020002;
	case G2D_BLD_DSTIN: /* 5 */
		return 0x02000200;
	case G2D_BLD_SRCOUT: /* 8 */
		return 0x00030003;
	case G2D_BLD_DSTOUT: /* 7 */
		return 0x03000300;
	case G2D_BLD_SRCATOP: /* 10 */
		return 0x03020302;
	case G2D_BLD_DSTATOP: /* 9 */
		return 0x02030203;
	case G2D_BLD_XOR: /* 11 */
		return 0x03030303;
	default:
		return 0x03010301; /* Default to SRCOVER */
	}
}

/**
 * g2d_rcq_bld_csc_set - Configure CSC (Color Space Conversion) in BLD block
 * @bld: Pointer to BLD register structure
 * @csc_no: CSC unit number (0=CSC0/pipe0, 1=CSC1/pipe1/V0, 2=CSC2/output)
 * @mode: CSC mode (G2D_CSC_*)
 * 
 * Configures CSC matrix coefficients and enable bit in the BLD block.
 * This is the RCQ-based version of the BSP's bld_csc_reg_set().
 * 
 * Uses "Full -> Limit" range for RGB->YUV (encoding/saving).
 * Uses "Limit -> Full" range for YUV->RGB (playback/display).
 */
static enum g2d_rcq_csc_mode g2d_rcq_select_mode(u8 color_space,
						 bool yuv_to_rgb)
{
	switch (color_space) {
	case G2D_COLOR_SPACE_BT709:
		return yuv_to_rgb ? G2D_CSC_YUV2RGB_709 : G2D_CSC_RGB2YUV_709;
	case G2D_COLOR_SPACE_BT2020:
		return yuv_to_rgb ? G2D_CSC_YUV2RGB_2020 :
				    G2D_CSC_RGB2YUV_2020;
	case G2D_COLOR_SPACE_BT601:
	default:
		return yuv_to_rgb ? G2D_CSC_YUV2RGB_601 : G2D_CSC_RGB2YUV_601;
	}
}

static void g2d_rcq_bld_csc_set(struct g2d_mixer_bld_reg *bld, u32 csc_no,
				enum g2d_rcq_csc_mode mode,
				struct g2d_csc_state *csc_state)
{
	void *csc_base_addr;
	const s32 *coeff_table;
	int offset = 0;
	bool swap_rb = false;
	s32 coeff_swizzled[12];
	
	if (!bld)
		return;
	
	/* Set fill colors (BSP pattern) */
	bld->bld_fill_color[0] = 0x00108080;
	bld->bld_fill_color[1] = 0x00108080;
	
	/* Select coefficient table and offset */
	switch (mode) {
	case G2D_CSC_RGB2YUV_601:
		coeff_table = rgb2Ycbcr_601;
		offset = 0; /* Full -> Limit */
		break;
	case G2D_CSC_RGB2YUV_709:
		coeff_table = rgb2Ycbcr_709;
		offset = 0; /* Full -> Limit */
		break;
	case G2D_CSC_YUV2RGB_601:
		/* Use dynamic table if available and initialized */
		if (csc_state && csc_state->base_601) {
			coeff_table = csc_state->current_601;
		} else {
			coeff_table = Ycbcr2rgb_601;
		}
		offset = 36; /* Limit -> Full (0x24) */
		swap_rb = true;
		break;
	case G2D_CSC_YUV2RGB_709:
		/* Use dynamic table if available and initialized */
		if (csc_state && csc_state->base_709)
			coeff_table = csc_state->current_709;
		else
			coeff_table = Ycbcr2rgb_709;
		offset = 36; /* Limit -> Full (0x24) */
		swap_rb = true;
		break;
	case G2D_CSC_RGB2YUV_2020:
		coeff_table = rgb2Ycbcr_2020;
		offset = 0;
		break;
	case G2D_CSC_YUV2RGB_2020:
		if (csc_state && csc_state->base_2020)
			coeff_table = csc_state->current_2020;
		else
			coeff_table = Ycbcr2rgb_2020;
		offset = 36;
		swap_rb = true;
		break;
	default:
		return;
	}
	
	/* Select CSC unit and enable bit */
	switch (csc_no) {
	case 0:  /* CSC0: pipe0 (UI0) input conversion */
		csc_base_addr = &bld->csc0_coeff0_reg0;
		bld->cs_ctrl.bits.cs0_en = 1;
		break;
	case 1:  /* CSC1: pipe1 (V0) input conversion */
		csc_base_addr = &bld->csc1_coeff0_reg0;
		bld->cs_ctrl.bits.cs1_en = 1;
		break;
	case 2:  /* CSC2: output (blender result) conversion */
		csc_base_addr = &bld->csc2_coeff0_reg0;
		bld->cs_ctrl.bits.cs2_en = 1;
		break;
	default:
		pr_warn("Invalid CSC number: %u\n", csc_no);
		return;
	}
	
	/* Copy 12 coefficient registers (3 rows × 4 values each) */
	/* Note: If using dynamic table, offset is already applied or table is full size?
	 * The static tables are 48 ints. Offset 36 is the last block.
	 * My dynamic tables are 48 ints too.
	 * g2d_csc_update should update the block at offset 36.
	 */
	/* Optionally swap R/B rows for hardware quirk */
	if (swap_rb) {
		/* Rows are in order R,G,B; swap row0 and row2 */
		memcpy(coeff_swizzled + 0, coeff_table + offset + 8,
		       4 * sizeof(s32)); /* B -> R slot */
		memcpy(coeff_swizzled + 4, coeff_table + offset + 4,
		       4 * sizeof(s32)); /* G stays */
		memcpy(coeff_swizzled + 8, coeff_table + offset + 0,
		       4 * sizeof(s32)); /* R -> B slot */
		memcpy(csc_base_addr, coeff_swizzled, sizeof(coeff_swizzled));
	} else {
		memcpy(csc_base_addr, coeff_table + offset,
		       12 * sizeof(u32));
	}
}

/**
 * g2d_rcq_build_v0_fillcolor - Build V0 overlay block configured for fill color
 * @width: Width in pixels
 * @height: Height in pixels
 * @pitch: Pitch/stride in bytes
 * @color: Fill color value (format depends on @color_fmt)
 * @color_fmt: Color format (G2D_FORMAT_* from sunxi-g2d-structs.h)
 * @out_block: Output pointer to receive allocated block data
 * @out_size: Output size of block in bytes
 * 
 * Returns: 0 on success, negative error on failure
 * 
 * The caller must kfree(*out_block) when done.
 */
int g2d_rcq_build_v0_fillcolor(u32 width, u32 height, u32 pitch,
				 u32 color, u32 color_fmt,
				 u32 **out_block, u32 *out_size)
{
	struct g2d_mixer_ovl_v_reg v0 = { 0 };
	u32 *regs;
	if (!out_block || !out_size)
		return -EINVAL;

	/* V0 block: Full 16 registers (0x40 bytes) matching struct g2d_mixer_ovl_v_reg */
	regs = kzalloc(16 * sizeof(u32), GFP_KERNEL);
	if (!regs)
		return -ENOMEM;

	/* Configure V0 for fill color mode */
	v0.ovl_attr.bits.lay_en = 1;
	v0.ovl_attr.bits.lay_fillcolor_en = 1; /* Use fill color */
	v0.ovl_attr.bits.lay_fbfmt = color_fmt;
	v0.ovl_attr.bits.lay_glbalpha = 0xFF;

	v0.ovl_mem.bits.lay_width = width - 1;
	v0.ovl_mem.bits.lay_height = height - 1;
	v0.ovl_mem_coor.bits.lay_xcoor = 0;
	v0.ovl_mem_coor.bits.lay_ycoor = 0;
	v0.ovl_mem_pitch0 = pitch;
	v0.ovl_fill_color = color;
	v0.ovl_winsize.bits.width = width - 1;
	v0.ovl_winsize.bits.height = height - 1;

	/* Pack FULL struct into register array (16 regs) */
	regs[0] = v0.ovl_attr.dwval;
	regs[1] = v0.ovl_mem.dwval;
	regs[2] = v0.ovl_mem_coor.dwval;
	regs[3] = v0.ovl_mem_pitch0;
	regs[4] = v0.ovl_mem_pitch1;
	regs[5] = v0.ovl_mem_pitch2;
	regs[6] = v0.ovl_mem_low_addr0;
	regs[7] = v0.ovl_mem_low_addr1;
	regs[8] = v0.ovl_mem_low_addr2;
	regs[9] = v0.ovl_fill_color;
	regs[10] = v0.ovl_mem_high_addr.dwval;
	regs[11] = v0.ovl_winsize.dwval;
	regs[12] = v0.hor_down_sample0.dwval;
	regs[13] = v0.hor_down_sample1.dwval;
	regs[14] = v0.ver_down_sample0.dwval;
	regs[15] = v0.ver_down_sample1.dwval;

	*out_block = regs;
	*out_size = 16 * sizeof(u32);

	return 0;
}

/**
 * g2d_rcq_build_v0_memory - Build V0 block for memory source
 * @width: Layer width in pixels (ovl_mem size)
 * @height: Layer height in pixels (ovl_mem size)
 * @stride: Array of 3 pitches/strides in bytes [Y/RGB, U/UV, V] for multi-plane formats
 * @dma_addr: Source DMA base address
 * @plane_offset: Array of 3 plane offsets from base [Y/RGB, U/UV, V] for multi-plane formats
 * @src_fmt: Source format (G2D_FORMAT_* from sunxi-g2d-structs.h)
 * @win_x: Window X coordinate (ovl_mem_coor, BSP g2d_vlayer_overlay_set)
 * @win_y: Window Y coordinate (ovl_mem_coor, BSP g2d_vlayer_overlay_set)
 * @win_w: Window width (ovl_winsize)
 * @win_h: Window height (ovl_winsize)
 * @out_block: Output pointer to receive allocated block data
 * @out_size: Output size of block in bytes
 * 
 * Supports RGB packed, YUV packed, YUV semi-planar, and YUV planar formats.
 * For RGB/packed: only stride[0], plane_offset[0] used
 * For semi-planar (NV12/NV21): stride[0,1], plane_offset[0,1] used
 * For planar (I420/YV12): stride[0,1,2], plane_offset[0,1,2] used
 * 
 * Window positioning (win_x, win_y) and size (win_w, win_h) determine which part
 * of the layer is visible in the blend output (BSP g2d_vlayer_overlay_set pattern).
 * 
 * Returns: 0 on success, negative error on failure
 * 
 * The caller must kfree(*out_block) when done.
 */
int g2d_rcq_build_v0_memory(u32 width, u32 height, const u32 stride[3],
                             dma_addr_t dma_addr, const u32 plane_offset[3],
                             u32 src_fmt,
                             u32 win_x, u32 win_y, u32 win_w, u32 win_h,
                             u32 **out_block, u32 *out_size)
{
	struct g2d_mixer_ovl_v_reg v0 = {0};
	u32 *regs;
	u64 plane0_dma, plane1_dma, plane2_dma;
	
	if (!out_block || !out_size || !stride || !plane_offset)
		return -EINVAL;

	/* V0 block: Full 16 registers (0x40 bytes) matching struct g2d_mixer_ovl_v_reg */
	regs = kzalloc(16 * sizeof(u32), GFP_KERNEL);
	if (!regs)
		return -ENOMEM;
	
	/* Calculate plane DMA addresses
	 * For semi-planar formats (NV12/NV21), stride[2] is 0 indicating no V plane
	 * For RGB/packed formats, stride[1] and stride[2] are 0
	 * Only calculate address if stride is non-zero (plane is used)
	 */
	plane0_dma = dma_addr + plane_offset[0];
	plane1_dma = stride[1] ? (dma_addr + plane_offset[1]) : 0;
	plane2_dma = stride[2] ? (dma_addr + plane_offset[2]) : 0;
	
	/* Configure V0 for memory source */
	v0.ovl_attr.bits.lay_en = 1;
	v0.ovl_attr.bits.lay_fillcolor_en = 0;  /* Read from memory */
	v0.ovl_attr.bits.lay_fbfmt = src_fmt;
	v0.ovl_attr.bits.lay_glbalpha = 0xFF;
	
	/* Configure Alpha Mode
	 * Mode 0 = Pixel Alpha (use alpha channel from pixel data)
	 * Mode 1 = Global Alpha (use lay_glbalpha value)
	 * 
	 * CRITICAL: For formats without alpha (XRGB, YUV, etc.), we MUST use Global Alpha.
	 * If we use Pixel Alpha for XRGB/XBGR, the hardware reads the 'X' byte (usually 0)
	 * as alpha, resulting in a transparent (black) image.
	 */
	bool has_alpha = (src_fmt == G2D_FORMAT_ARGB8888 || src_fmt == G2D_FORMAT_ABGR8888 ||
			  src_fmt == G2D_FORMAT_RGBA8888 || src_fmt == G2D_FORMAT_BGRA8888 ||
			  src_fmt == G2D_FORMAT_ARGB4444 || src_fmt == G2D_FORMAT_ABGR4444 ||
			  src_fmt == G2D_FORMAT_RGBA4444 || src_fmt == G2D_FORMAT_BGRA4444 ||
			  src_fmt == G2D_FORMAT_ARGB1555 || src_fmt == G2D_FORMAT_ABGR1555 ||
			  src_fmt == G2D_FORMAT_RGBA5551 || src_fmt == G2D_FORMAT_BGRA5551 ||
			  src_fmt == G2D_FORMAT_ARGB2101010 || src_fmt == G2D_FORMAT_ABGR2101010 ||
			  src_fmt == G2D_FORMAT_RGBA1010102 || src_fmt == G2D_FORMAT_BGRA1010102);

	if (has_alpha) {
		v0.ovl_attr.bits.alpha_mode = 0; /* Pixel Alpha */
	} else {
		v0.ovl_attr.bits.alpha_mode = 1; /* Global Alpha */
	}
	
	/* Enable pixel alpha bit (Bit 16) and lay_en (Bit 0)
	 * Legacy code sets 0xff010001 (alpha=0xff, pixel_alpha=1, lay_en=1)
	 * Note: lay_en is already set by bits.lay_en=1 above.
	 * 
	 * UPDATE: Bit 16 is PREMUL_CTL. For YUV, we probably don't want premultiplied alpha.
	 * Let's try disabling it to see if it fixes artifacts.
	 * 
	 * UPDATE 2: For XRGB/XBGR formats, we MUST NOT set bit 16 if it means "use pixel alpha"
	 * or "premultiplied".
	 * Actually, bit 16 is PREMUL_CTL bit 0.
	 * If we set it, we enable premultiplication.
	 * If alpha_mode is GLOBAL (1), premultiplication shouldn't matter, but let's be safe.
	 * 
	 * Let's ONLY set bit 16 if we are using Pixel Alpha (alpha_mode=0).
	 * FIX: Do NOT set bit 16 (PREMUL_CTL) by default. It assumes premultiplied source.
	 * Standard ARGB is not premultiplied.
	 */
	v0.ovl_attr.dwval &= ~(1 << 16);
	
	/* Layer memory region (full layer size) */
	v0.ovl_mem.bits.lay_width = width - 1;
	v0.ovl_mem.bits.lay_height = height - 1;
	
	/* Window position in output (BSP g2d_vlayer_overlay_set pattern)
	 * This determines where the V0 layer's visible region appears */
	v0.ovl_mem_coor.bits.lay_xcoor = win_x;
	v0.ovl_mem_coor.bits.lay_ycoor = win_y;
	
	/* Configure pitches for all 3 planes */
	v0.ovl_mem_pitch0 = stride[0];
	v0.ovl_mem_pitch1 = stride[1];
	v0.ovl_mem_pitch2 = stride[2];
	
	/* Configure DMA addresses for all 3 planes
	 * For semi-planar (stride[2]=0), plane1_dma and plane2_dma will be 0
	 */
	v0.ovl_mem_low_addr0 = (u32)(plane0_dma & 0xFFFFFFFF);
	v0.ovl_mem_low_addr1 = (u32)(plane1_dma & 0xFFFFFFFF);
	v0.ovl_mem_low_addr2 = (u32)(plane2_dma & 0xFFFFFFFF);
	
	/* High address bits (only write if plane is used) */
	v0.ovl_mem_high_addr.bits.lay_y_hadd = (u32)(plane0_dma >> 32);
	v0.ovl_mem_high_addr.bits.lay_u_hadd = plane1_dma ? (u32)(plane1_dma >> 32) : 0;
	v0.ovl_mem_high_addr.bits.lay_v_hadd = plane2_dma ? (u32)(plane2_dma >> 32) : 0;
	
	/* Window size (visible region of layer in output) */
	v0.ovl_winsize.bits.width = win_w - 1;
	v0.ovl_winsize.bits.height = win_h - 1;
	
	/* Configure Chroma Subsampling (HDS/VDS)
	 * NOTE: Programming these registers on T113 causes a hardware timeout (-110).
	 * We leave them at 0 (default), hoping the hardware derives subsampling
	 * from the format code (lay_fbfmt) or that 0 means "auto".
	 * 
	 * Previous attempts to set M=1/N=2 for 4:2:0 caused hangs.
	 */
	// v0.hor_down_sample0.dwval = ...
	
	/* Pack FULL struct into register array (16 regs) */
	regs[0] = v0.ovl_attr.dwval;
	regs[1] = v0.ovl_mem.dwval;
	regs[2] = v0.ovl_mem_coor.dwval;
	regs[3] = v0.ovl_mem_pitch0;
	regs[4] = v0.ovl_mem_pitch1;
	regs[5] = v0.ovl_mem_pitch2;
	regs[6] = v0.ovl_mem_low_addr0;
	regs[7] = v0.ovl_mem_low_addr1;
	regs[8] = v0.ovl_mem_low_addr2;
	regs[9] = v0.ovl_fill_color;
	regs[10] = v0.ovl_mem_high_addr.dwval;
	regs[11] = v0.ovl_winsize.dwval;
	regs[12] = v0.hor_down_sample0.dwval;
	regs[13] = v0.hor_down_sample1.dwval;
	regs[14] = v0.ver_down_sample0.dwval;
	regs[15] = v0.ver_down_sample1.dwval;
	
	/* Debug: Log V0 configuration for memory sources */
	pr_debug("V0_BUILDER: attr=0x%08x mem=0x%08x coor=0x%08x fmt=0x%02x\n",
		regs[0], regs[1], regs[2], src_fmt);
	pr_debug("V0_BUILDER: pitch0=%u addr0=0x%08x size=%ux%u win=%ux%u@%u,%u\n",
		regs[3], regs[6], width, height, win_w, win_h, win_x, win_y);
	
	*out_block = regs;
	*out_size = 16 * sizeof(u32);
	
	return 0;
}

/**
 * g2d_rcq_build_bld_fillcolor - Build BLD block for fill color operation
 * @width: Width in pixels
 * @height: Height in pixels
 * @fill_color: Fill color value for pipe 1
 * @porter_duff: Porter-Duff blend mode (e.g., 0x03010301 for SRCOVER)
 * @out_block: Output pointer to receive allocated block structure
 * @out_size: Output size of block in bytes
 * 
 * Configures BLD with dual-pipe setup (pipe0=V0, pipe1=fill color).
 * 
 * Returns: 0 on success, negative error on failure
 * 
 * The caller must kfree(*out_block) when done.
 */
int g2d_rcq_build_bld_fillcolor(u32 width, u32 height, u32 fill_color,
                                 u32 porter_duff,
                                 struct g2d_mixer_bld_reg **out_block,
                                 u32 *out_size)
{
	struct g2d_mixer_bld_reg *bld;
	
	if (!out_block || !out_size)
		return -EINVAL;
	
	bld = kzalloc(sizeof(*bld), GFP_KERNEL);
	if (!bld)
		return -ENOMEM;
	
	/* Dual-pipe setup: p0=V0 layer, p1=fill color */
	bld->bld_en_ctrl.bits.p0_en = 1;
	bld->bld_en_ctrl.bits.p1_en = 1;
	bld->bld_en_ctrl.bits.p1_fcen = 1;  /* Pipe 1 uses fill color */
	
	bld->premulti_ctrl.dwval = 0;
	
	/* Configure both pipes (BSP pattern) */
	bld->mem_size[0].bits.width = width - 1;
	bld->mem_size[0].bits.height = height - 1;
	bld->mem_coor[0].bits.xcoor = 0;
	bld->mem_coor[0].bits.ycoor = 0;
	
	bld->mem_size[1].bits.width = width - 1;
	bld->mem_size[1].bits.height = height - 1;
	bld->mem_coor[1].bits.xcoor = 0;
	bld->mem_coor[1].bits.ycoor = 0;
	
	bld->bld_fill_color[0] = 0x00000000;  /* Pipe 0 unused */
	bld->bld_fill_color[1] = fill_color;  /* Pipe 1 fill color */
	
	bld->out_size.bits.width = width - 1;
	bld->out_size.bits.height = height - 1;
	
	bld->out_color.bits.alpha_mode = 0;  /* RGB mode */
	bld->out_color.bits.premul_en = 0;
	
	/* Porter-Duff blend factors */
	bld->bld_ctrl.dwval = porter_duff;
	
	/* ROP registers for fillrect (BSP pattern) */
	bld->rop_ctrl.dwval = 0x000000f0;
	bld->ch3_index0.dwval = 0x00061080;
	
	*out_block = bld;
	*out_size = sizeof(*bld);
	
	return 0;
}

/**
 * g2d_rcq_build_bld - Build BLD block for blending or copy operations
 * @width: Operation width in pixels
 * @height: Operation height in pixels
 * @out_width: Output buffer width
 * @out_height: Output buffer height
 * @fmt_p0: Format for Pipe 0 (V0/Background)
 * @fmt_p1: Format for Pipe 1 (UI2/Foreground/VSU)
 * @out_fmt: Output format (for CSC2 configuration)
 * @p0_en: Enable Pipe 0
 * @p1_en: Enable Pipe 1
 * @p0_x: Pipe 0 X position
 * @p0_y: Pipe 0 Y position
 * @p1_x: Pipe 1 X position
 * @p1_y: Pipe 1 Y position
 * @bld_mode: Blend mode (G2D_BLD_*)
 * @premul_mode: Premultiplication mode (G2D_PREMUL_NONE, G2D_PREMUL_ALPHA)
 * @p1_is_copy_src: If true and mode is COPY, use ROP DSTCOPY (0xAA) to select Pipe 1
 * @out_block: Output pointer to receive allocated block structure
 * @out_size: Output size of block in bytes
 *
 * Returns: 0 on success, negative error on failure
 *
 * Unified builder for Blender configuration. Handles both simple copy/scale
 * (using ROPs) and alpha blending (using Porter-Duff modes).
 * Automatically configures CSC based on input/output formats.
 */
int g2d_rcq_build_bld(u32 p0_w, u32 p0_h, u32 p1_w, u32 p1_h,
		      u32 out_width, u32 out_height, u32 fmt_p0, u32 fmt_p1,
		      u32 out_fmt, u8 cs_p0, u8 cs_p1, u8 cs_out, bool p0_en,
		      bool p1_en, u32 p0_x, u32 p0_y, u32 p1_x, u32 p1_y,
		      u32 bld_mode, u32 premul_mode, bool p1_is_copy_src,
		      struct g2d_csc_state *csc_state, u32 **out_block,
		      u32 *out_size)
{
	struct g2d_mixer_bld_reg *bld;
	bool p0_is_yuv, p1_is_yuv, out_is_yuv;
	enum g2d_rcq_csc_mode csc_p0, csc_p1, csc_out;

	if (!out_block || !out_size)
		return -EINVAL;

	bld = kzalloc(sizeof(*bld), GFP_KERNEL);
	if (!bld)
		return -ENOMEM;

	/* Detect YUV formats (using BSP logic: > BGRA1010102 is YUV) */
	p0_is_yuv = (fmt_p0 > G2D_FORMAT_BGRA1010102);
	p1_is_yuv = (fmt_p1 > G2D_FORMAT_BGRA1010102);
	out_is_yuv = (out_fmt > G2D_FORMAT_BGRA1010102);
	
	csc_p0 = g2d_rcq_select_mode(cs_p0, true);
	csc_p1 = g2d_rcq_select_mode(cs_p1, true);
	csc_out = g2d_rcq_select_mode(cs_out, false);

	/* Configure Pipe Enable and Fetch Control
	 * CRITICAL: fcen=1 means "Fill Color Enable" (ignore layer data).
	 * fcen=0 means "Use Layer Data".
	 * Legacy code sets fcen=0.
	 */
	bld->bld_en_ctrl.bits.p0_en = p0_en ? 1 : 0;
	bld->bld_en_ctrl.bits.p1_en = p1_en ? 1 : 0;
	bld->bld_en_ctrl.bits.p0_fcen = 0; /* 0 = Fetch from Layer */
	bld->bld_en_ctrl.bits.p1_fcen = 0; /* 0 = Fetch from Layer */

	/* Configure pipe sizes (hardware expects size - 1) */
	bld->mem_size[0].bits.width = p0_w - 1;
	bld->mem_size[0].bits.height = p0_h - 1;
	bld->mem_size[1].bits.width = p1_w - 1;
	bld->mem_size[1].bits.height = p1_h - 1;

	/* Configure pipe positions */
	bld->mem_coor[0].bits.xcoor = p0_x;
	bld->mem_coor[0].bits.ycoor = p0_y;
	bld->mem_coor[1].bits.xcoor = p1_x;
	bld->mem_coor[1].bits.ycoor = p1_y;

	/* Output size */
	bld->out_size.bits.width = out_width - 1;
	bld->out_size.bits.height = out_height - 1;

	/* Set output color space (0=RGB, 1=YUV) */
	bld->out_color.bits.alpha_mode = out_is_yuv ? 1 : 0;
	bld->out_color.bits.premul_en = 0;

	/* Configure Premultiplication Control
	 * If Pipe 1 (UI2) is enabled and premul_mode is set, enable p1_alpha_mode.
	 * This tells the blender that the input data already has alpha multiplied.
	 */
	if (p1_en && (premul_mode == G2D_PREMUL_ALPHA))
		bld->premulti_ctrl.bits.p1_alpha_mode = 1;
	else
		bld->premulti_ctrl.bits.p1_alpha_mode = 0;
	
	bld->premulti_ctrl.bits.p0_alpha_mode = 0; /* Assume V0 is not premultiplied */

	/* Configure CSC for input pipes (YUV -> RGB) */
	if (p0_en) {
		if (p0_is_yuv)
			g2d_rcq_bld_csc_set(bld, 0, csc_p0, csc_state);
		else
			bld->cs_ctrl.bits.cs0_en = 0;
	}

	if (p1_en) {
		if (p1_is_yuv)
			g2d_rcq_bld_csc_set(bld, 1, csc_p1, csc_state);
		else
			bld->cs_ctrl.bits.cs1_en = 0;
	}

	/* CSC2 (Output) - Enable for RGB -> YUV */
	if (out_is_yuv) {
		/* If output is YUV, we need to convert from Blender's RGB space to YUV */
		g2d_rcq_bld_csc_set(bld, 2, csc_out, NULL);
	} else {
		bld->cs_ctrl.bits.cs2_en = 0;
	}

	/* Configure Blend Mode and ROP */
	if (bld_mode == G2D_BLD_COPY) {
		/* Simple Copy Mode - Use ROPs */
		bld->bld_ctrl.dwval = 0x00000000; /* Passthrough blending */
		
		if (p1_is_copy_src) {
			/* Select Pipe 1 (Dest in ROP terms) */
			bld->rop_ctrl.dwval = 0x000000aa; /* ROP3 DSTCOPY */
		} else {
			/* Select Pipe 0 (Pattern in ROP terms) */
			bld->rop_ctrl.dwval = 0x000000f0; /* ROP3 PATCOPY */
		}
		bld->ch3_index0.dwval = 0x00061080; /* BSP pattern for copy */
	} else {
		/* Blending Mode - Use Porter-Duff */
		bld->bld_ctrl.dwval = g2d_rcq_get_bld_mode(bld_mode);
		
		/* Reverted: Hardware Premultiplication in UI2 layer should handle this.
		 * We use standard SRCOVER coefficients (0x03010301).
		 */
		
		bld->rop_ctrl.dwval = 0x000000f0; /* Passthrough ROP */
	}

	*out_block = (u32 *)bld;
	*out_size = sizeof(*bld);

	return 0;
}

/**
 * g2d_rcq_build_ui2_memory - Build UI2 block for memory source
 * @width: Layer width in pixels (ovl_mem size, crop region)
 * @height: Layer height in pixels (ovl_mem size, crop region)
 * @pitch: Pitch/stride in bytes (of full buffer)
 * @dma_addr: Source DMA base address (buffer start)
 * @crop_offset: Byte offset from base to crop region start
 * @src_fmt: Source format (G2D_FORMAT_*)
 * @win_x: Window X coordinate (ovl_mem_coor, BSP g2d_uilayer_overlay_set)
 * @win_y: Window Y coordinate (ovl_mem_coor, BSP g2d_uilayer_overlay_set)
 * @win_w: Window width (ovl_winsize)
 * @win_h: Window height (ovl_winsize)
 * @alpha_mode: Alpha mode (G2D_PIXEL_ALPHA, G2D_GLOBAL_ALPHA, etc.)
 * @global_alpha: Global alpha value (0-255)
 * @premul_mode: Premultiplication mode (G2D_PREMUL_NONE, G2D_PREMUL_ALPHA)
 * @out_block: Output pointer to receive allocated block structure
 * @out_size: Output size of block in bytes
 *
 * Returns: 0 on success, negative error on failure
 *
 * UI2 is UI layer 2, used as foreground source in blending operations.
 * Window positioning (win_x, win_y) determines where this layer appears
 * in the final blend output (BSP pattern: g2d_uilayer_overlay_set).
 * 
 * COHERENT WITH V0 PATTERN: Receives base DMA address and separate crop_offset,
 * allowing buffer reuse with different crop regions without address recalculation.
 */
int g2d_rcq_build_ui2_memory(u32 width, u32 height, u32 pitch,
			     dma_addr_t dma_addr, u32 crop_offset, u32 src_fmt,
			     u32 win_x, u32 win_y, u32 win_w, u32 win_h,
			     u32 alpha_mode, u32 global_alpha, u32 premul_mode,
			     struct g2d_mixer_ovl_u_reg **out_block, u32 *out_size)
{
	struct g2d_mixer_ovl_u_reg *ui2;
	dma_addr_t final_dma;
	
	if (!out_block || !out_size)
		return -EINVAL;

	ui2 = kzalloc(sizeof(*ui2), GFP_KERNEL);
	if (!ui2)
		return -ENOMEM;
	
	/* Calculate final DMA address: base + crop_offset
	 * This matches V0 pattern of base + plane_offset[i] */
	final_dma = dma_addr + crop_offset;
	
	/* Configure UI2 attributes */
	ui2->ovl_attr.bits.lay_en = 1;
	ui2->ovl_attr.bits.lay_fillcolor_en = 0; /* Read from memory */
	ui2->ovl_attr.bits.lay_fbfmt = src_fmt;
	ui2->ovl_attr.bits.lay_glbalpha = global_alpha;
	ui2->ovl_attr.bits.alpha_mode = alpha_mode;
	ui2->ovl_attr.bits.lay_premul_ctl = premul_mode;
	
	/* Layer memory region (crop region size) */
	ui2->ovl_mem.bits.lay_width = width - 1;
	ui2->ovl_mem.bits.lay_height = height - 1;
	
	/* Window position in output (BSP g2d_uilayer_overlay_set pattern)
	 * This determines WHERE the UI2 layer appears in the blend output */
	ui2->ovl_mem_coor.bits.lay_xcoor = win_x;
	ui2->ovl_mem_coor.bits.lay_ycoor = win_y;
	
	ui2->ovl_mem_pitch0 = pitch;
	ui2->ovl_mem_low_addr0 = (u32)(final_dma & 0xFFFFFFFF);
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	ui2->ovl_mem_high_addr = (u32)(final_dma >> 32);
#else
	ui2->ovl_mem_high_addr = 0;
#endif
	
	/* Window size (visible region of layer in output) */
	ui2->ovl_winsize.bits.width = win_w - 1;
	ui2->ovl_winsize.bits.height = win_h - 1;
	
	/* Premultiplication control (if supported by hardware/BSP)
	 * Note: BSP doesn't seem to set premul in UI2_ATTR, but in BLD.
	 * However, some G2D versions have it here. We'll leave it for now
	 * as the main control is usually in the blender or scaler.
	 */
	
	*out_block = ui2;
	*out_size = sizeof(*ui2);
	
	return 0;
}

/**
 * g2d_rcq_build_wb - Build WB (writeback) block
 * @width: Width in pixels
 * @height: Height in pixels
 * @pitch: Pitch/stride in bytes
 * @dma_addr: Destination DMA address
 * @dst_fmt: Destination format (G2D_FORMAT_* from sunxi-g2d-structs.h)
 * @out_block: Output pointer to receive allocated block data
 * @out_size: Output size of block in bytes
 * 
 * Returns: 0 on success, negative error on failure
 * 
 * The caller must kfree(*out_block) when done.
 */
int g2d_rcq_build_wb(u32 width, u32 height, u32 pitch,
                     dma_addr_t dma_addr, u32 dst_fmt,
                     u32 **out_block, u32 *out_size)
{
	struct g2d_mixer_write_back_reg wb = {0};
	u32 *regs;
	
	if (!out_block || !out_size)
		return -EINVAL;

	/* WB block: 9 registers (WB_ATT to WB_HADD0) 
	 * Offsets:
	 * 0x00: WB_ATT
	 * 0x04: WB_SIZE
	 * 0x08: WB_PITCH0
	 * 0x0C: WB_PITCH1
	 * 0x10: WB_PITCH2
	 * 0x14: WB_LADD0
	 * 0x18: WB_LADD1
	 * 0x1C: WB_LADD2
	 * 0x20: WB_HADD0
	 */
	regs = kzalloc(9 * sizeof(u32), GFP_KERNEL);
	if (!regs)
		return -ENOMEM;
	
	/* Validate hardware limits (errors only, no hot path logging) */
	if (width == 0 || height == 0 || width > 8192 || height > 8192) {
		kfree(regs);
		return -EINVAL;
	}
	if (pitch < width) {
		kfree(regs);
		return -EINVAL;
	}
	
	/* Configure writeback */
	/* Note: Legacy code sets en=0. If en=1 causes issues, we stick to 0. */
	wb.wb_attr.bits.en = 1;
	wb.wb_attr.bits.fmt = dst_fmt;
	wb.wb_attr.bits.round_en = 0;
	
	wb.data_size.bits.width = width - 1;
	wb.data_size.bits.height = height - 1;
	
	wb.pitch0 = pitch;
	wb.laddr0 = (u32)(dma_addr & 0xFFFFFFFF);
	wb.haddr0 = (u32)((u64)dma_addr >> 32);
	
	pr_debug("WB_BUILDER: dma=0x%llx → laddr0=0x%08x haddr0=0x%08x\n",
		(u64)dma_addr, wb.laddr0, wb.haddr0);
	
	/* Pack into register array */
	regs[0] = wb.wb_attr.dwval;
	regs[1] = wb.data_size.dwval;
	regs[2] = wb.pitch0;
	regs[3] = 0;  /* pitch1 */
	regs[4] = 0;  /* pitch2 */
	regs[5] = wb.laddr0;
	regs[6] = 0;  /* laddr1 */
	regs[7] = 0;  /* laddr2 */
	regs[8] = wb.haddr0;
	
	pr_debug("WB_BUILDER: regs[0-8] = 0x%08x 0x%08x 0x%08x ... 0x%08x\n",
		regs[0], regs[1], regs[2], regs[8]);
	
	*out_block = regs;
	*out_size = 9 * sizeof(u32);
	
	return 0;
}

/**
 * g2d_rcq_build_ui_dummy - Build dummy UI layer block (inactive)
 * @out_block: Output pointer to receive allocated block data
 * @out_size: Output size of block in bytes
 * 
 * Creates a zeroed UI layer block for filling mandatory 7-block structure.
 * 
 * Returns: 0 on success, negative error on failure
 * 
 * The caller must kfree(*out_block) when done.
 */
int g2d_rcq_build_ui_dummy(u32 **out_block, u32 *out_size)
{
	u32 *regs;
	
	if (!out_block || !out_size)
		return -EINVAL;
	
	/* UI block: 8 registers (minimal) */
	regs = kzalloc(8 * sizeof(u32), GFP_KERNEL);
	if (!regs)
		return -ENOMEM;
	
	/* All zeros - inactive layer */
	memset(regs, 0, 8 * sizeof(u32));
	
	*out_block = regs;
	*out_size = 8 * sizeof(u32);
	
	return 0;
}

/**
 * g2d_rcq_build_scaler_passthrough - Build VSU block in 1:1 passthrough mode
 * @width: Input/output width
 * @height: Input/output height
 * @fmt: Source format (for filter_type selection)
 * @out_block: Output pointer to receive allocated block data
 * @out_size: Output size of block in bytes
 * 
 * Creates a VSU scaler block configured for 1:1 passthrough (no scaling).
 * BSP pattern: Even without scaling, YUV formats require VSU to be enabled.
 * Configures VS_CTRL.en=1, filter_type based on format, step=1.0.
 * 
 * Returns: 0 on success, negative error on failure
 * 
 * The caller must kfree(*out_block) when done.
 */
int g2d_rcq_build_scaler_passthrough(u32 width, u32 height, u32 fmt,
				     u32 **out_block, u32 *out_size)
{
	u32 *regs;
	u32 size_reg;
	u32 step_1_0;
	
	if (!out_block || !out_size)
		return -EINVAL;
	
	/* Scaler block: 1152 bytes (BSP measured) */
	regs = kzalloc(1152, GFP_KERNEL);
	if (!regs)
		return -ENOMEM;
	
	/* Initialize to zeros */
	memset(regs, 0, 1152);
	
	/* Configure passthrough mode (1:1 scaling)
	 * Register layout matches struct g2d_mixer_video_scaler_reg from BSP
	 * Offsets: VS_CTRL=0x00, VS_OUT_SIZE=0x40, VS_Y_SIZE=0x80,
	 *          VS_Y_HSTEP=0x88, VS_Y_VSTEP=0x8C, VS_GLB_ALPHA=0x90,
	 *          VS_C_SIZE=0xC0, VS_C_HSTEP=0xC8, VS_C_VSTEP=0xCC */
	
	size_reg = ((height - 1) << 16) | (width - 1);
	step_1_0 = 0x00100000;  /* 1.0 in fixed-point (0x80000 << 1) */
	
	/* VS_CTRL @0x00: en=1, coef_access=1, filter_type
	 * Must be in COEF_ACCESS mode to load coefficients.
	 * EN=1 because BSP enables it during load!
	 * 
	 * FIX: Must set FILTER_TYPE correctly during load phase too!
	 * BSP Logic: if (fmt > G2D_FORMAT_IYUV422_Y1U0Y0V0) filter_type=1; else filter_type=0;
	 * 
	 * UPDATE: We set EN=0 here to avoid starting the scaler while loading coefficients.
	 * The separate SCAL_EN block will set EN=1 later.
	 */
	if (fmt > G2D_FORMAT_IYUV422_Y1U0Y0V0) {
		regs[0x00 / 4] = 0x00010100;  /* en=0, coef_access=1, filter_type=1 */
	} else {
		regs[0x00 / 4] = 0x00000100;  /* en=0, coef_access=1, filter_type=0 */
	}
	
	/* VS_OUT_SIZE @0x40: output dimensions (H-1)<<16 | (W-1) */
	regs[0x40 / 4] = size_reg;
	
	/* VS_Y_SIZE @0x80: input Y channel dimensions */
	regs[0x80 / 4] = size_reg;
	
	/* VS_Y_HSTEP @0x88, VS_Y_VSTEP @0x8C: Y channel step (1.0) */
	regs[0x88 / 4] = step_1_0;
	regs[0x8C / 4] = step_1_0;
	
	/* VS_GLB_ALPHA @0x44: global alpha value (BSP always sets to 0xFF) */
	regs[0x44 / 4] = 0x000000FF;
	
	/* VS_C_SIZE @0xC0: input chroma dimensions */
	regs[0xC0 / 4] = size_reg;
	
	/* VS_C_HSTEP @0xC8, VS_C_VSTEP @0xCC: chroma step (1.0) */
	regs[0xC8 / 4] = step_1_0;
	regs[0xCC / 4] = step_1_0;
	
	/* Load horizontal FIR coefficients (Lanczos2, phase 0 only for 1:1)
	 * BSP always loads coefficients even for passthrough
	 * VS_Y_HCOEF0..31 @0x200-0x27C, VS_C_HCOEF0..31 @0x300-0x37C */
	{
		/* For 1:1 scaling (step=0x00080000), use center tap coefficient
		 * BSP lan2coefftab32_full[0] = 0x00004000 (1.0 in fixed-point)
		 * Simple passthrough: all weight on center tap */
		u32 center_coef = 0x00004000;  /* 1.0 coefficient */
		int i;
		
		/* Y channel horizontal coefficients @0x200 */
		regs[(VS_Y_HCOEF0 - G2D_VSU) / 4] = center_coef;  /* Phase 0, center tap */
		for (i = 1; i < 32; i++)
			regs[(VS_Y_HCOEF0 - G2D_VSU) / 4 + i] = 0;
		
		/* Y channel vertical coefficients @0x300 */
		regs[(VS_Y_VCOEF0 - G2D_VSU) / 4] = center_coef;
		for (i = 1; i < 32; i++)
			regs[(VS_Y_VCOEF0 - G2D_VSU) / 4 + i] = 0;
		
		/* Chroma horizontal coefficients @0x400 */
		regs[(VS_C_HCOEF0 - G2D_VSU) / 4] = center_coef;
		for (i = 1; i < 32; i++)
			regs[(VS_C_HCOEF0 - G2D_VSU) / 4 + i] = 0;

		/* Chroma vertical coefficients @0x500 */
		regs[(VS_C_VCOEF0 - G2D_VSU) / 4] = center_coef;
		for (i = 1; i < 32; i++)
			regs[(VS_C_VCOEF0 - G2D_VSU) / 4 + i] = 0;
	}
	
	*out_block = regs;
	*out_size = 1152;

	pr_debug("SCAL_BUILDER: returning regs=%p size=%u\n", regs, *out_size);

	return 0;
}

/**
 * g2d_rcq_build_scaler_dummy - Build dummy scaler block (inactive)
 * @out_block: Output pointer to receive allocated block data
 * @out_size: Output size of block in bytes
 * 
 * Creates a zeroed scaler block for filling mandatory 7-block structure.
 * 
 * Returns: 0 on success, negative error on failure
 * 
 * The caller must kfree(*out_block) when done.
 */
int g2d_rcq_build_scaler_dummy(u32 **out_block, u32 *out_size)
{
	u32 *regs;
	
	if (!out_block || !out_size)
		return -EINVAL;
	
	/* Scaler block: 1152 bytes (BSP measured) */
	regs = kzalloc(1152, GFP_KERNEL);
	if (!regs)
		return -ENOMEM;
	
	/* All zeros - inactive scaler */
	memset(regs, 0, 1152);
	
	*out_block = regs;
	*out_size = 1152;
	
	return 0;
}

/**
 * g2d_vsu_calc_fir_coef - Calculate FIR coefficient offset
 * @step: Scale step value (fixed-point)
 *
 * Returns the offset (in words) into the coefficient table based on the
 * scaling ratio. Used to select appropriate filter coefficients for the
 * current scale factor.
 * 
 * This is the RCQ builder version of the legacy function from sunxi-g2d-main.c
 */
static u32 g2d_vsu_calc_fir_coef(u32 step)
{
	u32 pt_coef;
	u32 scale_ratio, int_part, float_part, fir_coef_ofst;

	scale_ratio = step >> (VSU_PHASE_FRAC_BITWIDTH - 3);
	int_part = scale_ratio >> 3;
	float_part = scale_ratio & 0x7;

	fir_coef_ofst =
		(int_part == 0) ?
			VSU_ZOOM0_SIZE :
		(int_part == 1) ?
			VSU_ZOOM0_SIZE + float_part :
		(int_part == 2) ?
			VSU_ZOOM0_SIZE + VSU_ZOOM1_SIZE + (float_part >> 1) :
		(int_part == 3) ?
			VSU_ZOOM0_SIZE + VSU_ZOOM1_SIZE + VSU_ZOOM2_SIZE :
		(int_part == 4) ?
			VSU_ZOOM0_SIZE + VSU_ZOOM1_SIZE + VSU_ZOOM2_SIZE +
				VSU_ZOOM3_SIZE :
			VSU_ZOOM0_SIZE + VSU_ZOOM1_SIZE + VSU_ZOOM2_SIZE +
				VSU_ZOOM3_SIZE + VSU_ZOOM4_SIZE;

	pt_coef = fir_coef_ofst * VSU_PHASE_NUM;
	return pt_coef;
}

/* VSU scaling filter coefficients now in sunxi-g2d-scaler-coeffs.h */

/**
 * g2d_rcq_build_scaler_active - Build active scaler block with VSU configuration
 * @in_w: Input width in pixels
 * @in_h: Input height in pixels
 * @out_w: Output width in pixels
 * @out_h: Output height in pixels
 * @fmt: Format (G2D_FMT_* from API) to determine RGB vs YUV filtering
 * @alpha: Global alpha value (0-255) for scaled output
 * @out_block: Output pointer to receive allocated block data
 * @out_size: Output size of block in bytes
 * 
 * Creates a fully configured VSU block for active scaling from in_w×in_h to out_w×out_h.
 * Uses Lanczos filtering for both RGB and YUV with appropriate coefficient tables.
 * The global alpha replaces per-pixel alpha (VSU hardware limitation).
 * 
 * Returns: 0 on success, negative error on failure
 * 
 * The caller must kfree(*out_block) when done.
 */
int g2d_rcq_build_scaler_active(u32 in_w, u32 in_h, u32 out_w, u32 out_h,
u32 fmt, u8 alpha, u32 **out_block, u32 *out_size)
{
	u32 *regs;
	u64 temp;
	u32 yhstep, yvstep;
	u32 yhcoef_offset, yvcoef_offset;
	u32 format;
	int i;
	
	 pr_debug("SCAL_BUILDER: g2d_rcq_build_scaler_active CALLED: in=%ux%u out=%ux%u fmt=0x%02x alpha=%u\n",
		 in_w, in_h, out_w, out_h, fmt, alpha);
	
	if (!out_block || !out_size)
		return -EINVAL;
	
	if (out_w == 0 || out_h == 0) {
		pr_err("g2d_rcq: scaler_active: invalid output size %ux%u\n",
	   out_w, out_h);
		return -EINVAL;
	}
	
	/* Determine format type (RGB vs YUV) for filter selection */
	if (fmt >= 0x10) /* YUV formats start at 0x10 */
		format = VSU_FORMAT_YUV422;
	else
		format = VSU_FORMAT_RGB;
	
	/* Allocate scaler block: 1152 bytes (BSP measured) */
	regs = kzalloc(1152, GFP_KERNEL);
	if (!regs)
		return -ENOMEM;
	
	/* Calculate horizontal and vertical scaling steps */
	temp = (u64)in_w << VSU_PHASE_FRAC_BITWIDTH;
	do_div(temp, out_w);
	yhstep = (u32)temp;
	
	temp = (u64)in_h << VSU_PHASE_FRAC_BITWIDTH;
	do_div(temp, out_h);
	yvstep = (u32)temp;
	
	/* Calculate coefficient offsets for adaptive filtering */
	yhcoef_offset = g2d_vsu_calc_fir_coef(yhstep);
	yvcoef_offset = g2d_vsu_calc_fir_coef(yvstep);
	
	/* Configure VSU registers (offsets relative to G2D_VSU base) */
	
	/* VS_CTRL: Configure scaler for coefficient access (DISABLE ENABLE BIT)
	 * BIT(0) = EN (0 = disable) - Don't enable while loading coeffs!
	 * BIT(8) = COEF_ACCESS_SEL (1 = enable coefficient loading)
	 * BIT(16) = FILTER_TYPE (1 = High YUV, 0 = RGB/Low YUV)
	 * 
	 * FIX: Set EN=0 here. The separate SCAL_EN block will set EN=1 and COEF_ACCESS=0 later.
	 */
	if (format == VSU_FORMAT_RGB) {
		regs[(VS_CTRL - G2D_VSU) / 4] = 0x00100; /* RGB: EN=0 | COEF_ACCESS | FILTER_TYPE=0 */
		pr_debug("SCAL_BUILDER: VS_CTRL=0x00100 (RGB mode, loading coeffs)\n");
	} else {
		/* Logic for YUV: if > 0x23, use 1. Else use 0. */
		if (fmt > G2D_FORMAT_IYUV422_Y1U0Y0V0) {
			regs[(VS_CTRL - G2D_VSU) / 4] = 0x10100; /* High YUV: EN=0 | COEF_ACCESS | FILTER_TYPE=1 */
			pr_debug("SCAL_BUILDER: VS_CTRL=0x10100 (High YUV mode, loading coeffs)\n");
		} else {
			regs[(VS_CTRL - G2D_VSU) / 4] = 0x00100; /* Low YUV: EN=0 | COEF_ACCESS | FILTER_TYPE=0 */
			pr_debug("SCAL_BUILDER: VS_CTRL=0x00100 (Low YUV mode, loading coeffs)\n");
		}
	}
	
	/* VS_OUT_SIZE: Output dimensions */
	regs[(VS_OUT_SIZE - G2D_VSU) / 4] = ((out_w - 1) & 0x1FFF) | (((out_h - 1) & 0x1FFF) << 16);
	 pr_debug("SCAL_BUILDER: VS_OUT_SIZE=0x%08x (w=%u h=%u)\n",
		 regs[(VS_OUT_SIZE - G2D_VSU) / 4], out_w, out_h);
	
	/* VS_GLB_ALPHA: Global alpha
	 * Set to the provided alpha value (usually 0xFF for opaque/full alpha).
	 */
	regs[(VS_GLB_ALPHA - G2D_VSU) / 4] = alpha;
	
	/* VS_Y_SIZE: Input dimensions for luma/RGB channel */
	regs[(VS_Y_SIZE - G2D_VSU) / 4] = ((in_w - 1) & 0x1FFF) | (((in_h - 1) & 0x1FFF) << 16);
	 pr_debug("SCAL_BUILDER: VS_Y_SIZE=0x%08x (w=%u h=%u)\n",
		 regs[(VS_Y_SIZE - G2D_VSU) / 4], in_w, in_h);
	
	/* VS_Y_HSTEP / VS_Y_VSTEP: Scaling factors (shifted by 1 for hardware) */
	regs[(VS_Y_HSTEP - G2D_VSU) / 4] = yhstep << 1;
	regs[(VS_Y_VSTEP - G2D_VSU) / 4] = yvstep << 1;
	 pr_debug("SCAL_BUILDER: VS_Y_HSTEP=0x%08x VS_Y_VSTEP=0x%08x (scaling %ux%u→%ux%u)\n",
		 regs[(VS_Y_HSTEP - G2D_VSU) / 4], regs[(VS_Y_VSTEP - G2D_VSU) / 4],
		 in_w, in_h, out_w, out_h);
	
	/* Load horizontal Y coefficients (Lanczos) */
	for (i = 0; i < VSU_PHASE_NUM; i++) {
		regs[(VS_Y_HCOEF0 - G2D_VSU) / 4 + i] = lan2coefftab32_full[yhcoef_offset + i];
	}
	
	/* Load vertical Y coefficients */
	if (format == VSU_FORMAT_RGB) {
		/* RGB: Use linear interpolation for vertical */
		for (i = 0; i < VSU_PHASE_NUM; i++) {
			regs[(VS_Y_VCOEF0 - G2D_VSU) / 4 + i] = linearcoefftab32[i];
		}
	} else {
		/* YUV: Use Lanczos for vertical */
		for (i = 0; i < VSU_PHASE_NUM; i++) {
			regs[(VS_Y_VCOEF0 - G2D_VSU) / 4 + i] = lan2coefftab32_full[yvcoef_offset + i];
		}
	}
	
	/* Configure phase registers (CRITICAL for hardware operation)
	 * BSP pattern: phase=0 for RGB/most YUV, special values for YUV420 */
	regs[(VS_Y_HPHASE - G2D_VSU) / 4] = 0;
	regs[(VS_Y_VPHASE0 - G2D_VSU) / 4] = 0;
	regs[(VS_C_HPHASE - G2D_VSU) / 4] = 0;
	regs[(VS_C_VPHASE0 - G2D_VSU) / 4] = 0;
	
	/* Configure chroma (C) channel - BSP does this for ALL formats including RGB */
	{
		u32 c_hstep, c_vstep;
		u32 c_in_w, c_in_h;
		u32 chcoef_offset, cvcoef_offset;
		u32 c_size_val;
		int c_size_offset;
		
		/* For RGB: chroma same as luma (no subsampling)
		 * For YUV420: chroma is half resolution
		 * For YUV422: chroma is half width */
		if (fmt >= 0x28 && fmt <= 0x2A) {
			/* YUV420: 1/2 width, 1/2 height */
			c_in_w = in_w / 2;
			c_in_h = in_h / 2;
		} else if (fmt >= 0x20 && fmt <= 0x27) {
			/* YUV422: 1/2 width, full height */
			c_in_w = in_w / 2;
			c_in_h = in_h;
		} else if (fmt >= 0x2C && fmt <= 0x2E) {
			/* YUV411: 1/4 width, full height */
			c_in_w = in_w / 4;
			c_in_h = in_h;
		} else {
			/* RGB or others: full resolution */
			c_in_w = in_w;
			c_in_h = in_h;
		}
		
		/* Calculate chroma steps */
		temp = (u64)c_in_w << VSU_PHASE_FRAC_BITWIDTH;
		do_div(temp, out_w);
		c_hstep = (u32)temp;
		
		temp = (u64)c_in_h << VSU_PHASE_FRAC_BITWIDTH;
		do_div(temp, out_h);
		c_vstep = (u32)temp;
		
		chcoef_offset = g2d_vsu_calc_fir_coef(c_hstep);
		cvcoef_offset = g2d_vsu_calc_fir_coef(c_vstep);
		
		/* VS_C_SIZE: Chroma input size */
		c_size_offset = (VS_C_SIZE - G2D_VSU) / 4;
		c_size_val = ((c_in_w - 1) & 0x1FFF) | (((c_in_h - 1) & 0x1FFF) << 16);
		 pr_debug("SCAL_BUILDER: VS_C_SIZE offset=%d (0x%x) value=0x%08x (w=%u h=%u)\n",
			 c_size_offset, c_size_offset * 4, c_size_val, c_in_w, c_in_h);
		regs[c_size_offset] = c_size_val;
		
		/* VS_C_HSTEP / VS_C_VSTEP */
		regs[(VS_C_HSTEP - G2D_VSU) / 4] = c_hstep << 1;
		regs[(VS_C_VSTEP - G2D_VSU) / 4] = c_vstep << 1;
		
		/* Load chroma coefficients */
		if (format == VSU_FORMAT_RGB) {
			/* RGB: Use Lanczos horizontal + linear vertical (BSP pattern) */
			for (i = 0; i < VSU_PHASE_NUM; i++) {
				regs[(VS_C_HCOEF0 - G2D_VSU) / 4 + i] = lan2coefftab32_full[chcoef_offset + i];
				/* Enable C_VCOEF0 for RGB too, just in case */
				regs[(VS_C_VCOEF0 - G2D_VSU) / 4 + i] = linearcoefftab32[i];
			}
		} else {
			/* YUV: Use Lanczos for both */
			for (i = 0; i < VSU_PHASE_NUM; i++) {
				regs[(VS_C_HCOEF0 - G2D_VSU) / 4 + i] = lan2coefftab32_full[chcoef_offset + i];
				/* Enable C_VCOEF0 for YUV - CRITICAL for YUV420 upsampling! */
				regs[(VS_C_VCOEF0 - G2D_VSU) / 4 + i] = lan2coefftab32_full[cvcoef_offset + i];
			}
		}
	}
	
	*out_block = regs;
	*out_size = 1152;
	
	return 0;
}

int g2d_rcq_build_scaler_enable(u32 fmt, u32 **out_block, u32 *out_size)
{
	u32 *regs;
	u32 size = 16; /* 4 registers */

	regs = kzalloc(size, GFP_KERNEL);
	if (!regs)
		return -ENOMEM;

	/* 0x00: SCAL_CTL - Enable scaler */
	regs[0] = 0x00000001; /* Enable */

	/* 0x04: SCAL_OUT_CTL - Output format */
	/* Note: This seems to be redundant if SCAL_OUT_FMT is set, but let's keep it safe */
	regs[1] = 0; 

	/* 0x08: SCAL_OUT_FMT */
	regs[2] = fmt;

	/* 0x0C: SCAL_OUT_SIZE - Not needed here, set in active builder */
	regs[3] = 0;

	*out_block = regs;
	*out_size = size;

	return 0;
}

int g2d_rcq_build_rot(u32 src_w, u32 src_h, u32 src_pitch,
		      dma_addr_t src_addr, u32 src_fmt,
		      u32 dst_w, u32 dst_h, u32 dst_pitch,
		      dma_addr_t dst_addr, u32 dst_fmt,
		      u32 rot_mode,
		      struct g2d_rot_reg **out_block, u32 *out_size)
{
	struct g2d_rot_reg *rot;
	u32 size = sizeof(struct g2d_rot_reg);

	rot = kzalloc(size, GFP_KERNEL);
	if (!rot)
		return -ENOMEM;

	/* Input Size (value = pixels - 1) */
	rot->insize.bits.width = src_w - 1;
	rot->insize.bits.height = src_h - 1;

	/* Input Format */
	rot->infmt.bits.fmt = src_fmt;

	/* Input Pitch */
	rot->pitch0 = src_pitch;
	/* For planar formats, pitch1/2 would be needed, but let's assume packed/semi-planar for now or calculate */
	/* Assuming standard semi-planar (NV12/NV21) or packed */
	if (src_fmt == G2D_FORMAT_YUV420UVC_V1U1V0U0 || src_fmt == G2D_FORMAT_YUV420UVC_U1V1U0V0) {
		rot->pitch1 = src_pitch; /* UV plane usually same pitch */
	}

	/* Input Address */
	rot->laddr0 = lower_32_bits(src_addr);
	rot->haddr0 = upper_32_bits(src_addr);
	/* Calculate UV offset if needed. For now assume contiguous */
	/* This is a simplification. Real driver might need plane offsets. */
	/* But the caller passes a single dma_addr. */
	
	/* Output Size (value = pixels - 1) */
	rot->outsize.bits.width = dst_w - 1;
	rot->outsize.bits.height = dst_h - 1;

	/* Output Pitch */
	rot->out_pitch0 = dst_pitch;
	if (dst_fmt == G2D_FORMAT_YUV420UVC_V1U1V0U0 || dst_fmt == G2D_FORMAT_YUV420UVC_U1V1U0V0) {
		rot->out_pitch1 = dst_pitch;
	}

	/* Output Address */
	rot->out_laddr0 = lower_32_bits(dst_addr);
	rot->out_haddr0 = upper_32_bits(dst_addr);

	/* Rotation Control */
	if (rot_mode & G2D_ROT_90)
		rot->rot_ctrl.bits.degree = 1;
	else if (rot_mode & G2D_ROT_180)
		rot->rot_ctrl.bits.degree = 2;
	else if (rot_mode & G2D_ROT_270)
		rot->rot_ctrl.bits.degree = 3;
	else
		rot->rot_ctrl.bits.degree = 0;

	if (rot_mode & G2D_ROT_H)
		rot->rot_ctrl.bits.hflip_en = 1;
	
	if (rot_mode & G2D_ROT_V)
		rot->rot_ctrl.bits.vflip_en = 1;

	/* Interrupt Enable */
	rot->rot_int.bits.finish_irq = 1; /* Enable interrupt */

	/* Timeout */
	rot->time_ctrl.dwval = 0xFFFFFFFF; /* Max timeout */

	/* Start bit - MUST be set last in the block if possible, but RCQ writes sequentially.
	 * However, since RCQ writes the whole block, and rot_ctrl is at offset 0x00,
	 * it gets written FIRST. This is problematic if the hardware latches the start
	 * bit immediately and ignores subsequent writes to INT/SIZE/ADDR registers
	 * for the current operation.
	 *
	 * BUT, the BSP driver writes INT then CTL.
	 *
	 * If we use RCQ, we are stuck with the struct layout order.
	 * UNLESS the hardware buffers the config until some trigger?
	 * No, G2D usually triggers on register write.
	 *
	 * WORKAROUND: Do NOT set start bit in the main block.
	 * Instead, add a SECOND small block just for ROT_CTL with start bit set.
	 * This ensures all other registers (INT, SIZE, ADDR) are written first.
	 */
	rot->rot_ctrl.bits.start = 0;

	*out_block = rot;
	*out_size = size;

	return 0;
}
