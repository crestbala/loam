# Zeus refactor plan: a small, simple core, with Zeus's extras on top

Status: proposal (2026-10-31). Nothing in this document has been done yet
except where it says **Done**.

## Goal

Make the Zeus engine small and simple to read, reason about and port: a
compact layout core that declares a tree, lays it out in a few passes and
returns a flat list of render commands. On top of that core sits everything
else Zeus offers: signals, animation, scroll physics, text input, focus,
accessibility, theming, routing. Two hard constraints:

1. **No app-visible change.** `Box`, `Text`, `Style(...)`, `zui.*`, `zeus.*`
   and every `zeus-components` widget keep their syntax and behaviour. An app
   that builds today builds and looks the same after every phase.
2. **No regression.** Each phase lands only when the full suite passes, the
   draw-list goldens match, and the performance probes are no worse.

## Where Zeus is today

| Area | Size | What makes it hard |
|---|---|---|
| `zeuscore/arena.loam` | 7.3k lines, 381 fns | One `UiNode` record with **176 fields** mixing layout, style, paint, scroll, animation, a11y, input and list state |
| `zeuscore/layout.loam` | 1.9k lines | Flex / grid / abs plus a ~450-line subtree re-solve (`relayout`, `find_boundary`, `imposed_w/h`) that exists only to avoid re-running layout |
| `zeuscore/scene.loam` | 2.4k lines | Kind-specific paint branches (button, progress, slider, toggle, check, input…), plus retained spans, patching, splicing and damage capture |
| `zeusbase.loam` / `zeus.loam` | 3.6k + 2.4k lines | The element layer, style application, engine entry points |
| `zeus-components` | 10.2k lines | Widgets; already built from `Box` / `Text` / `Style` |

The target core is a few thousand lines: declare the tree, size X, wrap
text, size Y, position (with scroll offsets and floating), emit render
commands, answer pointer queries. Its speed comes from a flat arena and
redoing cheap work each frame, not from caching.

Most of Zeus's complexity is caching that keeps it from recomputing: subtree
re-solve, retained op spans, patch and splice, damage rects. Each of those has
produced bugs (stale spans, scroll offsets lost in the patch walk, popups
drifting).

### Already done (2026-10-31)

- **Done:** scroll at position time. A scroll re-places floaters only and never
  re-solves layout.
- **Done:** culling. Off-screen nodes under a clip record no ops.
- **Done:** one pointer model in paint order (floaters first, window space,
  clips, occlusion, passthrough).
- **Done:** floating boxes (`Style.floating`, 9×9 attach points, root /
  parent / anchor), per-side and between-children borders, `textAlign`, and
  pointer queries (`engine_hit`, `pointer_over`, `screen_x/y`).
- **Done:** scrollbar polish. Eased fade-in and fade-out with an 800 ms
  linger, a re-show during fade-out that continues from the live alpha, a
  track aligned with the thumb that shows only on hover or drag, a hot strip
  that widens the thumb, and a release that lingers.

See `docs/layout.md` for how those work today.

## Target architecture

```
┌─────────────────────────────────────────────────────────────┐
│ App / zeus-components  (unchanged syntax)                    │
├─────────────────────────────────────────────────────────────┤
│ Extras  (each optional, each one file)                       │
│  signals+retained · anim · scroll physics · focus · a11y ·   │
│  text edit · virtual lists · theme · devtools                │
├─────────────────────────────────────────────────────────────┤
│ Element layer  zeusbase: Style → core fields, events → ids   │
├─────────────────────────────────────────────────────────────┤
│ Core  ("zcore", no imports above it)                         │
│  tree.loam    flat element arena, parent / first-kid / next  │
│  layout.loam  size-x → wrap → size-y → position → floating   │
│  render.loam  culled render commands                         │
│  pointer.loam hit / pointer-over / scroll target             │
├─────────────────────────────────────────────────────────────┤
│ Host seam  platform.loam: measure text, insets, clock, blit  │
└─────────────────────────────────────────────────────────────┘
```

Rules for the core:

- It imports nothing above it. It knows nothing about signals, widgets,
  themes or animation.
- It takes **declarations** (an element config: layout, floating, clip,
  border, corner radius, background, text, image, custom) and returns a
  **render command list** plus pointer answers.
- Layout recomputes whole each layout frame, and is made fast by:
  - flat structure-of-arrays storage;
  - a text-measure cache keyed on (string, font, width);
  - layout not running on paint-only frames, which Zeus already does.
- Render commands are a small fixed set: rectangle, border, text, image, scissor
  start / end, custom. Kind-specific painting moves out of the core (below).

## Phases

Every phase follows the same loop:

1. Add the new path beside the old one.
2. Run both in tests and compare (the draw list and boxes must be identical).
3. Switch the default.
4. Delete the old path one release later.

### Phase 0: safety net (no engine change)

- Extend `tests/draw_golden` from 15 goldens to cover every `zeus-components`
  widget in its main states (closed and open, hovered, scrolled, disabled),
  plus every `examples/zeus/*` app's first frame.
- Add a box golden: each node's `(x, y, w, h)` after layout, dumped by a new
  `zeus.dump_boxes()`. Layout refactors compare against it.
- Add perf probes as tests with budgets. Use counts (draw ops, full layouts,
  nodes measured), not wall time, so they don't flake:
  - 2000-row scroll: ops < 200, full layouts 0 (exists: `zeus_scroll_cull`);
  - a 5000-node tree: layout visits ≤ 2 × nodes;
  - popup open while scrolling: 0 full layouts (exists).
- Freeze the public surface in `docs/api-freeze.md`: every `pub`/exported
  `zeus.*`, `zui.*` and `Style` key. A lint fails if one disappears.

**Exit:** the goldens cover all widgets and apps, the probes are green, and
the freeze list is checked in CI.

**Done (2026-10-31):**

- `tests/draw_golden/golden_w_*`: every public `zeus-components` widget, at
  rest and in the states that matter (open / closed, hovered, scrolled,
  disabled / invalid, collapsed). Sub-parts (`MenuItem`, `CalCell`, …) are
  covered through their parents and, where usable alone, in
  `golden_w_parts`.
- `tests/draw_golden/golden_ex_*`: the first frame of `counter`,
  `dashboard`, `dracula`, `gallery`, `membench`, `mune`, `myapp`, `vidget`,
  `vidy` and `ws`. Host start-up (servers, stores, pollers) is not run.
  `gallery` and `myapp` leave out their RAM chip (live process memory), and
  `gallery` its kv-backed form. `greeninfer` has no golden: its first frame
  indexes the source tree and prints wall-clock timings. `repl` has no Loam
  UI source.
- `zeus.dump_boxes()` and the box goldens `tests/draw_golden/box_*`.
- `zeus.layout_visits()` / `zeus.measure_visits()` and the probe
  `tests/compile_pass/zeus_layout_visits.loam` (5k nodes: today 1 visit per
  node, budget 2).
- `docs/api-freeze.md`, checked by the `api_freeze` stanza in `nob.loam`
  (run by `make test`).

### Phase 1: split the node record (pure data move)

Split `UiNode`'s 176 fields into tables indexed by node id. Each table is
owned by one module:

| Table | Fields (examples) | Owner |
|---|---|---|
| `tree` | parent, kids, kind flags, show | core/tree |
| `lay` | sizing, padding, gap, align, pins, floating, out-box `x y w h` | core/layout |
| `look` | bg, fg, border, radius, elevation, opacity, text, font, align | core/render |
| `scroll` | scroll_x/y, ext, vel, smooth, bar stamps | extras/scroll |
| `anim` | tracks, tween, show/hover/press amounts, transforms | extras/anim |
| `ui` | handlers, focus, a11y, edit slot, cursor, list window | element layer |

The record is replaced with accessor functions first (`arena.nodes[id].x` →
`lay_x(id)`), mechanically and file by file, then the storage is split.

**Exit:** no behaviour change (goldens identical), and `arena.loam` shrinks to
the tables plus dirty flags.

**Done (2026-10-31): the storage split.** `UiNode` is gone. A node is an id
into eight tables, each at most 30 fields. There are two more tables than
planned, because six cannot hold 176 fields at that size: layout owns three.

| Table | Record | Fields | Holds |
|---|---|---|---|
| `tree` | `TreeRec` | 22 | parent / kids / frag, kind, show, dirty bits, exit flag, boundary, scoped scheme and density, list window |
| `lay` | `LayRec` | 29 | sizing, min / max, percent, aspect, padding, border width, dir, justify, align, wrap, reverse, gap, overflow |
| `pin` | `PinRec` | 27 | margins, align-self, position and pins, grid cell and columns, safe area, z, anchor, floating |
| `box` | `BoxRec` | 14 | layout output: `x y w h`, natural and content extents, text size, wrap scratch, measure stamp |
| `look` | `LookRec` | 27 | colors, radii, per-side borders, elevation, opacity, text, font, cursor, spin / pulse |
| `scrl` | `ScrlRec` | 10 | scroll offsets, wheel targets, velocity, scrollbar stamps |
| `anim` | `AnimRec` | 17 | tween, hover / press / show amounts, transforms, reveal, exit transition |
| `ui` | `UiRec` | 30 | handlers, bound signals, focus, edit slot, a11y, interaction flags, pointer passthrough |

`arena.nodes[id].f` became `arena.<table>[id].f` at all ~1,800 sites. The
rewrite was mechanical, and the compiler checks it, because each field exists
on exactly one record. `push_blank` / `blank_at` / `pop_slot` keep the tables
the same length, and `tree.len` is the node count. All draw and box goldens
are unchanged. Accessor functions were skipped: table-qualified access gives
the same compiler guarantee without a call per field.

**Done (2026-10-31): the tables leave the arena.** The arena sits under
layout, scene and input in the import graph, so the tables could only move
down, into a new leaf module. `zeuscore/nodes.loam` imports nothing. It holds
the eight records and tables, the slot helpers (`push_blank`, `blank_at`,
`pop_slot`, `reset`, `node_record_bytes`), and 30 queries that read only the
tables:

- corner radii (`set_corner`, `radius_at`, `radius_max`, …);
- tree walks (`child_index`, `subtree_ids`, `is_or_under`, `boundary_of`,
  `in_exiting`, `frag_parent`);
- box-model queries (`box_pad_x/y`, `gap_x/y`, `has_w/h`, `child_align`,
  `fills_cross`, `axis_frozen`);
- floater predicates (`is_floater`, `is_placed`, `is_window_floater`,
  `scrolls_x`);
- `invalidate_path`, `node_paint_bits` and the memory counters.

Every module now reads `nodes.box[id].x`, not `arena.box[id].x`. This module is
the seed of `zcore`'s `tree.loam`. `arena.loam` went from 7.7k to 6.9k lines.

What remains in `arena.loam` is signals, dirty channels, the frame clock,
focus, animation tracks, scroll physics, exit transitions, scheme and palette
resolution, and edit slots. Those are the Phase 4 extras: they move out as
modules there, not as part of this data split. Pure queries that are
domain logic (`paint_alpha`, `scheme_of`, `palette_of`, `is_focusable`,
`exit_snapshot`, `stamp_edits`) stay with them on purpose.

### Phase 2: layout passes

Rewrite `layout.loam` as five passes over the `lay` table:

1. **Size X:** fit children bottom-up, then grow / shrink / percent top-down,
   honouring min and max (fit, grow, fixed and percent sizing, which are
   today's default, `grow`, `width = px(..)` and `width = pct(..)`, with
   `minWidth` / `maxWidth`).
2. **Wrap text** at the final widths (the measure cache makes this cheap).
3. **Size Y**, the same as X on the other axis.
4. **Position:** alignment, gap, margins, absolute pins. Scroll offsets apply
   here, never in sizing.
5. **Floating:** attach points against final boxes (`place_float`, done
   already).

Grid and flex-wrap stay, as extra cases in passes 1 and 4.

Then measure, without guessing:

- If whole-tree layout of the largest app is under budget (target < 1 ms for
  5k nodes on desktop), **delete the subtree re-solve** (`relayout`,
  `find_boundary`, `imposed_*`, `snapshot_natural`, `box_mark_moved`, about
  450 lines).
- If not, keep a much simpler version: re-run layout from the nearest
  fixed-size ancestor.

**Exit:** the box goldens match pixel for pixel on every widget and app, and
the probes pass.

**Measured (2026-10-31).** Whole-tree layout of the 5,083-node probe tree, at
the shipped `-O1` flags:

| | before | after the text cache |
|---|---|---|
| warm (measure stamps current) | 7.8 ms | **0.75 ms** |
| cold (every measure invalidated) | 14.1 ms | 1.9 ms |

Nearly all of the old cost was the host re-measuring text, wrapped labels word
by word, on every layout. Headless test builds are `-O0` with stub metrics, so
they show about 7 ms either way; time the shipped flags.

