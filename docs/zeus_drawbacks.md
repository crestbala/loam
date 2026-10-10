# Zeus + zeus-components — current drawbacks

An audit of the code as it stands (2026-10-09, updated the same day), not of
the plan. Part 2 of
`docs/loam_zeus_v2.md` is partly out of date: several of its drawbacks have
since shipped. This file lists what is fixed, then what is still wrong, with
the code that shows it.

---

## Already addressed (no longer drawbacks)

| v2 item | Where it landed |
|---|---|
| 2.1 Manual subtree rebuilds | `Each` / `Show` / `Match` in `zeus-components/std/atoms.loam`; no component calls `drop_children` or `on_restyle` |
| 2.2 No error boundary | `zui.Boundary` (`atoms.loam:898`), arena `mark` / `rollback`; every route builds under it (`router.loam:513`) |
| 2.3 Accessibility | Semantic-tree dump (`zeus.loam:931`), consumed by the web loader |
| 2.4 No devtools | `std:devtools` — the inspector is itself a Zeus app |
| 2.5 No hot reload | Signal-arena save / restore (`zeus.loam:1098`, `hosts/web/wasm.c:467`) |
| 2.8 No animation system | Paint-only tracks (`animate_scale` / `_x` / `_y` / `_opacity` / ...) with `EASE.Spring` |
| 2.9 Layout limits | Grid and wrapping rows in `zeuscore/layout.loam` |
| 1.3 Closure env leak | Signals and handlers are adopted by their row / node (`arena.adopt_records`) |
| 5.x Router | `std:router` — typed params, history, loaders |
| §1 below: keyed lists | Per-host hash table for keys (`arena.key_*`, O(1) lookup / count), O(n) uniqueness (`keys_unique`), push-based refresh (`host_push` / `host_queue`; only geometry hosts are polled). `tests/compile_pass/zeus_for_scale.loam` |
| §2 below: frozen rows | One `zui.Each(items, build)` (`For` / `ForLive` / `Index` removed): takes an array or a signal read, adds no wrapper node, moves a row whose value stays and replaces a row whose value changed. `zeus_for_edit.loam` |
| §3 below: list / match edges | Multi-root rows are wrapped in an implicit column (one-time warning) instead of forcing full rebuilds; `zui.MatchKeep` keeps arms alive. `zeus_list_edges.loam` |
| §4 below: Boundary | Handler / effect / list-refresh traps are caught on every host (`arena.invoke_on`); on wasm the loader recovers (`zeus_recover`) and re-instantiates for startup traps. `zeus_boundary_runtime.loam`, `hosts/web/wasm_trap.ts` |
| §5 below: stale captures | Compiler **error** for a `let mut` written (assigned, `push` / `pop`, or passed as `&mut`) after a closure copied it. `tests/compile_fail/stale_capture.loam` |
| §7 below: layering | Control-flow primitives in `zeus:flow`; `zeus-components` imports no `zeuscore/*` (`nob` arena lint) |
| §9 below: presence | `exit_fade` / `exit_slide` / `exit_scale`; `Each`, `Match` and lazy `Show` keep a removed node pinned and inert while it exits, `MatchKeep` plays it as an arm hides; the fade covers the subtree. `zeus_presence.loam`, `zeus_presence_keep.loam` |
| §12 below: scoped theme | `zui.Scope(dark = …, density = …, accent = …, palette = …)`: paint-time scheme and role palette per subtree, density resolved per node. `zeus_theme_scope.loam` |

---

## 1. Keyed lists are O(n²) and polled every frame

- `kfor_unique` (`atoms.loam:584`) is a pairwise scan that calls the user's
  `key()` closure twice per pair: O(n²) closure calls.
- `key_index` (`zeuscore/arena.loam:889`) scans one **global** parallel array
  (`keyed_host` / `keyed_key`) holding the keys of every list in the app, so
  each lookup is O(total keys). The move fast path performs one lookup per row,
  twice.
- `refresh_lists` (`zeus.loam:318`) walks every arena node on every layout and
  invokes each `list_fn`. List refresh is polled, not pushed — the opposite of
  the fine-grained-signals pitch.

**Impact:** visible on any list past ~1k rows, and with several lists on
screen the global key table makes each one slower.

**Fix:** a per-host hash map (key → node); a hash set for uniqueness; refresh
lists only from their bound signal's write instead of a per-frame sweep.

**Status: done.** One open-addressed `(host, key)` table with per-host chains;
`Each` / `Match` are push hosts queued by `hosts_for_signal`
(and by a stale `computed` they read); refit / windowed hosts are polled from a
registry instead of an all-nodes walk. 2×2000-row reorder test: 0.69 s → 0.11 s.

## 2. `Each` rows freeze the item they were built with

