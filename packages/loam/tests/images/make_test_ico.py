#!/usr/bin/env python3
"""Generate the ICO fixtures for `image.loam`'s icon decoder.

Written by **Pillow**, like the GIF fixtures and for the same reason: an icon is
a container of a BMP-DIB or a PNG, so agreement with an independent encoder is
what the test is worth. Each fixture also gets a `.rgba` file holding Pillow's
own decode in the form the decoder produces (premultiplied, transparent pixels
zeroed).

Run from anywhere:  python3 packages/loam/tests/images/make_test_ico.py
Needs Pillow (`pip install pillow`).
"""
import os
import struct

from PIL import Image


def make_art(w, h, alpha=False):
    im = Image.new("RGBA" if alpha else "RGB", (w, h), (0, 0, 0, 0) if alpha else (0, 0, 0))
    for y in range(h):
        for x in range(w):
            r, g, b = (x * 8 % 256), (y * 8 % 256), ((x + y) * 4 % 256)
            # A transparent border ring, so the AND mask or the alpha channel has
            # something to do either way.
            inside = 2 <= x < w - 2 and 2 <= y < h - 2
            if alpha:
                im.putpixel((x, y), (r, g, b, 255) if inside else (0, 0, 0, 0))
            else:
                im.putpixel((x, y), (r, g, b) if inside else (255, 255, 255))
    return im


def dump(name, path, out):
    im = Image.open(path)
    im.load()
    rgba = im.convert("RGBA")
    px = bytearray()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            r, g, b, a = rgba.getpixel((x, y))
            if a == 0:
                r = g = b = 0
            px += bytes([(r * a + 127) // 255, (g * a + 127) // 255,
                         (b * a + 127) // 255, a])
    with open(os.path.join(out, name + ".rgba"), "wb") as f:
        f.write(px)
    # Which kind of entry did Pillow write?
    b = open(path, "rb").read()
    count = struct.unpack("<H", b[4:6])[0]
    kinds = []
    for i in range(count):
        e = 6 + 16 * i
        w = b[e] or 256
        h = b[e + 1] or 256
        off = struct.unpack("<I", b[e + 12:e + 16])[0]
        kind = "png" if b[off:off + 8] == b"\x89PNG\r\n\x1a\n" else "dib"
        kinds.append(f"{w}x{h}:{kind}")
    print(f"  {name}: size={im.size} entries=[{', '.join(kinds)}] "
          f"oracle={len(px)} bytes")


def main():
    out = os.path.dirname(os.path.abspath(__file__))

    # 1. A DIB entry (BMP-without-file-header, height doubled for the AND mask).
    #    Pillow writes PNG entries by default, so ask for BMP explicitly: a real
    #    icon editor's output is the DIB form for small sizes.
    make_art(32, 32).save(os.path.join(out, "icon_dib.ico"), sizes=[(32, 32)],
                          bitmap_format="bmp")
    # 2. An RGBA DIB icon: the alpha byte decides, not the mask.
    make_art(32, 32, alpha=True).save(os.path.join(out, "icon_alpha.ico"),
                                      sizes=[(32, 32)], bitmap_format="bmp")
    # 3. Several sizes in one file, which is what a real app icon looks like.
    make_art(48, 48, alpha=True).save(os.path.join(out, "icon_multi.ico"),
                                      sizes=[(16, 16), (32, 32), (48, 48)])
    # 4. A 256x256 entry, which ICO stores as an embedded PNG.
    make_art(256, 256, alpha=True).save(os.path.join(out, "icon_png.ico"),
                                        sizes=[(256, 256)])

    for name in ["icon_dib.ico", "icon_alpha.ico", "icon_multi.ico",
                 "icon_png.ico"]:
        print(f"wrote {name} ({os.path.getsize(os.path.join(out, name))} bytes)")
        dump(name, os.path.join(out, name), out)


if __name__ == "__main__":
    main()
