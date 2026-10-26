/*
 * fontprobe.h - cairo-only glyph coverage probe for non-European catalogs.
 *
 * The cairo toy API (cairo_select_font_face + cairo_show_text) exposes no
 * glyph-index query and does not fall back per glyph. A catalog can therefore
 * name a family that has no glyphs for its own text and the UI silently paints
 * a screen of .notdef boxes ("tofu"). cd_font_covers() detects that by
 * measuring ink, so startup can refuse such a catalog and stay on English.
 *
 * This module deliberately depends on cairo only: no GL, no Wayland, no
 * chrome.h. All three drawing programs link it.
 */
#ifndef CD_FONTPROBE_H
#define CD_FONTPROBE_H

#include <stdbool.h>
#include <stddef.h>

/* i18n_font()/i18n_monofont()/i18n_sample_glyphs() come from i18n.h:
 * include/i18n.h marks their presence with I18N_HAS_FONT_DIRECTIVE. The
 * parallel-compat shim that let this module build before that API existed was
 * removed when the two branches merged; the symbols are real now. */
#include "i18n.h"

/* True when `family` actually has a glyph for the single code point in
 * `utf8`. The toy API exposes no glyph-index query, so this measures ink: the
 * candidate is rendered into a tiny A8 surface and compared against a
 * deliberately unassigned code point (U+F8FF, private use). Identical pixels
 * means the font fell back to .notdef - the same tofu box - i.e. no coverage.
 * A NULL/empty family falls back to "DejaVu Sans"; allocation/rendering failure
 * (and an empty or U+F8FF sample) reports coverage rather than blocking a
 * language on a probe error. */
bool cd_font_covers(const char *family, const char *utf8);

/* Probe the loaded catalog: family = i18n_font() or the program's default,
 * samples = i18n_sample_glyphs(). Returns the number of samples with no
 * coverage (0 = good) and copies the first offender into first_bad. Returns 0
 * immediately when the active language is English. */
int cd_font_probe_catalog(const char *default_family, char *first_bad, size_t cap);

/* Print the loud startup diagnostic for a failed probe: language, family,
 * count and the first offender as U+XXXX plus its UTF-8 bytes. Shared by the
 * three startup call sites so the message and the UTF-8 decoding exist once.
 * Kept separate from cd_font_probe_catalog so the probe itself stays quiet and
 * testable; this is not part of the SPEC's required ABI. */
void cd_font_report_missing(const char *lang, const char *family, int count,
                            const char *first_bad);

#endif /* CD_FONTPROBE_H */
