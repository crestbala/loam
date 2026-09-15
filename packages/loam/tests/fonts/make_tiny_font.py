#!/usr/bin/env python3
"""Generate `tiny.ttf`, a minimal spec-valid TrueType font for `std:font` tests.

It is deliberately tiny and hand-built so the expected metrics are obvious:

  unitsPerEm = 1000, numGlyphs = 8
  gid:  0=.notdef 1='A' 2='B' 3='C' 4=' ' 5=U+00E9(é) 6=U+1F600(😀) 7=pad
  advance (design units): 500 600 600 600 250 500 1000 500
  hhea ascender/descender/lineGap = 800 / -200 / 200  (line height = 1.2em)

Two `cmap` subtables are emitted so both parsers are exercised:
  - format 4, platform (3,1), BMP; one segment uses `glyphIdArray` (idRangeOffset)
  - format 12, platform (3,10), full Unicode (wins in `cmap_best`)

Run from anywhere:  python3 packages/loam/tests/fonts/make_tiny_font.py
The output path is next to this script.
"""
import os
import struct

UPM = 1000
GLYPHS = 8
ADV = [500, 600, 600, 600, 250, 500, 1000, 500]
ASCENDER, DESCENDER, LINE_GAP = 800, -200, 200


def be16(v):
    return struct.pack(">H", v & 0xFFFF)


def bei16(v):
    return struct.pack(">h", v)


def be32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def cmap4():
    """Format 4 for BMP: segments (idDelta for space/é, glyphIdArray for A-C)."""
    segs = [
        (0x0020, 0x0020, 4 - 0x0020, None),        # space, idDelta
        (0x0041, 0x0043, 0, [1, 2, 3]),            # A-C, glyphIdArray
        (0x00E9, 0x00E9, 5 - 0x00E9, None),        # é, idDelta
        (0xFFFF, 0xFFFF, 1, None),                 # required sentinel
    ]
    seg_count = len(segs)
    glyph_array = [g for _, _, _, arr in segs if arr for g in arr]

    end = b"".join(be16(e) for _, e, _, _ in segs)
    start = b"".join(be16(st) for st, _, _, _ in segs)
    delta = b"".join(bei16(d) for _, _, d, _ in segs)

    # idRangeOffset[i] = (address of glyphIdArray entry) - (address of entry i).
    ro = []
    gid_pos = 0
    for _, _, _, arr in segs:
        if arr is None:
            ro.append(0)
        else:
            # entry i's own address is `i*2`; glyph array base is `seg_count*2`.
            ro.append(seg_count * 2 + gid_pos * 2 - (len(ro)) * 2)
            gid_pos += len(arr)
    roblob = b"".join(be16(v) for v in ro)
    gidblob = b"".join(be16(g) for g in glyph_array)

    body = (be16(seg_count * 2) + be16(0) + be16(0) + be16(0) +
            end + be16(0) + start + delta + roblob + gidblob)
    length = 14 + len(body)
    return be16(4) + be16(length) + be16(0) + body


def cmap12():
    """Format 12: full Unicode, groups map a contiguous run to a glyph run."""
    groups = [
        (0x0020, 0x0020, 4),
        (0x0041, 0x0043, 1),
        (0x00E9, 0x00E9, 5),
        (0x1F600, 0x1F600, 6),
    ]
    body = b"".join(be32(a) + be32(b) + be32(g) for a, b, g in groups)
    length = 16 + len(body)
    return (be16(12) + be16(0) + be32(length) + be32(0) +
            be32(len(groups)) + body)


def cmap_table():
    sub4 = cmap4()
    sub12 = cmap12()
    header = be16(0) + be16(2)
    # offsets are from the start of the cmap table; records are 8 bytes each.
    recs_len = 4 + 2 * 8
    off4 = recs_len
    off12 = off4 + len(sub4)
    recs = (be16(3) + be16(1) + be32(off4) +
            be16(3) + be16(10) + be32(off12))
    return header + recs + sub4 + sub12


def head_table():
    # version, fontRevision, checkSumAdjustment, magic, flags, unitsPerEm,
    # created(8), modified(8), xMin yMin xMax yMax, macStyle, lowestRecPPEM,
    # fontDirectionHint, indexToLocFormat, glyphDataFormat
    return (be32(0x00010000) + be32(0x00010000) + be32(0) + be32(0x5F0F3CF5) +
            be16(0) + be16(UPM) + b"\0" * 16 +
            bei16(0) + bei16(-200) + bei16(1000) + bei16(800) +
            be16(0) + be16(8) + bei16(2) + bei16(0) + bei16(0))


def hhea_table():
    return (be32(0x00010000) + bei16(ASCENDER) + bei16(DESCENDER) +
            bei16(LINE_GAP) + be16(600) + bei16(0) + bei16(0) + bei16(1000) +
            bei16(1) + bei16(0) + bei16(0) + b"\0" * 8 +
            bei16(0) + be16(GLYPHS))


def maxp_table():
    return be32(0x00010000) + be16(GLYPHS) + b"\0" * 26


def hmtx_table():
    return b"".join(be16(ADV[g]) + bei16(0) for g in range(GLYPHS))


def build():
    tables = {
        "cmap": cmap_table(),
        "head": head_table(),
        "hhea": hhea_table(),
        "hmtx": hmtx_table(),
        "maxp": maxp_table(),
    }
    tags = sorted(tables)
    n = len(tags)
    search_range = 16 * (2 ** (n.bit_length() - 1))
    entry_selector = n.bit_length() - 1
    range_shift = n * 16 - search_range

    offset = 12 + n * 16
    records = b""
    blobs = b""
    for tag in tags:
        blob = tables[tag]
        records += tag.encode("ascii") + be32(0) + be32(offset) + be32(len(blob))
        blobs += blob + b"\0" * ((4 - len(blob) % 4) % 4)
        offset += len(blob) + ((4 - len(blob) % 4) % 4)

    header = (be32(0x00010000) + be16(n) + be16(search_range) +
              be16(entry_selector) + be16(range_shift))
    return header + records + blobs


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tiny.ttf")
    data = build()
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
