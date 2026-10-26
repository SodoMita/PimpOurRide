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
RFB input ─► backend_egl (shared) ──► scene + spline EV + fixed-step physics
                                      │
                                      ▼
                           surfaceless EGL color/depth FBO
                                      │ one glReadPixels + row flip
                        ┌─────────────┴─────────────┐
                        ▼                           ▼
             embedded libvncserver RFB      fbdev format convert/blit
                        │ websockify                │ /dev/fb* mmap (pan/vsync)
                        ▼                           ▼
          static noVNC tools/viewer/index.html   current kernel fb mode
```

The alternate `--drm` path uses the same scene and GL-context ownership but a
GBM window surface/default framebuffer instead of the surfaceless output FBO.
It performs direct KMS modesetting and page-flip scanout, with no per-frame
`glReadPixels` or fbdev format-conversion blit.

`tools/run-web.sh` is the fullstack launcher. It builds the C programs, stages
the operating system's noVNC assets, starts the headless compositor, and serves
the browser UI on the same origin as its WebSocket bridge. There is no Node
runtime, frontend build, X server, screenshot-video encoder, or web-based
Wayland protocol implementation.

```sh
sudo apt-get install -y libsdl2-dev libwayland-dev wayland-protocols \
  libgl-dev libegl-dev libxkbcommon-dev libwebp-dev libcairo2-dev \
  libdrm-dev libgbm-dev libvncserver-dev novnc websockify foot
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
body stations (`9` control stations from `z = -5.85` to `+5.72`), seven
transverse cross-section profile control points (undertray spine, lower chine,
sculpted/scooped waist, shoulder, upper deck and hood/deck spine), and `7` fastback canopy
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
geometry is used in every camera view; envelope tests include the transformed
outer headrest crowns and both seating eye positions as well as the EV
components. Driver and passenger eyes come from the same
`vehicle_seat_anchor()` / `vehicle_seat_eye()` layout as the upholstery, so the
camera cannot silently drift back to a point between the seats.
`VEHICLE_COCKPIT_FORWARD` places that shared seating layout 1.55 m farther
forward, beside the actual door aperture. The same car-local offset owns the
dashboard, console, steering wheel, door upholstery, display mounts and seat
cameras, so moving the occupants does not desynchronize rendering or picking.

The exterior is closed at the nose (`u = 0`) and tail (`u = 1`) by watertight
front single-frame grille and rear Kammback diffuser fascia caps, with a
continuous dark panoramic canopy whose sill (`v_c = 0`) is embedded directly
into the hull's upper beltline deck (`v = 0.84`). Exterior and driver views
evaluate that exact same `vehicle_canopy_point()` surface; the mirror bracket
is anchored to its front roof header. Driver mode changes only the material
regions and draws the remaining glass in a late transparent pass—there is no
separate driver-only roof approximation. The seats, dashboard, door panels and
pulls, steering wheel, console, controls, and Wayland monitor geometry are also
submitted from the same scene functions in every view, before the hull; opaque
bodywork occludes them and the same tinted window openings reveal them outside.
There is no simplified exterior-only cockpit.

The doors are actual full-height side assemblies, not cutlines: the lower
aperture reaches from the underbody/sill crease (`v = 1/6`) to the beltline
(`v = 0.84`), and its exact canopy continuation reaches through the side glass
and cantrail (`v_c = 0.715`). The undertray and roof crown remain fixed. Shared
split mesh axes at those boundaries prevent leftover strips or overlaps between
fixed bodywork and the moving door. Separate outer/inner shells, edge thickness,
hinge knuckles, weather seals, recessed satin pulls, quilted panels, armrests,
speakers, upper frames and glass all use the same front-edge hinge. Meta+O reversibly
opens/closes both sides through 68 degrees in 1.15 seconds of fixed-step travel;
door-mounted Wayland displays follow too, and F10 recovery closes both doors.
Collision extents include the moving lower panels, upper frame, glass and trim,
so an open door cannot pass through a garage wall outside the closed-hull box.

`render_ev_systems()` is omitted in first person so internal HV cables never
poke through the dashboard pad, and the exterior camera
(`VEHICLE_CAMERA_CHASE`) supports middle-drag / right-drag orbit inspection
around the vehicle.

