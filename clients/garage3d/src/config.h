/*
 * config.h - GarageSettings: every tunable in the scene, loadable live.
 *
 * Settings live in "$XDG_CONFIG_HOME/garage3d/garage3d.conf" (same tiny
 * INI shape as the compositor's chromedrive.conf - [section], key = value,
 * '#' comments, bools as true/false, colours as #RRGGBB). The writer is
 * the project config GUI (bin/chromedrive-config); this client only ever
 * reads. Precedence here: built-in defaults < config file < CLI flags,
 * and while running the file is re-read whenever its mtime changes
 * (main.c polls), so GUI slider drags land in the live scene.
 *
 * Garage3D is standalone-installable, so it cannot link the compositor's
 * src/cdconfig.c; this is a small typed loader for the same format (no
 * store, no writer - a reader needs neither). Every value is range-clamped
 * on the way in: the file may be hand-edited and must never be able to put
 * the scene into a NaN/negative-count/absurd state.
 */
#ifndef GARAGE3D_CONFIG_H
#define GARAGE3D_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef struct {
    /* [window] - startup values (fullscreen/scale also apply live) */
    int width, height;      /* initial logical size */
    bool fullscreen;
    int scale;              /* 0 = follow output, 1..4 = forced buffer scale */

    /* [camera] */
    float fov;              /* degrees, 30..90 */
    float orbit_speed;      /* multiplier on drag orbit sensitivity */
    float zoom_speed;       /* multiplier on wheel/pinch zoom */
    float pan_speed;        /* multiplier on drag pan */
    float move_speed;       /* held-key locomotion, m/s */
    bool attract;           /* idle view drift */
    float attract_delay;    /* seconds idle before attract starts */

    /* [scene] */
    float neon_left[3];     /* sRGB-ish base colour of the left tubes */
    float neon_right[3];
    float neon_intensity;   /* HDR gain applied to the base colours */
    float light_intensity;  /* multiplier on all scene lights */
    float fog_density;      /* exp2 density, 0 disables */
    int rain;               /* active rain streaks, 0..420 */
    float mist;             /* mist brightness multiplier */
    float reflections;      /* wet-floor reflection strength */
    float city_density;     /* lit-window density in the skyline shader */
    bool flicker;           /* the faulty right-wall tube */

    /* [physics] */
    bool physics_enabled;
    float gravity;          /* multiplier on 9.81 m/s^2 */
    float lamp_swing;       /* pendulum amplitude, degrees (0 = hangs still) */
    float fan_speed;        /* multiplier on the ceiling-fan spin */
    bool balls;             /* show the bouncing balls */
} GarageSettings;

/* Built-in defaults: exactly the values the original hard-coded scene used
 * (verified by the smoke test's pixel checks running config-isolated). */
void settings_defaults(GarageSettings *s);

/* "$XDG_CONFIG_HOME/garage3d/garage3d.conf" (or ~/.config/...). */
bool settings_path(char *buf, size_t size);

/* Starts from defaults, then overlays the file (if any), then clamps.
 * Returns true if the file existed. Unknown keys are ignored. */
bool settings_load(const char *path, GarageSettings *s);

/* Ready to poll: 0 if the file does not exist. */
time_t settings_mtime(const char *path);

/* Range-limits every field. Idempotent. */
void settings_clamp(GarageSettings *s);

#endif /* GARAGE3D_CONFIG_H */
