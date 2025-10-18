// SPDX-License-Identifier: GPL-2.0
// Minimal register map for Allwinner G2D (v2.x style), extracted/adapted from BSP
// Only the offsets we need initially; extend as we implement more ops.

#ifndef __SUNXI_G2D_REGS_H__
#define __SUNXI_G2D_REGS_H__

// Module base blocks (legacy layout, before RCQ)
#define G2D_TOP        0x00000
#define G2D_MIXER      0x00100
#define G2D_BLD        0x00400
#define G2D_V0         0x00800
#define G2D_UI0        0x01000
#define G2D_UI1        0x01800
#define G2D_UI2        0x02000
#define G2D_WB         0x03000
#define G2D_VSU        0x08000

/* RCQ-based layout (T113/D1): legacy blocks shift to +0x28000 when RCQ active */
#define G2D_RCQ_BASE   0x28000

/* RCQ control block (still located within TOP address space) */
#define G2D_RCQ_IRQ_CTL   (0x20 + G2D_TOP)
#define  G2D_RCQ_IRQ_SEL              BIT(0)
#define  G2D_RCQ_IRQ_TASK_END_EN      BIT(4)
#define  G2D_RCQ_IRQ_CFG_FINISH_EN    BIT(6)

#define G2D_RCQ_STATUS    (0x24 + G2D_TOP)
#define  G2D_RCQ_STATUS_TASK_END      BIT(0)
#define  G2D_RCQ_STATUS_CFG_FINISH    BIT(2)
#define  G2D_RCQ_STATUS_FRAME_CNT_SHIFT 8
#define  G2D_RCQ_STATUS_FRAME_CNT_MASK  (0xFF << G2D_RCQ_STATUS_FRAME_CNT_SHIFT)

#define G2D_RCQ_CTRL      (0x28 + G2D_TOP)
#define  G2D_RCQ_CTRL_UPDATE          BIT(0)

#define G2D_RCQ_HEAD_LOW  (0x2C + G2D_TOP)
#define G2D_RCQ_HEAD_HIGH (0x30 + G2D_TOP)
#define G2D_RCQ_HEAD_LEN  (0x34 + G2D_TOP)
#define  G2D_RCQ_HEAD_LEN_MASK        0xFFFF

#define G2D_ROT        0x28000
#define G2D_GSU        0x30000

// TOP gates
#define G2D_SCLK_GATE  (0x00 + G2D_TOP)
#define  G2D_SCLK_GATE_MIXER  BIT(0)
#define  G2D_SCLK_GATE_ROT    BIT(1)
#define  G2D_SCLK_GATE_ALL    0x000000FF
#define G2D_HCLK_GATE  (0x04 + G2D_TOP)
#define  G2D_HCLK_GATE_MIXER  BIT(0)
#define  G2D_HCLK_GATE_ROT    BIT(1)
#define  G2D_HCLK_GATE_ALL    0x000000FF
#define G2D_AHB_RESET  (0x08 + G2D_TOP)
#define  G2D_AHB_MIXER_RESET  BIT(0)
#define  G2D_AHB_ROT_RESET    BIT(1)
#define  G2D_AHB_RESET_ALL    0x000000FF
#define G2D_SCLK_DIV   (0x0C + G2D_TOP)

/* T113/D1: Internal IP clock and gating/reset registers */
#define G2D_CLK_REG    (0x0630 + G2D_TOP)
#define  G2D_CLK_GATING       BIT(31)      /* 1: Clock ON */
#define  G2D_CLK_SRC_SEL_SHIFT 24          /* 3 bits */
#define  G2D_CLK_SRC_SEL_MASK  (0x7 << G2D_CLK_SRC_SEL_SHIFT)
#define  G2D_CLK_FACTOR_M_MASK 0x1F        /* M = FACTOR_M + 1 */

#define G2D_BGR_REG    (0x063C + G2D_TOP)
#define  G2D_BGR_GATING      BIT(0)        /* 1: Pass */
#define  G2D_BGR_RST         BIT(16)       /* 1: De-assert */

// MIXER (global)
#define G2D_MIXER_CTL  (0x00 + G2D_MIXER)
#define  G2D_MIXER_CTL_START  BIT(31)
#define G2D_MIXER_INT  (0x04 + G2D_MIXER)
#define  G2D_MIXER_INT_IRQ_PENDING     BIT(0)
#define  G2D_MIXER_INT_FINISH_IRQ_EN   BIT(4)
#define G2D_MIXER_CLK  (0x08 + G2D_MIXER)

