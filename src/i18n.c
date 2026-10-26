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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One path buffer size shared by the directory list, the full catalog path,
 * and the stored g_catalog, so no snprintf can be flagged as truncating. */
#define I18N_MAX_DIR 4096
#define I18N_MAX_PATH (I18N_MAX_DIR + 64)
#define I18N_FONT_MAX 128

typedef struct {
    char *key;                  /* English source text (also the lookup key) */
    char *val;                  /* translation */
    size_t seq;                 /* load-time appearance counter, for g_order */
} Entry;

static char g_lang[32] = "en";          /* active language tag */
static char g_catalog[I18N_MAX_PATH] = "";  /* "" when running on English */
static char g_font[I18N_FONT_MAX] = "";     /* from "@font"; "" means unset */
static char g_monofont[I18N_FONT_MAX] = ""; /* from "@monofont"; "" unset */
static Entry *g_entries;
static int g_count, g_cap;
static int *g_order;                /* entry indices in first-appearance order */
static int g_load_seq;              /* counter behind g_entries[].seq */
static int g_lookups, g_misses;         /* cumulative since last init */

/* Untranslated-string audit. tr() is the single choke point every drawn label
 * passes through, so a string that reaches the screen with no catalog entry is
 * exactly a miss here. Counting tr("...") call sites cannot see that: the
 * settings rows draw tr(d->label), where the text arrives from a table, and
 * that is how a whole column stayed English in all four catalogs. Enabled by
 * CHROMEDRIVE_TEXT_AUDIT; see tools/check-client-overflow.sh. */
#define AUDIT_MAX 512            /* distinct strings remembered */
#define AUDIT_PRINT_MAX 200      /* lines printed before going quiet */
static int g_audit = -1;         /* -1 = environment not read yet */
static const char *g_audit_seen[AUDIT_MAX];
static int g_audit_n, g_audit_printed;

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
    free(g_order);
    g_order = NULL;
}

/* Catalog metadata directives (@font / @monofont). Empty means unset. */
static void directives_clear(void) {
    g_font[0] = 0;
    g_monofont[0] = 0;
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
    g_entries[g_count].seq = (size_t)g_load_seq++;
    g_count++;
    return true;
}

