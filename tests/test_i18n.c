/* test_i18n.c - catalog parsing, language resolution, and catalog drift.
 *
 * Everything here is hermetic: catalogs are written into a mkdtemp() tree and
 * reached through CHROMEDRIVE_LOCALE_DIR / XDG_* so the tests never read the
 * developer's real locale files. The one exception is the drift guard at the
 * end, which reads the repository's own sources on purpose.
 */
#define _POSIX_C_SOURCE 200809L

#include "i18n.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__);    \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static char work[4096];         /* scratch root */

/* Single-level mkdir: every fixture path is directly under `work`. */
static void ensure_dir(const char *path) {
    if (mkdir(path, 0777) && errno != EEXIST) {
        fprintf(stderr, "FAIL: mkdir %s: %s\n", path, strerror(errno));
        failures++;
    }
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "FAIL: cannot write %s\n", path);
        failures++;
        return;
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void clear_env(void) {
    unsetenv("CHROMEDRIVE_LOCALE_DIR");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("LC_ALL");
    unsetenv("LC_MESSAGES");
    unsetenv("LANG");
}

/* ------------------------------------------------------------- behaviour */

static void test_english_fallback(void) {
    clear_env();
    setenv("LANG", "C", 1);
    CHECK(!strcmp(i18n_init(NULL, NULL), "en"), "C locale resolves to English");
    CHECK(!strcmp(tr("Make yourself at home."), "Make yourself at home."),
          "no catalog returns the English source unchanged");
    CHECK(tr(NULL) && !tr(NULL)[0], "tr(NULL) is an empty string, not a crash");
    CHECK(!strcmp(i18n_catalog_path(), ""), "English reports no catalog path");
}

static void test_parsing(void) {
    clear_env();
    char dir[4200], catalog[4300];
    snprintf(dir, sizeof dir, "%s/parse", work);
    ensure_dir(dir);
    snprintf(catalog, sizeof catalog, "%s/ru.lang", dir);
    /* BOM + CRLF + comments + blank lines + padding + escapes + duplicate. */
    write_file(catalog,
        "\xEF\xBB\xBF# Leading comment\r\n"
        "; semicolon comment too\r\n"
        "\r\n"
        "   Window width   =   Ширина окна   \r\n"
        "Field of view=Угол обзора\r\n"
        "Multi = line\\nbreak\r\n"
        "Tabbed = a\\tb\r\n"
        "Backslash = a\\\\b\r\n"
        "Value = keeps = signs\r\n"
        "no_equals_here\r\n"
        "  \r\n"
        "Duplicate = first\r\n"
        "Duplicate = second\r\n"
        "Empty value =\r\n");
    setenv("CHROMEDRIVE_LOCALE_DIR", dir, 1);
    CHECK(!strcmp(i18n_init(NULL, "ru"), "ru"), "ru catalog loads");
    CHECK(!strcmp(i18n_catalog_path(), catalog), "catalog path is reported");
    CHECK(!strcmp(tr("Window width"), "Ширина окна"), "whitespace trimmed on both sides");
    CHECK(!strcmp(tr("Field of view"), "Угол обзора"), "no spaces around '=' works");
    CHECK(!strcmp(tr("Multi"), "line\nbreak"), "\\n unescapes");
    CHECK(!strcmp(tr("Tabbed"), "a\tb"), "\\t unescapes");
    CHECK(!strcmp(tr("Backslash"), "a\\b"), "\\\\ unescapes");
    CHECK(!strcmp(tr("Value"), "keeps = signs"), "only the first '=' splits");
    CHECK(!strcmp(tr("Duplicate"), "second"), "duplicate key: last one wins");
    CHECK(!strcmp(tr("Empty value"), ""), "empty translation is allowed");
    CHECK(!strcmp(tr("No such key"), "No such key"), "unknown key falls back to English");

    int entries, lookups, misses;
    i18n_stats(&entries, &lookups, &misses);
    CHECK(entries == 8, "duplicate collapsed: 8 entries from the fixture");
    CHECK(lookups > 0 && misses > 0, "stats count lookups and misses");
}

