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
#define G2D_GSU        0x30000

/* RCQ-based layout (T113/D1): legacy blocks shift to +0x28000 when RCQ active */
#define G2D_RCQ_BASE   0x28000

/* RCQ control block: back to TOP addresses (0x20-0x34)
 * These require special handling in T113 - accessed via raw mmio, not through calc_off */
#define G2D_RCQ_IRQ_CTL   (0x20 + G2D_TOP)  /* 0x0020 */
#define  G2D_RCQ_IRQ_SEL              BIT(0)
#define  G2D_RCQ_IRQ_TASK_END_EN      BIT(4)
#define  G2D_RCQ_IRQ_CFG_FINISH_EN    BIT(6)

#define G2D_RCQ_STATUS    (0x24 + G2D_TOP)  /* 0x0024 */
#define  G2D_RCQ_STATUS_TASK_END      BIT(0)
#define  G2D_RCQ_STATUS_CFG_FINISH    BIT(2)
#define  G2D_RCQ_STATUS_FRAME_CNT_SHIFT 8
#define  G2D_RCQ_STATUS_FRAME_CNT_MASK  (0xFF << G2D_RCQ_STATUS_FRAME_CNT_SHIFT)

#define G2D_RCQ_CTRL      (0x28 + G2D_TOP)  /* 0x0028 */
#define  G2D_RCQ_CTRL_UPDATE          BIT(0)
/* En T113 (v2 RCQ) hay un bit de habilitación adicional para RCQ.
 * Los BSP suelen usar EN|UPDATE. Documentación no pública sugiere EN en bit4.
 * Permitimos ajustar por sysfs si fuese necesario, pero por defecto BIT(4).
 */
#define  G2D_RCQ_CTRL_EN              BIT(4)

#define G2D_RCQ_HEAD_LOW  (0x2C + G2D_TOP)  /* 0x002C */
#define G2D_RCQ_HEAD_HIGH (0x30 + G2D_TOP)  /* 0x0030 */
#define G2D_RCQ_HEAD_LEN  (0x34 + G2D_TOP)  /* 0x0034 */
#define  G2D_RCQ_HEAD_LEN_MASK        0xFFFF

/* CMDQ and control registers in TOP space (BSP naming vs driver naming) */
#define G2D_CONTROL      (0x00 + G2D_TOP)
#define G2D_CMDQ_CTL     (0x140 + G2D_TOP)
#define G2D_CMDQ_STS     (0x144 + G2D_TOP)
#define G2D_CMDQ_ADDR    (0x148 + G2D_TOP)

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
#define G2D_VERSION    (0x10 + G2D_TOP)  /* IP version identification */
#define G2D_CMD_CTL    (0x14 + G2D_TOP)  /* DRAM command control - CRITICAL for RCQ DMA */
#define  G2D_CMD_CTL_DEFAULT       0x00010001  /* Enable CORE0 and RT_WB */

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
#define  G2D_MIXER_CTL_RESET  BIT(0)   /* Software reset */
#define CMD_CTL        G2D_MIXER_CTL  /* Alias for compatibility */
#define  CMD_CTL_START G2D_MIXER_CTL_START
#define G2D_MIXER_INT  (0x04 + G2D_MIXER)
#define  G2D_MIXER_INT_IRQ_PENDING     BIT(0)
#define  G2D_MIXER_INT_FINISH_IRQ_EN   BIT(4)
#define G2D_MIXER_CLK  (0x08 + G2D_MIXER)
#define MIXER_FILLCOLOR0 (0x0C + G2D_MIXER)
#define MIXER_FILLCOLOR1 (0x10 + G2D_MIXER)
#define MIXER_SIZE       (0x14 + G2D_MIXER)

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
#define  BLD_CTL_PIPE0_FCOLOR_EN   BIT(0)  /* Use fill color for pipe 0 */
#define  BLD_CTL_PIPE1_FCOLOR_EN   BIT(1)  /* Use fill color for pipe 1 */
#define  BLD_CTL_PIPE0_ALPHA_SHIFT 8       /* Alpha mode for pipe 0 */
#define  BLD_CTL_PIPE0_ALPHA_MASK  (0x3 << BLD_CTL_PIPE0_ALPHA_SHIFT)
#define  BLD_CTL_PIPE1_ALPHA_SHIFT 10      /* Alpha mode for pipe 1 */
#define  BLD_CTL_PIPE1_ALPHA_MASK  (0x3 << BLD_CTL_PIPE1_ALPHA_SHIFT)
#define  BLD_CTL_PIPE0_FCOLOR_ALPHA_SHIFT 16  /* Fill color alpha for pipe 0 */
#define  BLD_CTL_PIPE0_FCOLOR_ALPHA_MASK  (0xFF << BLD_CTL_PIPE0_FCOLOR_ALPHA_SHIFT)
#define  BLD_CTL_PIPE1_FCOLOR_ALPHA_SHIFT 24  /* Fill color alpha for pipe 1 */
#define  BLD_CTL_PIPE1_FCOLOR_ALPHA_MASK  (0xFF << BLD_CTL_PIPE1_FCOLOR_ALPHA_SHIFT)
/* Alpha blending modes */
#define  BLD_ALPHA_MODE_PIXEL      0  /* Use pixel alpha */
#define  BLD_ALPHA_MODE_GLOBAL     1  /* Use global alpha */
#define  BLD_ALPHA_MODE_MIXED      2  /* Pixel * global */
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