static bool load_catalog(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    /* A fresh load is a fresh file: directives reset even mid-parse, and the
     * appearance counter restarts for g_order. */
    directives_clear();
    g_load_seq = 0;

    /* "Warn once per unknown name" bookkeeping lives only inside this load. */
    bool seen_font = false, seen_monofont = false;
    char warned_unknown[32][64];
    int n_warned_unknown = 0;

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

        /* A metadata directive, never a translation entry. */
        if (*s == '@') {
            char *eq = strchr(s, '=');
            if (!eq) {
                if (!warned_parse)
                    fprintf(stderr, "[i18n] %s: ignoring line without '=': %s\n", path, s);
                warned_parse = true;
                continue;
            }
            *eq = 0;
            char *name = s + 1, *val = eq + 1;
            trim(name);
            char *dst = NULL;
            if (!strcmp(name, "font")) dst = g_font;
            else if (!strcmp(name, "monofont")) dst = g_monofont;
            if (dst) {
                trim(val);
                unescape(val);
                bool *seen = dst == g_font ? &seen_font : &seen_monofont;
                if (*seen)
                    fprintf(stderr, "[i18n] %s: duplicate directive '@%s', last value wins\n",
                            path, name);
                *seen = true;
                snprintf(dst, I18N_FONT_MAX, "%s", val);
                continue;
            }
            /* Unknown name: ignore the line and warn once per name, so a
             * newer catalog may carry directives this binary does not know
             * about yet. */
            bool known_w = false;
            for (int u = 0; u < n_warned_unknown && !known_w; u++)
                known_w = !strcmp(warned_unknown[u], name);
            if (!known_w) {
                fprintf(stderr, "[i18n] %s: ignoring unknown directive '@%s'\n",
                        path, name);
                if (n_warned_unknown < (int)(sizeof warned_unknown / sizeof *warned_unknown))
                    snprintf(warned_unknown[n_warned_unknown++],
                             sizeof *warned_unknown, "%s", name);
            }
            continue;
        }

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

    /* Restore the file's appearance order over the now key-sorted entries
     * for i18n_sample_glyphs(). Indices, not pointers, so the realloc()'d
     * g_entries array cannot leave this stale. Small arrays: a plain
     * insertion sort beats qsort_r, which is not portable at -std=c11. */
    free(g_order);
    g_order = NULL;
    if (g_count > 0) {
        g_order = malloc((size_t)g_count * sizeof *g_order);
        if (g_order) {
            for (int i = 0; i < g_count; i++) g_order[i] = i;
            for (int i = 1; i < g_count; i++) {
                int tmp = g_order[i], j = i - 1;
                while (j >= 0 && g_entries[g_order[j]].seq > g_entries[tmp].seq) {
                    g_order[j + 1] = g_order[j];
                    j--;
                }
                g_order[j + 1] = tmp;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------- API */

const char *i18n_language(void) {
    return g_lang;
}

const char *i18n_catalog_path(void) {
    return g_catalog;
}

/* Reports one string that came back untranslated. Single short tokens are
 * skipped on purpose: "COCKPIT", "NEON", "auto", "headless", "windowed" are
 * labels and values the catalogs keep in English by design, and reporting them
 * would bury the prose. Anything with a space in it, or long enough to be a
 * sentence, is a translation gap. */
static void audit_miss(const char *english) {
    if (g_audit < 0) g_audit = getenv("CHROMEDRIVE_TEXT_AUDIT") != NULL;
    if (!g_audit || !g_count) return;
    if (!strchr(english, ' ') && strlen(english) < 12) return;
    for (int i = 0; i < g_audit_n; i++)
        if (!strcmp(g_audit_seen[i], english)) return;
    if (g_audit_n == AUDIT_MAX) return;
    g_audit_seen[g_audit_n++] = english;
    if (g_audit_printed >= AUDIT_PRINT_MAX) return;
    g_audit_printed++;
    fprintf(stderr, "[text-audit] untranslated \"%s\"\n", english);
}

const char *tr(const char *english) {
    if (!english) return "";
    g_lookups++;
    if (g_count) {
        Entry needle = { (char *)english, NULL, 0 };
        Entry *hit = bsearch(&needle, g_entries, (size_t)g_count,
                             sizeof *g_entries, entry_cmp);
        if (hit) return hit->val;
    }
    g_misses++;
    audit_miss(english);
    return english;
}

/* Returns the length of the codepoint starting at `p` (1..4), or 1 for a byte
 * that cannot start one - malformed input must not stall the copy. */
static size_t utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t i18n_utf8_truncate(char *dst, size_t cap, const char *src, size_t max_bytes) {
    if (!dst) return 0;
    /* A size above PTRDIFF_MAX cannot describe an object, so it is a negative
     * int that was converted on the way in: "sizeof(buf) - used" with used >
     * sizeof(buf), or a similar computed budget. Trusting it would let the copy
     * loop run off the end of a real buffer. Treat a wrapped cap as no space,
     * and a wrapped budget as "the destination is the only limit". */
    if (cap > (size_t)PTRDIFF_MAX) cap = 0;
    if (max_bytes > (size_t)PTRDIFF_MAX) max_bytes = cap ? cap - 1 : 0;
    if (!cap) return 0;
    dst[0] = 0;
    if (!src) return 0;
    size_t out = 0;
    for (size_t i = 0; src[i] && out < cap - 1;) {
        size_t n = utf8_len((unsigned char)src[i]);
        /* Stop before a codepoint that would straddle the byte limit, and
         * before one that does not fit the destination at all. */
        if (i + n > max_bytes || out + n >= cap) break;
        memcpy(dst + out, src + i, n);
        out += n;
        i += n;
    }
    dst[out] = 0;
    return out;
}

/* Signed budget: the caller computed it and it can be negative. Negative means
 * no room, which must not turn into SIZE_MAX bytes of room. */
size_t i18n_utf8_truncate_n(char *dst, size_t cap, const char *src, long max_bytes) {
    if (!dst) return 0;
    if (max_bytes < 0) {
        if (cap) dst[0] = 0;
        return 0;
    }
    return i18n_utf8_truncate(dst, cap, src, (size_t)max_bytes);
}

/* Family the loaded catalog asks for via its @font / @monofont directives,
 * or NULL when it asked for nothing ("" is unset, per include/i18n.h). */
const char *i18n_font(void) {
    return g_font[0] ? g_font : NULL;
}

const char *i18n_monofont(void) {
    return g_monofont[0] ? g_monofont : NULL;
}

/* One place defines the fallback family, so no drawing path has to spell
 * "DejaVu Sans" itself - a hardcoded family is how a fully translated CJK
 * screen renders as tofu. clients/ and the compositor both go through here. */
const char *i18n_font_family(bool mono) {
    const char *asked = mono ? i18n_monofont() : i18n_font();
    if (asked) return asked;
    return mono ? "DejaVu Sans Mono" : "DejaVu Sans";
}

/* Samples are harvested live from the entries in appearance order (g_order),
 * so they can never drift away from what tr() actually returns. Malformed
 * catalog bytes are skipped, never emitted: the probe renders what it is
 * given, so it must only ever receive whole UTF-8 sequences. */
int i18n_sample_glyphs(char out[][8], int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < g_count && n < max; i++) {
        const char *val = g_entries[g_order ? g_order[i] : i].val;
        size_t vlen = strlen(val);
        for (size_t b = 0; b < vlen && n < max;) {
            unsigned char lead = (unsigned char)val[b];
            if (lead < 0x80) { b++; continue; }   /* ASCII only draws as-is */
            size_t len = utf8_len(lead);
            bool ok = len >= 2 && b + len <= vlen;
            unsigned cp = len == 2 ? lead & 0x1F
                        : len == 3 ? lead & 0x0F : len == 4 ? lead & 0x07 : 0;
            if (ok) {
                for (size_t k = 1; k < len; k++) {
                    unsigned char cc = (unsigned char)val[b + k];
                    if ((cc & 0xC0) != 0x80) { ok = false; break; }
                    cp = (cp << 6) | (cc & 0x3F);
                }
                if (ok && ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                           (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
                           (cp >= 0xD800 && cp <= 0xDFFF)))
                    ok = false;
            }
            if (!ok) { b++; continue; }   /* never emit a partial sequence */
            /* Zero-width / variation selectors draw no ink; a coverage probe
             * fed them would fail every font systematically. */
            if ((cp >= 0x200B && cp <= 0x200F) || cp == 0xFE0F) { b += len; continue; }
            bool have = false;
            for (int r = 0; r < n && !have; r++)     /* UTF-8 is canonical, so
                                                      * equal bytes == equal cp */
                have = strlen(out[r]) == len && !memcmp(out[r], val + b, len);
            if (!have) {
                memcpy(out[n], val + b, len);
                out[n][len] = 0;
                n++;
            }
            b += len;
        }
    }
    return n;
}

bool i18n_root_from_exe(char *buf, size_t size) {
    if (!buf || size < 2) return false;
    buf[0] = 0;
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) { buf[0] = 0; return false; }
    buf[n] = 0;
    for (int strip = 0; strip < 2; strip++) {   /* binary name, then bin/ */
        char *slash = strrchr(buf, '/');
        if (!slash) { buf[0] = 0; return false; }
        *slash = 0;
    }
    return true;
}

void i18n_stats(int *entries, int *lookups, int *misses) {
    if (entries) *entries = g_count;
    if (lookups) *lookups = g_lookups;
    if (misses) *misses = g_misses;
}

void i18n_free(void) {
    entries_clear();
    directives_clear();
    g_catalog[0] = 0;
    g_lookups = g_misses = 0;
    g_audit_n = g_audit_printed = 0;
}

const char *i18n_set_language(const char *root, const char *requested) {
    entries_clear();
    directives_clear();
    g_catalog[0] = 0;
    g_lookups = g_misses = 0;
    g_audit_n = g_audit_printed = 0;

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
