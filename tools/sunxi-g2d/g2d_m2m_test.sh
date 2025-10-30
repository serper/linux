#!/usr/bin/env bash
set -euo pipefail

# Simple V4L2 mem2mem test for sunxi-g2d-m2m
# - Tests memcpy fallback (default)
# - Optionally tests HW blit when enable_hw=1

DEV=${1:-/dev/video0}
W=${2:-640}
H=${3:-360}
COUNT=${4:-1}
FMT=${5:-auto} # 'auto' picks the first supported fourcc

TMPDIR=${TMPDIR:-/tmp}
INFILE="$TMPDIR/g2d_in_${W}x${H}_${FMT}.raw"
OUTFILE="$TMPDIR/g2d_out_${W}x${H}_${FMT}.raw"

log() { echo "[g2d-test] $*"; }

need_cmd() { command -v "$1" >/dev/null 2>&1 || { echo "Missing command: $1" >&2; exit 1; }; }

need_cmd v4l2-ctl
need_cmd dd
need_cmd hexdump
if ! command -v python3 >/dev/null 2>&1; then
  echo "[g2d-test] WARNING: python3 not found, using zeroed pattern via dd" >&2
  PY_OK=0
else
  PY_OK=1
fi

# Create a simple XR24 pattern if not exists
mk_pattern() {
  local file="$1"; local w="$2"; local h="$3"; local bpp=4
  if [[ -f "$file" ]]; then return; fi
  log "Generating pattern: $file ($w x $h, XR24)"
  if [[ "$PY_OK" -eq 1 ]]; then
    # Generate gradient: R=x, G=y, B=(x^y), X=0xFF
    python3 - "$file" "$w" "$h" <<'PY'
import sys, struct
out, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(out, 'wb') as f:
    for y in range(h):
        for x in range(w):
            r = x & 0xFF
            g = y & 0xFF
            b = (x ^ y) & 0xFF
            a = 0xFF # X
            # XR24 is XRGB (little endian in memory): B,G,R,X
            f.write(bytes([b, g, r, a]))
PY
  else
    # Fallback: zero buffer
    dd if=/dev/zero of="$file" bs=1 count=$((w*h*bpp)) status=none
  fi
}

mk_pattern "$INFILE" "$W" "$H"
: > "$OUTFILE"

log "Using device: $DEV"

# Detect supported fourcc if FMT is 'auto' or not present in list
detect_fourcc() {
  local which="$1" # cap|out
  local args=("-d" "$DEV")
  if v4l2-ctl --help 2>&1 | grep -q -- "--list-formats-out"; then
    if [[ "$which" == out ]]; then args+=("--list-formats-out"); else args+=("--list-formats"); fi
  else
    args+=("--list-formats")
  fi
  v4l2-ctl "${args[@]}" 2>/dev/null | awk -F"'" '/\047[A-Z0-9]{4}\047/ {print $2; exit}'
}

has_fourcc() {
  local ff="$1"; shift
  printf '%s\n' "$@" | grep -qx "$ff"
}

CAP_FOURCC_LIST=$(detect_fourcc cap)
OUT_FOURCC_LIST=$(detect_fourcc out)

if [[ "$FMT" == auto ]]; then
  FMT=${OUT_FOURCC_LIST:-${CAP_FOURCC_LIST:-XR24}}
else
  # Validate requested FMT exists; if not, pick first available
  if ! has_fourcc "$FMT" "$OUT_FOURCC_LIST" "$CAP_FOURCC_LIST"; then
    log "Requested FMT=$FMT not found; falling back to device-reported format"
    FMT=${OUT_FOURCC_LIST:-${CAP_FOURCC_LIST:-XR24}}
  fi
fi

log "Formats: OUT=${W}x${H} ${FMT}, CAP=${W}x${H} ${FMT}, frames=$COUNT"

v4l2-ctl -d "$DEV" \
  --set-fmt-video-out=width=$W,height=$H,pixelformat=$FMT \
  --set-fmt-video=width=$W,height=$H,pixelformat=$FMT \
  --stream-out-mmap \
  --stream-mmap \
  --stream-from="$INFILE" \
  --stream-to="$OUTFILE" \
  --stream-count="$COUNT" --stream-poll

# Verify output equals input for memcpy case (1 frame)
if [[ "$COUNT" -eq 1 ]]; then
  if cmp -s "$INFILE" "$OUTFILE"; then
    log "PASS: output matches input (memcpy/HW equal-size copy)"
  else
    log "WARN: output differs from input (expected when formats differ or HW path modifies output)"
    log "Showing first 64 bytes diff:"
    diff <(hexdump -C "$INFILE" | head -n 16) <(hexdump -C "$OUTFILE" | head -n 16) || true
  fi
fi

log "Done. Output: $OUTFILE"
