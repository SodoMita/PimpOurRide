#define _POSIX_C_SOURCE 200809L /* mkdtemp, setenv */
/* Tests for the glyph-coverage probe (src/fontprobe.c) and its interaction
 * with the catalog directives from src/i18n.c.
 *
 * The probe's whole job is to answer "will this catalog actually paint, or a
 * screen of .notdef boxes?" - so the tests are phrased that way:
 *
 *   - a family that certainly lacks a glyph must be reported as uncovered;
 *   - a family that certainly has one must be reported as covered;
 *   - the catalog probe must follow the catalog's @font directive, not the
 *     caller's default;
 *   - and a catalog whose directive names a nonexistent family must fail, with
 *     the first missing code point reported.
 *
 * CJK coverage depends on fonts being installed, which is not a property of
 * this program. When Noto Sans CJK is absent the CJK assertions are SKIPPED
 * and counted, never silently passed - a probe test that always passes is
 * worse than no test. */
#include "fontprobe.h"
#include "i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks, skipped;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
        }                                                                     \
    } while (0)

/* Kept far away from anything a UI font assigns. This is the FIRST non-ASCII
 * code point of the test catalog below, which is what the probe must report. */
static const char *CJK_SAMPLE = "\xe5\xbc\x95";      /* U+5F15 引 */
static const char *BOGUS_FAMILY = "No Such Family 12345";

static bool have_family(const char *family) {
    /* A bogus family resolves to whatever fontconfig picks as a fallback; that
     * font is a real family name, so detect the real thing by asking cairo to
     * cover a CJK glyph through it. */
    return cd_font_covers(family, CJK_SAMPLE);
}

/* Write a catalog into a private temp dir and point the search path at it. */
static char g_dir[256];
static bool make_catalog(const char *tag, const char *body) {
    snprintf(g_dir, sizeof(g_dir), "/tmp/cd-fontprobe-XXXXXX");
    if (!mkdtemp(g_dir)) return false;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.lang", g_dir, tag);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(body, 1, strlen(body), f) == strlen(body);
    fclose(f);
    setenv("CHROMEDRIVE_LOCALE_DIR", g_dir, 1);
    return ok;
}

static void drop_catalog(void) {
    if (g_dir[0]) {
        char path[512];
        snprintf(path, sizeof(path), "%s/xx.lang", g_dir);
        unlink(path);
        snprintf(path, sizeof(path), "%s/yy.lang", g_dir);
        unlink(path);
        rmdir(g_dir);
        g_dir[0] = 0;
    }
    unsetenv("CHROMEDRIVE_LOCALE_DIR");
    i18n_free();
}

static void test_coverage_basics(void) {
    /* Every UI font has Latin. */
    CHECK(cd_font_covers("DejaVu Sans", "A"), "DejaVu Sans must cover 'A'");
    /* Nothing sane has a glyph for a bogus family name... it falls back, so the
     * meaningful assertion is the opposite direction: the fallback DOES render
     * 'A' (i.e. the probe is not simply always false). */
    CHECK(cd_font_covers(BOGUS_FAMILY, "A"),
          "a bogus family falls back to a real font for Latin, probe must not be always-false");
    /* Empty/NULL never crash and never block a language. */
    CHECK(cd_font_covers(NULL, "A"), "NULL family must fall back, not crash");
    CHECK(cd_font_covers("DejaVu Sans", ""), "empty sample must report covered");
    CHECK(cd_font_covers("DejaVu Sans", NULL), "NULL sample must report covered");
    /* The reference code point is covered by definition. */
    CHECK(cd_font_covers("DejaVu Sans", "\xef\xa3\xbf"), "U+F8FF reference must not be judged");
}

static void test_cjk_direction(void) {
    /* DejaVu has no CJK: this is the measured fact the whole slice rests on. */
    CHECK(!cd_font_covers("DejaVu Sans", CJK_SAMPLE),
          "DejaVu Sans must NOT report coverage for U+5F15");
    if (have_family("Noto Sans CJK SC")) {
        CHECK(cd_font_covers("Noto Sans CJK SC", CJK_SAMPLE),
              "Noto Sans CJK SC must cover U+5F15");
        CHECK(cd_font_covers("Noto Sans CJK JP", CJK_SAMPLE),
              "Noto Sans CJK JP must cover U+5F15 (shared Han block)");
    } else {
        skipped++;
        fprintf(stderr, "SKIP: Noto Sans CJK not installed, CJK positive coverage untested\n");
    }
}

static void test_catalog_probe_follows_directive(void) {
    /* A catalog that asks for a family with no CJK coverage must fail the
     * probe, and the reported offender must be the CJK sample - not the ASCII
     * in the same value. */
    const char *body =
        "@font = No Such Family 12345\n"
        "@monofont = No Such Family 12345\n"
        "Engine room. = 引擎室。\n";
    CHECK(make_catalog("xx", body), "cannot write the test catalog");
    CHECK(!strcmp(i18n_set_language(NULL, "xx"), "xx"), "catalog did not load");
    CHECK(i18n_font() && !strcmp(i18n_font(), BOGUS_FAMILY),
          "the directive was not read back: %s", i18n_font() ? i18n_font() : "(null)");

    char first_bad[8] = "";
    int missing = cd_font_probe_catalog("DejaVu Sans", first_bad, sizeof first_bad);
    CHECK(missing > 0, "a catalog whose family lacks its glyphs must fail the probe");
    CHECK(!strcmp(first_bad, CJK_SAMPLE), "first offender must be U+5F15, got \"%s\"", first_bad);

    /* Same text, a family that does have the glyphs: the probe must pass.
     * DejaVu has no CJK, so use the CJK family when it is installed. */
    if (have_family("Noto Sans CJK SC")) {
        const char *good =
            "@font = Noto Sans CJK SC\n"
            "Engine room. = 引擎室。\n";
        CHECK(make_catalog("yy", good), "cannot write the second test catalog");
        CHECK(!strcmp(i18n_set_language(NULL, "yy"), "yy"), "second catalog did not load");
        CHECK(strcmp(i18n_language(), "en") != 0, "must not have fallen back to English");
        char bad2[8] = "";
        int miss2 = cd_font_probe_catalog("DejaVu Sans", bad2, sizeof bad2);
        CHECK(miss2 == 0, "a CJK catalog with a CJK family must pass the probe (%d missing)", miss2);
    } else {
        skipped++;
    }
    drop_catalog();
}

static void test_english_short_circuits(void) {
    /* English must not spend any rendering work, and must never be "missing". */
    i18n_set_language(NULL, "en");
    char first_bad[8] = "x";
    CHECK(cd_font_probe_catalog("DejaVu Sans", first_bad, sizeof first_bad) == 0,
          "English must never fail the probe");
    CHECK(first_bad[0] == 'x', "English must not touch the offender buffer");
}

int main(void) {
    test_coverage_basics();
    test_cjk_direction();
    test_catalog_probe_follows_directive();
    test_english_short_circuits();
    if (failures) {
        fprintf(stderr, "test-fontprobe: %d/%d checks FAILED (%d skipped)\n", failures, checks, skipped);
        return 1;
    }
    printf("test-fontprobe: %d checks passed (%d skipped)\n", checks, skipped);
    return 0;
}
