/*
 * i18n.h - zero-dependency runtime translation for ChromeDrive chrome.
 *
 * Scope. This translates the compositor's *own* user-facing strings (HUD,
 * Help panel, Launch Control slots, on-screen notices, the Garage3D
 * configurator). It has nothing to do with the locales of hosted Wayland
 * clients: a compositor is not responsible for their translations, and it
 * must not try to become one.
 *
 * Why the English string is the key. There is no separate key namespace:
 *
 *     text(c, 58, 130, 45, tr("Make yourself at home."), true);
 *
 * The English source text *is* the lookup key (the gettext model). That buys
 * three properties that matter for a project this size:
 *
 *   1. English needs no catalog and can never go missing. If no catalog is
 *      loaded, or a key is absent, tr() returns its argument. A broken or
 *      partially translated install degrades to English, never to blanks.
 *   2. Call sites stay readable - no invented identifiers to keep in sync
 *      with the drawn text.
 *   3. There is no generated header, no extraction tool, and no build step.
 *      The catalog is a plain UTF-8 text file a translator can edit.
 *
 * The trade-off, stated plainly: editing the English text silently drops its
 * translations until the catalog is updated. tests/test_i18n.c guards the
 * shipped catalog by checking that every key it defines is still referenced
 * from the sources, so drift shows up in `make test`, not in a screenshot.
 *
 * Catalog format - deliberately the same shape as cdconfig.h's INI because
 * the project already has one text format:
 *
 *     # comment (';' also starts one)
 *     Window width = Ширина окна
 *     Field of view = Угол обзора
 *
 * One `key = value` per line; the first '=' separates them. Leading/trailing
 * spaces and tabs are trimmed from both sides (English UI text never begins
 * or ends with a space, so this is safe and keeps catalogs readable). A
 * trailing CR is stripped, so CRLF catalogs work. Escapes: \n \t \\ ;
 * any other backslash sequence is kept verbatim. Files are UTF-8; a leading
 * BOM is skipped. Duplicate keys: last one wins (with a warning).
 *
 * A line whose first non-space character is '@' is a metadata directive, not
 * a translation:
 *
 *     @font = Noto Sans CJK SC
 *     @monofont = Noto Sans Mono CJK SC
 *
 * Both sides are trimmed like ordinary lines and the value is unescaped.
 * The last occurrence wins. Directives are cleared by i18n_free() and by
 * every catalog (re)load, so switching language cannot retain an old
 * family; a failed or missing load leaves both unset. Unknown names are
 * ignored with a warning, so a catalog may carry directives a binary does
 * not know about yet.
 *
 * Search order for <tag>.lang - first hit wins, no merging:
 *
 *     1. $CHROMEDRIVE_LOCALE_DIR          (explicit override; tests use this)
 *     2. $XDG_CONFIG_HOME/chromedrive/locale   (user's own overrides)
 *     3. $XDG_DATA_HOME/chromedrive/locale     (installed per-user)
 *     4. <root>/assets/locale                  (shipped with the install)
 *
 * Language selection: an explicit tag ("ru", "ru_RU.UTF-8") or "auto", which
 * reads LC_ALL, then LC_MESSAGES, then LANG in that order. "C", "POSIX" and
 * unset all mean English. A requested tag is normalized to lowercase and
 * tried as-is first, then reduced to its base language, so ru_RU finds
 * ru.lang.
 *
 * This header is standalone on purpose (no chrome.h, no GL, no cairo) so the
 * configurator client links src/i18n.c exactly like it links src/cdconfig.c.
 */
#ifndef I18N_H
#define I18N_H

#include <stdbool.h>
#include <stddef.h>

/* Resolve `requested` and load its catalog.
 *
 *   root      install root for <root>/assets/locale; NULL is allowed.
 *   requested "auto" (or NULL/empty) to follow the environment, else a tag.
 *
 * Always succeeds. Returns the language actually in effect - "en" when
 * nothing matched, including when a catalog was requested but missing (the
 * caller can distinguish that case with i18n_catalog_path()).
 */
const char *i18n_init(const char *root, const char *requested);

/* Same as i18n_init for an already-initialized module: re-resolves the
 * language and swaps the catalog in place. This is what the compositor's
 * config hot-reload calls when [ui] language changes. Returns the new tag. */
const char *i18n_set_language(const char *root, const char *requested);

/* Active language tag (lowercase, e.g. "en", "ru"). Never NULL. */
const char *i18n_language(void);

/* Path of the loaded catalog, or "" when running on built-in English. */
const char *i18n_catalog_path(void);

/* Translate an English source string. Never returns NULL, and returns
 * `english` unchanged when no catalog is loaded or the key is absent. */
const char *tr(const char *english);

/* Family the loaded catalog asks for, or NULL when it asks for nothing (the
 * caller then keeps its built-in default). The pointer is owned by the
 * module and valid until the next i18n_init()/i18n_set_language()/
 * i18n_free(). */
