/*
 * config.c - chromedrive-config: the settings GUI for ChromeDrive+Garage3D.
 *
 * A native Wayland client (wl_shm + cairo, same toolkit-free approach as
 * clients/demo.c) whose job is to edit two INI files:
 *
 *   ~/.config/chromedrive/chromedrive.conf   (the compositor)
 *   ~/.config/garage3d/garage3d.conf         (the 3D client)
 *
 * Three tabs, in order of importance:
 *
 *   1. Launch Menu - a KDE-menu-editor-style two-pane editor for the six
 *      Launch Control entries. Every slot can hold an installed app or a
 *      built-in cockpit action: right-click a slot (or use "Choose app")
 *      for the searchable app-picker popup, then adjust name, description,
 *      command, app-id focus matching, icon, enabled and autostart. Saved
 *      values hot-relabel the cockpit buttons.
 *   2. Garage3D - scene/camera/physics sliders, toggles and colour pickers.
 *   3. Compositor - display/cockpit/session settings (some need a restart,
 *      marked with a RESTART chip).
 *
 * Every edit writes the file atomically (see src/cdconfig.c); the running
 * apps poll mtimes and apply changes live, so dragging a slider here is
 * immediately visible in a running Garage3D/cockpit. Unknown keys in the
 * files are preserved (the store round-trips them).
 *
 * The whole UI is drawn on a virtual 1000x625 canvas scaled to the real
 * buffer (exactly like demo.c), so it fills a ChromeDrive monitor 1:1 and
 * still works on any other compositor. Sliders/toggles/lists work with
 * pointer *and* touch; text fields need a real keyboard (no text-input/
 * OSK protocol implemented - documented limitation).
 *
 * Headless modes (no Wayland display needed):
 *   --selftest              Drive the real widget logic against temp files
 *                           and render every tab offscreen; PASS/FAIL exit.
 *   --screenshot OUT.webp   Render one tab (--tab 0|1|2) to a WebP image.
 *
 * Build: root `make` produces bin/chromedrive-config (see Makefile).
 * Schema reference: docs/configuration.md.
 */
#define _GNU_SOURCE
#include "cdconfig.h"
#include "i18n.h"
#include "math3d.h"
#include "xdg-shell-client.h"
#include <cairo/cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <webp/encode.h>
#include <xkbcommon/xkbcommon.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ misc */

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
static bool file_exists(const char *p) { struct stat st; return p && stat(p, &st) == 0; }

/* ------------------------------------------------- schema for slider tabs */

enum { FILE_G3 = 0, FILE_CD = 1 };
typedef enum { K_FLOAT, K_INT, K_BOOL, K_COLOR, K_ENUM, K_INT_INPUT, K_FLOAT_INPUT } Kind;

typedef struct {
    const char *group;          /* heading drawn when it changes */
    const char *sec, *key, *label;
    Kind kind;
    double min, max, step;
    const char *fmt;            /* value formatting ("%.2f", "%d") */
    const char *def;            /* default, string form (reset + fallback) */
    /* K_ENUM labels. 8 rather than the historical 6 so the language row can
     * offer "auto" plus up to seven installed catalogs; see gui_load_languages. */
    const char *enums[8];
    int enum_count;
    bool enum_is_string;        /* store label itself instead of the index */
    bool restart;               /* chip: needs app restart to take effect */
} Def;

static const Def G3_DEFS[] = {
    { "Window", "window", "width",  "Window width",  K_INT, 320, 3840, 10, "%.0f", "1280", {0}, 0, false, true },
    { "Window", "window", "height", "Window height", K_INT, 240, 2160, 10, "%.0f", "720",  {0}, 0, false, true },
    { "Window", "window", "fullscreen", "Start fullscreen", K_BOOL, 0, 0, 0, NULL, "false", {0}, 0, false, false },
    { "Window", "window", "scale", "Buffer scale (HiDPI)", K_ENUM, 0, 4, 1, NULL, "0", { "Auto", "1x", "2x", "3x", "4x" }, 5, false, false },
    { "Camera", "camera", "fov", "Field of view", K_FLOAT, 30, 90, 0.5, "%.1f", "62", {0}, 0, false, false },
    { "Camera", "camera", "orbit_speed", "Orbit sensitivity", K_FLOAT, 0.1, 3, 0.05, "%.2f", "1", {0}, 0, false, false },
    { "Camera", "camera", "zoom_speed", "Zoom sensitivity", K_FLOAT, 0.1, 3, 0.05, "%.2f", "1", {0}, 0, false, false },
    { "Camera", "camera", "pan_speed", "Pan sensitivity", K_FLOAT, 0.1, 3, 0.05, "%.2f", "1", {0}, 0, false, false },
    { "Camera", "camera", "move_speed", "Move speed (m/s)", K_FLOAT, 1, 20, 0.5, "%.1f", "6", {0}, 0, false, false },
    { "Camera", "camera", "attract", "Idle attract drift", K_BOOL, 0, 0, 0, NULL, "true", {0}, 0, false, false },
    { "Camera", "camera", "attract_delay", "Attract after (s)", K_FLOAT, 1, 60, 1, "%.0f", "6", {0}, 0, false, false },
    { "Scene", "scene", "neon_left", "Neon colour - left wall", K_COLOR, 0, 0, 0, NULL, "#1AFFE9", {0}, 0, false, false },
    { "Scene", "scene", "neon_right", "Neon colour - right wall", K_COLOR, 0, 0, 0, NULL, "#F525FF", {0}, 0, false, false },
    { "Scene", "scene", "neon_intensity", "Neon intensity (HDR gain)", K_FLOAT, 0, 4, 0.05, "%.2f", "2.5", {0}, 0, false, false },
    { "Scene", "scene", "light_intensity", "Light intensity", K_FLOAT, 0, 2, 0.05, "%.2f", "1", {0}, 0, false, false },
    { "Scene", "scene", "fog_density", "Fog density", K_FLOAT, 0, 0.05, 0.001, "%.3f", "0.016", {0}, 0, false, false },
    { "Scene", "scene", "rain", "Rain streaks", K_INT, 0, 420, 5, "%.0f", "420", {0}, 0, false, false },
    { "Scene", "scene", "mist", "Mist amount", K_FLOAT, 0, 2, 0.05, "%.2f", "1", {0}, 0, false, false },
    { "Scene", "scene", "reflections", "Wet-floor reflections", K_FLOAT, 0, 1.5, 0.05, "%.2f", "1", {0}, 0, false, false },
    { "Scene", "scene", "city_density", "City window density", K_FLOAT, 0, 1, 0.02, "%.2f", "0.30", {0}, 0, false, false },
    { "Scene", "scene", "flicker", "Faulty tube flicker", K_BOOL, 0, 0, 0, NULL, "true", {0}, 0, false, false },
    { "Physics", "physics", "enabled", "Physics enabled", K_BOOL, 0, 0, 0, NULL, "true", {0}, 0, false, false },
    { "Physics", "physics", "gravity", "Gravity multiplier", K_FLOAT, 0.1, 3, 0.05, "%.2f", "1", {0}, 0, false, false },
    { "Physics", "physics", "lamp_swing", "Lamp swing (degrees)", K_FLOAT, 0, 45, 0.5, "%.1f", "18", {0}, 0, false, false },
    { "Physics", "physics", "fan_speed", "Ceiling fan speed", K_FLOAT, 0, 3, 0.05, "%.2f", "1", {0}, 0, false, false },
    { "Physics", "physics", "balls", "Bouncing balls", K_BOOL, 0, 0, 0, NULL, "true", {0}, 0, false, false },
};
#define N_G3_DEFS ((int)(sizeof(G3_DEFS) / sizeof(G3_DEFS[0])))

/* Row 0 is filled in at runtime by gui_load_languages(): its choices are the
 * catalogs actually installed, which is not knowable at compile time. */
#define CD_LANGUAGE_ROW 0
/* Filled in at runtime by gui_load_post_shaders(): bundled shader presets plus
 * a currently stored custom path so it stays selected in the row. */
#define CD_POST_SHADER_ROW 9
static Def CD_DEFS[] = {
    { "Interface", "ui", "language", "Interface language", K_ENUM, 0, 0, 1, NULL, "auto", {0}, 0, true, false },
    { "Display", "display", "width", "Framebuffer width", K_INT, 640, 3840, 10, "%.0f", "1600", {0}, 0, false, true },
    { "Display", "display", "height", "Framebuffer height", K_INT, 400, 2160, 10, "%.0f", "1000", {0}, 0, false, true },
    { "Display", "display", "fps", "Frame cap (fps)", K_INT, 10, 120, 1, "%.0f", "60", {0}, 0, false, false },
    { "Display", "display", "fov", "Driver field of view", K_FLOAT, CD_DRIVER_FOV_MIN, CD_DRIVER_FOV_MAX, 0.5, "%.1f", "65", {0}, 0, false, false },
    { "Display", "display", "render_to_texture", "Render to texture", K_BOOL, 0, 0, 0, NULL, "true", {0}, 0, false, false },
    { "Display", "display", "render_scale", "3D render scale (HUD stays native)", K_FLOAT_INPUT, 0.001, 100, 0, "%.6g", "1", {0}, 0, false, false },
    { "Display", "display", "render_upscale", "Render upscale sampling", K_ENUM, 0, 1, 1, NULL, "linear", { "nearest", "linear" }, 2, true, false },
    { "Display", "display", "render_downscale", "Render downscale sampling", K_ENUM, 0, 5, 1, NULL, "linear_mipmap_linear", { "nearest", "linear", "nearest_mipmap_nearest", "linear_mipmap_nearest", "nearest_mipmap_linear", "linear_mipmap_linear" }, 6, true, false },
    { "Display", "display", "post_shader", "Post shader (.frag, 3D world only)", K_ENUM, 0, 0, 1, NULL, "off", {0}, 0, true, false },
    { "Display", "display", "fullscreen", "Start fullscreen (windowed)", K_BOOL, 0, 0, 0, NULL, "false", {0}, 0, false, true },
    { "Cockpit", "cockpit", "neon", "Underglow colour", K_ENUM, 0, 2, 1, NULL, "0", { "Ultraviolet", "Ice Blue", "Acid Lime" }, 3, false, false },
    { "Cockpit", "cockpit", "hud", "HUD overlay", K_BOOL, 0, 0, 0, NULL, "true", {0}, 0, false, false },
    { "Cockpit", "cockpit", "background", "Garage backdrop", K_ENUM, 0, 2, 1, NULL, "3d", { "3d", "texture", "none" }, 3, true, false },
    { "Session", "session", "backend", "Display backend", K_ENUM, 0, 2, 1, NULL, "headless", { "headless", "windowed", "fbdev" }, 3, true, true },
    { "Session", "session", "headless", "Legacy headless (if backend unset)", K_BOOL, 0, 0, 0, NULL, "true", {0}, 0, false, true },
    { "Session", "session", "vnc_port", "VNC port (headless/mirror)", K_INT_INPUT, 1024, 65535, 1, "%.0f", "5900", {0}, 0, false, true },
    { "Session", "session", "demo", "Launch autostart apps at start", K_BOOL, 0, 0, 0, NULL, "false", {0}, 0, false, true },
};
#define N_CD_DEFS ((int)(sizeof(CD_DEFS) / sizeof(CD_DEFS[0])))

/* -------------------------------------------------- launch-menu app slots */

enum { ACT_APP = 0, ACT_NEON = 1, ACT_COCKPIT = 2 };

typedef struct {
    const char *sec;            /* [app.terminal] ... */
    const char *def_name, *def_sub;
    int def_icon;
    const char *def_exec;       /* %root% template, NULL for default built-ins */
    const char *def_app_id;     /* NULL for default built-ins */
    bool def_autostart;
    int def_action;             /* ACT_*: any slot may be reassigned later */
    const char *builtin_desc;
    const char *env_name;       /* CHROMEDRIVE_* override var, or NULL */
} AppDef;

static const AppDef APP_DEFS[6] = {
    { "app.terminal", "TERMINAL", "01 / SHELL", 0,
      "foot --config='%root%/assets/foot.ini' --title='Terminal' /bin/sh '%root%/assets/terminal-welcome.sh'",
      "foot", true, ACT_APP, NULL, "CHROMEDRIVE_TERMINAL" },
    { "app.files", "FILES", "02 / EXPLORE", 1,
      "'%root%/bin/chromedrive-demo' files", "chromedrive.files", true, ACT_APP, NULL, "CHROMEDRIVE_FILES" },
    { "app.audio", "AUDIO", "03 / SESSION", 2,
      "'%root%/bin/chromedrive-demo' audio", "chromedrive.audio", true, ACT_APP, NULL, "CHROMEDRIVE_AUDIO" },
    { "app.meters", "METERS", "04 / EV", 3,
      "'%root%/bin/chromedrive-demo' cluster", "chromedrive.cluster", false, ACT_APP, NULL, "CHROMEDRIVE_METERS" },
    { "app.neon", "NEON", "05 / COLOR", 4, NULL, NULL, false, ACT_NEON,
      "Built-in action: cycles the underglow colour (same as F4).", NULL },
    { "app.cockpit", "COCKPIT", "06 / HOME", 5, NULL, NULL, false, ACT_COCKPIT,
      "Built-in action: resets the driver camera (same as F2).", NULL },
};
static const char *ICON_NAMES[6] = { "Prompt", "Folder", "Music", "Meters", "Sun", "Wheel" };

/* One entry in the right-click "assign an app to this icon" popup. The
 * catalog is the two cockpit built-ins, the ChromeDrive programs, then every
 * visible Type=Application desktop entry found in an XDG applications
 * directory, in that order; desktop entries are name-sorted. */
enum { CHOICE_APP = 0, CHOICE_NEON = 1, CHOICE_COCKPIT = 2 };
typedef struct {
    char name[128];
    char comment[192];
    char exec[512];             /* shell-ready command; empty for built-ins */
    char app_id[256];
    char desktop_id[256];       /* basename without .desktop (dedupe key) */
    char categories[256];
    int kind;                   /* CHOICE_* */
    int icon;                   /* nearest of the six cockpit glyphs */
    bool terminal;
} AppChoice;

/* ------------------------------------------------------------- GUI state */

enum { TAB_APPS = 0, TAB_G3 = 1, TAB_CD = 2 };
enum { FLD_NAME = 0, FLD_SUB, FLD_EXEC, FLD_APPID, FLD_COUNT };
enum {
    H_NONE = 0, H_TAB, H_SLIDER, H_TOGGLE, H_COLOR, H_COLSLIDE,
    H_ENUM_DEC, H_ENUM_INC, H_APP_ROW, H_FIELD, H_NUM_FIELD, H_ICON_DEC, H_ICON_INC,
    H_APP_ENABLED, H_APP_AUTOSTART, H_RESET_TAB, H_RESET_ENTRY,
    H_CHOOSE_APP, H_PICKER_ITEM, H_PICKER_UP, H_PICKER_DOWN, H_PICKER_CLOSE
};

typedef struct {
    double x, y, w, h;
    int kind, a, b, c;          /* a: file/def/app/field, b: index, c: aux */
} HitBox;

typedef struct {
    char buf[512];
    int caret;                  /* byte offset; fields are edited as UTF-8 but
                                 * caret ops are codepoint-aware (see key()) */
} TextField;

typedef struct {
    /* model */
    char root[4096];            /* install root: catalogs live in assets/locale */
    char language_applied[32];  /* [ui] language currently loaded */
    CdConfig *store[2];
    char path[2][4096];
    bool save_pending[2];
    double last_change;
    char saved_note[96];
    /* ui */
    int tab;
    double scroll[3];
    int sel_app;
    TextField fields[FLD_COUNT];
    int focus;                  /* FLD_* or -1 */
    int field_app;              /* app index the fields currently show */
    bool field_dirty;
    TextField num_field;        /* schema numeric text input (the VNC port) */
    int num_file, num_idx;      /* def being edited, num_idx < 0 = inactive */
    bool num_dirty;
    AppChoice *choices;         /* installed-app catalog for the picker */
    int choice_count, choice_cap;
    bool picker_open;
    int picker_slot;            /* Launch Control slot being assigned */
    double picker_x, picker_y;  /* requested popup anchor (virtual coords) */
    int picker_scroll;          /* first visible filtered entry */
    int picker_sel;             /* selected filtered entry */
    char picker_filter[96];
    char picker_request_path[4096]; /* compositor -> GUI "open picker" IPC */
    char picker_request_seen[64];   /* last consumed request nonce */
    time_t picker_request_mtime;
    long picker_request_mtime_ns;
    int drag_kind, drag_a, drag_b, drag_c; /* active slider drag (H_* + ids) */
    int exp_color_file, exp_color_idx;     /* expanded colour editor, -1 none */
    double px, py;              /* pointer (virtual coords) */
    bool draw_dirty;
    HitBox boxes[768];
    int box_count;
    double content_h;           /* last built schema-tab content height */
    cairo_t *measure;           /* scratch context for text metrics */
    cairo_surface_t *measure_surf;
} Gui;

static CdConfig *store_of(Gui *g, int file) { return g->store[file]; }
static void gui_sync_language(Gui *g);

static void touch_file(Gui *g, int file) {
    g->save_pending[file] = true;
    g->last_change = now_s();
    g->draw_dirty = true;
}

