# tests/golden — visual regression baseline

Two halves of the same reference screen:

| | what | how |
|---|---|---|
| **structural** | `ref_scene.draw.txt` — the draw list at logical 1x | `run.sh`, exact text diff |
| **pixel @2x** | `raster_scene.ppm` — the screen rasterized by Zeus at 2x | `capture.sh`, exact byte diff |

```
sh tests/golden/run.sh              # both halves
sh tests/golden/capture.sh --update # regenerate the pixel golden (say so in the commit)
ZEUS_SKIP_PIXEL_GOLDEN=1 sh tests/golden/run.sh   # structural only
```

The structural golden pins paint order, geometry, and which primitive (fill /
stroke / shadow / gradient / text / image) each element resolves to. It is what
stops a later phase from silently reordering paint or dropping a stroke.

The pixel golden is the stronger one: `raster_scene.loam` builds the reference
screen, rasterizes it through the Loam rasterizer into the buffer Zeus owns at
the physical backing scale, and dumps a PPM. **No graphics API is in that chain**
— no CoreGraphics, no Canvas2D, no Metal — so the bytes in the golden are bytes
this project produced. That was the point of the whole exercise, and it is why
this half is now a real check instead of the loud-failure stub it started as.

It is 160x120 logical at scale 2 → a 320x240 image. Small on purpose: a PPM is
uncompressed, and one that is a few hundred KB is reviewable in a commit. It
covers what the raster pass can do today:

* the surface fill;
* a **1px hairline border**, which lands on exactly two device rows (verified:
  rows 16 and 17 are `#cccccc` and nothing else) — quality rule 8;
* a rounded card with a 1px border, both on crisp device rows;
* a soft **drop shadow** grading from the card outward;
* a vertical **gradient** whose interpolation is done in linear space.

Make a copy of `raster_scene.ppm` into any viewer to look at it; `python3 -c
"from PIL import Image; Image.open('...').show()"` works, as does converting it
with `sips`.

## What is still missing, and why

Text and the image op are **absent on purpose**, and the program fails (exit 2)
if the pass ever counts an op it cannot rasterize while producing this screen —
so the gap is a number, not a quietly emptier picture.

* **Text** needs a font file. The tree ships only `tiny.ttf`, an 8-glyph fixture
  for the font and atlas tests; there is no usable UI font to render a body-text
  baseline with. The chain that would consume it (parse -> atlas -> blit) is
  landed and tested; what is missing is a real font to point it at.
* **The image op** carries a `src` string (a path or URL), and this program has
  no decode in it. The pass blits images the app has decoded and registered
  (`scene_register_image`); wiring an actual file into the golden is a small
  step, deliberately deferred so the golden does not depend on a fixture whose
  bytes could change.

## Notes for whoever regenerates this

* A pixel golden is not a place for tolerance. If a byte moves, either the change
  was intended (regenerate and say so) or it is a regression. `capture.sh` prints
  the first differing pixel as `(x,y) channel: want W got G`.
* The image depends on LAYOUT, so a layout change moves pixels. That is intended:
  this is the half that catches "the engine still paints the same list but it no
  longer looks the same".
* The rasterizer is deterministic integer and table maths, so the golden is
  reproducible on the platform that generated it. A different CPU architecture
  could in principle round one of the `f32` curve-flattening steps differently;
  if that ever happens, the diff will name the pixel and the fix is to decide
  whether the arithmetic should be integer, not to widen the comparison.
