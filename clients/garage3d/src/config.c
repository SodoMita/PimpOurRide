/* config.c - typed INI loader for GarageSettings. See config.h. */
#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

void settings_defaults(GarageSettings *s) {
    memset(s, 0, sizeof(*s));
    s->width = 1280;
    s->height = 720;
    s->fullscreen = false;
    s->scale = 0;

    s->fov = 62.0f;
    s->orbit_speed = 1.0f;
    s->zoom_speed = 1.0f;
    s->pan_speed = 1.0f;
    s->move_speed = 6.0f;
    s->attract = true;
    s->attract_delay = 6.0f;

    /* Base colours x neon_intensity reproduce the original hard-coded HDR
     * emissives: teal (0.25, 2.4, 2.2), magenta (2.3, 0.35, 2.7). */
    s->neon_left[0] = 0x1A / 255.0f; s->neon_left[1] = 0xFF / 255.0f; s->neon_left[2] = 0xE9 / 255.0f;
    s->neon_right[0] = 0xF5 / 255.0f; s->neon_right[1] = 0x25 / 255.0f; s->neon_right[2] = 0xFF / 255.0f;
    s->neon_intensity = 2.5f;
    s->light_intensity = 1.0f;
    s->fog_density = 0.016f;
    s->rain = 420;
    s->mist = 1.0f;
    s->reflections = 1.0f;
    s->city_density = 0.30f; /* the near skyline layer's lit-window fraction */
    s->flicker = true;

    s->physics_enabled = true;
    s->gravity = 1.0f;
    s->lamp_swing = 18.0f;
    s->fan_speed = 1.0f;
    s->balls = true;
}

static float clampf_(float v, float lo, float hi) {
    if (!(v == v)) return lo; /* NaN guard: a bad file must not poison the scene */
    return v < lo ? lo : v > hi ? hi : v;
}
static int clampi_(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void settings_clamp(GarageSettings *s) {
    s->width = clampi_(s->width, 320, 7680);
    s->height = clampi_(s->height, 240, 4320);
    s->scale = clampi_(s->scale, 0, 4);
    s->fov = clampf_(s->fov, 30, 90);
    s->orbit_speed = clampf_(s->orbit_speed, 0.1f, 5);
    s->zoom_speed = clampf_(s->zoom_speed, 0.1f, 5);
    s->pan_speed = clampf_(s->pan_speed, 0.1f, 5);
    s->move_speed = clampf_(s->move_speed, 0.5f, 30);
    s->attract_delay = clampf_(s->attract_delay, 1, 120);
    for (int i = 0; i < 3; i++) {
        s->neon_left[i] = clampf_(s->neon_left[i], 0, 1);
        s->neon_right[i] = clampf_(s->neon_right[i], 0, 1);
    }
    s->neon_intensity = clampf_(s->neon_intensity, 0, 6);
    s->light_intensity = clampf_(s->light_intensity, 0, 3);
    s->fog_density = clampf_(s->fog_density, 0, 0.08f);
    s->rain = clampi_(s->rain, 0, 420);
    s->mist = clampf_(s->mist, 0, 2);
    s->reflections = clampf_(s->reflections, 0, 1.5f);
    s->city_density = clampf_(s->city_density, 0, 1);
    s->gravity = clampf_(s->gravity, 0.1f, 3);
    s->lamp_swing = clampf_(s->lamp_swing, 0, 45);
    s->fan_speed = clampf_(s->fan_speed, 0, 4);
}

bool settings_path(char *buf, size_t size) {
    if (!buf || size == 0) return false;
    buf[0] = '\0';
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;
    if (xdg && *xdg) n = snprintf(buf, size, "%s/garage3d/garage3d.conf", xdg);
    else if (home && *home) n = snprintf(buf, size, "%s/.config/garage3d/garage3d.conf", home);
    else return false;
    if (n < 0 || (size_t)n >= size) { buf[0] = '\0'; return false; }
    return true;
}

time_t settings_mtime(const char *path) {
    if (!path || !*path) return 0;
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

static bool parse_bool(const char *v, bool fallback) {
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") ||
        !strcasecmp(v, "on") || !strcmp(v, "1")) return true;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") ||
        !strcasecmp(v, "off") || !strcmp(v, "0")) return false;
    return fallback;
}

