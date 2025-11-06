# G2D V2 Register Analysis - Scaling Operations

## Test Case: Scale ball from 110×110 to 55×55

### Expected Register Configuration (Based on BSP)

#### V0 Layer (Source - Ball)
```
Input parameters:
- Buffer: 110×110 ARGB8888
- Crop: (0,0) 110×110 (full buffer)
- Format: G2D_FMT_ARGB8888 (0x0)
- Base address: ball_dma_addr

Calculated values:
- bpp = 4 (ARGB8888)
- pitch = 110 * 4 = 440 bytes
- offset = 0 * 440 + 0 * 4 = 0
- addr = ball_dma_addr + 0

Expected register writes:
V0_ATTCTL    = 0x00FF0001  // EN=1, FMT=0 (ARGB8888), ALPHA_MODE=0 (pixel), ALPHA=0xFF
V0_MBSIZE    = 0x006D006D  // (110-1) << 16 | (110-1) = 0x6D = 109
V0_SIZE      = 0x006D006D  // Same as MBSIZE
V0_COOR      = 0x00000000  // (0,0) - no offset needed, address already adjusted
V0_PITCH0    = 0x000001B8  // 440 decimal = 0x1B8
V0_LADD0     = ball_dma_addr (low 32 bits)
V0_HADD      = ball_dma_addr >> 32 (high 8 bits)
```

#### VSU (Scaler)
```
Input parameters:
- Input size: 110×110 (from V0 MBSIZE)
- Output size: 55×55 (destination crop)
- Format: VSU_FORMAT_RGB

Calculated values:
- yhstep = (110 << 20) / 55 = 0x00200000 (2.0 in fixed point)
- yvstep = (110 << 20) / 55 = 0x00200000 (2.0 in fixed point)
- yhcoef_offset = g2d_vsu_calc_fir_coef(0x00200000)
- yvcoef_offset = g2d_vsu_calc_fir_coef(0x00200000)

Expected register writes:
VS_CTRL      = 0x00000101  // EN=1, COEF_ACCESS=1, FORMAT=RGB
VS_OUT_SIZE  = 0x00360036  // (55-1) << 16 | (55-1) = 0x36 = 54
VS_GLB_ALPHA = 0x000000FF  // 255
VS_Y_SIZE    = 0x006D006D  // (110-1) << 16 | (110-1)
VS_Y_HSTEP   = 0x00400000  // yhstep << 1
VS_Y_VSTEP   = 0x00400000  // yvstep << 1
VS_C_SIZE    = 0x006D006D  // Same as Y_SIZE for RGB
VS_C_HSTEP   = 0x00400000  // Same as Y_HSTEP for RGB
VS_C_VSTEP   = 0x00400000  // Same as Y_VSTEP for RGB
VS_Y_HPHASE  = 0x00000000
VS_Y_VPHASE0 = 0x00000000
VS_C_HPHASE  = 0x00000000
VS_C_VPHASE0 = 0x00000000
VS_Y_HCOEF0-31 = lan2coefftab32_full[yhcoef_offset + i]
VS_Y_VCOEF0-31 = linearcoefftab32[i]
VS_C_HCOEF0-31 = lan2coefftab32_full[yhcoef_offset + i]
```

#### UI2 Layer (Background - Temp)
```
Input parameters:
- Buffer: 800×480 XRGB8888
- Crop: (x,y) 55×55 (where ball should appear)
- Format: G2D_FMT_XRGB8888 (0x4)
- Base address: temp_dma_addr

Calculated values (example x=100, y=100):
- bpp = 4 (XRGB8888)
- pitch = 800 * 4 = 3200 bytes
- offset = 100 * 3200 + 100 * 4 = 320400 bytes
- addr = temp_dma_addr + 320400

Expected register writes:
UI2_ATTR     = 0xFF000401  // EN=1, FMT=4 (XRGB8888), ALPHA_MODE=0, ALPHA=0xFF
UI2_MBSIZE   = 0x00360036  // (55-1) << 16 | (55-1)
UI2_SIZE     = 0x00360036  // Same as MBSIZE
UI2_COOR     = 0x00000000  // (0,0) - address already offset
UI2_PITCH    = 0x00000C80  // 3200 decimal = 0xC80
UI2_LADD     = (temp_dma_addr + 320400) & 0xFFFFFFFF
UI2_HADD     = (temp_dma_addr + 320400) >> 32
```

