#!/usr/bin/env python3
"""Generate the GIF fixtures for `image.loam`'s GIF decoder.

These are written by **Pillow**, not by hand, and that is the whole point: our
PNG/BMP fixtures were hand-built (so the container, filters and buffers could be
tested exactly), but a GIF fixture written by hand would share an implementation
with the decoder under test. Pillow is an independent, widely used GIF encoder,
so if our LZW and palette handling agree with it, they agree with the world.

Run from anywhere:  python3 packages/loam/tests/images/make_test_gif.py
Needs Pillow (`pip install pillow`).
"""
import os

from PIL import Image

W, H = 32, 24


def palette_entry(i):
    """The colour of palette slot `i`. Deliberately distinctive per index, so a
    decoder that misplaces a palette entry or an index cannot pass by luck."""
    return (i, (255 - i) % 256, (i * 3) % 256)


def index_at(x, y):
    return (x * 7 + y * 3) % 256


def main():
    out = os.path.dirname(os.path.abspath(__file__))

    # 1. Flat paletted image: a global colour table, no transparency.
    im = Image.new("P", (W, H))
    pal = []
    for i in range(256):
        pal += list(palette_entry(i))
    im.putpalette(pal)
    im.putdata([index_at(x, y) for y in range(H) for x in range(W)])
    im.save(os.path.join(out, "gif_flat.gif"), optimize=False)

    # 2. Transparency: a red square on a transparent field, which GIF stores as
    #    one "transparent index" rather than an alpha channel.
    im2 = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    for y in range(16):
        for x in range(16):
            if 4 <= x < 12 and 4 <= y < 12:
                im2.putpixel((x, y), (255, 0, 0, 255))
    im2.save(os.path.join(out, "gif_trans.gif"))

    # 3. Interlaced: rows stored in GIF's four passes.
    im3 = Image.new("RGB", (24, 20))
    for y in range(20):
        for x in range(24):
            im3.putpixel((x, y), (x * 10 % 256, y * 12 % 256, (x + y) % 256))
    im3.save(os.path.join(out, "gif_interlaced.gif"), interlace=True)

    # 4. An animation: the decoder takes the first frame, and that must be the
    #    first frame rather than, say, the last one written.
    frames = []
    for k, base in enumerate([(255, 0, 0), (0, 255, 0), (0, 0, 255)]):
        f = Image.new("RGB", (12, 12), base)
        f.putpixel((k, k), (0, 0, 0))
        frames.append(f)
    frames[0].save(os.path.join(out, "gif_anim.gif"), save_all=True,
                   append_images=frames[1:], duration=100, loop=0)

    for name in ["gif_flat.gif", "gif_trans.gif", "gif_interlaced.gif",
                 "gif_anim.gif"]:
        p = os.path.join(out, name)
        print(f"wrote {name} ({os.path.getsize(p)} bytes)")

    # Report what Pillow actually encoded, so the Loam test asserts against the
    # file rather than against our assumptions about it.
    for name in ["gif_flat.gif", "gif_trans.gif", "gif_interlaced.gif", "gif_anim.gif"]:
        im = Image.open(os.path.join(out, name))
        info = im.info
        print(f"  {name}: mode={im.mode} size={im.size} frames="
              f"{getattr(im, 'n_frames', 1)} transparency={info.get('transparency')} "
              f"interlace={info.get('interlace')}")
        # And dump the expected pixels for the decoder test, in EXACTLY the
        # form the decoder produces: premultiplied RGBA8, with a transparent
        # pixel's colour zeroed. Pillow does the LZW decode (the part worth
        # checking against an independent implementation); the composition
        # rule here is the one image.loam documents.
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
        print(f"    {name}.rgba ({len(px)} bytes)")


if __name__ == "__main__":
    main()