static void flush_saves(Gui *g, bool force) {
    if (!force && now_s() - g->last_change < 0.12) return;
    bool saved = false;
    for (int f = 0; f < 2; f++) {
        if (!g->save_pending[f]) continue;
        if (cd_config_save(g->store[f], g->path[f])) {
            g->save_pending[f] = false;
            saved = true;
        } else {
            snprintf(g->saved_note, sizeof(g->saved_note),
                     tr("SAVE FAILED: %s"), g->path[f]);
        }
    }
    if (saved) {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        snprintf(g->saved_note, sizeof(g->saved_note),
                 tr("Saved %02d:%02d:%02d - running apps apply edits live"),
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        g->draw_dirty = true;
    }
}

/* Def value accessors (store string is the truth; def->def is fallback). */
static double def_get_num(Gui *g, int file, const Def *d) {
    return cd_config_get_double(store_of(g, file), d->sec, d->key, atof(d->def));
}
static bool def_get_bool(Gui *g, int file, const Def *d) {
    return cd_config_get_bool(store_of(g, file), d->sec, d->key,
                              strcmp(d->def, "true") == 0);
}
static int def_get_enum(Gui *g, int file, const Def *d) {
    CdConfig *st = store_of(g, file);
    if (d->enum_is_string) {
        const char *v = cd_config_get_string(st, d->sec, d->key, d->def);
        for (int i = 0; i < d->enum_count; i++)
            if (strcmp(v, d->enums[i]) == 0) return i;
        return 0;
    }
    long v = cd_config_get_int(st, d->sec, d->key, atol(d->def));
    if (v < 0) v = 0;
    if (v >= d->enum_count) v = d->enum_count - 1;
    return (int)v;
}
static void def_set_num(Gui *g, int file, const Def *d, double v) {
    v = clampd(v, d->min, d->max);
    if (d->step > 0) v = floor(v / d->step + 0.5) * d->step;
    if (d->kind == K_INT || d->kind == K_INT_INPUT) cd_config_set_int(store_of(g, file), d->sec, d->key, (long)llround(v));
    else cd_config_set_double(store_of(g, file), d->sec, d->key, v);
    touch_file(g, file);
}
static void def_set_enum(Gui *g, int file, const Def *d, int idx) {
    idx = (int)clampd(idx, 0, d->enum_count - 1);
    if (d->enum_is_string) cd_config_set_string(store_of(g, file), d->sec, d->key, d->enums[idx]);
    else cd_config_set_int(store_of(g, file), d->sec, d->key, idx);
    touch_file(g, file);
}

/* Numeric text inputs. The VNC port stays whole-number-only; render_scale
 * is a typed float accepting digits, one decimal point and a simple
 * exponent. Enter/blur commits, Escape reverts; invalid or out-of-range text
 * is never written. */
static const Def *num_def(Gui *g) {
    if (g->num_idx < 0) return NULL;
    return g->num_file == FILE_G3 ? &G3_DEFS[g->num_idx] : &CD_DEFS[g->num_idx];
}
static bool num_parse(const Def *d, const char *s, double *out) {
    if (!s || !*s) return false;
    errno = 0;
    char *end = NULL;
    if (d->kind == K_FLOAT_INPUT) {
        double v = strtod(s, &end);
        if (errno || !end || end == s || *end || !isfinite(v)) return false;
        *out = v;
        return true;
    }
    long v = strtol(s, &end, 10);
    if (errno || !end || end == s || *end) return false;
    *out = (double)v;
    return true;
}
static bool num_value_valid(Gui *g, double *out) {
    const Def *d = num_def(g);
    double v;
    if (!d || !num_parse(d, g->num_field.buf, &v)) return false;
    if (d->kind == K_FLOAT_INPUT) {
        if (v < d->min || v > d->max) return false;
    } else if (v < (double)ceil(d->min) || v > (double)floor(d->max)) {
        return false;
    }
    if (out) *out = v;
    return true;
}
static void num_begin(Gui *g, int file, int idx) {
    const Def *d = file == FILE_G3 ? &G3_DEFS[idx] : &CD_DEFS[idx];
    g->num_file = file;
    g->num_idx = idx;
    if (d->kind == K_FLOAT_INPUT)
        snprintf(g->num_field.buf, sizeof(g->num_field.buf), "%.6g",
                 def_get_num(g, file, d));
    else
        snprintf(g->num_field.buf, sizeof(g->num_field.buf), "%lld",
                 llround(def_get_num(g, file, d)));
    g->num_field.caret = (int)strlen(g->num_field.buf);
    g->num_dirty = false;
    g->focus = -1;
    g->draw_dirty = true;
}
static bool num_commit(Gui *g) {
    const Def *d = num_def(g);
    if (!d) return true;
    if (!g->num_dirty) { g->num_idx = -1; g->draw_dirty = true; return true; }
    double v;
    if (!num_value_valid(g, &v)) {
        if (d->kind == K_FLOAT_INPUT)
            snprintf(g->saved_note, sizeof(g->saved_note),
                     tr("Enter a number from %g to %g"), d->min, d->max);
        else
            snprintf(g->saved_note, sizeof(g->saved_note),
                     tr("Enter a whole number from %.0f to %.0f"), d->min, d->max);
        g->draw_dirty = true;
        return false;
    }
    int file = g->num_file;
    g->num_idx = -1;
    g->num_dirty = false;
    def_set_num(g, file, d, v);
    flush_saves(g, true);
    return true;
}
static void num_cancel(Gui *g) {
    g->num_idx = -1;
    g->num_dirty = false;
    g->draw_dirty = true;
}

/* App-entry accessors (compositor store, [app.*] sections). */
static const char *app_str(Gui *g, int app, const char *key, const char *fallback) {
    const char *v = cd_config_get_string(store_of(g, FILE_CD), APP_DEFS[app].sec, key, NULL);
    return v ? v : (fallback ? fallback : "");
}
static int app_icon(Gui *g, int app) {
    long v = cd_config_get_int(store_of(g, FILE_CD), APP_DEFS[app].sec, "icon", APP_DEFS[app].def_icon);
    return (int)clampd((double)v, 0, 5);
}
static const char *action_key(int action) {
    return action == ACT_NEON ? "neon" : action == ACT_COCKPIT ? "cockpit" : "app";
}
static int app_action(Gui *g, int app) {
    const char *a = cd_config_get_string(store_of(g, FILE_CD), APP_DEFS[app].sec, "action", NULL);
    if (!a) return APP_DEFS[app].def_action;
    if (!strcmp(a, "neon")) return ACT_NEON;
    if (!strcmp(a, "cockpit")) return ACT_COCKPIT;
    if (!strcmp(a, "app")) return ACT_APP;
    return APP_DEFS[app].def_action;
}
static const char *action_desc(int action) {
    if (action == ACT_NEON) return "Built-in action: cycles the underglow colour (same as F4).";
    if (action == ACT_COCKPIT) return "Built-in action: resets the driver camera (same as F2).";
    return "";
}
static void app_load_fields(Gui *g, int app) {
    static const struct { int fld; const char *key; const char *def; } map[FLD_COUNT] = { 0 };
    (void)map;
    const AppDef *d = &APP_DEFS[app];
    snprintf(g->fields[FLD_NAME].buf, sizeof(g->fields[0].buf), "%s", app_str(g, app, "name", d->def_name));
    snprintf(g->fields[FLD_SUB].buf, sizeof(g->fields[0].buf), "%s", app_str(g, app, "sub", d->def_sub));
    snprintf(g->fields[FLD_EXEC].buf, sizeof(g->fields[0].buf), "%s", app_str(g, app, "exec", d->def_exec));
    snprintf(g->fields[FLD_APPID].buf, sizeof(g->fields[0].buf), "%s", app_str(g, app, "app_id", d->def_app_id));
    for (int i = 0; i < FLD_COUNT; i++) g->fields[i].caret = (int)strlen(g->fields[i].buf);
    g->field_app = app;
    g->field_dirty = false;
}
static void app_commit_fields(Gui *g) {
    if (!g->field_dirty || g->field_app < 0) return;
    int app = g->field_app;
    CdConfig *st = store_of(g, FILE_CD);
    const char *sec = APP_DEFS[app].sec;
    if (*g->fields[FLD_NAME].buf) cd_config_set_string(st, sec, "name", g->fields[FLD_NAME].buf);
    cd_config_set_string(st, sec, "sub", g->fields[FLD_SUB].buf);
    if (app_action(g, app) == ACT_APP) {
        cd_config_set_string(st, sec, "exec", g->fields[FLD_EXEC].buf);
        cd_config_set_string(st, sec, "app_id", g->fields[FLD_APPID].buf);
    }
    g->field_dirty = false;
    touch_file(g, FILE_CD);
    flush_saves(g, true);
}
static void app_select(Gui *g, int app) {
    if (app == g->sel_app && g->field_app == app) return;
    app_commit_fields(g);
    g->sel_app = app;
    g->focus = -1;
    app_load_fields(g, app);
    g->draw_dirty = true;
}

/* ------------------------------------------------ installed-app catalog */

#define PICKER_VISIBLE 11

static bool choice_push(Gui *g, const AppChoice *ch) {
    if (g->choice_count == g->choice_cap) {
        int cap = g->choice_cap ? g->choice_cap * 2 : 64;
        AppChoice *next = realloc(g->choices, (size_t)cap * sizeof(*next));
        if (!next) return false;
        g->choices = next;
        g->choice_cap = cap;
    }
    g->choices[g->choice_count++] = *ch;
    return true;
}
static bool push_choice_values(Gui *g, int kind, const char *name, const char *comment,
                               const char *exec, const char *app_id,
                               const char *desktop_id, int icon) {
    AppChoice ch;
    memset(&ch, 0, sizeof(ch));
    snprintf(ch.name, sizeof(ch.name), "%s", name ? name : "");
    snprintf(ch.comment, sizeof(ch.comment), "%s", comment ? comment : "");
    snprintf(ch.exec, sizeof(ch.exec), "%s", exec ? exec : "");
    snprintf(ch.app_id, sizeof(ch.app_id), "%s", app_id ? app_id : "");
    snprintf(ch.desktop_id, sizeof(ch.desktop_id), "%s", desktop_id ? desktop_id : "");
    ch.kind = kind;
    ch.icon = (int)clampd(icon, 0, 5);
    return choice_push(g, &ch);
}
static void trim_inplace(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t b = 0;
    while (s[b] && isspace((unsigned char)s[b])) b++;
    if (b) memmove(s, s + b, strlen(s + b) + 1);
}
static bool desktop_bool(const char *v) {
    return v && (!strcasecmp(v, "true") || !strcmp(v, "1"));
}
static void desktop_unescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && r[1]) {
            r++;
            *w++ = *r == 's' ? ' ' : *r == 'n' ? '\n' : *r == 't' ? '\t' :
                   *r == 'r' ? '\r' : *r;
            r++;
        } else *w++ = *r++;
    }
    *w = 0;
}
static bool localized_key(const char *key, const char *base, const char *lang) {
    size_t bl = strlen(base);
    if (!lang || strlen(lang) < 2) return false;
    if (strncmp(key, base, bl) != 0 || key[bl] != '[') return false;
    return strncmp(key + bl + 1, lang, 2) == 0 &&
           (key[bl + 3] == ']' || key[bl + 3] == '_' || key[bl + 3] == '@');
}
static int infer_icon(const char *categories, const char *name, const char *exec) {
    const char *all[3] = { categories, name, exec };
    static const struct { const char *needle; int icon; } map[] = {
        { "TerminalEmulator", 0 }, { "Development", 0 }, { "ConsoleOnly", 0 },
        { "AudioVideo", 2 }, { "Audio", 2 }, { "Music", 2 },
        { "FileManager", 1 }, { "FileTools", 1 },
        { "Graphics", 4 }, { "Photography", 4 }, { "Image", 4 },
        { "Game", 5 }, { "3DGraphics", 5 },
        { "Monitor", 3 }, { "Settings", 3 }, { "System", 3 },
        { "Network", 1 }, { "WebBrowser", 1 },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        for (int j = 0; j < 3; j++)
            if (all[j] && strcasestr(all[j], map[i].needle)) return map[i].icon;
    return 0;
}
static bool program_available(const char *prog) {
    if (!prog || !*prog) return true;
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy) return true;
    bool found = false;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
        char full[4096];
        if (snprintf(full, sizeof(full), "%s/%s", *dir ? dir : ".", prog) < (int)sizeof(full))
            found = access(full, X_OK) == 0;
    }
    free(copy);
    return found;
}
static void clean_desktop_exec(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    if (!outsz) return;
    for (const char *p = in ? in : ""; *p && o + 1 < outsz; p++) {
        if (*p == '%' && p[1]) {
            if (p[1] == '%') { out[o++] = '%'; p++; continue; }
            if (strchr("fFuUdDnNickvm", p[1])) { p++; continue; }
        }
        out[o++] = *p;
    }
    out[o] = 0;
    trim_inplace(out);
}
static bool shell_quote(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    if (outsz < 3) return false;
    out[o++] = '\'';
    for (const char *p = in ? in : ""; *p; p++) {
        if (*p == '\'') {
            if (o + 5 >= outsz) return false;
            memcpy(out + o, "'\\''", 4); o += 4;
        } else {
            if (o + 2 >= outsz) return false;
            out[o++] = *p;
        }
    }
    if (o + 2 > outsz) return false;
    out[o++] = '\'';
    out[o] = 0;
    return true;
}
static bool parse_desktop_file(const char *path, const char *desktop_id,
                               const char *lang, AppChoice *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char name[128] = "", loc_name[128] = "", comment[192] = "", loc_comment[192] = "";
    char generic[192] = "", exec_raw[512] = "", tryexec[512] = "", wmclass[256] = "";
    char categories[256] = "", type[64] = "";
    bool in_entry = false, nodisplay = false, hidden = false, terminal = false;
    bool dbus_activatable = false;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (!*s || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) *e = 0;
            in_entry = strcmp(s + 1, "Desktop Entry") == 0;
            continue;
        }
        if (!in_entry) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = s, *value = eq + 1;
        trim_inplace(key);
        trim_inplace(value);
        if (!strcmp(key, "Type")) snprintf(type, sizeof(type), "%s", value);
        else if (!strcmp(key, "Name")) snprintf(name, sizeof(name), "%s", value);
        else if (localized_key(key, "Name", lang)) snprintf(loc_name, sizeof(loc_name), "%s", value);
        else if (!strcmp(key, "Comment")) snprintf(comment, sizeof(comment), "%s", value);
        else if (localized_key(key, "Comment", lang)) snprintf(loc_comment, sizeof(loc_comment), "%s", value);
        else if (!strcmp(key, "GenericName")) snprintf(generic, sizeof(generic), "%s", value);
        else if (!strcmp(key, "Exec")) snprintf(exec_raw, sizeof(exec_raw), "%s", value);
        else if (!strcmp(key, "TryExec")) snprintf(tryexec, sizeof(tryexec), "%s", value);
        else if (!strcmp(key, "StartupWMClass")) snprintf(wmclass, sizeof(wmclass), "%s", value);
        else if (!strcmp(key, "Categories")) snprintf(categories, sizeof(categories), "%s", value);
        else if (!strcmp(key, "NoDisplay")) nodisplay = desktop_bool(value);
        else if (!strcmp(key, "Hidden")) hidden = desktop_bool(value);
        else if (!strcmp(key, "Terminal")) terminal = desktop_bool(value);
        else if (!strcmp(key, "DBusActivatable")) dbus_activatable = desktop_bool(value);
    }
    fclose(f);
    if (strcmp(type, "Application") || nodisplay || hidden) return false;
    if (!*name && !*loc_name) return false;
    char clean[512];
    clean_desktop_exec(exec_raw, clean, sizeof(clean));
    if (!program_available(tryexec)) return false;
    if (!*clean && !(dbus_activatable && program_available("gio"))) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", *loc_name ? loc_name : name);
    snprintf(out->comment, sizeof(out->comment), "%s",
             *loc_comment ? loc_comment : *comment ? comment : generic);
    desktop_unescape(out->name);
    desktop_unescape(out->comment);
    if (!*clean) {
        /* DBusActivatable entries may legitimately have no Exec key. */
        char qpath[4400];
        if (!shell_quote(path, qpath, sizeof(qpath)) ||
            snprintf(out->exec, sizeof(out->exec), "gio launch %s", qpath) >= (int)sizeof(out->exec))
            return false;
    } else if (terminal) {
        char qexec[1024], qname[512];
        if (!shell_quote(clean, qexec, sizeof(qexec)) ||
            !shell_quote(out->name, qname, sizeof(qname)) ||
            snprintf(out->exec, sizeof(out->exec), "foot --title=%s -- sh -lc %s", qname, qexec) >= (int)sizeof(out->exec))
            snprintf(out->exec, sizeof(out->exec), "%s", clean);
    } else snprintf(out->exec, sizeof(out->exec), "%s", clean);
    snprintf(out->app_id, sizeof(out->app_id), "%s", *wmclass ? wmclass : desktop_id);
    snprintf(out->desktop_id, sizeof(out->desktop_id), "%s", desktop_id);
    snprintf(out->categories, sizeof(out->categories), "%s", categories);
    out->kind = CHOICE_APP;
    out->icon = infer_icon(categories, out->name, clean);
    out->terminal = terminal;
    return true;
}
/* Desktop-file IDs already claimed by a higher-precedence directory. An ID
 * is claimed before parsing, so a user-level Hidden/NoDisplay override masks
 * the system copy instead of letting it reappear in the picker. */
typedef struct {
    char (*ids)[256];
    int count, cap;
} DesktopSeen;
static bool desktop_seen_contains(const DesktopSeen *seen, const char *id) {
    for (int i = 0; i < seen->count; i++)
        if (!strcmp(seen->ids[i], id)) return true;
    return false;
}
static bool desktop_seen_add(DesktopSeen *seen, const char *id) {
    if (desktop_seen_contains(seen, id)) return true;
    if (seen->count == seen->cap) {
        int cap = seen->cap ? seen->cap * 2 : 128;
        char (*next)[256] = realloc(seen->ids, (size_t)cap * sizeof(*next));
        if (!next) return false;
        seen->ids = next;
        seen->cap = cap;
    }
    snprintf(seen->ids[seen->count++], sizeof(seen->ids[0]), "%s", id);
    return true;
}
/* Recurse a little: desktop files may live in subdirectories, in which case
 * the relative path with '-' for '/' is their desktop-file ID. */
