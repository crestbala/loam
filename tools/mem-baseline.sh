#!/bin/sh
# Phase 0 memory baseline for Zeus (`docs/mem-baseline.md`).
#
# Two halves:
#
#   1. Headless arena baseline — always runnable. Builds the release-shaped
#      membench and runs `empty`, `one`, `thousand`, capturing the
#      ZEUS_MEM report (arena buckets, draw list, process footprint).
#
#   2. GUI process baseline — needs a desktop session with a live window.
#      Launches membench natively, samples `vmmap --summary` and `footprint -p`,
#      splits DIRTY by region, and checks for IOAccelerator / Metal regions.
#      Skipped (loudly) when there is no GUI.
#
# Usage:
#   tools/mem-baseline.sh              # both halves, GUI auto-skipped
#   tools/mem-baseline.sh --headless   # only the headless half
#
# Run from anywhere; paths resolve relative to the repo root.
set -e

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
LOAM=$ROOT/bin/loamc
OUT=$ROOT/docs/mem
RELEASE=$ROOT/out/membench_release
SRC=$ROOT/examples/zeus/membench/membench.loam

MODE=both
[ "$1" = "--headless" ] && MODE=headless

if [ ! -x "$LOAM" ]; then
  echo "mem-baseline: build the compiler first: make -C \"$ROOT\" bin/loamc" >&2
  exit 1
fi

mkdir -p "$OUT"

# --- 1. headless arena baseline ---------------------------------------------
# Compile WITHOUT ZEUS_HEADLESS so this is the release-shaped native code path
# (-O1, Cocoa linked); run WITH it so no window resource is involved. The build
# flag bakes the gate in; the env var would also work.
echo "mem-baseline: building release membench" >&2
(cd "$ROOT" && ZEUS_MEM_STATS=1 "$LOAM" "$SRC" -o "$RELEASE") >/dev/null

: > "$OUT/headless.txt"
for m in empty one thousand; do
  {
    echo "=== $m ==="
    ZEUS_MEM_STATS=1 ZEUS_HEADLESS=1 "$RELEASE" "$m"
  } | tee -a "$OUT/headless.txt" >&2
done

if [ "$MODE" = headless ]; then
  echo "mem-baseline: headless results in $OUT/headless.txt" >&2
  exit 0
fi

# --- 2. GUI process baseline ------------------------------------------------
# Skip cleanly when there is no window server (CI, ssh, a sandbox with no
# display). A skipped run must never look like a pass.
if ! command -v vmmap >/dev/null 2>&1 || ! command -v footprint >/dev/null 2>&1; then
  echo "mem-baseline: vmmap/footprint missing — GUI half skipped" >&2
  exit 0
fi

echo "mem-baseline: launching a native window for GUI sampling" >&2
( cd "$ROOT" && ZEUS_MEM_STATS=1 ZEUS_MEM_DEBUG=1 "$RELEASE" thousand ) &
PID=$!
trap 'kill "$PID" 2>/dev/null || true' EXIT

# Give the window time to open and the first frames to settle.
sleep 4
if ! kill -0 "$PID" 2>/dev/null; then
  echo "mem-baseline: the app exited immediately — no GUI session; GUI half skipped" >&2
  exit 0
fi

vmmap --summary "$PID"                > "$OUT/vmmap-thousand.txt"    2>&1 || true
footprint -p "$PID"                   > "$OUT/footprint-thousand.txt" 2>&1 || true

# DIRTY column by region, top rows. `vmmap --summary` prints a table whose
# first columns are VIRTUAL RESIDENT DIRTY SWAP; keep the header and any line
# with a numeric DIRTY, which is every region we care about.
{
  echo "# vmmap --summary DIRTY by region (thousand)"
  echo
  grep -E '^(VIRTUAL|=====|MALLOC|TEXT|STACK|__LINKEDIT|__TEXT|shared memory|IOKit|CG |CoreGraphics|IOAccelerator|IOSurface|[A-Za-z_]+ *[0-9])' \
    "$OUT/vmmap-thousand.txt" 2>/dev/null || cat "$OUT/vmmap-thousand.txt"
  echo
  echo "# IOAccelerator / Metal check"
  if grep -Ei 'IOAccelerator|Metal' "$OUT/vmmap-thousand.txt" >/dev/null 2>&1; then
    echo "FAIL: a GPU region is present — a GPU path is being pulled in"
    grep -Ei 'IOAccelerator|Metal' "$OUT/vmmap-thousand.txt"
  else
    echo "ok: no IOAccelerator / Metal regions"
  fi
} > "$OUT/regions.md"

echo "mem-baseline: GUI results in $OUT/vmmap-thousand.txt, $OUT/footprint-thousand.txt, $OUT/regions.md" >&2
echo "mem-baseline: copy the DIRTY split into docs/mem-baseline.md §4" >&2