static void test_resolution(void) {
    clear_env();
    char dir[4200], catalog[4300];
    snprintf(dir, sizeof dir, "%s/resolve", work);
    ensure_dir(dir);
    snprintf(catalog, sizeof catalog, "%s/ru.lang", dir);
    write_file(catalog, "Menu = Меню\n");
    setenv("CHROMEDRIVE_LOCALE_DIR", dir, 1);

    /* Territory and encoding are shed before the file lookup. */
    CHECK(!strcmp(i18n_init(NULL, "ru_RU.UTF-8"), "ru"), "ru_RU.UTF-8 resolves to ru");
    CHECK(!strcmp(tr("Menu"), "Меню"), "base-language fallback loads ru.lang");

    /* auto follows LC_ALL > LC_MESSAGES > LANG. */
    setenv("LANG", "ru_RU.UTF-8", 1);
    CHECK(!strcmp(i18n_init(NULL, "auto"), "ru"), "auto honours LANG");
    setenv("LC_MESSAGES", "C", 1);
    CHECK(!strcmp(i18n_init(NULL, "auto"), "en"), "LC_MESSAGES beats LANG");
    setenv("LC_ALL", "ru_RU.UTF-8", 1);
    CHECK(!strcmp(i18n_init(NULL, "auto"), "ru"), "LC_ALL beats LC_MESSAGES");

    clear_env();
    setenv("LANG", "POSIX", 1);
    CHECK(!strcmp(i18n_init(NULL, NULL), "en"), "POSIX resolves to English");

    /* A requested language with no catalog degrades to English, silently. */
    CHECK(!strcmp(i18n_init(NULL, "de"), "en"), "missing catalog falls back to English");
    CHECK(!strcmp(tr("Menu"), "Menu"), "...and translations are gone with it");
}

static void test_search_order(void) {
    clear_env();
    char root[4200], cfg[4300], data[4300], shipped[4300], path[4800], data_locale[4400];
    snprintf(root, sizeof root, "%s/order", work);
    snprintf(cfg, sizeof cfg, "%s/config-home", work);
    snprintf(data, sizeof data, "%s/data-home", work);
    snprintf(shipped, sizeof shipped, "%s/root", work);

    const char *dirs[] = { cfg, data, shipped };
    for (int i = 0; i < 3; i++) {
        char sub[4700];
        snprintf(sub, sizeof sub, "%s/chromedrive/locale", dirs[i]);
        if (i == 2) snprintf(sub, sizeof sub, "%s/assets/locale", dirs[i]);
        char mk[4900];
        snprintf(mk, sizeof mk, "mkdir -p '%s'", sub);
        if (system(mk)) { fprintf(stderr, "FAIL: mkdir\n"); failures++; }
        snprintf(path, sizeof path, "%s/ru.lang", sub);
        char body[64];
        snprintf(body, sizeof body, "Where = %s\n", i == 0 ? "config" : i == 1 ? "data" : "shipped");
        write_file(path, body);
    }

    setenv("XDG_CONFIG_HOME", cfg, 1);
    setenv("XDG_DATA_HOME", data, 1);
    i18n_init(shipped, "ru");
    CHECK(!strcmp(tr("Where"), "config"), "XDG_CONFIG_HOME wins");
    CHECK(!strcmp(i18n_language(), "ru"), "language tag reported");

    unsetenv("XDG_CONFIG_HOME");
    i18n_init(shipped, "ru");
    CHECK(!strcmp(tr("Where"), "data"), "XDG_DATA_HOME next");

    unsetenv("XDG_DATA_HOME");
    i18n_init(shipped, "ru");
    CHECK(!strcmp(tr("Where"), "shipped"), "<root>/assets/locale last");

    snprintf(data_locale, sizeof data_locale, "%s/chromedrive/locale", data);
    setenv("CHROMEDRIVE_LOCALE_DIR", data_locale, 1);
    i18n_init(shipped, "ru");
    CHECK(!strcmp(tr("Where"), "data"), "CHROMEDRIVE_LOCALE_DIR overrides everything");

    /* Hot switch back to English drops the catalog entirely. */
    clear_env();
    CHECK(!strcmp(i18n_set_language(shipped, "en"), "en"), "switching to en");
    CHECK(!strcmp(tr("Where"), "Where"), "English ignores catalogs");
}