/* BLD CSC (Color Space Conversion) - complete map from BSP */
#define BLD_CSC_CTL        (0x100 + G2D_BLD)

// CSC0 coefficients (for pipe 0)
#define BLD_CSC0_COEF00    (0x110 + G2D_BLD)
#define BLD_CSC0_COEF01    (0x114 + G2D_BLD)
#define BLD_CSC0_COEF02    (0x118 + G2D_BLD)
#define BLD_CSC0_CONST0    (0x11C + G2D_BLD)
#define BLD_CSC0_COEF10    (0x120 + G2D_BLD)
#define BLD_CSC0_COEF11    (0x124 + G2D_BLD)
#define BLD_CSC0_COEF12    (0x128 + G2D_BLD)
#define BLD_CSC0_CONST1    (0x12C + G2D_BLD)
#define BLD_CSC0_COEF20    (0x130 + G2D_BLD)
#define BLD_CSC0_COEF21    (0x134 + G2D_BLD)
#define BLD_CSC0_COEF22    (0x138 + G2D_BLD)
#define BLD_CSC0_CONST2    (0x13C + G2D_BLD)

// CSC1 coefficients (for pipe 1)
#define BLD_CSC1_COEF00    (0x140 + G2D_BLD)
#define BLD_CSC1_COEF01    (0x144 + G2D_BLD)
#define BLD_CSC1_COEF02    (0x148 + G2D_BLD)
#define BLD_CSC1_CONST0    (0x14C + G2D_BLD)
#define BLD_CSC1_COEF10    (0x150 + G2D_BLD)
#define BLD_CSC1_COEF11    (0x154 + G2D_BLD)
#define BLD_CSC1_COEF12    (0x158 + G2D_BLD)
#define BLD_CSC1_CONST1    (0x15C + G2D_BLD)
#define BLD_CSC1_COEF20    (0x160 + G2D_BLD)
#define BLD_CSC1_COEF21    (0x164 + G2D_BLD)
#define BLD_CSC1_COEF22    (0x168 + G2D_BLD)
#define BLD_CSC1_CONST2    (0x16C + G2D_BLD)

// CSC2 coefficients (for output)
#define BLD_CSC2_COEF00    (0x170 + G2D_BLD)
#define BLD_CSC2_COEF01    (0x174 + G2D_BLD)
#define BLD_CSC2_COEF02    (0x178 + G2D_BLD)
#define BLD_CSC2_CONST0    (0x17C + G2D_BLD)
#define BLD_CSC2_COEF10    (0x180 + G2D_BLD)
#define BLD_CSC2_COEF11    (0x184 + G2D_BLD)
#define BLD_CSC2_COEF12    (0x188 + G2D_BLD)
#define BLD_CSC2_CONST1    (0x18C + G2D_BLD)
#define BLD_CSC2_COEF20    (0x190 + G2D_BLD)
#define BLD_CSC2_COEF21    (0x194 + G2D_BLD)
#define BLD_CSC2_COEF22    (0x198 + G2D_BLD)
#define BLD_CSC2_CONST2    (0x19C + G2D_BLD)

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
#define V0_HDS_CTL0    (0x30 + G2D_V0)  /* Horizontal downscaler */
#define V0_HDS_CTL1    (0x34 + G2D_V0)
#define V0_VDS_CTL0    (0x38 + G2D_V0)  /* Vertical downscaler */
#define V0_VDS_CTL1    (0x3C + G2D_V0)

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

