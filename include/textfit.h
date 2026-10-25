/* Width-aware text fitting for the bundled cairo clients.
 *
 * The compositor chrome gets a fixed pixel budget per slot, and translations
 * change length: Russian is routinely 20-40% wider than English, and German
 * compounds are worse. Drawing a translated string into a fixed slot without
 * measuring it either spills into the neighbouring column or gets clipped by a
 * cairo clip rectangle, and clipping is how you lose the end of "Сбросить
 * выбранную запись" with no visual hint that anything is missing.
 *
 * The policy here is shrink first, then ellipsize, never clip:
 *   1. if the text fits at 100%, draw it untouched;
 *   2. otherwise scale the font down until it fits, but no smaller than
 *      min_scale (legibility floor);
 *   3. if it still does not fit at the floor, cut whole code points off the
 *      end and append U+2026, still at the floor size.
 *
 * This file is deliberately free of any cairo or locale dependency: the caller
 * supplies the measurement callback, so the fitting logic is unit-testable
 * with a synthetic metric (tests/test_textfit.c) and can be reused by any
 * future text backend.
 */
#ifndef CD_TEXTFIT_H
#define CD_TEXTFIT_H

#include <stdbool.h>
#include <stddef.h>

/* U+2026 HORIZONTAL ELLIPSIS. DejaVu Sans (the clients' only family) has it. */
#define CD_TEXT_ELLIPSIS "\xe2\x80\xa6"

/* Returns the advance width in device units for `s` rendered at `scale`
 * (1.0 = the caller's normal size). Must be monotonically non-decreasing in
 * both prefix length and scale. */
typedef double (*cd_measure_fn)(void *ctx, const char *s, double scale);

typedef struct {
    double scale;      /* 1.0 untouched, < 1.0 shrunk */
    bool ellipsized;   /* true when the tail was replaced by an ellipsis */
    bool overflow;     /* true when nothing could be drawn within maxw */
    double width;      /* measured width of the final string */
} CdFit;

/* Fits `src` into `maxw` and writes the result to `out` (always NUL
 * terminated, always cut on a UTF-8 boundary). `out_cap` should be at least
 * strlen(src) + 4 so the ellipsis can be substituted in place; if it is not,
 * the result is a plain truncation and CdFit.ellipsized is set.
 *
 * maxw <= 0 means "no constraint": the text is copied untouched. */
double cd_text_fit(char *out, size_t out_cap, const char *src, double maxw,
                   cd_measure_fn measure, void *ctx, double min_scale,
                   CdFit *result);

/* Codepoint-aware buffer helpers.
 *
 * Why these exist: snprintf(dst, n, "%s", translated) truncates at a BYTE
 * count, so a longer translation can be cut in the middle of a code point.
 * cairo validates UTF-8 on cairo_show_text()/cairo_text_extents() and latches
 * CAIRO_STATUS_INVALID_STRING for the rest of the context - every later draw
 * call in that frame is silently dropped. That is not a cosmetic bug: one
 * over-long translation blanks the whole window below it. */
size_t cd_utf8_prev_boundary(const char *s, size_t n);
/* Copies src into dst, cutting on a code point boundary, and never leaves a
 * partial sequence in dst even if src itself ends mid-sequence (which is what
 * a previous byte-truncating snprintf leaves behind). Returns strlen(dst). */
size_t cd_utf8_copy(char *dst, size_t dst_cap, const char *src);
/* Bounded formatted write with the same guarantee. `fmt` may be a translated
 * string; arguments are formatted normally. */
void cd_utf8_printf(char *dst, size_t dst_cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif
