# Zeus reactivity — remaining coarse paths

An audit (2026-10-10) of where Zeus is *not* fine-grained, on top of the phases
already landed in `packages/zeus/docs/reactivity.md` (0–12) and
`docs/zeus_drawbacks.md`. The core is fine-grained: a `set` re-runs only the
effects that read it, prop writes mark one node, layout re-solves from the
nearest size-stable ancestor, paint re-records only marked subtrees, `Each` /
`Match` / `Show` are push-bound hosts, `computed<T>` is lazy. What follows is
every place that still falls back to a coarse path, worst first.

---

## 1. `view` mode rebuilds the whole tree on any layout write

`zeus.loam:371-391`: a `view` / `app` frame rebuilds when `resized`,
`arena.tree_dirty`, `arena.layout_dirty` or `arena.paint_all` is set, and the
rebuild calls `arena.reset_tree()` + `track.reset()` — the whole effect graph
is torn down.

`layout_dirty` is a global set by `queue_layout` (`zeuscore/arena.loam:4201`),
which `mark_layout_path` calls for **every targeted prop write that affects
layout**. So in `view` mode a text change that resizes one label, a `w`/`h`
write, a padding change — all cause a full rebuild + graph reset, even though
the retained (`App`) path would re-solve just that node's boundary.

**Fix:** decouple the rebuild trigger from `layout_dirty`. Rebuild only on
`tree_dirty` that is *not* attributable to a host (a host's structural change
is already scoped by `mark_tree_at`), a resize that the view reads, or an
unattributable `paint_all`. Targeted layout dirt (`dirty_layout_nodes`
non-empty, no frame-wide `mark_layout()`) should go to `layout.relayout` and
keep the retained tree. Alternatively deprecate `view` in favour of `App`.

**Test:** a `view` app with a label whose text is a signal; write it, assert
`track.reset` did not run (effect count / intern count stays flat, a sibling's
local signal keeps its value).

## 2. Unattributable writes fall back to frame-wide marks

`arena.note_write` (`zeuscore/arena.loam:2833`): when effects ran but not all
of them reported a node (`ran > 0 && pw < ran`), the write calls
`mark_paint()` + `mark_chrome()` (whole-frame paint / chrome), and in `view`
mode that is also a rebuild (`paint_all`).

**Fix:** find which effects are unattributed in practice (add a dev counter
keyed by effect id / creation site), and make them report a node: every
`zeus` / `zui` effect that touches a node should go through a reporting
setter. For user effects that touch no node (pure side effects, signal
forwarding), treat "touched nothing" as *no dirt* rather than frame-wide
dirt — only an effect that wrote arena state without reporting should
fall back.

## 3. `Each` replaces a changed row instead of patching it

`docs/zeus_drawbacks.md` §2: row identity is the item value. An item that
keeps its key but changes a field is a *new* value, so its row is torn down
and rebuilt — local signals, focus inside the row, and scroll reset.

**Fix:** split identity from content. Options:
- an optional `key` function on `Each`; when the key holds and the value
  changed, write the new value into a per-row `Signal<T>` the row was built
  with (`build(i, item: Signal<T>)` or an `EachSig` variant), so only the
  effects reading changed fields re-run;
- or per-field derived signals (Solid store style) — larger change.

Also: `i` is the build-time position; give rows a reactive index signal.

## 4. `Match` rebuilds the whole arm

`Match` tears the arm down and rebuilds it every time `on` moves, even if it
lands on the same arm with a different value. `MatchKeep` keeps arms alive
but needs a key function.

**Fix:** when the arm (discriminant) is unchanged, don't rebuild — pass the
arm a signal of the payload. Rebuild only on an arm change.

## 5. Geometry hosts are polled, and charts rebuild on width change

Refit / windowed hosts (charts, `VirtualList`, `VirtualTable`) are polled
from a registry each layout rather than pushed. A refit host (chart) re-runs
its build closure — drop children, rebuild — whenever its width moves.

**Fix:** push geometry: when `relayout` changes a registered host's box,
queue that host (like `host_queue` does for signals) instead of polling the
registry. For charts, expose width as a signal to the chart body so the paths
(SVG / draw ops) update through effects instead of a rebuild.

## 6. Whole-tree fallbacks in layout and paint

- Layout: `relayout` verifies the boundary box; when it did not hold, it
  falls back to a whole-tree solve.
- Paint: when a node's draw-op run changes length the frame does a full
  record (`reactivity.md` phase 10).
- Floaters: the floater section is always re-recorded whole.

**Fix:**
- layout: on a failed verify, retry from the next ancestor up instead of the
  root (walk up until a box holds).
- paint: make the retained list splice-able (a gap buffer or per-node op
  chunks with an index) so a run that grows or shrinks shifts only its
  successors, not a full record.
- floaters: give each floater its own run keyed by node, re-record only those
  whose trigger rect or content changed.

Add counters (`full_layouts`, `full_records`) and assert they stay at zero in
the existing patch tests, so fallbacks are visible.

## 7. App-level generation counters

`examples/zeus/dracula` keeps plain module globals and bumps a
`Signal<int>` to force `Match(gen, ...)` to rebuild
(`components/stocks.loam`, `routes/backtest/page.loam`). That's a coarse
rebuild in the user's own code, because the state isn't in signals.

**Fix:** port that state to signals / `computed<T>`, and treat this example
as the test that items 3–5 are enough to make it unnecessary.

---

## Status (2026-10-10)

| § | State | Where |
|---|-------|-------|
| 1 | done — `arena.view_stale`; resize rebuilds only when the build read `window_size()`; `set_scale` marks the view stale | `zeus.loam` `engine_layout`, test `zeus_view_mode` |
| 2 | done — chaining setters report from effects (`touched`), silent runs leave no dirt, `batch` settles after flush, direct field writes in `install_behaviour` / `progress_tween` report; counters `silent_runs`, `silent_runs_by_effect`, `effect_owner_node`, `frame_wide_writes` | `arena.note_write` / `settle_write`, test `zeus_unattributed` |
| 3 | done — `EachKeyed(each, key, build(index: Signal<int>, item: Signal<T>))`; plain `Each` still replaces a changed row (identity = value) | `flow.loam`, test `zeus_each_keyed` |
| 4 | done — `MatchBy(on, arm, build(payload: Signal<T>))` | `flow.loam`, test `zeus_match_by` |
| 5 | done — `arena.host_geo` gates polled geometry hooks on box / scroll / bound writes; `width_signal`; Bar / Line / Area charts follow width through effects (no refit) | test `zeus_geo_hosts`, `zeus_chart_responsive` |
| 6 | done — layout climbs on a failed verify (the root itself is still never a boundary, so a change under a root-level box still solves the whole tree); paint splices length changes; per-floater runs; counters | tests `zeus_paint_splice` |
| 7 | open — the generation counters mostly swap chart data, and the charts take arrays at build time; needs a chart API over `Signal` data before the port removes the rebuilds | — |

## Order

1. §1 `view` rebuild on layout dirt — biggest perf cliff, smallest change.
2. §2 unattributed writes — add the counter first, then fix the sources.
3. §3 `Each` in-place row update.
4. §4 `Match` same-arm update.
5. §5 push geometry hosts / reactive chart width.
6. §6 fallbacks (counters first, then incremental fixes).
7. §7 port dracula once 3–5 land.

Each step lands behind the existing golden / draw suites (invariants in
`packages/zeus/docs/reactivity.md` §"Invariants every phase preserves").
