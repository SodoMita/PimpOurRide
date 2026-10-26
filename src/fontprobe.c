/* Cairo-only font coverage probe. See include/fontprobe.h. */
#include "fontprobe.h"

#include <cairo/cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROBE_SIZE 64
#define PROBE_FONT_SIZE 32.0

/* U+F8FF, a private-use code point no sane UI font assigns: rendering it is
 * the toy API's "I don't have this glyph" case, i.e. the .notdef box. */
static const char REFERENCE_GLYPH[] = "\xef\xa3\xbf";

static unsigned char *render_alpha(const char *family, const char *utf8,
                                   size_t *out_len) {
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_A8, PROBE_SIZE, PROBE_SIZE);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }
    cairo_t *cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return NULL;
    }

    /* Clear to transparent first, so only the glyph's ink is compared. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_select_font_face(cr, family, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, PROBE_FONT_SIZE);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, utf8, &ext);
    double x = (PROBE_SIZE - ext.width) / 2.0 - ext.x_bearing;
    double y = (PROBE_SIZE - ext.height) / 2.0 - ext.y_bearing;
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, utf8);

    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return NULL;
    }
    cairo_surface_flush(surface);

    int stride = cairo_image_surface_get_stride(surface);
    int height = cairo_image_surface_get_height(surface);
    size_t len = (size_t)stride * (size_t)height;
    unsigned char *copy = malloc(len);
    if (copy) {
        memcpy(copy, cairo_image_surface_get_data(surface), len);
        *out_len = len;
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return copy;
}

bool cd_font_covers(const char *family, const char *utf8) {
    if (!utf8 || !*utf8) return true;
    if (!family || !*family) family = "DejaVu Sans";
    if (!strcmp(utf8, REFERENCE_GLYPH)) return true;

    size_t ref_len = 0, sample_len = 0;
    unsigned char *ref = render_alpha(family, REFERENCE_GLYPH, &ref_len);
    unsigned char *sample = render_alpha(family, utf8, &sample_len);

    /* A probe failure must not block a catalog: report coverage (true) when we
     * could not obtain both rasters. */
    bool covered = true;
    if (ref && sample && ref_len == sample_len &&
        memcmp(ref, sample, ref_len) == 0)
        covered = false;

    free(ref);
    free(sample);
    return covered;
}

int cd_font_probe_catalog(const char *default_family, char *first_bad,
                          size_t cap) {
    if (!strcmp(i18n_language(), "en")) return 0;

    const char *family = i18n_font();
    if (!family) family = default_family;

    /* The sample comes from the catalog that is already loaded (i18n keeps the
     * entry table in memory), so no search-path juggling is needed here: what
     * this probes is exactly what tr() will return. */
    char samples[12][8];
    int n = i18n_sample_glyphs(samples, 12);
    int missing = 0;
    bool have_first = false;
    for (int i = 0; i < n; i++) {
        if (cd_font_covers(family, samples[i])) continue;
        missing++;
        if (!have_first) {
            if (first_bad && cap) {
                snprintf(first_bad, cap, "%s", samples[i]);
            }
            have_first = true;
        }
    }
    return missing;
}

static unsigned long first_codepoint(const char *s, unsigned char bytes[4],
                                     int *byte_count) {
    const unsigned char *p = (const unsigned char *)s;
    int len = 1;
    unsigned long cp = p[0];
    if (p[0] < 0x80) {
        len = 1;
    } else if ((p[0] & 0xe0) == 0xc0) {
        len = 2;
        cp = p[0] & 0x1f;
    } else if ((p[0] & 0xf0) == 0xe0) {
        len = 3;
        cp = p[0] & 0x0f;
    } else if ((p[0] & 0xf8) == 0xf0) {
        len = 4;
        cp = p[0] & 0x07;
    } else {
        len = 1;
    }
    for (int i = 1; i < len; i++) {
        if (!p[i]) {
            len = i;
            break;
        }
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    for (int i = 0; i < len; i++) bytes[i] = p[i];
    *byte_count = len;
    return cp;
}

void cd_font_report_missing(const char *lang, const char *family, int count,
                            const char *first_bad) {
    if (!lang) lang = "?";
    if (!family) family = "DejaVu Sans";
    unsigned char bytes[4] = {0, 0, 0, 0};
    int nybbles = 1;
    unsigned long cp = 0;
    if (first_bad && *first_bad) {
        cp = first_codepoint(first_bad, bytes, &nybbles);
    }
    fprintf(stderr,
            "[font] WARNING: catalog '%s' cannot be rendered\n"
            "[font]   %d sample glyph(s) missing from family '%s'\n"
            "[font]   first missing code point: U+%04lX (UTF-8:",
            lang, count, family, cp);
    for (int i = 0; i < nybbles; i++) fprintf(stderr, " %02X", bytes[i]);
    fprintf(stderr, ")\n"
                    "[font]   falling back to English; install a font that "
                    "covers this catalog or fix its @font directive\n");
}
