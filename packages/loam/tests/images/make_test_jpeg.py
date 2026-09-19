#!/usr/bin/env python3
"""Generate the JPEG fixtures for `image.loam`'s baseline JPEG decoder.

Written by **Pillow**, like the GIF and ICO fixtures and unlike the hand-built
PNG/BMP ones. A JPEG fixture written by hand would share an implementation with
the decoder under test; libjpeg is an independent encoder, so agreeing with it
means agreeing with the world.

JPEG is lossy, so the `.rgba` oracle is not exact: the Loam test compares with a
tolerance that accounts for the two IDCTs (libjpeg's and ours) differing in the
last bit or two. What the oracle pins is that our chroma upsampling, colour
transform, zigzag/dequant and block placement are the same *decision* as
libjpeg's, not that we reproduce its rounding.

Run from anywhere:  python3 packages/loam/tests/images/make_test_jpeg.py
Needs Pillow (`pip install pillow`).
"""
import os

from PIL import Image

W, H = 32, 24


def pattern(x, y):
    """A smooth-ish image with a few saturated areas. Smooth gradients exercise
    the IDCT's low frequencies; the saturated blocks exercise chroma, so a
    decoder that swapped Cb/Cr or lost the DC term cannot pass."""
    if 4 <= x < 12 and 4 <= y < 12:
        return (230, 40, 30)          # saturated red
    if 20 <= x < 28 and 4 <= y < 12:
        return (30, 200, 60)          # saturated green
    if 4 <= x < 28 and 16 <= y < 21:
        return (40, 60, 230)          # saturated blue
    return (x * 8 % 256, y * 10 % 256, (x * 4 + y * 6) % 256)


def make_rgb(subsampling):
    im = Image.new("RGB", (W, H))
    for y in range(H):
        for x in range(W):
            im.putpixel((x, y), pattern(x, y))
    return im


def rgba_oracle(im):
    """The decoder's output form: premultiplied RGBA8. JPEG is opaque, so this
    is just (r, g, b, 255) — but it is written through the same rule so the test
    compares like with like."""
    rgba = im.convert("RGBA")
    px = bytearray()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            r, g, b, a = rgba.getpixel((x, y))
            px += bytes([(r * a + 127) // 255, (g * a + 127) // 255,
                         (b * a + 127) // 255, a])
    return bytes(px)


def main():
    out = os.path.dirname(os.path.abspath(__file__))
    written = []

    # 1. 4:4:4 — no subsampling, so every component is full resolution.
    make_rgb(0).save(os.path.join(out, "jpg_444.jpg"), quality=92, subsampling=0)
    written.append("jpg_444.jpg")

    # 2. 4:2:2 — chroma halved horizontally.
    make_rgb(1).save(os.path.join(out, "jpg_422.jpg"), quality=92, subsampling=1)
    written.append("jpg_422.jpg")

    # 3. 4:2:0 — chroma halved both ways. The common case by far.
    make_rgb(2).save(os.path.join(out, "jpg_420.jpg"), quality=92, subsampling=2)
    written.append("jpg_420.jpg")

    # 4. Grayscale — one component, no colour transform at all.
    g = Image.new("L", (W, H))
    for y in range(H):
        for x in range(W):
            g.putpixel((x, y), (x * 8 + y * 10) % 256)
    g.save(os.path.join(out, "jpg_gray.jpg"), quality=92)
    written.append("jpg_gray.jpg")

    # 5. Progressive — a real format the in-Loam decoder does NOT cover, so the
    #    status must say "unsupported" and the platform path keeps loading it.
    make_rgb(2).save(os.path.join(out, "jpg_progressive.jpg"), quality=92,
                     progressive=True)
    written.append("jpg_progressive.jpg")

    # 6. Restart markers (DRI + RSTn). A separate fixture with no subsampling, so
    #    an MCU is one block per component and the interval lands mid-band: the
    #    decoder must consume each RSTn, discard the padding bits, and reset the
    #    DC predictors. A decoder that ignores restarts drifts after the first
    #    one and the whole image goes wrong, which is the point of testing it.
    big = Image.new("RGB", (64, 32))
    for y in range(32):
        for x in range(64):
            big.putpixel((x, y), (x * 4 % 256, y * 8 % 256, (x * 2 + y * 3) % 256))
    big.save(os.path.join(out, "jpg_restart.jpg"), quality=92, subsampling=0,
             restart_marker_blocks=4)
    written.append("jpg_restart.jpg")

    for name in written:
        p = os.path.join(out, name)
        print(f"wrote {name} ({os.path.getsize(p)} bytes)")

    # Report what Pillow actually encoded, so the Loam test asserts against the
    # file rather than against our assumptions about it.
    for name in written:
        im = Image.open(os.path.join(out, name))
        prog = im.info.get("progressive", 0)
        print(f"  {name}: mode={im.mode} size={im.size} "
              f"sampling={im.layer if hasattr(im, 'layer') else '?'} progressive={prog}")
        base = os.path.join(out, name)
        if prog:
            continue
        orc = base + ".rgba"
        with open(orc, "wb") as f:
            f.write(rgba_oracle(im))
        print(f"    {name}.rgba ({os.path.getsize(orc)} bytes)")


if __name__ == "__main__":
    main()
