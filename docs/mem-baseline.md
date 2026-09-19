# Zeus memory baseline — Phase 0

Instrumentation and baseline only. **No rendering, layout, or allocation
behavior changed in this phase.** The numbers here are the measuring stick every
later phase is compared against.

Everything in this file was produced by commands in
[`tools/mem-baseline.sh`](../tools/mem-baseline.sh) unless marked *pending*.

## 1. What was added

| Piece | Where |
|---|---|
| Arena counters: `live_nodes`, `bytes_used` per bucket, `high_water_mark` | `packages/zeus/std/zeuscore/arena.loam` |
| On-exit report (arena + draw list + process footprint) | `packages/zeus/std/zeus.loam` (`mem_stats_report`, called from `raw_run`) |
| `ZEUS_MEM_STATS` gate | `platform.plat_mem_stats` — build flag `-DZEUS_MEM_STATS` (via `driver.c`) **or** `ZEUS_MEM_STATS=1` at runtime |
| `membench` app, three modes | `examples/zeus/membench/membench.loam` |
| Structural golden scene | `tests/golden/ref_scene.loam` |
| Frame damage list (Phase 1) | `packages/zeus/std/zeuscore/arena.loam`, `scene.loam` |
| Node-arena ceiling (Phase 2) | `packages/zeus/std/zeuscore/arena.loam` (`node_cap`) |
| `UiNode` field ordering (Phase 3) | `packages/zeus/std/zeuscore/arena.loam` |
| Framebuffer + color pipeline (Phase 4) | `packages/zeus/std/zeuscore/raster.loam` |

The gate is a no-op when off: `mem_stats_report` calls one host function that
returns 0, and `mem_sample()` (the only per-frame cost) is skipped entirely.
Every existing draw golden still passes unchanged (§5).

### Why "per bucket", not "per arena"

Phase 2 splits the arena into a persistent and a scratch arena. Phase 0 has
one growable store made of independent global vectors, so the report groups
them the way Phase 2 will: `tree` (nodes + child vectors), `signals`,
`interning` (rebuild-scope records, keyed-`For` index), `animation` (tracks +
layout anims), and `misc` (theme slots, focus chains, dirty lists, free lists).

## 2. Method

Release-shaped native binary (`-O1`, Cocoa linked), run headless so no window
resource is involved:

```
ZEUS_MEM_STATS=1 ./bin/loamc examples/zeus/membench/membench.loam -o out/membench_release
for m in empty one thousand; do
  ZEUS_MEM_STATS=1 ZEUS_HEADLESS=1 ./out/membench_release "$m"
done
```

`empty` is an empty root box, `one` adds one Ghost button, `thousand` adds
1000. Every mode is the same tree plus N buttons, so slopes are meaningful and
absolutes are not.

## 3. Results (macOS arm64, release-shaped headless)

| Metric | empty | one | thousand |
|---|---:|---:|---:|
| live nodes | 2 | 4 | 2002 |
| node slots (`nodes.len`) | 3 | 5 | 2003 |
| tree bytes | 1 468 | 2 452 | 985 468 |
| signals bytes | 4 | 4 | 4 |
| interning bytes | 0 | 0 | 0 |
| animation bytes | 20 480 | 20 480 | 20 480 |
| misc bytes | 728 | 728 | 8 720 |
| **arena total bytes** | **22 680** | **23 664** | **1 014 672** |
| peak bytes (high-water) | 22 680 | 23 668 | 1 018 672 |
| draw ops | 4 | 5 | 1004 |
| draw list bytes | 224 | 280 | 56 224 |
| `process_kb` (phys_footprint) | 1 904 | 3 777 | 4 817 |
| `__sizeof(UiNode)` | 488 | 488 | 488 |

`process_kb` varies a few percent run to run (ASLR / malloc arenas); the empty
value has been seen between 1 904 and 2 112 KB. The arena columns do not vary.

### Derived bytes per node (1 vs 1000 slope)

```
nodes/button   = (2002 - 4) / 1000                = 1.998
tree bytes/btn = (985468 - 2452) / 1000           = 983.0
bytes/node     = 983016 / 1998                    = 492.0
draw ops/btn   = (1004 - 5) / 1000                = 0.999
draw bytes/op  = 55944 / 999                      = 56.0
misc bytes/node= (8720 - 728) / 1998              = 4.0   (dirty-paint marks)
process KB/btn = (4817 - 3777) / 1000             = 1.04
```

**Headline: ~492 bytes/node.** A `UiNode` record is 488 bytes on its own —
99% of tree bytes are the record, child vectors are ~8 KB for 1000 nodes. This
is the number Phase 3 (struct-of-arrays, interned styles, interned strings,
narrowed handles) exists to move, and it is why the Phase 3 note in
`docs/loam_zeus_v2.md` §3.8 treats field layout as the real win.

Two fixed costs worth naming so later phases do not chase them:

* `animation = 20 480` on every mode: 512 `Track` slots (36 B) + 64 `LayoutAnim`
  slots (32 B), preallocated by `anim_ensure`. Constant, not a leak.
* `process_kb` jumps 1 904 → 3 777 KB between `empty` and `one`. Nothing in the
  arena moved (2 → 4 nodes); the first text measurement pulls in AppKit /
  CoreText font machinery. Our own arena is ~3% of the process at 1000 nodes.
  Phase 4's in-tree font stack is what makes this region disappear.

## 4. vmmap / footprint by region — *pending*

The task asks for `vmmap --summary <pid>` split by region (`MALLOC_*`, our
framebuffer, CG backing stores, CoreText/font caches, everything else) and
`footprint -p <pid>`, and a check that no `IOAccelerator` / Metal regions exist.

**This cannot be run in the current environment**: it needs a process with a
live window on a real Aqua session, and there is no display here. It is not
skipped silently — `tools/mem-baseline.sh` performs it and writes the raw dumps
the moment it runs on a desktop session. The placeholders that script fills are:

```
docs/mem/vmmap-thousand.txt     # vmmap --summary, DIRTY column by region
docs/mem/footprint-thousand.txt # footprint -p
docs/mem/regions.md             # the split table + the IOAccelerator check
```

Until that runs, treat the per-region split as **unmeasured**. `process_kb`
above is the only real process number this environment can produce.

## 5. Golden result