static void test_languages(void) {
    clear_env();
    char dir[4200], path[4300];
    snprintf(dir, sizeof dir, "%s/list", work);
    ensure_dir(dir);
    setenv("CHROMEDRIVE_LOCALE_DIR", dir, 1);
    const char *tags[] = { "ru", "de", "fr", "en" };
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "%s/%s.lang", dir, tags[i]);
        write_file(path, "x = y\n");
    }
    snprintf(path, sizeof path, "%s/notalang.txt", dir);
    write_file(path, "x = y\n");

    char buf[256];
    size_t n = i18n_languages(buf, sizeof buf, NULL);
    CHECK(n == 4, "four languages listed (en once, .txt ignored)");
    CHECK(!strcmp(buf, "en,de,fr,ru"), "English first, then alphabetical");

    char tiny[8];
    i18n_languages(tiny, sizeof tiny, NULL);
    CHECK(strlen(tiny) < sizeof tiny, "truncation always NUL-terminates");
    char none[8];
    clear_env();
    i18n_languages(none, sizeof none, NULL);
    CHECK(!strcmp(none, "en"), "no directories still yields English");
}

/* Loaded repeatedly to catch leaks/growth under the sanitiser build. */
static void test_reload_stability(void) {
    clear_env();
    char dir[4200], path[4300];
    snprintf(dir, sizeof dir, "%s/stability", work);
    ensure_dir(dir);
    setenv("CHROMEDRIVE_LOCALE_DIR", dir, 1);
    for (int i = 0; i < 200; i++) {
        snprintf(path, sizeof path, "%s/ru.lang", dir);
        char body[128];
        snprintf(body, sizeof body, "Key%d = Value%d\n", i, i);
        char want[32];
        snprintf(want, sizeof want, "Key%d", i);
        char expect[32];
        snprintf(expect, sizeof expect, "Value%d", i);
        write_file(path, body);
        i18n_set_language(NULL, "ru");
        CHECK(!strcmp(tr(want), expect), "each reload picks up the current catalog");
        CHECK(!strcmp(tr("Key0"), i == 0 ? "Value0" : "Key0"),
              "keys dropped from the catalog fall back to English");
    }
    i18n_free();
    CHECK(!strcmp(tr("Key0"), "Key0"), "tr() still works after i18n_free()");
}

/* -------------------------------------------------------- @font / @monofont */

static void test_directives(void) {
    clear_env();
    char dir[4200], catalog[4300], zh[4300];
    snprintf(dir, sizeof dir, "%s/directives", work);
    ensure_dir(dir);
    snprintf(catalog, sizeof catalog, "%s/ru.lang", dir);
    /* Directives, padding on both sides, an unknown name, one without '='. */
    write_file(catalog,
        "@font = Noto Sans CJK SC\n"
        "   @monofont   =   Noto Sans Mono CJK SC   \n"
        "@unknown = whatever\n"
        "@without_equals\n"
        "Window width = Ширина окна\n");
    setenv("CHROMEDRIVE_LOCALE_DIR", dir, 1);
    CHECK(!strcmp(i18n_init(NULL, "ru"), "ru"), "unknown directives do not abort the load");
    CHECK(i18n_font() && !strcmp(i18n_font(), "Noto Sans CJK SC"),
          "@font is parsed and exposed");
    CHECK(i18n_monofont() && !strcmp(i18n_monofont(), "Noto Sans Mono CJK SC"),
          "@monofont is parsed and its padding trimmed");
    CHECK(!strcmp(tr("Window width"), "Ширина окна"),
          "ordinary keys still translate beside directives");

    int entries;
    i18n_stats(&entries, NULL, NULL);
    CHECK(entries == 1, "a directive is never a translation entry");
    CHECK(!strcmp(tr("@font"), "@font"), "tr(\"@font\") does not resolve");

    write_file(catalog, "@font = Double\\\\Back\nKey = З\n");
    i18n_init(NULL, "ru");
    CHECK(i18n_font() && !strcmp(i18n_font(), "Double\\Back"),
          "directive values unescape");

    write_file(catalog, "@font = First\n@font = Second\nKey = З\n");
    i18n_init(NULL, "ru");
    CHECK(i18n_font() && !strcmp(i18n_font(), "Second"),
          "duplicate directive: last one wins");

    write_file(catalog, "Key = З\n");
    i18n_init(NULL, "ru");
    CHECK(i18n_font() == NULL && i18n_monofont() == NULL,
          "reloading a catalog without directives clears both");

    CHECK(!strcmp(i18n_set_language(NULL, "de"), "en"), "missing catalog degrades to en");
    CHECK(i18n_font() == NULL && i18n_monofont() == NULL,
          "a failed catalog load leaves both directives unset");

    snprintf(zh, sizeof zh, "%s/zh.lang", dir);
    write_file(zh, "@font = Noto Sans CJK SC\n@monofont = Mono CJK\nKey = 面板\n");
    i18n_init(NULL, "zh");
    CHECK(i18n_font() && i18n_monofont(), "switching to the @font catalog sets both");
    i18n_set_language(NULL, "ru");
    CHECK(i18n_font() == NULL && i18n_monofont() == NULL,
          "switching away from a @font catalog leaves both unset");
    i18n_free();
    CHECK(i18n_font() == NULL && i18n_monofont() == NULL, "i18n_free() clears directives");
}

