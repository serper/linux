// SPDX-License-Identifier: GPL-2.0
// Minimal register map for Allwinner G2D (v2.x style), extracted/adapted from BSP
// Only the offsets we need initially; extend as we implement more ops.

#ifndef __SUNXI_G2D_REGS_H__
#define __SUNXI_G2D_REGS_H__

// Module base blocks
#define G2D_TOP        0x00000
#define G2D_MIXER      0x00100
#define G2D_BLD        0x00400
#define G2D_V0         0x00800
#define G2D_UI0        0x01000
#define G2D_UI1        0x01800
#define G2D_UI2        0x02000
#define G2D_WB         0x03000
#define G2D_VSU        0x08000
#define G2D_ROT        0x28000
#define G2D_GSU        0x30000

// TOP
#define G2D_SCLK_GATE  (0x00 + G2D_TOP)
#define G2D_HCLK_GATE  (0x04 + G2D_TOP)
#define G2D_AHB_RESET  (0x08 + G2D_TOP)
#define G2D_SCLK_DIV   (0x0C + G2D_TOP)

// MIXER (global)
#define G2D_MIXER_CTL  (0x00 + G2D_MIXER)
#define G2D_MIXER_INT  (0x04 + G2D_MIXER)
#define G2D_MIXER_CLK  (0x08 + G2D_MIXER)

// Video layer (V0) — source input for M2M path
#define V0_ATTCTL      (0x00 + G2D_V0)
#define V0_MBSIZE      (0x04 + G2D_V0)
#define V0_COOR        (0x08 + G2D_V0)
#define V0_PITCH0      (0x0C + G2D_V0)
#define V0_PITCH1      (0x10 + G2D_V0)
#define V0_PITCH2      (0x14 + G2D_V0)
#define V0_LADD0       (0x18 + G2D_V0)
#define V0_LADD1       (0x1C + G2D_V0)
#define V0_LADD2       (0x20 + G2D_V0)
#define V0_FILLC       (0x24 + G2D_V0)
#define V0_HADD        (0x28 + G2D_V0)
#define V0_SIZE        (0x2C + G2D_V0)

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