static void scan_desktop_dir(Gui *g, const char *dir, const char *id_prefix,
                             const char *lang, DesktopSeen *seen, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue; /* ., .., and hidden dirs/files */
        char path[4096];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth >= 4) continue;
            char child_prefix[256];
            if (id_prefix && *id_prefix) {
                if (snprintf(child_prefix, sizeof(child_prefix), "%s-%s", id_prefix, e->d_name) >= (int)sizeof(child_prefix)) continue;
            } else {
                if (snprintf(child_prefix, sizeof(child_prefix), "%s", e->d_name) >= (int)sizeof(child_prefix)) continue;
            }
            scan_desktop_dir(g, path, child_prefix, lang, seen, depth + 1);
            continue;
        }
        size_t n = strlen(e->d_name);
        if (n <= 8 || strcmp(e->d_name + n - 8, ".desktop")) continue;
        char desktop_id[256], base_id[256];
        if (n - 8 >= sizeof(base_id)) continue;
        memcpy(base_id, e->d_name, n - 8);
        base_id[n - 8] = 0;
        if (id_prefix && *id_prefix) {
            if (snprintf(desktop_id, sizeof(desktop_id), "%s-%s", id_prefix, base_id) >= (int)sizeof(desktop_id)) continue;
        } else {
            if (snprintf(desktop_id, sizeof(desktop_id), "%s", base_id) >= (int)sizeof(desktop_id)) continue;
        }
        if (desktop_seen_contains(seen, desktop_id)) continue;
        if (!desktop_seen_add(seen, desktop_id)) break;
        /* Claim before this check so a higher-precedence .desktop symlink to
         * /dev/null (the usual "hide this app" override) still masks the ID. */
        if (!S_ISREG(st.st_mode)) continue;
        AppChoice ch;
        if (parse_desktop_file(path, desktop_id, lang, &ch) && !choice_push(g, &ch)) break;
    }
    closedir(d);
}
static void scan_applications_base(Gui *g, const char *base, const char *lang, DesktopSeen *seen) {
    if (!base || !*base) return;
    char dir[4096];
    if (snprintf(dir, sizeof(dir), "%s/applications", base) < (int)sizeof(dir))
        scan_desktop_dir(g, dir, "", lang, seen, 0);
}
static int choice_cmp(const void *a, const void *b) {
    int c = strcasecmp(((const AppChoice *)a)->name, ((const AppChoice *)b)->name);
    return c ? c : strcmp(((const AppChoice *)a)->desktop_id, ((const AppChoice *)b)->desktop_id);
}
static bool catalog_load(Gui *g) {
    /* Built-ins first: these remain assignable even on a host with no
     * desktop-entry database at all. */
    if (!push_choice_values(g, CHOICE_NEON, "Built-in - Cycle underglow", "ChromeDrive action", "", "", "chromedrive.builtin.neon", 4) ||
        !push_choice_values(g, CHOICE_COCKPIT, "Built-in - Driver view", "ChromeDrive action", "", "", "chromedrive.builtin.cockpit", 5) ||
        !push_choice_values(g, CHOICE_APP, "TERMINAL", "SHELL", APP_DEFS[0].def_exec, "foot", "chromedrive.terminal", 0) ||
        !push_choice_values(g, CHOICE_APP, "FILES", "EXPLORE", APP_DEFS[1].def_exec, "chromedrive.files", "chromedrive.files", 1) ||
        !push_choice_values(g, CHOICE_APP, "AUDIO", "SESSION", APP_DEFS[2].def_exec, "chromedrive.audio", "chromedrive.audio", 2) ||
        !push_choice_values(g, CHOICE_APP, "METERS", "LIVE", APP_DEFS[3].def_exec, "chromedrive.telemetry", "chromedrive.meters", 3) ||
        !push_choice_values(g, CHOICE_APP, "Garage3D", "Native 3D garage", "'%root%/clients/garage3d/bin/garage3d'", "chromedrive.garage3d", "chromedrive.garage3d", 5) ||
        !push_choice_values(g, CHOICE_APP, "ChromeDrive Settings", "Launch Menu and settings", "'%root%/bin/chromedrive-config'", "chromedrive.config", "chromedrive.settings", 3))
        return false;
    const char *loc = getenv("LC_ALL");
    if (!loc || !*loc) loc = getenv("LC_MESSAGES");
    if (!loc || !*loc) loc = getenv("LANG");
    char lang[16] = "";
    if (loc) {
        size_t i = 0;
        while (loc[i] && loc[i] != '.' && loc[i] != '@' && i + 1 < sizeof(lang)) { lang[i] = loc[i]; i++; }
        lang[i] = 0;
    }
    int desktop_start = g->choice_count;
    DesktopSeen seen = { 0 };
    for (int i = 0; i < g->choice_count; i++)
        if (!desktop_seen_add(&seen, g->choices[i].desktop_id)) { free(seen.ids); return false; }
    const char *home = getenv("HOME");
    const char *data_home = getenv("XDG_DATA_HOME");
    if (data_home && *data_home) scan_applications_base(g, data_home, lang, &seen);
    else if (home) {
        char base[4096];
        if (snprintf(base, sizeof(base), "%s/.local/share", home) < (int)sizeof(base))
            scan_applications_base(g, base, lang, &seen);
    }
    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (!data_dirs || !*data_dirs) data_dirs = "/usr/local/share:/usr/share";
    char *copy = strdup(data_dirs);
    if (copy) {
        char *save = NULL;
        for (char *base = strtok_r(copy, ":", &save); base; base = strtok_r(NULL, ":", &save))
            scan_applications_base(g, base, lang, &seen);
        free(copy);
    }
    /* Flatpak/Snap export dirs are normally in XDG_DATA_DIRS, but a bare
     * SSH/headless environment often lacks a desktop-session environment.
     * Seen-ID masking makes these fallbacks harmless if already scanned. */
    if (home) {
        char base[4096];
        if (snprintf(base, sizeof(base), "%s/.local/share/flatpak/exports/share", home) < (int)sizeof(base))
            scan_applications_base(g, base, lang, &seen);
    }
    scan_applications_base(g, "/var/lib/flatpak/exports/share", lang, &seen);
    scan_applications_base(g, "/var/lib/snapd/desktop", lang, &seen);
    free(seen.ids);
    if (g->choice_count > desktop_start)
        qsort(g->choices + desktop_start, (size_t)(g->choice_count - desktop_start),
              sizeof(AppChoice), choice_cmp);
    return true;
}

static bool choice_matches(const AppChoice *ch, const char *filter) {
    if (!filter || !*filter) return true;
    return strcasestr(ch->name, filter) || strcasestr(ch->comment, filter) ||
           strcasestr(ch->app_id, filter) || strcasestr(ch->desktop_id, filter) ||
           strcasestr(ch->exec, filter);
}
static int picker_count(Gui *g) {
    int n = 0;
    for (int i = 0; i < g->choice_count; i++)
        if (choice_matches(&g->choices[i], g->picker_filter)) n++;
    return n;
}
static AppChoice *picker_at(Gui *g, int ordinal, int *catalog_idx) {
    int n = 0;
    for (int i = 0; i < g->choice_count; i++) {
        if (!choice_matches(&g->choices[i], g->picker_filter)) continue;
        if (n++ == ordinal) {
            if (catalog_idx) *catalog_idx = i;
            return &g->choices[i];
        }
    }
    return NULL;
}
static bool choice_is_current(Gui *g, int slot, const AppChoice *ch) {
    int act = app_action(g, slot);
    if (act == ACT_NEON) return ch->kind == CHOICE_NEON;
    if (act == ACT_COCKPIT) return ch->kind == CHOICE_COCKPIT;
    if (ch->kind != CHOICE_APP) return false;
    const char *exec = app_str(g, slot, "exec", APP_DEFS[slot].def_exec);
    return exec && !strcmp(exec, ch->exec);
}
static void picker_clamp(Gui *g) {
    int n = picker_count(g);
    if (n <= 0) { g->picker_sel = 0; g->picker_scroll = 0; return; }
    if (g->picker_sel < 0) g->picker_sel = 0;
    if (g->picker_sel >= n) g->picker_sel = n - 1;
    int max_scroll = n > PICKER_VISIBLE ? n - PICKER_VISIBLE : 0;
    if (g->picker_scroll > max_scroll) g->picker_scroll = max_scroll;
    if (g->picker_scroll < 0) g->picker_scroll = 0;
    if (g->picker_sel < g->picker_scroll) g->picker_scroll = g->picker_sel;
    if (g->picker_sel >= g->picker_scroll + PICKER_VISIBLE)
        g->picker_scroll = g->picker_sel - PICKER_VISIBLE + 1;
}
static void picker_close(Gui *g) {
    g->picker_open = false;
    g->draw_dirty = true;
}
static void picker_open(Gui *g, int slot, double x, double y) {
    if (slot < 0 || slot > 5) slot = g->sel_app;
    app_commit_fields(g);
    if (g->num_idx >= 0 && !num_commit(g)) num_cancel(g);
    if (g->sel_app != slot || g->field_app != slot) app_select(g, slot);
    g->tab = TAB_APPS;
    g->picker_open = true;
    g->picker_slot = slot;
    g->picker_x = x;
    g->picker_y = y;
    g->picker_filter[0] = 0;
    g->picker_sel = 0;
    g->picker_scroll = 0;
    for (int i = 0; i < g->choice_count; i++) {
        if (choice_is_current(g, slot, &g->choices[i])) { g->picker_sel = i; break; }
    }
    picker_clamp(g);
    g->focus = -1;
    g->draw_dirty = true;
}
/* A running Settings window cannot receive new argv when the compositor's
 * cog/right-click handler wants to deep-link it to a slot. The compositor
 * therefore atomically writes a one-line request ("<slot> <nonce>") in the
 * private runtime dir; run_gui polls it and opens the picker. The filename
 * is keyed by the Wayland socket basename to avoid cross-session requests. */
static bool picker_request_read(Gui *g, int *slot, char nonce[64]) {
    if (!*g->picker_request_path) return false;
    FILE *f = fopen(g->picker_request_path, "r");
    if (!f) return false;
    int s = -1;
    char n[64] = "";
    bool ok = fscanf(f, "%d %63s", &s, n) == 2 && s >= 0 && s < 6 && *n;
    fclose(f);
    if (!ok) return false;
    *slot = s;
    snprintf(nonce, 64, "%s", n);
    return true;
}
static void picker_request_prime(Gui *g) {
    int slot;
    char nonce[64];
    if (picker_request_read(g, &slot, nonce))
        snprintf(g->picker_request_seen, sizeof(g->picker_request_seen), "%s", nonce);
    struct stat st;
    if (*g->picker_request_path && stat(g->picker_request_path, &st) == 0) {
        g->picker_request_mtime = st.st_mtim.tv_sec;
        g->picker_request_mtime_ns = st.st_mtim.tv_nsec;
    }
}
static void picker_request_set_path(Gui *g, const char *path) {
    snprintf(g->picker_request_path, sizeof(g->picker_request_path), "%s", path ? path : "");
    g->picker_request_seen[0] = 0;
    g->picker_request_mtime = 0;
    g->picker_request_mtime_ns = 0;
    picker_request_prime(g);
}
static void picker_request_init(Gui *g) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char runtime_buf[4096];
    if (!runtime || !*runtime) {
        snprintf(runtime_buf, sizeof(runtime_buf), "/tmp/chromedrive-%d", (int)getuid());
        runtime = runtime_buf;
    }
    const char *display = getenv("WAYLAND_DISPLAY");
    if (!display || !*display) display = "wayland-0";
    const char *base = strrchr(display, '/');
    base = base ? base + 1 : display;
    char safe[128];
    size_t j = 0;
    for (size_t i = 0; base[i] && j + 1 < sizeof(safe); i++) {
        unsigned char ch = (unsigned char)base[i];
        safe[j++] = (isalnum(ch) || ch == '.' || ch == '_' || ch == '-') ? (char)ch : '_';
    }
    safe[j] = 0;
    if (!*safe) snprintf(safe, sizeof(safe), "wayland-0");
    char path[4400];
    if (snprintf(path, sizeof(path), "%s/chromedrive-config.%s.picker-request", runtime, safe) < (int)sizeof(path))
        picker_request_set_path(g, path);
}
static void picker_request_poll(Gui *g) {
    if (!*g->picker_request_path) return;
    struct stat st;
    if (stat(g->picker_request_path, &st) != 0) return;
    if (st.st_mtim.tv_sec == g->picker_request_mtime &&
        st.st_mtim.tv_nsec == g->picker_request_mtime_ns) return;
    g->picker_request_mtime = st.st_mtim.tv_sec;
    g->picker_request_mtime_ns = st.st_mtim.tv_nsec;
    int slot;
    char nonce[64];
    if (!picker_request_read(g, &slot, nonce)) return;
    if (!strcmp(nonce, g->picker_request_seen)) return;
    snprintf(g->picker_request_seen, sizeof(g->picker_request_seen), "%s", nonce);
    picker_open(g, slot, 292, 92);
}

