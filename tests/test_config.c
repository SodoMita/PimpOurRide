/*
 * test_config.c - unit tests for the cdconfig INI store (src/cdconfig.c).
 * Covers: defaults on a missing file, typed getters, malformed values,
 * unknown-key preservation through a save round-trip, atomic save + mkdir,
 * colour round-trip, and mtime change detection (the hot-reload trigger).
 */
#define _POSIX_C_SOURCE 200809L
#include "cdconfig.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

int main(void) {
    char dir[] = "/tmp/cdconfig-test-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char path[512], sub[512];
    snprintf(path, sizeof(path), "%s/a.conf", dir);
    snprintf(sub, sizeof(sub), "%s/nested/deeper/b.conf", dir);

    /* Missing file -> empty store, getters return defaults, mtime 0. */
    CdConfig *cfg = cd_config_load(path);
    CHECK(cfg != NULL);
    CHECK(cd_config_get_int(cfg, "display", "width", 1600) == 1600);
    CHECK(cd_config_get_bool(cfg, "cockpit", "hud", true) == true);
    CHECK(!cd_config_has(cfg, "display", "width"));
    CHECK(cd_config_mtime(path) == 0);
    cd_config_free(cfg);

    /* Hand-written file with comments, odd spacing, an unknown key and a
     * malformed number. */
    write_text(path,
        "# a comment\n"
        "; another\n"
        "[display]\n"
        "width=1920\n"
        "height = 1080\n"
        "fps = not-a-number\n"
        "fov = 55.5 trailing\n"
        "post_shader = a.frag ; b.frag\n"
        "[cockpit]\n"
        "hud = off\n"
        "neon = 2\n"
        "custom_future_key = keep me\n"
        "[scene]\n"
        "neon_left = #1AFFE9\n"
        "rain = 137 # inline comment\n");
    cfg = cd_config_load(path);
    CHECK(cd_config_get_int(cfg, "display", "width", 0) == 1920);
    CHECK(cd_config_get_int(cfg, "display", "height", 0) == 1080);
    CHECK(cd_config_get_int(cfg, "display", "fps", 60) == 60); /* malformed -> default */
    CHECK(fabs(cd_config_get_double(cfg, "display", "fov", 0) - 55.5) < 1e-9);
    CHECK(strcmp(cd_config_get_string(cfg, "display", "post_shader", ""),
                 "a.frag ; b.frag") == 0); /* spaces around ';' are data, not a comment */
    CHECK(cd_config_get_bool(cfg, "cockpit", "hud", true) == false);
    CHECK(cd_config_get_int(cfg, "cockpit", "neon", 0) == 2);
    CHECK(cd_config_get_int(cfg, "scene", "rain", 0) == 137);
    float rgb[3] = {0, 0, 0};
    CHECK(cd_config_get_color(cfg, "scene", "neon_left", rgb));
    CHECK(fabs(rgb[0] - 0x1A / 255.0f) < 1e-6);
    CHECK(fabs(rgb[1] - 1.0f) < 1e-6);
    CHECK(fabs(rgb[2] - 0xE9 / 255.0f) < 1e-6);
    time_t m1 = cd_config_mtime(path);
    CHECK(m1 != 0);

    /* Edit like the GUI would, save to a nested path (mkdir-parents), and
     * verify the round-trip: known keys updated, UNKNOWN key preserved. */
    cd_config_set_int(cfg, "display", "width", 1280);
    cd_config_set_bool(cfg, "cockpit", "hud", true);
    float c2[3] = {1.0f, 0.0f, 0.5f};
    cd_config_set_color(cfg, "scene", "neon_right", c2);
    CHECK(cd_config_save(cfg, sub));
    cd_config_free(cfg);

    CdConfig *re = cd_config_load(sub);
    CHECK(cd_config_get_int(re, "display", "width", 0) == 1280);
    CHECK(cd_config_get_int(re, "display", "height", 0) == 1080);
    CHECK(cd_config_get_bool(re, "cockpit", "hud", false) == true);
    CHECK(strcmp(cd_config_get_string(re, "cockpit", "custom_future_key", ""), "keep me") == 0);
    float rgb2[3] = {0, 0, 0};
    CHECK(cd_config_get_color(re, "scene", "neon_right", rgb2));
    CHECK(fabs(rgb2[0] - 1.0f) < 1e-6 && fabs(rgb2[2] - 128 / 255.0f) < 1e-6);
    cd_config_free(re);

    /* mtime moves after a rewrite (hot-reload trigger). Force a distinct
     * timestamp rather than sleeping: touch with a future time via utimensat
     * is overkill - one second resolution means sleep(1) is the honest test. */
    sleep(1);
    write_text(path, "[display]\nwidth=800\n");
    CHECK(cd_config_mtime(path) > m1);

    /* XDG path resolution. */
    char buf[512];
    setenv("XDG_CONFIG_HOME", "/tmp/cfgx", 1);
    CHECK(cd_config_path(buf, sizeof(buf), "garage3d", "garage3d.conf"));
    CHECK(strcmp(buf, "/tmp/cfgx/garage3d/garage3d.conf") == 0);
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/home/tester", 1);
    CHECK(cd_config_path(buf, sizeof(buf), "chromedrive", "chromedrive.conf"));
    CHECK(strcmp(buf, "/home/tester/.config/chromedrive/chromedrive.conf") == 0);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS: cdconfig store (defaults, typed getters, unknown-key round-trip, "
           "atomic save, mtime, XDG paths)\n");
    return 0;
}
