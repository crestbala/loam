#!/usr/bin/env python3
"""Generate the PNG fixtures for the `image.loam` decoder tests.

Deliberately written by hand rather than with Pillow, for two reasons:

  1. The fixtures must use **stored (uncompressed) DEFLATE blocks**. `zlib` may
     choose fixed or dynamic Huffman even at level 0 depending on the input, and
     the Loam decoder covers the stored block case (the container, filters and
     buffers are the point; Huffman-coded DEFLATE is the remaining decoder
     work). Writing the blocks here makes that case exact and intentional.
  2. Chunk CRCs and the Adler-32 come from the standard library, so the fixtures
     are spec-valid rather than "valid enough for our decoder".

Outputs, next to this script:

  checker.png         64x64 RGB, 1px checkerboard, filter 0 (None)
  checker_sub.png     the same pixels, filter 1 (Sub)
  checker_up.png      the same pixels, filter 2 (Up)
  checker_avg.png     the same pixels, filter 3 (Average)
  checker_paeth.png   the same pixels, filter 4 (Paeth)
  grad.png            64x48 RGB horizontal gradient, filter 0
  alpha.png           8x8 RGBA: opaque half-transparent red | fully transparent
  bad_sig.png         a PNG with a corrupted signature (must be refused)

Run from anywhere:  python3 packages/loam/tests/images/make_test_png.py
"""
import os
import struct
import zlib

BPP = {2: 3, 6: 4}  # color type -> bytes per pixel


def stored_deflate(data):
    """DEFLATE with stored blocks only, wrapped in a zlib stream."""
    out = b"\x78\x01"  # CMF/FLG: deflate, 32K window, no dict, valid check bits
    i = 0
    n = len(data)
    while True:
        chunk = data[i:i + 65535]
        i += len(chunk)
        final = 1 if i >= n else 0
        out += bytes([final])            # BFINAL + BTYPE=00, byte aligned
        out += struct.pack("<HH", len(chunk), 0xFFFF ^ len(chunk))
        out += chunk
        if final:
            break
    out += struct.pack(">I", zlib.adler32(data) & 0xFFFFFFFF)
    return out


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def filter_rows(rows, ft):
    """Apply PNG filter `ft` to every row; returns the filtered scanline stream."""
    bpp = len(rows[0][0])
    out = bytearray()
    for y, row in enumerate(rows):
        raw = bytearray()
        for x, px in enumerate(row):
            for k in range(bpp):
                v = px[k]
                left = row[x - 1][k] if x > 0 else 0
                up = rows[y - 1][x][k] if y > 0 else 0
                ul = rows[y - 1][x - 1][k] if (y > 0 and x > 0) else 0
                if ft == 0:
                    f = 0
                elif ft == 1:
                    f = left
                elif ft == 2:
                    f = up
                elif ft == 3:
                    f = (left + up) // 2
                else:
                    f = paeth(left, up, ul)
                raw.append((v - f) & 0xFF)
        out.append(ft)
        out += raw
    return bytes(out)


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def png(w, h, ct, rows, ft=0, sig=b"\x89PNG\r\n\x1a\n"):
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0)
    raw = filter_rows(rows, ft)
    return (sig + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", stored_deflate(raw)) +
            chunk(b"IEND", b""))


def checker(w, h, ct):
    """1px cells: the highest frequency an image can carry. Any filter that is
    not area-correct turns this into flat grey."""
    bpp = BPP[ct]
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            v = 255 if (x + y) % 2 == 0 else 0
            px = bytes([v, v, v] + ([255] if bpp == 4 else []))
            row.append(px)
        rows.append(row)
    return rows


def grad(w, h):
    rows = []
    for _ in range(h):
        row = []
        for x in range(w):
            v = x * 255 // (w - 1)
            row.append(bytes([v, v, v]))
        rows.append(row)
    return rows


def alpha_img(w, h):
    rows = []
    for _ in range(h):
        row = []
        for x in range(w):
            # Left half: red at 50% alpha. Right half: fully transparent green,
            # which must premultiply to nothing (a fringe test).
            row.append(bytes([255, 0, 0, 128]) if x < w // 2
                       else bytes([0, 255, 0, 0]))
        rows.append(row)
    return rows