**Done (2026-10-31):**

- **Text-measure cache** in `metrics.loam`: direct-mapped, 4096 slots, keyed on
  (string, px, max width, wrap). It is cleared by `bind` / `unbind`,
  `set_font_family` / `load_font`, and the new host trampoline
  `engine_fonts_changed`, which the web loader calls when a late font lands.
  That trampoline also re-lays out the tree, so text measured with the
  fallback face is measured again.
- **The subtree re-solve is deleted:** `find_boundary`, `imposed_w/h`,
  `wraps_under_column`, `snapshot_natural`, the climb loop and `lay_h`, about
  370 lines. A frame whose layout dirt names nodes now solves from the root
  and stays *targeted* for paint: a flat snapshot of every box, extent and
  scroll offset is taken before the solve, and only the nodes that moved are
  paint-marked. Retained-op reuse and small damage rects are unchanged. A
  targeted frame on the 5k tree costs 1.06 ms end to end.
  `zeus.relayout_climbs` stays in the frozen API and is always 0.

**Done (2026-10-31): frame-cost pass.** The goal is a library that never
lags, so the next step was chosen by measuring every interactive path, rather
than by refactor order. Probe: a 2,000-row list of rows with buttons, 10,003
nodes, shipped `-O1` flags, per frame:

| Frame | before | after |
|---|---|---|
| label write (targeted layout) | 5.0 ms | **1.6 ms** |
| pointer move over rows | 0.68 ms | **0.36 ms** |
| wheel scroll | 0.39 ms | **0.28 ms** |
| full repaint | 0.20 ms | 0.20 ms |
| idle | 0.04 ms | 0.04 ms |

Fixes, each found with a sampling profile:

- **Animation-track lookup was O(pool).** `anim_find` scanned all 512 track
  slots, and the chrome pass calls it three times for each hot target (hover,
  press, press scale) on every layout frame: about 3M steps per frame with
  2,000 buttons. A live-track count per node and per signal now answers
  "none" in O(1).
- **Layout memo.** `layout_node` skips a subtree laid out with the same box in
  the same epoch, with nothing invalidated inside (`dirty` bit 2, set by
  `invalidate_path`). The epoch moves with the measure generation, the window
  and the safe insets. A label write at the top of the 5k-node visits probe now
  visits under 2% of the tree. That budget is a test in
  `zeus_layout_visits.loam`. The memo also brings back most of what the
  subtree re-solve saved, without any of its boundary rules.
  `ZEUS_LAYOUT_VERIFY=1` re-runs every layout with the memo off and traps on any
  box that differs. The full suite passes in that mode.
- **Hover syncs only what changed.** A pointer move used to scan every node,
  then mark the whole interaction-state pass for the next frame, which scanned
  every node again. It now queues just the nodes whose hover changed, plus any
  still easing.
- **Hit-pass floater memo.** `floater_collect` walked the whole tree on every
  pointer event to find the few floaters. A subtree found to hold none is now
  stamped with `floater_gen` and skipped. Every layout run and every tree,
  layout or show mark bumps the generation, so the stamp cannot go stale. The
  paint pass keeps the full walk, because it must also see nodes that are
  still fading out.

**Done (2026-10-31): interaction-path pass.** A second probe covers what users
feel as lag beyond scroll and hover. It is a 9k-node screen: a text field, a
dialog, 20 spinners, a 1,000-row `Each` and 1,000 static rows, measured at the
shipped `-O1` flags:

| Path | before | after |
|---|---|---|
| replace all 1,000 `Each` rows | 385 ms | **11 ms** |
| first mount | 376 ms | **75 ms** |
| keystroke in a text field | 7.3 ms | **1.0 ms** |
| append one `Each` row | 3.0 ms | 1.7 ms |
| dialog open / close | 7.3 ms | 2.5 ms |
| resize | 2.3 ms | 1.3 ms |

Fixes:

