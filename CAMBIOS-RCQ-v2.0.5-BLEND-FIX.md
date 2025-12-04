# Fix for G2D_CMD_BLEND with Color Key

## Issue
The user reported that `G2D_CMD_BLEND` works correctly when used alone, but fails (no operation or incorrect output) when `color_key_enable` (mask) is also enabled.

## Investigation
1. Analyzed `sunxi_g2d_cmd_blend` in `sunxi-g2d-main.c`.
2. Analyzed `sunxi_g2d_do_blend_rcq` in `sunxi-g2d-main.c`.
3. Analyzed `g2d_rcq_build_bld` in `sunxi-g2d-rcq-builders.c`.

## Findings
1. **Format Swap Bug**: In `sunxi_g2d_do_blend_rcq`, the format assignment for `fmt_p0` and `fmt_p1` was inverted.
   - `p0` corresponds to V0 (Background/Dest in default mode).
   - `p1` corresponds to UI2 (Foreground/Source in default mode).
   - The code was assigning `src_hw_fmt` to `p0` and `dst_hw_fmt` to `p1`.
   - This causes incorrect CSC (Color Space Conversion) configuration if formats differ (e.g. YUV source on RGB dest).
   - While this might be benign for RGB/RGB blending, it is a critical bug for YUV/RGB blending.

2. **Color Key Configuration**: The `g2d_rcq_build_bld` function configures `BLD_KEY_CTL` and `BLD_KEY_CON` correctly for RGB matching.
   - Added debug prints to verify the configuration at runtime.

## Fix
- Corrected the assignment of `fmt_p0` and `fmt_p1` in `sunxi_g2d_do_blend_rcq`.
- Added debug logging in `g2d_rcq_build_bld` to trace Color Key parameters.

## Verification
- The user should re-test the `blend` + `mask` operation.
- If the issue persists (e.g. for RGB/RGB cases), further investigation into `ROP_CTL` interaction with Color Key might be needed.