def bmp(w, h, rows, ct=24, bottom_up=True, comp=0, palette=None):
    """An uncompressed BMP. `ct` is 24 (BGR) or 32 (BGRA), or 8 for a paletted
    image, which is how older icon assets are stored. For ct == 8 the first
    channel of each pixel is the palette index."""
    if ct == 8:
        bpp = 8
        entries = palette if palette else [(i, i, i) for i in range(256)]
        # RGBQUAD is stored blue, green, red, reserved.
        pal = bytes(v for (r, g, b) in entries for v in (b, g, r, 0))
    else:
        bpp, entries, pal = ct, [], b""
    row_bytes = ((w * bpp + 31) // 32) * 4
    # bpp is bits: divide before subtracting, or a 3-pixel 8-bit row gets a
    # negative pad and silently loses its alignment byte.
    pad = row_bytes - (w * bpp) // 8
    # A positive height means the file stores the BOTTOM row first; a negative
    # height means top-down.
    order = list(reversed(rows)) if bottom_up else rows
    body = bytearray()
    for row in order:
        if ct == 8:
            body += bytes(px[0] for px in row)
        elif ct == 24:
            for px in row:
                body += bytes([px[2], px[1], px[0]])
        else:
            for px in row:
                body += bytes([px[2], px[1], px[0], px[3]])
        body += b"\0" * pad
    height = h if bottom_up else -h
    dib = struct.pack("<IiiHHIIiiII", 40, w, height, 1, bpp, comp,
                      len(body), 2835, 2835, len(entries), 0)
    off = 14 + 40 + len(pal)
    header = b"BM" + struct.pack("<IHHI", off + len(body), 0, 0, off)
    return header + dib + pal + bytes(body)


def magic_files():
    """Tiny byte strings with each real container's magic, so the format sniffer
    can be tested without shipping a JPEG (and without pretending the Loam
    decoder handles one)."""
    return {
        "magic_jpeg.bin": b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01",
        "magic_gif.bin": b"GIF89a\x10\x00\x10\x00\x80\x00\x00",
        "magic_webp.bin": b"RIFF\x24\x00\x00\x00WEBPVP8 ",
        "magic_avif.bin": b"\x00\x00\x00\x20ftypavif\x00\x00\x00\x00",
        "magic_tiff.bin": b"II*\x00\x08\x00\x00\x00",
        "magic_ico.bin": b"\x00\x00\x01\x00\x01\x00\x10\x10",
        "magic_svg.txt": b"\n  <svg xmlns=\"http://www.w3.org/2000/svg\"></svg>",
        "magic_unknown.bin": b"hello world, this is not an image at all",
    }


def deflate_btype(z):
    """Which DEFLATE block type the stream starts with: 0 stored, 1 fixed, 2 dynamic."""
    b0 = z[2]  # first byte after the 2-byte zlib header
    return (b0 >> 1) & 3


def png_compressed(w, h, ct, rows, level=6, strategy=0):
    """A PNG whose IDAT is compressed by zlib itself, so the Loam inflater sees
    real fixed/dynamic Huffman blocks instead of our stored-block fixtures."""
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0)
    raw = filter_rows(rows, 0)
    co = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
    z = co.compress(raw) + co.flush()
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", z) + chunk(b"IEND", b""), deflate_btype(z))


def ramp(w, h):
    """v = (x*7 + y*13) % 256: no long matches, so the literal alphabet gets
    exercised across its whole range."""
    return [[bytes([(x * 7 + y * 13) % 256] * 3) for x in range(w)]
            for y in range(h)]


def runs(w, h):
    """16px runs alternating every 2 rows: long LZ77 matches at a distance of a
    whole row stride, which is what exercises the length and distance codes."""
    return [[bytes([255, 255, 255] if ((x // 16 + y // 2) % 2 == 0)
                   else [0, 0, 0]) for x in range(w)] for y in range(h)]


def png_from_raw(w, h, ct, raw, level=6):
    """IHDR for one size, scanlines for whatever the caller passes. Lets a
    fixture have valid CRCs and a valid zlib stream but the wrong content."""
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0)
    co = zlib.compressobj(level, zlib.DEFLATED, 15, 9, 0)
    z = co.compress(raw) + co.flush()
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", z) + chunk(b"IEND", b""))


def filter_rows_mixed(rows, pattern):
    """Apply a different filter per row, the way a real encoder picks one per
    scanline (there is no reason a file uses only one)."""
    bpp = len(rows[0][0])
    out = bytearray()
    for y, row in enumerate(rows):
        ft = pattern[y % len(pattern)]
        raw = bytearray()
        for x, px in enumerate(row):
            for k in range(bpp):
                v = px[k]
                left = row[x - 1][k] if x > 0 else 0
                up = rows[y - 1][x][k] if y > 0 else 0
                ul = rows[y - 1][x - 1][k] if (y > 0 and x > 0) else 0
                if ft == 0:
                    f = 0
                elif ft == 1:
                    f = left
                elif ft == 2:
                    f = up
                elif ft == 3:
                    f = (left + up) // 2
                else:
                    f = paeth(left, up, ul)
                raw.append((v - f) & 0xFF)
        out.append(ft)
        out += raw
    return bytes(out)


def png_multi_idat(w, h, ct, rows, pattern, chunk_size):
    """Dynamic Huffman, a filter per row, and the IDAT split across several
    chunks -- all three of which real encoders do and none of which a single
    hand-built fixture would cover."""
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0)
    raw = filter_rows_mixed(rows, pattern)
    co = zlib.compressobj(6, zlib.DEFLATED, 15, 9, 0)
    z = co.compress(raw) + co.flush()
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
    for i in range(0, len(z), chunk_size):
        out += chunk(b"IDAT", z[i:i + chunk_size])
    return out + chunk(b"IEND", b"")


