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
- **A framework with no coarse rebuild mode.** `view` / `app` rebuilt the tree
  (and `track.reset()` the graph) every frame (`zeus.loam:367-378`), and
  `Component(..., rebuild = ...)` re-executed builders. **Phase 11:** a `view` app
  rebuilds only on a frame that can have changed the tree — a structural or layout
  frame, or a resize. A frame whose dirt is attributable paint keeps the retained
  tree, so the effects the previous build created update their own nodes and the
  graph is not reset. The list hosts had already grown generation guards
  (`virtual_refresh`, `match_refresh`); the keyed list had not, so it re-derived
  every row key — with an O(rows^2) uniqueness pass — on every layout. It now
  guards on the list signal's generation, which makes an untouched list O(1) per
  layout. `Each` and `For` were already one keyed engine (phase 3). What remains
  coarse by design: a write the engine cannot attribute still rebuilds, and a
  structural change still rebuilds the host it lands in.
- **`Each` is not reactive.** It was a plain one-shot loop (`atoms.loam:201-206`).
  Phase 3 makes `Each` the reactive keyed list and moves the static loop to
  `Loop`. A positional list cannot reconcile correctly without value equality
  (Loam rejects `==` on a generic `T`: "cannot compare T"), so identity comes
  from a key — the same contract as `For`.

### B — Invalidation & scheduling granularity
- **Targeted per-node invalidation.** Writes set global flags — `paint_dirty` /
  `layout_dirty` / `chrome_dirty` (`arena.loam:943-978`, `2136-2138`,
  `2002-2004`, `1845-1858`). A per-node `PAINT` bit exists, but the write
  channel marks the whole frame. **Phase 4:** the prop setters report the node
  they wrote and the write marks exactly those nodes when every effect it ran
  reported one; a paint-only property skips layout and damages only its node.
  Unattributable effects fall back to the frame-wide mark.
- **Dependency-driven layout.** Any layout-affecting write re-lays the whole
  tree (`zeus.loam:381-397`). **Phase 5:** a measure cache invalidated only along
  the written node's path, and a re-solve of the nearest size-stable ancestor's
  subtree. It falls back to a whole-tree solve when no such ancestor exists, a
  write sits inside a scroller, or a floater is anchored.
- **Dependency-driven paint.** A dirty frame popped the whole draw list and
  re-recorded the visible tree; `damage` optimized the host blit, not the
  traversal. **Phase 10:** the retained list gains a per-node run — everything
  `paint_node(id)` emits is contiguous, because a node's ops bracket its
  children — and `mark_paint_id` marks the node's ancestors too. A frame whose
  dirt is attributable (not a `mark_paint` global, not a structural change) walks
  the tree and re-records only the marked subtrees, reusing every other run; a
  node with only a descendant marked keeps its own ops. Resolution — theme
  colors, text, shadow packing, transforms — is what the reuse skips. When a run
  changes length the frame falls back to a full record, so the worst case is the
  old behaviour. A frame with any visible floater still records in full: a
  floater's ops follow its trigger's screen rect, not only its own subtree, and
  the floaters pass is a second traversal.
- **Synchronous, uniform propagation.** `Int` writes notify inline; non-int
  signals go through `loam_track_notify`, bypass `arena.store_sig`, and are
  drained once per frame (`track.loam:22-25`); `batch` defers to a pending list
  (`track.loam:35-38`, `370-406`). **Phase 9:** one write channel. The accounting
  `store_sig` did after binding an int — count the effects that ran and the nodes
  they reported, then choose targeted marks or a frame-wide fallback — moves into
  `arena.note_write`, which generated code calls by name after it binds any other
  value. A string or struct prop write is now attributed exactly like an int one;
  only a write with no node to attribute still falls back to a frame mark.

### C — Channel unification
- Three visual channels: prop effects, coarse rebuild, paint-only. Animation
  tracks panic if the tick writes state (`arena.loam:953-959`); theme / accent /
  elevation resolve with "no rebuild and no effect run"; the shared interaction
  overlay is its own pass. **Phase 6:** prop effects and the interaction-state
  flags share one node-scoped `invalidate(id, kind)`; the overlay's chrome pass
  is node-scoped (a `signal -> bound node` index covers the tween a bind drives
  with no effect). Theme resolution and motion stay deliberately out: theme is an
  in-place slot recolor, motion is a paint-only overlay by contract.
- Reactive tweens are deliberately excluded (paint-only motion).

