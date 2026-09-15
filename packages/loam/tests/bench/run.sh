#!/bin/sh
# Phase 10 benchmark: before = the pre-flip tree (git HEAD, `int`=i64), after =
# the current 32-bit default. Records arena size, layout time, native binary
# size, generated C size, and wasm binary size when a wasm32 clang is available
# (set LOAM_WASM_CC, e.g. /opt/homebrew/opt/llvm/bin/clang).
#
# `--int64-compat` is a source-migration bridge, not a second runtime: the C
# seam is 32-bit now, so the HEAD tree is the honest "before".
cd "$(dirname "$0")/../../../.."
export ZEUS_HEADLESS=1 MAYA_HEADLESS=1

LOAM=./bin/loam
APP_REL=packages/loam/tests/bench/bench.loam
OUT=packages/loam/tests/tmp/bench
BASE="$OUT/baseline"
# The baseline is whatever HEAD happens to be, and the language has lived at
# three paths across the restructure. Probe them newest-first.
BASE_APP_REL=packages/loam/tests/bench/bench.loam
for cand in packages/loam/tests/bench/bench.loam yuga/tests/bench/bench.loam; do
    if [ -f "$BASE/$cand" ]; then BASE_APP_REL=$cand; break; fi
done
mkdir -p "$OUT"

size_of() {
    stat -f%z "$1" 2>/dev/null || stat -c%s "$1"
}

run_one() {
    label="$1"; loam="$2"; app="$3"; outstem="$4"
    if ! $loam $app -o "$outstem" >/dev/null 2>&1; then
        echo "bench: $label compile failed"; return 1
    fi
    $loam --emit-c $app -o "$outstem.c" >/dev/null 2>&1 || true
    echo "== $label =="
    echo "native_bytes  $(size_of "$outstem")"
    if [ -f "$outstem.c" ]; then echo "gen_c_bytes   $(size_of "$outstem.c")"; fi
    "$outstem"
}

# Baseline: pristine HEAD, built in a scratch tree.
if [ ! -x "$BASE/bin/loam" ]; then
    mkdir -p "$BASE"
    ( cd . && git archive HEAD ) | tar -x -C "$BASE"
    ( cd "$BASE" && make all >/dev/null 2>&1 ) || {
        echo "bench: baseline build failed"; exit 1; }
fi
mkdir -p "$BASE/$(dirname "$BASE_APP_REL")"
# The HEAD std has no `__sizeof` diagnostics, so the baseline app omits them.
grep -v -E 'node_bytes|arena_bytes|draw_op_bytes|draw_bytes' "$APP_REL" > "$BASE/$BASE_APP_REL"
run_one "before (HEAD, int=i64)" "$BASE/bin/loam" "$BASE/$BASE_APP_REL" "$OUT/bench_before"
run_one "after  (default, int=i32)" "$LOAM" "$APP_REL" "$OUT/bench_after"

if [ -n "$LOAM_WASM_CC" ] && command -v "$LOAM_WASM_CC" >/dev/null 2>&1; then
    $LOAM --target=wasm32 "$APP_REL" -o "$OUT/bench_after.wasm" >/dev/null 2>&1 &&
        echo "wasm_bytes_after $(size_of "$OUT/bench_after.wasm")"
    $BASE/bin/loam --target=wasm32 "$BASE/$BASE_APP_REL" -o "$OUT/bench_before.wasm" >/dev/null 2>&1 &&
        echo "wasm_bytes_before $(size_of "$OUT/bench_before.wasm")"
else
    echo "wasm: skipped (set LOAM_WASM_CC to a wasm32 clang to measure)"
fi
