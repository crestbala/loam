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
