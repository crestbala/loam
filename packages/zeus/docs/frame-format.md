# Zeus frame format (version 1)

Each paint, the engine hands the host **one frame**: a flat list of drawing
ops, plus a string table and a path stream. This buffer is the contract
between Zeus and a host. Drawing a frame means decoding these ops, in order,
into the platform's 2D API. Nothing else crosses the seam for paint.

The engine lowers everything that needs computing before the frame leaves it:
SVG parsing, text wrapping, number formatting and text opacity. So every op is
one primitive and a host contains no layout or parsing logic.

Decoders that exist today:

| Host | Decoder |
|---|---|
| C, Objective-C, JNI (macOS, iOS, Android, Linux) | `zeus_replay` in `packages/loam/runtime/zeus_plat.c`, into a `ZeusDraw` callback table |
| Browser | `decodeFrame` in `hosts/web/loader.js`, straight from wasm memory into Canvas2D |
| Tests | `runtime.trace_draw` (prints every call), and the Deno smoke test `hosts/web/wasm_smoke.ts` |

A new host can bind a `ZeusDraw` and let `zeus_replay` decode, or take the
buffer whole with `zeus_bind_frame(sink)` and decode it itself.

## Layout

All integers are little-endian `int32`.

**Header** (6 ints from Loam, `scene.frame_head`):

| index | field | value |
|---|---|---|
| 0 | magic | `0x5355455A` ("ZEUS") |
| 1 | version | `1` |
| 2 | stride | ints per op: `11` |
| 3 | n | op count |
| 4 | ns | string count |
| 5 | np | path stream length, in ints |

A decoder must check `version` and `stride`, and draw nothing for a frame it
does not understand. C hosts see the header as the `ZeusFrame` struct in
`zeus_rt.h`. The web host's JS gets 8 ints, `version stride n ops ns strs np
path`, where each pointer is an offset into wasm memory and `strs` is `ns`
(pointer, byte length) pairs.

**Ops:** `n` records of `stride` ints:

```
kind x y w h rgb radius alpha font value str
```

- Coordinates are layout points (the host scales to device pixels).
- `rgb` is `0xRRGGBB`; mask it with `0xFFFFFF`.
- `alpha` is 0..255.
- `str` indexes the string table (UTF-8, not NUL-terminated), or is -1.

**Path stream:** `np` ints that path ops point into. Commands are `0` move
`x y`, `1` line `x y`, `2` cubic `x1 y1 x2 y2 x y`, and `3` close.
Coordinates are in 1/64 of a layout point. The stream is shared: each path op
names its own `[start, start + len)`.

## Ops

| kind | op | fields | notes |
|---|---|---|---|
| 0 | fill | x y w h rgb radius | rounded when radius > 0; radius 999 is a pill |
| 1 | fill_a | + alpha | |
| 2 | text | x y str rgb font | `font` is the pixel size; y is the top of the line box |
| 6 | save | | |
| 7 | clip | x y w h radius | intersects; rounded when radius > 0 |
| 8 | restore | | pops one save, alpha or a save made by any other op |
| 9 | text_rot | x y str rgb font, w = degrees | clockwise about (x, y) |
| 10 | image | x y w h str radius alpha, value = fit | str is a path, URL or data URI; fit 0 stretch, 1 contain, 2 cover, 3 none |
| 11 | fill_g | x y w h, rgb to value, font = axis, radius alpha | linear gradient; axis 0 vertical, 1 horizontal |
| 12 | shadow | x y w h radius rgb alpha, font = blur, value = dx + 128 + 256 × (dy + 128) | skip when alpha, w or h ≤ 0 |
| 13 | stroke | x y w h rgb radius alpha, font = width | drawn inside the rect; skip when width, w or h ≤ 0 |
| 14 | fill4 | x y w h rgb alpha, radius = tl + 65536 × tr, value = br + 65536 × bl | per-corner radii |
| 15 | xform | x y = translate in 1/16 dp, radius = scale permille (1000 = none), font = degrees, w h = origin | applies to the current save level |
| 16 | path fill | rgb alpha, value = start, w = length | non-zero winding |
| 17 | path stroke | as 16, font = width (1/64 pt), radius = cap + 4 × join | caps 0 butt / 1 round / 2 square; joins 0 miter / 1 round / 2 bevel; skip when width ≤ 0 |
| 18 | alpha | alpha | save, then multiply opacity; closed by a `restore` |

Kinds 3 (number label), 4 (wrapped text) and 5 (SVG) appear only inside the
engine's draw list. `scene.present` lowers them to the ops above, so they never
reach a host. A decoder must ignore any kind it does not know.

## Changing the format

- Adding an op kind is compatible, because decoders skip unknown kinds. Add it
  to this table, to `zeus_replay`, to `decodeFrame` and to the trace host.
- Changing a field's meaning, the stride or the header bumps `version`.
- `tests/draw_golden/replay_trace` pins the exact call sequence `zeus_replay`
  makes. `wasm_smoke` decodes every frame the wasm build paints with the real
  `decodeFrame`.