/* Minimal UTF-8 validator: shape, continuation bytes, overlong, surrogates,
 * range. i18n_sample_glyphs() must hand the glyph probe whole sequences. */
static bool valid_utf8(const char *s) {
    if (!s) return false;
    const unsigned char *p = (const unsigned char *)s;
    size_t len = strlen(s), i = 0;
    while (i < len) {
        int need;
        if (p[i] < 0x80) need = 1;
        else if ((p[i] & 0xE0) == 0xC0) need = 2;
        else if ((p[i] & 0xF0) == 0xE0) need = 3;
        else if ((p[i] & 0xF8) == 0xF0) need = 4;
        else return false;
        if (i + (size_t)need > len) return false;
        unsigned cp = need == 1 ? p[i] : p[i] & (need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
        for (int k = 1; k < need; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = cp << 6 | (p[i + k] & 0x3F);
        }
        if ((need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) ||
            (need == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += (size_t)need;
    }
    return true;
}

static void test_sample_glyphs(void) {
    clear_env();
    char dir[4200], catalog[4300];
    snprintf(dir, sizeof dir, "%s/glyphs", work);
    ensure_dir(dir);
    snprintf(catalog, sizeof catalog, "%s/ru.lang", dir);
    /* Glyphs only ever come from VALUES: the CJK key below must never be
     * sampled. U+200B (in ZWSP) and U+FE0F (in Cyrillic) draw no ink and
     * must be skipped; repeats must not be added twice. */
    write_file(catalog,
        "@font = Some Family\n"
        "Launch panel = 启动面板\n"
        "键 = ASCII value\n"
        "Cyrillic = АБ""\xEF""\xB8""\x8F""В\n"
        "ZWSP = 启""\xE2""\x80""\x8B""动\n"
        "Repeat = 面板\n");
    setenv("CHROMEDRIVE_LOCALE_DIR", dir, 1);
    CHECK(!strcmp(i18n_init(NULL, "ru"), "ru"), "glyph fixture loads");
    CHECK(!strcmp(i18n_font(), "Some Family"), "fixture's @font active");

    char samples[16][8];
    int n = i18n_sample_glyphs(samples, 16);
    CHECK(n == 7, "seven distinct code points, skips applied");
    CHECK(!strcmp(samples[0], "启"), "first-appearance order, not key-sorted order");
    CHECK(!strcmp(samples[3], "板"), "value glyphs appear in catalog order");
    CHECK(!strcmp(samples[4], "А"), "later entries follow");
    bool key_leaked = false;
    for (int i = 0; i < n; i++)
        key_leaked |= !strcmp(samples[i], "键");
    CHECK(!key_leaked, "glyphs come from values, never from keys");
    for (int i = 0; i < n; i++) {
        CHECK(valid_utf8(samples[i]), "sample is valid UTF-8");
        size_t bytes = strlen(samples[i]);
        CHECK(bytes >= 2 && bytes <= 4, "sample is 2-4 bytes");
    }

    char two[2][8];
    CHECK(i18n_sample_glyphs(two, 2) == 2, "cap respected");
    CHECK(!strcmp(two[1], "动"), "a capped result is still a prefix in order");
    CHECK(i18n_sample_glyphs(samples, 0) == 0, "max=0 returns nothing");
    CHECK(i18n_sample_glyphs(NULL, 4) == 0, "NULL out is tolerated");

    write_file(catalog, "Hello = World\n");
    i18n_init(NULL, "ru");
    CHECK(i18n_sample_glyphs(samples, 16) == 0, "ASCII-only catalog yields zero");

    clear_env();
    setenv("LANG", "C", 1);
    i18n_init(NULL, NULL);
    CHECK(i18n_sample_glyphs(samples, 16) == 0, "English (no catalog) yields zero");
}

/* --------------------------------------------------- utf8 byte budgeting */

/* These helpers sit directly under user-readable strings, and their budgets
 * are often computed ("sizeof(buf) - used"), not `sizeof`d. The two ways that
 * goes wrong are a negative int converted to size_t and a negative long passed
 * straight through; both would let the copy loop believe it has 2^63 bytes of
 * room. Covered here because the failure mode is a silent overrun. */
static void test_utf8_hardening(void) {
    char buf[8];

    /* Ordinary truncation, for reference. */
    memset(buf, 'X', sizeof buf);
    CHECK(i18n_utf8_truncate(buf, sizeof buf, "abcdefghij", 5) == 5, "budget 5 copies 5");
    CHECK(!strcmp(buf, "abcde"), "truncation result is the exact prefix");

    /* A negative budget converted to size_t (the classic "sizeof - used" wrap)
     * must not be believed; the destination is then the only limit. */
    memset(buf, 'X', sizeof buf);
    CHECK(i18n_utf8_truncate(buf, sizeof buf, "abcdefghij", (size_t)-1) == 7,
          "wrapped max_bytes falls back to cap - 1");
    CHECK(!strcmp(buf, "abcdefg"), "no overrun past the destination");

    /* A negative cap is not a large cap: nothing is written at all. */
    memset(buf, 'X', sizeof buf);
    CHECK(i18n_utf8_truncate(buf, (size_t)-1, "abc", 3) == 0,
          "wrapped cap is treated as no space");
    CHECK(buf[0] == 'X', "a wrapped cap must not write anything");

    /* The signed variant exists exactly for computed budgets. */
    memset(buf, 'X', sizeof buf);
    CHECK(i18n_utf8_truncate_n(buf, sizeof buf, "abcdefghij", -5) == 0,
          "negative signed budget copies nothing");
    CHECK(buf[0] == 0, "negative signed budget empties the destination");
    CHECK(i18n_utf8_truncate_n(buf, sizeof buf, "abcdefghij", 4) == 4 &&
              !strcmp(buf, "abcd"),
          "positive signed budget behaves like the size_t version");

    /* A budget that lands inside a multi-byte character stops before it. */
    char cjk[16];
    /* U+5F15 U+6587 -> 3 bytes each */
    CHECK(i18n_utf8_truncate(cjk, sizeof cjk, "\xe5\xbc\x95\xe6\x96\x87", 4) == 3,
          "a budget inside a codepoint keeps only the whole prefix");
    CHECK(!strcmp(cjk, "\xe5\xbc\x95"), "and produces valid UTF-8");

    /* Cap 0 / NULL source never crash and never write. */
    CHECK(i18n_utf8_truncate(buf, 0, "abc", 3) == 0, "cap 0 copies nothing");
    CHECK(i18n_utf8_truncate(buf, sizeof buf, NULL, 3) == 0, "NULL source copies nothing");
    CHECK(i18n_utf8_truncate(NULL, sizeof buf, "abc", 3) == 0, "NULL destination is ignored");
}

/* ------------------------------------------------------ font family source */

/* Every drawing path must ask for the catalog's family instead of naming
 * "DejaVu Sans" itself; a hardcoded family is how a translated CJK screen
 * renders as tofu. i18n_font_family() is that single source. */
static void test_font_family(void) {
    i18n_set_language(NULL, "en");
    CHECK(!strcmp(i18n_font_family(false), "DejaVu Sans"),
          "English draws with the built-in proportional family");
    CHECK(!strcmp(i18n_font_family(true), "DejaVu Sans Mono"),
          "English draws with the built-in mono family");

    char dir[4200], catalog[4300];
    snprintf(dir, sizeof dir, "%s/fam", work);
    ensure_dir(dir);
    snprintf(catalog, sizeof catalog, "%s/zz.lang", dir);
    write_file(catalog, "@font = Noto Sans CJK SC\n@monofont = Noto Sans Mono CJK SC\nKey = \xe5\xbc\x95\n");
    setenv("CHROMEDRIVE_LOCALE_DIR", dir, 1);
    CHECK(!strcmp(i18n_set_language(NULL, "zz"), "zz"), "the @font catalog loads");
    CHECK(!strcmp(i18n_font_family(false), "Noto Sans CJK SC"),
          "the catalog family wins over the built-in default");
    CHECK(!strcmp(i18n_font_family(true), "Noto Sans Mono CJK SC"),
          "the mono directive is used for mono text");

    /* A catalog with no directive keeps the defaults, and switching back to
     * English restores them: the helper is asked at draw time. */
    i18n_set_language(NULL, "en");
    CHECK(!strcmp(i18n_font_family(false), "DejaVu Sans"),
          "switching back to English restores the default family");
    unsetenv("CHROMEDRIVE_LOCALE_DIR");
}

/* -------------------------------------------------------- drift guard */

/* Reads every .c under src/ and clients/ into one buffer. */
static char *source_corpus(size_t *len) {
    const char *dirs[] = { "src", "clients" };
    size_t cap = 1 << 20, used = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = 0;

    for (int d = 0; d < 2; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) { free(buf); return NULL; }
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t n = strlen(ent->d_name);
            if (n < 3 || strcmp(ent->d_name + n - 2, ".c")) continue;
            char path[512];
            snprintf(path, sizeof path, "%s/%s", dirs[d], ent->d_name);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            for (;;) {
                if (used + 4096 > cap) {
                    cap *= 2;
                    char *grown = realloc(buf, cap);
                    if (!grown) { fclose(f); closedir(dir); free(buf); return NULL; }
                    buf = grown;
                }
                size_t got = fread(buf + used, 1, cap - used - 1, f);
                if (!got) break;
                used += got;
            }
            buf[used++] = '\n';
            fclose(f);
        }
        closedir(dir);
    }
    buf[used] = 0;
    if (len) *len = used;
    return buf;
}

static void dr_trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

/* A catalog key set, in file order. */
typedef struct {
    char **v;
    int n, cap;
} keylist;

static void keylist_add(keylist *l, const char *k) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        char **grown = realloc(l->v, (size_t)l->cap * sizeof *grown);
        if (!grown) return;
        l->v = grown;
    }
    l->v[l->n] = strdup(k);
    if (l->v[l->n]) l->n++;
}