### D — Graph completeness & typing
- Signals are id-indexed `int` arena slots; `computed` / `computed_str` /
  `computed_bool` were three hand-written variants (`zeusbase.loam:303-330`) because
  the runtime interns and invokes only four closure kinds (`plat_intern_fn` / `_int`
  / `_bool` / `_str` and the matching `plat_invoke_*`) and Loam has no generic
  persistent closure table or generic invoke. **Phase 7b:** one generic
  `computed<T>`. The body is a closure, and a closure can only be persisted through
  an environment that is a plain int copy — so the body is stored in a *signal
  cell* (whose payload is `sizeof(T)` typed bytes, monomorphized by the compiler)
  and rehydrated inside a non-capturing void thunk that is interned the ordinary
  way. No compiler or runtime hook is needed, and `computed_str` / `computed_bool`
  are now thin wrappers. `Context<T>` still requires `Copy`, as `Signal<T>` does.
- Computeds were eager push memos: an internal effect wrote an output signal
  (`zeusbase.loam:303-310`, `track.loam:26-34`). **Phase 7b:** a memo is a *lazy*
  node. A write marks it dirty and queues its readers; it recomputes when something
  reads it (`get` / `peek`) or when the flush reaches one of its readers, and only
  if an input's recorded generation actually moved — the check-clean pass, over
  `plat_sig_gen`. A reader whose inputs turn out not to have moved is skipped, so
  the change gate the eager form got from `set` is preserved, and a memo nothing
  observes is never recomputed at all.
- Ownership is manual: `effect_owner`, `ui_parents`, `record_intern`,
  `release_sigs` / `release_fns` (`zeusbase.loam:91-149`, `arena.loam:563-624`,
  `830-840`), plus wholesale `track.reset()` in view mode. **Phase 7a:** a
  nested-computation disposal graph: `alloc` records the computation whose body
  created an effect, and a `scope` (`scope_eid`) disposes its children before it
  re-runs, recycling their rows. A plain `effect` keeps its children (owned by
  the surrounding node), so nothing existing changes. Phase 7b extends the same
  owner to the arena for `computed<T>`'s cells. **Phase 8:** `arena.owner_now()` is
  the single rule — the innermost live `scope` as a negative id, else the host
  rebuild scope — and `signal<T>` and the `intern_*` helpers record under it, so a
  `scope` owns everything its body creates. A scope's children are disposed
  *before* its cells are released, so a child's `on_cleanup` still has a handler
  to invoke.

## The phases

| Phase | Area | Deliverable | State |
|-------|------|-------------|-------|
| 0 | — | This roadmap | landed |
| 1 | A | Per-node structural operators: `insert_child` / `remove_child` / `move_child` + accessors, with `arena.child_*` primitives that maintain `kids` / `parent` and mark minimal dirt | landed |
| 2 | A | O(changes) keyed list reconciliation in `kfor_refresh`: build only new rows, free only removed rows, move the rest — per-row ownership via `scope_begin` | landed |
| 3 | A | Reactive `Each` over a `Signal<[]T>`: the keyed engine exposed as the general list primitive, with the static one-shot loop renamed `Loop` | landed |
| 4 | B | Targeted per-node invalidation: prop setters report their node; a write marks exactly those nodes (and skips layout) when every effect it ran reported one, else the frame-wide mark | landed |
| 5 | B | Dependency-driven layout: a measure cache invalidated along the written path, and a subtree re-solve from the nearest size-stable ancestor (with a whole-tree fallback) | landed |
| 6 | C | One invalidation channel: a node-scoped `invalidate(id, kind)` used by prop effects and the interaction-state flags, plus a `signal -> bound node` index so a write syncs exactly the bound nodes' chrome (a full pass only for pointer / scroll / layout / tree events) | landed |
| 7 | D | Typed, lazy graph: generic `computed<T>` with pull-on-read, and a nested-computation disposal graph that auto-tears-down on owner re-run | landed |
| 7a | D | Disposal graph: `scope` / `scope_eid` — a re-run disposes the computations its body created, recycling their effect rows and intern ids; a plain `effect` is unchanged | landed |
| 7b | D | Typed, lazy graph: generic `computed<T>` (the body persists in a signal cell, so no runtime hook), and a lazy memo — dirty on write, recomputed on read, skipped when no input generation moved, and recycled with its scope | landed |
| 8 | D | Ownership below the graph: a `scope` owns the signals and interned handlers its body creates (`arena.owner_now`, one record per id), so a re-running scope recycles them; plus `zeus.intern_count()` so a test can hold the handler table flat | landed |
| 9 | B | One write channel: `arena.note_write` is the accounting every state write shares, so a non-`int` write is as targeted as an `int` one instead of draining a frame mark | landed |
| 10 | B | Dependency-driven paint traversal: per-node draw-op runs, an ancestor paint bit, and a patch path that re-records only the dirty subtrees — with a full record as the fallback when a run's length changes | landed |
| 11 | A | One model: a `view` app rebuilds only when the tree can have changed, so non-structural frames keep the retained tree and its live effect graph; and the keyed list guards on its signal generation, making an untouched list O(1) per layout | landed |

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