#### BLD (Blender)
```
Input sizes (AFTER VSU scaling):
- Pipe0 (UI2): 55×55
- Pipe1 (V0+VSU): 55×55
- Output: 55×55

Expected register writes:
BLD_EN_CTL       = 0x00000003  // P0_EN=1, P1_EN=1
BLD_PREMUL_CTL   = 0x00000003  // P0_ALPHA_MODE=1, P1_ALPHA_MODE=1
BLD_CH_ISIZE0    = 0x00360036  // (55-1) << 16 | (55-1)
BLD_CH_ISIZE1    = 0x00360036  // (55-1) << 16 | (55-1)
BLD_CH_OFFSET0   = 0x00000000  // (0,0)
BLD_CH_OFFSET1   = 0x00000000  // (0,0)
BLD_SIZE         = 0x00360036  // (55-1) << 16 | (55-1)
BLD_CTL          = 0x03010301  // SRCOVER mode
BLD_OUT_COLOR    = 0x00000000  // RGB mode, no premul
ROP_CTL          = 0x000000F0  // Source copy
```

#### WB (Writeback)
```
Output parameters:
- Buffer: 800×480 XRGB8888 (temp buffer)
- Region: (x,y) 55×55
- Format: G2D_FMT_XRGB8888 (0x4)

Calculated values (example x=100, y=100):
- bpp = 4
- pitch = 800 * 4 = 3200 bytes
- offset = 100 * 3200 + 100 * 4 = 320400
- addr = temp_dma_addr + 320400

Expected register writes:
WB_ATT       = 0x00000004  // FMT=4 (XRGB8888)
WB_SIZE      = 0x00360036  // (55-1) << 16 | (55-1)
WB_PITCH0    = 0x00000C80  // 3200 decimal
WB_LADD0     = (temp_dma_addr + 320400) & 0xFFFFFFFF
WB_HADD0     = (temp_dma_addr + 320400) >> 32
```

## Comparison Template

### Actual Values from Kernel Logs

```
[Paste dmesg output here after running test]
```

### Analysis Checklist

- [ ] V0_MBSIZE matches expected (0x006D006D for 110×110)
- [ ] V0_PITCH0 matches expected (440 bytes for 110-wide buffer)
- [ ] V0_LADD0 is ball_dma_addr (no offset since crop starts at 0,0)
- [ ] VS_CTRL shows VSU enabled (bit 0 = 1)
- [ ] VS_Y_SIZE matches V0_MBSIZE (input to VSU = 110×110)
- [ ] VS_OUT_SIZE matches blend size (output from VSU = 55×55)
- [ ] VS_Y_HSTEP/VSTEP are correct for 2:1 scale (0x00400000)
- [ ] BLD_CH_ISIZE0/1 match VS_OUT_SIZE (55×55, not 110×110)
- [ ] BLD_SIZE matches output size (55×55)
- [ ] WB_SIZE matches BLD_SIZE (55×55)

## Common Mistakes to Avoid

1. **Setting BLD sizes to layer input instead of VSU output**
   - ❌ BLD_CH_ISIZE1 = 110×110 (V0 size before scaling)
   - ✅ BLD_CH_ISIZE1 = 55×55 (after VSU scaling)

2. **Using crop width for pitch**
   - ❌ V0_PITCH0 = 55 * 4 = 220 (crop width)
   - ✅ V0_PITCH0 = 110 * 4 = 440 (full buffer width)

3. **Not offsetting address for crop**
   - ❌ V0_LADD0 = base_addr (when crop_x/y ≠ 0)
   - ✅ V0_LADD0 = base_addr + crop_y*pitch + crop_x*bpp

4. **Setting MBSIZE to full buffer instead of crop**
   - ❌ V0_MBSIZE = 110×110 when reading 55×55 crop
   - ✅ V0_MBSIZE = 55×55 (crop dimensions)

## Test Procedure

1. Compile driver with debug logging enabled
2. Run demo: `sudo ./demo-bouncing-ball`
3. Capture first few frames: `sudo dmesg | grep -E "V0_|VS_|BLD_|WB_" > /tmp/g2d_regs.log`
4. Compare actual vs expected values
5. Identify discrepancies
6. Fix driver code
7. Repeat until registers match expected configuration