static void keylist_free(keylist *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static int keylist_has(const keylist *l, const char *k) {
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->v[i], k)) return 1;
    return 0;
}

/* Mirrors the loader: trim, skip blanks and #/; comments, split on the first
 * '=', trim the key, and ignore translator directives (@font, @monofont).
 * Returns non-zero when the file cannot be read at all. */
static int keylist_load(const char *path, keylist *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
        char key[1024];
        snprintf(key, sizeof key, "%s", line);
        dr_trim(key);
        if (!*key || *key == '#' || *key == ';') continue;
        char *eq = strchr(key, '=');
        if (!eq) continue;
        *eq = 0;
        dr_trim(key);
        if (key[0] == '@') continue;
        keylist_add(out, key);
    }
    free(line);
    fclose(f);
    return 0;
}

/* -------------------------------------------------------- drift guard */

/* Every key a shipped catalog defines must still appear verbatim in a source
 * file. This is the cost of using English text as the lookup key: if someone
 * rewords a label, its translation silently stops matching. Failing `make
 * test` is much better than discovering it in a screenshot. Skipped (not
 * failed) when run outside a checkout.
 *
 * This guard covers every .lang file, not just ru.lang: the reworded label
 * shows up as a *stale* key in each catalog that still carries it, and on the
 * branch that added the CJK catalogs exactly that had happened to three
 * strings after upstream reworded the wheel-mode labels. */
