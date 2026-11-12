#!/bin/sh
# Mune checks, from anywhere:
#
#   sh examples/zeus/mune/tests/run.sh
#
# Engine tests, the headless screen (both appearances), a real ffmpeg
# render driven through the screen, then the macOS app.
set -eu

APP=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$APP/../../.." && pwd)
cd "$ROOT"
LOAMC=./bin/loamc
[ -x "$LOAMC" ] || { echo "missing $LOAMC: run ./nob.sh in $ROOT first"; exit 1; }

export ZEUS_HEADLESS=1 MAYA_HEADLESS=1
OUT=$(mktemp -d "${TMPDIR:-/tmp}/mune-tests.XXXXXX")
trap 'rm -rf "$OUT"' EXIT

say() { printf '\n== %s\n' "$1"; }

quiet() {
    if ! "$@" >"$OUT/step.log" 2>&1; then
        cat "$OUT/step.log"
        echo "FAIL $*"
        exit 1
    fi
    tail -5 "$OUT/step.log"
}

build() {
    rm -f "$2"
    if ! "$LOAMC" "$1" -o "$2" >"$2.log" 2>&1; then
        cat "$2.log"
        echo "FAIL compile $1"
        exit 1
    fi
}

say "routes"
quiet ./bin/zeli routes "$APP"

say "engine"
quiet "$LOAMC" test "$APP/tests/engine.loam"

say "screen"
build "$APP/tests/routes.loam" "$OUT/routes"
"$OUT/routes"

say "session"
build "$APP/tests/session.loam" "$OUT/session"
"$OUT/session"

say "macos"
quiet ./bin/zeli build "$APP" --targets macos,web,server

printf '\nmune: all checks passed\n'
