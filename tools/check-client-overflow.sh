#!/bin/sh
# Fail if any shipped language has to ellipsize client chrome.
#
# Both cairo clients (chromedrive-config and the four chromedrive-demo modes)
# draw into a fixed 1000x625 box. The shared policy in src/textfit.c shrinks a
# string to a legibility floor and only then cuts it, appending U+2026. A shrink
# is fine - it is the layout absorbing a longer translation. An ellipsis means
# the slot is too small for the language, which is a layout bug the translator
# cannot fix, so this script treats it as a failure.
#
# Every offending string is printed with its slot width, because "the label is
# cut off" is useless in a bug report and "Shirina kadrovogo bufera, 199 px"
# is actionable.
#
# The same runs also audit *untranslated* text: every string drawn through
# tr() is a lookup, so a prose string that comes back unchanged means the
# catalog has no entry for it and the UI is silently half in English. Strings
# that stay English by design (product names, unit tokens, shader keywords)
# are listed in assets/locale/untranslated.allow.
#
# Usage: tools/check-client-overflow.sh [--report] [LANG...]
#   --report  print findings but always exit 0 (this is how `make test` runs it)
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
bin="$root/bin"
report=0
if [ "${1:-}" = "--report" ]; then report=1; shift; fi

[ -x "$bin/chromedrive-config" ] || { echo "check-client-overflow: build first (make all)" >&2; exit 2; }
[ -x "$bin/chromedrive-demo" ] || { echo "check-client-overflow: build first (make all)" >&2; exit 2; }

allow="$root/assets/locale/untranslated.allow"
# Strings deliberately left in English. One per line, '#' starts a comment;
# a line matches a reported string when it is identical or a prefix of it.
allowed() {
    [ -f "$allow" ] || return 1
    while IFS= read -r want; do
        case "$want" in ''|'#'*) continue ;; esac
        case "$1" in "$want"*) return 0 ;; esac
    done <"$allow"
    return 1
}

if [ $# -gt 0 ]; then

    langs=$*
else
    langs=$(cd "$root/assets/locale" 2>/dev/null && ls *.lang 2>/dev/null | sed 's/\.lang$//')
    langs="en $langs"
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/cd-overflow.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT INT TERM

fail=0
for lang in $langs; do
    log="$tmp/$lang.log"
    # Settings GUI: three schema tabs plus the app picker, which has its own
    # tighter row layout (icon + name + check mark + app id).
    for tab in 0 1 2; do
        CHROMEDRIVE_LANG=$lang CHROMEDRIVE_TEXT_AUDIT=1 \
            "$bin/chromedrive-config" --screenshot "$tmp/$lang-$tab.webp" --tab $tab 2>>"$log"
    done
    CHROMEDRIVE_LANG=$lang CHROMEDRIVE_TEXT_AUDIT=1 \
        "$bin/chromedrive-config" --screenshot "$tmp/$lang-picker.webp" --tab 0 --slot 1 --choose-app 2>>"$log"
    # Demo clients, one offscreen render per mode - no compositor needed, so the
    # result does not depend on a window mount angle or the VNC surface size.
    for mode in files audio telemetry cluster; do
        CHROMEDRIVE_LANG=$lang CHROMEDRIVE_TEXT_AUDIT=1 \
            "$bin/chromedrive-demo" "$mode" --screenshot "$tmp/$lang-$mode.webp" 2>>"$log"
    done

    # The compositor's own chrome - HUD bar, dash tiles, Driver's Manual rows,
    # monitor titles - is drawn by bin/chromedrive, not by the clients, so it
    # needs its own pass. Headless and quiet: one second, no VNC mirror, no
    # screenshot, only the audit lines on stderr are wanted. This is the pass
    # that catches a help-panel row with no catalog entry, which is a gap the
    # client renders cannot see.
    CHROMEDRIVE_LANG=$lang CHROMEDRIVE_TEXT_AUDIT=1 \
        "$bin/chromedrive" --no-config --exit-after 1 2>>"$log" || true

    # Untranslated prose. English is the reference and is skipped: it *is* the
    # key, so every lookup legitimately returns its argument. Each render is a
    # separate process, so the same string is reported once per tab and mode -
    # sort -u collapses that before anything is counted.
    [ "$lang" = "en" ] && continue
    sed -n 's/.*\[text-audit\] untranslated "//p' "$log" | sed 's/"$//' | sort -u \
        >"$tmp/$lang.untranslated"
    while IFS= read -r text; do
        [ -n "$text" ] || continue
        allowed "$text" && continue
        printf '       [untranslated] %s\n' "$text"
        untranslated=$(( ${untranslated:-0} + 1 ))
    done <"$tmp/$lang.untranslated"

    cut=$(grep -c 'ellipsis=1' "$log" 2>/dev/null || true)
    shrunk=$(grep -c 'ellipsis=0' "$log" 2>/dev/null || true)
    if [ "${cut:-0}" -eq 0 ] && [ "${shrunk:-0}" -eq 0 ]; then
        printf '  %-4s ok        (no slot needed shrinking)\n' "$lang"
        continue
    fi
    printf '  %-4s %d shrunk, %d ellipsized\n' "$lang" "${shrunk:-0}" "${cut:-0}"
    grep 'ellipsis=1' "$log" 2>/dev/null | sed 's/^/       /'
    if [ "${cut:-0}" -gt 0 ]; then fail=1; fi

done

if [ "${untranslated:-0}" -gt 0 ]; then
    echo "check-client-overflow: $untranslated string(s) have no catalog entry" >&2
    echo "  translate them, or add them to assets/locale/untranslated.allow" >&2
    fail=1
fi

# The English source strings are the reference layout: if any of them has to be
# shrunk, a slot got smaller than the text it was designed for.
en_issues=$(grep -c 'text-audit' "$tmp/en.log" 2>/dev/null || true)
if [ "${en_issues:-0}" -gt 0 ]; then
    echo "check-client-overflow: EN baseline regressed - English text no longer fits:" >&2
    grep 'text-audit' "$tmp/en.log" >&2
    fail=1
fi

if [ "$fail" -ne 0 ] && [ "$report" -eq 0 ]; then
    echo "check-client-overflow: FAIL" >&2
    exit 1
fi
[ "$fail" -ne 0 ] && echo "check-client-overflow: report only (not failing)"
echo "check-client-overflow: done"
exit 0
