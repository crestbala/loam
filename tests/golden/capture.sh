#!/bin/sh
# Pixel-golden capture for the reference screen: `ref@2x.png`.
#
# BLOCKED by design until Phase 4. The project requires Zeus to rasterize every
# pixel itself in Loam (docs/mem-baseline.md, task constraints), and that
# software rasterizer does not exist yet, so there is no code path in the tree
# that turns a node tree into pixels. Guessing one with CoreGraphics or Canvas2D
# would produce a golden that later phases are not allowed to match.
#
# Phase 4 fills this in: the rasterizer exposes a physical-pixel buffer for the
# snapshot window, this script reads the backing scale (2.0), renders at
# logical_size * scale, and writes the PNG here as `ref@2x.png`. The pixel diff
# then runs against that with the tolerance the task specifies (explain every
# perceptual difference; never accept one silently).
set -e

# The Phase 4 rasterizer is expected to expose a dump entry point here; until it
# does, this is the honest outcome: no pixels, no golden, loud failure.
echo "golden: pixel capture needs the Phase 4 software rasterizer (tests/golden/README.md)" >&2
exit 1
