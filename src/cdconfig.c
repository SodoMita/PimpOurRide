/* cdconfig.c - see include/cdconfig.h. Plain C, libc only. */
#define _POSIX_C_SOURCE 200809L
#include "cdconfig.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *section;
    char *key;
    char *value;
} Entry;

struct CdConfig {
    Entry *entries;
    size_t count, cap;
};

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

static Entry *find_entry(const CdConfig *cfg, const char *section, const char *key) {
    if (!cfg) return NULL;
    for (size_t i = 0; i < cfg->count; i++) {
        Entry *e = (Entry *)&cfg->entries[i];
        if (strcmp(e->section, section) == 0 && strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

void cd_config_set_string(CdConfig *cfg, const char *section, const char *key,
                          const char *value) {
    if (!cfg || !section || !key || !value) return;
    Entry *e = find_entry(cfg, section, key);
    if (e) {
        char *v = xstrdup(value);
        if (!v) return;
        free(e->value);
        e->value = v;
        return;
    }
    if (cfg->count == cfg->cap) {
        size_t cap = cfg->cap ? cfg->cap * 2 : 32;
        Entry *grown = realloc(cfg->entries, cap * sizeof(*grown));
        if (!grown) return;
        cfg->entries = grown;
        cfg->cap = cap;
    }
    Entry *n = &cfg->entries[cfg->count];
    n->section = xstrdup(section);
    n->key = xstrdup(key);
    n->value = xstrdup(value);
    if (!n->section || !n->key || !n->value) {
        free(n->section); free(n->key); free(n->value);
        return;
    }
    cfg->count++;
}

CdConfig *cd_config_load(const char *path) {
    CdConfig *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;
    if (!path) return cfg;
    FILE *f = fopen(path, "r");
    if (!f) return cfg; /* absent config = all defaults, not an error */

    char *line = NULL;
    size_t cap = 0;
    char section[128] = "";
    while (getline(&line, &cap, f) >= 0) {
        char *s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) continue;
            *end = '\0';
            snprintf(section, sizeof(section), "%s", trim(s + 1));
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || eq == s) continue;
        *eq = '\0';
        char *key = trim(s);
        char *value = trim(eq + 1);
        /* Tolerate an inline '#' comment on plain string values; our own
         * writer never emits one. ';' is a full-line comment only: values such
         * as display.post_shader are ';'-separated lists and may put spaces
         * around the separators, which must not truncate the list. */
        for (char *p = value; *p; p++) {
            if (p[0] == '#' && p > value && p[-1] == ' ') {
                p[-1] = '\0';
                break;
            }
        }
        if (*key) cd_config_set_string(cfg, section, key, trim(value));
    }
    free(line);
    fclose(f);
    return cfg;
}

void cd_config_free(CdConfig *cfg) {
    if (!cfg) return;
    for (size_t i = 0; i < cfg->count; i++) {
        free(cfg->entries[i].section);
        free(cfg->entries[i].key);
        free(cfg->entries[i].value);
    }
    free(cfg->entries);
    free(cfg);
}

static bool mkdir_parents(const char *path) {
    char buf[4096];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) return false;
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0700) < 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return true; /* the final component (the file) is intentionally skipped */
}

bool cd_config_save(const CdConfig *cfg, const char *path) {
    if (!cfg || !path) return false;
    if (!mkdir_parents(path)) return false;
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return false;
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "# Written by chromedrive-config. Hand edits are fine; running\n"
               "# apps re-read this file when it changes. Comments/blank lines\n"
               "# are not preserved by the GUI, unknown keys are.\n");
    const char *last_section = NULL;
    for (size_t i = 0; i < cfg->count; i++) {
        const Entry *e = &cfg->entries[i];
        if (!last_section || strcmp(last_section, e->section) != 0) {
            fprintf(f, "\n[%s]\n", e->section);
            last_section = e->section;
        }
        fprintf(f, "%s = %s\n", e->key, e->value);
    }
    bool ok = fflush(f) == 0;
    if (ok) (void)fsync(fileno(f)); /* best effort durability */
    ok = fclose(f) == 0 && ok;
    if (!ok) { unlink(tmp); return false; }
    return rename(tmp, path) == 0; /* atomic for mtime pollers */
}

bool cd_config_path(char *buf, size_t size, const char *app, const char *file) {
    if (!buf || size == 0) return false;
    buf[0] = '\0';
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;
    if (xdg && *xdg) n = snprintf(buf, size, "%s/%s/%s", xdg, app, file);
    else if (home && *home) n = snprintf(buf, size, "%s/.config/%s/%s", home, app, file);
    else return false;
    if (n < 0 || (size_t)n >= size) { buf[0] = '\0'; return false; }
    return true;
}

time_t cd_config_mtime(const char *path) {
    if (!path || !*path) return 0;
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

bool cd_config_has(const CdConfig *cfg, const char *section, const char *key) {
    return find_entry(cfg, section, key) != NULL;
}

const char *cd_config_get_string(const CdConfig *cfg, const char *section,
                                 const char *key, const char *fallback) {
    Entry *e = find_entry(cfg, section, key);
    return e ? e->value : fallback;
}

long cd_config_get_int(const CdConfig *cfg, const char *section,
                       const char *key, long fallback) {
    const char *v = cd_config_get_string(cfg, section, key, NULL);
    if (!v) return fallback;
    char *end;
    errno = 0;
    long n = strtol(v, &end, 10);
    return (errno == 0 && end != v) ? n : fallback;
}

double cd_config_get_double(const CdConfig *cfg, const char *section,
                            const char *key, double fallback) {
    const char *v = cd_config_get_string(cfg, section, key, NULL);
    if (!v) return fallback;
    char *end;
    errno = 0;
    double n = strtod(v, &end);
    return (errno == 0 && end != v) ? n : fallback;
}

bool cd_config_get_bool(const CdConfig *cfg, const char *section,
                        const char *key, bool fallback) {
    const char *v = cd_config_get_string(cfg, section, key, NULL);
    if (!v) return fallback;
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") ||
        !strcasecmp(v, "on") || !strcmp(v, "1")) return true;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") ||
        !strcasecmp(v, "off") || !strcmp(v, "0")) return false;
    return fallback;
}

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool cd_config_get_color(const CdConfig *cfg, const char *section,
                         const char *key, float rgb[3]) {
    const char *v = cd_config_get_string(cfg, section, key, NULL);
    if (!v) return false;
    if (*v == '#') v++;
    if (strlen(v) != 6) return false;
    for (int i = 0; i < 3; i++) {
        int hi = hexval(v[2 * i]), lo = hexval(v[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        rgb[i] = (hi * 16 + lo) / 255.0f;
    }
    return true;
}

void cd_config_set_int(CdConfig *cfg, const char *section, const char *key, long value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", value);
    cd_config_set_string(cfg, section, key, buf);
}

void cd_config_set_double(CdConfig *cfg, const char *section, const char *key, double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6g", value);
    cd_config_set_string(cfg, section, key, buf);
}

void cd_config_set_bool(CdConfig *cfg, const char *section, const char *key, bool value) {
    cd_config_set_string(cfg, section, key, value ? "true" : "false");
}

void cd_config_set_color(CdConfig *cfg, const char *section, const char *key,
                         const float rgb[3]) {
    char buf[8];
    unsigned c[3];
    for (int i = 0; i < 3; i++) {
        float f = rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i];
        c[i] = (unsigned)(f * 255.0f + 0.5f);
    }
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", c[0], c[1], c[2]);
    cd_config_set_string(cfg, section, key, buf);
}