static int utf8_prefix_len(const char *s, int max_bytes) {
    int len = (int)strlen(s);
    int n = len < max_bytes ? len : max_bytes;
    if (n < len) while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
    return n;
}
static void picker_assign(Gui *g, int catalog_idx) {
    if (catalog_idx < 0 || catalog_idx >= g->choice_count) return;
    AppChoice *ch = &g->choices[catalog_idx];
    int slot = g->picker_slot;
    if (g->sel_app != slot || g->field_app != slot) app_select(g, slot);
    CdConfig *st = store_of(g, FILE_CD);
    const char *sec = APP_DEFS[slot].sec;
    if (ch->kind == CHOICE_NEON || ch->kind == CHOICE_COCKPIT) {
        int act = ch->kind == CHOICE_NEON ? ACT_NEON : ACT_COCKPIT;
        char sub[64];
        snprintf(sub, sizeof(sub), "%02d / %s", slot + 1, act == ACT_NEON ? "COLOR" : "HOME");
        cd_config_set_string(st, sec, "action", action_key(act));
        cd_config_set_string(st, sec, "name", act == ACT_NEON ? "NEON" : "COCKPIT");
        cd_config_set_string(st, sec, "sub", sub);
        cd_config_set_string(st, sec, "exec", "");
        cd_config_set_string(st, sec, "app_id", "");
        cd_config_set_int(st, sec, "icon", act == ACT_NEON ? 4 : 5);
    } else {
        char sub[96];
        const char *detail = *ch->comment ? ch->comment : *ch->app_id ? ch->app_id : "APP";
        snprintf(sub, sizeof(sub), "%02d / %.*s", slot + 1,
                 utf8_prefix_len(detail, 28), detail);
        for (char *p = sub; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
        cd_config_set_string(st, sec, "action", "app");
        cd_config_set_string(st, sec, "name", ch->name);
        cd_config_set_string(st, sec, "sub", sub);
        cd_config_set_string(st, sec, "exec", ch->exec);
        cd_config_set_string(st, sec, "app_id", ch->app_id);
        cd_config_set_int(st, sec, "icon", ch->icon);
    }
    cd_config_set_bool(st, sec, "enabled", true);
    char assigned_name[128];
    snprintf(assigned_name, sizeof(assigned_name), "%s", ch->name);
    g->picker_open = false;
    g->focus = -1;
    app_load_fields(g, slot);
    touch_file(g, FILE_CD);
    flush_saves(g, true);
    if (!g->save_pending[FILE_CD]) {
        /* Width-limited "%.*s" counts bytes and cannot translate the name, so
         * truncate with the UTF-8-safe helper and hand tr() a plain %s. */
        char shown[64];
        i18n_utf8_truncate(shown, sizeof(shown), tr(assigned_name), 52);
        snprintf(g->saved_note, sizeof(g->saved_note),
                 tr("Assigned %s to Launch Control %d"), shown, slot + 1);
    }
    g->draw_dirty = true;
}
static void picker_geometry(Gui *g, double *x, double *y, double *w, double *h) {
    *w = 560;
    *h = 486;
    *x = clampd(g->picker_x, 12, 1000 - *w - 12);
    *y = clampd(g->picker_y, 58, 625 - *h - 10);
}

/* ------------------------------------------------------------ cairo paint */

static void rr(cairo_t *c, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(c);
    cairo_arc(c, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(c, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(c, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(c, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(c);
}
static void fg(cairo_t *c, double r, double g, double b) { cairo_set_source_rgb(c, r, g, b); }
static void fga(cairo_t *c, double r, double g, double b, double a) { cairo_set_source_rgba(c, r, g, b, a); }
static void font(cairo_t *c, double size, bool bold, bool mono) {
    cairo_select_font_face(c, mono ? "DejaVu Sans Mono" : "DejaVu Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(c, size);
}
static void txt(cairo_t *c, double x, double y, const char *s) {
    cairo_move_to(c, x, y);
    cairo_show_text(c, s);
}
static double txt_w(cairo_t *c, const char *s) {
    cairo_text_extents_t e;
    cairo_text_extents(c, s, &e);
    return e.x_advance;
}
static void txt_r(cairo_t *c, double xr, double y, const char *s) {
    txt(c, xr - txt_w(c, s), y, s);
}
/* Truncate with ".." until it fits maxw (toy API: byte-wise is fine for the
 * mostly-ASCII labels/paths shown here). */
static void txt_fit(cairo_t *c, double x, double y, double maxw, const char *s) {
    if (txt_w(c, s) <= maxw) { txt(c, x, y, s); return; }
    char buf[640];
    snprintf(buf, sizeof(buf), "%s", s);
    for (int n = (int)strlen(buf); n > 3; n--) {
        /* Back off over UTF-8 continuation bytes so a truncation can never
         * split a codepoint (Russian already, CJK later). */
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;
        buf[n] = 0;
        char tmp[644];
        snprintf(tmp, sizeof(tmp), "%s..", buf);
        if (txt_w(c, tmp) <= maxw) { txt(c, x, y, tmp); return; }
    }
}

/* The six cockpit glyphs, ported from src/ui.c icon() so previews match. */
static void draw_glyph(cairo_t *c, int i, double x, double y, double scale) {
    cairo_save(c);
    cairo_translate(c, x, y);
    cairo_scale(c, scale, scale);
    /* cairo_arc() lines from the current point to the arc start if a path
     * exists - and cairo_show_text() leaves one (in device space). The
     * Sun/Wheel glyphs start with arc(), so without this they grow a long
     * stray line back to the last text drawn. */
    cairo_new_path(c);
    cairo_set_line_width(c, 3);
    cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
    fg(c, .48, .89, .78);
    if (i == 0) {
        cairo_move_to(c, 0, 0); cairo_line_to(c, 13, 11); cairo_line_to(c, 0, 22); cairo_stroke(c);
        cairo_move_to(c, 18, 22); cairo_line_to(c, 34, 22); cairo_stroke(c);
    } else if (i == 1) {
        cairo_move_to(c, 0, 24); cairo_line_to(c, 0, 0); cairo_line_to(c, 13, 0);
        cairo_line_to(c, 18, 6); cairo_line_to(c, 35, 6); cairo_line_to(c, 35, 24);
        cairo_close_path(c); cairo_stroke(c);
    } else if (i == 2) {
        cairo_move_to(c, 10, 22); cairo_line_to(c, 10, -2); cairo_line_to(c, 34, -7);
        cairo_line_to(c, 34, 17); cairo_stroke(c);
        cairo_arc(c, 4, 23, 6, 0, 2 * M_PI); cairo_fill(c);
        cairo_arc(c, 28, 18, 6, 0, 2 * M_PI); cairo_fill(c);
    } else if (i == 3) {
        for (int n = 0; n < 4; n++) { cairo_rectangle(c, n * 10, 25 - n * 8, 5, n * 8 + 3); cairo_fill(c); }
    } else if (i == 4) {
        cairo_arc(c, 17, 12, 11, 0, 2 * M_PI); cairo_stroke(c);
        for (int n = 0; n < 8; n++) {
            double a = n * M_PI / 4;
            cairo_move_to(c, 17 + 18 * cos(a), 12 + 18 * sin(a));
            cairo_line_to(c, 17 + 22 * cos(a), 12 + 22 * sin(a)); cairo_stroke(c);
        }
    } else {
        cairo_arc(c, 17, 12, 18, 0, 2 * M_PI); cairo_stroke(c);
        cairo_arc(c, 17, 12, 5, 0, 2 * M_PI); cairo_stroke(c);
        cairo_move_to(c, 0, 12); cairo_line_to(c, 12, 12); cairo_stroke(c);
        cairo_move_to(c, 22, 12); cairo_line_to(c, 35, 12); cairo_stroke(c);
        cairo_move_to(c, 17, 18); cairo_line_to(c, 17, 29); cairo_stroke(c);
    }
    cairo_restore(c);
}

static void add_box(Gui *g, int kind, int a, int b, int cc,
                    double x, double y, double w, double h) {
    if (g->box_count >= (int)(sizeof(g->boxes) / sizeof(g->boxes[0]))) return;
    g->boxes[g->box_count++] = (HitBox){ x, y, w, h, kind, a, b, cc };
}
static HitBox *find_box(Gui *g, int kind, int a, int b) {
    for (int i = 0; i < g->box_count; i++)
        if (g->boxes[i].kind == kind && g->boxes[i].a == a && g->boxes[i].b == b)
            return &g->boxes[i];
    return NULL;
}

static void draw_checkbox(cairo_t *c, double x, double y, bool on) {
    fga(c, .10, .14, .22, 1); rr(c, x, y, 20, 20, 5); cairo_fill(c);
    fg(c, .30, .40, .55); cairo_set_line_width(c, 1.2); rr(c, x, y, 20, 20, 5); cairo_stroke(c);
    if (on) {
        fg(c, .25, .92, .68); cairo_set_line_width(c, 2.6);
        cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(c, x + 4.5, y + 10.5); cairo_line_to(c, x + 8.5, y + 15);
        cairo_line_to(c, x + 16, y + 5.5); cairo_stroke(c);
    }
}
static void draw_restart_chip(cairo_t *c, double x, double y) {
    fga(c, .45, .30, .10, .9); rr(c, x, y - 11, 58, 15, 4); cairo_fill(c);
    font(c, 8.5, true, false); fg(c, 1, .80, .45); txt(c, x + 6, y, tr("RESTART"));
}

/* One slider track+knob; returns nothing, registers the hitbox. */
static void draw_slider(Gui *g, cairo_t *c, int kind, int a, int b, int cc,
                        double x, double y, double w, double frac) {
    frac = clampd(frac, 0, 1);
    fga(c, .13, .18, .27, 1); rr(c, x, y - 3, w, 6, 3); cairo_fill(c);
    fga(c, .20, .75, .85, 1); rr(c, x, y - 3, w * frac, 6, 3); cairo_fill(c);
    double kx = x + w * frac;
    fg(c, .85, .95, 1.0); cairo_arc(c, kx, y, 8, 0, 2 * M_PI); cairo_fill(c);
    fg(c, .10, .35, .45); cairo_arc(c, kx, y, 3.5, 0, 2 * M_PI); cairo_fill(c);
    add_box(g, kind, a, b, cc, x - 6, y - 12, w + 12, 24);
}

static void draw_schema_tab(Gui *g, cairo_t *c, int file, const Def *defs, int n,
                            double top, double bottom) {
    double y = top - g->scroll[g->tab];
    const char *last_group = "";
    cairo_save(c);
    cairo_rectangle(c, 0, top, 1000, bottom - top);
    cairo_clip(c);
    for (int i = 0; i < n; i++) {
        const Def *d = &defs[i];
        if (strcmp(d->group, last_group) != 0) {
            last_group = d->group;
            y += 12;
            font(c, 12, true, false); fg(c, .45, .85, .95);
            txt(c, 28, y + 12, tr(d->group));
            fga(c, .25, .35, .50, .6); cairo_set_line_width(c, 1);
            cairo_move_to(c, 28, y + 20); cairo_line_to(c, 952, y + 20); cairo_stroke(c);
            y += 30;
        }
        double ry = y;
        double row_h = 34;
        if (d->kind == K_COLOR && g->exp_color_file == file && g->exp_color_idx == i)
            row_h = 34 + 3 * 26;
        if (ry + row_h >= top - 40 && ry <= bottom + 40) {
            /* row background on hover-ish alternation */
            if (i % 2 == 0) { fga(c, 1, 1, 1, .022); rr(c, 18, ry - 4, 944, row_h, 6); cairo_fill(c); }
            font(c, 14.5, false, false); fg(c, .82, .88, .96);
            /* Translate at draw time, never in the table: d->sec/d->key are
             * config keys, and when enum_is_string the *stored* value is the
             * English label (def_get_enum compares against it verbatim). */
            const char *label = tr(d->label);
            txt_fit(c, 28, ry + 20, 285, label);
            if (d->restart) draw_restart_chip(c, 28 + txt_w(c, label) + 10, ry + 20);
            double vx = 340, vw = 420; /* control lane */
            char val[64];
            switch (d->kind) {
            case K_BOOL: {
                bool on = def_get_bool(g, file, d);
                draw_checkbox(c, vx, ry + 4, on);
                font(c, 13, false, false); fg(c, .65, .74, .88);
                txt(c, vx + 30, ry + 19, on ? tr("On") : tr("Off"));
                add_box(g, H_TOGGLE, file, i, 0, vx - 4, ry, 90, 28);
                break;
            }
            case K_FLOAT: case K_INT: case K_INT_INPUT: case K_FLOAT_INPUT: {
                double v = def_get_num(g, file, d);
                if (d->kind == K_INT_INPUT || d->kind == K_FLOAT_INPUT) {
                    bool active = g->num_file == file && g->num_idx == i;
                    bool invalid = active && g->num_dirty && !num_value_valid(g, NULL);
                    snprintf(val, sizeof(val), d->fmt, v);
                    const char *shown = active ? g->num_field.buf : val;
                    fga(c, .07, .10, .17, 1); rr(c, vx, ry + 1, 180, 28, 6); cairo_fill(c);
                    cairo_set_line_width(c, active ? 2 : 1.2);
                    if (invalid) fg(c, .95, .38, .32);
                    else if (active) fg(c, .30, .85, .95);
                    else fg(c, .28, .38, .55);
                    rr(c, vx, ry + 1, 180, 28, 6); cairo_stroke(c);
                    font(c, 14, false, true); fg(c, .88, .94, 1.0);
                    txt(c, vx + 11, ry + 20, shown);
                    if (active && (int)(now_s() * 1.9) % 2 == 0) {
                        int caret = g->num_field.caret;
                        int len = (int)strlen(g->num_field.buf);
                        if (caret < 0) caret = 0;
                        if (caret > len) caret = len;
                        char save = g->num_field.buf[caret];
                        g->num_field.buf[caret] = 0;
                        double cx = vx + 11 + txt_w(c, g->num_field.buf) + 1;
                        g->num_field.buf[caret] = save;
                        fg(c, .85, .95, 1.0); cairo_set_line_width(c, 1.5);
                        cairo_move_to(c, cx, ry + 7); cairo_line_to(c, cx, ry + 24); cairo_stroke(c);
                    }
                    font(c, 12, false, true); fg(c, invalid ? 1 : .52, invalid ? .55 : .66, invalid ? .48 : .82);
                    char range[64];
                    if (d->kind == K_FLOAT_INPUT)
                        snprintf(range, sizeof(range), tr("%g-%g  (type exact value)"), d->min, d->max);
                    else
                        snprintf(range, sizeof(range), tr("%.0f-%.0f  (type exact value)"), d->min, d->max);
                    txt(c, vx + 194, ry + 19, range);
                    add_box(g, H_NUM_FIELD, file, i, 0, vx, ry, 180, 30);
                } else {
                    double frac = (v - d->min) / (d->max - d->min);
                    draw_slider(g, c, H_SLIDER, file, i, 0, vx, ry + 14, vw, frac);
                    snprintf(val, sizeof(val), d->fmt, v);
                    font(c, 13.5, false, true); fg(c, .80, .92, 1.0);
                    txt(c, vx + vw + 22, ry + 19, val);
                }
                break;
            }
            case K_ENUM: {
                int idx = def_get_enum(g, file, d);
                font(c, 16, true, false);
                fg(c, .55, .70, .90); txt(c, vx, ry + 20, "<");
                fg(c, .55, .70, .90); txt(c, vx + 190, ry + 20, ">");
                font(c, 14, false, false); fg(c, .85, .93, 1.0);
                txt_fit(c, vx + 28, ry + 19, 155, tr(d->enums[idx]));
                add_box(g, H_ENUM_DEC, file, i, 0, vx - 6, ry, 30, 28);
                add_box(g, H_ENUM_INC, file, i, 0, vx + 182, ry, 30, 28);
                break;
            }
            case K_COLOR: {
                float rgb[3] = { 0, 0, 0 };
                if (!cd_config_get_color(store_of(g, file), d->sec, d->key, rgb)) {
                    CdConfig *tmp = cd_config_load(NULL);
                    cd_config_set_string(tmp, "x", "y", d->def);
                    cd_config_get_color(tmp, "x", "y", rgb);
                    cd_config_free(tmp);
                }
                fg(c, rgb[0], rgb[1], rgb[2]); rr(c, vx, ry + 3, 52, 22, 5); cairo_fill(c);
                fg(c, .6, .7, .85); cairo_set_line_width(c, 1); rr(c, vx, ry + 3, 52, 22, 5); cairo_stroke(c);
                char hex[8];
                snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                         (unsigned)(rgb[0] * 255 + .5), (unsigned)(rgb[1] * 255 + .5),
                         (unsigned)(rgb[2] * 255 + .5));
                font(c, 13, false, true); fg(c, .8, .88, .98); txt(c, vx + 64, ry + 19, hex);
                font(c, 11, false, false); fg(c, .5, .62, .8);
                bool open = g->exp_color_file == file && g->exp_color_idx == i;
                txt(c, vx + 138, ry + 18, open ? tr("(tap to close)") : tr("(tap to edit)"));
                add_box(g, H_COLOR, file, i, 0, vx, ry, 240, 28);
                if (open) {
                    static const char *ch_name[3] = { "R", "G", "B" };
                    for (int ch = 0; ch < 3; ch++) {
                        double cy = ry + 40 + ch * 26;
                        font(c, 12, true, true);
                        fg(c, ch == 0 ? 1 : .45, ch == 1 ? .95 : .45, ch == 2 ? 1 : .5);
                        txt(c, vx + 8, cy + 4, ch_name[ch]);
                        draw_slider(g, c, H_COLSLIDE, file, i, ch, vx + 34, cy, vw - 60, rgb[ch]);
                        snprintf(val, sizeof(val), "%d", (int)(rgb[ch] * 255 + .5));
                        font(c, 12, false, true); fg(c, .75, .85, .98);
                        txt(c, vx + vw - 14, cy + 4, val);
                    }
                }
                break;
            }
            }
        }
        y += row_h;
    }
    g->content_h = (y + g->scroll[g->tab]) - top;
    cairo_restore(c);
    /* scrollbar */
    double view = bottom - top;
    if (g->content_h > view) {
        double sh = view * view / g->content_h;
        double sy = top + (g->scroll[g->tab] / g->content_h) * view;
        fga(c, .35, .48, .65, .8); rr(c, 984, sy, 8, sh, 4); cairo_fill(c);
    }
}

static void draw_field(Gui *g, cairo_t *c, int fld, double x, double y, double w,
                       const char *label, const char *hint) {
    font(c, 11, true, false); fg(c, .55, .68, .88);
    txt(c, x, y, label);
    bool foc = g->focus == fld;
    fga(c, .07, .10, .17, 1); rr(c, x, y + 6, w, 34, 6); cairo_fill(c);
    cairo_set_line_width(c, foc ? 2 : 1.2);
    if (foc) fg(c, .30, .85, .95); else fg(c, .28, .38, .55);
    rr(c, x, y + 6, w, 34, 6); cairo_stroke(c);
    font(c, 13.5, false, fld >= FLD_EXEC);
    fg(c, .88, .93, 1.0);
    TextField *f = &g->fields[fld];
    /* Horizontal scroll so the caret is always visible. */
    double inner = w - 20;
    char save = 0;
    int caret = f->caret > (int)strlen(f->buf) ? (int)strlen(f->buf) : f->caret;
    save = f->buf[caret]; f->buf[caret] = 0;
    double caret_x = txt_w(c, f->buf);
    f->buf[caret] = save;
    double ox = 0;
    if (caret_x - ox > inner) ox = caret_x - inner;
    double total = txt_w(c, f->buf);
    if (ox > 0 && total - ox < inner) ox = total - inner > 0 ? total - inner : 0;
    cairo_save(c);
    cairo_rectangle(c, x + 8, y + 6, w - 16, 34);
    cairo_clip(c);
    txt(c, x + 10 - ox, y + 28, f->buf);
    if (foc && (int)(now_s() * 1.9) % 2 == 0) {
        fg(c, .85, .95, 1.0); cairo_set_line_width(c, 1.6);
        cairo_move_to(c, x + 10 - ox + caret_x + 1, y + 13);
        cairo_line_to(c, x + 10 - ox + caret_x + 1, y + 33);
        cairo_stroke(c);
    }
    cairo_restore(c);
    if (hint) {
        font(c, 10.5, false, false); fg(c, .45, .55, .72);
        txt_fit(c, x + 2, y + 54, w, hint);
    }
    add_box(g, H_FIELD, fld, 0, 0, x, y + 6, w, 34);
}

static void draw_apps_tab(Gui *g, cairo_t *c, double top, double bottom) {
    CdConfig *st = store_of(g, FILE_CD);
    /* --- left: entry list (KDE menu-editor style) */
    double lx = 20, lw = 276;
    fga(c, 1, 1, 1, .025); rr(c, lx - 6, top + 2, lw + 12, bottom - top - 66, 8); cairo_fill(c);
    for (int i = 0; i < 6; i++) {
        double ry = top + 12 + i * 58;
        bool sel = i == g->sel_app;
        if (sel) { fga(c, .16, .30, .45, .9); rr(c, lx, ry, lw, 50, 7); cairo_fill(c); }
        bool en = cd_config_get_bool(st, APP_DEFS[i].sec, "enabled", true);
        draw_glyph(c, app_icon(g, i), lx + 12, ry + 12, 0.85);
        font(c, 14.5, true, false);
        if (en) fg(c, .88, .93, 1); else fg(c, .45, .52, .65);
        const char *nm = (g->field_app == i) ? g->fields[FLD_NAME].buf
                                             : app_str(g, i, "name", APP_DEFS[i].def_name);
        txt_fit(c, lx + 52, ry + 22, lw - 105, tr(nm));
        font(c, 10.5, false, false); fg(c, .50, .62, .80);
        char detail[192];
        int act = app_action(g, i);
        if (act == ACT_NEON) snprintf(detail, sizeof(detail), "%s", tr("Built-in / underglow"));
        else if (act == ACT_COCKPIT) snprintf(detail, sizeof(detail), "%s", tr("Built-in / driver view"));
        else {
            const char *id = app_str(g, i, "app_id", APP_DEFS[i].def_app_id);
            snprintf(detail, sizeof(detail), tr("App / %s"), *id ? id : APP_DEFS[i].sec);
        }
        txt_fit(c, lx + 52, ry + 39, lw - 62, detail);
        if (!en) { font(c, 9.5, true, false); fg(c, .75, .55, .35); txt_r(c, lx + lw - 8, ry + 20, tr("OFF")); }
        add_box(g, H_APP_ROW, i, 0, 0, lx, ry, lw, 50);
    }
    /* reset entry button (under the list) */
    double by = bottom - 52;
    fga(c, .16, .22, .34, 1); rr(c, lx, by, lw, 32, 7); cairo_fill(c);
    font(c, 12.5, false, false); fg(c, .80, .88, .98);
    txt(c, lx + 14, by + 21, tr("Reset selected entry to defaults"));
    add_box(g, H_RESET_ENTRY, 0, 0, 0, lx, by, lw, 32);

    /* --- right: editor form */
    double fx = 330, fw = 612;
    const AppDef *d = &APP_DEFS[g->sel_app];
    int act = app_action(g, g->sel_app);
    double y = top + 16;
    /* The essential mapping, stated first: which app this icon launches.
     * Clicking opens the same popup as right-clicking the slot. */
    fga(c, .11, .24, .38, 1); rr(c, fx, y, fw, 46, 8); cairo_fill(c);
    cairo_set_line_width(c, 1.2); fg(c, .35, .62, .85); rr(c, fx, y, fw, 46, 8); cairo_stroke(c);
    draw_glyph(c, app_icon(g, g->sel_app), fx + 12, y + 10, 0.85);
    font(c, 10.5, true, false); fg(c, .58, .74, .94);
    txt(c, fx + 54, y + 17, tr("ASSIGNED TO THIS ICON"));
    font(c, 15, true, false); fg(c, .90, .95, 1);
    /* Read-only summaries (this line and the slot list above) translate the
     * label, because built-in defaults are our own English strings. The
     * editable text fields deliberately do NOT: they show the bytes actually
     * stored in the config file, so typing in them can never write a
     * translated label back into the user's settings. A name the user invented
     * is not in the catalog and therefore passes through unchanged. */
    const char *assigned = act == ACT_NEON ? tr("Built-in - Cycle underglow") :
                           act == ACT_COCKPIT ? tr("Built-in - Driver view")
                                              : tr(g->fields[FLD_NAME].buf);
    txt_fit(c, fx + 54, y + 38, fw - 220, assigned);
    font(c, 13.5, true, false); fg(c, .55, .90, 1);
    txt_r(c, fx + fw - 16, y + 29, tr("Choose app  >"));
    add_box(g, H_CHOOSE_APP, g->sel_app, 0, 0, fx, y, fw, 46);
    y += 58;
    draw_field(g, c, FLD_NAME, fx, y, fw, tr("NAME (button label)"), NULL); y += 62;
    draw_field(g, c, FLD_SUB, fx, y, fw, tr("DESCRIPTION (small second line)"), NULL); y += 62;
    if (act == ACT_APP) {
        const char *envv = d->env_name ? getenv(d->env_name) : NULL;
        char hint[256];
        if (envv)
            snprintf(hint, sizeof(hint), tr("WARNING: $%s is set in this environment - compositor uses it instead."), d->env_name);
        else
            snprintf(hint, sizeof(hint), "%s", tr("Shell command. %root% expands to the ChromeDrive install directory."));
        draw_field(g, c, FLD_EXEC, fx, y, fw, tr("COMMAND (exec)"), hint); y += 76;
        draw_field(g, c, FLD_APPID, fx, y, fw, tr("APP ID MATCH (focus instead of relaunch)"),
                   tr("Substring of the window's app_id, e.g. org.kde.dolphin. Empty means always launch new."));
        y += 76;
    } else {
        font(c, 12.5, false, false); fg(c, .60, .72, .92);
        txt_fit(c, fx, y + 12, fw, tr(action_desc(act)));
        y += 40;
    }
    /* icon cycler */
    font(c, 11, true, false); fg(c, .55, .68, .88); txt(c, fx, y, tr("ICON"));
    int icon = app_icon(g, g->sel_app);
    font(c, 17, true, false); fg(c, .55, .70, .90);
    txt(c, fx, y + 30, "<"); txt(c, fx + 170, y + 30, ">");
    draw_glyph(c, icon, fx + 34, y + 10, 0.9);
    font(c, 13.5, false, false); fg(c, .85, .92, 1);
    txt(c, fx + 78, y + 29, tr(ICON_NAMES[icon]));
    add_box(g, H_ICON_DEC, 0, 0, 0, fx - 6, y + 6, 32, 32);
    add_box(g, H_ICON_INC, 0, 0, 0, fx + 162, y + 6, 32, 32);
    y += 58;
    /* toggles */
    bool en = cd_config_get_bool(st, d->sec, "enabled", true);
    draw_checkbox(c, fx, y, en);
    font(c, 13.5, false, false); fg(c, .82, .89, .97);
    txt(c, fx + 30, y + 15, tr("Enabled (button responds to clicks)"));
    add_box(g, H_APP_ENABLED, 0, 0, 0, fx - 4, y - 4, 380, 28);
    if (act == ACT_APP) {
        bool as = cd_config_get_bool(st, d->sec, "autostart", d->def_autostart);
        draw_checkbox(c, fx + 400, y, as);
        txt(c, fx + 430, y + 15, tr("Autostart (--demo set)"));
        add_box(g, H_APP_AUTOSTART, 0, 0, 0, fx + 396, y - 4, 280, 28);
    }
    g->content_h = bottom - top;
}

/* The right-click app picker: an anchored, modal, searchable list of every
 * assignable app, drawn last so its hitboxes win over the editor beneath. */
static void draw_picker(Gui *g, cairo_t *c) {
    if (!g->picker_open) return;
    picker_clamp(g);
    double x, y, w, h;
    picker_geometry(g, &x, &y, &w, &h);
    fga(c, .005, .008, .02, .58); cairo_rectangle(c, 0, 0, 1000, 625); cairo_fill(c);
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(grad, 0, .055, .085, .15, .99);
    cairo_pattern_add_color_stop_rgba(grad, 1, .025, .045, .09, .99);
    rr(c, x, y, w, h, 12); cairo_set_source(c, grad); cairo_fill(c); cairo_pattern_destroy(grad);
    cairo_set_line_width(c, 1.6); fg(c, .35, .65, .90); rr(c, x, y, w, h, 12); cairo_stroke(c);

    font(c, 10.5, true, true); fg(c, .55, .72, .95);
    char head[96];
    snprintf(head, sizeof(head), tr("LAUNCH CONTROL / SLOT %d OF 6"), g->picker_slot + 1);
    txt(c, x + 20, y + 24, head);
    font(c, 19, true, false); fg(c, .92, .96, 1);
    txt(c, x + 20, y + 50, tr("Choose an app for this icon"));
    font(c, 18, true, false); fg(c, .65, .76, .92); txt_r(c, x + w - 18, y + 34, "x");
    add_box(g, H_PICKER_CLOSE, 0, 0, 0, x + w - 44, y + 8, 36, 36);

    /* Type-to-filter field */
    double sy = y + 66;
    fga(c, .045, .07, .13, 1); rr(c, x + 18, sy, w - 36, 34, 7); cairo_fill(c);
    cairo_set_line_width(c, 1.3); fg(c, .30, .55, .82); rr(c, x + 18, sy, w - 36, 34, 7); cairo_stroke(c);
    font(c, 13, false, false);
    if (*g->picker_filter) { fg(c, .90, .95, 1); txt(c, x + 32, sy + 22, g->picker_filter); }
    else { fg(c, .42, .54, .74); txt(c, x + 32, sy + 22, tr("Type to filter all installed apps...")); }

    int count = picker_count(g);
    double ly = y + 114, row_h = 31;
    if (!count) {
        font(c, 14, false, false); fg(c, .55, .65, .82);
        txt(c, x + 24, ly + 28, tr("No installed app matches that filter."));
    }
    for (int row = 0; row < PICKER_VISIBLE; row++) {
        int ordinal = g->picker_scroll + row;
        if (ordinal >= count) break;
        int idx = -1;
        AppChoice *ch = picker_at(g, ordinal, &idx);
        if (!ch) break;
        double ry = ly + row * row_h;
        bool hover = g->px >= x + 10 && g->px <= x + w - 38 && g->py >= ry && g->py < ry + row_h;
        if (ordinal == g->picker_sel || hover) {
            fga(c, .16, .32, .50, hover ? .98 : .88); rr(c, x + 10, ry + 1, w - 48, row_h - 2, 6); cairo_fill(c);
        }
        draw_glyph(c, ch->icon, x + 20, ry + 5, 0.72);
        font(c, 13.5, true, false); fg(c, .90, .94, 1);
        txt_fit(c, x + 58, ry + 21, w - 255, tr(ch->name));
        font(c, 10.5, false, true); fg(c, .50, .64, .84);
        const char *side = ch->kind == CHOICE_APP ? ch->app_id : tr("built-in");
        txt_r(c, x + w - 48, ry + 20, side);
        if (choice_is_current(g, g->picker_slot, ch)) {
            font(c, 13, true, false); fg(c, .45, .95, .70);
            txt_r(c, x + w - 44, ry + 21, "");
            /* a small check mark before the app-id column */
            cairo_set_line_width(c, 2);
            cairo_move_to(c, x + w - 250, ry + 16);
            cairo_line_to(c, x + w - 244, ry + 22);
            cairo_line_to(c, x + w - 236, ry + 9);
            cairo_stroke(c);
        }
        add_box(g, H_PICKER_ITEM, idx, 0, 0, x + 10, ry, w - 48, row_h);
    }
    /* scrollbar + page controls */
    double tx = x + w - 28, th = PICKER_VISIBLE * row_h;
    fga(c, .10, .16, .26, 1); rr(c, tx, ly, 10, th, 5); cairo_fill(c);
    if (count > PICKER_VISIBLE) {
        double sh = th * PICKER_VISIBLE / count;
        double syy = ly + th * g->picker_scroll / count;
        fga(c, .38, .58, .82, 1); rr(c, tx, syy, 10, sh, 5); cairo_fill(c);
    } else { fga(c, .30, .42, .62, .55); rr(c, tx, ly, 10, th, 5); cairo_fill(c); }
    add_box(g, H_PICKER_UP, 0, 0, 0, tx - 4, ly - 24, 20, 22);
    add_box(g, H_PICKER_DOWN, 0, 0, 0, tx - 4, ly + th + 2, 20, 22);
    font(c, 13, true, false); fg(c, .60, .74, .94);
    txt(c, tx - 1, ly - 8, "^"); txt(c, tx - 2, ly + th + 17, "v");

    char foot[160];
    snprintf(foot, sizeof(foot), "%d apps  -  click or Enter assigns  -  wheel/arrows scroll  -  Esc cancels", count);
    font(c, 10.5, false, true); fg(c, .48, .62, .82);
    txt(c, x + 20, y + h - 16, foot);
}

static void draw_ui(Gui *g, cairo_t *c) {
    /* Picking a language in the schema tab re-labels this very frame. */
    gui_sync_language(g);
    g->box_count = 0;
    /* background */
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, 625);
    cairo_pattern_add_color_stop_rgb(grad, 0, .045, .06, .11);
    cairo_pattern_add_color_stop_rgb(grad, 1, .02, .03, .07);
    cairo_set_source(c, grad); cairo_paint(c); cairo_pattern_destroy(grad);
    fga(c, .35, .15, .55, .10); cairo_rectangle(c, 0, 0, 1000, 54); cairo_fill(c);

    /* header: title + tabs */
    font(c, 19, true, false); fg(c, .92, .95, 1);
    txt(c, 22, 35, tr("ChromeDrive Settings"));
    static const char *TAB_NAMES[3] = { "Launch Menu", "Garage3D", "Compositor" };
    double tx = 352;
    for (int i = 0; i < 3; i++) {
        font(c, 14.5, i == g->tab, false);
        double tw = txt_w(c, tr(TAB_NAMES[i])) + 36;
        if (i == g->tab) { fga(c, .18, .35, .55, .95); rr(c, tx, 12, tw, 34, 8); cairo_fill(c); }
        fg(c, i == g->tab ? .95 : .62, i == g->tab ? .98 : .72, 1);
        txt(c, tx + 18, 35, tr(TAB_NAMES[i]));
        add_box(g, H_TAB, i, 0, 0, tx, 10, tw, 38);
        tx += tw + 10;
    }
    if (g->tab != TAB_APPS) {
        fga(c, .16, .22, .34, 1); rr(c, 806, 12, 172, 34, 8); cairo_fill(c);
        font(c, 12, false, false); fg(c, .80, .88, .98);
        txt(c, 822, 34, tr("Reset tab to defaults"));
        add_box(g, H_RESET_TAB, 0, 0, 0, 806, 12, 172, 34);
    }

    double top = 64, bottom = 576;
    if (g->tab == TAB_APPS) draw_apps_tab(g, c, top, bottom);
    else if (g->tab == TAB_G3) draw_schema_tab(g, c, FILE_G3, G3_DEFS, N_G3_DEFS, top, bottom);
    else draw_schema_tab(g, c, FILE_CD, CD_DEFS, N_CD_DEFS, top, bottom);

    /* footer */
    fga(c, 1, 1, 1, .04); cairo_rectangle(c, 0, 582, 1000, 43); cairo_fill(c);
    font(c, 10.5, false, true); fg(c, .48, .60, .78);
    txt_fit(c, 22, 599, 560, g->tab == TAB_G3 ? g->path[FILE_G3] : g->path[FILE_CD]);
    font(c, 11, false, false);
    if (g->save_pending[0] || g->save_pending[1]) { fg(c, .95, .80, .45); txt_r(c, 978, 599, tr("Saving...")); }
    else { fg(c, .45, .85, .65); txt_r(c, 978, 599, g->saved_note); }
    font(c, 10.5, false, false); fg(c, .40, .50, .68);
    txt(c, 22, 617, g->tab == TAB_APPS
        ? tr("Right-click a slot (or Choose app) to assign an installed app; changes relabel Launch Control live.")
        : tr("Drag sliders / tap toggles, or click the VNC port and type - files save automatically."));
    draw_picker(g, c);
}

/* --------------------------------------------------------- input handling */

static void slider_apply(Gui *g, int file, int idx, double x) {
    HitBox *b = find_box(g, H_SLIDER, file, idx);
    if (!b) return;
    const Def *d = file == FILE_G3 ? &G3_DEFS[idx] : &CD_DEFS[idx];
    double frac = clampd((x - (b->x + 6)) / (b->w - 12), 0, 1);
    def_set_num(g, file, d, d->min + frac * (d->max - d->min));
}
static void colslide_apply(Gui *g, int file, int idx, int ch, double x) {
    HitBox *b = NULL;
    for (int i = 0; i < g->box_count; i++)
        if (g->boxes[i].kind == H_COLSLIDE && g->boxes[i].a == file &&
            g->boxes[i].b == idx && g->boxes[i].c == ch) b = &g->boxes[i];
    if (!b) return;
    const Def *d = file == FILE_G3 ? &G3_DEFS[idx] : &CD_DEFS[idx];
    double frac = clampd((x - (b->x + 6)) / (b->w - 12), 0, 1);
    float rgb[3] = { 0, 0, 0 };
    if (!cd_config_get_color(store_of(g, file), d->sec, d->key, rgb)) {
        CdConfig *tmp = cd_config_load(NULL);
        cd_config_set_string(tmp, "x", "y", d->def);
        cd_config_get_color(tmp, "x", "y", rgb);
        cd_config_free(tmp);
    }
    rgb[ch] = (float)frac;
    cd_config_set_color(store_of(g, file), d->sec, d->key, rgb);
    touch_file(g, file);
}

static int field_caret_at(Gui *g, int fld, double x) {
    HitBox *b = find_box(g, H_FIELD, fld, 0);
    if (!b || !g->measure) return (int)strlen(g->fields[fld].buf);
    cairo_t *c = g->measure;
    font(c, 13.5, false, fld >= FLD_EXEC);
    TextField *f = &g->fields[fld];
    double target = x - (b->x + 10);
    /* approximate with the same origin as draw (ox=0 case covers most) */
    int len = (int)strlen(f->buf), best = len;
    char save = f->buf[len];
    for (int i = 0; i <= len; i++) {
        char t = f->buf[i]; f->buf[i] = 0;
        double wdt = txt_w(c, f->buf);
        f->buf[i] = t;
        if (wdt >= target) { best = i; break; }
    }
    (void)save;
    return best;
}
static HitBox *hit_at(Gui *g, double x, double y) {
    for (int i = g->box_count - 1; i >= 0; i--) {
        HitBox *b = &g->boxes[i];
        if (x >= b->x && x <= b->x + b->w && y >= b->y && y <= b->y + b->h) return b;
    }
    return NULL;
}
static int num_caret_at(Gui *g, double x) {
    HitBox *b = find_box(g, H_NUM_FIELD, g->num_file, g->num_idx);
    if (!b || !g->measure) return (int)strlen(g->num_field.buf);
    cairo_t *c = g->measure;
    font(c, 14, false, true);
    TextField *f = &g->num_field;
    double target = x - (b->x + 11);
    int len = (int)strlen(f->buf);
    for (int i = 0; i <= len; i++) {
        char t = f->buf[i]; f->buf[i] = 0;
        double wdt = txt_w(c, f->buf);
        f->buf[i] = t;
        if (wdt >= target) return i;
    }
    return len;
}

static void gui_press(Gui *g, double x, double y) {
    g->px = x; g->py = y;
    HitBox *hit = hit_at(g, x, y);
    if (g->picker_open) {
        if (hit && hit->kind == H_PICKER_ITEM) { picker_assign(g, hit->a); return; }
        if (hit && hit->kind == H_PICKER_CLOSE) { picker_close(g); return; }
        if (hit && hit->kind == H_PICKER_UP) {
            g->picker_sel = g->picker_scroll = (g->picker_scroll > PICKER_VISIBLE ? g->picker_scroll - PICKER_VISIBLE : 0);
            picker_clamp(g); g->draw_dirty = true; return;
        }
        if (hit && hit->kind == H_PICKER_DOWN) {
            g->picker_scroll += PICKER_VISIBLE;
            g->picker_sel = g->picker_scroll;
            picker_clamp(g); g->draw_dirty = true; return;
        }
        double px, py, pw, ph;
        picker_geometry(g, &px, &py, &pw, &ph);
        if (x >= px && x <= px + pw && y >= py && y <= py + ph) return; /* header/filter/empty area */
        picker_close(g);
        return; /* an outside click dismisses; it does not activate beneath */
    }
    if (g->num_idx >= 0 && (!hit || hit->kind != H_NUM_FIELD ||
                             hit->a != g->num_file || hit->b != g->num_idx)) {
        if (!num_commit(g)) num_cancel(g); /* invalid text reverts on blur */
    }
    if (g->focus >= 0 && (!hit || hit->kind != H_FIELD || hit->a != g->focus)) {
        app_commit_fields(g);
        g->focus = -1;
        g->draw_dirty = true;
    }
    if (!hit) return;
    const Def *defs;
    switch (hit->kind) {
    case H_TAB:
        app_commit_fields(g); g->focus = -1; g->tab = hit->a; g->draw_dirty = true; break;
    case H_SLIDER:
        g->drag_kind = H_SLIDER; g->drag_a = hit->a; g->drag_b = hit->b; g->drag_c = 0;
        slider_apply(g, hit->a, hit->b, x); break;
    case H_COLSLIDE:
        g->drag_kind = H_COLSLIDE; g->drag_a = hit->a; g->drag_b = hit->b; g->drag_c = hit->c;
        colslide_apply(g, hit->a, hit->b, hit->c, x); break;
    case H_TOGGLE: {
        defs = hit->a == FILE_G3 ? G3_DEFS : CD_DEFS;
        const Def *d = &defs[hit->b];
        cd_config_set_bool(store_of(g, hit->a), d->sec, d->key, !def_get_bool(g, hit->a, d));
        touch_file(g, hit->a); flush_saves(g, true);
        break;
    }
    case H_COLOR:
        if (g->exp_color_file == hit->a && g->exp_color_idx == hit->b) {
            g->exp_color_file = g->exp_color_idx = -1;
        } else { g->exp_color_file = hit->a; g->exp_color_idx = hit->b; }
        g->draw_dirty = true;
        break;
    case H_ENUM_DEC: case H_ENUM_INC: {
        defs = hit->a == FILE_G3 ? G3_DEFS : CD_DEFS;
        const Def *d = &defs[hit->b];
        int idx = def_get_enum(g, hit->a, d) + (hit->kind == H_ENUM_INC ? 1 : -1);
        idx = (idx % d->enum_count + d->enum_count) % d->enum_count;
        def_set_enum(g, hit->a, d, idx); flush_saves(g, true);
        break;
    }
    case H_APP_ROW: app_select(g, hit->a); break;
    case H_FIELD:
        g->focus = hit->a;
        g->fields[hit->a].caret = field_caret_at(g, hit->a, x);
        g->draw_dirty = true;
        break;
    case H_NUM_FIELD:
        if (!(g->num_file == hit->a && g->num_idx == hit->b)) num_begin(g, hit->a, hit->b);
        g->num_field.caret = num_caret_at(g, x);
        g->draw_dirty = true;
        break;
    case H_CHOOSE_APP:
        picker_open(g, hit->a, x, y);
        break;
    case H_ICON_DEC: case H_ICON_INC: {
        int icon = app_icon(g, g->sel_app) + (hit->kind == H_ICON_INC ? 1 : -1);
        icon = (icon % 6 + 6) % 6;
        cd_config_set_int(store_of(g, FILE_CD), APP_DEFS[g->sel_app].sec, "icon", icon);
        touch_file(g, FILE_CD); flush_saves(g, true);
        break;
    }
    case H_APP_ENABLED: {
        CdConfig *st = store_of(g, FILE_CD);
        bool en = cd_config_get_bool(st, APP_DEFS[g->sel_app].sec, "enabled", true);
        cd_config_set_bool(st, APP_DEFS[g->sel_app].sec, "enabled", !en);
        touch_file(g, FILE_CD); flush_saves(g, true);
        break;
    }
    case H_APP_AUTOSTART: {
        CdConfig *st = store_of(g, FILE_CD);
        bool as = cd_config_get_bool(st, APP_DEFS[g->sel_app].sec, "autostart", APP_DEFS[g->sel_app].def_autostart);
        cd_config_set_bool(st, APP_DEFS[g->sel_app].sec, "autostart", !as);
        touch_file(g, FILE_CD); flush_saves(g, true);
        break;
    }
    case H_RESET_TAB: {
        int file = g->tab == TAB_CD ? FILE_CD : FILE_G3;
        const Def *dd = file == FILE_CD ? CD_DEFS : G3_DEFS;
        int nd = file == FILE_CD ? N_CD_DEFS : N_G3_DEFS;
        for (int i = 0; i < nd; i++)
            cd_config_set_string(store_of(g, file), dd[i].sec, dd[i].key, dd[i].def);
        touch_file(g, file); flush_saves(g, true);
        break;
    }
    case H_RESET_ENTRY: {
        int app = g->sel_app;
        const AppDef *d = &APP_DEFS[app];
        CdConfig *st = store_of(g, FILE_CD);
        cd_config_set_string(st, d->sec, "name", d->def_name);
        cd_config_set_string(st, d->sec, "sub", d->def_sub);
        cd_config_set_int(st, d->sec, "icon", d->def_icon);
        cd_config_set_bool(st, d->sec, "enabled", true);
        cd_config_set_string(st, d->sec, "action", action_key(d->def_action));
        cd_config_set_string(st, d->sec, "exec", d->def_exec ? d->def_exec : "");
        cd_config_set_string(st, d->sec, "app_id", d->def_app_id ? d->def_app_id : "");
        cd_config_set_bool(st, d->sec, "autostart", d->def_autostart);
        g->focus = -1;
        app_load_fields(g, app);
        touch_file(g, FILE_CD); flush_saves(g, true);
        break;
    }
    }
}
static void gui_motion(Gui *g, double x, double y) {
    g->px = x; g->py = y;
    if (g->drag_kind == H_SLIDER) slider_apply(g, g->drag_a, g->drag_b, x);
    else if (g->drag_kind == H_COLSLIDE) colslide_apply(g, g->drag_a, g->drag_b, g->drag_c, x);
    else if (g->picker_open) g->draw_dirty = true; /* row hover highlight */
}
/* Right-click is reserved for the app-assignment context popup: on a Launch
 * Control row it assigns that row; anywhere in the editor form it assigns
 * the selected entry. A second right-click (or Esc/click-away) dismisses. */
static void gui_right_press(Gui *g, double x, double y) {
    g->px = x; g->py = y;
    HitBox *hit = hit_at(g, x, y);
    if (g->picker_open) {
        if (hit && hit->kind == H_APP_ROW) picker_open(g, hit->a, x, y);
        else picker_close(g);
        return;
    }
    if (g->tab != TAB_APPS) return;
    if (hit && hit->kind == H_APP_ROW) picker_open(g, hit->a, x, y);
    else if (x >= 316) picker_open(g, g->sel_app, x, y);
}
static void gui_release(Gui *g) {
    if (g->drag_kind) { g->drag_kind = H_NONE; flush_saves(g, true); }
}
static void gui_wheel(Gui *g, double dy) {
    if (g->picker_open) {
        int step = dy > 0 ? 3 : dy < 0 ? -3 : 0;
        g->picker_scroll += step;
        g->picker_sel = g->picker_scroll;
        picker_clamp(g);
        g->draw_dirty = true;
        return;
    }
    if (g->tab == TAB_APPS) return;
    double maxs = g->content_h - (576 - 64);
    if (maxs < 0) maxs = 0;
    g->scroll[g->tab] = clampd(g->scroll[g->tab] + dy * 40, 0, maxs);
    g->draw_dirty = true;
}

/* UTF-8 caret helpers: caret is a byte offset; step over codepoints. */
static int utf8_prev(const char *s, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && (s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}
static int utf8_next(const char *s, int pos) {
    int len = (int)strlen(s);
    if (pos >= len) return len;
    pos++;
    while (pos < len && (s[pos] & 0xC0) == 0x80) pos++;
    return pos;
}
static void gui_key(Gui *g, xkb_keysym_t sym, const char *utf8, int utf8_len) {
    if (g->picker_open) {
        int count = picker_count(g);
        switch (sym) {
        case XKB_KEY_Escape: picker_close(g); return;
        case XKB_KEY_Return: case XKB_KEY_KP_Enter: {
            int idx = -1;
            if (picker_at(g, g->picker_sel, &idx)) picker_assign(g, idx);
            return;
        }
        case XKB_KEY_Down: g->picker_sel++; break;
        case XKB_KEY_Up: g->picker_sel--; break;
        case XKB_KEY_Page_Down: g->picker_sel += PICKER_VISIBLE; break;
        case XKB_KEY_Page_Up: g->picker_sel -= PICKER_VISIBLE; break;
        case XKB_KEY_Home: g->picker_sel = 0; break;
        case XKB_KEY_End: g->picker_sel = count - 1; break;
        case XKB_KEY_Tab: g->picker_sel++; break;
        case XKB_KEY_BackSpace: {
            int len = (int)strlen(g->picker_filter);
            if (len) g->picker_filter[utf8_prev(g->picker_filter, len)] = 0;
            g->picker_sel = g->picker_scroll = 0;
            break;
        }
        case XKB_KEY_Delete:
            g->picker_filter[0] = 0;
            g->picker_sel = g->picker_scroll = 0;
            break;
        default:
            if (utf8 && utf8_len > 0 && (unsigned char)utf8[0] >= 0x20 &&
                (int)strlen(g->picker_filter) + utf8_len < (int)sizeof(g->picker_filter)) {
                strncat(g->picker_filter, utf8, (size_t)utf8_len);
                g->picker_sel = g->picker_scroll = 0;
            }
            break;
        }
        picker_clamp(g);
        g->draw_dirty = true;
        return;
    }
    if (g->num_idx >= 0) {
        TextField *f = &g->num_field;
        int len = (int)strlen(f->buf);
        if (f->caret > len) f->caret = len;
        bool edited = false;
        switch (sym) {
        case XKB_KEY_Escape: num_cancel(g); return;
        case XKB_KEY_Return: case XKB_KEY_KP_Enter: num_commit(g); return;
        case XKB_KEY_Tab: num_commit(g); return;
        case XKB_KEY_BackSpace:
            if (f->caret > 0) {
                memmove(f->buf + f->caret - 1, f->buf + f->caret, (size_t)(len - f->caret + 1));
                f->caret--; edited = true;
            }
            break;
        case XKB_KEY_Delete:
            if (f->caret < len) {
                memmove(f->buf + f->caret, f->buf + f->caret + 1, (size_t)(len - f->caret));
                edited = true;
            }
            break;
        case XKB_KEY_Left: if (f->caret > 0) f->caret--; break;
        case XKB_KEY_Right: if (f->caret < len) f->caret++; break;
        case XKB_KEY_Home: f->caret = 0; break;
        case XKB_KEY_End: f->caret = len; break;
        default: {
            const Def *d = num_def(g);
            bool is_float = d && d->kind == K_FLOAT_INPUT;
            if (utf8 && utf8_len == 1 && len < (is_float ? 24 : 12)) {
                char ch = utf8[0];
                bool digit = isdigit((unsigned char)ch) != 0;
                bool minus = ch == '-' && d && d->min < 0 && f->caret == 0 && f->buf[0] != '-';
                bool dot = is_float && ch == '.' && !strchr(f->buf, '.') &&
                           !strchr(f->buf, 'e') && !strchr(f->buf, 'E');
                bool exp = is_float && (ch == 'e' || ch == 'E') &&
                           !strchr(f->buf, 'e') && !strchr(f->buf, 'E') && f->caret > 0 &&
                           (isdigit((unsigned char)f->buf[f->caret - 1]) || f->buf[f->caret - 1] == '.');
                bool expsign = is_float && (ch == '+' || ch == '-') && f->caret > 0 &&
                               (f->buf[f->caret - 1] == 'e' || f->buf[f->caret - 1] == 'E');
                if (digit || minus || dot || exp || expsign) {
                    memmove(f->buf + f->caret + 1, f->buf + f->caret, (size_t)(len - f->caret + 1));
                    f->buf[f->caret++] = ch;
                    edited = true;
                }
            }
            break;
        }
        }
        if (edited) g->num_dirty = true;
        g->draw_dirty = true;
        return;
    }
    if (sym == XKB_KEY_Escape && g->focus >= 0) {
        /* revert field contents from the store */
        app_load_fields(g, g->field_app);
        g->focus = -1; g->draw_dirty = true;
        return;
    }
    if (g->focus < 0) {
        if (sym == XKB_KEY_Tab) { g->tab = (g->tab + 1) % 3; g->draw_dirty = true; }
        return;
    }
    TextField *f = &g->fields[g->focus];
    int len = (int)strlen(f->buf);
    if (f->caret > len) f->caret = len;
    bool edited = true;
    switch (sym) {
    case XKB_KEY_Return: case XKB_KEY_KP_Enter:
        app_commit_fields(g); g->focus = -1; break;
    case XKB_KEY_Tab: {
        app_commit_fields(g);
        int maxf = app_action(g, g->sel_app) != ACT_APP ? FLD_SUB : FLD_APPID;
        g->focus = g->focus >= maxf ? 0 : g->focus + 1;
        if (g->focus >= 0) g->fields[g->focus].caret = (int)strlen(g->fields[g->focus].buf);
        break;
    }
    case XKB_KEY_BackSpace:
        if (f->caret > 0) {
            int p = utf8_prev(f->buf, f->caret);
            memmove(f->buf + p, f->buf + f->caret, len - f->caret + 1);
            f->caret = p;
        }
        break;
    case XKB_KEY_Delete:
        if (f->caret < len) {
            int n = utf8_next(f->buf, f->caret);
            memmove(f->buf + f->caret, f->buf + n, len - n + 1);
        }
        break;
    case XKB_KEY_Left:  f->caret = utf8_prev(f->buf, f->caret); edited = false; break;
    case XKB_KEY_Right: f->caret = utf8_next(f->buf, f->caret); edited = false; break;
    case XKB_KEY_Home:  f->caret = 0; edited = false; break;
    case XKB_KEY_End:   f->caret = len; edited = false; break;
    default:
        if (utf8 && utf8_len > 0 && utf8[0] >= 0x20 &&
            len + utf8_len < (int)sizeof(f->buf)) {
            memmove(f->buf + f->caret + utf8_len, f->buf + f->caret, len - f->caret + 1);
            memcpy(f->buf + f->caret, utf8, utf8_len);
            f->caret += utf8_len;
        } else edited = false;
        break;
    }
    if (edited) g->field_dirty = true;
    g->draw_dirty = true;
}

/* ------------------------------------------------------------- GUI create */

/* The settings client is <root>/bin/chromedrive-config, so the install root
 * is two directories up - the same convention src/main.c uses via
 * /proc/self/exe, repeated here because this binary never links chrome.h. */
static void gui_resolve_root(Gui *g) {
    ssize_t n = readlink("/proc/self/exe", g->root, sizeof(g->root) - 1);
    if (n <= 0) { g->root[0] = 0; return; }
    g->root[n] = 0;
    char *slash = strrchr(g->root, '/');
    if (slash) *slash = 0;              /* strip the binary name */
    slash = strrchr(g->root, '/');
    if (slash) *slash = 0;              /* strip bin/ */
}

/* Backing store for the language row's labels: Def keeps bare pointers, so
 * these have to outlive the table. */
static char language_tags[8][32];

/* Choice 0 is "auto" (follow LC_ALL/LC_MESSAGES/LANG); the rest are the
 * catalogs i18n_languages() found on disk, English first then alphabetical. */
static void gui_load_languages(Gui *g) {
    Def *d = &CD_DEFS[CD_LANGUAGE_ROW];
    int n = 0;
    snprintf(language_tags[n], sizeof language_tags[0], "auto");
    d->enums[n] = language_tags[n];
    n++;
    char list[256];
    i18n_languages(list, sizeof list, g->root);
    for (const char *p = list; *p && n < 8; ) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len && len < sizeof language_tags[0]) {
            memcpy(language_tags[n], p, len);
            language_tags[n][len] = 0;
            d->enums[n] = language_tags[n];
            n++;
        }
        p += len + (comma ? 1 : 0);
    }
    d->enum_count = n;
    d->max = (double)(n - 1);
    d->min = 0;
}

/* Backing store for the post-shader row's labels, same lifetime rule as
 * language_tags: Def keeps bare pointers, so these must outlive the table. */
static char post_shader_paths[8][1024];

/* The built-in presets are fixed; a hand-edited custom path is appended when
 * it is non-empty and not already a preset, so the row shows it instead of
 * silently falling back to "off". Picking another choice overwrites it. */
static void gui_load_post_shaders(Gui *g) {
    static const char *const presets[] = {
        "off",
        "shaders/khronos-pbr-neutral.frag",
        "shaders/ssim-downscale.frag",
        "shaders/hermite-downscale.frag",
        "shaders/hermite-downscale-mipmap.frag",
    };
    Def *d = &CD_DEFS[CD_POST_SHADER_ROW];
    int n = 0;
    for (size_t i = 0; i < sizeof presets / sizeof presets[0]; i++) {
        snprintf(post_shader_paths[n], sizeof post_shader_paths[0], "%s", presets[i]);
        d->enums[n] = post_shader_paths[n];
        n++;
    }
    const char *cur = cd_config_get_string(g->store[FILE_CD], d->sec, d->key, d->def);
    if (cur && *cur && strcmp(cur, "off") != 0) {
        bool known = false;
        for (int i = 0; i < n; i++)
            if (!strcmp(cur, d->enums[i])) { known = true; break; }
        if (!known && n < (int)(sizeof d->enums / sizeof d->enums[0])) {
            snprintf(post_shader_paths[n], sizeof post_shader_paths[0], "%s", cur);
            d->enums[n] = post_shader_paths[n];
            n++;
        }
    }
    d->enum_count = n;
    d->max = (double)(n - 1);
    d->min = 0;
}

/* The GUI edits the config file live and the compositor reloads it, but this
 * process has to relabel itself too. Cheap enough to poll once a frame: it is
 * one string compare. */
static void gui_sync_language(Gui *g) {
    const char *lang_env = getenv("CHROMEDRIVE_LANG");
    const char *want = (lang_env && *lang_env) ? lang_env
                       : cd_config_get_string(g->store[FILE_CD], "ui", "language", "auto");
    if (!strcmp(want, g->language_applied)) return;
    snprintf(g->language_applied, sizeof g->language_applied, "%s", want);
    i18n_set_language(g->root, want);
    g->draw_dirty = true;
}

static bool gui_init(Gui *g, const char *config_dir) {
    memset(g, 0, sizeof(*g));
    g->focus = -1;
    g->exp_color_file = g->exp_color_idx = -1;
    g->field_app = -1;
    if (config_dir) {
        snprintf(g->path[FILE_CD], sizeof(g->path[0]), "%s/chromedrive/chromedrive.conf", config_dir);
        snprintf(g->path[FILE_G3], sizeof(g->path[0]), "%s/garage3d/garage3d.conf", config_dir);
    } else {
        if (!cd_config_path(g->path[FILE_CD], sizeof(g->path[0]), "chromedrive", "chromedrive.conf") ||
            !cd_config_path(g->path[FILE_G3], sizeof(g->path[0]), "garage3d", "garage3d.conf"))
            return false;
    }
    g->store[FILE_CD] = cd_config_load(g->path[FILE_CD]);
    g->store[FILE_G3] = cd_config_load(g->path[FILE_G3]);
    if (!g->store[FILE_CD] || !g->store[FILE_G3]) return false;
    /* Language before any text is built: gui_load_languages() needs the root,
     * and the status note below is drawn through tr(). */
    gui_resolve_root(g);
    /* $CHROMEDRIVE_LANG beats the config file, matching the compositor's
     * apply_language() and the CHROMEDRIVE_* override convention. */
    const char *lang_env = getenv("CHROMEDRIVE_LANG");
    snprintf(g->language_applied, sizeof g->language_applied, "%s",
             (lang_env && *lang_env) ? lang_env
                                     : cd_config_get_string(g->store[FILE_CD], "ui", "language", "auto"));
    i18n_set_language(g->root, g->language_applied);
    gui_load_languages(g);
    gui_load_post_shaders(g);
    snprintf(g->saved_note, sizeof(g->saved_note), "%s",
             file_exists(g->path[FILE_CD]) || file_exists(g->path[FILE_G3])
                 ? tr("Settings loaded") : tr("Defaults (files created on first edit)"));
    g->measure_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    g->measure = cairo_create(g->measure_surf);
    g->num_file = -1;
    g->num_idx = -1;
    if (!catalog_load(g)) return false;
    app_load_fields(g, 0);
    g->draw_dirty = true;
    return true;
}
static void gui_free(Gui *g) {
    app_commit_fields(g);
    if (g->num_idx >= 0 && !num_commit(g)) num_cancel(g);
    flush_saves(g, true);
    free(g->choices);
    g->choices = NULL;
    if (g->measure) cairo_destroy(g->measure);
    if (g->measure_surf) cairo_surface_destroy(g->measure_surf);
    cd_config_free(g->store[0]);
    cd_config_free(g->store[1]);
}

/* ------------------------------------------------------ headless utilities */

static cairo_surface_t *render_tab_image(Gui *g, int tab) {
    g->tab = tab;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1000, 625);
    cairo_t *c = cairo_create(surf);
    draw_ui(g, c);
    cairo_destroy(c);
    cairo_surface_flush(surf);
    return surf;
}
static bool save_webp(cairo_surface_t *surf, const char *path) {
    int w = cairo_image_surface_get_width(surf), h = cairo_image_surface_get_height(surf);
    uint8_t *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    uint8_t *out = NULL;
    size_t n = WebPEncodeBGRA(data, w, h, stride, 90.0f, &out);
    bool ok = false;
    if (n && out) {
        FILE *f = fopen(path, "wb");
        if (f) { ok = fwrite(out, 1, n, f) == n; fclose(f); }
    }
    if (out) WebPFree(out);
    return ok;
}
static bool click_box(Gui *g, int kind, int a, int b) {
    HitBox *box = find_box(g, kind, a, b);
    if (!box) return false;
    gui_press(g, box->x + box->w / 2, box->y + box->h / 2);
    gui_release(g);
    return true;
}
static bool right_click_box(Gui *g, int kind, int a, int b) {
    HitBox *box = find_box(g, kind, a, b);
    if (!box) return false;
    gui_right_press(g, box->x + box->w / 2, box->y + box->h / 2);
    return true;
}
static bool click_picker_desktop(Gui *g, const char *desktop_id) {
    for (int i = 0; i < g->box_count; i++) {
        HitBox *box = &g->boxes[i];
        if (box->kind != H_PICKER_ITEM || box->a < 0 || box->a >= g->choice_count) continue;
        if (!strcmp(g->choices[box->a].desktop_id, desktop_id)) {
            gui_press(g, box->x + box->w / 2, box->y + box->h / 2);
            gui_release(g);
            return true;
        }
    }
    return false;
}
static void type_text(Gui *g, const char *s) {
    for (const char *p = s; *p; p++)
        gui_key(g, (xkb_keysym_t)(unsigned char)*p, p, 1);
}
static void clear_field(Gui *g) {
    gui_key(g, XKB_KEY_End, NULL, 0);
    for (int i = 0; i < 600; i++) gui_key(g, XKB_KEY_BackSpace, NULL, 0);
}
static bool write_fixture_file(const char *path, const char *text) {
    char dir[4096];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)) return false;
    char *slash = strrchr(dir, '/');
    if (!slash) return false;
    *slash = 0;
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(dir, 0700) < 0 && errno != EEXIST) return false;
        *p = '/';
    }
    if (mkdir(dir, 0700) < 0 && errno != EEXIST) return false;
    FILE *f = fopen(path, "w");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}