// UI layers (UI0, UI1, UI2) — complete register maps from BSP
#define UI0_ATTR       (0x00 + G2D_UI0)
#define  UI_ATTR_EN              BIT(0)   /* layer enable */
#define  UI_ATTR_ALPHA_MODE_SHIFT 1
#define  UI_ATTR_ALPHA_MODE_MASK (0x3 << UI_ATTR_ALPHA_MODE_SHIFT)
#define  UI_ATTR_FILLCOLOR_EN    BIT(4)
#define  UI_ATTR_FMT_SHIFT       8
#define  UI_ATTR_FMT_MASK        (0x3F << UI_ATTR_FMT_SHIFT)
#define  UI_ATTR_PREMUL_SHIFT    16
#define  UI_ATTR_PREMUL_MASK     (0x3 << UI_ATTR_PREMUL_SHIFT)
#define  UI_ATTR_ALPHA_SHIFT     24       /* Global alpha value */
#define  UI_ATTR_ALPHA_MASK      (0xFF << UI_ATTR_ALPHA_SHIFT)

/* UI0 layer registers - single plane only (RGB formats) */
#define UI0_ATTR       (0x00 + G2D_UI0)
#define UI0_MBSIZE     (0x04 + G2D_UI0)
#define UI0_COOR       (0x08 + G2D_UI0)
#define UI0_PITCH      (0x0C + G2D_UI0)
#define UI0_PITCH0     (0x0C + G2D_UI0)    /* Alias */
#define UI0_LADD       (0x10 + G2D_UI0)
#define UI0_LADD0      (0x10 + G2D_UI0)    /* Alias */
#define UI0_FILLC      (0x14 + G2D_UI0)
#define UI0_HADD       (0x18 + G2D_UI0)
#define UI0_HADD0      (0x18 + G2D_UI0)    /* Alias */
#define UI0_SIZE       (0x1C + G2D_UI0)

/* UI1 layer registers - single plane only (RGB formats) */
#define UI1_ATTR       (0x00 + G2D_UI1)
#define UI1_MBSIZE     (0x04 + G2D_UI1)
#define UI1_COOR       (0x08 + G2D_UI1)
#define UI1_PITCH      (0x0C + G2D_UI1)
#define UI1_PITCH0     (0x0C + G2D_UI1)    /* Alias */
#define UI1_LADD       (0x10 + G2D_UI1)
#define UI1_LADD0      (0x10 + G2D_UI1)    /* Alias */
#define UI1_FILLC      (0x14 + G2D_UI1)
#define UI1_HADD       (0x18 + G2D_UI1)
#define UI1_HADD0      (0x18 + G2D_UI1)    /* Alias */
#define UI1_SIZE       (0x1C + G2D_UI1)

/* UI2 layer registers - single plane only (RGB formats) */
#define UI2_ATTR       (0x00 + G2D_UI2)
#define UI2_MBSIZE     (0x04 + G2D_UI2)
#define UI2_COOR       (0x08 + G2D_UI2)
#define UI2_PITCH      (0x0C + G2D_UI2)
#define UI2_PITCH0     (0x0C + G2D_UI2)    /* Alias */
#define UI2_LADD       (0x10 + G2D_UI2)
#define UI2_LADD0      (0x10 + G2D_UI2)    /* Alias */
#define UI2_FILLC      (0x14 + G2D_UI2)
#define UI2_HADD       (0x18 + G2D_UI2)
#define UI2_HADD0      (0x18 + G2D_UI2)    /* Alias */
#define UI2_SIZE       (0x1C + G2D_UI2)

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
#define WB_CROP_COOR   (0x2C + G2D_WB)  /* Crop coordinate: X|Y position for output */

/* WB_ATT bits (minimal, based on BSP patterns; subject to refinement) */
#define WB_ATT_EN          BIT(0)  /* writeback enable */
/* Optional: format selection; keep 0 for default for now. */
/* FBFMT field for WB: matches BSP/Tina enum ids at bits [13:8] */
#define WB_ATT_FMT_SHIFT     8
#define WB_ATT_FMT_MASK      (0x3F << WB_ATT_FMT_SHIFT)

// VSU (Video Scaler Unit) - Complete register map from BSP
#define VS_CTRL           (0x000 + G2D_VSU)
#define  VS_CTRL_EN                BIT(0)
#define  VS_CTRL_COEF_ACCESS_SEL   BIT(1)
#define  VS_CTRL_FILTER_TYPE_RGB   BIT(16)  /* 1=RGB, 0=YUV */

#define VS_OUT_SIZE       (0x040 + G2D_VSU)
#define VS_GLB_ALPHA      (0x044 + G2D_VSU)

