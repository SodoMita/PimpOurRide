# Electric hover-car + browser remote desktop

ChromeDrive 0.3 keeps the compositor in native C and adds a complete electric
hover-car rig to the existing first-person Wayland cockpit. The browser is a
remote client of that compositor, not a replacement renderer.

## Fullstack data flow

```text
Real Wayland apps (foot, demo clients, other native programs)
        │  wl_shm / linux-dmabuf, xdg-shell, wl_seat
        ▼
libwayland-server ──► per-app GL textures on spatial monitors
        │                         ▲
        │ pointer/keyboard/axis   │ one current GL context, one thread
        │                         │
RFB input ─► backend_headless ──► scene + spline EV + fixed-step physics
                                      │
                                      ▼
                           surfaceless EGL color/depth FBO
                                      │ glReadPixels + vertical flip
                                      ▼
                             embedded libvncserver RFB
                                      │ websockify (protocol bridge only)
                                      ▼
                         static noVNC tools/viewer/index.html
```

`tools/run-web.sh` is the fullstack launcher. It builds the C programs, stages
the operating system's noVNC assets, starts the headless compositor, and serves
the browser UI on the same origin as its WebSocket bridge. There is no Node
runtime, frontend build, X server, screenshot-video encoder, or web-based
Wayland protocol implementation.

```sh
sudo apt-get install -y libsdl2-dev libwayland-dev wayland-protocols \
  libgl-dev libegl-dev libxkbcommon-dev libwebp-dev libcairo2-dev \
  libdrm-dev libvncserver-dev novnc websockify foot
./tools/run-web.sh
# http://localhost:8080/

WEB_PORT=9000 VNC_PORT=5901 SIZE=1280x800 ./tools/run-web.sh
CHROMEDRIVE_DEMO=0 ./tools/run-web.sh --no-config
```

The RFB/HTTP development endpoints do not add authentication or TLS. Put them
behind an authenticated TLS reverse proxy and firewall before exposing them to
an untrusted network. Do not place credentials in the repository.

## Spline-built electric vehicle

`src/vehicle_scene.c` renders a complete concept EV in car-local coordinates.
Longitudinal body stations and roof stations are interpolated with Catmull-Rom
splines. This representation was chosen because it:

- interpolates the designer's control stations rather than merely approaching
  them, so the nose, cabin, and deck remain anchored;
- gives continuous tangents across panel segments without a mesh dependency;
- keeps edits local, unlike changing a global polynomial;
- can be evaluated by both the renderer and small native unit tests.

The orange high-voltage harnesses and blue coolant loop use the same spline
routine in 3D, including duplicated endpoint control points for exact connector
anchoring. No wheels or tire meshes are generated.

The model includes the principal production-EV systems rather than only a body
shell:

- structural skateboard chassis and an 82 kWh pack with twelve cell modules,
  bus bars, contactor/fuse junction hardware, and service covers;
- front and rear permanent-magnet drive units, reduction housings, inverters,
  and half-shaft-like housings repurposed for the hover platform;
- four emissive magnetic hover pods (visual geometry, not four tire solvers);
- orange high-voltage routing, onboard charger, charge port, high-voltage to
  12 V DC/DC converter, and a low-voltage battery;
- front heat exchanger, coolant reservoir, and a separate thermal loop;
- panoramic fastback glazing, flush camera mirrors and roof sensor strip, and
  thin curved LED signatures.

The exterior is a closed superellipse-style loft through the same body spline
stations, with a continuous dark panoramic canopy rather than separate upright
roof/bumper pieces. Its rounded single-frame nose and metallic emblem are also
geometry. The driver view uses the same stations as an interior visibility
cutaway with transparent glazing, so neither the desktop nor the EV model is
hidden by the exterior treatment.

F5 cycles **driver → exterior concept → EV systems cutaway**. The cutaway removes
opaque body panels but retains their spline wire cage so the electrical and
thermal components can be inspected in place.

## Vehicle dynamics

`src/vehicle.c` is independent of GL and the Wayland server. It uses 120 Hz
fixed steps with a render-frame accumulator and a 250 ms catch-up cap. This is
important for reproducible acceleration and stable hover dynamics when
llvmpipe or a remote framebuffer slows presentation.

The model uses:

- signed longitudinal speed with a 360 kW power limit and low-speed force cap;
- aerodynamic and levitation drag, auxiliary power draw, state of charge, and
  motor temperature telemetry;
- service/regenerative braking with zero-crossing protection, plus reverse;
- a bounded kinematic-bicycle yaw response (no tire/contact-patch pretence);
- one spring/damper heave degree of freedom shared by the four visual pods;
- smoothed body roll/pitch and energy-absorbing garage-boundary contacts.

It is an interactive concept-car model, not a validated automotive safety or
homologation simulation. Pack energy is a `double`: at parking load, a single
120 Hz tick consumes only a few micro-kWh, which would round away if repeatedly
subtracted from a `float` near 69 kWh.

### One rig, exact picking

Cockpit geometry, dashboard buttons, the steering wheel, and all spatial
Wayland monitors stay in their original car-local coordinates. The vehicle
pose applies `T * Ry * Rx * Rz` to that complete rig. The garage remains in
world space. Rendering uses this transform directly; picking transforms the
camera ray by its exact inverse, preserving application-local UV coordinates
while the car moves. Exterior cameras are also defined in car-local space, so
they follow rather than drift.