static bool write_fixture(const char *root, const char *rel, const char *text) {
    char path[4096];
    return snprintf(path, sizeof(path), "%s/%s", root, rel) < (int)sizeof(path) &&
           write_fixture_file(path, text);
}
static AppChoice *find_choice(Gui *g, const char *desktop_id) {
    for (int i = 0; i < g->choice_count; i++)
        if (!strcmp(g->choices[i].desktop_id, desktop_id)) return &g->choices[i];
    return NULL;
}
static char *dup_env(const char *name) {
    const char *v = getenv(name);
    return v ? strdup(v) : NULL;
}
static void restore_env(const char *name, char *old) {
    if (old) setenv(name, old, 1);
    else unsetenv(name);
    free(old);
}

static int selftest(const char *config_dir_arg) {
    char dirbuf[] = "/tmp/cdconfig-gui-XXXXXX";
    const char *dir = config_dir_arg;
    if (!dir) {
        if (!mkdtemp(dirbuf)) { perror("mkdtemp"); return 1; }
        dir = dirbuf;
    }
    Gui g;
    if (!gui_init(&g, dir)) { fprintf(stderr, "selftest: gui_init failed\n"); return 1; }
    int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "selftest FAIL: " __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

    /* Render tab 0 (builds hitboxes), then drive it like a user would. */
    cairo_surface_t *img = render_tab_image(&g, TAB_APPS);
    cairo_surface_destroy(img);
    CHECK(click_box(&g, H_APP_ROW, 1, 0), "no app row 1 hitbox");
    CHECK(g.sel_app == 1, "app selection = %d", g.sel_app);
    img = render_tab_image(&g, TAB_APPS); cairo_surface_destroy(img);
    CHECK(click_box(&g, H_FIELD, FLD_NAME, 0), "no name field");
    clear_field(&g); type_text(&g, "GARAGE3D");
    CHECK(click_box(&g, H_FIELD, FLD_EXEC, 0), "no exec field");
    clear_field(&g); type_text(&g, "%root%/clients/garage3d/bin/garage3d");
    gui_key(&g, XKB_KEY_Return, NULL, 0);
    CHECK(click_box(&g, H_ICON_INC, 0, 0), "no icon-inc");
    while (app_icon(&g, 1) != 5) { img = render_tab_image(&g, TAB_APPS); cairo_surface_destroy(img); CHECK(click_box(&g, H_ICON_INC, 0, 0), "icon inc lost"); }
    img = render_tab_image(&g, TAB_APPS); cairo_surface_destroy(img);
    CHECK(click_box(&g, H_APP_AUTOSTART, 0, 0), "no autostart toggle"); /* was on -> off */
    /* Right-click app picker: slot 6 starts as the COCKPIT built-in. Assign
     * Settings, switch it back to the built-in, then assign Garage3D. This
     * proves both popup directions and that every slot can hold an app. */
    CHECK(g.choice_count >= 8, "app catalog has only %d entries", g.choice_count);
    CHECK(right_click_box(&g, H_APP_ROW, 5, 0), "no cockpit row for right-click");
    CHECK(g.picker_open && g.picker_slot == 5 && g.sel_app == 5, "right-click did not open picker for slot 6");
    img = render_tab_image(&g, TAB_APPS); cairo_surface_destroy(img);
    type_text(&g, "settings");
    img = render_tab_image(&g, TAB_APPS); cairo_surface_destroy(img);
    CHECK(click_picker_desktop(&g, "chromedrive.settings"), "settings choice not visible in picker");
    CHECK(!g.picker_open && app_action(&g, 5) == ACT_APP, "picker did not assign an app action to slot 6");
    CHECK(strstr(g.fields[FLD_EXEC].buf, "chromedrive-config") &&
          !strcmp(g.fields[FLD_APPID].buf, "chromedrive.config"), "settings assignment fields wrong");
    img = render_tab_image(&g, TAB_APPS); cairo_surface_destroy(img);
    CHECK(right_click_box(&g, H_APP_ROW, 5, 0), "no slot 6 row for second right-click");
    img = render_tab_image(&g, TAB_APPS); cairo_surface_destroy(img);
    type_text(&g, "Driver view");
    img = render_tab_image(&g, TAB_APPS); cairo_surface_destroy(img);
    CHECK(click_picker_desktop(&g, "chromedrive.builtin.cockpit"), "driver-view built-in not visible in picker");
    CHECK(app_action(&g, 5) == ACT_COCKPIT && !*g.fields[FLD_EXEC].buf &&
          !*g.fields[FLD_APPID].buf, "picker did not restore/clear slot 6 built-in");
    img = render_tab_image(&g, TAB_APPS); cairo_surface_destroy(img);
    CHECK(right_click_box(&g, H_APP_ROW, 5, 0), "no slot 6 row for third right-click");
    img = render_tab_image(&g, TAB_APPS); cairo_surface_destroy(img);
    type_text(&g, "Garage3D");
    gui_key(&g, XKB_KEY_Return, NULL, 0);
    CHECK(!g.picker_open && app_action(&g, 5) == ACT_APP &&
          strstr(g.fields[FLD_EXEC].buf, "garage3d") &&
          !strcmp(g.fields[FLD_APPID].buf, "chromedrive.garage3d"), "keyboard picker assignment failed");
    /* Compositor -> already-running-GUI picker request (runtime-dir IPC). */
    char reqpath[4096];
    snprintf(reqpath, sizeof(reqpath), "%s/picker-request", dir);
    picker_request_set_path(&g, reqpath);
    FILE *rf = fopen(reqpath, "w");
    CHECK(rf != NULL, "cannot write picker request");
    if (rf) { fprintf(rf, "2 selftest-nonce\n"); fclose(rf); }
    picker_request_poll(&g);
    CHECK(g.picker_open && g.picker_slot == 2 && g.sel_app == 2, "picker request did not open slot 3");
    picker_close(&g);
    picker_request_poll(&g);
    CHECK(!g.picker_open, "picker request nonce was consumed twice");
    /* Garage3D tab: flip flicker off; set fog via def API path a drag uses. */
    img = render_tab_image(&g, TAB_G3); cairo_surface_destroy(img);
    /* find flicker def index */
    int fi = -1;
    for (int i = 0; i < N_G3_DEFS; i++) if (!strcmp(G3_DEFS[i].key, "flicker")) fi = i;
    /* flicker row may be below the fold: toggle through the box if present,
     * else via the same setter the box handler calls. */
    if (!click_box(&g, H_TOGGLE, FILE_G3, fi)) {
        cd_config_set_bool(g.store[FILE_G3], "scene", "flicker", false);
        touch_file(&g, FILE_G3);
    }
    cd_config_set_double(g.store[FILE_G3], "scene", "fog_density", 0.03);
    touch_file(&g, FILE_G3);
    float orange[3] = { 1.0f, 0x80 / 255.0f, 0.0f };
    cd_config_set_color(g.store[FILE_G3], "scene", "neon_left", orange);
    img = render_tab_image(&g, TAB_CD); cairo_surface_destroy(img);
    for (int i = 0; i < N_CD_DEFS; i++)
        if (!strcmp(CD_DEFS[i].key, "background")) def_set_enum(&g, FILE_CD, &CD_DEFS[i], 1);
    /* Post shader row: bundled presets cycle through the real widget, and a
     * hand-edited custom path is injected so it displays (and is preserved). */
    int psi = -1;
    for (int i = 0; i < N_CD_DEFS; i++) if (!strcmp(CD_DEFS[i].key, "post_shader")) psi = i;
    CHECK(psi >= 0 && CD_DEFS[psi].kind == K_ENUM && CD_DEFS[psi].enum_is_string &&
          CD_DEFS[psi].enum_count == 5 &&
          !strcmp(CD_DEFS[psi].enums[0], "off") &&
          !strcmp(CD_DEFS[psi].enums[4], "shaders/hermite-downscale-mipmap.frag"),
          "post_shader presets missing");
    img = render_tab_image(&g, TAB_CD); cairo_surface_destroy(img);
    CHECK(click_box(&g, H_ENUM_INC, FILE_CD, psi), "no post_shader enum + hitbox");
    CHECK(!strcmp(cd_config_get_string(g.store[FILE_CD], "display", "post_shader", ""),
                  "shaders/khronos-pbr-neutral.frag"),
          "post_shader cycle did not write next preset");
    cd_config_set_string(g.store[FILE_CD], "display", "post_shader", "custom/hello.frag");
    gui_load_post_shaders(&g);
    CHECK(CD_DEFS[psi].enum_count == 6 &&
          !strcmp(CD_DEFS[psi].enums[5], "custom/hello.frag") &&
          def_get_enum(&g, FILE_CD, &CD_DEFS[psi]) == 5,
          "custom post_shader path not injected/selected");
    def_set_enum(&g, FILE_CD, &CD_DEFS[psi], 0);
    CHECK(!strcmp(cd_config_get_string(g.store[FILE_CD], "display", "post_shader", ""), "off"),
          "post_shader off not written");
    gui_load_post_shaders(&g);
    CHECK(CD_DEFS[psi].enum_count == 5, "post_shader custom value not dropped after reset");
    /* VNC port is a typed input, never a 1024..65535 slider. */
    int pi = -1;
    for (int i = 0; i < N_CD_DEFS; i++) if (!strcmp(CD_DEFS[i].key, "vnc_port")) pi = i;
    CHECK(pi >= 0 && CD_DEFS[pi].kind == K_INT_INPUT, "vnc_port is not an input def");
    /* The localization row and render-scale controls add height; keep both the
     * render_scale input and the Session VNC input inside the visible fold. */
    g.scroll[TAB_CD] = 270;
    img = render_tab_image(&g, TAB_CD); cairo_surface_destroy(img);
    CHECK(find_box(&g, H_NUM_FIELD, FILE_CD, pi) != NULL, "no VNC port input hitbox");
    CHECK(find_box(&g, H_SLIDER, FILE_CD, pi) == NULL, "VNC port still has a slider hitbox");
    CHECK(click_box(&g, H_NUM_FIELD, FILE_CD, pi), "cannot focus VNC port input");
    clear_field(&g); type_text(&g, "5917");
    gui_key(&g, XKB_KEY_Return, NULL, 0);
    CHECK(g.num_idx < 0 && cd_config_get_int(g.store[FILE_CD], "session", "vnc_port", -1) == 5917,
          "typed VNC port did not commit");
    img = render_tab_image(&g, TAB_CD); cairo_surface_destroy(img);
    CHECK(click_box(&g, H_NUM_FIELD, FILE_CD, pi), "cannot refocus VNC port input");
    clear_field(&g); type_text(&g, "99");
    gui_key(&g, XKB_KEY_Return, NULL, 0);
    CHECK(g.num_idx == pi && cd_config_get_int(g.store[FILE_CD], "session", "vnc_port", -1) == 5917,
          "out-of-range VNC port overwrote the saved value");
    gui_key(&g, XKB_KEY_Escape, NULL, 0);
    CHECK(g.num_idx < 0, "Escape did not cancel VNC port edit");
    /* render_scale is a typed float input (not a slider); out-of-range or
     * malformed text is never written. */
    int ri = -1;
    for (int i = 0; i < N_CD_DEFS; i++) if (!strcmp(CD_DEFS[i].key, "render_scale")) ri = i;
    CHECK(ri >= 0 && CD_DEFS[ri].kind == K_FLOAT_INPUT, "render_scale is not a float input def");
    img = render_tab_image(&g, TAB_CD); cairo_surface_destroy(img);
    CHECK(find_box(&g, H_NUM_FIELD, FILE_CD, ri) != NULL, "no render_scale input hitbox");
    CHECK(find_box(&g, H_SLIDER, FILE_CD, ri) == NULL, "render_scale still has a slider hitbox");
    CHECK(click_box(&g, H_NUM_FIELD, FILE_CD, ri), "cannot focus render_scale input");
    clear_field(&g); type_text(&g, "2.5");
    gui_key(&g, XKB_KEY_Return, NULL, 0);
    CHECK(g.num_idx < 0 && fabs(cd_config_get_double(g.store[FILE_CD], "display", "render_scale", -1) - 2.5) < 1e-9,
          "typed render_scale did not commit");
    CHECK(click_box(&g, H_NUM_FIELD, FILE_CD, ri), "cannot refocus render_scale input");
    clear_field(&g); type_text(&g, "0");
    gui_key(&g, XKB_KEY_Return, NULL, 0);
    CHECK(g.num_idx == ri && fabs(cd_config_get_double(g.store[FILE_CD], "display", "render_scale", -1) - 2.5) < 1e-9,
          "out-of-range render_scale overwrote the saved value");
    gui_key(&g, XKB_KEY_Escape, NULL, 0);
    CHECK(g.num_idx < 0, "Escape did not cancel render_scale edit");
    g.scroll[TAB_CD] = 0;
    flush_saves(&g, true);

    /* Render all three tabs; each must be visibly non-blank. */
    for (int t = 0; t < 3; t++) {
        img = render_tab_image(&g, t);
        uint8_t *px = cairo_image_surface_get_data(img);
        long lit = 0;
        for (int i = 0; i < 1000 * 625; i++)
            if (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2] > 200) lit++;
        CHECK(lit > 20000, "tab %d looks blank (lit=%ld)", t, lit);
        cairo_surface_destroy(img);
    }
    /* Verify from disk with a fresh store. */
    CdConfig *v = cd_config_load(g.path[FILE_CD]);
    CHECK(v && !strcmp(cd_config_get_string(v, "app.files", "name", ""), "GARAGE3D"), "name not saved");
    CHECK(v && strstr(cd_config_get_string(v, "app.files", "exec", ""), "garage3d/bin/garage3d"), "exec not saved");
    CHECK(v && cd_config_get_int(v, "app.files", "icon", -1) == 5, "icon not saved");
    CHECK(v && cd_config_get_bool(v, "app.files", "autostart", true) == false, "autostart not saved");
    CHECK(v && !strcmp(cd_config_get_string(v, "cockpit", "background", ""), "texture"), "background not saved");
    CHECK(v && cd_config_get_int(v, "session", "vnc_port", -1) == 5917, "VNC port not saved");
    CHECK(v && fabs(cd_config_get_double(v, "display", "render_scale", -1) - 2.5) < 1e-9, "render_scale not saved");
    CHECK(v && !strcmp(cd_config_get_string(v, "display", "post_shader", ""), "off"), "post_shader not saved");
    CHECK(v && !strcmp(cd_config_get_string(v, "app.cockpit", "action", ""), "app") &&
          !strcmp(cd_config_get_string(v, "app.cockpit", "name", ""), "Garage3D") &&
          strstr(cd_config_get_string(v, "app.cockpit", "exec", ""), "garage3d") &&
          !strcmp(cd_config_get_string(v, "app.cockpit", "app_id", ""), "chromedrive.garage3d"),
          "picker assignment for slot 6 not saved");
    cd_config_free(v);
    v = cd_config_load(g.path[FILE_G3]);
    CHECK(v && cd_config_get_bool(v, "scene", "flicker", true) == false, "flicker not saved");
    CHECK(v && fabs(cd_config_get_double(v, "scene", "fog_density", 0) - 0.03) < 1e-9, "fog not saved");
    float rgb[3];
    CHECK(v && cd_config_get_color(v, "scene", "neon_left", rgb) && rgb[0] > 0.99, "neon color not saved");
    cd_config_free(v);
    gui_free(&g);

    /* Installed-app catalog fixture: user-over-system masking, recursive
     * discovery, Flatpak fallback, Terminal/TryExec handling, and an
     * optional no-Exec DBusActivatable entry. A fresh Gui is used because
     * catalog_load() intentionally reads the environment only at init. */
    {
        char root[4096], home[4096], user_share[4096], sys_share[4096], dirs[8192];
        bool paths_ok =
            snprintf(root, sizeof(root), "%s/catalog-fixture", dir) < (int)sizeof(root) &&
            snprintf(home, sizeof(home), "%s/home", root) < (int)sizeof(home) &&
            snprintf(user_share, sizeof(user_share), "%s/user-share", root) < (int)sizeof(user_share) &&
            snprintf(sys_share, sizeof(sys_share), "%s/system-share", root) < (int)sizeof(sys_share) &&
            snprintf(dirs, sizeof(dirs), "%s", sys_share) < (int)sizeof(dirs);
        CHECK(paths_ok, "catalog fixture paths too long");
        if (paths_ok) {
            bool wrote = true;
            wrote = write_fixture(user_share, "applications/Masked.desktop",
                "[Desktop Entry]\nType=Application\nName=User Mask\nExec=/bin/true\nHidden=true\n") && wrote;
            wrote = write_fixture(sys_share, "applications/Masked.desktop",
                "[Desktop Entry]\nType=Application\nName=System Masked App\nExec=/bin/true\n") && wrote;
            wrote = write_fixture(user_share, "applications/org.example.Shadow.desktop",
                "[Desktop Entry]\nType=Application\nName=User Shadow\nExec=/bin/true\nStartupWMClass=org.example.UserShadow\n") && wrote;
            wrote = write_fixture(sys_share, "applications/org.example.Shadow.desktop",
                "[Desktop Entry]\nType=Application\nName=System Shadow\nExec=/bin/false\n") && wrote;
            wrote = write_fixture(sys_share, "applications/kde/org.example.Deep.desktop",
                "[Desktop Entry]\nType=Application\nName=Deep App\nExec=/bin/true --flag %U\nStartupWMClass=org.example.Deep\nCategories=Utility;\n") && wrote;
            wrote = write_fixture(sys_share, "applications/termapp.desktop",
                "[Desktop Entry]\nType=Application\nName=Terminal Fixture\nExec=htop %U\nTryExec=/bin/sh\nTerminal=true\nCategories=System;TerminalEmulator;\n") && wrote;
            wrote = write_fixture(sys_share, "applications/missing.desktop",
                "[Desktop Entry]\nType=Application\nName=Missing TryExec\nExec=/bin/true\nTryExec=/definitely/not/installed\n") && wrote;
            wrote = write_fixture(sys_share, "applications/dbusapp.desktop",
                "[Desktop Entry]\nType=Application\nName=DBus Fixture\nDBusActivatable=true\nStartupWMClass=org.example.DBus\n") && wrote;
            wrote = write_fixture(home, ".local/share/flatpak/exports/share/applications/org.example.Flatpak.desktop",
                "[Desktop Entry]\nType=Application\nName=Flatpak Fixture\nExec=/usr/bin/flatpak run org.example.Flatpak\n") && wrote;
            wrote = write_fixture(sys_share, "applications/devnull.desktop",
                "[Desktop Entry]\nType=Application\nName=Devnull Masked\nExec=/bin/true\n") && wrote;
            char devnull_link[4096];
            bool devnull_ok = snprintf(devnull_link, sizeof(devnull_link),
                                       "%s/applications/devnull.desktop", user_share) < (int)sizeof(devnull_link) &&
                              symlink("/dev/null", devnull_link) == 0;
            wrote = devnull_ok && wrote;
            CHECK(wrote, "could not create catalog fixture");

            char *old_home = dup_env("HOME");
            char *old_data_home = dup_env("XDG_DATA_HOME");
            char *old_data_dirs = dup_env("XDG_DATA_DIRS");
            setenv("HOME", home, 1);
            setenv("XDG_DATA_HOME", user_share, 1);
            setenv("XDG_DATA_DIRS", dirs, 1);
            Gui cg;
            bool cg_ok = gui_init(&cg, dir);
            CHECK(cg_ok, "catalog fixture gui_init failed");
            if (cg_ok) {
                AppChoice *shadow = find_choice(&cg, "org.example.Shadow");
                CHECK(!find_choice(&cg, "masked"), "hidden user entry did not mask system desktop ID");
                CHECK(!find_choice(&cg, "devnull"), "/dev/null user symlink did not mask system desktop ID");
                CHECK(shadow && !strcmp(shadow->name, "User Shadow") &&
                      !strcmp(shadow->app_id, "org.example.UserShadow"),
                      "user desktop entry did not shadow system copy");
                AppChoice *deep = find_choice(&cg, "kde-org.example.Deep");
                CHECK(deep && !strcmp(deep->app_id, "org.example.Deep") &&
                      !strstr(deep->exec, "%U"), "recursive desktop entry/field-code handling failed");
                AppChoice *term = find_choice(&cg, "termapp");
                CHECK(term && term->terminal && strstr(term->exec, "foot --title=") &&
                      strstr(term->exec, "htop") && !strstr(term->exec, "%U"),
                      "Terminal=true entry was not wrapped cleanly");
                CHECK(!find_choice(&cg, "missing"), "TryExec-missing entry was not filtered");
                CHECK(find_choice(&cg, "org.example.Flatpak") != NULL,
                      "Flatpak exports fallback was not scanned");
                AppChoice *dbus = find_choice(&cg, "dbusapp");
                if (program_available("gio")) {
                    CHECK(dbus && strstr(dbus->exec, "gio launch ") &&
                          strstr(dbus->exec, "dbusapp.desktop"),
                          "DBusActivatable no-Exec entry was not made launchable");
                } else {
                    CHECK(!dbus, "DBusActivatable entry appeared without gio");
                }
                gui_free(&cg);
            }
            restore_env("HOME", old_home);
            restore_env("XDG_DATA_HOME", old_data_home);
            restore_env("XDG_DATA_DIRS", old_data_dirs);
        }
    }
    if (fails) { fprintf(stderr, "selftest: %d failure(s)\n", fails); return 1; }
    printf("PASS: chromedrive-config selftest (app editor widgets, schema edits, file round-trip, offscreen renders)\n");
    return 0;