/* Presence marker: include/fontprobe.h used a weak-shim fallback while the
 * @font work and the coverage-probe work were being written in parallel
 * branches. Both are merged now, so this simply documents that the API below
 * exists - an out-of-tree user compiled against the old header can still
 * #ifdef on it. */
#define I18N_HAS_FONT_DIRECTIVE 1

const char *i18n_font(void);
const char *i18n_monofont(void);

/* Family to draw with: the catalog's @font / @monofont when it named one, else
 * the built-in DejaVu default. This is the ONLY place the default family names
 * live - a new drawing path calls this (or cd_font()/ui_font_family(), which
 * delegate here) instead of spelling "DejaVu Sans" again, because a hardcoded
 * family is exactly how a fully translated CJK screen renders as tofu. */
const char *i18n_font_family(bool mono);

/* Up to `max` DISTINCT non-ASCII code points taken from the loaded catalog's
 * VALUES (never its keys), in first-appearance order, each written as a
 * NUL-terminated UTF-8 sequence into out[i] (8 bytes each is enough).
 * Returns how many were written (0 when running on English or when the
 * catalog is pure ASCII). This is the sample the glyph probe tests, so it
 * must come from the text that will actually be drawn, not from a hardcoded
 * table that can drift away from the catalog. U+200B..U+200F and U+FE0F are
 * skipped (zero width / variation selectors): they draw no ink and would
 * confuse a coverage probe based on it. Malformed bytes are never reported. */
int i18n_sample_glyphs(char out[][8], int max);

/* UTF-8-safe byte truncation. Copies the longest prefix of `src` that is both
 * a whole number of UTF-8 codepoints and at most `max_bytes` bytes long, into
 * `dst` (`cap` bytes, always NUL-terminated). Returns bytes copied.
 *
 * Use this instead of a printf "%.Ns" precision or "%.*s" on any text that can
 * be a translation. Those count BYTES, so they cut multi-byte characters in
 * half and produce mojibake - already reachable with Russian, and certain with
 * CJK. Truncating a cache key is fine; truncating what the user reads is not. */
size_t i18n_utf8_truncate(char *dst, size_t cap, const char *src, size_t max_bytes);

/* Same, with a SIGNED byte budget.
 *
 * Call sites compute their budget (`sizeof(buf) - used`, `cap - prefix`), and
 * an int expression that goes negative converts to a huge size_t: the copy
 * loop then believes it has SIZE_MAX bytes of room and runs off the buffer.
 * That is an overrun that looks like a one-character bug. Anything negative
 * here means "no room" and writes an empty string. Reach for this whenever the
 * budget is computed rather than `sizeof`d.
 *
 * The size_t version above is hardened too: a `cap` or `max_bytes` above
 * PTRDIFF_MAX cannot describe a real object, so it is a converted negative
 * value; `cap` is then treated as 0 and `max_bytes` as "the destination is the
 * only limit". */
size_t i18n_utf8_truncate_n(char *dst, size_t cap, const char *src, long max_bytes);

/* Install root derived from the running binary (<root>/bin/<prog>): the same
 * two-directories-up rule src/main.c uses for assets, factored out so the
 * settings GUI and the demo clients can find <root>/assets/locale without
 * copying the logic. Writes "" and returns false if /proc/self/exe is
 * unavailable. */
bool i18n_root_from_exe(char *buf, size_t size);

/* Comma-separated list of languages available to a UI picker, English first:
 * "en,ru,de". Scans the four search directories above for *.lang. `root` is
 * the install root (NULL allowed). Returns the number of tags written, and
 * always writes a NUL-terminated string (at minimum "en"). */
size_t i18n_languages(char *buf, size_t size, const char *root);

/* Diagnostics for tests and for the startup log line. Counters are
 * cumulative since the last i18n_init(). */
void i18n_stats(int *entries, int *lookups, int *misses);

/* Untranslated-string audit. With CHROMEDRIVE_TEXT_AUDIT set in the
 * environment, every *prose* string that comes back untranslated - it contains
 * a space, or is 12 characters or longer - is printed once to stderr as
 *
 *     [text-audit] untranslated "Render to texture"
 *
 * Single short tokens ("COCKPIT", "auto", "windowed") are never reported: the
 * catalogs keep those in English on purpose. This is the only measurement that
 * finds a gap the source scan cannot see, because a label that arrives from a
 * table - tr(d->label) in the settings GUI - has no tr("...") call site to
 * count. tools/check-client-overflow.sh turns these lines into a build gate,
 * with assets/locale/untranslated.allow for the strings that stay English. */

/* Releases the catalog. Safe to call repeatedly; tr() keeps working
 * (it falls back to English). */
void i18n_free(void);

#endif /* I18N_H */
