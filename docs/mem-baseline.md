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

**Remaining Phase 4** (not done, and each is substantial): image decode with
Mitchell/Lanczos2 under a fixed byte budget with LRU eviction, the host half of
the blit (a no-copy `CGImage` provider on native, a typed-array view on web), the
advance/kerning step the atlas deliberately does not own, and driving
`scene.paint` through the rasterizer (which changes every draw golden and so
needs its own decision).