- **Ownership records are per-owner lists.** The signals and handlers a build
  allocates were one flat list, scanned in full for every freed owner, so
  teardown was quadratic in rows. Each owner's records now form a linked list,
  with an id → record map and a sequence number for marks. Release, adopt,
  `own_end` and re-attribution cost O(that owner's records). The C allocator
  files through `arena.record_sig`.
- **One effect sweep per reconcile.** `free_subtree` swept the whole effect
  table per row. `defer_drops_begin` / `defer_drops_end` now batch the frees of
  a list reconcile, which builds nothing between them. The sweep's `member`
  test is an O(1) stamp. Its per-move rewrite of `parent_eid` / `produced_by`
  is a union-find resolved once, to the same values. `dispose_children` skips
  the scan when a row has no children (`kids_n`). `queue_layout` no longer
  scans its queue.
- **Typing is targeted.** A field's commit wrote a string signal that often
  has no reader. That counted as an unattributed write, so each keystroke
  was an untargeted whole-tree frame. The commit now marks the field itself and
  settles as targeted. It still sets `view_stale`, so a `view` app that read the
  signal untracked rebuilds as before.
- **Text cache 4k → 16k slots.** A 9k-node app has more distinct labels than
  4k slots, so a full re-measure thrashed the cache (792 misses a keystroke
  before typing was targeted).

`tests/compile_pass/zeus_perf_paths.loam` holds the budgets: a keystroke is
never untargeted and touches under 2% of the tree, and a 200-row replace
takes at most 2 effect sweeps with flat record counts.

**Done (2026-10-31): `show` writes are targeted.** Layout reads a `show`
predicate or a bound number straight from its signal, so a write used to mark
the whole tree. `arena.mark_sig_layout_node` now files each such signal
against the nodes that read it. A write queues those nodes and paint-marks
their parents, pruning nodes that no longer read the signal as it goes. It
falls back to the whole tree only when no reader is known, and it still sets
`view_stale`. Dialog open / close: 2.5 → **1.0 ms**, 11 layout visits.
`tests/compile_pass/zeus_show_targeted.loam` toggles a `Show`, a bound number
and a `Dialog`. After each toggle it checks that the targeted frame's draw list
is identical to a forced full repaint.

**Done: moved subtrees are translated, not re-solved.** A subtree called with
the same size and nothing invalidated inside, but at a new position (rows
below a growing sibling), is shifted by the delta (`layout.shift_subtree`).
That is exact unless the subtree holds a floater, which is placed in window
space against its target, so a subtree with one is laid out again. On the
earlier 10k-node frame probe, a label write is now 0.93 ms, hover 0.27 ms and
scroll 0.17 ms.

`ZEUS_LAYOUT_VERIFY` compares boxes before floaters are placed, so floater
placement is covered by the float / select / tooltip goldens and tests
instead.

**Done (2026-10-31): responsive check and host measurement.**

- **Responsive invariance** (`zeus_responsive.loam`): after any resize history,
  boxes equal a fresh build at the same size, and paint equals a full repaint.
  Live drag: 1.3 ms a frame on 9k nodes, 0.34 ms on a 1.9k-node wrap-heavy
  screen, with no text-cache misses across the drag.
- **Mount:** a warm mount is 15 ms for 7.5k nodes. A cold first mount is 40 to
  75 ms, dominated by the host measuring about 3k never-seen strings through
  `sizeWithAttributes:`. A faster CoreText line measure was not adopted: it
  could differ from what `NSString` draws by a sub-pixel, and clip text.
- **Real window** (macOS, `ZEUS_FRAME_DEBUG`): a 2,000-row list scrolling every
  frame with 20 spinners holds 60 fps at 3.8 ms of work a frame. Of that,
  about 3.5 ms is CoreGraphics CPU rasterisation: anti-aliased rounded rects
  and arcs, glyph blits, and the surface clear. The engine is about 0.2 ms.
  Going further is a GPU (Metal) draw path for the host, not engine work.

**Decision: the five-pass rewrite is dropped.** Layout is in budget, and the
memo plus the text cache are what made it fast. Rewriting 1.2k lines of
working solver into five passes would change no frame time and would put
pixel parity at risk across every golden. What remains per frame on large
trees is a few linear walks of about 0.15 ms each per 10k nodes: the
paint-diff snapshot, the overlay / anchor placement walk, and the full chrome
re-sync after a layout. They are the next candidates if a real app shows
them.

### Phase 3: render commands, widgets out of the core

- Turn `scene.loam`'s kind branches into small widget painters in the element
  layer that emit core commands:
  - kind 4 button, kind 6 progress, kind 7 toggle, kind 8 check, kind 13
    slider, kind 10 input;
  - each becomes `Box`es plus a `custom` command where a shape needs it.
- The core's paint becomes a single walk: rectangle + border + text / image,
  scissor for clips, cull, floaters after the page in z order. That is
  one final-layout loop.
- Decide the retained-ops machinery (spans, `patch_frame`, `splice_frame`,
  floater-section reuse, about 700 lines) by measurement:
  - With culling, a frame is tens to low hundreds of commands. Regenerating
    them all each paint frame is the simpler model and probably cheaper than the
    bookkeeping.
  - Keep damage rects, which hosts use to blit less, but compute them by
    diffing the previous and new command lists instead of capturing during
    paint.

**Exit:** the draw goldens match. The core paint walk is under ~400 lines. The
patch and splice code is deleted, or kept only if benchmarks prove it.

### Phase 4: extras as modules

Move each concern behind a small interface the core never calls:

| Extra | Hooks it needs from the core |
|---|---|
| signals / retained tree | writes fields, marks layout or paint dirty |
| animation tracks | writes `look` / transform fields each step |
| scroll physics + scrollbar | owns the `scroll` table; adds a custom command for the thumb |
| focus / keyboard | reads boxes, `pointer` for click-to-focus |
| accessibility mirror | reads the tree, boxes, roles |
| text editing | owns edit slots; paints via a `custom` command |
| virtual lists | reads the scroller's box and scroll before layout |
| devtools | reads everything, writes nothing |

**Exit:** each extra is one file with a short header comment naming its hooks.
Deleting an extra's import removes the feature and nothing else.

### Phase 5: portability

- `zcore` builds on its own: a test program declares a tree and reads render
  commands with no `zeus` import.
- Document the host contract in one page:
  - text measure (string, font, max width) → (w, h);
  - safe-area insets;
  - a wall clock;
  - render commands to replay.
- Optional: a C header that exposes the command list, so a non-Loam host can
  drive it with its own renderer.

**Exit:** `examples/zcore-hello` renders through the existing Canvas2D and
macOS hosts using only `zcore`.

## What stays exactly the same

- Every `Style` key and value constructor (`px`, `pct`, `edges*`, `corners*`,
  `line`, `ratio`, `Float`, `Border`).
- `zeus.App`, `zeus.view`, `Box`, `Text`, `Scroll`, `Overlay`, `Each`, `Show`,
  signals, `effect`, `on_mount`, refs, `anchor`, `float_on`,
  `pointer_through`.
- All `zui` / `zeus-components` widgets and their props.
- Host entry points (`engine_layout`, `engine_paint`, `engine_step_dt`,
  input calls) and the draw-record format hosts replay. Phase 3 may add
  command kinds but never changes existing ones.

## Risks and how each is contained

| Risk | Containment |
|---|---|
| Layout differences of a pixel or two after the pass rewrite | Box goldens per widget and app; the old solver runs beside the new one in tests until they match |
| Whole-tree layout too slow for big apps | Measured before deleting `relayout`; fallback is re-solving from a fixed-size ancestor |
| Dropping retained ops costs CPU on huge, unculled screens | Benchmarked in Phase 3; patching stays if it pays for itself |
| Widget painters drift from today's look | Draw goldens in every state, from Phase 0 |
| A mechanical field rename misses a site | Phase 1 goes through accessors first; the compiler finds every use |
| Hidden coupling between extras | Each extra lands with a test that removes it and checks the rest still works |

## Measures of done

| Metric | Today | Target |
|---|---|---|
| Core engine lines (`zeuscore` minus extras) | ~16k | ≤ 6k |
| Fields per node record | 176 in one struct | ≤ 30 per table, 6 tables |
| Paths that hit-test | 1 (done) | 1 |
| Kind branches in core paint | ~10 | 0 |
| Draw ops for a 2000-row list frame | ~50 (done) | ≤ 60 |
| Full layouts per scroll frame | 0 (done) | 0 |
| Public API removed | — | none |

## Order and rough size

| Phase | Size | Can ship alone |
|---|---|---|
| 0 Safety net | small | yes |
| 1 Split node record | large, mechanical | yes |
| 2 Layout passes | large (done as a frame-cost pass; five-pass rewrite dropped) | yes |
| 3 Render commands | large | yes |
| 4 Extras as modules | medium | yes |
| 5 Portability | small | yes |

Each phase is its own branch and PR, merged only on a green full suite with
the goldens unchanged.