// BLD (blender) — mínimos para enrutar V0 -> salida
#define BLD_EN_CTL         (0x000 + G2D_BLD)
#define  BLD_PIPE0_FENCE_EN BIT(0)
#define  BLD_PIPE1_FENCE_EN BIT(1)
#define  BLD_PIPE0_EN      BIT(8)
#define  BLD_PIPE1_EN      BIT(9)
#define BLD_FILLC0         (0x010 + G2D_BLD)
#define BLD_FILLC1         (0x014 + G2D_BLD)
#define BLD_CH_ISIZE0      (0x020 + G2D_BLD) /* input 0 size */
#define BLD_CH_ISIZE1      (0x024 + G2D_BLD)
#define BLD_CH_OFFSET0     (0x030 + G2D_BLD)
#define BLD_CH_OFFSET1     (0x034 + G2D_BLD)
#define BLD_PREMUL_CTL     (0x040 + G2D_BLD)
#define  BLD_PREMUL_CTL_PIPE0_ALPHA_MODE BIT(0)
#define  BLD_PREMUL_CTL_PIPE1_ALPHA_MODE BIT(1)
#define BLD_BK_COLOR       (0x044 + G2D_BLD)
#define BLD_SIZE           (0x048 + G2D_BLD) /* BSP: blend output size */
#define BLD_OUT_SIZE       (0x048 + G2D_BLD) /* Alias de BLD_SIZE */
#define BLD_CTL            (0x04C + G2D_BLD)
#define BLD_KEY_CTL        (0x050 + G2D_BLD)
#define BLD_KEY_CON        (0x054 + G2D_BLD)
#define BLD_KEY_MAX        (0x058 + G2D_BLD)
#define BLD_KEY_MIN        (0x05C + G2D_BLD)
#define BLD_OUT_COLOR      (0x060 + G2D_BLD)
#define  BLD_OUT_COLOR_PREMUL_EN   BIT(0)
#define  BLD_OUT_COLOR_ALPHA_MODE  BIT(1)

/* ROP (Raster Operation) - within BLD block */
#define ROP_CTL            (0x080 + G2D_BLD)
#define ROP_INDEX0         (0x084 + G2D_BLD)
#define ROP_INDEX1         (0x088 + G2D_BLD)

// Video layer (V0) — source input for M2M path
#define V0_ATTCTL      (0x00 + G2D_V0)
#define V0_MBSIZE      (0x04 + G2D_V0)
#define V0_COOR        (0x08 + G2D_V0)
#define V0_PITCH0      (0x0C + G2D_V0)
#define V0_PITCH1      (0x10 + G2D_V0)
#define V0_PITCH2      (0x14 + G2D_V0)
#define V0_LADD0       (0x18 + G2D_V0) /* aka V0_LADDR0 */
#define V0_LADD1       (0x1C + G2D_V0)
#define V0_LADD2       (0x20 + G2D_V0)
#define V0_FILLC       (0x24 + G2D_V0)
#define V0_HADD        (0x28 + G2D_V0)
#define V0_SIZE        (0x2C + G2D_V0)

/* V0_ATTCTL bits */
#define V0_ATTCTL_EN              BIT(0)   /* layer enable */
#define V0_ATTCTL_ALPHA_MODE      GENMASK(2, 1)
#define V0_ATTCTL_FILLCOLOR_EN    BIT(4)
/* Optional: format selection (implementation-dependent). Keep 0 for default. */
/* FBFMT field for V0 layer: matches BSP/Tina enum ids at bits [13:8] */
#define V0_ATTCTL_FMT_SHIFT       8
#define V0_ATTCTL_FMT_MASK        (0x3F << V0_ATTCTL_FMT_SHIFT)
#define V0_ATTCTL_PREMUL_CTL      GENMASK(17, 16)
#define V0_ATTCTL_GLBALPHA        GENMASK(31, 24)

// Writeback (destination memory)
#define WB_ATT         (0x00 + G2D_WB)
#define WB_SIZE        (0x04 + G2D_WB)
#define WB_PITCH0      (0x08 + G2D_WB)
#define WB_PITCH1      (0x0C + G2D_WB)
#define WB_PITCH2      (0x10 + G2D_WB)
#define WB_LADD0       (0x14 + G2D_WB)
#define WB_HADD0       (0x18 + G2D_WB)
#define WB_LADD1       (0x1C + G2D_WB)
#define WB_HADD1       (0x20 + G2D_WB)
#define WB_LADD2       (0x24 + G2D_WB)
#define WB_HADD2       (0x28 + G2D_WB)

/* WB_ATT bits (minimal, based on BSP patterns; subject to refinement) */
#define WB_ATT_EN          BIT(0)  /* writeback enable */
/* Optional: format selection; keep 0 for default for now. */
/* FBFMT field for WB: matches BSP/Tina enum ids at bits [13:8] */
#define WB_ATT_FMT_SHIFT     8
#define WB_ATT_FMT_MASK      (0x3F << WB_ATT_FMT_SHIFT)

// VSU (scaler)
#define VS_CTRL        (0x000 + G2D_VSU)
#define VS_OUT_SIZE    (0x040 + G2D_VSU)
#define VS_Y_SIZE      (0x080 + G2D_VSU)
#define VS_Y_HSTEP     (0x088 + G2D_VSU)
#define VS_Y_VSTEP     (0x08C + G2D_VSU)
#define VS_Y_HPHASE    (0x090 + G2D_VSU)

// ROT (rotator) — optional for later
#define ROT_CTL        (0x00 + G2D_ROT)
#define ROT_INT        (0x04 + G2D_ROT)

#endif // __SUNXI_G2D_REGS_H__
