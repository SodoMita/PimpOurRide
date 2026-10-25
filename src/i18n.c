/* i18n.c - see include/i18n.h for the format and the search-order contract.
 *
 * Sorted-array + bsearch rather than a hash map: catalogs are small (tens to
 * low hundreds of entries), load once, and the sorted array is both simpler
 * and more cache-friendly than open addressing for that size. Lookups are on
 * the UI draw path, so we do still want them to be O(log n) rather than the
 * linear scan a naive implementation would use.
 */
#define _POSIX_C_SOURCE 200809L

#include "i18n.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One path buffer size shared by the directory list, the full catalog path,
 * and the stored g_catalog, so no snprintf can be flagged as truncating. */
#define I18N_MAX_DIR 4096
#define I18N_MAX_PATH (I18N_MAX_DIR + 64)

typedef struct {
    char *key;                  /* English source text (also the lookup key) */
    char *val;                  /* translation */
} Entry;

static char g_lang[32] = "en";          /* active language tag */
static char g_catalog[I18N_MAX_PATH] = "";  /* "" when running on English */
static Entry *g_entries;
static int g_count, g_cap;
static int g_lookups, g_misses;         /* cumulative since last init */

/* ------------------------------------------------------------- utilities */

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

/* \n \t \\ only. An unrecognised sequence is preserved verbatim so a stray
 * backslash in a translation is never silently eaten. */
static void unescape(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '\\' && r[1]) {
            switch (r[1]) {
            case 'n': *w++ = '\n'; r++; continue;
            case 't': *w++ = '\t'; r++; continue;
            case '\\': *w++ = '\\'; r++; continue;
            default: break;     /* keep both characters */
            }
        }
        *w++ = *r;
    }
    *w = 0;
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const Entry *)a)->key, ((const Entry *)b)->key);
}

/* Empty tag, "C" and "POSIX" all mean English. Territory/encoding are cut
 * off so "ru_RU.UTF-8" and "ru" name the same language. */
static void normalize_tag(const char *in, char *out, size_t n) {
    size_t i = 0;
    if (!in) in = "";
    for (; in[i] && in[i] != '.' && in[i] != '@' && i + 1 < n; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = 0;
    if (!out[0] || !strcmp(out, "c") || !strcmp(out, "posix"))
        snprintf(out, n, "en");
}

static void base_of(const char *tag, char *out, size_t n) {
    size_t i = 0;
    for (; tag[i] && tag[i] != '_' && i + 1 < n; i++) out[i] = tag[i];
    out[i] = 0;
}

/* LC_ALL, then LC_MESSAGES, then LANG - the usual precedence. */
static const char *env_lang(void) {
    static const char *vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++) {
        const char *v = getenv(vars[i]);
        if (v && *v) return v;
    }
    return "en";
}

/* ${XDG_*_HOME} or ~/<fallback>, plus "/chromedrive/locale". Returns false if
 * neither the variable nor HOME is usable. */
static bool xdg_locale_dir(char *out, size_t n, const char *var, const char *home_rel) {
    const char *base = getenv(var);
    if (base && *base) return snprintf(out, n, "%s/chromedrive/locale", base) < (int)n;
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    return snprintf(out, n, "%s/%s/chromedrive/locale", home, home_rel) < (int)n;
}

#define MAX_DIRS 4
#define MAX_DIR_LEN I18N_MAX_DIR

static int catalog_dirs(char dirs[MAX_DIRS][MAX_DIR_LEN], const char *root) {
    int n = 0;
    const char *explicit_dir = getenv("CHROMEDRIVE_LOCALE_DIR");
    if (explicit_dir && *explicit_dir)
        snprintf(dirs[n++], MAX_DIR_LEN, "%s", explicit_dir);
    char buf[MAX_DIR_LEN];
    if (xdg_locale_dir(buf, sizeof buf, "XDG_CONFIG_HOME", ".config"))
        snprintf(dirs[n++], MAX_DIR_LEN, "%s", buf);
    if (xdg_locale_dir(buf, sizeof buf, "XDG_DATA_HOME", ".local/share"))
        snprintf(dirs[n++], MAX_DIR_LEN, "%s", buf);
    if (root && *root)
        snprintf(dirs[n++], MAX_DIR_LEN, "%s/assets/locale", root);
    return n;
}

/* --------------------------------------------------------------- catalog */

static void entries_clear(void) {
    for (int i = 0; i < g_count; i++) {
        free(g_entries[i].key);
        free(g_entries[i].val);
    }
    free(g_entries);
    g_entries = NULL;
    g_count = g_cap = 0;
}

/* Last definition wins. Catalogs are small enough that the linear duplicate
 * check here is irrelevant next to the cost of parsing the file. */
static bool entry_set(const char *key, const char *val) {
    for (int i = 0; i < g_count; i++) {
        if (!strcmp(g_entries[i].key, key)) {
            char *dup = strdup(val);
            if (!dup) return false;
            free(g_entries[i].val);
            g_entries[i].val = dup;
            return true;
        }
    }
    if (g_count == g_cap) {
        int cap = g_cap ? g_cap * 2 : 64;
        Entry *grown = realloc(g_entries, (size_t)cap * sizeof *grown);
        if (!grown) return false;
        g_entries = grown;
        g_cap = cap;
    }
    char *k = strdup(key), *v = strdup(val);
    if (!k || !v) { free(k); free(v); return false; }
    g_entries[g_count].key = k;
    g_entries[g_count].val = v;
    g_count++;
    return true;
}