static void test_catalog_drift(void) {
    DIR *dir = opendir("assets/locale");
    if (!dir) {
        printf("  (drift guard skipped: no assets/locale/ here)\n");
        return;
    }

    char *corpus = source_corpus(NULL);
    if (!corpus) {
        closedir(dir);
        printf("  (drift guard skipped: no src/ or clients/ here)\n");
        return;
    }

    int files = 0, checked = 0, stale = 0, printed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n < 6 || strcmp(ent->d_name + n - 5, ".lang")) continue;
        char path[512];
        snprintf(path, sizeof path, "assets/locale/%s", ent->d_name);
        keylist keys = {0};
        if (keylist_load(path, &keys)) continue;
        files++;
        for (int i = 0; i < keys.n; i++) {
            checked++;
            if (strstr(corpus, keys.v[i])) continue;
            stale++;
            if (printed++ < 12)
                fprintf(stderr, "FAIL: %s key no longer in sources: \"%s\"\n",
                        ent->d_name, keys.v[i]);
        }
        keylist_free(&keys);
    }
    closedir(dir);

    /* Translating a bare "%s" leaves the actual dynamic message in English.
     * Keep this source-level check beside the key-drift guard so newly added
     * global actions cannot silently reintroduce that localization hole. */
    CHECK(strstr(corpus, "tr(\"%s\")") == NULL,
          "dynamic messages translate their text, not a bare %s placeholder");
    free(corpus);

    CHECK(files > 0, "drift guard found catalogs to check");
    CHECK(checked > 0, "drift guard found keys to check");
    CHECK(stale == 0, "every catalog key still exists verbatim in the sources");
    printf("  drift guard: %d keys across %d catalog(s) verified against src/ + clients/\n",
           checked, files);
}