static double parse_num(const char *v, double fallback) {
    char *end;
    errno = 0;
    double n = strtod(v, &end);
    return (errno == 0 && end != v && isfinite(n)) ? n : fallback;
}

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool parse_color(const char *v, float rgb[3]) {
    if (*v == '#') v++;
    if (strlen(v) != 6) return false;
    float out[3];
    for (int i = 0; i < 3; i++) {
        int hi = hexval(v[2 * i]), lo = hexval(v[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (hi * 16 + lo) / 255.0f;
    }
    memcpy(rgb, out, sizeof(out));
    return true;
}

static void apply_kv(GarageSettings *s, const char *section, const char *key, const char *v) {
    if (strcmp(section, "window") == 0) {
        if (strcmp(key, "width") == 0) s->width = (int)parse_num(v, s->width);
        else if (strcmp(key, "height") == 0) s->height = (int)parse_num(v, s->height);
        else if (strcmp(key, "fullscreen") == 0) s->fullscreen = parse_bool(v, s->fullscreen);
        else if (strcmp(key, "scale") == 0) s->scale = (int)parse_num(v, s->scale);
    } else if (strcmp(section, "camera") == 0) {
        if (strcmp(key, "fov") == 0) s->fov = (float)parse_num(v, s->fov);
        else if (strcmp(key, "orbit_speed") == 0) s->orbit_speed = (float)parse_num(v, s->orbit_speed);
        else if (strcmp(key, "zoom_speed") == 0) s->zoom_speed = (float)parse_num(v, s->zoom_speed);
        else if (strcmp(key, "pan_speed") == 0) s->pan_speed = (float)parse_num(v, s->pan_speed);
        else if (strcmp(key, "move_speed") == 0) s->move_speed = (float)parse_num(v, s->move_speed);
        else if (strcmp(key, "attract") == 0) s->attract = parse_bool(v, s->attract);
        else if (strcmp(key, "attract_delay") == 0) s->attract_delay = (float)parse_num(v, s->attract_delay);
    } else if (strcmp(section, "scene") == 0) {
        if (strcmp(key, "neon_left") == 0) parse_color(v, s->neon_left);
        else if (strcmp(key, "neon_right") == 0) parse_color(v, s->neon_right);
        else if (strcmp(key, "neon_intensity") == 0) s->neon_intensity = (float)parse_num(v, s->neon_intensity);
        else if (strcmp(key, "light_intensity") == 0) s->light_intensity = (float)parse_num(v, s->light_intensity);
        else if (strcmp(key, "fog_density") == 0) s->fog_density = (float)parse_num(v, s->fog_density);
        else if (strcmp(key, "rain") == 0) s->rain = (int)parse_num(v, s->rain);
        else if (strcmp(key, "mist") == 0) s->mist = (float)parse_num(v, s->mist);
        else if (strcmp(key, "reflections") == 0) s->reflections = (float)parse_num(v, s->reflections);
        else if (strcmp(key, "city_density") == 0) s->city_density = (float)parse_num(v, s->city_density);
        else if (strcmp(key, "flicker") == 0) s->flicker = parse_bool(v, s->flicker);
    } else if (strcmp(section, "physics") == 0) {
        if (strcmp(key, "enabled") == 0) s->physics_enabled = parse_bool(v, s->physics_enabled);
        else if (strcmp(key, "gravity") == 0) s->gravity = (float)parse_num(v, s->gravity);
        else if (strcmp(key, "lamp_swing") == 0) s->lamp_swing = (float)parse_num(v, s->lamp_swing);
        else if (strcmp(key, "fan_speed") == 0) s->fan_speed = (float)parse_num(v, s->fan_speed);
        else if (strcmp(key, "balls") == 0) s->balls = parse_bool(v, s->balls);
    }
    /* anything else: a newer GUI's key, or a hand-added note - ignore */
}

bool settings_load(const char *path, GarageSettings *s) {
    settings_defaults(s);
    if (!path || !*path) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char *line = NULL;
    size_t cap = 0;
    char section[128] = "";
    while (getline(&line, &cap, f) >= 0) {
        char *t = trim(line);
        if (*t == '\0' || *t == '#' || *t == ';') continue;
        if (*t == '[') {
            char *end = strchr(t, ']');
            if (!end) continue;
            *end = '\0';
            snprintf(section, sizeof(section), "%s", trim(t + 1));
            continue;
        }
        char *eq = strchr(t, '=');
        if (!eq || eq == t) continue;
        *eq = '\0';
        char *key = trim(t);
        char *value = trim(eq + 1);
        for (char *p = value; *p; p++) { /* strip " # comment" / " ; comment" */
            if ((p[0] == '#' || p[0] == ';') && p > value && p[-1] == ' ') { p[-1] = '\0'; break; }
        }
        if (*key) apply_kv(s, section, key, trim(value));
    }
    free(line);
    fclose(f);
    settings_clamp(s);
    return true;
}
