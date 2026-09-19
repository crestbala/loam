#!/bin/sh
# Pixel-golden check at 2x: the reference screen rasterized by Zeus itself.
#
# This used to be the "Phase 5 will fill this in" stub, and it failed loudly
# because there was no code path in the tree that turned a node tree into pixels.
# There is one now: the Loam rasterizer plus the scene pass. `raster_scene.loam`
# builds the reference screen, rasterizes it at the physical backing scale (2x)
# into the buffer Zeus owns, and dumps a PPM — no CoreGraphics, no Canvas2D, no
# GPU anywhere in the chain, so the bytes in the golden are bytes this project
# produced.
#
# The diff is exact. A pixel golden is not a place for tolerance: if a byte
# moves, either the change was intended (regenerate the golden and say so in the
# commit) or it is a regression. The first differing pixel is reported so the
# failure is actionable rather than just "files differ".
set -e

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
LOAM=$ROOT/bin/loamc
WANT=$HERE/raster_scene.ppm
GOT=$HERE/.tmp/raster_scene.ppm
SRC=$HERE/raster_scene.loam

if [ ! -x "$LOAM" ]; then
  echo "golden: build the compiler first: make -C \"$ROOT\" bin/loamc" >&2
  exit 1
fi

mkdir -p "$HERE/.tmp"
cd "$ROOT"
ZEUS_HEADLESS=1 "$LOAM" "$SRC" -o "$HERE/.tmp/raster_scene" >/dev/null
ZEUS_HEADLESS=1 "$HERE/.tmp/raster_scene" "$GOT"

if [ "$1" = "--update" ]; then
  cp "$GOT" "$WANT"
  echo "golden: regenerated $WANT ($(wc -c < "$WANT" | tr -d ' ') bytes)"
  exit 0
fi

if cmp -s "$WANT" "$GOT"; then
  echo "ok   golden pixel @2x (raster_scene.ppm, $(wc -c < "$GOT" | tr -d ' ') bytes)"
  exit 0
fi

echo "FAIL golden pixel @2x (raster_scene.ppm differs)" >&2
if command -v python3 >/dev/null 2>&1; then
  python3 - "$WANT" "$GOT" <<'PY' >&2
import sys
def load(p):
    with open(p, 'rb') as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts = data.split(b'\n', 3)
    w, h = (int(v) for v in parts[1].split())
    return w, h, parts[3]
w, h, a = load(sys.argv[1])
w2, h2, b = load(sys.argv[2])
if (w, h) != (w2, h2):
    print(f"  size changed: {w}x{h} -> {w2}x{h2}")
    sys.exit(0)
n = w * h * 3
bad = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
print(f"  {len(bad)} of {n} channel bytes differ")
for i in bad[:8]:
    px = i // 3
    x, y = px % w, px // w
    c = "RGB"[i % 3]
    print(f"  first at ({x},{y}) {c}: want {a[i]} got {b[i]}")
PY
fi
echo "golden: if this change was intended, regenerate and say so in the commit:" >&2
echo "golden:   sh tests/golden/capture.sh --update" >&2
exit 1
