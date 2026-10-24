# Configuration: the launch menu, the settings GUI, and the config files

Everything user-facing in ChromeDrive and Garage3D is configurable through
**`bin/chromedrive-config`**, a native Wayland settings app, or by hand-editing
two INI files. Edits apply **live**: both programs poll their config file
every 0.5 s and hot-apply whatever changed — relabel a button in the GUI and
the cockpit's Launch Control shows the new label about half a second later.

![Launch Menu editor](config-launch-menu.webp)

![Live result in the cockpit](config-live-cockpit.webp)

## Running the GUI

```sh
make                        # builds bin/chromedrive-config with everything else
./bin/chromedrive-config                       # on any Wayland session
WAYLAND_DISPLAY=<chromedrive-socket> ./bin/chromedrive-config   # inside ChromeDrive
./bin/chromedrive --exec "'$PWD/bin/chromedrive-config'"        # auto-launch it in the cockpit
```

Three tabs:

1. **Launch Menu** — the app-menu editor (next section). This is the main event.
2. **Garage3D** — window/camera/scene/physics of the 3D client.
3. **Compositor** — display/cockpit/session of ChromeDrive itself.

Sliders, toggles, colour pickers and the entry list work with pointer and
touch. Text fields need a physical keyboard (there is no text-input/OSK
protocol support; on a touch-only device, edit text in the file directly).
Headless modes for scripts/CI: `--selftest` drives the real widgets against
temp files and verifies the round-trip (also part of `make test`), and
`--screenshot OUT.webp --tab 0|1|2` renders a tab offscreen.

## The Launch Menu editor

The cockpit's Launch Control has six entries. Four are app slots
(`app.terminal`, `app.files`, `app.audio`, `app.meters`); two are built-in
actions (`app.neon` = cycle underglow, `app.cockpit` = reset camera). All six
are editable; app slots expose the full KDE-menu-editor-style field set:

| Field (key) | Meaning |
|---|---|
| Name (`name`) | Button label, first line. |
| Description (`sub`) | Small second line (e.g. `02 / EXPLORE`). |
| Command (`exec`) | Shell command run on click (via `sh -lc`). `%root%` expands to the ChromeDrive install root, so configs stay portable: `exec = %root%/clients/garage3d/bin/garage3d`. |
| App ID match (`app_id`) | Substring matched against a mapped window's `app_id`: clicking a running app focuses its monitor instead of launching a second copy. Empty = always launch new. |
| Icon (`icon`) | One of the six cockpit glyphs: `0` prompt, `1` folder, `2` music, `3` meters, `4` sun, `5` wheel. |
| Enabled (`enabled`) | `false` greys the button out ("DISABLED"); clicks show a notice instead of launching. |
| Autostart (`autostart`) | Whether the app joins the `--demo` / `session.demo` launch set. |

Built-in entries accept `name`, `sub`, `icon`, `enabled` (disabling NEON or
COCKPIT removes that action from the pad; F-keys still work).

**Adding your own app (example).** Point slot 2 at any Wayland program —
here, Garage3D itself, and a file manager with a proper app-id for focusing:

```ini
[app.files]
name = GARAGE3D
sub = 02 / 3D SCENE
exec = %root%/clients/garage3d/bin/garage3d
app_id = chromedrive.garage3d
icon = 5
autostart = true

[app.audio]
name = DOLPHIN
sub = 03 / FILES
exec = dolphin
app_id = org.kde.dolphin
icon = 1
```

Notes and caveats:

- The six slots' *positions* are fixed cockpit geometry (2 rows x 3); editing
  changes everything about an entry except which physical button it is.
- `CHROMEDRIVE_TERMINAL/FILES/AUDIO/METERS` environment variables, if set,
  win over `exec` (they are an explicit launch-time override from before the
  config file existed). The GUI warns inside the Command field when one is set.
- Changes are live: labels/icons regenerate immediately; a new `exec` or
  `app_id` is used from the next click. Already-running apps are unaffected.

## Files, format, precedence

| App | File |
|---|---|
| Compositor | `$XDG_CONFIG_HOME/chromedrive/chromedrive.conf` (fallback `~/.config/...`) |
| Garage3D | `$XDG_CONFIG_HOME/garage3d/garage3d.conf` |

Format (both files): `[section]`, `key = value`, `#`/`;` comments, booleans
as `true/false` (`yes/no/on/off/1/0` accepted), colours as `#RRGGBB`.
The GUI saves atomically (tmp file + rename) and **preserves unknown keys**
it doesn't know about; comments/blank lines are not preserved through a GUI
save (hand-edits without the GUI are of course untouched). Missing file =
built-in defaults; malformed values are ignored with a warning on stderr,
never fatal.

