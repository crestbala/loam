# Zeus reactivity: gap analysis and phased plan

Zeus already reacts: a `set` re-runs exactly the effects that read the signal
(`track.notify`), a prop effect writes one node attribute, and the engine has
per-node `PAINT` bits and a damage list. What it does **not** have is
fine-grained *structure*, *targeted invalidation*, and a *typed, lazy* graph.
This document is the map from the four gaps to the phases that close them, one
commit per phase. Line anchors are for the tree at `feat/zeus-upgrade-v2`.

## The four gaps

### A — Structural granularity
- **Per-node structural operators.** Effects are barred from touching structure
  (`zeusbase.loam:463-471`, `track.loam:1-5`); `child` only appends
  (`zeusbase.loam:455-461`) and there is no insert / remove / move.
- **Change-proportional list updates.** `/`, `Index`, `VirtualList`,
  `VirtualTable`, `Match`, `refit` re-run a build closure; the slow path calls
  `rebuild_begin` → `release_sigs` + `release_fns` + `drop_children` and
  re-creates every row (`atoms.loam:494-544`, `arena.loam:826-840`). The fast
  path only reorders the host's `kids` array (`atoms.loam:500-520`).
- **A framework with no coarse rebuild mode.** `view` / `app` rebuild the tree
  every frame (`zeus.loam:361-371`); `Component(..., rebuild = ...)` re-executes
  builders (`zeusbase.loam:2127-2129`).
- **`Each` is not reactive.** A plain one-shot loop (`atoms.loam:201-206`).

### B — Invalidation & scheduling granularity
- **Targeted per-node invalidation.** Writes set global flags — `paint_dirty` /
  `layout_dirty` / `chrome_dirty` (`arena.loam:943-978`, `2136-2138`,
  `2002-2004`, `1845-1858`). A per-node `PAINT` bit exists, but the write
  channel marks the whole frame.
- **Dependency-driven layout.** Any layout-affecting write re-lays the whole
  tree (`zeus.loam:381-397`).
- **Dependency-driven paint.** A dirty frame pops the whole draw list and
  re-records the visible tree (`scene.loam`); `damage` optimizes the host blit,
  not the traversal.
- **Synchronous, uniform propagation.** `Int` writes notify inline; non-int
  signals go through `loam_track_notify`, bypass `arena.store_sig`, and are
  drained once per frame (`track.loam:22-25`); `batch` defers to a pending list
  (`track.loam:35-38`, `370-406`).

### C — Channel unification
- Three visual channels: prop effects, coarse rebuild, paint-only. Animation
  tracks panic if the tick writes state (`arena.loam:953-959`); theme / accent /
  elevation resolve with "no rebuild and no effect run"; the shared interaction
  overlay is its own pass.
- Reactive tweens are deliberately excluded (paint-only motion).

### D — Graph completeness & typing
- Signals are id-indexed `int` arena slots; `computed` / `computed_str` /
  `computed_bool` are three hand-written variants (`zeusbase.loam:303-330`);
  `Context<T>` requires `Copy` (`zeusbase.loam:158-163`).
- Computeds are eager push memos: an internal effect writes an output signal
  (`zeusbase.loam:303-310`, `track.loam:26-34`).
- Ownership is manual: `effect_owner`, `ui_parents`, `record_intern`,
  `release_sigs` / `release_fns` (`zeusbase.loam:91-149`, `arena.loam:563-624`,
  `830-840`), plus wholesale `track.reset()` in view mode.

## The phases

| Phase | Area | Deliverable | State |
|-------|------|-------------|-------|
| 0 | — | This roadmap | landed |
| 1 | A | Per-node structural operators: `insert_child` / `remove_child` / `move_child` + accessors, with `arena.child_*` primitives that maintain `kids` / `parent` and mark minimal dirt | planned |
| 2 | A | O(changes) keyed list reconciliation in `kfor_refresh`: build only new rows, free only removed rows, move the rest — per-row ownership via `scope_begin` | planned |
| 3 | A | Reactive `Each` over a `Signal<[]T>` built on the phase-1 operators | planned |
| 4 | B | Targeted per-node invalidation: a `signal → node` index so a write marks exactly the nodes whose effects ran, not the whole frame | planned |
| 5 | B | Dependency-driven layout: dirty-subtree re-solve instead of a whole-tree pass | planned |
| 6 | C | One reactive channel: fold the interaction overlay and theme resolution into the effect graph; keep motion paint-only by contract | planned |
| 7 | D | Typed, lazy graph: generic `computed<T>` with pull-on-read, and a nested-computation disposal graph that auto-tears-down on owner re-run | planned |

Phases 1–3 are additive over the current model: existing `For` / `Index` /
`VirtualList` keep working while gaining a per-node fast path. Phases 4–7 change
the engine's scheduling, so each lands behind the existing golden/draw suites.

## Invariants every phase preserves

1. A signal write re-runs exactly the effects that read it, once.
2. An effect writes paint state or an arena field — never structure. Structure
   moves only through the phase-1 operators or a rebuild scope.
3. Ids are recycled: streaming a list must not grow the node / signal tables
   (`zeus_for_recycle`).
4. A node has at most one parent; sibling order is the `kids` array.
5. Goldens and draw goldens hold: the identity path is byte-identical.