## Steering and input ownership

F7 changes the owner of driving input. The native HUD is authoritative.

| Control | Client mode (default) | Drive mode |
|---|---|---|
| A/D or left/right | Sent to focused Wayland client | Steer |
| W/up | Sent to focused Wayland client | Accelerate |
| S/down | Sent to focused Wayland client | Brake while moving forward, reverse at rest |
| Space | Sent to focused Wayland client | Regenerative/service brake |
| Left-drag 3D wheel | Horizontal pointer-axis/scroll input to focused app | Steer |
| F5 | Driver/exterior/systems camera | Same |
| F8 | Recover pose without refilling battery/resetting odometer | Same |
| F2 | Return to driver camera | Same |

Client mode also commands a parking brake so manipulating an application cannot
leave the car accelerating. On mode changes and host focus loss, both Wayland
keys and compositor-local drive keys are released. Browser hold buttons use
pointer capture and release on pointer-up, pointer-cancel, lost capture, window
blur, and disconnect.

## Wayland protocol sequences that matter

### xdg toplevel mapping

The implemented sequence is intentionally strict:

1. Client creates `wl_surface`, `xdg_surface`, and `xdg_toplevel`.
2. Its initial bufferless commit receives `xdg_toplevel.configure(size,states)`
   followed by `xdg_surface.configure(serial)`.
3. Client acknowledges that serial with `ack_configure` before attaching its
   first real buffer. An unconfigured buffer is a protocol error, not a guess.
4. A later `wl_surface.commit` atomically applies pending buffer, scale,
   transform, input region, subsurface state, and frame callbacks.
5. The first textured commit maps the toplevel to one free spatial monitor and
   emits the appropriate `wl_surface.enter`/`wl_output` and keyboard focus
   events.

### Frame callback render-loop pattern

`wl_surface.frame` is pending surface state. It is **not** an immediate request
for a timer event:

1. A client requests `wl_surface.frame` for its next frame, normally before the
   commit carrying that frame's buffer.
2. `surface_frame()` creates a one-shot `wl_callback` and stores it in
   `pending_callbacks`.
3. `wl_surface.commit` moves it to the committed callback list along with the
   rest of that frame's state.
4. The compositor polls input, dispatches Wayland, advances fixed-step physics,
   renders, and presents/readbacks the FBO.
5. Only then does `server_frame_done()` send `wl_callback.done(time)` and destroy
   the one-shot resource. `wl_display_flush_clients()` makes the event visible.
6. The client renders again from that callback and requests exactly one new
   callback. This supplies backpressure and avoids a client queuing frames
   faster than the compositor can display them.

Sending `done` at request time would acknowledge the wrong frame. Requesting a
client callback after its buffer commit can instead attach pacing to an
unexpected later commit; the standalone Garage3D client documentation covers
that client-side ordering in more detail.

### Steering wheel as client input

`wl_pointer.axis` requires pointer focus; keyboard focus alone is insufficient.
Starting a client-mode wheel drag therefore sends a matching leave to the old
pointer surface, enters the active application's surface at its centre, then
uses the normal sequence for protocol v5+:

```text
wl_pointer.axis_source(wheel)
wl_pointer.axis_discrete(horizontal_scroll, detents)
wl_pointer.axis(horizontal_scroll, continuous_value)
wl_pointer.frame
```

High-frequency RFB/SDL motion is coalesced into detents before this sequence.
Older pointer resources receive the events supported by their version.

## EGL/GL context ownership

The default backend explicitly selects `EGL_PLATFORM_SURFACELESS_MESA`, binds
`EGL_OPENGL_API` before context creation, and makes a context current with
`EGL_NO_SURFACE` for draw/read. Its color+depth renderbuffer FBO is the output;
there is no window surface and therefore no `eglSwapBuffers` in this path.

Backend initialization precedes Wayland server dispatch. This ordering matters
because a client commit can upload `wl_shm` pixels with `glTexImage2D`, and a
linux-dmabuf commit can reference an `EGLImage` texture. Those are GL calls,
not protocol-only bookkeeping, and must run with a current context on the same
thread as the scene. The optional windowed backend remains SDL2/GLX and does
not advertise EGLImage dma-buf import.

Headless presentation binds the FBO, calls `glReadPixels`, flips GL's bottom-up
rows into RFB's top-down framebuffer, and marks the rectangle modified. During
shutdown, clients and scene textures are destroyed before the EGL context is
unbound and terminated. No physics code, browser code, or worker thread owns a
GL context.

## Validation

```sh
make clean && make -j"$(nproc)" all
make test
# Optional deeper native client/protocol checks:
make bin/protocol-probe
make garage3d-test
```

`tests/test_vehicle.c` checks spline interpolation, forward/reverse motion,
braking, steering coordinate signs, garage bounds, transform round trips,
hover/body finiteness, and identical results under different render cadences.
`tests/test_keymap.c` pins the exact RFB/X11-to-evdev table, including the
non-contiguous letter codes needed by WASD. Existing math/config tests and the
native settings selftest remain in `make test`. Runtime smoke testing uses the
real surfaceless EGL/VNC backend and can inject standard RFB key events—the
same path used by noVNC.