def real_image(w, h):
    """Structured content: a gradient, a disc, and a block pattern. Real bytes
    rather than a synthetic constant, so every filter has something to do."""
    rows = []
    cx, cy, rad = w // 2, h // 2, h // 3
    for y in range(h):
        row = []
        for x in range(w):
            r = (x * 3 + y) % 256
            dx, dy = x - cx, y - cy
            g = 255 if dx * dx + dy * dy < rad * rad else (y % 32) * 8
            b = 255 if (x // 8 + y // 8) % 2 == 0 else 0
            row.append(bytes([r, g, b]))
        rows.append(row)
    return rows


def pack_samples(rows, depth, channels):
    """Pack samples MSB-first into filtered scanlines (filter 0 throughout).
    Several pixels share a byte at depths 1/2/4, and 16-bit samples are
    big-endian, which is what the shift-based packer gives us for free."""
    out = bytearray()
    for row in rows:
        line = bytearray()
        acc = 0
        nbits = 0
        for px in row:
            for s in px:
                acc = (acc << depth) | (s & ((1 << depth) - 1))
                nbits += depth
                while nbits >= 8:
                    nbits -= 8
                    line.append((acc >> nbits) & 0xFF)
                    acc &= (1 << nbits) - 1
        if nbits:
            line.append((acc << (8 - nbits)) & 0xFF)
        out.append(0)
        out += line
    return bytes(out)


def png_ex(w, h, ct, depth, rows, palette=None, trns=None, gama=None,
           interlace=0, level=6):
    """A PNG with a chosen colour type, sample depth, and optional PLTE / tRNS /
    gAMA chunks -- the variants the RGB/RGBA path does not exercise."""
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    ihdr = struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, interlace)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
    if gama is not None:
        out += chunk(b"gAMA", struct.pack(">I", gama))
    if palette is not None:
        out += chunk(b"PLTE", bytes(v for e in palette for v in e))
    if trns is not None:
        out += chunk(b"tRNS", trns)
    co = zlib.compressobj(level, zlib.DEFLATED, 15, 9, 0)
    z = co.compress(pack_samples(rows, depth, channels)) + co.flush()
    return out + chunk(b"IDAT", z) + chunk(b"IEND", b"")


