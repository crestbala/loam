#!/bin/sh
# Every vidy check, headless, ~seconds. Run from anywhere:
#
#   sh examples/zeus/vidy/tests/run.sh
#
#   1. std:str in-language tests (vidy's string helpers)
#   2. unit tests for lib/ (time labels, aspect boxes, names, ffmpeg job)
#   3. the e2e: real screen, keys and clicks, null host player
#   4. the resize probe: the stage follows the window and nothing overflows
#   5. the macOS and web builds
set -eu

APP=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$APP/../../.." && pwd)
cd "$ROOT"
LOAMC=./bin/loamc
[ -x "$LOAMC" ] || { echo "missing $LOAMC: run make in $ROOT first"; exit 1; }

export ZEUS_HEADLESS=1 MAYA_HEADLESS=1
OUT=$(mktemp -d "${TMPDIR:-/tmp}/vidy-tests.XXXXXX")
trap 'rm -rf "$OUT"' EXIT

say() { printf '\n== %s\n' "$1"; }

# Run a command quietly; on failure show its whole output and stop.
quiet() {
    if ! "$@" >"$OUT/step.log" 2>&1; then
        cat "$OUT/step.log"
        echo "FAIL $*"
        exit 1
    fi
    tail -1 "$OUT/step.log"
}

# A failed compile keeps no binary, so a stale one can never pass for new code.
build() {
    rm -f "$2"
    if ! "$LOAMC" "$1" -o "$2" >"$2.log" 2>&1; then
        cat "$2.log"
        echo "FAIL compile $1"
        exit 1
    fi
}

say "std:str"
quiet "$LOAMC" test packages/loam/tests/inlang/str_tests.loam

say "unit (lib/)"
quiet "$LOAMC" test "$APP/tests/unit.loam"

say "e2e"
build "$APP/tests/e2e.loam" "$OUT/e2e"
"$OUT/e2e"

say "resize probe"
build "$APP/tests/probe_resize.loam" "$OUT/resize"
"$OUT/resize" >"$OUT/resize.txt"
cat "$OUT/resize.txt"
# window WxH ... scroll_max N right_edge E: no scrolling, nothing past the
# edge, and all five sizes reported.
awk '{ split($2, wh, "x"); if ($(NF-2) != 0 || $NF > wh[1]) { print "FAIL overflow: " $0; bad = 1 } }
     END { if (NR != 5) { print "FAIL resize probe printed " NR " of 5 sizes"; bad = 1 }; exit bad }' "$OUT/resize.txt"

say "builds"
quiet ./bin/zeli build "$APP" --targets macos,web

printf '\nvidy: all checks passed\n'
