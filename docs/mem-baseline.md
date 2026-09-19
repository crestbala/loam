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
