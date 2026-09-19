#!/bin/sh
# Phase 0 golden runner for the reference screen.
#
# Checks the STRUCTURAL golden today: it builds `ref_scene.loam`, dumps its
# draw list, and diffs it against `ref_scene.draw.txt`. That pins paint order
# and geometry so no later phase moves them silently.
#
# The PIXEL golden (the same screen at 2x) is the Phase 4 capture; this script
# runs it too once `ZEUS_GOLDEN_CAPTURE=1` and a rasterizer dump path exist.
set -e

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
LOAM=$ROOT/bin/loamc

if [ ! -x "$LOAM" ]; then
  echo "golden: build the compiler first: make -C \"$ROOT\" bin/loamc" >&2
  exit 1
fi

OUT=$HERE/.tmp
mkdir -p "$OUT"

cd "$ROOT"
ZEUS_HEADLESS=1 "$LOAM" "$HERE/ref_scene.loam" -o "$OUT/ref_scene" >/dev/null
ZEUS_HEADLESS=1 "$OUT/ref_scene" > "$OUT/ref_scene.draw.txt"

if diff -u "$HERE/ref_scene.draw.txt" "$OUT/ref_scene.draw.txt"; then
  echo "ok   golden structural (ref_scene draw list)"
else
  echo "FAIL golden structural (ref_scene draw list)" >&2
  exit 1
fi

# The pixel half is headless too — the rasterizer IS our renderer, so nothing
# here needs a display. Run it always; it is the check that catches a visual
# regression rather than a structural one. Set ZEUS_SKIP_PIXEL_GOLDEN=1 to skip
# it while iterating on the structural half.
if [ -z "$ZEUS_SKIP_PIXEL_GOLDEN" ]; then
  "$HERE/capture.sh"
fi
