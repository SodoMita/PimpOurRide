# ChromeDrive

![ChromeDrive cockpit](docs/hero.webp)

A native Wayland compositor styled like a pimped-out car cockpit. You sit in the
driver's seat, first-person. Dashboard buttons — picked with real 3D raycasts,
not HTML — launch genuine Wayland applications, and **each app gets its own
spatial monitor** mounted around the windshield.

This is a real compositor: it implements `wl_compositor`, `wl_subcompositor`,
`xdg-shell` (toplevels + popups), `wl_seat` (pointer + xkbcommon keyboard),
`wl_data_device_manager` (in-session clipboard), `zxdg_decoration_manager_v1`,
`zwp_linux_dmabuf_v1` (GPU zero-copy buffer import), and one `wl_output`
global per virtual monitor. Any well-behaved Wayland client can connect to it
and will appear on its own screen.

It runs two ways:

- **`--headless` (default): no X server, no window, anywhere.** The GL context
  is a surfaceless EGL context (no window system at all); a per-frame
  `glReadPixels` feeds an embedded [libvncserver](https://libvnc.org/) RFB
  server, so the whole cockpit — compositor and every client window inside it
  — is visible over VNC. `tools/viewer` bridges that to a browser via
  websockify + noVNC.
- **`--windowed`: a real SDL2/GLX window on an X server**, kept only as a
  local dev/debug path with eyes on a real display. `zwp_linux_dmabuf_v1` is
  **not** advertised here — see "GPU / dma-buf" below for why.

## What's real vs. simulated

- **Real**: the Wayland protocol implementation, OpenGL scene, 3D raycast
  picking for buttons/screens, input routing to actual client processes, the
  terminal (`foot`) and the three bundled demo apps (Files, Audio UI, Meters —
  all genuine `wl_shm` clients speaking real Wayland, source in
  `clients/demo.c`), the headless EGL+VNC pipeline, and the
  `zwp_linux_dmabuf_v1` GPU-import code path (real EGLImage calls — see
  caveat below). The garage beyond the windshield is real 3D geometry now,
  not the flat `assets/garage.webp` backdrop quad it used to be — see
  “Garage3D” below.
- **Limitations in this build**: no layer-shell, Xwayland, or DRM/KMS backend
  — it never owns real display hardware directly, it's always either nested
  (`--windowed`) or headless-with-VNC-output (`--headless`); 8 virtual
  monitors max; buffers capped at 16 MiB/64 MiB total per surface for safety;
  single input seat, no touch/tablet.
- The "Audio" app is explicitly a UI/interaction demo — it does not play audio.
  "Meters" reads real `/proc/stat` and `/proc/meminfo` for CPU/memory, it is
  not simulated vehicle telemetry.

## GPU / dma-buf

`zwp_linux_dmabuf_v1` (protocol version 3) is implemented for real: a client's
dma-buf fd is imported with `eglCreateImageKHR(..., EGL_LINUX_DMA_BUF_EXT, ...)`
and bound to a GL texture with `glEGLImageTargetTexture2DOES` — a genuine
zero-copy GPU import, the same mechanism wlroots/Mutter/KWin use for
GPU-accelerated clients (most browsers, Electron, `mpv --gpu`, etc). It's only
advertised on the `--headless` EGL backend, because `EGLImage` does not
interoperate with a GLX-created context, only with an EGL-created one — so
`--windowed` (SDL2/GLX) intentionally never advertises it.

**Honesty check on this specific sandbox/build environment:** it has no
`/dev/dri` and no `/dev/udmabuf`, so nothing running in it can actually
*produce* a dma-buf fd to hand to this code. The negotiation path (format +
modifier advertisement) and the structural-validation/protocol-error path are
both exercised by `tests/protocol_probe.c` and pass. The "is this fd real GPU
memory" success path cannot be round-trip verified here — and, interestingly,
this sandbox's software EGL driver (llvmpipe) doesn't reliably reject bogus
fds either (it imports lazily and only fails on a genuinely malformed format);
a real GPU/DRM driver is stricter. On real hardware (a machine with `/dev/dri`
and a GPU-accelerated Wayland client), this same binary and code path is
expected to work unmodified — it's standard EGL, not sandbox-specific code.
The compositor's own rendering also goes through this same EGL/GL stack, so on
a host with a real GPU driver it will render on the GPU automatically (no
code change needed); here it falls back to Mesa's llvmpipe software
rasterizer because that's all the sandbox exposes.

## Build

```sh
sudo apt-get install -y libsdl2-dev libwayland-dev wayland-protocols \
  libgl-dev libegl-dev libgles-dev libgles2-mesa-dev mesa-common-dev libxkbcommon-dev libwebp-dev \
  libcairo2-dev libdrm-dev libvncserver-dev foot
make -j"$(nproc)"
make test        # math/raycast + config-store tests + GUI selftest
make garage3d    # optional: the standalone GLES 3.1 garage client (below)
make garage3d-test
```

`make bin/protocol-probe` builds the Wayland protocol conformance test; run it
against a live `chromedrive` instance (see below).

## Run

```sh
./bin/chromedrive --demo            # headless (default): EGL + embedded VNC server, no X at all
./bin/chromedrive --demo --windowed # dev/debug: a real SDL2 window on an X server
```

Either way it launches a terminal, Files, and Audio demo as real Wayland
clients, each bound to its own virtual monitor. Launch more clients targeting
the socket it prints:

```sh
WAYLAND_DISPLAY=<printed-socket> foot &
WAYLAND_DISPLAY=<printed-socket> weston-terminal &
```

### Garage3D: the backdrop, rebuilt as a 3D Wayland client

![Garage3D client](docs/garage3d-client.webp)

`clients/garage3d/` is a standalone native Wayland client (raw
`libwayland-client` + `xdg-shell` + EGL/`wl_egl_window` + OpenGL ES 3.1, no
SDL/GLFW/toolkit) that rebuilds the old flat `assets/garage.webp` backdrop as an
explorable 3D scene: scene graph with parent-child hierarchies, perspective
camera, a directional light plus six attenuated point lights (one of them the
swinging work lamp, integrated as pendulum physics), bouncing/rolling balls,
420 rain streaks outside the far doorway, a procedural city skyline, and
pointer/keyboard/**touch** camera control (one-finger orbit, pinch zoom,
two-finger pan, double-tap reset). Animation is procedural + fixed-step
(120 Hz) physics, all delta-time based; rendering is throttled purely by
`wl_surface` frame callbacks. The same hall also replaced the compositor's
own background quad in `src/scene.c:environment()`, so looking around the
cockpit now has real parallax (`garage.webp` survives only in the rear-view
mirror).

```sh
make garage3d
./clients/garage3d/bin/garage3d                       # on weston/sway/GNOME/KDE/…
WAYLAND_DISPLAY=<printed-socket> ./clients/garage3d/bin/garage3d   # inside ChromeDrive
make -C clients/garage3d smoke                        # headless weston pixel check
```

Architecture, the full Wayland surface lifecycle, and the frame-callback
render loop are documented in
[`docs/garage3d-architecture.md`](docs/garage3d-architecture.md).
Note the touch asymmetry: the *client* implements `wl_touch` fully, but the
ChromeDrive *compositor* does not yet advertise or forward touch (its seat is
pointer+keyboard), so gestures work on touch-capable host compositors, not
yet through ChromeDrive itself.

### Configuration: the launch-menu editor & settings GUI

![Launch Menu editor](docs/config-launch-menu.webp)

`bin/chromedrive-config` is a native settings app (wl_shm + cairo, like the
demo clients) for both programs, built by plain `make`. In the cockpit it is
one click away: the slowly spinning **3D cog on the dashboard** (between the
steering wheel and Launch Control) launches it, or focuses it if already
open. Its first tab is a
KDE-menu-editor-style **Launch Menu editor**: rename any of the six Launch
Control entries, change their icons/descriptions, point the four app slots
at arbitrary commands (e.g. `dolphin`, or Garage3D), set the `app_id` used
to focus a running instance instead of relaunching, toggle entries on/off,
and choose the `--demo` autostart set. The other tabs cover every Garage3D
tunable (neon colours, fog, rain, physics…) and every compositor setting
(size/FPS/FOV, underglow, 3D-vs-texture backdrop, VNC port…).

It edits `~/.config/chromedrive/chromedrive.conf` and
`~/.config/garage3d/garage3d.conf` (plain INI, hand-editable). Both programs
**hot-reload**: drag a slider or rename a button and the running cockpit /
3D scene follows within ~half a second. Full schema, `%root%` command
templates, precedence (`defaults < file < env < CLI`) and live-vs-restart
details: [`docs/configuration.md`](docs/configuration.md).

```sh
./bin/chromedrive-config                          # any Wayland session
./bin/chromedrive --exec "'$PWD/bin/chromedrive-config'"   # inside the cockpit
./bin/chromedrive-config --selftest               # headless widget/file test (make test)
```

### Watching the headless session (VNC / browser preview)

`--headless` starts an RFB server on `--vnc-port` (default 5900) listening on
all interfaces. Point any VNC client at it directly:

```sh
vncviewer HOST:5900
```

…or bridge it to a browser with the bundled noVNC viewer:

```sh
./tools/prepare-viewer.sh     # stages noVNC's static assets into tools/viewer once
websockify --web=tools/viewer 0.0.0.0:8080 127.0.0.1:5900
# then open http://HOST:8080/
```

### Controls

| Input | Action |
|---|---|
| Middle-drag (mouse or VNC) | Look around (first-person) |
| Click a screen | Focus it — keyboard/pointer now go to that app |
| Scroll over a screen | Scroll inside that app |
| Scroll elsewhere | Zoom (narrow the field of view) |
| Click a dashboard button | Launch/focus that app |
| Click the dashboard cog | Open Settings (`chromedrive-config`) |
| Shift + dashboard button | Force a new instance |
| F1 | Help overlay |
| F2 | Reset to the driver's seat view |
| F3 | Toggle a tight focus zoom on the active screen |
| F4 | Cycle underglow color (violet / ice blue / acid lime) |
| F6 | Cycle to the next connected screen |
| F11 | `--windowed` only: toggle fullscreen |
| F12 | Save a screenshot to `screenshots/` |
| Ctrl+Alt+Enter | Open a new terminal |
| Ctrl+Alt+Q | Quit |

Over VNC, keyboard input covers typing, navigation keys, function keys and
modifiers (see `keysym_to_evdev` in `src/backend_headless.c`); key auto-repeat
is handled client-side (by each app's own xkb repeat timer), same as a real
compositor — holding a key sends one press/release pair, not a flood of events.

A neon crosshair reticle is drawn directly into every frame at the pointer
position (brightening/widening over a clickable dashboard button) — there is
no OS cursor to fall back on in `--headless` mode (no X, no window; a raw VNC
client without cursor-shape-update support would otherwise show nothing), so
the compositor draws its own, in both backends, for a consistent look.

### CLI flags

`--demo`, `--socket NAME`, `--exec COMMAND` (repeatable), `--size WxH`,
`--fps N`, `--headless` (default)/`--windowed`, `--vnc-port N`,
`--fullscreen` (`--windowed` only), `--exit-after SECONDS`,
`--screenshot PATH`.

Launch commands for the dashboard buttons can be overridden with the
`CHROMEDRIVE_TERMINAL`, `CHROMEDRIVE_FILES`, `CHROMEDRIVE_AUDIO`, and
`CHROMEDRIVE_METERS` environment variables.

## Layout

```
include/chrome.h, math3d.h, cdconfig.h  shared types, ray/plane math, config store API
src/math3d.c                 camera rays, plane intersection, monitor layout
src/wayland.c                 the actual compositor protocol implementation
src/dmabuf.c                  zwp_linux_dmabuf_v1: real EGLImage GPU buffer import
src/scene.c                   OpenGL cockpit, monitors, buttons, picking, 3D garage backdrop
src/ui.c                      cairo-rendered textures: gauges, HUD, labels
src/main.c                    CLI parsing, event loop, process spawn (backend-agnostic)
src/backend_sdl.c             --windowed: SDL2 window + GLX context + SDL input
src/backend_headless.c        --headless (default): surfaceless EGL + embedded VNC + RFB input
clients/demo.c                 three independent real Wayland clients
clients/config.c               chromedrive-config: launch-menu editor + settings GUI
clients/garage3d/              Garage3D: native Wayland + GLES 3.1 client (the garage in 3D)
src/cdconfig.c                 shared INI config store (compositor + GUI; Garage3D has its own loader)
docs/garage3d-architecture.md  Garage3D architecture + Wayland lifecycle documentation
docs/configuration.md          config files, launch-menu editing, hot-reload semantics
tests/test_math.c             raycast/picking unit tests
tests/test_config.c           INI store round-trip tests
tests/protocol_probe.c        end-to-end Wayland protocol conformance test (incl. dma-buf)
tools/viewer/                 noVNC web viewer for the headless VNC server (see prepare-viewer.sh)
```

## Known limitations / next steps

- No DRM/KMS backend — always nested (`--windowed`) or VNC-served
  (`--headless`), never boots real displays directly.
- No layer-shell (so panels/launchers built for wlroots-style shells won't run).
- dma-buf import only works on `--headless` (EGL); not on `--windowed` (GLX) —
  by design, see "GPU / dma-buf" above.
- Single input seat, no touch, no tablet *in the compositor* (the Garage3D
  client implements `wl_touch` gestures, but ChromeDrive does not yet
  advertise/forward touch to clients — see “Garage3D” above).
- 8-monitor cap is fixed at compile time (`CD_MAX_MONITORS`).
