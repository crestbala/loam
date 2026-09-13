#!/bin/sh
# Phase 10 benchmark: before = the pre-flip tree (git HEAD, `int`=i64), after =
# the current 32-bit default. Records arena size, layout time, native binary
# size, generated C size, and wasm binary size when a wasm32 clang is available
# (set YUGA_WASM_CC, e.g. /opt/homebrew/opt/llvm/bin/clang).
#
# `--int64-compat` is a source-migration bridge, not a second runtime: the C
# seam is 32-bit now, so the HEAD tree is the honest "before".
cd "$(dirname "$0")/../../../.."
export ZEUS_HEADLESS=1 MAYA_HEADLESS=1

YUGAC=./bin/yugac
APP_REL=packages/compiler/tests/bench/bench.yuga
OUT=packages/compiler/tests/tmp/bench
BASE="$OUT/baseline"
mkdir -p "$OUT"

size_of() {
    stat -f%z "$1" 2>/dev/null || stat -c%s "$1"
}

run_one() {
    label="$1"; yugac="$2"; app="$3"; outstem="$4"
    if ! $yugac $app -o "$outstem" >/dev/null 2>&1; then
        echo "bench: $label compile failed"; return 1
    fi
    $yugac --emit-c $app -o "$outstem.c" >/dev/null 2>&1 || true
    echo "== $label =="
    echo "native_bytes  $(size_of "$outstem")"
    if [ -f "$outstem.c" ]; then echo "gen_c_bytes   $(size_of "$outstem.c")"; fi
    "$outstem"
}

# Baseline: pristine HEAD, built in a scratch tree.
if [ ! -x "$BASE/bin/yugac" ]; then
    mkdir -p "$BASE"
    ( cd . && git archive HEAD ) | tar -x -C "$BASE"
    ( cd "$BASE" && make all >/dev/null 2>&1 ) || {
        echo "bench: baseline build failed"; exit 1; }
fi
mkdir -p "$BASE/$(dirname "$APP_REL")"
cp "$APP_REL" "$BASE/$APP_REL"
run_one "before (HEAD, int=i64)" "$BASE/bin/yugac" "$BASE/$APP_REL" "$OUT/bench_before"
run_one "after  (default, int=i32)" "$YUGAC" "$APP_REL" "$OUT/bench_after"

if [ -n "$YUGA_WASM_CC" ] && command -v "$YUGA_WASM_CC" >/dev/null 2>&1; then
    $YUGAC --target=wasm32 "$APP_REL" -o "$OUT/bench_after.wasm" >/dev/null 2>&1 &&
        echo "wasm_bytes_after $(size_of "$OUT/bench_after.wasm")"
    $BASE/bin/yugac --target=wasm32 "$BASE/$APP_REL" -o "$OUT/bench_before.wasm" >/dev/null 2>&1 &&
        echo "wasm_bytes_before $(size_of "$OUT/bench_before.wasm")"
else
    echo "wasm: skipped (set YUGA_WASM_CC to a wasm32 clang to measure)"
fi
