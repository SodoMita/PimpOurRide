#!/bin/sh
# One-command fullstack development session: native C backend + static noVNC UI.
# No X server, screenshot encoder, Node process, database, or second compositor
# is involved. websockify only translates WebSocket framing to the compositor's
# existing RFB stream; Wayland input continues through backend_headless.c.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
WEB_PORT=${WEB_PORT:-8080}
VNC_PORT=${VNC_PORT:-5900}
SIZE=${SIZE:-1600x1000}

make all
./tools/prepare-viewer.sh

set -- --headless --vnc-port "$VNC_PORT" --size "$SIZE" "$@"
if [ "${CHROMEDRIVE_DEMO:-1}" != 0 ]; then set -- --demo "$@"; fi
./bin/chromedrive "$@" &
compositor_pid=$!
cleanup() {
 kill "$compositor_pid" 2>/dev/null || true
 wait "$compositor_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
sleep 1
if ! kill -0 "$compositor_pid" 2>/dev/null; then
 wait "$compositor_pid"
 exit 1
fi
printf 'ChromeDrive web remote desktop: http://0.0.0.0:%s/ (RFB 127.0.0.1:%s)\n' "$WEB_PORT" "$VNC_PORT"
# Foreground process is intentional: service managers and Ctrl+C supervise the
# bridge, while the EXIT trap always reaps the native compositor and its demos.
websockify --web="$ROOT/tools/viewer" "0.0.0.0:$WEB_PORT" "127.0.0.1:$VNC_PORT"
