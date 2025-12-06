# Fix for G2D_CMD_MASK Argument Mismatch

## Issue
The user reported a timeout/crash (`-110`) when using `G2D_CMD_MASK` with an external 8bpp alpha mask.
The log showed `RCQ execution timeout`.

## Investigation
1. Analyzed `sunxi_g2d_cmd_mask` in `sunxi-g2d-main.c`.
2. Found a critical argument mismatch in the call to `sunxi_g2d_do_blend_rcq`.
   - The function expects: `ck_enable`, `ck_on_src`, `ck_min`, `ck_max`, `csc_state`.
   - The code was passing: `mask_alpha`, `color_key_enable`, `true`, `color_key_min`, `color_key_max`, `&csc_state`.
   - This resulted in **6 arguments** being passed for **5 parameters**.
   - Crucially, `color_key_max` (a `u32` value) was being passed as the `csc_state` pointer.
   - When `sunxi_g2d_do_blend_rcq` (or its callees) tried to access `csc_state`, it likely caused a memory access violation or undefined behavior, leading to the timeout/crash.

## Fix
- Removed the extra `mask_alpha` argument.
- Corrected the argument mapping:
  - `ck_enable` <- `job->data.blit.color_key_enable`
  - `ck_on_src` <- `true` (Always key on source for MASK command)
  - `ck_min` <- `job->data.blit.color_key_min`
  - `ck_max` <- `job->data.blit.color_key_max`
  - `csc_state` <- `&job->csc_state`

## Verification
- The user should re-test the `G2D_CMD_MASK` operation with 8bpp alpha.
- This should resolve the timeout and correct the logic for both Color Keying (`mask_alpha=0`) and Alpha Masking (`mask_alpha=1`).