* Structural golden: **pass**, logical 1x, `tests/golden/ref_scene.draw.txt`.
  Covers body text at 11/13/17 px, a 1px border, a rounded card with a drop
  shadow, a gradient, and an image op. Runner: `tests/golden/run.sh`.
* Pixel golden at 2x: **pending, and blocked**. Producing pixels requires the
  Phase 4 software rasterizer; nothing in the tree today can rasterize a node
  tree. `tests/golden/README.md` states exactly what lands there and when.
* Regression check: all existing draw goldens pass unchanged
  (`golden_accordion`, `golden_controls`, `golden_overlays`,
  `golden_scale_gallery`, `golden_feedback`, `golden_menu`, `golden_picker`),
  as do `zeus_layout_golden`, `zeus_anim_proof`, `zeus_motion`, `zeus_scale`,
  `zeus_for_reorder`, `zeus_for_recycle`.

## 5.1 Scroll memory — measured growth (reported issue)

Reported: scrolling increases RAM. Reproduced headlessly, and the arena is not
where it shows — which is exactly why this needed the instrumentation.

**Reproduction.** A windowed `VirtualList` of 500 rows (`row_h = 40`) in a
300pt viewport, scrolled 500 frames at one row per frame:

| | value |
|---|---|
| arena bytes, settled | 55 632 B **flat** for the whole scroll |
| `node_slots` / `sigs` | 68 / 18, flat after the first shift |
| `phys_footprint` | 1 872 KB → 3 456 KB over 500 frames (**+1 584 KB**) |

The footprint grows roughly linearly with scroll distance (~3.2 KB/frame)
while the arena does not move. So it is not node/signal slot growth.

**What is not the cause.** Scrolling a *static* retained `App` tree is flat:
1 728 KB constant across 400 scroll frames. Scrolling is not the problem by
itself.