static bool load_catalog(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char *line = NULL;
    size_t cap = 0;
    bool first = true, warned_parse = false;

    while (getline(&line, &cap, f) >= 0) {
        char *s = line;
        if (first) {
            first = false;
            if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
                (unsigned char)s[2] == 0xBF)
                s += 3;         /* UTF-8 BOM from a Windows editor */
        }
        size_t n = strlen(s);
        while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
        trim(s);
        if (!*s || *s == '#' || *s == ';') continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            if (!warned_parse)
                fprintf(stderr, "[i18n] %s: ignoring line without '=': %s\n", path, s);
            warned_parse = true;
            continue;
        }
        *eq = 0;
        char *key = s, *val = eq + 1;
        trim(key);
        trim(val);
        if (!*key) continue;
        unescape(key);
        unescape(val);
        if (!entry_set(key, val))
            fprintf(stderr, "[i18n] %s: out of memory at key '%s'\n", path, key);
    }

    free(line);
    fclose(f);
    /* bsearch() requires sorted input, and duplicates are already collapsed. */
    if (g_count > 1) qsort(g_entries, (size_t)g_count, sizeof *g_entries, entry_cmp);
    return true;
}

/* ------------------------------------------------------------------- API */

const char *i18n_language(void) {
    return g_lang;
}

const char *i18n_catalog_path(void) {
    return g_catalog;
}

const char *tr(const char *english) {
    if (!english) return "";
    g_lookups++;
    if (g_count) {
        Entry needle = { (char *)english, NULL };
        Entry *hit = bsearch(&needle, g_entries, (size_t)g_count,
                             sizeof *g_entries, entry_cmp);
        if (hit) return hit->val;
    }
    g_misses++;
    return english;
}

void i18n_stats(int *entries, int *lookups, int *misses) {
    if (entries) *entries = g_count;
    if (lookups) *lookups = g_lookups;
    if (misses) *misses = g_misses;
}

void i18n_free(void) {
    entries_clear();
    g_catalog[0] = 0;
    g_lookups = g_misses = 0;
}

const char *i18n_set_language(const char *root, const char *requested) {
    entries_clear();
    g_catalog[0] = 0;
    g_lookups = g_misses = 0;

    char requested_tag[32];
    normalize_tag((!requested || !*requested || !strcmp(requested, "auto"))
                      ? env_lang() : requested,
                  requested_tag, sizeof requested_tag);
    snprintf(g_lang, sizeof g_lang, "en");

    char dirs[MAX_DIRS][MAX_DIR_LEN];
    int ndirs = catalog_dirs(dirs, root);
    char base[32];
    base_of(requested_tag, base, sizeof base);
    /* Exact tag first, then the base language, so ru_RU finds ru.lang. */
    const char *candidates[2] = { requested_tag, base };
    for (int c = 0; c < 2; c++) {
        if (!candidates[c][0]) continue;
        if (c == 1 && !strcmp(candidates[1], candidates[0])) break;
        for (int d = 0; d < ndirs; d++) {
            char path[I18N_MAX_PATH];
            if (snprintf(path, sizeof path, "%s/%s.lang", dirs[d], candidates[c])
                >= (int)sizeof path)
                continue;
            if (!load_catalog(path)) continue;
            snprintf(g_catalog, sizeof g_catalog, "%s", path);
            snprintf(g_lang, sizeof g_lang, "%s", candidates[c]);
            return g_lang;
        }
    }
    return g_lang;              /* English; tr() now returns its argument */
}

const char *i18n_init(const char *root, const char *requested) {
    return i18n_set_language(root, requested);
}

size_t i18n_languages(char *buf, size_t size, const char *root) {
    if (!buf || size < 4) {
        if (buf && size) buf[0] = 0;
        return 0;
    }

    enum { MAX_TAGS = 48 };
    static char tags[MAX_TAGS][32];
    int ntags = 0;

    char dirs[MAX_DIRS][MAX_DIR_LEN];
    int ndirs = catalog_dirs(dirs, root);
    for (int d = 0; d < ndirs; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t len = strlen(ent->d_name);
            if (len <= 5 || strcmp(ent->d_name + len - 5, ".lang")) continue;
            char tag[32];
            if (len - 5 >= sizeof tag) continue;
            memcpy(tag, ent->d_name, len - 5);
            tag[len - 5] = 0;
            for (size_t i = 0; tag[i]; i++) tag[i] = (char)tolower((unsigned char)tag[i]);
            if (!strcmp(tag, "en")) continue;   /* English is always first */
            bool seen = false;
            for (int i = 0; i < ntags && !seen; i++)
                if (!strcmp(tags[i], tag)) seen = true;
            if (!seen && ntags < MAX_TAGS) snprintf(tags[ntags++], 32, "%s", tag);
        }
        closedir(dir);
    }

    /* English first, then alphabetical - stable for the picker widget. */
    /* Fixed-size rows, so plain memcpy: snprintf() between rows of the same
     * array trips -Wrestrict, which cannot prove they never overlap. */
    for (int i = 1; i < ntags; i++) {
        char tmp[32];
        memcpy(tmp, tags[i], sizeof tmp);
        int j = i - 1;
        while (j >= 0 && strcmp(tags[j], tmp) > 0) {
            memcpy(tags[j + 1], tags[j], sizeof tmp);
            j--;
        }
        memcpy(tags[j + 1], tmp, sizeof tmp);
    }

    size_t used = 0;
    int written = 0;
    for (int i = -1; i < ntags; i++) {
        const char *tag = i < 0 ? "en" : tags[i];
        int need = snprintf(buf + used, size - used, "%s%s", written ? "," : "", tag);
        if (need < 0 || (size_t)need >= size - used) break;
        used += (size_t)need;
        written++;
    }
    if (!written) {
        snprintf(buf, size, "en");
        written = 1;
    }
    return (size_t)written;
}