`build(i, item)` runs once, at mount. When an item keeps its key but its
fields change, the row keeps showing the old values. The doc comment says to
"read a signal inside the row instead of `i`", which means one signal per item.

**Impact:** the most common data-table update (edit a row in place) silently
does nothing. This is Solid's trade-off; React users will not expect it.

**Fix:** either re-run a row-scoped `update(item)` when the item at a key
changes (compare by value), or pass the row a derived `Signal<T>` for its slot.

**Status: done.** `Each(items, build)` is the only list primitive (`For`, `ForLive`, and `Index` are gone). A row's identity is the item value: an unchanged value is moved, a changed value replaces that row, so its own signals start over (React keeps them; Zeus rows are
not re-run). `i` is the position the row was built at. Note: an
index-assign into `sig.get()` writes through to the cell's buffer (copy-on-write
covers `push` only), so the following `set` sees no change — build a new array.

## 3. List and match edge cases fail silently

- Items must be Copy (`structs.loam:22`): no rows that own `[]U` or `Box<T>`.
- A row builder that mounts zero or several top-level nodes makes the keyed
  path fall back to a **full rebuild** on every change — losing focus, scroll,
  and row state — with no warning.
- `Match` (`atoms.loam:853`) tears down and rebuilds the whole arm whenever
  `on` moves, so signals declared inside an arm reset on every switch.

**Fix:** a dev-build diagnostic for multi-root rows; wrap rows in an implicit
host; an opt-in `keep_alive` for `Match` arms.

**Status: done** except the Copy restriction (a language rule). The wrap warns
once at run time (there is no dev-build flag). `MatchKeep` names arms by a key
function because a type parameter has no `Eq`.

## 4. `Boundary` does not work on wasm, and catches build only

From `atoms.loam:891-897`: a trap outside `build` (handlers, effects, layout)
is not caught, and "on wasm there is no `setjmp`, so a trap still aborts."

**Impact:** the web is the primary target, so on the platform that matters
most a bad index still kills the app. Even on native, most runtime bugs live
in handlers and effects, which are not covered.

**Fix:** on wasm, run guarded code through the exception-handling proposal or
a JS-side trampoline that reinstantiates state; on all hosts, route handler
and effect invocation through the nearest enclosing boundary.

**Status: done.** Remaining limit on wasm: a run-time trap loses the rest of the
interrupted call (the event, or the enclosing build after the boundary) rather
than resuming it; only startup traps are retried, by re-instantiation. Native
hosts other than the web loader need nothing (setjmp).

## 5. Stale captures still have no diagnostic

Closures still capture by copy and the env is a snapshot. The leak half of
v2 §1.3 is fixed; the stale-value half is not. Nothing in `packages/loam/src`
warns when a closure captures a non-handle value that is later mutated in the
enclosing scope. §4.3's "a React bug reintroduced" still stands.

**Fix:** the compiler warning v2 already specified (capture-by-copy of a
binding that is mutated after the capture point), then make it an error.

**Status: done.** An error, not a warning (once per binding): assignments,
`push` / `pop`, and `&mut x` taken after the capture. A write inside a
closure to its own copy is not reported. Zero hits in the tree.

## 6. Text is half-solved

- `std:font` is unshaped (`packages/loam/std/font.loam:259`: "no GSUB/GPOS").
  No ligatures, no mark positioning, no bidi / RTL, no Arabic or Indic scripts.
- Measurement is in-tree but drawing is still the host's (`metrics.loam`
  header): "a bound font can differ slightly from what the host draws." That
  produces clipping and wrap differences per host.
- No IME / composition support was found: CJK input is likely broken.

**Fix:** finish Phase 13 — move glyph drawing in-tree so measure and draw use
one source, then shaping, then bidi, then platform IME bridges.

## 7. The component layer reaches into engine internals

The README says components depend on `zeusbase` only. In practice `atoms`,
`molecules`, `organisms`, and `colors` all import `zeus:zeuscore/arena` and
`zeus:zeuscore/platform`; `atoms.loam` has 156 direct uses, including writing
`arena.nodes[...]` fields.

**Impact:**
- The "almost never changes / constantly changes" layering is nominal: an
  engine refactor breaks the design system.
- `Each`, `Match`, `Boundary`, `Show`, and `scroll_to` are framework
  primitives living in the design-system package.
- A third-party component library cannot be built at the same level, because
  the public API is not sufficient to write these.

**Fix:** move the control-flow primitives into `zeusbase` (or `zeuscore`),
expose what's left through a small public API, and forbid `zeuscore/*`
imports from `zeus-components` with a build check.