#undef CHECK
}

/* -------------------------------------------------------- Wayland client */

typedef struct { struct wl_buffer *wl; uint8_t *pixels; size_t bytes; int w, h; bool busy; } ShmBuffer;

typedef struct {
    Gui gui;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_touch *touch;
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *top;
    ShmBuffer buffers[2];
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    int w, h;
    bool configured, frame_ready, running, pointer_down;
    double tx, ty;              /* touch position (single active touch) */
    int touch_id;
    uint64_t frames;
} App;

static volatile sig_atomic_t stop_flag;
static void on_signal(int sig) { (void)sig; stop_flag = 1; }
static double vx(App *a, double x) { return x * 1000.0 / (a->w > 0 ? a->w : 1000); }
static double vy(App *a, double y) { return y * 625.0 / (a->h > 0 ? a->h : 625); }

static void buffer_release(void *data, struct wl_buffer *wl) {
    (void)wl; ((ShmBuffer *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static bool make_buffer(App *a, ShmBuffer *b) {
    if (b->wl) { wl_buffer_destroy(b->wl); munmap(b->pixels, b->bytes); }
    memset(b, 0, sizeof(*b));
    b->w = a->w; b->h = a->h; b->bytes = (size_t)a->w * a->h * 4;
    int fd = memfd_create("chromedrive-config", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)b->bytes) < 0) { if (fd >= 0) close(fd); return false; }
    b->pixels = mmap(NULL, b->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (b->pixels == MAP_FAILED) { close(fd); return false; }
    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int)b->bytes);
    b->wl = wl_shm_pool_create_buffer(pool, 0, a->w, a->h, a->w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->wl, &buffer_listener, b);
    return true;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time; wl_callback_destroy(cb); ((App *)data)->frame_ready = true;
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void draw_frame(App *a) {
    ShmBuffer *b = NULL;
    for (int i = 0; i < 2; i++) if (!a->buffers[i].busy) { b = &a->buffers[i]; break; }
    if (!b) return;
    if (!b->wl || b->w != a->w || b->h != a->h)
        if (!make_buffer(a, b)) { a->running = false; return; }
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        b->pixels, CAIRO_FORMAT_ARGB32, a->w, a->h, a->w * 4);
    cairo_t *c = cairo_create(surf);
    cairo_scale(c, a->w / 1000.0, a->h / 625.0);
    draw_ui(&a->gui, c);
    cairo_destroy(c);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);
    struct wl_callback *cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(cb, &frame_listener, a);
    a->frame_ready = false;
    a->gui.draw_dirty = false;
    b->busy = true;
    wl_surface_attach(a->surface, b->wl, 0, 0);
    wl_surface_damage(a->surface, 0, 0, a->w, a->h);
    wl_surface_commit(a->surface);
    a->frames++;
}

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    (void)data; xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };
static void xdg_configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
    App *a = data; xdg_surface_ack_configure(xdg, serial);
    a->configured = true; a->gui.draw_dirty = true;
}
static const struct xdg_surface_listener xdg_listener = { .configure = xdg_configure };
static void top_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h, struct wl_array *states) {
    (void)top; (void)states; App *a = data;
    if (w > 0 && h > 0) { a->w = w; a->h = h; }
}
static void top_close(void *data, struct xdg_toplevel *top) { (void)top; ((App *)data)->running = false; }
static const struct xdg_toplevel_listener top_listener = { .configure = top_configure, .close = top_close };

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
static void p_enter(void *data, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y) {
    (void)p; (void)s; (void)sf; App *a = data;
    a->gui.px = vx(a, wl_fixed_to_double(x)); a->gui.py = vy(a, wl_fixed_to_double(y));
}
static void p_leave(void *data, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) { (void)data; (void)p; (void)s; (void)sf; }
static void p_motion(void *data, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)p; (void)t; App *a = data;
    gui_motion(&a->gui, vx(a, wl_fixed_to_double(x)), vy(a, wl_fixed_to_double(y)));
}
static void p_button(void *data, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t button, uint32_t state) {
    (void)p; (void)s; (void)t; App *a = data;
    if (button == BTN_RIGHT) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) gui_right_press(&a->gui, a->gui.px, a->gui.py);
        return;
    }
    if (button != BTN_LEFT) return;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) { a->pointer_down = true; gui_press(&a->gui, a->gui.px, a->gui.py); }
    else { a->pointer_down = false; gui_release(&a->gui); }
}
static void p_axis(void *data, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
    (void)p; (void)t; App *a = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        gui_wheel(&a->gui, wl_fixed_to_double(v) / 15.0);
}
/*
 * ALL wl_pointer slots up to v5 must be non-NULL: libwayland aborts the
 * client ("listener function for opcode N of wl_pointer is NULL") when an
 * event arrives for an empty slot, and compositors (ChromeDrive included)
 * send frame/axis_source/axis_discrete routinely on v5+ pointers. We apply
 * motion/button immediately instead of batching per frame, so frame and
 * friends are deliberate no-ops; axis_discrete duplicates the continuous
 * axis values we already consume, so it is ignored to avoid double-scroll.
 * (axis_value120/axis_relative_direction are v8/v9 - the seat is capped at
 * v7 below, so no compositor may legally send them to this proxy.)
 */
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener pointer_listener = {
    .enter = p_enter, .leave = p_leave, .motion = p_motion,
    .button = p_button, .axis = p_axis, .frame = p_frame,
    .axis_source = p_axis_source, .axis_stop = p_axis_stop,
    .axis_discrete = p_axis_discrete,
};
static void k_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size) {
    (void)k; App *a = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
        if (a->keymap) xkb_keymap_unref(a->keymap);
        if (a->xkb_state) xkb_state_unref(a->xkb_state);
        a->keymap = xkb_keymap_new_from_string(a->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
        a->xkb_state = a->keymap ? xkb_state_new(a->keymap) : NULL;
        munmap(map, size);
    }
    close(fd);
}
static void k_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys) { (void)d; (void)k; (void)s; (void)sf; (void)keys; }
static void k_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d; (void)k; (void)s; (void)sf; }
static void k_key(void *data, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t state) {
    (void)k; (void)s; (void)t; App *a = data;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !a->xkb_state) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);
    char buf[64] = "";
    int n = xkb_state_key_get_utf8(a->xkb_state, key + 8, buf, sizeof(buf));
    gui_key(&a->gui, sym, n > 0 ? buf : NULL, n > 0 ? n : 0);
}
static void k_mod(void *data, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lok, uint32_t grp) {
    (void)k; (void)s; App *a = data;
    if (a->xkb_state) xkb_state_update_mask(a->xkb_state, dep, lat, lok, 0, 0, grp);
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t del) { (void)d; (void)k; (void)r; (void)del; }
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = k_keymap, .enter = k_enter, .leave = k_leave,
    .key = k_key, .modifiers = k_mod, .repeat_info = k_repeat,
};
static void t_down(void *data, struct wl_touch *t, uint32_t s, uint32_t time, struct wl_surface *sf, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)t; (void)s; (void)time; (void)sf; App *a = data;
    if (a->touch_id != -1) return;
    a->touch_id = id;
    a->tx = vx(a, wl_fixed_to_double(x)); a->ty = vy(a, wl_fixed_to_double(y));
    gui_press(&a->gui, a->tx, a->ty);
}
static void t_up(void *data, struct wl_touch *t, uint32_t s, uint32_t time, int32_t id) {
    (void)t; (void)s; (void)time; App *a = data;
    if (id != a->touch_id) return;
    a->touch_id = -1; gui_release(&a->gui);
}
static void t_motion(void *data, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)t; (void)time; App *a = data;
    if (id != a->touch_id) return;
    gui_motion(&a->gui, vx(a, wl_fixed_to_double(x)), vy(a, wl_fixed_to_double(y)));
}
static void t_frame(void *d, struct wl_touch *t) { (void)d; (void)t; }
static void t_cancel(void *data, struct wl_touch *t) { (void)t; App *a = data; a->touch_id = -1; gui_release(&a->gui); }
/* shape/orientation (v6): same NULL-slot abort rule as wl_pointer - the
 * GUI is a point-and-drag UI, so contact geometry is a deliberate no-op. */