F8 cycles **driver → exterior concept → EV systems cutaway**, and F9 switches
between the aligned driver and passenger seats. Returning to a seat with F8
restores its forward camera and normal FOV rather than retaining an exterior
orbit or app-focus zoom; F2 provides the same seat-camera reset directly. The cutaway removes
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
- smoothed body roll/pitch and energy-absorbing wall/building contacts;
- a yaw-rotated collision box covering the complete nose, tail, body width, and
  mirror clearance, tested against the same home/city wall and building boxes
  used by the renderer. The home far wall has a real opening onto the street.

It is an interactive concept-car model, not a validated automotive safety or
homologation simulation. Pack energy is a `double`: at parking load, a single
120 Hz tick consumes only a few micro-kWh, which would round away if repeatedly
subtracted from a `float` near 69 kWh.

### Connected city, garages, and autopilot

`src/city.c` owns the protected core, navigation, traffic and collision;
`src/city_stream.c` is the allocation-free procedural model; and
`src/city_scene.c` renders those same records. Each 80 m chunk owns its minimum
X/Z road edges and land-use interior. Integer hashing makes districts, buildings,
parked vehicles and garages identical on every visit, including negative and
far-world coordinates. Six coherent districts use local roads, alleys, avenues,
boulevards and covered tunnels, with blocks, public squares, parking courts,
parks and garage courts. Tunnel walls have matching 12 m intersection openings;
garage walls, fountains, kiosks, trees and parked cars are physical rather than
renderer-only decoration.

The aligned 20-chunk core preserves the detailed Home hall and original city.
Home stands in a paved off-street campus with four additional physical workshops,
not on a public through-road. Three connected streets and two cross streets join
**ChromeDrive / Home**, **Neon Works / West**, **Skyline Lab / North**, and
**Harbor Service / East**. Remote and streamed garages have real side walls,
roofs, service benches and clear drive-through lanes. Core geometry is cached in
a GL display list; nearby streamed chunks are emitted from the same data used by
collision and garage lookup. Vehicle-relative rain is suppressed inside garages
and tunnels. Eight moving cars use the authoritative fixed-step simulation clock
for rendering, prediction and collision. Moving and parked cars share seeded
Catmull–Rom body/cabin lofts, with both wheeled and wheel-free variants; tyre
assemblies meet the road while hover variants retain their plasma pods.

Meta+A toggles autopilot in either input-ownership mode. Engagement requests
both doors to close and holds the parking brake until they have physically
finished. Each 120 Hz tick then produces an ordinary `VehicleInput`: throttle,
regenerative/service brake, and steering pass through the same hover dynamics,
battery/thermal telemetry, and collision path as manual input. There is no
render-time translation of the car along a spline. The deterministic expedition
visits all three original remote garages, continues through the northern tunnel,
a streamed district garage, a parking court and civic square, then returns along
Harbor's open drive-through axis and parks at Home. It does not attempt a U-turn
inside the enclosed Home hall. Starting another run after parking nose-in first
reverses out of the home doorway, then resumes forward driving. Multi-phase
fixed-step tests require every destination, actual parked rest and zero contacts.
Ambient traffic follows deterministic closed-form routes on that same
clock; it is not a substitute for the main car's simulated controls. Rendering,
predictive yielding/braking, and collision fallback use identical traffic poses
and per-seed body envelopes (including spline, wheel and lamp overhang). Streamed
parked solids likewise carry the exact renderer seed and matching footprint.
Crossing reservations protect corners and Home exits;
last-resort contact resolution remains enabled rather than hiding traffic.

In Drive mode, pressing a driving key immediately disengages autopilot and
returns manual control. In Client mode those keys still go to the focused
Wayland application, so it is possible to work while the car drives. F10
recovery also disengages autopilot. The native HUD shows the current garage or
district/land-use area, next autopilot destination, and live FOV; the browser toolbar exposes the same
Meta+A shortcut.

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
never perform a meaningless slide. All nine deployed output rectangles are
packaged inside the same hull/canopy rather than placed on a generic monitor
ring. `tests/test_vehicle.c` checks every transformed screen corner at deployed,
stowed, and mid-animation poses, in addition to dense interior door-panel
surface checks against the concave bicubic door skin.

