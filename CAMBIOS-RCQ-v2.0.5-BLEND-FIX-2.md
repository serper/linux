# Fix for G2D_CMD_BLEND with Color Key (Attempt 2)

## Issue
The user reported that `G2D_CMD_BLEND` with `color_key_enable` (mask) still fails (no operation) even after the format swap fix.
Log analysis shows that `ROP_CTL` was set to `0xF0` (Bypass) in blending mode.

## Investigation
1. **ROP Bypass**: The driver was setting `ROP_CTL` to `0xF0` (Bypass All Channels) for blending operations to pass the blender output directly.
2. **Color Key Interaction**: It is highly probable that the Color Key logic is implemented within or dependent on the ROP unit. Bypassing the ROP unit likely bypasses the Color Key effect as well.
3. **Symptoms**: "No realiza ni el blend ni el mask". If ROP is bypassed, we see the raw Blender output. If Color Key requires ROP to apply the transparency mask, bypassing it means no mask. If the "No Blend" symptom means "Background Only", it might be that `0xF0` (PATCOPY) selects Pattern (Pipe 0/Background) if the Blender output is not correctly routed when bypassed? Or maybe the user meant "I see the background where I expected the masked source".

## Fix
- Modified `g2d_rcq_build_bld` in `sunxi-g2d-rcq-builders.c`.
- When `ck_enable` is true, we now:
  1. Set `ROP_CTL` to `0x00000000` (Enable ROP, disable bypass).
  2. Set `CH3_INDEX0` to `0x00061080` (Standard Copy Pattern).
- This ensures the ROP unit is active and can apply the Color Key mask to the Blender output.

## Verification
- The user should re-test the `blend` + `mask` operation.
- Verify if the output now shows the blended source with transparent regions where the key matches.
