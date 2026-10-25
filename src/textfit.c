/* Width-aware text fitting. See include/textfit.h for the policy.
 * No cairo, no locale, no allocation: the caller supplies the metric. */
#include "textfit.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CD_FIT_SHRINK_STEP 0.02
#define CD_FIT_MIN_SCALE 0.05
#define CD_FIT_MAX_SCALE 1.0

size_t cd_utf8_prev_boundary(const char *s, size_t n) {
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

/* Length of the longest prefix of s[0..n) that ends on a *complete* code point
 * boundary. Differs from cd_utf8_prev_boundary(): that one walks back from an
 * arbitrary offset, this one walks forward and drops a trailing sequence that
 * was itself cut short. */
static size_t complete_prefix(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char ch = (unsigned char)s[i];
        size_t need = ch < 0x80 ? 0 : (ch >> 5) == 0x06 ? 1 : (ch >> 4) == 0x0E ? 2 : (ch >> 3) == 0x1E ? 3 : 99;
        if (need == 99) return i;             /* not a lead byte we understand */
        if (i + need >= n) {
            /* Sequence runs past n: complete only if n == i + need (a short
             * buffer that cut it) - otherwise it is truncated. */
            return n == i + need + 1 ? n : i;
        }
        for (size_t k = 1; k <= need; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return i;
        i += need + 1;
    }
    return i;
}

size_t cd_utf8_copy(char *dst, size_t dst_cap, const char *src) {
    if (!dst || dst_cap == 0) return 0;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n > dst_cap - 1) n = dst_cap - 1;
    n = complete_prefix(src, n);
    memcpy(dst, src, n);
    dst[n] = 0;
    return n;
}

void cd_utf8_printf(char *dst, size_t dst_cap, const char *fmt, ...) {
    if (!dst || dst_cap == 0) return;
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt ? fmt : "", ap);
    va_end(ap);
    if (n < 0) { dst[0] = 0; return; }
    cd_utf8_copy(dst, dst_cap, tmp);
}

/* Byte offset of the boundary after `k` code points. */
static size_t boundary_of(const char *s, size_t k) {
    size_t i = 0;
    while (k-- > 0 && s[i]) {
        i++;
        while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    }
    return i;
}

static size_t codepoint_count(const char *s) {
    size_t n = 0;
    for (size_t i = 0; s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

/* Temporarily writes the ellipsis at `cut` and measures. Restores the buffer,
 * so the caller's copy of the text survives the probe. */
static bool probe_ellipsis(char *out, size_t cap, size_t cut, double maxw,
                           cd_measure_fn measure, void *ctx, double scale,
                           double *width_out) {
    if (cut + 4 > cap) return false; /* not enough room for "…" + NUL */
    char saved[4];
    memcpy(saved, out + cut, 4);
    memcpy(out + cut, CD_TEXT_ELLIPSIS, 3);
    out[cut + 3] = 0;
    double w = measure(ctx, out, scale);
    memcpy(out + cut, saved, 4);
    if (width_out) *width_out = w;
    return w <= maxw;
}

double cd_text_fit(char *out, size_t out_cap, const char *src, double maxw,
                   cd_measure_fn measure, void *ctx, double min_scale,
                   CdFit *result) {
    CdFit local = { 1.0, false, false, 0.0 };
    if (!result) result = &local;
    *result = local;

    if (!out || out_cap == 0) {
        result->overflow = true;
        return 1.0;
    }
    if (!src) src = "";

    size_t copied = cd_utf8_copy(out, out_cap, src);
    bool buffer_cut = copied < strlen(src);
    result->ellipsized = buffer_cut;

    if (min_scale > CD_FIT_MAX_SCALE) min_scale = CD_FIT_MAX_SCALE;
    if (min_scale < CD_FIT_MIN_SCALE) min_scale = CD_FIT_MIN_SCALE;

    double w = measure(ctx, out, 1.0);
    result->scale = 1.0;
    result->width = w;

    /* No budget, empty string, or it already fits: nothing to do. Zero-width
     * text (a blank field) must never be counted as "too wide". */
    if (maxw <= 0 || w <= 0.0 || w <= maxw) return 1.0;

    /* 1) Shrink. Advance width is proportional to the font size for the
     *    outline faces the clients use, so maxw/w is the exact answer; verify
     *    it because hinting and sub-pixel snapping are not perfectly linear. */
    double s = maxw / w;
    if (s > CD_FIT_MAX_SCALE) s = CD_FIT_MAX_SCALE;
    if (s < min_scale) s = min_scale;
    w = measure(ctx, out, s);
    while (w > maxw && s > min_scale) {
        s -= CD_FIT_SHRINK_STEP;
        if (s < min_scale) s = min_scale;
        w = measure(ctx, out, s);
    }
    if (w <= maxw) {
        result->scale = s;
        result->width = w;
        return s;
    }

    /* 2) Ellipsize at the legibility floor. Search the code-point prefix that
     *    leaves room for "…", so a wide-then-narrow mix (Russian + a Latin
     *    path) keeps as much as possible. */
    s = min_scale;
    size_t total = codepoint_count(out);
    size_t lo = 0, hi = total, best = 0;
    double best_w = 0.0;
    bool any = false;
    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;
        double pw = 0.0;
        if (probe_ellipsis(out, out_cap, boundary_of(out, mid), maxw, measure, ctx, s, &pw)) {
            any = true;
            best = mid;
            best_w = pw;
            if (mid == total) break;
            lo = mid + 1;
        } else {
            if (mid == 0) break;
            hi = mid - 1;
        }
    }

    size_t cut = boundary_of(out, best);
    if (cut + 4 > out_cap) {
        /* The buffer is too tight for prefix + "…": plain truncation. */
        result->scale = s;
        result->ellipsized = true;
        result->overflow = cut == 0;
        result->width = best_w;
        return s;
    }
    memcpy(out + cut, CD_TEXT_ELLIPSIS, 3);
    out[cut + 3] = 0;
    result->scale = s;
    result->ellipsized = true;
    result->overflow = !any && best == 0;
    result->width = best_w;
    return s;
}
