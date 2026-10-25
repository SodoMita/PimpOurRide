/* Unit tests for the shared text-fitting policy (src/textfit.c).
 *
 * The metric is synthetic on purpose: width = code points * 10 * scale. That
 * makes the expected cut points exact, so this test proves the *policy* -
 * shrink first, ellipsize second, never split a code point, never exceed the
 * budget - independently of whatever font cairo happens to pick at runtime.
 * The cairo side is covered by the clients' CHROMEDRIVE_TEXT_AUDIT logs. */
#include "textfit.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "\n");                                           \
        }                                                                    \
    } while (0)

/* Monospace-ish synthetic metric: every code point is 10 units at scale 1. */
static double measure_cp(void *ctx, const char *s, double scale) {
    (void)ctx;
    size_t n = 0;
    for (size_t i = 0; s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return (double)n * 10.0 * scale;
}

/* Proportional metric: wide Latin/Cyrillic, narrow CJK - the case that makes a
 * naive "cut a fixed percentage" heuristic leave space on the table. */
static double measure_mixed(void *ctx, const char *s, double scale) {
    (void)ctx;
    double w = 0;
    for (size_t i = 0; s[i];) {
        unsigned char ch = (unsigned char)s[i];
        int len = ch < 0x80 ? 1 : (ch >> 5) == 0x6 ? 2 : (ch >> 4) == 0xE ? 3 : 4;
        if (ch >= 0xE0) w += 20.0;       /* 3-byte (CJK, most symbols) */
        else if (ch >= 0xC0) w += 8.0;   /* 2-byte (Latin-1, Cyrillic) */
        else w += 6.0;                   /* ASCII */
        i += (size_t)len;
    }
    return w * scale;
}

static bool valid_utf8_tail(const char *s) {
    /* A split code point shows up as a leading continuation byte. */
    for (size_t i = 0; s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) == 0x80 && i == 0) return false;
    return true;
}

static void test_fits_untouched(void) {
    char out[64];
    CdFit r;
    double s = cd_text_fit(out, sizeof(out), "Framebuffer width", 500, measure_cp, NULL, 0.8, &r);
    CHECK(s == 1.0, "fits at full size, got %.2f", s);
    CHECK(!r.ellipsized, "untouched text must not be marked ellipsized");
    CHECK(strcmp(out, "Framebuffer width") == 0, "text must be copied verbatim");
}

static void test_shrinks_before_ellipsizing(void) {
    char out[128];
    CdFit r;
    /* 20 code points = 200 units at 1.0; budget 170 => 0.85 is enough. */
    double s = cd_text_fit(out, sizeof(out), "ABCDEFGHIJKLMNOPQRST", 170, measure_cp, NULL, 0.8, &r);
    CHECK(s <= 0.86 && s >= 0.8, "expected shrink into [0.80,0.86], got %.3f", s);
    CHECK(!r.ellipsized, "shrinking alone should have sufficed");
    CHECK(measure_cp(NULL, out, s) <= 170.0, "shrunk text must fit");
    CHECK(strcmp(out, "ABCDEFGHIJKLMNOPQRST") == 0, "shrinking must not change the text");
}

static void test_ellipsizes_at_the_floor(void) {
    char out[128];
    CdFit r;
    /* 40 code points = 400 units; at the 0.8 floor that is 320 > 120 budget,
     * so it must ellipsize, and the result must still fit. */
    const char *long_text = "YYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY";
    double s = cd_text_fit(out, sizeof(out), long_text, 120, measure_cp, NULL, 0.8, &r);
    CHECK(s == 0.8, "ellipsizing happens at the floor, scale=%.3f", s);
    CHECK(r.ellipsized, "long string must be ellipsized");
    CHECK(measure_cp(NULL, out, s) <= 120.0, "ellipsized text must fit (%f)", measure_cp(NULL, out, s));
    CHECK(strstr(out, CD_TEXT_ELLIPSIS) != NULL, "ellipsis must be present: \"%s\"", out);
    CHECK(strlen(out) == 14 + 3, "120 budget at 0.8 => 15 cp, 14 + ellipsis: \"%s\"", out);
}

static void test_cyrillic_never_split(void) {
    char out[256];
    CdFit r;
    /* "Сбросить выбранную запись" is 24 code points / 46 bytes. Cutting by
     * bytes would leave a dangling continuation byte. */
    const char *ru = "Сбросить выбранную запись";
    for (double budget = 20; budget < 240; budget += 7) {
        cd_text_fit(out, sizeof(out), ru, budget, measure_cp, NULL, 0.8, &r);
        CHECK(valid_utf8_tail(out), "budget %.0f produced invalid UTF-8: %s", budget, out);
        CHECK(strlen(out) % 1 == 0, "sanity");
        /* The final string must be a prefix of the source (plus ellipsis). */
        size_t n = strlen(out) - (r.ellipsized ? 3 : 0);
        CHECK(strncmp(out, ru, n) == 0, "result is not a prefix of the source: %s", out);
    }
}

static void test_japanese_wide_glyphs(void) {
    char out[256];
    CdFit r;
    /* 3-byte code points at 20 units each: 30 chars = 600; budget 200 at 0.8
     * leaves 25 units => 1 glyph plus ellipsis. Proves the fitter agrees with
     * a metric where the "characters" are not bytes. */
    const char *ja = "起動パネル設定画面表示設定項目一覧操作説明書本体設定";
    cd_text_fit(out, sizeof(out), ja, 200, measure_mixed, NULL, 0.8, &r);
    CHECK(r.ellipsized, "wide text must be ellipsized");
    CHECK(measure_mixed(NULL, out, r.scale) <= 200.0, "must fit: %f", measure_mixed(NULL, out, r.scale));
    CHECK(valid_utf8_tail(out), "invalid UTF-8: %s", out);
    CHECK(strncmp(out, ja, strlen(out) - 3) == 0, "not a prefix: %s", out);
}

static void test_unconstrained_and_empty(void) {
    char out[32];
    CdFit r;
    double s = cd_text_fit(out, sizeof(out), "anything at all", 0, measure_cp, NULL, 0.8, &r);
    CHECK(s == 1.0 && !r.ellipsized, "maxw<=0 means no constraint");
    s = cd_text_fit(out, sizeof(out), "", 50, measure_cp, NULL, 0.8, &r);
    CHECK(s == 1.0 && !r.ellipsized && out[0] == 0, "empty string is not an overflow");
    s = cd_text_fit(out, sizeof(out), NULL, 50, measure_cp, NULL, 0.8, &r);
    CHECK(s == 1.0 && out[0] == 0, "NULL is treated as empty");
}

static void test_tiny_budget_does_not_wedge(void) {
    char out[64];
    CdFit r;
    /* A budget narrower than a single ellipsis cannot be satisfied; the
     * fitter must still terminate and leave a valid string. */
    double s = cd_text_fit(out, sizeof(out), "Настройки", 3, measure_cp, NULL, 0.8, &r);
    CHECK(s >= 0.8, "scale must stay at or above the floor");
    CHECK(valid_utf8_tail(out), "invalid UTF-8: %s", out);
    CHECK(r.overflow, "an unsatisfiable budget must be reported, not silently clipped");
}

static void test_floor_is_respected(void) {
    char out[128];
    CdFit r;
    /* floor 1.0 = shrink disabled: the text must jump straight to ellipsis. */
    cd_text_fit(out, sizeof(out), "AAAAAAAAAAAAAAAAAAAA", 40, measure_cp, NULL, 1.0, &r);
    CHECK(r.scale == 1.0 && r.ellipsized, "floor 1.0 must ellipsize without shrinking");
    /* floor 0.5 lets it shrink further than the default 0.8. */
    cd_text_fit(out, sizeof(out), "AAAAAAAAAAAAAAAAAAAA", 100, measure_cp, NULL, 0.5, &r);
    CHECK(r.scale <= 0.51 && !r.ellipsized, "floor 0.5 should allow shrink to 0.5, got %.2f", r.scale);
}

static void test_utf8_printf_helper(void) {
    char buf[14]; /* 13 bytes of room */
    /* 13 bytes cuts "абвгд" (10 bytes) plus "е" (2) = 12, ok; but 13 lands in
     * the middle of the next 2-byte sequence and must be dropped. */
    cd_utf8_printf(buf, sizeof(buf), "%s", "абвгдеж");
    CHECK(valid_utf8_tail(buf), "printf-helper left a partial code point: %s", buf);
    CHECK(strlen(buf) <= 13, "must respect the cap");
    /* A source that is ALREADY truncated (exactly what a byte-wise snprintf
     * leaves) must be sanitised, not passed through. */
    char bad[32], clean[32];
    snprintf(bad, sizeof(bad), "Настройк\xd0"); /* "Настройк" + first byte of "и" */
    cd_utf8_copy(clean, sizeof(clean), bad);
    CHECK(valid_utf8_tail(clean), "must drop a dangling lead byte: %s", clean);
    CHECK(strcmp(clean, "Настройк") == 0, "got \"%s\"", clean);
    /* And a plain over-long translated string into a small note buffer - the
     * real saved-note case - must never poison a cairo context. */
    char note[96];
    cd_utf8_printf(note, sizeof(note), "%s",
                   "Значения по умолчанию (файлы создаются при первой правке)");
    CHECK(valid_utf8_tail(note), "saved-note truncation left a partial code point");
    CHECK(strlen(note) % 1 == 0 && note[strlen(note) - 1] != (char)0xd0, "tail: %s", note);
}

static void test_utf8_copy_helper(void) {
    char buf[6];
    size_t n = cd_utf8_copy(buf, sizeof(buf), "абв"); /* 6 bytes + NUL */
    CHECK(n == 4 && strcmp(buf, "аб") == 0, "must cut on a boundary, got %zu \"%s\"", n, buf);
    CHECK(cd_utf8_prev_boundary("abc", 3) == 3, "ascii boundary");
    CHECK(cd_utf8_prev_boundary("аб", 1) == 0, "must skip continuation bytes");
    CHECK(cd_utf8_prev_boundary("аб", 2) == 2, "byte 2 starts a new code point");
}

int main(void) {
    test_fits_untouched();
    test_shrinks_before_ellipsizing();
    test_ellipsizes_at_the_floor();
    test_cyrillic_never_split();
    test_japanese_wide_glyphs();
    test_unconstrained_and_empty();
    test_tiny_budget_does_not_wedge();
    test_floor_is_respected();
    test_utf8_printf_helper();
    test_utf8_copy_helper();
    if (failures) {
        fprintf(stderr, "test-textfit: %d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test-textfit: %d checks passed\n", checks);
    return 0;
}
