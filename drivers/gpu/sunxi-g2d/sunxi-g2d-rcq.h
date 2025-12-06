/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sunxi-g2d-rcq.h - Register Command Queue (RCQ) for Allwinner G2D
 * 
 * Copyright (C) 2025 Sergio Perez
 * 
 * RCQ allows building a command list in memory that the G2D hardware
 * will execute automatically via DMA, eliminating the need for CPU
 * to write each register individually.
 */

#ifndef __SUNXI_G2D_RCQ_H__
#define __SUNXI_G2D_RCQ_H__

#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <uapi/linux/sunxi_g2d.h>

/* Forward declarations for builder functions */
struct g2d_mixer_bld_reg;
struct g2d_mixer_ovl_u_reg;

/* CSC State for Brightness/Contrast/Saturation */
struct g2d_csc_state {
	/* Base tables (static/default) */
	const s32 *base_601;
	const s32 *base_709;
	const s32 *base_2020;
	
	/* Current tables (possibly modified) */
	s32 current_601[48];
	s32 current_709[48];
	s32 current_2020[48];
	
	/* Current adjustment values */
	struct g2d_csc_adjust adj;
	
	/* Dirty flags */
	bool dirty_601;
	bool dirty_709;
	bool dirty_2020;
};

/* RCQ alignment requirements from hardware */
#define G2D_RCQ_BYTE_ALIGN(x)   (((x) + 31) & ~31)  /* 32-byte align */
#define G2D_RCQ_HEADER_ALIGN(x) (((x) + 1) & ~1)    /* 2-byte align */

/* Maximum RCQ command buffer size - one page should be enough for most cases */
#define G2D_RCQ_MAX_SIZE        (4 * 1024)

/* Forward declarations for builder functions */
struct g2d_rot_reg;

/* Special Blend Mode for Copy Operations (uses ROPs instead of Porter-Duff) */
#define G2D_BLD_COPY            0xFF

/**
 * struct g2d_rcq_header - RCQ command header
 * @low_addr: Low 32-bits of physical address (must be 32-byte aligned)
 * @dw0: Contains length (24-bit) and high address (8-bit)
 * @dirty: Dirty flag and next header length
 * @reg_offset: Register offset from G2D base address
 * 
 * Each header describes one block of register writes.
 * The hardware will DMA the data from @low_addr and write
 * to registers starting at @reg_offset.
 */
struct g2d_rcq_header {
	u32 low_addr;
	union {
		u32 dwval;
		struct {
			u32 len:24;       /* Data length in bytes */
			u32 high_addr:8;  /* High 8 bits of address */
		} bits;
	} dw0;
	union {
		u32 dwval;
		struct {
			u32 dirty:1;           /* 1=update this block */
			u32 res0:15;
			u32 n_header_len:16;   /* Next frame header length */
		} bits;
	} dirty;
	u32 reg_offset;   /* Offset from G2D base (e.g. 0x0100 for MIXER) */
};

struct g2d_rcq_mem {
	void *vir_addr;
	dma_addr_t phy_addr;
	u32 size;
	u32 used;
	u32 header_count;
	u32 header_len_bytes;
};

/**
 * struct sunxi_g2d_reg_block - Logical RCQ register block descriptor
 * @phy_addr: 32-byte aligned physical address of register payload in RCQ buffer
 * @vir_addr: CPU-mapped virtual address of payload in RCQ buffer
 * @size:     Size in bytes of valid payload (before 32-byte alignment)
 * @reg_offset: Register offset from legacy G2D base (e.g. WB_ATT, V0_ATTCTL)
 * @dirty:    Non-zero if this block should be updated by RCQ
 * @header:   Pointer to the RCQ header associated with this block
 *
 * This descriptor mirrors the BSP g2d_reg_block concept but is restricted
 * to the single, pre-allocated RCQ buffer managed by struct g2d_rcq_mem.
 * It is used to build BSP-like RCQ layouts for prototyping.
 */
struct sunxi_g2d_reg_block {
	dma_addr_t phy_addr;
	void *vir_addr;
	u32 size;
	u32 reg_offset;
	u32 dirty;
	struct g2d_rcq_header *header;
};

/**
 * struct sunxi_g2d_rcq_frame_layout - Single-frame RCQ layout description
 * @headers:          Base pointer to RCQ headers within rcq->vir_addr
 * @header_count:     Number of valid headers for this frame
 * @header_len_bytes: Total length in bytes of header region for this frame
 * @blocks:           Fixed array of logical register blocks (V0, U0-2, SCAL, BLD, WB)
 * @block_count:      Number of valid entries in @blocks (BSP uses 7 blocks)
 *
 * This structure describes the logical layout of a single RCQ frame using
 * BSP-like register blocks. BSP always generates 7 blocks in fixed order:
 * V0, U0, U1, U2, SCAL, BLD, WB (inactive blocks have dirty=0).
 */
struct sunxi_g2d_rcq_frame_layout {
	struct g2d_rcq_header *headers;
	u32 header_count;
	u32 header_len_bytes;
	struct sunxi_g2d_reg_block blocks[8];  /* Max 7 used, 8 for alignment */
	u32 block_count;
};

/* Function prototypes */
int sunxi_g2d_rcq_alloc(struct device *dev, struct g2d_rcq_mem *rcq, u32 size);
void sunxi_g2d_rcq_free(struct device *dev, struct g2d_rcq_mem *rcq);
void sunxi_g2d_rcq_reset(struct g2d_rcq_mem *rcq);
int sunxi_g2d_rcq_add_block(struct g2d_rcq_mem *rcq, u32 reg_offset, 
                             const void *data, u32 size);
