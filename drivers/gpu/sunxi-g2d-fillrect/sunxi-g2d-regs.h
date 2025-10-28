/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal register definitions for Allwinner G2D (RCQ-capable variants).
 * Extracted from the downstream BSP and pared down to the subset required
 * by the fillrect self-test driver.
 */

#ifndef __SUNXI_G2D_REGS_MINI_H__
#define __SUNXI_G2D_REGS_MINI_H__

/* Module base blocks (legacy CPU view) */
#define G2D_TOP        0x00000
#define G2D_MIXER      0x00100
#define G2D_BLD        0x00400
#define G2D_V0         0x00800
#define G2D_WB         0x03000

/* T113/D1 RCQ layout - sub-blocks shift by +0x28000 */
#define G2D_RCQ_OFFSET  0x28000
/* G2D MIXER registers - T113 uses HYBRID addressing:
 * - Control/IRQ registers are in LEGACY locations (no +0x28000)
 * - Data/config registers are in RCQ locations (+0x28000)
 */
#define G2D_MIXER_T113                  0x28100   /* RCQ location for data regs */
#define G2D_MIXER_CTL_T113              (G2D_MIXER_T113 + 0x0)  /* 0x28100 RCQ */
#define G2D_MIXER_INT_T113              0x00104   /* LEGACY location! BSP confirmed */
#define G2D_MIXER_CLK_T113              (G2D_MIXER_T113 + 0x8)  /* 0x28108 RCQ */
#define G2D_BLD_T113    0x00400   /* BSP confirmed address */
#define G2D_V0_T113     0x00800   /* BSP confirmed address */
#define G2D_WB_T113     0x03000   /* BSP legacy address - testing if writable */

/* RCQ control registers live in TOP space */
#define G2D_RCQ_IRQ_CTL   (0x20 + G2D_TOP)
#define  G2D_RCQ_IRQ_SEL              BIT(0)  /* 0=RCQ mode, 1=Direct mode */
#define  G2D_RCQ_IRQ_TASK_END_EN      BIT(4)
#define  G2D_RCQ_IRQ_CFG_FINISH_EN    BIT(6)

#define G2D_RCQ_STATUS    (0x24 + G2D_TOP)
#define  G2D_RCQ_STATUS_TASK_END      BIT(0)
#define  G2D_RCQ_STATUS_CFG_FINISH    BIT(2)
#define  G2D_RCQ_STATUS_FRAME_CNT_MASK GENMASK(15, 8)

#define G2D_RCQ_CTRL      (0x28 + G2D_TOP)
#define  G2D_RCQ_CTRL_UPDATE          BIT(0)  /* BSP: g2d_top_rcq_update_en() - trigger RCQ fetch */
#define  G2D_RCQ_CTRL_EN              BIT(4)  /* T113 v2 RCQ: enable RCQ execution (needed!) */
/* NOTE: T113 RCQ v2 requires BOTH EN (bit4) and UPDATE (bit0) to execute pipeline */

#define G2D_RCQ_HEAD_LOW  (0x2C + G2D_TOP)
#define G2D_RCQ_HEAD_HIGH (0x30 + G2D_TOP)
#define G2D_RCQ_HEAD_LEN  (0x34 + G2D_TOP)

/* CMDQ control registers - alternative command queue interface */
#define G2D_CMDQ_ADDR     (0x38 + G2D_TOP)  /* CMDQ base address (low 32-bit) */
#define G2D_CMDQ_ADDR_H   (0x3C + G2D_TOP)  /* CMDQ base address (high 32-bit) */
#define G2D_CMDQ_CTL      (0x40 + G2D_TOP)  /* CMDQ control */
#define  G2D_CMDQ_CTL_START       BIT(0)    /* Start CMDQ execution */
#define G2D_CMDQ_STS      (0x44 + G2D_TOP)  /* CMDQ status */
#define  G2D_CMDQ_STS_BUSY        BIT(0)    /* CMDQ busy */
#define  G2D_CMDQ_STS_DONE        BIT(1)    /* CMDQ done */

/* Master control register - might be needed for mixer operation */
#define G2D_CONTROL      (0x00 + G2D_TOP)

/* TOP gates (same view for CPU and RCQ) */
#define G2D_SCLK_GATE  (0x00 + G2D_TOP)
#define  G2D_SCLK_GATE_MIXER  BIT(0)
#define  G2D_SCLK_GATE_ROT    BIT(1)
#define G2D_HCLK_GATE  (0x04 + G2D_TOP)
#define  G2D_HCLK_GATE_MIXER  BIT(0)
#define  G2D_HCLK_GATE_ROT    BIT(1)
#define G2D_AHB_RESET  (0x08 + G2D_TOP)
#define  G2D_AHB_MIXER_RESET  BIT(0)
#define  G2D_AHB_ROT_RESET    BIT(1)
#define G2D_SCLK_DIV   (0x0C + G2D_TOP)  /* Clock divider control */
#define G2D_VERSION    (0x10 + G2D_TOP)  /* IP version identification */
#define G2D_CMD_CTL    (0x14 + G2D_TOP)  /* DRAM command control - CRITICAL for RCQ DMA */
#define  G2D_CMD_CTL_RT_WB_SHIFT   16    /* RT_WB_CMD_CTL[19:16] */
#define  G2D_CMD_CTL_CORE1_SHIFT   4     /* CORE1_CMD_CTL[7:4] */
#define  G2D_CMD_CTL_CORE0_SHIFT   0     /* CORE0_CMD_CTL[3:0] */
#define  G2D_CMD_CTL_DEFAULT       0x00010001  /* Enable CORE0 and RT_WB */