Every CD-01..CD-08 mount also has a three-stage telescopic linear-actuator
manipulator. `monitor_manipulator_for_plane_on_mount()` solves against the live
window plane while keeping its base and shoulder attached to the deployed car
mount. Frame dragging and Arrange movement therefore change actuator extension
rather than dragging the chassis hardpoints with the window. Door displays pass
the physically hinged mount through the same solver, so the complete support
assembly follows the door without losing the free-window transform. Normalized
upper-arm, forearm, and auxiliary-piston extension drives the concentric barrel,
sleeve, and chrome ram rendering; exact link lengths, fixed anchors, deployment,
and moved-window behavior are covered by `tests/test_math.c`.

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
| Left-drag 3D wheel | Continuous analog horizontal pointer-axis input to focused app | Steer |
| F8 | Driver/exterior/systems camera | Same |
| F9 | Switch driver/passenger seat | Same |
| F10 | Recover pose without refilling battery/resetting odometer | Same |
| Meta+A | Toggle deterministic city autopilot | Same |
| Meta+H | Show or hide the 2D HUD overlay (dash HUD button mirrors it) | Same |
| Meta+Shift+Q | Quit ChromeDrive immediately | Same |
| Red dash EXIT button | First press arms exit for 3 s; a second press quits | Same |
| F2 | Return to the selected seat camera | Same |

Client mode normally commands a parking brake so manipulating an application
cannot leave the car accelerating; an engaged autopilot deliberately overrides
that parked input inside the fixed-step update. On mode changes and host focus
loss, both Wayland keys and compositor-local drive keys are released. Browser hold buttons use
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
   renders, and attempts presentation/readback of the FBO without waiting for
   DRM scanout.
5. Only then does `server_frame_done()` send `wl_callback.done(time)` and destroy
   the one-shot resource. `wl_display_flush_clients()` makes the event visible.