/* ------------------------------------------------------- parity guard */

/* The language menu lists every .lang under assets/locale, so a catalog that
 * is missing keys is a language a user can select and then see half in
 * English. Catching that here is the only mechanical defence: counting tr()
 * call sites does not tell you whether a *language* is complete.
 *
 * ru.lang is the reference set (it is the catalog that tracks the English
 * sources most closely), so a new English label must be added to every
 * shipped catalog. A key whose translation genuinely matches the English is
 * still listed, with the English text as its value: explicit beats absent. */
static void test_catalog_parity(void) {
    keylist base = {0};
    if (keylist_load("assets/locale/ru.lang", &base)) {
        printf("  (parity guard skipped: no assets/locale/ru.lang here)\n");
        return;
    }

    DIR *dir = opendir("assets/locale");
    if (!dir) {
        keylist_free(&base);
        printf("  (parity guard skipped: no assets/locale/ here)\n");
        return;
    }

    int catalogs = 0, gaps = 0, strays = 0, printed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n < 6 || strcmp(ent->d_name + n - 5, ".lang")) continue;
        char path[512];
        snprintf(path, sizeof path, "assets/locale/%s", ent->d_name);
        keylist keys = {0};
        if (keylist_load(path, &keys)) continue;
        catalogs++;

        int missing = 0, extra = 0;
        for (int i = 0; i < base.n; i++) {
            if (keylist_has(&keys, base.v[i])) continue;
            missing++;
            if (printed++ < 12)
                fprintf(stderr, "FAIL: %s has no entry for \"%s\"\n",
                        ent->d_name, base.v[i]);
        }
        for (int i = 0; i < keys.n; i++) {
            if (keylist_has(&base, keys.v[i])) continue;
            extra++;
            if (printed++ < 12)
                fprintf(stderr, "FAIL: %s key not in ru.lang: \"%s\"\n",
                        ent->d_name, keys.v[i]);
        }
        gaps += missing;
        strays += extra;
        printf("  parity: %-9s %3d keys %s\n", ent->d_name, keys.n,
               missing || extra ? "(INCOMPLETE)" : "ok");
        keylist_free(&keys);
    }
    closedir(dir);

    CHECK(catalogs > 1, "parity guard found catalogs to compare");
    CHECK(base.n > 0, "parity guard found a reference key set in ru.lang");
    CHECK(gaps == 0, "every shipped catalog translates every ru.lang key");
    CHECK(strays == 0, "no catalog carries keys ru.lang does not have");
    keylist_free(&base);
}

/* Removes the scratch tree. The test writes real catalogs under mkdtemp() and
 * used to leave the directory behind on every run, which turns a repeated
 * `make test` into a slow /tmp leak. */
static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char child[4300];
            snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
            struct stat st;
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(child);
            else unlink(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

int main(void) {
    snprintf(work, sizeof work, "/tmp/cd-i18n-XXXXXX");
    if (!mkdtemp(work)) {
        fprintf(stderr, "FAIL: mkdtemp\n");
        return 1;
    }

    test_english_fallback();
    test_parsing();
    test_resolution();
    test_search_order();
    test_languages();
    test_reload_stability();
    test_directives();
    test_sample_glyphs();
    test_utf8_hardening();
    test_font_family();
    test_catalog_drift();
    test_catalog_parity();

    i18n_free();
    remove_tree(work);
    if (failures) {
        fprintf(stderr, "FAIL: %d i18n assertion(s)\n", failures);
        return 1;
    }
    printf("PASS: i18n (catalog parsing, escapes, language resolution, search order, "
           "hot reload, language listing, @font directives, glyph sampling, catalog drift)\n");
    return 0;
}
