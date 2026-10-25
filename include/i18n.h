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

/* UTF-8-safe byte truncation. Copies the longest prefix of `src` that is both
 * a whole number of UTF-8 codepoints and at most `max_bytes` bytes long, into
 * `dst` (`cap` bytes, always NUL-terminated). Returns bytes copied.
 *
 * Use this instead of a printf "%.Ns" precision or "%.*s" on any text that can
 * be a translation. Those count BYTES, so they cut multi-byte characters in
 * half and produce mojibake - already reachable with Russian, and certain with
 * CJK. Truncating a cache key is fine; truncating what the user reads is not. */
size_t i18n_utf8_truncate(char *dst, size_t cap, const char *src, size_t max_bytes);

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

/* Releases the catalog. Safe to call repeatedly; tr() keeps working
 * (it falls back to English). */
void i18n_free(void);

#endif /* I18N_H */