/* CCU registers (T113/D1 clock/reset controller) */
#define G2D_CLK_REG    0x0630
#define  G2D_CLK_GATING       BIT(31)
#define G2D_BGR_REG    0x063c
#define  G2D_BGR_GATING      BIT(0)
#define  G2D_BGR_RST         BIT(16)

/* Mixer global registers - T113 RCQ addresses */
#define G2D_MIXER_CLK  (0x08 + G2D_MIXER_T113)
#define G2D_MIXER_INT  G2D_MIXER_INT_T113  /* Use legacy location */
#define  G2D_MIXER_INT_IRQ_PENDING     BIT(0)
#define  G2D_MIXER_INT_FINISH_IRQ_EN   BIT(4)
/* CRITICAL: MIXER_CTL must use LEGACY address 0x00100 for DIRECT mode writes! */
/* The RCQ address 0x28100 is NOT CPU-writable, only for RCQ payload data */
#define G2D_MIXER_CTL_LEGACY  (0x00 + G2D_MIXER)  /* 0x00100 - CPU writable */
#define G2D_MIXER_CTL_RCQ     (0x00 + G2D_MIXER_T113)  /* 0x28100 - RCQ only */
#define G2D_MIXER_CTL         G2D_MIXER_CTL_LEGACY  /* Default to LEGACY for DIRECT mode */
#define  G2D_MIXER_CTL_START           BIT(31)

/* Blender subset - T113 RCQ addresses */
#define BLD_EN_CTL         (0x000 + G2D_BLD_T113)
#define BLD_FILLC0         (0x010 + G2D_BLD_T113)
#define BLD_FILLC1         (0x014 + G2D_BLD_T113)
#define BLD_CH_ISIZE0      (0x020 + G2D_BLD_T113)
#define BLD_CH_OFFSET0     (0x030 + G2D_BLD_T113)
#define BLD_PREMUL_CTL     (0x040 + G2D_BLD_T113)
#define BLD_BK_COLOR       (0x044 + G2D_BLD_T113)
#define BLD_OUT_SIZE       (0x048 + G2D_BLD_T113)
#define BLD_SIZE           (0x048 + G2D_BLD_T113)
#define BLD_CTL            (0x04C + G2D_BLD_T113)
#define BLD_OUT_COLOR      (0x060 + G2D_BLD_T113)
#define ROP_CTL            (0x080 + G2D_BLD_T113)
#define ROP_INDEX0         (0x084 + G2D_BLD_T113)  /* ROP index for pipe 0 */
#define  BLD_PIPE0_EN      BIT(8)

/* Video layer (V0) subset - T113 RCQ addresses */
#define V0_ATTCTL      (0x00 + G2D_V0_T113)
#define  V0_ATTCTL_EN              BIT(0)
#define  V0_ATTCTL_FILLCOLOR_EN    BIT(4)
#define  V0_ATTCTL_FMT_SHIFT       8
#define V0_MBSIZE      (0x04 + G2D_V0_T113)
#define V0_COOR        (0x08 + G2D_V0_T113)
#define V0_PITCH0      (0x0C + G2D_V0_T113)
#define V0_PITCH1      (0x10 + G2D_V0_T113)
#define V0_PITCH2      (0x14 + G2D_V0_T113)
#define V0_LADD0       (0x18 + G2D_V0_T113)
#define V0_LADD1       (0x1C + G2D_V0_T113)
#define V0_LADD2       (0x20 + G2D_V0_T113)
#define V0_FILLC       (0x24 + G2D_V0_T113)
#define V0_HADD        (0x28 + G2D_V0_T113)
#define V0_SIZE        (0x2C + G2D_V0_T113)

/* G2D supported pixel formats (from BSP) */
enum g2d_fmt_hw_id {
	G2D_FORMAT_ARGB8888 = 0,
	G2D_FORMAT_ABGR8888,
	G2D_FORMAT_RGBA8888,
	G2D_FORMAT_BGRA8888,
	G2D_FORMAT_XRGB8888,   /* = 4 - XRGB8888 is index 4 in BSP enum */
	G2D_FORMAT_XBGR8888,
	G2D_FORMAT_RGBX8888,
	G2D_FORMAT_BGRX8888,
	G2D_FORMAT_RGB888,
	G2D_FORMAT_BGR888,
	G2D_FORMAT_RGB565,
	G2D_FORMAT_BGR565,
	/* More formats exist but not needed for basic fillrect */
};

