# Fix for G2D_CMD_BLEND with Color Key (Attempt 3)

## Issue
The user reported that the previous fix (enabling ROP) caused both blend and mask to fail.
The original issue was that mask was not working.

## Investigation
1. **ROP Bypass**: Reverting the ROP change is necessary because it broke the blending. The default `0xF0` (Bypass) seems correct for blending operations.
2. **Pipe Selection Logic**:
   - The code was setting `dir = 0` when `ck_on_ui2` (Key on Foreground/Pipe 1) was true.
   - Assuming `dir` maps to the pipe index (0=Pipe 0, 1=Pipe 1), this was selecting Pipe 0 (Background) for keying.
   - This explains why the mask was not working on the source image.
   - It also explains why the blend was working (but without mask) in the original report: we were keying the background, which likely didn't match the key color, so no transparency was added, and the blend proceeded normally.

## Fix
- Reverted the ROP configuration change in `g2d_rcq_build_bld`.
- Corrected the `dir` selection logic:
  - If `ck_on_ui2` is true, set `dir = 1` (Select Pipe 1).
  - If `ck_on_ui2` is false, set `dir = 0` (Select Pipe 0).

## Verification
- The user should re-test the `blend` + `mask` operation.
- This should now correctly apply the color key to the source (foreground) layer.
