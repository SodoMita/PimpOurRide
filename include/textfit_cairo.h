/* Cairo glue for include/textfit.h, shared by clients/demo.c and
 * clients/config.c so both draw text with the same shrink-then-ellipsize
 * policy. Keeping it in one place is the point: the previous clients each had
 * a private font() helper and drew unbounded strings into fixed slots.
 *
 * Set CHROMEDRIVE_TEXT_AUDIT=1 to log every string that had to be shrunk or
 * ellipsized. That is how the overflow fix is verified from a script: the log
 * lists exactly which strings no longer fit their slot, so "it looks fine" is
 * replaced by a count. */
#ifndef CD_TEXTFIT_CAIRO_H
#define CD_TEXTFIT_CAIRO_H

#include "textfit.h"

#include <cairo/cairo.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CD_ALIGN_LEFT,
    CD_ALIGN_RIGHT,
    CD_ALIGN_CENTER
} CdAlign;

/* Select the clients' only two families. `size` is the nominal (unshrunk)
 * size in device units, `y` the baseline in every draw call below. */
void cd_font(cairo_t *c, double size, bool bold, bool mono);

/* Advance width of `s` at `size`, without drawing. */
double cd_text_width(cairo_t *c, double size, bool bold, bool mono, const char *s);

/* Fit without drawing: writes the string that would be drawn into `out` and
 * returns the scale it would be drawn at. Used by callers that lay out a box
 * around the text (the settings tab bar sizes its highlight from the label).
 * `out_cap` must be at least strlen(s) + 4. */
double cd_fit_measure(cairo_t *c, double size, bool bold, bool mono, double maxw,
                      double floor, const char *s, char *out, size_t out_cap);

/* Draw `s` fitted into `maxw` device units. `x` is the left edge, the right
 * edge, or the centre according to `align`. `floor` is the smallest scale to
 * shrink to before the text is ellipsized (1.0 disables shrinking).
 * Returns the scale actually used (1.0 = untouched). */
double cd_draw_fit(cairo_t *c, double x, double y, double size, bool bold,
                   bool mono, double maxw, CdAlign align, double floor,
                   const char *s);

/* Same, and reports what happened (scale, final width, whether the tail was
 * replaced). `fit_out` may be NULL. */
double cd_draw_fit_ex(cairo_t *c, double x, double y, double size, bool bold,
                      bool mono, double maxw, CdAlign align, double floor,
                      const char *s, CdFit *fit_out);

#endif