static void t_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t maj, wl_fixed_t min) { (void)d; (void)t; (void)id; (void)maj; (void)min; }
static void t_orientation(void *d, struct wl_touch *t, int32_t id, wl_fixed_t o) { (void)d; (void)t; (void)id; (void)o; }
static const struct wl_touch_listener touch_listener = {
    .down = t_down, .up = t_up, .motion = t_motion, .frame = t_frame, .cancel = t_cancel,
    .shape = t_shape, .orientation = t_orientation,
};
static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
    App *a = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        a->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a->pointer, &pointer_listener, a);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        a->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a->keyboard, &keyboard_listener, a);
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !a->touch) {
        a->touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(a->touch, &touch_listener, a);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version) {
    App *a = data;
    if (!strcmp(iface, wl_compositor_interface.name))
        a->compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        a->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name))
        a->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, version < 3 ? version : 3);
    else if (!strcmp(iface, wl_seat_interface.name)) {
        a->seat = wl_registry_bind(r, name, &wl_seat_interface, version < 7 ? version : 7);
        wl_seat_add_listener(a->seat, &seat_listener, a);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = { .global = reg_global, .global_remove = reg_remove };

static int run_gui(const char *config_dir, int initial_slot, bool choose_app) {
    App a;
    memset(&a, 0, sizeof(a));
    a.w = 1000; a.h = 625; a.running = true; a.frame_ready = true; a.touch_id = -1;
    if (!gui_init(&a.gui, config_dir)) {
        fprintf(stderr, "chromedrive-config: cannot resolve config paths (set HOME or --config-dir)\n");
        return 1;
    }
    if (initial_slot >= 0 && initial_slot < 6) {
        a.gui.tab = TAB_APPS;
        if (choose_app) picker_open(&a.gui, initial_slot, 292, 92);
        else { a.gui.sel_app = initial_slot; a.gui.focus = -1; app_load_fields(&a.gui, initial_slot); }
    } else if (choose_app) picker_open(&a.gui, a.gui.sel_app, 292, 92);
    picker_request_init(&a.gui);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    a.xkb_ctx = xkb_context_new(0);
    a.display = wl_display_connect(NULL);
    if (!a.display) { fprintf(stderr, "chromedrive-config: no Wayland display: %s\n", strerror(errno)); gui_free(&a.gui); return 1; }
    a.registry = wl_display_get_registry(a.display);
    wl_registry_add_listener(a.registry, &registry_listener, &a);
    wl_display_roundtrip(a.display);
    if (!a.compositor || !a.shm || !a.wm) { fprintf(stderr, "chromedrive-config: missing wl_compositor/wl_shm/xdg-shell\n"); return 1; }
    if (a.wm) xdg_wm_base_add_listener(a.wm, &wm_listener, &a);
    a.surface = wl_compositor_create_surface(a.compositor);
    a.xdg = xdg_wm_base_get_xdg_surface(a.wm, a.surface);
    xdg_surface_add_listener(a.xdg, &xdg_listener, &a);
    a.top = xdg_surface_get_toplevel(a.xdg);
    xdg_toplevel_add_listener(a.top, &top_listener, &a);
    xdg_toplevel_set_app_id(a.top, "chromedrive.config");
    xdg_toplevel_set_title(a.top, "ChromeDrive Settings");
    wl_surface_commit(a.surface);
    wl_display_roundtrip(a.display);

    double last_blink = now_s();
    while (a.running && !stop_flag) {
        while (wl_display_prepare_read(a.display) != 0)
            if (wl_display_dispatch_pending(a.display) < 0) goto done;
        if (wl_display_flush(a.display) < 0 && errno != EAGAIN) { wl_display_cancel_read(a.display); break; }
        int timeout = (a.gui.focus >= 0 || a.gui.num_idx >= 0) ? 400 :
                      (a.gui.save_pending[0] || a.gui.save_pending[1] ? 60 :
                       (*a.gui.picker_request_path ? 200 : 1000));
        struct pollfd pfd = { .fd = wl_display_get_fd(a.display), .events = POLLIN };
        int ret = poll(&pfd, 1, timeout);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events(a.display) < 0) break;
        } else wl_display_cancel_read(a.display);
        if (ret < 0 && errno != EINTR) break;
        if (wl_display_dispatch_pending(a.display) < 0) break;
        if ((a.gui.focus >= 0 || a.gui.num_idx >= 0) && now_s() - last_blink > 0.53) { last_blink = now_s(); a.gui.draw_dirty = true; }
        picker_request_poll(&a.gui);
        flush_saves(&a.gui, false);
        if (a.configured && a.frame_ready && a.gui.draw_dirty) draw_frame(&a);
    }
