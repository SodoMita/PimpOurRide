#!/bin/sh
# Browser bridge for the compositor's own embedded VNC server (backend_headless.c).
# ChromeDrive --headless IS the VNC server now (no x11vnc, no Xvfb involved) —
# this script only stages noVNC's static web client and prints the websockify
# command that bridges the raw RFB protocol to a WebSocket a browser can use.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NOVNC=${NOVNC_DIR:-/usr/share/novnc}
[ -f "$NOVNC/core/rfb.js" ] || { echo 'Install novnc and websockify first.' >&2; exit 1; }
ln -sfn "$NOVNC/core" "$ROOT/tools/viewer/core"
ln -sfn "$NOVNC/vendor" "$ROOT/tools/viewer/vendor"
printf 'Viewer ready. With chromedrive --headless --vnc-port 5900 running, bridge it with:\nwebsockify --web=%s/tools/viewer 0.0.0.0:8080 127.0.0.1:5900\nThen open http://HOST:8080/ in a browser.\n' "$ROOT"
