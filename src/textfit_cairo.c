/* Cairo glue for textfit. See include/textfit_cairo.h. */
#include "textfit_cairo.h"
#include "textfit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    cairo_t *c;
    double size;
    bool bold;
    bool mono;
} MeasureCtx;

static double measure(void *ctx, const char *s, double scale) {
    MeasureCtx *m = ctx;
    cd_font(m->c, m->size * scale, m->bold, m->mono);
    cairo_text_extents_t e;
    cairo_text_extents(m->c, s, &e);
    return e.x_advance;
}
void cd_font(cairo_t *c, double size, bool bold, bool mono) {
    if (size < 1.0) size = 1.0; /* cairo_rejects non-positive sizes */
    cairo_select_font_face(c, mono ? "DejaVu Sans Mono" : "DejaVu Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(c, size);
}

double cd_text_width(cairo_t *c, double size, bool bold, bool mono, const char *s) {
    cd_font(c, size, bold, mono);
    cairo_text_extents_t e;
    cairo_text_extents(c, s, &e);
    return e.x_advance;
}

double cd_fit_measure(cairo_t *c, double size, bool bold, bool mono, double maxw,
                      double floor, const char *s, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return 1.0;
    if (!s) s = "";
    MeasureCtx ctx = { c, size, bold, mono };
    CdFit fit;
    return cd_text_fit(out, out_cap, s, maxw, measure, &ctx, floor, &fit);
}

double cd_draw_fit_ex(cairo_t *c, double x, double y, double size, bool bold,
                      bool mono, double maxw, CdAlign align, double floor,
                      const char *s, CdFit *fit_out) {
    if (!s) s = "";
    /* Room for the longest input the clients hand us (a wrapped preview line or
     * the exec command) plus the ellipsis. Truncating here is a last resort for
     * a pathological config value, not the normal path. */
    char buf[1024];
    MeasureCtx ctx = { c, size, bold, mono };
    CdFit fit;
    double scale = cd_text_fit(buf, sizeof(buf), s, maxw, measure, &ctx, floor, &fit);
    if (fit_out) *fit_out = fit;

    const char *notice = getenv("CHROMEDRIVE_TEXT_AUDIT");
    if (notice && *notice && (scale < 0.999 || fit.ellipsized)) {
        fprintf(stderr, "[text-audit] scale=%.2f ellipsis=%d w=%.1f max=%.1f \"%s\"\n",
                scale, fit.ellipsized ? 1 : 0, fit.width, maxw, buf);
    }

    cd_font(c, size * scale, bold, mono);
    double w = fit.width;
    double ox = x;
    if (align == CD_ALIGN_RIGHT) ox = x - w;
    else if (align == CD_ALIGN_CENTER) ox = x - w / 2;
    cairo_move_to(c, ox, y);
    cairo_show_text(c, buf);
    return scale;
}

double cd_draw_fit(cairo_t *c, double x, double y, double size, bool bold,
                   bool mono, double maxw, CdAlign align, double floor,
                   const char *s) {
    return cd_draw_fit_ex(c, x, y, size, bold, mono, maxw, align, floor, s, NULL);
}
