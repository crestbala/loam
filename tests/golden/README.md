# tests/golden — visual regression baseline

The reference screen is `ref_scene.loam`. It covers every element the visual
baseline must: body text at 11/13/17 px, a 1px border, a rounded card with a
drop shadow, a vertical gradient, and an image op.

## What exists now (Phase 0)

`ref_scene.draw.txt` is the **structural** golden: the draw list the scene
produces at logical 1x, diffed by `run.sh`.

```
../../bin/loamc ref_scene.loam -o .tmp/ref_scene   # via run.sh
./run.sh
```

This pins paint order, geometry, and which primitive (fill / stroke / shadow /
grad / text / image) each element resolves to. It is what stops a later phase
from silently reordering paint or dropping a stroke.

## What does not exist yet, and why

`ref@2x.png` is the **pixel** golden: the same scene rasterized at the physical
backing scale (2.0 on retina) and diffed per pixel. It is required by Phase 5
and it **cannot be produced before Phase 4**, because:

* The constraints forbid using CoreGraphics, Canvas2D drawing calls, Metal, or
  any GPU/2D API for rendering. Building this PNG with any of them would create
  a golden the mandated rasterizer is then not allowed to match.
* The software rasterizer that must produce it is Phase 4 work and does not
  exist in the tree.

`capture.sh` is the hook Phase 4 completes. It fails loudly today rather than
writing a plausible-but-wrong image.

## When Phase 4 lands

1. `capture.sh` renders `ref_scene.loam` through the rasterizer's physical-pixel
   buffer (read the screen's backing scale; render at `size * scale`) and writes
   `ref@2x.png`.
2. `ZEUS_GOLDEN_CAPTURE=1 ./run.sh` then checks both halves.
3. Phase 5's checks are made against that file: even 11px spacing, equal text
   weight at both polarities, a 1px border on one crisp device row, no flat
   spots on corners, no gradient banding, no tile seams, and no scroll trails.
