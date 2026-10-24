# Garage3D

A native Wayland client that rebuilds ChromeDrive's flat `assets/garage.webp`
backdrop as a real-time, explorable 3D scene: raw `libwayland-client` +
`xdg-shell` + EGL on a `wl_egl_window` + OpenGL ES 3.1. **No SDL, GLFW, GTK,
Qt, or any windowing abstraction.** It runs on weston/sway/GNOME/KDE/phosh
and inside ChromeDrive itself (as an app on a cockpit monitor).

![Garage3D under weston](../../docs/garage3d-client.webp)

Full design notes — scene graph, physics, Wayland surface lifecycle,
frame-callback loop, input/touch, verification log — live in
[`docs/garage3d-architecture.md`](../../docs/garage3d-architecture.md).

## Build

```sh
sudo apt-get install -y build-essential pkg-config libwayland-dev \
  wayland-protocols libegl-dev libgles-dev libgles2-mesa-dev \
  mesa-common-dev libxkbcommon-dev libwebp-dev
make                    # -> bin/garage3d
make test               # headless math/physics/scene-graph unit tests
make smoke              # renders under weston/headless + pixel checks (needs weston, python3-pil)
```

From the repo root the same is `make garage3d` / `make garage3d-test`.

## Run

```sh
./bin/garage3d                                  # $WAYLAND_DISPLAY session
./bin/garage3d --fov 55 --fullscreen
WAYLAND_DISPLAY=<chromedrive-socket> ./bin/garage3d   # inside ChromeDrive
```

| Input | Action |
|---|---|
| Left-drag / 1-finger drag | Orbit |
| Right/middle-drag / 2-finger drag | Pan |
| Wheel / pinch | Zoom |
| Double-tap / `R` | Reset view |
| `WASD`+`QE`, `+`/`-` | Move, field of view |
| `Space`, `F`, `Esc` | Pause physics, fullscreen, quit |

## Settings

Every tunable (neon colours, fog, rain, gravity, lamp swing, camera speeds,
window size/scale/fullscreen, …) lives in
`~/.config/garage3d/garage3d.conf` — edit it by hand or with the project GUI
(root `make` → `bin/chromedrive-config`, Garage3D tab). The file is
**hot-reloaded** while running: drag a slider in the GUI and the scene
follows. Precedence: built-in defaults < file < CLI flags (`--config PATH`,
`--no-config` to override/disable). Full schema:
[`docs/configuration.md`](../../docs/configuration.md).

Layout: `src/wayland_client.c` (protocol + EGL + input, incl. `wl_touch`),
`src/renderer.c` (GLES programs/VAOs, draw order), `src/scene.c` (graph,
camera, lights), `src/animation.c` (procedural tracks + 120 Hz fixed-step
pendulum/ball/rain physics), `src/config.c` (typed settings loader),
`src/gmath.c`, `src/mesh.c`, `shaders/*` (10 GLSL files loaded from disk;
see `--shader-dir` / `$GARAGE3D_SHADER_DIR`). Only `wayland_client.c`
includes Wayland headers; the scene/animation/math modules are pure C and
unit-tested in `tests/`.
