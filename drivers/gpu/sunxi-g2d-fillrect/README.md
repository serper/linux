# T113 G2D Fillrect Test Driver - v1.0.0 STABLE

## Overview

This driver demonstrates a **working baseline** for G2D hardware acceleration on Allwinner T113-S3 SoC using **DIRECT mode** (CPU register writes). It successfully fills a 64×64 ARGB8888 buffer with solid color, proving the complete hardware path works.

## Status: ✓ WORKING

All 4096 pixels correctly filled with test color `0xFF00FF00` (green).

## Key Discoveries

### 1. Two Operational Modes

The T113 G2D hardware supports two modes:

- **DIRECT mode** (used here): CPU writes registers → `MIXER_CTL.START` → IRQ
  - Simpler, works for individual operations
  - Perfect for fillrect, blit, scaling, rotation
  - **This is what BSP uses for basic operations**

- **RCQ mode** (not needed): DMA command queue for batch processing
  - More complex, requires DMA descriptor chains
  - Only useful for batching multiple operations
  - Deferred to future optimization phase

### 2. MIXER_CTL Address Discovery

CRITICAL: The T113 has **TWO addresses** for MIXER_CTL:

- `0x00100` - Legacy address, **CPU-writable** for DIRECT mode ✓
- `0x28100` - RCQ-only address, **NOT CPU-writable** ✗

Using the wrong address causes all register writes to be ignored!

### 3. Missing BLD Registers

The blender (BLD) requires **sizing registers** to process more than 1 pixel:

- `BLD_CH_ISIZE0` - Input size for channel 0 (MUST match surface dimensions)
- `BLD_OUT_SIZE` - Output size (MUST match surface dimensions)
- `BLD_PREMUL_CTL` - Premultiply control
- `BLD_CH_OFFSET0` - Channel offset
- `BLD_OUT_COLOR` - Color space (RGB vs YUV)

**Without these, the blender only processes pixel (0,0) and the rest stay black!**

This was the critical bug that took 131 iterations to find.

## Register Sequence (DIRECT Mode)

Based on BSP analysis and confirmed working:

```c
1. Reset: G2D_AHB_RESET = 0 → 3

2. V0 Layer Setup:
   - V0_ATTCTL = 0xFF000011  // fillcolor_en | ARGB8888 | EN | alpha=0xFF
   - V0_MBSIZE = 0x003F003F  // 64×64 macroblock size
   - V0_SIZE = 0x003F003F    // 64×64 surface size
   - V0_COOR = 0x00000000    // Position (0,0)
   - V0_PITCH0 = 0x00000100  // 256 bytes pitch (64 pixels × 4 BPP)
   - V0_FILLC = 0xFF00FF00   // Fill color (green)

3. BLD (Blender) Setup - CRITICAL SIZING:
   - BLD_EN_CTL |= 0x100     // Enable pipe 0 (RMW!)
   - BLD_PREMUL_CTL = 0x0    // No premultiply
   - BLD_CH_ISIZE0 = 0x003F003F   // *** Channel 0 input: 64×64 ***
   - BLD_CH_OFFSET0 = 0x0    // No offset
   - BLD_OUT_SIZE = 0x003F003F    // *** Blender output: 64×64 ***
   - BLD_OUT_COLOR &= ~BIT(1)     // RGB mode (RMW!)
   - BLD_CTL = 0x0           // Mode 0: direct copy

4. ROP Setup:
   - ROP_CTL = 0xF0          // Pass-through
   - ROP_INDEX0 = 0x61080    // COPYPEN for pipe 0

5. WB (Writeback) Setup:
   - WB_LADD0 = lower_32_bits(dma_addr)
   - WB_HADD0 = upper_32_bits(dma_addr)
   - WB_PITCH0 = 0x100       // 256 bytes pitch
   - WB_SIZE = 0x003F003F    // 64×64 output
   - BLD_SIZE = 0x003F003F   // *** Write AGAIN here (timing!) ***
   - WB_ATT = 0x0            // ARGB8888 format

6. MIXER Control:
   - MIXER_INT = 0x11        // Clear pending + enable IRQ
   - MIXER_CTL |= 0x80000000 // Set START bit (RMW!)

7. Wait for IRQ:
   - IRQ fires with MIXER_INT.finish = 1
   - Handler clears pending bit
   - Task complete!
```

## Hardware Info

- **SoC**: Allwinner T113-S3
- **G2D IP**: 0x01100114 (version 1.1.0)
- **Base address**: 0x05410000
- **IRQ**: 253
- **Clocks**:
  - mod: 300 MHz
  - bus: 200 MHz
  - mbus: 396 MHz

## Build and Test

```bash
# Build
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M=drivers/gpu/sunxi-g2d-fillrect

# Copy to target
scp sunxi-g2d-fillrect.ko root@<target-ip>:/root/

# Load and test
insmod sunxi-g2d-fillrect.ko
dmesg | tail -50
```

Expected output:
```
>>> DIRECT Test Result: SUCCESS ✓
    [0]=0xff00ff00 [10]=0xff00ff00 [100]=0xff00ff00 [1000]=0xff00ff00
```

## Next Steps

This driver serves as the **proven baseline** for:

1. **V4L2 M2M driver** development (`sunxi-g2d-m2m`)
   - Use same register sequence for fillrect
   - Add V0 source layer configuration for blit
   - Add VSU block for scaling
   - Add rotation support (V0_ATTCTL rotation bits)
   - Add format conversion (CSC enable)

2. **Performance optimization**
   - Current: ~2ms per 64×64 frame (sufficient for 60fps)
   - Future: Batch operations with RCQ mode

3. **Features to implement**
   - Rotation (0°, 90°, 180°, 270°)
   - Scaling (up/down with quality control)
   - Format conversion (RGB ↔ YUV)
   - Alpha blending
   - Color space conversion

## References

- BSP kernel: `g2d_bsp_v2.c`, `g2d_driver_v2.c`
- Mainline reference: `drivers/gpu/sunxi-g2d-m2m/sunxi-g2d-m2m.c`
- Documentation: `T113-G2D-INVESTIGATION.md`

## Version History

- **v1.0.0** - STABLE: Working DIRECT mode baseline, all pixels correct
- **v0.0.131** - Added missing BLD registers (BLD_CH_ISIZE0, BLD_OUT_SIZE, etc.)
- **v0.0.130** - Added V0_MBSIZE
- **v0.0.123** - Fixed MIXER_CTL address (0x00100 vs 0x28100) - CRITICAL
- **v0.0.124** - Fixed IRQ handler (clear pending first)
- **v0.0.121** - Strategic pivot from RCQ to DIRECT mode
- **v0.0.1-v0.0.120** - RCQ mode investigation (learned RCQ not needed)

## Author

Developed through extensive hardware analysis and BSP reverse engineering for T113-S3 mainline Linux support.
