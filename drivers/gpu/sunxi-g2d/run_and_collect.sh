#!/usr/bin/env zsh
# run_and_collect.sh
# Copia el binario al dispositivo, ejecuta el demo en modo --pattern
# durante N segundos y recupera los dumps /tmp/*.raw y el log.
# Uso: ./run_and_collect.sh <user@device> [remote_dir] [runtime_seconds]

set -euo pipefail

USER_HOST="${1:-spuc@10.42.0.1}"
REMOTE_DIR="${2:-/home/spuc}"
RUNTIME="${3:-12}"
LOCAL_BIN="demo-bouncing-ball"
REMOTE_BIN="$REMOTE_DIR/${LOCAL_BIN}"

echo "[run_and_collect] user_host=$USER_HOST remote_dir=$REMOTE_DIR runtime=${RUNTIME}s"

if [ ! -x "${LOCAL_BIN}" ]; then
  if [ -f "${LOCAL_BIN}" ]; then
    echo "Info: ${LOCAL_BIN} exists but is not executable. Will still copy it." >&2
  else
    echo "ERROR: ${LOCAL_BIN} not found in current directory: $(pwd)" >&2
    exit 2
  fi
fi

# Copy binary
echo "[run_and_collect] copying ${LOCAL_BIN} -> ${USER_HOST}:${REMOTE_BIN}"
scp -q "${LOCAL_BIN}" "${USER_HOST}:${REMOTE_BIN}"

# Run remote with timeout and background logging
echo "[run_and_collect] running remote: timeout ${RUNTIME}s ${REMOTE_BIN} --pattern"
ssh -T "${USER_HOST}" <<EOF
  set -e
  cd "${REMOTE_DIR}"
  timeout ${RUNTIME}s "${REMOTE_BIN}" --pattern > demo_pattern.log 2>&1 || true
  echo "[run_and_collect-remote] finished (exit code in demo_pattern.log tail)"
  ls -l /tmp/temp_after_fill.raw /tmp/fb_before_blit.raw /tmp/fb_after_blit.raw /tmp/fb_after_blit2.raw 2>/dev/null || true
  tail -n 40 demo_pattern.log || true
EOF

# Small wait to ensure files are flushed
sleep 1

# Files to retrieve
FILES=(/tmp/\*.raw /home/spuc/demo_pattern.log)

OUT_DIR="./collected_$(date +%s)"
mkdir -p "$OUT_DIR"

for f in "${FILES[@]}"; do
  echo "[run_and_collect] attempting to copy ${USER_HOST}:$f -> $OUT_DIR/"
  scp -q "${USER_HOST}:$f" "$OUT_DIR/" || echo "[run_and_collect] not found: $f"
done

# convert fb_*.raw files to png
for rawfile in "$OUT_DIR"/fb_*.raw; do
  if [ -f "$rawfile" ]; then
    pngfile="${rawfile%.raw}.png"
    echo "[run_and_collect] converting $rawfile -> $pngfile"
    convert -size 800x960 -depth 8 BGRA:"$rawfile" "$pngfile" || true
  fi
done

# convert temp_after_fill.raw to png
for rawfile in "$OUT_DIR"/temp_*.raw; do
  if [ -f "$rawfile" ]; then
    pngfile="${rawfile%.raw}_temp.png"
    echo "[run_and_collect] converting $rawfile -> $pngfile"
    convert -size 800x480 -depth 8 BGRA:"$rawfile" "$pngfile" || true
  fi
done

echo "[run_and_collect] results saved to $OUT_DIR"
ls -lh "$OUT_DIR" || true

echo "Done. Inspect the files in $OUT_DIR (hexdump -C or convert)"