void sunxi_g2d_rcq_setup_hw(void __iomem *base, struct g2d_rcq_mem *rcq);
void sunxi_g2d_rcq_start(void __iomem *base, bool use_en_bit, bool enable_irq);

int sunxi_g2d_rcq_build_frame_layout_fillrect(struct sunxi_g2d_rcq_frame_layout *layout,
					void __iomem *base);
int sunxi_g2d_rcq_pack_frame(struct g2d_rcq_mem *rcq,
			   struct sunxi_g2d_rcq_frame_layout *layout,
			   const void *wb_regs,
			   const void *v0_regs,
			   const void *bld_regs,
			   const void *mixer_regs);
int sunxi_g2d_rcq_pack_frame_7blocks(struct g2d_rcq_mem *rcq,
				     struct sunxi_g2d_rcq_frame_layout *layout,
				     const void *v0_regs,
				     const void *u0_regs,
				     const void *u1_regs,
				     const void *u2_regs,
				     const void *scal_regs,
				     const void *bld_regs,
				     const void *wb_regs);
int sunxi_g2d_rcq_pack_frame_8blocks(struct g2d_rcq_mem *rcq,
				     struct sunxi_g2d_rcq_frame_layout *layout,
				     const void *v0_regs,
				     const void *u0_regs,
				     const void *u1_regs,
				     const void *u2_regs,
				     const void *scal_regs,
				     const void *scal_en_regs,
				     const void *bld_regs,
				     const void *wb_regs);
int sunxi_g2d_rcq_pack_frame_3blocks(struct g2d_rcq_mem *rcq,
				     struct sunxi_g2d_rcq_frame_layout *layout,
				     const void *v0_regs,
				     const void *bld_regs,
				     const void *wb_regs);
int sunxi_g2d_rcq_pack_frame_4blocks(struct g2d_rcq_mem *rcq,
				     struct sunxi_g2d_rcq_frame_layout *layout,
				     const void *v0_regs,
				     const void *bld_regs,
				     const void *scal_regs,
				     const void *wb_regs);
int sunxi_g2d_rcq_pack_frame_5blocks(struct g2d_rcq_mem *rcq,
				     struct sunxi_g2d_rcq_frame_layout *layout,
				     const void *ui2_regs,
				     const void *v0_regs,
				     const void *bld_regs,
				     const void *scal_regs,
				     const void *wb_regs);

/* RCQ block builder functions - modular helpers */
int g2d_rcq_build_v0_fillcolor(u32 width, u32 height, u32 pitch,
                                u32 color, u32 color_fmt,
                                u32 **out_block, u32 *out_size);
int g2d_rcq_build_v0_memory(u32 width, u32 height, const u32 stride[3],
                             dma_addr_t dma_addr, const u32 plane_offset[3],
                             u32 src_fmt,
                             u32 win_x, u32 win_y, u32 win_w, u32 win_h,
                             u32 **out_block, u32 *out_size);
int g2d_rcq_build_bld_fillcolor(u32 width, u32 height, u32 fill_color,
                                 u32 porter_duff, bool premul,
                                 struct g2d_mixer_bld_reg **out_block,
                                 u32 *out_size);
int g2d_rcq_build_bld(u32 p0_w, u32 p0_h, u32 p1_w, u32 p1_h,
		      u32 out_width, u32 out_height, u32 fmt_p0, u32 fmt_p1,
		      u32 out_fmt, u8 cs_p0, u8 cs_p1, u8 cs_out, bool p0_en,
		      bool p1_en, u32 p0_x, u32 p0_y, u32 p1_x, u32 p1_y,
		      u32 bld_mode, u32 premul_mode, bool p1_is_copy_src,
		      bool ck_enable, bool ck_on_ui2, u32 ck_min, u32 ck_max,
		      struct g2d_csc_state *csc_state, u32 **out_block,
		      u32 *out_size);
int g2d_rcq_build_ui2_memory(u32 width, u32 height, u32 pitch,
			     dma_addr_t dma_addr, u32 crop_offset, u32 src_fmt,
			     u32 win_x, u32 win_y, u32 win_w, u32 win_h,
			     u32 alpha_mode, u32 global_alpha, u32 premul_mode,
			     struct g2d_mixer_ovl_u_reg **out_block, u32 *out_size);
int g2d_rcq_build_wb(u32 width, u32 height, u32 pitch,
                     dma_addr_t dma_addr, u32 dst_fmt,
                     u32 **out_block, u32 *out_size);
int g2d_rcq_build_ui_dummy(u32 **out_block, u32 *out_size);
int g2d_rcq_build_scaler_passthrough(u32 width, u32 height, u32 fmt,
				     u32 **out_block, u32 *out_size);
int g2d_rcq_build_scaler_dummy(u32 **out_block, u32 *out_size);
int g2d_rcq_build_scaler_active(u32 in_w, u32 in_h, u32 out_w, u32 out_h,
				u32 fmt, u8 alpha, u32 **out_block, u32 *out_size);
int g2d_rcq_build_scaler_enable(u32 fmt, u32 **out_block, u32 *out_size);
int g2d_rcq_build_rot(u32 src_w, u32 src_h, u32 src_pitch,
		      dma_addr_t src_addr, u32 src_fmt,
		      u32 dst_w, u32 dst_h, u32 dst_pitch,
		      dma_addr_t dst_addr, u32 dst_fmt,
		      u32 rot_mode,
		      struct g2d_rot_reg **out_block, u32 *out_size);

/* CSC Helper Functions */
void g2d_csc_init(struct g2d_csc_state *state);
void g2d_csc_update(struct g2d_csc_state *state);

#endif /* __SUNXI_G2D_RCQ_H__ */
