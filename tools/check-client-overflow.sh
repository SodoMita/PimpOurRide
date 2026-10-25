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
# Usage: tools/check-client-overflow.sh [--report] [LANG...]
#   --report  print findings but always exit 0 (this is how `make test` runs it)
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
bin="$root/bin"
report=0
if [ "${1:-}" = "--report" ]; then report=1; shift; fi

[ -x "$bin/chromedrive-config" ] || { echo "check-client-overflow: build first (make all)" >&2; exit 2; }
[ -x "$bin/chromedrive-demo" ] || { echo "check-client-overflow: build first (make all)" >&2; exit 2; }

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