done:
    fprintf(stderr, "[chromedrive-config] %llu frames submitted\n", (unsigned long long)a.frames);
    gui_free(&a.gui);
    if (a.top) xdg_toplevel_destroy(a.top);
    if (a.xdg) xdg_surface_destroy(a.xdg);
    if (a.surface) wl_surface_destroy(a.surface);
    for (int i = 0; i < 2; i++)
        if (a.buffers[i].wl) { wl_buffer_destroy(a.buffers[i].wl); munmap(a.buffers[i].pixels, a.buffers[i].bytes); }
    if (a.pointer && wl_pointer_get_version(a.pointer) >= 3) wl_pointer_release(a.pointer);
    if (a.keyboard && wl_keyboard_get_version(a.keyboard) >= 3) wl_keyboard_release(a.keyboard);
    if (a.touch && wl_touch_get_version(a.touch) >= 3) wl_touch_release(a.touch);
    if (a.seat && wl_seat_get_version(a.seat) >= 5) wl_seat_release(a.seat);
    if (a.wm) xdg_wm_base_destroy(a.wm);
    if (a.shm) wl_shm_destroy(a.shm);
    if (a.compositor) wl_compositor_destroy(a.compositor);
    if (a.registry) wl_registry_destroy(a.registry);
    wl_display_flush(a.display);
    wl_display_disconnect(a.display);
    if (a.xkb_state) xkb_state_unref(a.xkb_state);
    if (a.keymap) xkb_keymap_unref(a.keymap);
    if (a.xkb_ctx) xkb_context_unref(a.xkb_ctx);
    return 0;
}

/* ------------------------------------------------------------------- main */

static void usage(const char *argv0) {
    printf("chromedrive-config - settings GUI for ChromeDrive + Garage3D\n\n"
           "Usage: %s [options]\n"
           "  (no options)            Open the settings window on $WAYLAND_DISPLAY\n"
           "  --selftest              Headless widget/file round-trip test (temp dir)\n"
           "  --screenshot OUT.webp   Render one tab offscreen to a WebP and exit\n"
           "  --tab N                 Tab for --screenshot: 0 Launch Menu, 1 Garage3D, 2 Compositor\n"
           "  --slot N                Open Launch Menu at slot N (0-5)\n"
           "  --choose-app            Open the installed-app picker for --slot\n"
           "  --config-dir DIR        Use DIR as config home instead of $XDG_CONFIG_HOME/~/.config\n"
           "  --help                  This help\n\n"
           "Edits ~/.config/chromedrive/chromedrive.conf and ~/.config/garage3d/garage3d.conf;\n"
           "running apps apply changes live. Schema: docs/configuration.md.\n", argv0);
}

int main(int argc, char **argv) {
    const char *config_dir = NULL, *screenshot = NULL;
    int tab = 0, slot = -1;
    bool do_selftest = false, choose_app = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(argv[i], "--selftest")) do_selftest = true;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i];
        else if (!strcmp(argv[i], "--tab") && i + 1 < argc) tab = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--slot") && i + 1 < argc) slot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--choose-app")) choose_app = true;
        else if (!strcmp(argv[i], "--config-dir") && i + 1 < argc) config_dir = argv[++i];
        else { fprintf(stderr, "chromedrive-config: unknown option '%s'\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (do_selftest) return selftest(config_dir);
    if (screenshot) {
        Gui g;
        if (!gui_init(&g, config_dir)) { fprintf(stderr, "chromedrive-config: cannot resolve config paths\n"); return 1; }
        if (slot >= 0 && slot < 6) { g.sel_app = slot; app_load_fields(&g, slot); }
        else if (tab == TAB_APPS) app_load_fields(&g, 0);
        if (choose_app) { picker_open(&g, slot >= 0 && slot < 6 ? slot : g.sel_app, 292, 92); tab = TAB_APPS; }
        cairo_surface_t *img = render_tab_image(&g, tab < 0 ? 0 : tab > 2 ? 2 : tab);
        bool ok = save_webp(img, screenshot);
        cairo_surface_destroy(img);
        gui_free(&g);
        if (!ok) { fprintf(stderr, "chromedrive-config: screenshot to %s failed\n", screenshot); return 1; }
        fprintf(stderr, "chromedrive-config: wrote %s\n", screenshot);
        return 0;
    }
    return run_gui(config_dir, slot, choose_app);
}