Precedence, both apps: **built-in defaults < config file < environment**
(compositor `exec` only) **< command line**. `--config PATH` picks a
different file, `--no-config` ignores configuration entirely (the smoke
tests run config-isolated for the same reason). Once running, the file is
the live control surface: editing it overrides the startup state. Session
tweaks made inside the apps (F4 neon cycle, Garage3D Space pause, +/- FOV)
last until the next file edit, then the file wins for the keys it sets.

### What's live vs restart

- **Compositor, live:** `display.fps`, `display.fov`, `cockpit.*`
  (neon/hud/background), all `[app.*]` menu keys.
  **Restart:** `display.width/height/fullscreen`, `session.*`.
- **Garage3D, live:** everything except `window.width/height` (initial size).
  `window.fullscreen`/`scale` are re-requested live. Physics toggles,
  pendulum amplitude changes (energy is rescaled smoothly) and rain count
  adjust without a hitch. Edge-triggered care is taken: a reload does not
  unpause a Space-paused scene unless `physics.enabled` itself changed.

## Full schema

### chromedrive.conf

```ini
[display]
width = 1600          # 640..7680            (restart)
height = 1000         # 400..4320            (restart)
fps = 60              # 1..120 frame cap
fov = 65              # 40..100 driver FOV
fullscreen = false    # --windowed only      (restart)

[cockpit]
neon = 0              # 0 Ultraviolet, 1 Ice Blue, 2 Acid Lime
hud = true            # HUD overlay visible
background = 3d       # 3d | texture (flat poster) | none

[session]
headless = true       # false = SDL/GLX dev window   (restart)
vnc_port = 5900       # headless VNC          (restart)
demo = false          # launch autostart apps (restart)
socket =              # Wayland socket name, empty = automatic (restart; file-only)

[app.terminal]        # + app.files / app.audio / app.meters / app.neon / app.cockpit
name = TERMINAL
sub = 01 / SHELL
exec = foot --config='%root%/assets/foot.ini' --title='Terminal' /bin/sh '%root%/assets/terminal-welcome.sh'
app_id = foot
icon = 0
enabled = true
autostart = true
```

### garage3d.conf

```ini
[window]
width = 1280          # initial logical size (restart)
height = 720          # (restart)
fullscreen = false
scale = 0             # 0 = follow output, 1..4 forced buffer scale

[camera]
fov = 62              # 30..90
orbit_speed = 1.0     # drag sensitivity multipliers
zoom_speed = 1.0
pan_speed = 1.0
move_speed = 6        # WASD locomotion, m/s
attract = true        # idle view drift
attract_delay = 6     # seconds idle before drift

[scene]
neon_left = #1AFFE9   # base colours; emissive = colour x neon_intensity
neon_right = #F525FF
neon_intensity = 2.5
light_intensity = 1.0 # master dimmer over all scene lights
fog_density = 0.016   # 0 disables
rain = 420            # 0..420 streaks outside the doorway
mist = 1.0
reflections = 1.0     # wet-floor reflection strength
city_density = 0.30   # lit-window fraction, near skyline layer
flicker = true        # the faulty right-wall tube

[physics]
enabled = true        # false = start (and stay, until edited) paused
gravity = 1.0         # multiplier on 9.81
lamp_swing = 18       # pendulum amplitude, degrees; 0 = hangs still
fan_speed = 1.0
balls = true
```

## Implementation notes

- `include/cdconfig.h` + `src/cdconfig.c`: the generic string-typed INI
  store (load/atomic-save/typed getters). Used by the compositor and the
  GUI. `tests/test_config.c` covers round-trips incl. unknown keys.
- Garage3D needs no store (it never writes): `clients/garage3d/src/config.c`
  is a typed loader for the same format, with every value range-clamped —
  it must stay standalone, hence the deliberate small duplication.
- The compositor applies config in `src/main.c` (`apply_config` at startup,
  `config_poll` + `apply_app_config` in the loop; `ui_refresh_buttons` in
  `src/ui.c` re-renders button textures). Garage3D applies in its `main.c`
  loop through `scene_apply_settings` plus edge-triggered diffs.
- The GUI (`clients/config.c`) is schema-driven for the slider tabs and
  hand-built for the menu editor; one `draw_ui()` builds both pixels and
  hitboxes, so `--selftest` can click real widget coordinates headlessly.