**Status: done.** `packages/zeus/std/flow.loam` (`zeus:flow`, beside
`zeusbase`) holds `Each`, `Match` / `MatchKeep`, `Show`, `Boundary`, `Scope`,
`VirtualList*`, `refit`, `scroll_to*`, `animate`, `pick_image` and the list
bookkeeping. `atoms` imports it, so `zui.Each` still resolves (re-export
reaches two import levels). What components still needed is public API:
`measured_height`, `progress_tween`, `scoped_density_sid`, `paint_rgb`, and
zeusbase's `left` / `top`. `atoms` / `molecules` / `organisms` / `colors`
import no `zeuscore/*`, and the `nob` arena lint fails on any such import or
any `arena.` / `platform.` / `geometry.` in the package. `atoms` is down from
4.2k to 3.1k lines. The facade (`zeus.loam`) still imports
`zeus-components:colors` for the role palette, which is the README's stated
direction (components ← facade).

## 8. Everything is still `int`

Opacity is 0..100, scale is percent, translation is integer dp, and `Node`
is an `int` id over an arena of mostly 64-bit fields. Sub-pixel motion and
smooth transforms are limited by integer steps, and v2 Part 3's cache-line
argument still applies.

## 9. No exit / presence transitions

Animations act on live nodes, but nothing keeps a node alive through an exit
animation when `Show`, `Match`, or `Each` removes it. Dialogs, toasts,
drawers, and list removals cannot animate out without manual delay hacks.

**Fix:** a `Presence` contract on the control-flow primitives — removal marks
the subtree "exiting", runs its exit track, frees on completion — designed
together with item 3.

**Status: done** for `Each`, `Match`, lazy `Show` and `MatchKeep` (the
leaving arm plays its exit pinned and alive, then hides with its layout and
opacity put back; switching back mid-exit snaps it in). The exit fade now
covers the node's subtree (it used to fade only the root's own fill, so a
row's labels stayed solid). `Dialog`, `Drawer` and `Toast` already fade out
through their `Enter.Fade` panel, with their scale / slide under it; that is
now covered by `zeus_presence_keep.loam`. Not wired: non-lazy `Show`, which
snaps by design (layout has already collapsed an in-flow node). An exiting
node is pinned out of flow, so siblings collapse at once (no height
animation).

## 10. Accessibility is data, not behavior

The semantic tree is serialized and the web loader consumes it. Not verified:

- native wiring to `NSAccessibility` / UIAccessibility / `AccessibilityNodeInfo`;
- live regions (toasts, async status) and announcements;
- focus trapping and restoration in `Dialog` / `Drawer` / `Sheet`;
- `aria-expanded` / `aria-selected` / value semantics for `Accordion`,
  `Tabs`, `Slider`, `Combobox`.

Until these are tested with a real screen reader, the legal-blocker concern
from v2 §2.3 is reduced, not closed.

## 11. A large component kit with almost no tests

~9.7k lines and 100+ widgets across three files (`atoms` 4k, `molecules`
3.2k, `organisms` 2.5k). `packages/zeus/tests` holds one golden file; coverage
is "the examples compile." Keyboard behavior in `Combobox`, `Menu`,
`DatePicker`, `Table`, and the roving-focus groups has no direct test, so
every restyle is verified by eye.

**Fix:** headless snapshot tests per widget (tree + layout boxes + a11y dump),
plus scripted key sequences for every interactive control.

## 12. Theming and density are global

`density()` and the role colors are app-wide signals (`colors.loam`). There is
no scoped theme: a dark sidebar in a light app, or a compact table on a
comfortable page, cannot be expressed.

**Fix:** a `Theme` / `Density` provider node whose values cascade to its
subtree, read through context rather than globals.

**Status: done.** `zui.Scope(accent = ACCENT.Rose)` repaints the accent ramp
and the roles on it (`accent()`, `accent_soft()`, `on_accent()`, `on_ink()`)
for the subtree. `Scope(palette = p)` takes any role table
(`palette_new` / `palette_role`). Knobs nest independently: a dark scope inside
an accent scope keeps the accent. The palette id rides in the node's existing
`scheme` field, so nodes grow no new field.

## 13. File size and organisation

Single files of 3–6k lines (`atoms.loam`, `molecules.loam`, `arena.loam`
at 5.8k, `image.loam`, `zeusbase.loam`). Loam has no re-export, so `zui`
can only be a facade by importing the tiers, and splitting a tier into
per-widget files means more modules that every caller must reach through.
Navigation, review, and compile granularity all suffer.

---

## Suggested order

1. **Lists (1, 2, 3):** every data-heavy app hits these first.
2. **Silent failures (4, 5):** a boundary on wasm, the stale-capture warning.
3. **Layering (7):** before the component API surface grows further.
4. **Text (6) and presence (9):** text blocks i18n; presence pairs with 3.
5. **Tests (11), a11y verification (10), scoped themes (12).**
