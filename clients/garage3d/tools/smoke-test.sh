#!/bin/sh
# smoke-test.sh - end-to-end check: real compositor -> garage3d -> pixels.
#
# Starts weston on its headless backend (no display/GPU required, Mesa
# llvmpipe is enough), runs garage3d against it for a fixed number of
# frames, captures a WebP via --screenshot (WebP is the only image format
# anything in this repo writes, see AGENTS.md), and asserts with Python/PIL
# that the frame is (a) not black and (b) contains both neon colours - i.e.
# the Wayland/EGL path, the shaders and the scene all actually produced the
# teal-left / magenta-right garage, not merely "a window that didn't crash".
#
#   make -C clients/garage3d smoke        # or: FRAMES=90 sh tools/smoke-test.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${GARAGE3D_BIN:-$ROOT/bin/garage3d}
FRAMES=${FRAMES:-45}

[ -x "$BIN" ] || { echo "smoke: $BIN not built (make first)" >&2; exit 2; }
command -v weston >/dev/null 2>&1 || { echo "smoke: weston not installed (apt install weston)" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "smoke: python3 required for pixel checks (apt install python3-pil)" >&2; exit 2; }
python3 -c 'import PIL.Image' >/dev/null 2>&1 || { echo "smoke: Python PIL required to decode the WebP capture (apt install python3-pil)" >&2; exit 2; }

TMP=$(mktemp -d)
WESTON_PID=""
cleanup() {
    [ -n "$WESTON_PID" ] && kill "$WESTON_PID" 2>/dev/null || true
    [ -n "$WESTON_PID" ] && wait "$WESTON_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

export XDG_RUNTIME_DIR="$TMP/rt"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

weston --backend=headless-backend.so --renderer=gl --width=960 --height=540 \
    --socket=garage3d-smoke --idle-time=0 --log="$TMP/weston.log" &
WESTON_PID=$!

i=0
while [ ! -S "$XDG_RUNTIME_DIR/garage3d-smoke" ]; do
    i=$((i + 1))
    [ "$i" -lt 100 ] || { echo "smoke: weston did not create its socket" >&2; cat "$TMP/weston.log" >&2; exit 1; }
    sleep 0.1
done

WAYLAND_DISPLAY=garage3d-smoke timeout 30 "$BIN" \
    --frames "$FRAMES" --screenshot "$TMP/frame.webp" --screenshot-frame 35

python3 - "$TMP/frame.webp" <<'PY'
import sys
from PIL import Image

path = sys.argv[1]
im = Image.open(path).convert("RGB")
w, h = im.size
data = im.tobytes()
assert len(data) == w * h * 3, f"short image: {len(data)} bytes for {w}x{h}"
bright = teal = magenta = 0
for i in range(0, len(data), 3):
    r, g, b = data[i], data[i + 1], data[i + 2]
    if r + g + b > 180:
        bright += 1
    if g > 70 and b > 70 and r < g:
        teal += 1
    if r > 70 and b > 70 and g < r:
        magenta += 1
n = w * h
print(f"smoke: {w}x{h} bright={bright} ({100*bright/n:.1f}%) teal={teal} magenta={magenta}")
assert bright > n * 0.02, "frame is (almost) black - nothing rendered?"
assert teal > 400, "no teal neon found (left wall missing?)"
assert magenta > 400, "no magenta neon found (right wall missing?)"
print("PASS: garage3d rendered the split-neon garage under weston/headless")
PY