6. The client renders again from that callback and requests exactly one new
   callback. Together with the compositor FPS cap this supplies backpressure.
   DRM may skip an output present while an older flip scans out; it does not
   stall these callbacks or make client performance depend on physical vblank.

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
wl_pointer.axis_source(continuous)
wl_pointer.axis(horizontal_scroll, fractional_analog_value)
wl_pointer.frame
```

Every RFB/SDL motion sample is forwarded; it is **not** accumulated into
28-pixel detents. The former 28-pixel step survives only as a sensitivity
scale: 28 pixels correspond to 15 axis units, while tiny and fractional drags
retain Wayland's 24.8 fixed-point precision. No `axis_discrete` or vertical
`axis` event is sent on this path, so the client receives a horizontal analog
stream rather than binary clicks or vertical scrolling. Right drag = positive
horizontal and left drag = negative horizontal at the client. The Files demo
accepts that horizontal continuous stream for its row UI and accumulates
fractional values instead of jumping three rows per event; ordinary physical
mouse wheels retain their normal source/discrete sequence. Older pointer
resources receive fractional axis values without the source event unsupported
by their version.

## EGL/GL context ownership

The headless and fbdev backends share `src/backend_egl.c`: it explicitly
selects `EGL_PLATFORM_SURFACELESS_MESA`, binds `EGL_OPENGL_API` before context
creation, and makes a context current with `EGL_NO_SURFACE` for draw/read. Its
color+depth renderbuffer FBO is the output; there is no window surface and
therefore no `eglSwapBuffers` in this path.

The DRM backend instead selects `EGL_PLATFORM_GBM_KHR`, matches an EGL config
to `GBM_FORMAT_XRGB8888` with 24-bit depth and 8-bit stencil for direct/fallback
3D rendering, and makes a GBM window surface current. Its default framebuffer
is the scene destination (`target_fbo = 0`). After `eglSwapBuffers`, it
registers the locked GBM buffer as a KMS framebuffer with its real plane offset
and modifier (legacy linear registration remains a fallback) and uses
`drmModeSetCrtc` for the first frame and `DRM_MODE_PAGE_FLIP_EVENT`
thereafter. At most the current and pending buffers stay locked; the previous
buffer is released only after its flip-completion event. `backend_drm_present`
waits for that event before returning, preserving the main loop's rule that
Wayland `wl_callback.done` follows actual presentation rather than submission.
This sandbox has no `/dev/dri`, so real GBM scanout, driver modifier
compatibility, flip cadence, and physical CRTC restoration require a
target-hardware run. Deterministic fake-KMS tests cover connector selection,
buffer/event control flow, and restoration/failure cleanup without claiming
physical scanout.

Backend initialization precedes Wayland server dispatch. This ordering matters
because a client commit can upload `wl_shm` pixels with `glTexImage2D`, and a
linux-dmabuf commit can reference an `EGLImage` texture. Those are GL calls,
not protocol-only bookkeeping, and must run with a current context on the same
thread as the scene. The optional windowed backend remains SDL2/GLX and does
not advertise EGLImage dma-buf import.

Headless presentation binds the FBO, calls `glReadPixels`, flips GL's bottom-up
rows into RFB's top-down framebuffer, and marks the rectangle modified. fbdev
presentation does the same single readback, then converts it into the device's
packed truecolor format and blits it into `/dev/fb*` memory, using
`FBIOPAN_DISPLAY` only when `yres_virtual` already has at least two pages and
treating `FBIO_WAITFORVSYNC` as best-effort. By default it adopts the current
kernel mode; with `--fbdev-modeset` or `--fbdev-mode` it requests a modeset via
`FBIOPUT_VSCREENINFO` and restores the original mode on exit. The driver may
adjust timings or reject the request. Local evdev input (keyboard, mouse,
touchscreen) is enabled by default for fbdev and DRM; `--no-evdev` (or
`--no-fbdev-evdev`) disables it. `--grab-input` opts in to `EVIOCGRAB` (fbdev
warns about VT lockout; DRM requires a managed controlling VT or downgrades the
grab), and `--input-device PATH` restricts enumeration (up to 8;
older fbdev-named aliases remain valid). A single epoll worker reads devices
and queues plain events with `CLOCK_MONOTONIC` kernel timestamps where
supported. It never calls Wayland, scene, physics, or GL code. The compositor
thread drains the bounded queue through the existing dispatch handlers. A full
queue blocks further production until the old prefix is drained and every
potentially latched key, touch, pointer button, and driving control is safely
released; device slots are never compacted underneath queued events.

On a controlling Linux VT, physical-evdev Ctrl+Alt+F1…F12 uses the DRM backend's
`VT_PROCESS`/`signalfd` lifecycle. The worker is joined before input state is
released and grabs are dropped; stale device bytes are discarded before a fresh
queue/worker starts on resume. DRM drains and retires old scanout buffers before
dropping master, and the main loop pauses Wayland, physics, GL and frame
callbacks while inactive. VNC/SDL chords never switch the host VT. fbdev keeps
its previous unmanaged VT behavior.

During shutdown, direct input is stopped and released before Wayland/xkb state
is destroyed. Clients and scene textures are then destroyed before the EGL
context is unbound and terminated. fbdev restores the original video mode (or
pan offset) before unmapping; an active DRM session waits for outstanding
flips and restores the previous CRTC, while inactive-VT cleanup skips that wait
and restoration. Both release locked GBM buffers and drop master only when
held. No physics
code, browser code, or worker thread owns a GL context.

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
`tests/test_fbdev.c` exercises packed 16/24/32bpp conversion, LUT quantization,
bottom-up row flips, independent strides/padding, geometry validation, and page
offsets without opening `/dev/fb0` (so `make test` also runs in CI/sandboxes),
plus `evdev_util` absolute scaling/touch-id pool and the asynchronous queue's
FIFO ordering, stable device IDs, overflow blocking, and post-reset resume.
`tests/test_drm.c` compiles the real DRM backend against deterministic fake
KMS/GBM/EGL entry points. It exercises initial modesetting, completed and
pending page flips, bounded buffer/FB lifetimes, linear and non-linear modifier
registration, required depth/stencil EGL attributes, cloned-connector CRTC
restoration plus stale-clone fallback, evdev ownership, and injected
init/event/FB/CRTC/flip failures.
It needs no DRM device and does not claim physical scanout validation.
`tests/test_keymap.c` pins the exact RFB/X11-to-evdev table, including the
non-contiguous letter codes needed by WASD. `tests/test_math.c` covers all nine
output transforms plus stowed/mid-travel/deployed picking, and
`tests/test_telemetry.c` verifies the inherited memfd, snapshot sequencing,
live values, and cleanup. Door checks include exact sill/beltline/cantrail
boundaries, rigid upper-frame motion, collision extents and forward seat-eye
placement; screen and upholstery samples still use the unweakened shared-body
enclosure assertions. Existing config tests and the native settings
selftest remain in `make test`. The protocol probe expects eight app outputs
plus the cluster output. Runtime smoke testing uses the real surfaceless
EGL/VNC backend and can inject standard RFB key events—the same path used by
noVNC.