**What is.** A window shift makes `virt_refresh` call
`arena.rebuild_begin(nid)` and rebuild **every visible row**, not just the row
that entered or left. Each rebuild allocates fresh closures (`on_click`
handlers, `styled` effect thunks) and row-owned signals. The arena and the host
intern table reclaim their slots — the arena columns above are flat — but each
closure *value* the runtime allocates per build is not reclaimed. That is the
documented `docs/loam_zeus_v2.md` §1.3 behavior ("closures are … interned for
the process"), so the footprint scales with the number of rebuilds, i.e. with
how far the list is scrolled.

Corroborating probe on the per-frame `view` path: 60 buttons rebuilt each frame
grows 1 920 KB → 5 713 KB over 600 frames (~6.3 KB/frame); 60 plain boxes
plateau at 2 272 KB. Same mechanism, no scroll involved.

**Where the fix belongs.** Phase 1 / Phase 5: keyed row identity so a one-row
shift *moves* rows instead of dropping and rebuilding the window, plus §1.3
closure lifetime in Phase 14. This was deliberately **not patched in Phase 0**:
Phase 0 is behavior-neutral, and a partial patch here would have masked the
real cost without moving the measurement.

**Reproduce:**

```
ZEUS_HEADLESS=1 ./bin/loamc <probe>.loam -o out/probe && ZEUS_HEADLESS=1 ./out/probe
```

where the probe builds the windowed list, then loops
`zeus.engine_scroll_step(...)` + `zeus.pump_frame()`, printing
`zeus.mem_bytes()` and `zeus.memory_kb()`.

## 5.2 Frame damage list (Phase 1)

The scope of a repaint, as window-space rects. Phase 4 consumes it to rasterize
only the tiles it touches and blit only that region; until then it is verified
and measured on its own.

**Semantics** (`arena.loam` counters, `scene.loam` capture):

- Frame boundary: `damage_frame_begin` runs on the first of layout / paint in a
  frame, `damage_frame_end` after `present`. A layout-then-paint frame keeps
  the layout's full-damage mark across both.
- Structural changes — layout ran, resize, tree rebuild — call `damage_all`.
  A local repaint is only ever claimed for a change known to be paint-only.
- Paint-only: a node whose PAINT bit is set opens a capture, and it plus every
  node painted inside it contributes a rect. A dirty container must repaint its
  children (they composite over its new fill), so capture covers the subtree.
- Expansion is generous by construction: resolved shadow blur + offset, any
  paint-transform travel, half the growth of a scale above 100%, and 1px of AA
  bleed. Too small leaves a trail; too large only costs fill rate.
- Scroll marks the **scroller's** PAINT bit (`mark_scroll_node`), so the shifted,
  clipped content is damaged locally rather than needing a full fallback.
- Safety net: a dirty frame that recorded nothing (an unmarked global change)
  falls back to `damage_all`. Under-damaging is the one failure that shows.

**Verification:** `packages/loam/tests/compile_pass/zeus_damage.loam` — a
layout frame is full; a paint-only track frame is local with ≥1 rect; an
elevated card's rect is wider than its box (shadow expansion); a scroll is local
and non-empty.

## 6. Per-phase log

Appended after each phase: DIRTY-by-region, footprint, arena high-water,
bytes-per-node, and golden result. **If a phase does not move bytes-per-node,
say so** — do not claim a win. Never report a memory win that cost a golden
regression.

### Phase 0 — instrument and baseline

- bytes/node: **492.0** (this is the baseline the later phases must beat)
- arena high-water (1000 buttons): 1 018 672 B
- process footprint: 1 904 / 3 777 / 4 817 KB (empty / one / thousand)
- vmmap-by-region: *pending* (§4)
- golden: structural pass; pixel @2x *pending* (§5)
- **found and measured, not fixed:** scroll growth is real and scales with
  scroll distance, but it is runtime closure lifetime, not arena growth (§5.1).
  Phase 0 did not patch it (behavior-neutral phase); it is the first Phase 1/
  Phase 5 target.

### Phase 1 — frame damage list

- bytes/node: **492.0 — unchanged.** Phase 1 did not move the per-node cost and
  does not claim to. It is a capability phase.
- arena high-water (1000 buttons): 1 018 672 B (unchanged)
- process footprint: 1 888 / 3 649 / 4 913 KB (empty / one / thousand)
- damage cost: **zero steady-state bytes.** On a layout frame `damage_all`
  supersedes the per-node rects and capture is skipped, so `misc` and `total`
  are byte-identical to Phase 0. A paint-only frame allocates ≤16 B per damaged
  node; the buffer is popped, not dropped, so it does not reallocate frame to
  frame.
- golden: structural pass; all 11 draw goldens unchanged; pixel @2x still
  *pending* (§5).
- scope note: the rebuild anti-pattern Phase 1 targets was **already absent** —
  `drop_children` is private to `arena` and reached only through
  `rebuild_begin`, used solely by the `each`/`show`-class hosts
  (`kfor_refresh`, `virt_refresh`, `match_build`, `refit_refresh`); there is no
  `paint_acc` and no caller outside the core. Fine-grained reactivity already
  exists in `track.loam` (a write re-runs only the props that read it). The one
  missing Phase 1 piece was the damage list, which is what this phase adds.
- **The scroll leak from §5.1 is not fixed by this phase.** Damage makes the
  repaint correct and local; the footprint growth is closure lifetime and still
  needs keyed row identity (Phase 5) / §1.3 closure work (Phase 14).

### Phase 2 — arena discipline (scoped)

- bytes/node: **492.0 — unchanged.** No per-node cost was touched.
- arena high-water (1000 buttons): 1 018 672 B (unchanged)
- process footprint: 1 904 / 3 889 / 5 105 KB (empty / one / thousand)
- golden: structural pass; draw goldens unchanged; pixel @2x still *pending*.

**New invariant.** The node arena has a stated maximum (`node_cap`, default
65535 = the `u16` id maximum). `alloc_node` refuses to grow past it, prints
once, sets `node_overflow`, and returns the null handle (id 0) instead of
growing without bound. Non-fatal and covered by `zeus_node_cap.loam`.

**Already satisfied by the existing design** (verified, not rewritten):

- Item 2, free list for node ids: `free_nodes` is recycled by `alloc_node`;
  `zeus_for_recycle` / `zeus_for_leak` already pin it.
- Item 1's substance, scratch buffers that reset rather than realloc: the
  per-frame buffers already pop to zero and keep their capacity (`damage`,
  `dirty_paint`, `zf_*`), which is the realizable form of "reset to offset 0"
  here — Loam has no raw-pointer bump allocator (no deref sigil, by rule).
  `zeus_anim_proof` / `zeus_motion` already assert flat allocation counts.
- Item 4, one flat render command list: `scene.draws` is that single array.
  Culling it to the damage region is deferred to Phase 4: the list is
  *retained* and re-presented when nothing changed, so culling now would leave
  the next present incomplete. It belongs where the rasterizer owns the pass.

**Reported conflict — item 3 preallocation not forced.** At today's 488-byte
`UiNode`, preallocating the 65535-node `u16` maximum reserves ~32 MB in every
process, against a 30-60 MB total budget. A smaller fixed cap contradicts "the
stated maximum" and would silently reject legitimate apps. The invariant item 3
exists for — never silently grow, never realloc-double — is enforced by the cap;
the up-front reservation should follow the Phase 3 record shrink, at which
point the maximum can be reserved cheaply or the cap raised. This matches the
Phase 3-before-narrowing ordering `docs/loam_zeus_v2.md` §3.8 argues for.

### Phase 3 — data layout (partial: ordering landed; SoA / interning / id width deferred)

- bytes/node: **492.0 → 476.0 (−3.3%).** A real, measured reduction.
- `node_record_bytes`: **488 → 472.** Field census: 71 `int` (4 B), 25 `u16`,
  4 `i16`, 38 `u8`, 4 `i8`, 4 `string` (16 B), 1 slice (24 B) = payload 472.
  The old order interleaved narrow fields between wide ones and paid 16 bytes of
  alignment padding; grouped widest-first (16 → 4 → 2 → 1), **472 is the exact
  payload — no padding remains.** Ordering is therefore at its floor; it cannot
  be repeated for further gain.
- tree bytes at 1000 buttons: 985 468 → 953 420. Arena total 1 014 672 →
  982 624.
- process footprint: 1 888 / 3 969 / 5 057 KB (empty / one / thousand).
- golden: structural pass; **all 20 draw goldens byte-identical** (order carries
  no semantics — every access is by name), 84 zeus tests pass.

**Not done, and why** (each is a broad, cross-cutting rewrite, not a bounded
step; doing them half-way is how a UI engine regresses silently):

- Item 1, struct-of-arrays: every `nodes[id].field` read/write across `arena`,
  `layout`, `scene`, `input`, and `zeus` becomes a parallel-array index. UFCS
  accessors can hide the call sites, but the change is engine-wide and cannot be
  landed and verified in one reviewed step.
- Item 2, style interning (`style_id: u16`): needs a style-set hash and a
  side table, and every style read to go through it.
- Item 3, string interning (`{offset: u32, len: u16}`): text flows through
  `metrics`, `scene` (draw ops), a11y, and inputs; the storage change is safe
  but wide.
- Item 4, `u16` ids/indices: cross-cutting, and it *collides with the Phase 2
  cap* — `u16` ids leave no sentinel headroom above 65535 if the cap is also
  65535. Also, item 4's "geometry in `f32`" is **not** how this tree works:
  geometry (`x/y/w/h`, scroll) is integer `int` today. Moving it to `f32` is a
  layout-wide precision change, not a storage tweak, and would need its own
  golden pass. Flagged, not silently skipped.

Net: Phase 3's free/ordering step is done and measured; its structural items
(1-3) and the `f32` geometry requirement (4) remain open.

### Phase 4 — software rasterizer (framebuffer, color, coverage, borders, shadows, tiles, damage clip, blit contract, multi-contour, glyph outlines, glyph bitmaps, glyph atlas)

- bytes/node: **476.0 — unchanged.** No tree/geometry code was touched.
- New module `packages/zeus/std/zeuscore/raster.loam`:
  - **One owned framebuffer**, `physical_w * physical_h * 4`, BGRA8,
    premultiplied. Enabled by a fact worth recording: Loam has `[]u8` with a
    1-byte element, so the buffer is 20 MB — not the 83 MB a `[]int` buffer at
    4 bytes/channel would have cost. That was the make-or-break question for
    this phase and the answer is favourable.
  - **Reallocated only on a real change.** Measured: 1440x900 logical @ scale 2
    → 2880x1800 physical, **20 736 000 bytes**; a second identical `ensure` is a
    no-op (generation and realloc count flat, footprint flat at 18 192 KB);
    switching to scale 1 reallocates exactly once to 5 184 000 bytes (6 256 KB).
    Process footprint across the allocation: 1 728 → 18 192 KB — this buffer is
    the largest allocation in the process, as intended.
  - **sRGB↔linear tables** and a compositor that blends in linear space:
    `srgb_to_lin[256]` into a 12-bit linear domain and `lin_to_srgb[4096]` back.
    (An 8-bit linear domain is unusable — sRGB bytes 1..12 collapse to linear 0,
    crushing shadows; the test asserts the dark end survives.) Verified:
    round-trip within 1, monotonic, non-linear curve, and the headline check —
    50% white over black lands at **188**, not the naive sRGB byte blend's 128.
  - **Ordered 8x8 Bayer dither**, asserted to be a permutation of 0..63, and a
    `raster_quantize` that applies it when a gradient writer rounds to a byte.
- golden: structural pass; all 15 draw goldens byte-identical; pixel @2x still
  *pending* — it needs geometry rasterization + the blit, which is the rest of
  this phase.

**Analytic coverage (second step).** `raster.loam` flattens a rounded rect /
circle into a convex device-pixel path and fills it with exact per-pixel area
coverage — the path is clipped to each pixel (Sutherland-Hodgman, valid because
the path is convex) and its area taken by shoelace. That is analytic coverage,
not point sampling. Coverage is computed from GLOBAL geometry and only the WRITE
is clipped to the region, so tiling cannot produce seams:

- `zeus_raster_tiles.loam` fills a 520px circle spanning a 3x3 grid of 256px
tiles; the tiled checksum **equals** the whole-surface checksum (373695643).
- 1764 edge pixels are graded — values strictly between background and fill —
so AA is real, and the interior/rounded-corner pixels are exact.
- Curves flatten to a **0.25 DEVICE-pixel** tolerance; a plain rectangle's edges
land exactly on the pixel grid (no half-lit rows).

**Borders and hairline snapping (third step).** A border is a ring, so it is not
convex: `raster_stroke_round_rect` accumulates its four straight runs and four
corner quarter-rings into a **tile-sized coverage buffer** and composites once.
Piece-by-piece compositing would double-blend at every shared edge and show a
seam around every border. A border whose device width rounds to <= 1px snaps its
outer edges to the pixel grid (quality rule 8). `zeus_raster_stroke.loam`:

- a 1px border is exactly one row on all four sides, interior untouched;
- a fractional device origin (20.4, what `logical * scale` produces) still lands
  **one crisp row**, not two half-lit ones;
- a 4px border is four solid rows and stops;
- a rounded ring leaves the interior clear and paints its corner arc.

The coverage buffer is scratch (tile-sized, reset per shape), per the Phase 4
rule that intermediates never reach surface size.

**Shadows (fourth step).** A drop shadow is one color at a soft alpha, so the
only thing to compute is a **single-channel** alpha mask: the rounded rect's
coverage, blurred by three separable box passes (a cheap Gaussian
approximation), offset, composited under the fill. The mask is scratch — one
byte per pixel over the shadow's own support plus the tile, reset per draw —
never RGBA and never surface-sized. `zeus_raster_shadow.loam` verifies: bounded
support (well outside the blur is untouched), the `dy` offset makes it heavier
below than above, the falloff is monotone (not a hard edge), and painting the
card over the shadow restores the interior while the surround stays tinted.

**Damage-limited tiles (fifth step).** The Phase 1 damage list now drives tile
selection: `scene.damage_tiles_build` turns it into the set of 256x256 tiles a
frame must visit — every tile on a full-damage frame, only the touched tiles on
a paint-only frame — deduped so overlapping damage rects (a dirty container
damages its subtree) never rasterize a tile twice. One flag per tile, reset per
frame; nothing is built on the graph. `zeus_damage_tiles.loam`: a layout frame
over a 600x600 surface selects all 9 tiles; a paint-only node at (300,300)
selects exactly one, at (256,256). This is the set the renderer will iterate in
place of tiling the whole surface.

**Damage-clipped raster (sixth step).** The tiled fills, strokes, and shadows
now honor a device-space clip box: each tile is intersected with it, and only
that sub-region is written. Coverage is still computed from global geometry, so
a clipped render is **byte-identical to the full render inside the clip** and
touches nothing outside it — which is what makes rasterizing only the damaged
tiles safe. `raster_clip_from_damage` derives the box from the frame's damage
list (its bounding box, a superset, so it can only damage more, never less) and
clears it on a full-damage frame. `zeus_raster_clip.loam` asserts the identity
inside the clip, a clean background outside it, and a full repaint once the clip
is reset.

**Blit (seventh step).** The zero-copy contract has two halves. The Loam half
is a **generation**: `raster_blit_generation()` changes only on reallocation
(size or backing-scale change), not per frame, so a host wraps the buffer once
and rebuilds its wrapper only when that number moves. Verified in
`zeus_raster_blit.loam`: a redraw keeps the generation (wrapper reused), a scale
change bumps it (wrapper invalidated). The **host half** — wrapping the bytes
with a no-copy `CGImage` data provider on native, a typed-array view on web — is
host FFI that needs a display and `devicePixelRatio`, so it is not landed here
and remains open. The same test lands the **headless PPM dump**
(`raster_dump_ppm`): a golden can now be captured from our own bytes with no
graphics API, which is what Phase 5 needs.

**Multi-contour fills (eighth step).** Glyph outlines need two things the
convex fills did not cover: **holes** (a letter's counters, a ring) and
**concave** contours. Concave single contours already worked — the pixel clip is
Sutherland-Hodgman against the pixel square, which does not require the subject
to be convex. Holes are added by accumulating each contour with a winding sign
(+1 outer, -1 hole) into the tile-sized coverage buffer and compositing once, so
a hole subtracts. `zeus_raster_holes.loam` verifies a ring (empty centre, filled
band), a concave L-shape (notch empty, both bars filled), and that the damage
clip applies. This is the rasterizer the glyph atlas will draw into.

(First run of that test failed and looked like a concave-fill bug; the polygon I
wrote was self-intersecting — one vertex had x=140 where it needed x=40. The
rasterizer was right and the test was wrong.)

**Glyph outlines (ninth step).** `std:font` now parses `loca` + `glyf` and
returns a glyph's contours in font units: points with their on/off-curve flag
and per-contour end indices. Simple glyphs are parsed; composite glyphs report
`ok = 0` for now. The `tiny.ttf` fixture was metrics-only, so the generator
(`make_tiny_font.py`) now emits `glyf`/`loca` too, including `B` as a
two-contour glyph (an outer box plus its counter) to exercise holes at the
outline level. `zeus_font_outline.loam` checks `A` (one contour, three
on-curve points, exact coordinates), `B` (two contours, eight points, the
counter inside the outer), and space (an empty `loca` entry, not an error).
Metrics are unchanged: `golden_font_metrics` still matches byte for byte.

**Compiler bug this work surfaced.** Adding the raster module's ~40 globals and
constants tipped four large programs (zeus_components / key_prop / reactive /
spec_props) past a **silent 256-entry cap** in `typecheck.c`: `gvars[256]`,
`monos[256]`, `struct_insts[256]` dropped entries once full with no diagnostic,
so `std/async.loam`'s own globals became "unknown identifier" — a bogus error
pointing at the wrong file. Raised to 4096 / 2048 / 2048 and every overflow is
now a compile error instead of a silent drop. It is a latent compiler bug any
large app could hit; Phase 4 is simply what surfaced it. (The full `nob test`
run: 449 passed; the 6 failures are the network suites this sandbox blocks.)

**Glyph bitmaps (tenth step).** The parsed outline now becomes pixels, still
with no font API. `raster_tt_contour` walks a glyph's on/off-curve points,
resolving implied on-curve midpoints between consecutive control points, and
flattens every quadratic to the same **0.25 device-pixel** tolerance the rest of
the rasterizer uses. `raster_multi_add_tt` derives each contour's winding sign
from its orientation relative to the first contour, so the caller passes no sign
and a counter subtracts; `raster_multi_add_glyph` converts font units to device
pixels (y-flip on the baseline) first, so the flip happens before orientation is
measured and the sign stays correct. `raster_multi_to_mask` rasterizes the set
into an offscreen **single-channel R8** mask over a device box at most one tile
a side — scratch, read back with `raster_mask_*`.

`zeus_raster_glyph.loam` is the end-to-end check, headless:

- `A` (a triangle) lands interior-opaque, corner-empty, with **126 graded
  pixels** — the slanted edges are antialiased, not hard.
- `B`'s **counter is empty** while both bands are filled. That single assertion
  can only pass if `loca`/`glyf` parsing, implied-on-curve resolution, quadratic
  flattening, and signed winding all work together — it is the whole glyph-shape
  problem in one check.
- an empty glyph (space) contributes no contours, and is not an error.

(That test failed on first run because *I* picked sc = 16/1000 when I meant
64/1000 — the glyph came out 12px wide and every sample point was outside it. A
probe with `raster_multi_add_poly` at the intended coordinates rendered the
triangle exactly as expected, which pointed at the scale, not the rasterizer.)

- bytes/node: **476.0 — unchanged.** The glyph path allocates only scratch
  (tile-sized coverage, one mask, two point vectors), all reset per call; the
  headless `membench` numbers are identical to the Phase 3 column
  (tree @1000 = 953 420, total 982 624, `node_record_bytes` 472).
- validation: 99 zeus `compile_pass` tests pass (the 2 failures are the
  network suites this sandbox blocks), all 15 draw goldens byte-identical,
  structural golden pass, `tools/mem-baseline.sh --headless` unchanged.

**The glyph atlas (eleventh step).** `std/zeuscore/atlas.loam` is the cache
that makes per-frame glyph rasterization affordable, and it is where the memory
discipline shows:

- **One R8 atlas**, 1024x1024 to start (1 MB), shelf-packed. It is a pure cache:
  any entry may be dropped at any time and re-rasterized from its key, which is
  what makes eviction, repacking and growth all safe.
- **Keys** are (font_id, glyph_id, size_px, subpixel_x). Subpixel-x is a
  **quarter-pixel** bucket at logical sizes <= 16 and always 0 above that, so a
  small glyph can occupy up to four entries. That is the one place quality costs
  memory, accepted deliberately: evenly spaced small text is worth the few
  hundred KB.
- **Bounded LRU, everywhere.** A fixed 4096-entry pool with a free list, a fixed
  16384-slot open-addressed index (rehashed at 70% load, tombstones dropped), and
  an exact O(1) intrusive LRU list. On pressure the cache **evicts rather than
grows**, because a miss costs one re-rasterization while a bigger atlas is real
  memory; it grows once only when a glyph cannot fit an *empty* atlas, then
  reallocates and drops the (cache) entries. There is no unbounded map anywhere:
  the string-interning table and the measurement cache have fixed capacities
  too, and a full string table is a counted, non-fatal condition.
- **Coverage gamma**: a 256-entry table, `cov = round(255 * (c/255)^(1/1.2))`,
  forced monotone, applied on insert. Compositing is already gamma-correct in
  linear space (which is what fixes polarity), and this restores the stem weight
  type was designed for, as one documented constant rather than an accident.
- **Repacking is overlap-safe**: live pixels go to scratch first, so moving them
  inside the single atlas buffer can never clobber a block it has not read yet.

`zeus_atlas.loam` verifies, headless: gamma is monotone with exact 0/255
endpoints and a modest midtone lift; the quarter-px buckets are 4 at 11-16px and
1 above; a 128x128 atlas seats exactly four ~54x47 glyphs; a repeat intern is a
pure hit (no re-rasterization); subpixel, font and size are all part of the key;
on pressure **exactly one** entry is evicted and it is the least recently used
one (the entry touched just before it survived); an evicted key re-interns and
is re-rasterized; growth happens only for a glyph larger than the whole atlas;
an over-tile glyph is refused, not drawn wrong; a blit lands ink where the glyph
is and leaves the background alone; and the measurement cache stays at its fixed
capacity while evicting.

- bytes/node: **476.0 — unchanged.** The atlas is lazily initialized, so
  `membench` reports `glyphs= 0` and its totals are byte-for-byte the Phase 3
  column (tree @1000 = 953 420, total 982 624). A frame that draws text will
  report the atlas under the new `bytes glyphs=` line rather than hiding it in
  `misc`.
- validation: 100 zeus `compile_pass` tests pass (the 2 failures are the network
  suites this sandbox blocks), all 15 draw goldens byte-identical, structural
  golden pass, `tools/mem-baseline.sh --headless` unchanged.

**Image resampling and cache (thirteenth step).** `image.loam` gains the part
that decides image memory, and it is deliberately the opposite of the usual
"decode and cache everything" approach:

- **Separable Mitchell** (B = C = 1/3), with the kernel widened by the reduction
  factor so the support becomes `2 * scale` source pixels. That is what makes a
  large downscale area-correct rather than aliased.
- The filter runs on **premultiplied** pixels (so a transparent edge cannot
  bleed its colour) and the result is unpremultiplied on the way into the cache,
  so the framebuffer's existing straight-source, gamma-correct blend composites
  it with no second blend path.
- **A fixed byte budget with LRU eviction**, not a growing map: entries are
  evicted until `bytes + need <= budget`, the entry pool is fixed at 64, and an
  image larger than the whole budget is **refused and counted** rather than
  stored. The source decode lives in one reusable scratch buffer and dies as
  soon as the resized result is cached, so a thumbnail never retains a
  full-resolution decode.

`zeus_image_resize.loam` asserts the aliasing property, not just "it runs": a
4x downscale of a 1px checkerboard (the highest frequency an image can carry)
comes out **flat** — the 12x12 interior is exactly constant at 128 — where
nearest-neighbour would swing the full 0..255. The outermost row/column is
allowed to differ, because the filter extends the boundary pixel and there is
genuinely no data outside the image; the test pins that deviation to <= 16 and
still mid grey. It also checks cache hits (a repeat request does not re-decode
or re-resample), that display size and image id are both part of the key, that
an upscale is monotone without ring overshoot, that the budget invariant holds
through eviction and that a refusal stores nothing, and that a blit lands the
averaged grey on the framebuffer.

- bytes/node: **476.0 — unchanged.** The image cache is lazily initialized and
  `membench` touches no images, so it reports `images= 0` and the totals are
  byte-for-byte the Phase 3 column. A real frame reports it under the new
  `bytes images=` line.
- validation: 103 zeus `compile_pass` tests pass (the 2 failures are the network
  suites this sandbox blocks), all 15 draw goldens byte-identical, structural
  golden pass, `tools/mem-baseline.sh --headless` unchanged.

**Formats, and keeping the existing ones working (fourteenth step).** The
pipeline is format-agnostic by construction — everything downstream of a decoder
sees premultiplied RGBA8 — so supporting a format means "produce premultiplied
RGBA8 here" and nothing else in the engine moves. This step makes that explicit
and adds the second decoder:

- **`src_format_of` sniffs the container by magic bytes**, not by file extension
  (which lies). It recognises PNG, BMP, JPEG, GIF, WebP, TIFF, ICO, AVIF
  (`ftyp`) and SVG (leading whitespace then `<`) and returns `unknown`
  otherwise. It is deliberately a *classifier*, not a decoder: the UI needs to
  know what a file is in order to route it.
- **Routing, not replacement.** `src_last_status()` distinguishes
  `SRC_MALFORMED` (the bytes are not a valid image of a format we decode) from
  `SRC_UNSUPPORTED` (a real format the in-Loam decoders do not cover). The
  second is the signal to use the platform image path — `plat_image`, reached
  through `platform.loam` (ImageIO/AppKit on native, the host's loader on web) —
  which is what has always loaded these formats. So **no format this engine
  already displayed stops working**: the `scene.image` op and its host call are
  untouched, and the two image draw goldens still pass byte-for-byte. As more
  decoders land, more formats take the in-Loam path; the rest keep working
  through the platform.
- **BMP**, the second in-Loam decoder: uncompressed `BI_RGB` only, 1/4/8-bit
  paletted plus 24- and 32-bit, bottom-up and top-down (negative height).
  Compressed (RLE) and `BI_BITFIELDS` are **refused with a reason**
  (`compressed bmp`) rather than misdecoded, which routes them to the platform.
- **A native-size request is now a byte-exact copy, not a filtered one.** That
  was a real bug the richer fixtures exposed: `image_get` at 1:1 was running a
  scale-1 Mitchell over the image and blurring it. "Show this image at its real
  size" is the common case, not an edge case.

`zeus_image_formats.loam` pins both halves: the sniffer classifies all nine
containers correctly and reports which the Loam decoders cover; a JPEG returns
`SRC_UNSUPPORTED` while a corrupt PNG returns `SRC_MALFORMED`; BMP decodes
pixel-for-pixel for 24-bit bottom-up, 24-bit top-down (agreeing with bottom-up),
32-bit with alpha, 8-bit paletted, and an RLE BMP is refused by name; and a
BMP downscaled 4x comes out **flat** — the same aliasing property as PNG, which
is what proves the pipeline really is format-agnostic. Two fixture-only bugs were
caught by writing the fixtures by hand: bottom-up BMP rows were emitted in image
order, and an 8-bit row's alignment padding was computed in bits instead of
bytes (a 3-pixel row silently lost its pad byte).

- bytes/node: **476.0 — unchanged**; membench reports `images= 0`.
- validation: 103 zeus `compile_pass` tests pass (the 2 failures are the network
  suites this sandbox blocks), all 15 draw goldens byte-identical, structural
  golden pass, `tools/mem-baseline.sh --headless` unchanged.

**A real DEFLATE inflater (fifteenth step).** Stored blocks alone would only
decode fixtures we generate ourselves; real PNGs are Huffman-coded, so this is
what turns the in-Loam decoder from a demonstration into a decoder. `png_inflate`
now implements RFC 1951 in full: stored blocks, **fixed** Huffman (type 1) and
**dynamic** Huffman (type 2), with the canonical code construction, symbol
decoding, and LZ77 back-references (byte-by-byte copy, so an overlapping
match is correct). Canonical Huffman is held **flat** — three tables side by
side as parallel arrays — so there is no per-block or per-node allocation, and
the bit reader is just a bit position over the IDAT bytes.

Three bugs, each caught by a fixture chosen for one code path:

- Dynamic blocks failed while fixed blocks worked. The canonical-code builder
  was counting **zero-length (unused) symbols** into `count[0]`, and `count[0]`
  feeds the first-code of length 1, so every code in the table was shifted. The
  fixed table has no zero lengths, which is exactly why only the dynamic path
  was broken — a single compressed fixture would have hidden this.
- The repeat codes in a dynamic header (16/17/18) read their extra bits
  unconditionally, so 17 and 18 consumed 2 bits too many and desynchronized the
  stream.
- `hl_lens[idx - 1]` for a leading repeat code indexed before the array.

`zeus_image_huffman.loam` decodes four compressed fixtures chosen for different
paths — `Z_FIXED` (block type 1), the default level (block type 2), a ramp
across all 256 literal values, and 16px runs whose LZ77 matches sit at a
distance of a whole row stride (so long length codes and 8+-bit distance codes
are exercised) — and checks every one of the 16384 ramp pixels and the whole
256x128 runs image. The fixture script prints each file's DEFLATE block type, so
the coverage is a fact rather than an assumption. Then `real.png`, shaped like a
real encoder's output — dynamic Huffman, a **different filter per row** cycling
through all five, and the **IDAT split across 29 chunks** — verifies all 90000
channels exactly. None of those three things appears in any hand-built fixture.
Two guards are pinned too: a container that declares 64x64 but carries 32x32
scanlines is refused by name (`wrong inflated length`), and `bomb.png` (4x4
declaring 200000 inflated bytes) is refused **without inflating them** — the
scratch stays at 219 bytes, where an inflate-then-check decoder would have
allocated 200 KB to find out.

- bytes/node: **476.0 — unchanged**; membench reports `images= 0`.
- validation: 104 zeus `compile_pass` tests pass (the 2 failures are the network
  suites this sandbox blocks), all 15 draw goldens byte-identical, structural
  golden pass, `tools/mem-baseline.sh --headless` unchanged.

**PNG's remaining sample formats (sixteenth step).** The RGB/RGBA path was the
easy half. `png_decode` now handles what real assets actually use, and what an
"8-bit RGB only" decoder silently gets wrong:

- **Sample formats**: grayscale (colour type 0) at depths 1/2/4/8/16,
grayscale+alpha (4), truecolour (2) and truecolour+alpha (6), and **palette**
  (3) at depths 1/2/4/8. Sub-byte samples are read MSB-first out of shared
  bytes, and 16-bit samples take the high byte (the engine is 8 bits per
  channel). The filter's "bytes per pixel" is derived from the bit depth, so a
  1-bit scanline filters correctly.
- **tRNS in all three of its meanings**: a named gray sample, a named RGB
triple, and a per-palette-entry alpha list.
- **gAMA**: noted rather than converted. The whole pipeline is sRGB end to end,
  so an image declaring a different transfer curve is composited as sRGB, and a
  counter (`src_gamma_assumed`) makes that assumption visible instead of silent.
  Real colour management needs a per-image transform and is not pretended here.
- **Adam7 interlacing**, in full. This was the last PNG feature that would send a
  real file to the platform. Each of the seven passes is unfiltered with ITS OWN
  scanline stride and row count, then scattered into the output — which is why
  the output buffer is pre-sized and written by index rather than appended, and
  why the pass table's row steps matter (a `0` there is an immediate division by
  zero). A pass with a zero dimension contributes no scanlines at all, so the
  expected inflated length is summed over the passes that exist and the pass
  offsets are computed from that, not from a running seven-pass guess.

`zeus_image_png_variants.loam` verifies Adam7 at dimensions that are *not*
multiples of 8 (13x11, 9x9, 5x3) so several passes are partial, checks all 429
channels of the interlaced truecolour file against the formula that generated it,
cross-checks the same image written flat, and covers Adam7 with packed sub-byte
samples plus a palette and tRNS.

- bytes/node: **476.0 — unchanged**; membench reports `images= 0`.
- validation: 105 zeus `compile_pass` tests pass (the 2 failures are the network
  suites this sandbox blocks), all 15 draw goldens byte-identical, structural
  golden pass.

**GIF (eighteenth step).** Palette + LZW, the transparent index from a graphic
control extension, GIF's four-pass interlacing, and the first frame of an
animation (the engine has no image animation loop, so a still frame is the
documented behaviour rather than a silent pick; a UI that wants the animation can
use the platform path).

The fixtures are written by **Pillow**, and that is deliberate. The PNG and BMP
fixtures are hand-built because their container *is* the spec, so agreeing with
them proves something; a hand-built GIF fixture would share an LZW implementation
with the decoder under test and prove nothing at all. So each GIF fixture ships a
`.rgba` file holding Pillow's own decode in exactly the form this decoder
produces — premultiplied RGBA8, a transparent pixel's colour zeroed — and
`zeus_image_gif.loam` compares **every byte** of four real GIFs: a flat paletted
image, one with a transparent index, an interlaced one, and a 3-frame animation
(the last also pinning that frame 0 is what comes out).

- bytes/node: **476.0 — unchanged**; membench reports `images= 0`.
- validation: 106 zeus `compile_pass` tests pass (the 2 failures are the network
  suites this sandbox blocks), all 15 draw goldens byte-identical, structural
  golden pass.

**ICO (nineteenth step).** An icon entry is an embedded PNG or a BMP DIB whose
declared height counts the AND mask (twice the real height), so this is a
container around the two decoders already tested rather than a third format:
the largest entry is chosen, the DIB's height is rewritten so the BMP decoder
sees a normal file, and the AND mask is applied.

Two details the fixtures settled, both of which a guess would have got wrong:

- **The AND mask is optional.** Pillow omits it for a 32-bit entry (its alpha
  channel already says what the mask would), so a decoder that requires it
  rejects perfectly ordinary icons. Its absence is now handled rather than
  treated as truncation — and the XOR rows being complete is still required.
- Pillow writes PNG entries by default and DIB entries only with
  `bitmap_format="bmp"`, so the fixtures ask for both on purpose: the DIB path
  (24-bit and 32-bit, both with the height-doubling and the mask) and the
  container-recursion path (a 256x256 embedded PNG) are each covered by real
  files.

`zeus_image_ico.loam` compares every byte against Pillow's decode for a 24-bit
DIB icon, a 32-bit alpha DIB icon, a three-size icon (proving the largest entry
is the one drawn) and a 256x256 PNG-entry icon, then checks the cache path
including a downscale.

- bytes/node: **476.0 — unchanged**; membench reports `images= 0`.
- validation: 107 zeus `compile_pass` tests pass (the 2 failures are the network
  suites this sandbox blocks), all 15 draw goldens byte-identical, structural
  golden pass, `tools/mem-baseline.sh --headless` unchanged.

**The raster scene pass (twentieth step).** `present()` hands the draw list to
the host, which draws it with a 2D API. `scene.rasterize(scale, ...)` is the
other consumer of the **same** list: the Loam rasterizer, into the one buffer we
own, at the backing scale. Paint does not change — a frame simply has two
possible consumers, and this one needs no drawing API at all. This is where the
two halves of the project meet.

What it rasterizes today: fills (uniform and per-corner `fill4`, the latter
decomposed into five convex bands plus one wedge per corner and accumulated into
the coverage buffer so overlaps composite once rather than double-blending),
strokes, shadows, the clip/save/restore nesting, and images the app has decoded
and registered (`scene_register_image`), so a draw op's `src` can be blitted from
our own cache instead of the host's.

Two rules it follows that are worth stating:

- Every op is emitted in LOGICAL units and scaled here, so the corner arcs are
  generated at physical resolution rather than scaled up from a bitmap — which
  is the whole point, and what the test checks by counting partially-covered
  pixels along the arc.
- An op the pass cannot rasterize yet is **counted per kind**, never skipped
  silently. Text, gradients, SVG and `xform` are the current gaps, and they are
  numbers (`scene_raster_unhandled(2)`), not a missing thing in a picture
  somebody has to notice. Rounded clips are likewise counted
  (`scene_raster_rounded_clips`) and currently treated as square.

`zeus_raster_scene.loam` builds a real scene through the public API, paints it,
and then rasterizes it — asserting against the geometry read back out of the
draw list, so it says "whatever paint decided this card is, it came out with a
round corner, a filled middle and an antialiased arc at 2x" rather than
hard-coding a layout the test would then be measuring instead of the engine. It
also pins that a shapes-only scene skips nothing while a scene with text reports
exactly those ops as unhandled.

- bytes/node: **476.0 — unchanged**; membench draw ops and bytes are unchanged
  (this step adds no per-node or per-frame state).
- validation: 108 zeus `compile_pass` tests pass (the 2 failures are the network
  suites this sandbox blocks), all 15 draw goldens byte-identical, structural
  golden pass, `tools/mem-baseline.sh --headless` unchanged.

**Gradients, in the raster pass (twenty-first step).** `raster_fill_path` gained
one branch: when a gradient is armed, the per-pixel composite takes its colour
from a lerp instead of the op's flat colour. Everything else about the fill is
unchanged — same coverage, same tiling, same clip — which is why the existing
raster tests still pass byte for byte.

Two things make it correct rather than merely present:

- **The lerp is in the 12-bit LINEAR domain**, so a black-to-white gradient has
  its perceptual midpoint at sRGB **188**. Lerping the sRGB bytes would put it
  at 128 and make every gradient look washed out — the same gamma rule the
  compositor already follows, applied to a gradient instead of an alpha blend.
  The test asserts the midpoint lands in 170..210, so an sRGB lerp fails it.
- **The result is dithered by half an 8-bit step** before quantization, which is
  quality rule 10 ("costs nothing, removes visible banding"). A consequence is
  that a gradient is no longer strictly monotone pixel to pixel, which is what
  dithering IS; the test therefore bounds the step (<= 2 in the mid-range)
  rather than demanding monotonicity, and checks the tile seam by requiring the
  step across a 256px boundary to be no larger than the steps around it — a
  gradient computed per tile would restart there, a jump of half the range.

The gradient's span is the SHAPE's device box, not the surface: a gradient runs
corner to corner of the thing it fills.

- bytes/node: **476.0 — unchanged**.
- validation: 108 zeus `compile_pass` tests pass (the 2 failures are the network
  suites this sandbox blocks), all 15 draw goldens byte-identical, structural
  golden pass.

**Remaining Phase 4** (not done): text and SVG in the raster pass, JPEG and WebP,
and the host blit (see the ABI note below).

**The host blit is blocked on an ABI gap, not on effort.** The Loam half is
landed and tested (the buffer's `fb_gen` generation changes only on reallocation,
never per frame, so a host wraps once and rebuilds only when it moves). The host
half needs the address of that buffer, and there is currently no way for a
platform function to receive a `[]u8` — the platform ABI carries `loam_str` and
scalars only, and Loam has no address-of for a vector. Two candidate fixes, both
small: let the host reach the generated symbol for the module's buffer, or add a
`&mut []u8` platform parameter. Until then the honest position is that the
zero-copy blit contract is specified and measured, and the host binding is the
next step — not verified, and not claimed.

**Image formats — status and roadmap.** Landed in Loam: **PNG** — colour types
0/2/3/4/6, depths 1/2/4/8/16, all five scanline filters, multiple IDAT chunks,
stored/fixed/dynamic DEFLATE, PLTE and all three forms of tRNS, **Adam7
interlaced** — **BMP** (uncompressed 1/4/8/24/32-bit, both scan orders), **GIF**
(palette, LZW, transparent index, interlacing, first frame of an animation) and
**ICO/CUR** (DIB or embedded PNG, largest entry, AND mask). There is no longer
any PNG a real encoder produces that we cannot decode.

Still handled by the platform path, and therefore still working: **everything
else** — JPEG, GIF, WebP, TIFF, ICO, HEIC and any PNG variant the Loam decoder
does not yet cover. `src_format_of` + `src_last_status` are what let the renderer
pick the in-Loam path where it applies and hand the rest to `plat_image`, so this
is a routing decision rather than a capability cliff.

The order that buys the most next:

1. **JPEG** baseline sequential: Huffman tables, dequantize, IDCT, YCbCr,
   chroma upsampling. The largest remaining decoder, and what photos and
   avatars need; progressive is a second pass.
2. **WebP**: lossless/VP8L is a moderate job (LZ77 with transforms); lossy VP8
   is a different order of magnitude and should be its own project.
3. **SVG** is not a pixel format: it routes through the rasterizer we already
   have (paths, fills, strokes), so it belongs with the paint work, not here.

Not planned and not pretended: **AVIF** and **HEIC**, which are AV1 and H.265
bitstreams. Those go to the platform (which is also what every other desktop
toolkit does), and a UI that needs them should ask the platform for the pixels
rather than grow a video decoder here.
