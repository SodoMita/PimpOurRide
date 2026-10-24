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

`src/vehicle.c` and `src/vehicle_scene.c` evaluate and render a complete
wheel-less electric hover-car concept in car-local coordinates. Longitudinal
body stations (`9` control stations from `z = -5.60` to `+5.50`), transverse
cross-section profile control points (`5` control points from `v = 0` undertray
centreline through `0.25` lower chine, `0.55` widest fender crease, `0.84`
upper beltline shoulder, and `1.00` hood/deck spine), and `7` fastback canopy
stations are interpolated with bicubic Catmull-Rom splines (`vehicle_hull_point`
and `vehicle_canopy_point`). This representation was chosen because it:

- interpolates the designer's control stations rather than merely approaching
  them, so the nose, cabin, haunches, and Kammback tail remain anchored;
- computes analytical central-difference normals (`dp/dv × dp/du`) with exact
  centreline horizontal tangent enforcement (`normal.x = 0` at `v = 0` and
  `v = 1`) for seamless metallic paint highlights across `x = 0`;
- anchors every exterior trim element (`hull_offset_point`)—laser-matrix
  headlamps, full-width crimson OLED rear lightbar, camera mirror stalks, flush
  door handles, side rocker neon blades, hover-bay side gill louvers, hood aero
  extractors, and charge port door—directly onto the evaluated hull surface so
  zero components float detached outside the body;
- verifies in `tests/test_vehicle.c` via `vehicle_point_inside_envelope()` that
  every internal EV component sits strictly inside the hull and canopy envelope.

The orange high-voltage harnesses and blue coolant loop use the same spline
routine in 3D, including duplicated endpoint control points for exact connector
anchoring. No wheels, tires, or wheel arches are generated.

The model includes the principal 800 V hover-EV systems rather than only a body
shell:

- structural skateboard chassis and an 82 kWh pack with twelve cell modules,
  orange 800 V busbars, contactor/fuse junction hardware, and service covers;
- front (`160 kW`) and rear (`260 kW`) axial-flux PMSM drive units, helical
  reduction housings, finned SiC inverters, and magnetic vector-flux conduits
  feeding the hover bays;
- four flush-mounted underbody magnetic-levitation / plasma-ring hover nacelles
  recessed into the front and rear undertray aprons (with titanium stator vanes
  and animated ground-effect plasma rings hovering above the garage floor);
- orange high-voltage routing, onboard AC charger, flush charge port, high-voltage
  to 12 V DC/DC converter, and a 12 V auxiliary LiFePO4 battery;
- slanted front heat exchanger, translucent glycol reservoir, and a separate
  thermal loop;
- panoramic fastback electrochromic glazing, flush camera mirrors, roof LiDAR
  pod, and thin curved LED signatures.

`src/vehicle_seat.c` supplies both front seats as sculpted upholstery rather
than cuboids: 25-ring by 49-sample cushion/back lofts, crowned superellipsoid
bolsters and headrests, recessed perforated inserts, continuous piping,
stitches, recliner hardware, rails, and belt buckles. The same detailed seat
geometry is used in the driver and EV-cutaway views. It is not submitted in the
opaque exterior pass, where the body already hides it; envelope tests include
the transformed outer headrest crowns as well as the EV components.

The exterior is closed at the nose (`u = 0`) and tail (`u = 1`) by watertight
front single-frame grille and rear Kammback diffuser fascia caps, with a
continuous dark panoramic canopy whose sill (`v_c = 0`) is embedded directly
into the hull's upper beltline deck (`v = 0.84`). Exterior and driver views
evaluate that exact same `vehicle_canopy_point()` surface; the mirror bracket
is anchored to its front roof header. Driver mode changes only the material
regions and draws the remaining glass in a late transparent pass—there is no
separate driver-only roof approximation. `render_ev_systems()` is omitted in
first person so internal HV cables never poke through the dashboard pad, and
the exterior camera (`VEHICLE_CAMERA_CHASE`) supports middle-drag / right-drag
orbit inspection around the vehicle.

F5 cycles **driver → exterior concept → EV systems cutaway**. The cutaway removes
opaque body panels and renders the full hull + fastback canopy spline wire cage
so the electrical, thermal, and hover-thruster components can be inspected
entirely inside the vehicle envelope.

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

## Car-part window mounts, cluster, and windshield HUD

The eight general application outputs are predefined pieces of cockpit hardware,
not eight copies of a freestanding monitor:

| Output | Car part | Deployment |
|---|---|---|
| CD-01 | Windshield | Fixed |
| CD-02 / CD-03 | Dashboard | Slide up on rails |
| CD-04 / CD-05 | Left / right door | Slide out on an arm |
| CD-06 / CD-07 | Left / right roof | Flip down from a hinge |
| CD-08 | Rear deck | Fixed |

A newly mapped sliding surface starts at its mount's stowed transform and
deploys over 0.72 seconds (0.90 seconds for roof hinges). `monitor_plane_at()`
owns the interpolation. `scene_update_mounts()` stores that exact current plane
in the `Monitor`, and both `draw_monitor()` and `scene_pick()` consume it, so
pointer UV coordinates remain correct during the animation rather than snapping
to the final position on the first frame. Camera focusing targets the deployed
plane deliberately. Remapping a surface restarts its deployment; fixed mounts
never perform a meaningless slide.

CD-09 is a **ninth, dedicated output** for the instrument panel behind the
steering wheel. It has a 1000×404 `wl_output` mode and a matching xdg configure,
rather than the 1000×625 general-app mode. Only `chromedrive.cluster` can occupy
it, and mapping this passive instrument does not steal keyboard focus from an
application. The METERS dashboard action focuses this fixed panel.

The cluster is not a compositor Cairo texture or a 2D overlay. It is the
`cluster` mode of `clients/demo.c`, an independent native Wayland client using
double-buffered `wl_shm`, xdg-shell, buffer releases, and `wl_surface.frame`
pacing like any other client. `src/telemetry.c` publishes actual speed, gear,
battery, power, motor temperature, hover height, odometer, and connected-app
state through an inherited memfd. A sequence counter provides consistent
snapshots; no values are synthesized in the client. The old `telemetry` mode
still exists for explicitly launched `/proc` CPU/memory monitoring, but it is a
different app from the EV instrument cluster.

The windshield HUD is a third, separate thing: a mostly transparent 3D texture
on the glass plane. It displays live speed, gear, pack, power, steering and hover
state while leaving the centre clear for the fixed windshield application. It
is not the instrument cluster, and it is distinct from the ordinary 2D
screen-edge remote-desktop controls. `cockpit.hud` controls both the glass HUD
and the screen-edge chrome.

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
5. The first textured commit maps the toplevel to its free car-part mount (or
   the reserved instrument output), emits the appropriate
   `wl_surface.enter`/`wl_output` events, starts any mechanical deployment, and
   applies keyboard focus only if it is not the passive cluster.

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
non-contiguous letter codes needed by WASD. `tests/test_math.c` covers all nine
output transforms plus stowed/mid-travel/deployed picking, and
`tests/test_telemetry.c` verifies the inherited memfd, snapshot sequencing,
live values, and cleanup. Existing config tests and the native settings
selftest remain in `make test`. The protocol probe expects eight app outputs
plus the cluster output. Runtime smoke testing uses the real surfaceless
EGL/VNC backend and can inject standard RFB key events—the same path used by
noVNC.
