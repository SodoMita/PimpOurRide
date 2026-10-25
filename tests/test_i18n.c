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

/* Every key the shipped catalog defines must still appear verbatim in a
 * source file. This is the cost of using English text as the lookup key: if
 * someone rewords a label, its translation silently stops matching. Failing
 * `make test` is much better than discovering it in a screenshot. Skipped
 * (not failed) when run outside a checkout. */
static void dr_trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static void test_catalog_drift(void) {
    FILE *f = fopen("assets/locale/ru.lang", "r");
    if (!f) { printf("  (drift guard skipped: no assets/locale/ru.lang here)\n"); return; }

    char *corpus = source_corpus(NULL);
    if (!corpus) { fclose(f); printf("  (drift guard skipped: no src/ or clients/ here)\n"); return; }

    char *line = NULL;
    size_t cap = 0;
    int checked = 0, stale = 0;
    while (getline(&line, &cap, f) >= 0) {
        char key[1024];
        snprintf(key, sizeof key, "%s", line);
        dr_trim(key);
        if (!*key || *key == '#' || *key == ';') continue;
        char *eq = strchr(key, '=');
        if (!eq) continue;
        *eq = 0;
        dr_trim(key);           /* the loader trims the key after splitting */

        checked++;
        if (!strstr(corpus, key)) {
            fprintf(stderr, "FAIL: ru.lang key no longer in sources: \"%s\"\n", key);
            stale++;
        }
    }
    free(line);
    fclose(f);

    /* Translating a bare "%s" leaves the actual dynamic message in English.
     * Keep this source-level check beside the key-drift guard so newly added
     * global actions cannot silently reintroduce that localization hole. */
    CHECK(strstr(corpus, "tr(\"%s\")") == NULL,
          "dynamic messages translate their text, not a bare %s placeholder");
    free(corpus);

    CHECK(checked > 0, "drift guard found keys to check");
    CHECK(stale == 0, "every ru.lang key still exists verbatim in the sources");
    printf("  drift guard: %d catalog keys verified against src/ + clients/\n", checked);
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
    test_catalog_drift();

    i18n_free();
    if (failures) {
        fprintf(stderr, "FAIL: %d i18n assertion(s)\n", failures);
        return 1;
    }
    printf("PASS: i18n (catalog parsing, escapes, language resolution, search order, "
           "hot reload, language listing, catalog drift)\n");
    return 0;
}