def main():
    out = os.path.dirname(os.path.abspath(__file__))
    rows = checker(64, 64, 2)
    rows32 = checker(64, 64, 6)
    files = {
        "checker.png": png(64, 64, 2, checker(64, 64, 2), 0),
        "checker_sub.png": png(64, 64, 2, checker(64, 64, 2), 1),
        "checker_up.png": png(64, 64, 2, checker(64, 64, 2), 2),
        "checker_avg.png": png(64, 64, 2, checker(64, 64, 2), 3),
        "checker_paeth.png": png(64, 64, 2, checker(64, 64, 2), 4),
        "grad.png": png(64, 48, 2, grad(64, 48), 0),
        "alpha.png": png(8, 8, 6, alpha_img(8, 8), 0),
        "bad_sig.png": png(4, 4, 2, checker(4, 4, 2), 0,
                           sig=b"\x89PNG\r\n\x1a\x0b"),
        "checker24.bmp": bmp(64, 64, rows, 24),
        "checker24_td.bmp": bmp(64, 64, rows, 24, bottom_up=False),
        "checker32.bmp": bmp(64, 64, rows32, 32),
        "grad8.bmp": bmp(64, 48, grad(64, 48), 8),
        "colors24.bmp": bmp(4, 1, [[bytes([255, 0, 0, 255]), bytes([0, 255, 0, 255]),
                                    bytes([0, 0, 255, 255]), bytes([255, 255, 255, 255])]], 24),
        "colors8.bmp": bmp(3, 1, [[bytes([0, 0, 0, 255]), bytes([1, 0, 0, 255]),
                                   bytes([2, 0, 0, 255])]], 8,
                           palette=[(255, 0, 0), (0, 255, 0), (0, 0, 255)]),
        # Compression 1 (RLE8): a real BMP we deliberately do not decode, so the
        # refusal path and the platform fallback are both exercised.
        "rle8.bmp": bmp(64, 64, rows, 24, comp=1),
    }
    files.update(magic_files())
    # Real Huffman-coded PNGs. Z_FIXED forces block type 1; the default level
    # produces block type 2. Both are reported so the fixtures provably cover
    # both paths (a decoder that only handles one would still pass a single
    # compressed fixture).
    for name, (blob, bt) in {
        "huff_fixed.png": png_compressed(64, 64, 2, checker(64, 64, 2),
                                         level=6, strategy=zlib.Z_FIXED),
        "huff_dynamic.png": png_compressed(64, 64, 2, checker(64, 64, 2)),
        "huff_literals.png": png_compressed(256, 64, 2, ramp(256, 64)),
        "huff_runs.png": png_compressed(256, 128, 2, runs(256, 128)),
    }.items():
        files[name] = blob
        kind = {0: "stored", 1: "fixed", 2: "dynamic"}.get(bt, f"btype {bt}")
        print(f"  {name}: deflate block type {bt} ({kind})")

    # Robustness fixtures: valid container, valid CRCs, valid zlib, wrong content.
    # bad_length declares 64x64 but carries 32x32 scanlines.
    files["bad_length.png"] = png_from_raw(64, 64, 2, filter_rows(checker(32, 32, 2), 0))
    # bomb declares 4x4 (52 inflated bytes) but carries 200000. A decoder that
    # inflates first and checks later allocates 200 KB here; the limit must stop
    # it before that.
    files["bomb.png"] = png_from_raw(4, 4, 2, b"\x00" * 200000)
    # A file shaped like a real encoder's output: dynamic Huffman, a different
    # filter per row, IDAT split into 500-byte chunks.
    files["real.png"] = png_multi_idat(200, 150, 2, real_image(200, 150),
                                       [0, 1, 2, 3, 4], 500)

    # PNG variants: the sample formats and chunks the RGB/RGBA path never sees.
    pal4 = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]
    files["gray8.png"] = png_ex(8, 4, 0, 8, [[(x * 36 % 256,) for x in range(8)]
                                             for _ in range(4)],
                                trns=struct.pack(">H", 36))
    files["gray1.png"] = png_ex(8, 2, 0, 1, [[(x % 2,) for x in range(8)]
                                             for _ in range(2)])
    files["gray4.png"] = png_ex(8, 2, 0, 4, [[(x % 16,) for x in range(8)]
                                             for _ in range(2)])
    files["ga8.png"] = png_ex(4, 1, 4, 8, [[(200, 255), (200, 128),
                                            (200, 0), (0, 255)]])
    files["rgb16.png"] = png_ex(3, 1, 2, 16, [[(0x1234, 0x5678, 0x9ABC),
                                              (0xFFFF, 0x0000, 0x0080),
                                              (0x0100, 0x0001, 0x00FF)]])
    files["rgb_trns.png"] = png_ex(3, 1, 2, 8, [[(10, 20, 30), (40, 50, 60),
                                                 (70, 80, 90)]],
                                   trns=struct.pack(">HHH", 40, 50, 60))
    files["pal8.png"] = png_ex(4, 2, 3, 8, [[(x % 4,) for x in range(4)]
                                            for _ in range(2)],
                               palette=pal4, trns=bytes([128]))
    files["pal4.png"] = png_ex(4, 2, 3, 4, [[(x % 4,) for x in range(4)]
                                            for _ in range(2)],
                               palette=pal4)
    files["pal1.png"] = png_ex(8, 1, 3, 1, [[(x % 2,) for x in range(8)]],
                               palette=[(0, 0, 0), (255, 255, 255)])
    files["gama_odd.png"] = png_ex(4, 1, 2, 8, [[(1, 2, 3), (4, 5, 6),
                                                (7, 8, 9), (10, 11, 12)]],
                                   gama=100000)
    # Adam7 is not implemented: the refusal must be explicit, not a wrong image.
    files["interlaced.png"] = png_ex(8, 8, 2, 8, [[(x, y, 0) for x in range(8)]
                                                  for y in range(8)],
                                     interlace=1)
    for name, data in files.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
        print(f"wrote {name} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