#define VS_Y_SIZE         (0x080 + G2D_VSU)
#define VS_Y_HSTEP        (0x088 + G2D_VSU)
#define VS_Y_VSTEP        (0x08C + G2D_VSU)
#define VS_Y_HPHASE       (0x090 + G2D_VSU)
#define VS_Y_VPHASE0      (0x098 + G2D_VSU)

#define VS_C_SIZE         (0x0C0 + G2D_VSU)
#define VS_C_HSTEP        (0x0C8 + G2D_VSU)
#define VS_C_VSTEP        (0x0CC + G2D_VSU)
#define VS_C_HPHASE       (0x0D0 + G2D_VSU)
#define VS_C_VPHASE0      (0x0D8 + G2D_VSU)

// VSU format types (matches BSP g2d_bsp.h vsu_pixel_format enum)
#define VSU_FORMAT_YUV422  0x00
#define VSU_FORMAT_YUV420  0x01
#define VSU_FORMAT_YUV411  0x02
#define VSU_FORMAT_RGB     0x03

// VSU coefficient tables
#define VS_Y_HCOEF0       (0x200 + G2D_VSU)  // Y horizontal coefficients (32 entries)
#define VS_Y_VCOEF0       (0x300 + G2D_VSU)  // Y vertical coefficients (32 entries)
#define VS_C_HCOEF0       (0x400 + G2D_VSU)  // Chroma horizontal coefficients (32 entries)
#define VS_C_VCOEF0       (0x500 + G2D_VSU)  // Chroma vertical coefficients (32 entries)

// VSU phase fractional bits and constants (from BSP)
// CRITICAL: BSP uses 19 bits, NOT 18!
// Combined with << 1 shift when writing to register, this gives 20-bit precision total
#define VSU_PHASE_FRAC_BITWIDTH  19
#define VSU_PHASE_NUM            32
#define VSU_ZOOM0_SIZE           1
#define VSU_ZOOM1_SIZE           8
#define VSU_ZOOM2_SIZE           4
#define VSU_ZOOM3_SIZE           1
#define VSU_ZOOM4_SIZE           1

// ROT (rotator) - for scaling and rotation
#define ROT_CTL        (0x00 + G2D_ROT)
#define  ROT_CTL_START         BIT(31)
#define  ROT_CTL_EN            BIT(0)
#define  ROT_CTL_ROT_90        (0x1 << 4)
#define  ROT_CTL_ROT_180       (0x2 << 4)
#define  ROT_CTL_ROT_270       (0x3 << 4)
#define  ROT_CTL_FLIP_H        BIT(7)
#define  ROT_CTL_FLIP_V        BIT(6)

#define ROT_INT        (0x04 + G2D_ROT)
#define  ROT_INT_FINISH        BIT(0)
#define  ROT_INT_FINISH_EN     BIT(16)

#define ROT_TIMEOUT    (0x08 + G2D_ROT)
#define ROT_IFMT       (0x20 + G2D_ROT)
#define ROT_ISIZE      (0x24 + G2D_ROT)
#define ROT_IPITCH0    (0x30 + G2D_ROT)
#define ROT_IPITCH1    (0x34 + G2D_ROT)
#define ROT_IPITCH2    (0x38 + G2D_ROT)
#define ROT_ILADD0     (0x40 + G2D_ROT)
#define ROT_IHADD0     (0x44 + G2D_ROT)
#define ROT_ILADD1     (0x48 + G2D_ROT)
#define ROT_IHADD1     (0x4C + G2D_ROT)
#define ROT_ILADD2     (0x50 + G2D_ROT)
#define ROT_IHADD2     (0x54 + G2D_ROT)

/* ROT output registers - BSP verified offsets (no ROT_OFMT!) */
#define ROT_OSIZE      (0x84 + G2D_ROT)
#define ROT_OPITCH0    (0x90 + G2D_ROT)
#define ROT_OPITCH1    (0x94 + G2D_ROT)
#define ROT_OPITCH2    (0x98 + G2D_ROT)
#define ROT_OLADD0     (0xA0 + G2D_ROT)
#define ROT_OHADD0     (0xA4 + G2D_ROT)
#define ROT_OLADD1     (0xA8 + G2D_ROT)
#define ROT_OHADD1     (0xAC + G2D_ROT)
#define ROT_OLADD2     (0xB0 + G2D_ROT)
#define ROT_OHADD2     (0xB4 + G2D_ROT)

#endif // __SUNXI_G2D_REGS_H__