/* Writeback subset - T113 RCQ addresses */
#define WB_ATT         (0x00 + G2D_WB_T113)
#define  WB_ATT_EN          BIT(0)
#define  WB_ATT_FMT_SHIFT     8
#define WB_SIZE        (0x04 + G2D_WB_T113)
#define WB_PITCH0      (0x08 + G2D_WB_T113)
#define WB_PITCH1      (0x0C + G2D_WB_T113)
#define WB_PITCH2      (0x10 + G2D_WB_T113)
#define WB_LADD0       (0x14 + G2D_WB_T113)
#define WB_HADD0       (0x18 + G2D_WB_T113)
#define WB_LADD1       (0x1C + G2D_WB_T113)
#define WB_HADD1       (0x20 + G2D_WB_T113)
#define WB_LADD2       (0x24 + G2D_WB_T113)
#define WB_HADD2       (0x28 + G2D_WB_T113)

/* ============================================================
 * V0.0.75: RCQ (Register Command Queue) structures
 * ============================================================
 * Based on BSP g2d_rcq.h - minimal implementation for fillrect
 */

/* RCQ header structure (32-byte aligned in DMA memory) */
struct g2d_rcq_head {
	u32 low_addr;        /* Lower 32 bits of register block physical address (32B aligned) */
	u32 dw0;             /* [31:24]=high_addr (bits 39:32), [23:0]=len (bytes, 2B aligned) */
	u32 dirty;           /* [0]=dirty flag (1=apply), [31:16]=n_header_len (frame header count) */
	u32 reg_offset;      /* Register offset from G2D base (e.g., 0x00800 for V0) */
};

/* Macros for building dirty field per BSP g2d_rcq.h */
#define G2D_RCQ_DIRTY_FLAG         BIT(0)    /* dirty=1: apply this block */
#define G2D_RCQ_N_HEADER_LEN_SHIFT 16        /* n_header_len in bits [31:16] */
#define G2D_RCQ_MAKE_DIRTY(dirty, n_headers) \
	(((dirty) & 0x1) | (((n_headers) & 0xFFFF) << G2D_RCQ_N_HEADER_LEN_SHIFT))

/* 32-byte alignment required by RCQ hardware */
#define G2D_RCQ_BYTE_ALIGN(x) (((x) + 31) & ~31)
/* 2-byte alignment for payload length */
#define G2D_RCQ_HEADER_ALIGN(x) (((x) + 1) & ~1)

/* Register blocks for each module (filled with actual register values) */
/* V0: Covers ATTCTL@+0x00 to FILLC@+0x24 = 0x28 bytes (40 bytes) */
struct rcq_v0_regs {
	u32 v0_attctl;       /* +0x00: V0_ATTCTL */
	u32 v0_mbsize;       /* +0x04: V0_MBSIZE (width/height) */
	u32 reserved1[8];    /* +0x08..+0x23: padding */
	u32 v0_fillc;        /* +0x24: V0_FILLC (fill color) */
};  /* Total: 0x28 bytes = 40 bytes */

/* BLD: Covers EN_CTL@+0x00 to ROP_CTL@+0x80 = 0x84 bytes */
struct rcq_bld_regs {
	u32 bld_en_ctl;      /* +0x00: BLD_EN_CTL */
	u32 reserved1[7];    /* +0x04..+0x1F: padding */
	u32 bld_ch_isize0;   /* +0x20: BLD_CH_ISIZE0 */
	u32 reserved2[9];    /* +0x24..+0x47: padding */
	u32 bld_out_size;    /* +0x48: BLD_OUT_SIZE */
	u32 bld_ctl;         /* +0x4C: BLD_CTL */
	u32 reserved3[12];   /* +0x50..+0x7F: padding to ROP */
	u32 rop_ctl;         /* +0x80: ROP_CTL - CRITICAL for g2d_fillrectangle! */
};  /* Total: 0x84 bytes = 132 bytes */

/* WB: Covers ATT@+0x00 to HADD0@+0x18 = 0x1C bytes, round to 0x2C for safety */
struct rcq_wb_regs {
	u32 wb_att;          /* +0x00: WB_ATT */
	u32 wb_size;         /* +0x04: WB_SIZE */
	u32 wb_pitch0;       /* +0x08: WB_PITCH0 */
	u32 reserved1[2];    /* +0x0C..+0x13: padding */
	u32 wb_ladd0;        /* +0x14: WB_LADD0 (low address) */
	u32 wb_hadd0;        /* +0x18: WB_HADD0 (high address) */
};  /* Total: 0x1C bytes = 28 bytes, but use 0x2C for alignment */

struct rcq_mixer_regs {
	u32 mixer_ctl;       /* MIXER_CTL @ +0x00 (RCQ 0x00100) - START bit[31] */
	u32 mixer_interrupt; /* MIXER_INT @ +0x04 (RCQ 0x00104) - finish_irq_en bit[4] */
};

#endif /* __SUNXI_G2D_REGS_MINI_H__ */

