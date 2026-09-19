# Zeus design audit

> **Status.** Phases 1–3 and Phase 4 batches 1–4 are landed — see §11 for what
> changed, what is still open, and the two traps that cost real time. Sections
> 1–10 are the *original* baseline and are left as written except where marked
> **[CLOSED — phase 2]** or **[CLOSED — phase 4]**, so the record of what the
> kit looked like before the upgrade stays readable.

Baseline for the design-system upgrade. Everything below is read off the tree at
`feat/zeus-ui-upgrade` (`8a61fc3`): `packages/zeus/std/zeus.loam` (6592 lines),
`packages/zeus/std/zeuscore/{arena,layout,scene,input,track,metrics,geometry,platform}.loam`,
the four hosts, `examples/zeus/{gallery,dashboard}`, and the eight
`packages/loam/tests/draw_golden` fixtures.

The architecture is sound and better than the brief assumes in one respect:
**theme roles are already resolved at paint time**, so a light/dark switch
repaints a retained tree with zero effect runs and zero rebuilds
(`arena.paint_rgb` / `zeus.role_token`). That mechanism is the model the rest of
this work should copy. The gaps are elsewhere: the paint primitives are too few
to express depth, the token set is a flat role list rather than scales, there is
no interaction-state model, and **animation is wired straight into the reactive
graph**, which §3.2 of the brief forbids outright.

---

## 1. Component inventory

Present (`fn` in `zeus.loam`), with what each actually paints today.

| Component | States it has | Why it reads plain |
|---|---|---|
| `Button` / `btn_look` | hover wash, press darken, focus ring, `enabled` | 6 looks × 3 sizes all share `RAD_MD` and a flat fill. No elevation, no gradient, no border on Solid, no loading state, no icon slot, no disabled *styling* (only a flag). Press is an alpha-black overlay, not a scale. |
| `IconButton` | same as Button | Fixed square; no tooltip pairing; no 44dp floor on touch. |
| `Card` | none | One flat `plate()` fill + 1dp `rule()` border + `RAD_XL`. No elevation, no hover lift, no header/footer/media slots, no interactive variant. |
| `Comp` | none | Raw styled box, correct as a primitive. |
| `Dialog` / `AlertDialog` | `enter_fade` (opacity only) | Scrim is `Overlay` at opacity 80 with **no fade** — it pops. Panel fades but never scales in. Fixed `width = 440` (no responsive/small-screen path). No focus trap, no Esc, no edge-awareness. Outside-click works. |
| `Tabs` / `Tab` / `Segmented` / `Choice` | selected via `selected_chrome` | Selection is an instant background swap on the trigger. **No indicator element at all**, so §3.5's "tabs indicator slide" has nothing to animate. No arrow-key navigation. |
| `Navbar` / `NavTab` | selected pill | Fine structurally; no elevation on scroll, no mobile collapse. |
| `TextInput` / `TextArea` / `Field` / `Input` | focus ring, caret, selection highlight | Caret is a 1px rect that **never blinks**. Selection highlight is a hardcoded `11053224` literal, not a theme role — wrong in dark. Placeholder exists via `raw_input`. No error/invalid state, no help text, no prefix/suffix slots, no character count. **[clear button: batch 17]** |
| `Select` / `SelectRow` | open/closed via `visible`, `enter_fade` | Menu is absolutely positioned at a hardcoded `top = SELECT_DROP (42)`, `width = SELECT_W (200)`, `z_index = 10`. **Not edge-aware** — near the viewport bottom it clips. No keyboard navigation, no typeahead, no Esc. |
| `Checkbox` (kind 8) | `tween` 0..100 on the check mark | Tween is frame-count stepped `±10`/frame in `scene.step`. No indeterminate state, no invalid state. |
| `Radio` / `RadioGroup` | selected dot | Dot swaps instantly (`selected_chrome`), no scale-in. No arrow-key roving focus. |
| `Switch` (kind 7) | `tween` 0..100 thumb travel | Linear `±9`/frame. No spring. Thumb is a plain circle, no shadow. |
| `Slider` (kind 13) | drag, arrow keys (`keys()`) | Knob has no shadow, no hover grow, no focus ring of its own, no value tooltip, no ticks, no range variant. |
| `Progress` (kind 6) / `Meter` | bound value | Instant jump on value change; no easing, no indeterminate mode. |
| `Badge` / `Chip` / `Kbd` | none | Flat pills. 6 `TINT`s. No dot/icon slot, no removable chip, no count variant. |
| `Alert` / `AlertDestructive` / `AlertWarning` | none | Three fixed tiers; no `info`/`success` tier, no dismiss, no action slot. |
| `Table*` (`TableHead`/`TableRow`/`TableCols`/`VirtualTable`) | none | Cells are `Text` with `grow = 1`. No zebra, no row hover, no sticky header, no sort affordance, no per-cell alignment, **no truncation** — long cells blow the column out. |
| `DatePicker` / `CalGrid` / `CalCell` | selected, open | Works; no range select, no today marker styling, no keyboard grid nav, no month slide. |
| `BarChart` / `LineChart` / `AreaChart` | `refit` on width change | Built from rects/svg. No gridlines, no axis labels, no hover crosshair, no tooltip, no animated entry, no legend. |
| `Skeleton` | `pulse` | Opacity sawtooth from a **global** `arena.pulse_t` frame counter mod 144 — a flicker, not a directional shimmer. |
| `Avatar` / `User` | none | Initials or `src`; no status dot, no group/stack, no fallback ring. |
| `Stat` / `StatCount` | none | No delta arrow, no sparkline, no trend color. |
| `Breadcrumbs` / `Pagination` | selected page | Pagination renders every page number — no ellipsis for large counts. |
| `Toggle` / `ToggleGroup` | pressed | Instant swap. |
| `Separator` / `Divider` / `Icon` / `Pip` / `Row` / `Column` / `Grow` | — | Primitives, fine. |

**Absent entirely** (grep returns zero definitions): `Menu`/`DropdownMenu`,
`Drawer`/`Sheet`, `Collapsible`, `Combobox`. `Spinner`/`Loader`, `Empty`,
`Accordion`, and the overlay family (`Popover` / `Tooltip` / `HoverCard` /
`Toast`) landed in phase 4, built on the clock-driven indicator, the paint-only
transform set, the single-signal single-open model, and anchored floaters.

**Icon set** is 10 constants (`IC_CHECK`, `IC_X`, `IC_CHEV_DOWN/LEFT/RIGHT`,
`IC_CALENDAR`, `IC_INFO`, `IC_ALERT_CIRCLE`, `IC_PLUS`, `IC_MINUS`), stored as
**SVG markup strings** parsed by a host-side reader that understands only
`path` / `circle` / `currentColor` / a few stroke attributes. They are not
Loam path data, and there is no `search`, `chevron-up`, `warning-triangle`,
`loader`, `eye`, `trash`, `copy`, `external-link`, `arrow-*`, or `more`.

---

## 2. Paint primitives

Twelve scene op kinds (`scene.loam`) over fourteen host ops
(`platform.loam` / `ZeusDraw` in `packages/loam/runtime/zeus_rt.h`).

**What exists:** `fill(rect, uniform radius)`, `fill_a(+alpha)`,
`fill_g(2-stop linear, axis 0=vertical | 1=horizontal)`, `text`, `text_int`,
`text_wrap`, `text_rot(deg)`, `svg(markup)`, `image(src, radius, fit)`,
`save` / `clip(rect)` / `restore`.

### Missing or weak

| Primitive | Status | Consequence |
|---|---|---|
| **Soft shadow** | **[CLOSED — phase 2 op; paint emits since phase 4 batch 4]** `plat_shadow`, `Box` / `Card.elevation` | No elevation is expressible at all. Cards, dialogs, popovers, switch thumbs, and menus are flat by construction. This is the single biggest reason the kit reads plain. Requires a new host op on all four backends. |
| **Per-corner radius** | **[CLOSED — phase 2 op; paint emits since batch 29]** `plat_fill4`, `Box.radius_top` / `radius_bottom`, `radius4` | No top-only rounded sheets, no tab-shaped triggers, no grouped button runs, no rounded-top table headers. Batch 29 gave `UiNode` four `u16` corner overrides (`radius` stays the uniform fallback), made `paint_fill_box` emit `plat_fill4` only when the resolved corners differ, and put the sheet shape on `Drawer`. |
| **Anti-aliased strokes** | **[CLOSED — phase 2 op; paint path switched over in phase 4 batch 4]** `plat_stroke` | `border_w` is faked: `paint_fill_box` draws a **full-bleed filled rect in the border color underneath**, then insets the background fill by `border_w`. Consequences: a bordered node cannot have a translucent background (the border color shows through), a bordered node cannot have a gradient background *and* a correct border, and a border cannot be drawn without a background. There is no line/polyline op, so charts draw strokes as thin rects or hand-written SVG. No dashed lines. |
| **Gradients** | **[PARTLY CLOSED — phase 2]** radius + alpha added; still 2 stops, 2 axes | `paint_fill_box` calls `fill_g(x, y, w, h, rgb, c1, 0)` — it drops `inner_r` entirely, so **any gradient background renders with square corners**. No radial, no conic, no >2 stops, no angle. |
| **Clipping** | **[CLOSED — phase 2]** `plat_clip` takes a radius | Cannot clip to a rounded rect, so an image or gradient inside a rounded card has square corners. §3.3's accordion plan (animate a clip rect) works, but a rounded accordion body will not. |
| **Opacity layers** | per-node only | `paint_alpha` multiplies a node's own alpha; there is **no group/layer opacity**, so fading a subtree fades each node independently and overlapping children double-darken. Dialog enter/exit and toast fades both need this. |
| **Transforms** | **[CLOSED — phase 2]** `plat_xform` | No scale, translate, or rotate for boxes. §3.5's press-scale (0.98), dialog scale-in, tabs-indicator slide/scale, toast translate, and switch-thumb spring **all require this**. Today the only way to move something is to change `x`/`y`/`w`/`h`, which is a *layout* change — exactly what §3.3 forbids. |
| **Text measure / ellipsis** | measure yes, ellipsis **[CLOSED — batch 27]** | `metrics.measure` / `measure_wrap` are solid (in-tree via `std:font` when bound, host otherwise). `metrics.ellipsize` binary-searches the longest grapheme prefix plus `…` that fits, so an overflowing label truncates instead of running out its box. `Table` cells, `Select` triggers, `Chip`, `Tab`, and `NavTab` opt in with `Text(ellipsis = true)`. |
| **Baseline alignment** | absent | `plat_text` anchors at the top-left of the line box; `paint_label` centers vertically by arithmetic (`(h - pad - th) / 2`). Mixed-size text on one row does not sit on a shared baseline — visible in `Stat`, `Badge`+`Text` rows, and chart axis labels. |
| **Icon rendering** | SVG string re-parsed per draw | Each `Icon` ships a ~200-byte markup string through the draw list every frame, parsed by the host. No path cache, no stroke-width scaling with size, no two-tone icons. The host parsers are a line-only lucide subset — they **skip `A`/`a` arc commands** — so `IC_LOADER` is a 270° polyline, not an arc (batch 29); any future arc motif must be converted the same way until the parsers grow arc support. |

---

## 3. Reactivity: where subtrees still rebuild

The fine-grained core is real and good: `track.loam` keeps a `(dep_sig, dep_eff)`
edge table, `styled(node, handler)` allocates an effect bound to one node, and
`selected_chrome` is a shared one-effect recipe used by `Tab`, `Choice`,
`NavTab`, `PageButton`, `Radio`, `Toggle`, `ToggleGroup`. Theme switching runs
**no effects at all** — `paint_rgb` resolves the role against `appearance()`
during paint.

Rebuilds that remain:

1. **`refit` (charts)** — `refit_refresh` compares `nodes[nid].w` against
   `list_gen` and, on any width change, calls `arena.rebuild_begin(nid)` →
   `drop_children` → re-runs the builder. Every chart tears down and rebuilds its
   whole subtree on every window resize step. This is the path the arena's
   swap-remove optimizations in `release_sigs` / `free_nodes_effects` /
   `drop_eid_deps` were written to rescue (the comments cite "~25 ms per chart
   refit"), which is evidence the rebuild is the problem, not the cleanup.
2. **`For` / `Index` / `VirtualList` / `VirtualTable`** — keyed `For` moves nodes
   (good), but `Index` and the virtual windows rebuild through
   `rebuild_begin`/`rebuild_end` whenever the window or generation moves.
3. **`zeus.view` mode** — `engine_layout` under `has_view != 0` calls
   `arena.reset_tree()` + `track.reset()` + full re-build **every frame**. Not the
   default (`mount` is), but it exists and the gallery must not regress onto it.
4. **`Boundary`** — `arena.mark()` / `rollback`, correct for its purpose.
5. **`track.notify(sid)`** allocates a `hit: []int` on **every signal write** and
   linear-scans the whole edge table. Hot-path allocation in the reactive core.

Nothing in the component layer calls `drop_children` directly — that constraint
is already held.

---

## 4. Invalidation: paint, layout, and tree are one channel

This is the deepest structural problem, and it is what §3.3 asks to fix.

**There are no dirty bits anywhere in the tree.** `UiNode` has 100+ fields and
not one of them is a dirty flag. The frame is:

```
host frame
  → zeus_layout(w, h)  → engine_layout
        async.tick()
        arena.physics_step()
        (view mode: reset_tree + full rebuild)
        arena.stamp_edits()
        layout.layout(w, h)          ← FULL tree layout, unconditionally
        if virt_stale() || refit_stale() { refresh_lists(); layout.layout(w,h) }  ← twice
  → zeus_step(dt)      → engine_step → scene.step()   ← FULL O(nodes) scan
  → engine_paint       → scene.paint()
        pops the entire draw list
        paint_node(root)             ← FULL tree repaint
        present()                    ← replays every op to the host
```

Every frame does full layout **and** full paint **and** a full-arena animation
scan, regardless of what changed. A hover fade on one button re-lays-out the
entire page. The return value of `engine_step()` is a single `int` — "give me
another frame" — with no notion of *why*. `engine_next_ms()` likewise collapses
async timers, scroll physics, and tweens into one deadline.

Credit where due: **idle genuinely costs zero frames.** `mac_schedule_next`
pauses the `CADisplayLink` when `more == 0` and `next_ms == 0`; the wasm loader
stops calling `requestAnimationFrame` when `zeus_paint` returns 0. §3.4's
"idle means zero frames" is already satisfied and must be preserved.

---

## 5. Animation: currently inside the reactive graph

§3.2 says "no signal is created, read, or written per frame." Today there are
**two** animation systems and the public one violates that rule on every frame.

### 5a. `zeus.animate` — signal tweening (`arena.anim_to` / `anim_step`)

```loam
fn anim_step() -> int {
    ...
    store_sig(a.sid, a.from + (a.to - a.from) * done / a.total)
    ...
}
```

`store_sig` calls `plat_sig_bind_int` **and `track.notify(sid)`** — which
re-runs every effect subscribed to that signal, every frame, for the whole
duration. If the signal feeds a `styled` closure, that closure re-runs 60×/sec;
if it feeds a component prop thunk, that thunk re-runs 60×/sec. Concretely this
breaks four of the brief's hard constraints at once:

- writes a signal setter from the tick (forbidden),
- re-runs reactive computations per frame (forbidden),
- allocates per frame — `anim_cancel` and `anim_step` each build a fresh
  `keep: []Anim`, and `track.notify` allocates a `hit: []int` per notified
  signal (forbidden: "allocation count per animated frame: must be zero"),
- has no `node_id`, `prop`, `easing`, or `state` — the track record is
  `{sid, from, to, done, total}`, and `done`/`total` are **frame counts**, so
  a 120 Hz host runs every tween at 2× speed.

`zeus_step(float dt)` in `packages/loam/runtime/zeus_plat.c` literally reads
`(void)dt;` — the host already supplies a delta and the engine discards it.

### 5b. `scene.step()` — the per-node chrome animations

Hover fade, press fade, switch `tween`, checkbox `tween`, scrollbar fade, and
`enter_fade` are stepped here. These **do not** touch signals — that part is
already correct and is the seed of the new subsystem. But:

- it is an **O(all nodes) scan every frame**, not a list of active tracks;
- every rate is a hardcoded integer step per frame (`d / 5`, `d / 4`, `+9`,
  `+10`, `(100 - s) / 5`) — frame-count driven, so 120 Hz is 2× fast and a
  dropped frame is a dropped increment;
- state lives in `u8` fields on `UiNode` (`hover_amt`, `press_amt`, `tween`,
  `show_amt`), so there is exactly one animatable channel per concept and no
  way to add a fifth without another node field;
- there is one easing function in the tree, `arena.ease_out` (a quadratic), and
  it is applied only to hover;
- interruption is by accident: a target flip mid-fade continues from the current
  value (fine), but a *retarget* of a `zeus.animate` tween restarts `done` at 0
  from the current signal value, and the array is rebuilt to do it;
- `pulse` (skeleton) reads a global `arena.pulse_t` frame counter and computes
  a triangle wave mod 144 — it is a flicker, phase-offset per node id, not a
  shimmer sweep;
- there is no reduced-motion path and no delta-time cap.

### 5c. No proof harness

None of §3.6 exists: no component-run counter, no effect-run counter, no debug
assertion against signal writes during the tick, no per-frame allocation count.
`ZEUS_FRAME_BENCH=1` in `mac.m` forces continuous frames for benchmarking and is
the only existing instrumentation hook.

---

## 6. Tokens

**[CLOSED — phase 2.]** Recorded as found, for the record.

`Palette` was 34 named roles × 2 appearances (xAI-derived), mirrored into `ROLE`
slots 1–36 and resolved at paint. Good bones; wrong shape for a design system.

| §2 requirement | As found | Now |
|---|---|---|
| 12-step neutral scale | **No scale** — nine flat neutrals | `NEUTRAL_L` / `NEUTRAL_D`, `zeus.n(1..12)` |
| Accent scale | **No accent at all** — `accent` was `#0a0a0a` / `#ffffff`, i.e. neutral ink, so a picker was not expressible | Five 12-step ramps, `zeus.ac(1..12)`, `set_accent`. Default **blue** `#2563eb` |
| success / warning / danger | Present, ad hoc | Four tiers × 5 steps (`OkS3/S6/S9/S10/S11`, …) |
| **info** | **Missing** | `InfoS3…S11`, `TINT.Info` |
| Live theme switch | Already ideal | Unchanged — still zero effects, zero rebuilds |
| Radius none/sm/md/lg/xl/full | No `RAD_NONE`; ramp compressed at the low end | `RAD_NONE` added; ramp otherwise unchanged (see §11) |
| 4pt spacing grid | `2, 6, 62` were off-grid | Snapped; enforced by `spacing_is_on_a_four_point_grid` |
| Type: sizes, weights, line heights, tabular | Sizes only | `LEAD_TIGHT/SNUG/NORMAL/RELAXED` + `line_height()`. **Weights and tabular figures remain open** — `plat_text` carries a pixel size only |
| Elevation 0–4 | **Absent**, inexpressible | `Elev` + `elevation` / `elevation_dark` / `elev_now`; dark roughly doubles alpha and tightens blur |
| Motion fast/base/slow + easings | **Absent** | `MOTION.Fast/Base/Slow` = 120/200/320, `EASE.Linear/Standard/Emphasized/Spring` (curves land in phase 3) |
| Focus ring color/width/offset | Colour only | `FOCUS_WIDTH` 2, `FOCUS_OFFSET` 2 |
| WCAG AA on every pair | Unverified; `faint` on `bg` was **2.14:1** | 260 pairs checked in CI across 5 accents × 2 appearances |

The contrast work is the part worth keeping in mind: it is executable, not
prose, and the ratio math is itself validated against hand-checkable references
first, so the suite cannot pass by being uniformly wrong. It found four real
defects — see §11.

## 7. Accessibility and input

Already there: `engine_a11y_dump` with roles/labels, relayed to a hidden DOM
mirror on web and `NSAccessibilityElement`s on macOS; a focus ring
(`chain_nodes` / `focus_ring` / `focus_step`) with tab / shift-tab; `kb_focus`
gating so the ring is keyboard-only; a key-binding prop on every widget
(`apply_key` / `install_key`); `plat_overlay_scroll()` already branches touch
hosts away from desktop scrollbars and hover washes.

Gaps against §5:

- **No arrow-key navigation** inside `Tabs`, `RadioGroup`, `Select`, or any
  menu. Only `Slider` (`keys()`) and `Pagination` (`keys_page()`) bind arrows.
- **No roving tabindex** — every radio in a group is its own tab stop.
- **[CLOSED — phase 4 batch 11]** *No focus trap* in `Dialog`, and *no Esc
  to dismiss* on `Dialog` or `Select`.
- **[CLOSED — phase 4 batch 12]** *No 44dp minimum hit target* on touch hosts.
  Hit-testing inflates tappable nodes to 44dp; paint/layout stay authored.
  `Pip` without a click handler is not a target.
- **[CLOSED — phase 4 batch 13]** Hover wash is skipped on overlay-scroll
  hosts, and `is_hot` now returns 0 there so pointer moves do not schedule
  HoverAmt tracks.

---

## 8. Gallery and fixtures

`examples/zeus/gallery/screen.loam` (500 lines) is three pages — Foundations,
Components, Patterns — behind `NavTab`, with a `Switch(zeus.appearance())` for
dark mode and a live RAM chip. It demonstrates components but not *matrices*:
`Button` shows 6 looks × 3 sizes, but nothing else shows more than one size, and
**no component shows its hover / pressed / focused / disabled / loading /
invalid states side by side**. There is no accent picker (there is no accent to
pick). Batch 28 added a **States** matrix and the **accent picker**; hover /
pressed / focused states are still only reachable by using the control, not
posed side by side. Five per-host launchers exist (`macos`, `ios`, `android`,
`frontend`, `backend`) plus `ZEUS_HEADLESS=1`.

Eight `draw_golden` fixtures: `golden_controls`, `golden_focus_ring`,
`golden_app_scroll_bg`, `golden_textarea`, `golden_picker`, `golden_image`,
`golden_image_auto`, `golden_font_metrics`, `golden_scale_gallery`. They capture
the draw list byte-exactly, which makes them an excellent regression net — and
means **every visual change in this project will require regenerating them.**

---

## 9. What this implies for the plan

Ordered by what unblocks what:

1. **Paint primitives are the gate.** Elevation needs a shadow op; press-scale,
   dialog scale-in, tabs-indicator slide, toast translate, and switch spring all
   need a paint-level transform; rounded gradients and rounded image clipping
   need radius plumbed into `fill_g` and a rounded-clip op; text overflow needs
   an ellipsis measure. **(the ellipsis measure landed without a host op —
   `metrics.ellipsize` is in-tree, batch 27)** None of §3.5 or §4 can land first. Four new host ops,
   identical on Cocoa / UIKit / Android Canvas / Canvas2D, look unavoidable —
   `shadow`, `transform push/pop`, `clip_rounded`, and a per-corner `radius4`
   variant of `fill`. I will keep the count to these and justify each in the
   Phase 2 commit.
2. **`radius` as one `u16` and `border_w` as an underlay fill** both need to
   change before the component work, or per-corner radii and translucent
   bordered surfaces stay impossible.
3. **The dirty-channel split must land before the animation subsystem**, because
   a `PAINT`-only frame is what makes a reactivity-free tick observable — and
   `engine_step`'s `int` return and `engine_layout`'s unconditional
   `layout.layout()` are what stand in the way.
4. **`zeus.animate` is public API.** Retargeting it onto the new track pool
   without changing its signature is possible (`animate(sig, to, ms)` becomes
   "set the signal once, then animate the paint value"), but its *semantics*
   change: bound effects will no longer re-run per frame. That is the intended
   fix, not a regression — I will call it out explicitly in the Phase 3 commit
   and migrate every call site in the same commit.
5. **Six components in §4 do not exist** (Accordion, Toast, Tooltip, HoverCard,
   Empty, Spinner) and one more is needed for §3.5's overlay work (Popover, which
   `Select` should then be rebuilt on). These are new construction, not upgrades.

## 10. Resolved: scope of entry motion

Settled before Phase 2, so the animation subsystem can encode it as a rule
rather than a per-component judgement call.

**Entry motion is scoped to mount-into-a-live-tree, never cold boot.** The first
frame after app start writes every animated value at its settled state: tracks
scheduled before the first frame completes skip straight to `to`, and the pool
starts empty. Nothing moves at launch.

This is one flag on the animation subsystem, not a case per component, and it
falls out correctly for every case in §3.5 — a dialog opening, a toast arriving,
or a chart appearing on a tab you just switched to are all *state changes in an
already-running app*, which is exactly what those transitions are for. It also
keeps the `draw_golden` fixtures deterministic: the first frame is always the
settled frame, so a golden can never race the animation clock. The tree already
leans this way — `UiNode.aw` carries the comment "Rest is 0, so DRAW goldens
hold."

**Charts specifically:**

- **Width** changes keep `refit` — responsiveness across viewports is the point,
  and a width change is a discrete layout event, not something to animate.
- **Value** changes interpolate paint-only against the cached post-refit rect at
  `MOTION.base` (200 ms). Bar heights and line points only: no staggered series
  entry, no left-to-right line draw, no sweep. Those read as demo-ware and wear
  badly on a dashboard that updates on a timer.
- **Mount** into a live tree gets one short fade + rise. The same chart present
  at boot does not.

Accepted consequence: a chart on the gallery's first page will not animate at
launch but will after navigating away and back. That is the correct behavior and
matches every mature kit, but it means "does the chart animate?" has two right
answers depending on how you arrived.

---

## 11. Progress, decisions, and handoff

### Phases

| # | Phase | State | Commit |
|---|---|---|---|
| 1 | Audit | **done** | `8289306` |
| 2 | Tokens + paint primitives | **done** | `2184563` |
| 3 | Dirty-channel split + animation subsystem + state layer | **done** | `feat(zeus): dirty-channel split + animation track pool (phase 3)` |
| 4 | Components, in batches | **in progress — batches 1–14** | `feat: paint-only motion + feedback primitives` / `feat: Accordion` / `feat: overlay family` / `feat: elevation + stroked borders` / `feat: Menu` / `feat: Collapsible + Drawer` / `fix: anchored floaters in a scroller` / `feat: Select + Combobox` / `feat: exit motion + reveal` / `fix: web host paint/a11y/wheel` / `fix: anchored placement, EASE béziers, layout-driving animate` / `feat: Tabs/RadioGroup arrows + roving tabindex` / `feat: Dialog focus trap + Esc` / `feat: 44dp touch hit target` / `perf: skip hover-fade math on touch` |
| 5 | Gallery + docs (`spec.md`, a `www/` design-system page) | not started | — |

`make test` at the end of phase 3: **361 passed, 0 failed** (the six network
suites — `http_auth`, `http_h1_live`, `http_h2_live`, `net_listen`,
`zeus_async_rpc`, `zeus_stream` — need outbound sockets and fail in a sandboxed
run, as they did before this phase). All four hosts build — Cocoa, wasm, iOS
(`.app`, signed), Android (Gradle project + JNI). All eight `draw_golden`
fixtures are byte-identical to phase 2: the settled first frame is unchanged, so
nothing needed regenerating.

Everything still open is tracked as a living list in **Remaining work** at the
end of this section — update it in the same commit that closes an item.

### Decisions taken (do not re-litigate)

1. **Entry motion is mount-into-a-live-tree, never cold boot.** Full reasoning
   in §10. One flag on the animation subsystem, not a case per component.
2. **Charts keep `refit` for width** (responsiveness is the point; a width
   change is a discrete layout event). Value changes interpolate paint-only
   against the cached post-refit rect at `MOTION.base`. No staggered series
   entry, no left-to-right line draw.
3. **Nothing animates on first paint** — this also keeps the draw goldens
   deterministic, which matters more than it sounds (see the traps below).
4. **The accent is blue** (`#2563eb`) with white ink, by request. Indigo, teal,
   rose, and the original sunset orange are selectable via `set_accent`.
5. **Flat role names are kept and remapped** onto the new ramps rather than
   replaced, so no call site in `examples/` or `www/` had to move.

### Two traps that cost real time

- **`ZEUS_HEADLESS=1` must be set for the golden *compile*, not just the run.**
  With it set the binary links a deterministic measurement stub; without it,
  real Cocoa text metrics, and text wraps differently. Regenerating with the
  variable on the run alone produces fixtures that fail in `nob`, with a diff
  that looks like a genuine layout regression. The correct incantation:

      ZEUS_HEADLESS=1 MAYA_HEADLESS=1 ./bin/loamc <src> -o <bin>
      ZEUS_HEADLESS=1 MAYA_HEADLESS=1 <bin> > <stem>.txt

  Generate each twice and diff before overwriting — a fixture that is not
  reproducible run-to-run should never be committed.

- **Loam accepts duplicate `enum` variant names without any diagnostic.** Two
  new `ROLE` members collided with existing ones (`OkFill`, `WarnFill`) and
  silently bound white text against a pale wash. Nothing errored; only the
  contrast test caught it. New `ROLE` members are now named by step
  (`OkS3`, `OkS9`, …) partly to make collisions structurally unlikely. Grep
  the enum before adding to it.

  Related: doc comments (`///`) are rejected *inside* an enum body. Use `//`.

### What phase 2 deliberately did **not** do

These are known-open and are the natural first moves in phases 3–4:

- **[CLOSED — phase 4 batch 4]** *The paint path still draws borders the old
  way.* `plat_stroke` exists and every host implements it, but
  `scene.paint_fill_box` still fakes a border by drawing a larger filled rect
  underneath. Switching it over is a visual change to every bordered node, so
  it belongs with the component work, not with the primitive that enables it.
- **`UiNode.radius` is still a single `u16`.** `plat_fill4` exists; nothing
  emits it yet. Per-corner radii need either three more node fields or a packed
  one.
- **[CLOSED — phase 4 batch 4 for `shadow`; phase 3 for `xform`]** *Nothing
  emits `shadow` or `xform` yet.* They were wired end-to-end and unused —
  deliberately, because both need the dirty-channel split to be animated
  without dragging layout along.
- **`RAD_*` values are unchanged** (6/8/10/14). `RAD_NONE` was added. Retuning
  the ramp is component work.
- **Type weights and tabular figures are not done and are host-blocked.**
  `plat_text` carries a pixel size only and `plat_set_font_family` is explicitly
  one global family. Tabular numerals for data components will need either a
  host op or an app-supplied font with tabular figures; the `LEAD_*` tokens
  landed, the weight axis did not.
- **`zeus.animate` still tweens signals** and therefore still re-runs every
  subscribed effect once per frame. This is the headline fix of phase 3 and a
  behavioural change to public API — bound effects will stop re-running per
  frame, which is the intended repair, not a regression. Call it out in that
  commit and migrate call sites in the same one.

### Where to start phase 3

The order matters, because each step makes the next observable:

1. Split invalidation into `PAINT` / `LAYOUT` / `TREE`. The obstacles are
   `engine_step`'s single `int` return and `engine_layout`'s unconditional
   `layout.layout()` — today every frame lays out and paints the whole tree
   regardless of what changed (§4). Add dirty bits to `UiNode`; there are none
   today.
2. Build the track pool (§3.2) — fixed-capacity arena, `{node_id, prop, from,
   to, current, elapsed, duration, easing, state}`, one clock read per frame.
   `zeus_step(float dt)` in `packages/loam/runtime/zeus_plat.c` already receives
   a delta and currently reads `(void)dt;` — that is the hook.
3. Port the existing per-node chrome animations out of `scene.step()`. They are
   already signal-free, which makes them the right first tenants; what they lack
   is a track list (today it is an O(all-nodes) scan), time-based rates (today
   fixed integer steps per frame, so 120 Hz runs 2× fast), and easing.
4. Only then the §3.6 proof harness: component-run and effect-run counters, the
   debug assertion against signal writes inside the tick, frame timings on
   Cocoa and wasm, and zero allocations per animated frame.

Preserve what already works: **idle costs zero frames** on both Cocoa
(`mac_schedule_next` pauses the `CADisplayLink`) and wasm (the loader stops
calling `requestAnimationFrame`). §3.4's hardest bullet is already satisfied and
is easy to break by accident.

### Phase 3 outcome (do not re-litigate)

What landed, in the order above:

1. **Dirty channels.** `UiNode.dirty` carries PAINT / LAYOUT / TREE bits and
   `arena.dirty_paint` lists the marked nodes. `engine_layout` runs
   `layout.layout()` only on a resize, a tree build, or a layout-prop write; a
   PAINT-only frame skips it, and `scene.paint` re-presents the retained draw
   list when nothing is dirty. Non-int signal writes (array / string / bool)
   bypass `store_sig` in generated C, so their dirt is noted on `track.notify`
   and drained once per frame.
2. **Track pool.** A `TRACK_CAP`-sized `Track` arena in `arena.loam`:
   `{node, prop, from, to, cur, elapsed, dur, easing, state}`, plain data, no
   per-frame allocation. `anim_tick(dt)` advances every active track and writes
   paint state only. `zeus_step(float dt)` now hands its delta to
   `engine_step_dt`; the delta is capped at 64 ms. Cold boot, zero duration, and
   reduced motion jump to the final value.
3. **Chrome ported.** Hover, press, switch / checkbox thumb, mounted-surface
   fade, and scrollbar fade are tracks now. Targets are retargeted by
   `sync_chrome` from discrete state (`chrome_dirty`), not scanned per frame;
   rates are milliseconds and use `EASE.Linear/Standard/Emphasized/Spring`
   (curves implemented in `anim_ease`). The skeleton shimmer is driven by the
   millisecond clock, not a frame counter.
4. **`zeus.animate` retargeted.** State moves once, the paint overlay
   interpolates. **Behaviour change:** effects bound to the signal no longer
   re-run per frame (the intended repair). `zeus_anim.loam` was migrated to the
   new semantics, and `anim_paint(sig)` exposes the interpolated paint value.
5. **Interaction state.** One `UISTATE` overlay (hover / pressed /
   focus-visible / disabled / loading / selected / invalid) with
   `interaction_state`, so paint and hit-test read one model; `enabled` sets
   disabled, and a disabled / loading node never hovers.
6. **Proof harness.** `proof_components` / `proof_effects` /
   `proof_signal_writes` / `proof_allocs` / `proof_anim_live`, a trap if a
   signal is set from inside the tick, and a runtime allocation counter
   (`-DLOAM_ALLOC_TRACE`, Zeus builds only). `zeus_anim_proof.loam` animates 40
   bound widgets at once and asserts all four counters stay flat, the paint
   advances, the tracks drain, and idle then wants zero frames.

Deliberately left as-is:

- **The draw list is still a full-frame blit.** A paint-only frame skips layout
  and the reactive graph, but `scene.paint` still walks the tree and
  `present()` replays every op, because all four hosts blit the whole surface
  (`mac.m` paints every pixel: the root scroller carries the background). Only
  the layout pass and the O(all-nodes) animation scan were removed; a true
  partial repaint would need a retained, spliced draw list and a host damage
  contract, which is not what this phase needed.
- **[CLOSED — phase 4 batch 9.4]** *`zeus.animate` on a signal that drives
  layout interpolates the paint read only; the layout-visible value is already
  at the target.* Width / height / pins now tween and reflow on the same
  track; `get()` is still the target and effects still run once.
- **[CLOSED — phase 4 batch 9.3]** *`EASE` curves are integer approximations*
  (cubic / quint ease-out, ease-out-back for Spring), matching the token
  ordering, not an exact cubic bezier.

### Phase 4 batch 1 outcome — feedback primitives (additive)

Phase 3 deferred the generic "animatable-by-default" paint props to the
component work; batch 1 lands them:

- `UiNode` gains paint-only `an_scale` / `an_tx` / `an_ty` / `an_rot`, written by
  `APROP.ScalePct` / `XlateX` / `XlateY` / `RotDeg` / `OpacityAmt` / `RadiusPx`
  tracks. `paint_node` wraps a transformed node in `save` / `xform` / `restore`;
  identity emits nothing, so every existing golden is byte-identical.
- Public `animate_scale` / `animate_x` / `animate_y` / `animate_rotate` /
  `animate_opacity` / `animate_radius`, plus `motion_*` readers.
- `Spinner` is a **clock-driven** indicator, not a track: paint derives the
  rotation from `now_ms`, so it is phase 0 (settled) on the first frame —
  goldens stay deterministic — and needs no schedule, no allocation, and no
  signal. `spin_live` keeps frames flowing; reduced motion stops it. This is the
  same treatment as the `pulse` shimmer: continuous indication is clock-driven,
  finite transitions are tracks.
- `Empty` is a centred icon/title/body with a trailing-block action.
- New golden `golden_feedback`; `zeus_motion.loam` proves scale and spinner run
  with flat component / effect / allocation counters and that reduced motion
  idles.

Still open for later batches: the four overlay components (`Popover` /
`Tooltip` / `HoverCard` / `Toast`), elevation (`shadow` emission), and switching
the border paint path to `plat_stroke`. **[RESOLVED]** — overlays landed in
batch 3; elevation and the stroke path in batch 4.

### Phase 4 batch 2 outcome — Accordion (single open signal)

**The container owns the only state.** `Accordion(open)` takes one signal: the
open item's index, or `-1` for all closed. `AccordionItem(open = …, index = …)`
carries only its index and reads that shared signal — it has no signal of its
own and allocates no state. Opening one item closes the rest by construction:
there is one index, so two bodies can never both match it. `zeus_accordion.loam`
asserts the arena's signal count does not grow across toggles.

The header toggle is a discrete state change (allowed to run the one chevron
effect); the chevron rotation is a paint-only `RotDeg` track and the body fades
in through the enter-fade track, so the settling frames re-run no component and
no effect. The run is checked in as `golden_accordion`; the nine earlier goldens
are unchanged.

Deliberately open for this component: the body is shown/hidden discretely with a
fade (no animated height). The audit's clip-height animation needs an animated
clip rect (a paint-only clip prop) that does not exist yet; the fade is the
correct fallback until it does.

### Phase 4 batch 3 outcome — overlay family (anchored floaters)

`Popover`, `Tooltip`, `HoverCard`, and `Toast` share one mechanism: an
out-of-flow floater (`position = pos_absolute`, `z_index`) whose `anchor` is a
node id. `arena.anchor_to` records the anchor; the layout pass
(`place_anchored` / `place_node` in `layout.loam`) places the floater against
the anchor's *screen* rect, flipping to the opposite side when the preferred
one would leave the viewport. `PLACE` names the four sides.

- `Popover(trigger, open, place, gap, width)` — a transparent full-window scrim
  closes on outside click; the panel claims focus on open and closes on Escape
  (`popover.close`). Trailing block is the body.
- `Tooltip(trigger, text, place, delay)` — non-interactive, hover-triggered
  after a delay (hover intent via the engine's `hover_sig` + `async.after`).
- `HoverCard(trigger, place, delay, width)` — the interactive variant.
- `Toast(open, ms, place)` — pinned bottom (or top) by a full-window overlay,
  rises 12dp and fades in via paint-only tracks, and auto-dismisses.

Known limits, recorded rather than hidden: no exit animation (closing hides
immediately); the anchored panel does not reposition during a scroll (scroll is
paint-only, so no layout pass runs); anchored placement assumes the desktop
inset origin (safe insets 0) — on a notched mobile host a popover would be off
by the safe-area origin. All three are follow-ups, not regressions: the previous
`Select` was a hardcoded offset with no flip at all.

New golden `golden_overlays`; `zeus_overlays.loam` checks open/close, the
bottom-edge flip, outside-click dismissal, tooltip show/hide on hover, and toast
auto-dismiss.

### Phase 4 batch 4 outcome — elevation + stroked borders (visual)

This batch closes the two paint-path items phase 2 deliberately deferred: the
`shadow` op is now emitted, and `border_w` is a real stroke instead of an
under-fill. It is intentionally a visual change to every bordered surface.

- **Elevation is a prop.** `Box` (and `Card`) take `elevation` 0–4; `UiNode`
  carries `elev` + a shadow-role sentinel (`elev_c`), and `SET.Elevation` writes
  them. The light / dark curves and the `Elev` record moved from `zeus.loam` into
  `zeuscore/arena.loam` as `arena.elevation` / `elevation_dark` / `elev_now`,
  because the paint pass resolves a node's shadow and `scene` cannot import the
  component module. `zeus.elevation` / `elev_now` now delegate there, so the
  numbers still have one home. `elevation` is in `setter_is_paint_only`, so
  changing it never runs layout. Defaults: `Card` 1; `Popover` 3; `Tooltip` /
  `HoverCard` / `Toast` 2; a dialog panel 4.
- **Paint emits the shadow first.** `paint_fill_box` draws the shadow (via
  `arena.elev_now` + `shadow`), then the fill at the **full** node radius, then
  the border. Painting the shadow after the fill would hide it.
- **Borders are a stroke.** The old path drew a full-bleed filled rect in the
  border color and inset the background by `border_w`; a bordered node could
  therefore not be translucent or carry a gradient, and could not be drawn
  without a background. `paint_fill_box` now fills at the full radius and emits
  `stroke(x, y, w, h, border_color, r, border_w, alpha)` on the outline. The
  hover wash and press overlay also use the full radius again, since there is no
  inset fill to align to.
- `dump_draw_list` now prints kinds 12–14 (`shadow`, `stroke`, `fill4`), which it
  had silently skipped.
- **Goldens.** Seven of the twelve were regenerated — `golden_accordion`,
  `golden_controls`, `golden_feedback`, `golden_overlays`, `golden_picker`,
  `golden_scale_gallery`, `golden_textarea`. The border diff is mechanical
  (`fill A B W H border r` + inset `fill` becomes one `fill A B W H bg r` + one
  `stroke …`); `golden_overlays` / `golden_feedback` also gain `shadow` lines,
  and `golden_feedback` now includes a `Card` so a level-1 shadow is covered.
  Each was compiled and run with the §11 headless incantation, generated twice,
  diffed for run-to-run stability, and only then committed.

Left open, deliberately: `UiNode.radius` is still one `u16` (nothing emits
`plat_fill4`), so per-corner radii are still not possible; only the surfaces
listed above elevate — `Alert`, `Table`, and the `Select` panel stay flat until a
component pass asks for depth; and there is still no line / polyline / dashed op.

### Phase 4 batch 5 outcome — Menu (one active index)

`Menu(trigger, active)` is a dropdown of commands on the batch-3 anchored
floater. As with `Accordion`, the whole menu is **one** signal: `active`, the
highlighted item's index, where `-1` means closed — so "open" and "highlighted"
cannot disagree. `MenuItem(active, index, count, label, icon, on_click, danger)`
reads that signal plus its own index and owns no open/selection state;
`MenuLabel` / `MenuSeparator` are the group heading and hairline.

- **Keyboard.** The active row is the only focusable row (roving), claims focus
  the moment it becomes active, and owns Up / Down (move, clamped to `count`)
  and Enter (run its handler, then close). Every handler is gated on `active`, so
  a row that keeps focus after a close is inert — the test presses Enter and Down
  after a close and asserts both do nothing.
- **One highlight.** Hover writes `active` through the item's hover signal, so
  the pointer and the keyboard share one highlight instead of showing two.
- **Dismissal.** The trigger toggles; a transparent scrim closes on outside
  click; Escape closes while focus is inside the menu. Opening lands on item 0.
- **No new host op, no per-frame signal.** `active` is set once per interaction;
  the settle-and-close path writes no signal at all.
- New `zeus_menu.loam` checks open / navigate / clamp / activate / close /
  outside-click / Escape and that toggling allocates no signal; new golden
  `golden_menu` captures the panel, shadow, group label, highlighted row,
  separator, and danger ink. The twelve earlier goldens are unchanged.

Deliberately open: a disabled item is shown dimmed and can still be highlighted —
activation is a no-op (click and Enter are gated on `disabled`), but Up / Down do
not skip over it. No typeahead yet, and the trigger is not a real toggle button
(it has no `aria-expanded`-style state beyond `active`).

### Phase 4 batch 6 outcome — Collapsible + Drawer / Sheet

Both are containers; both keep the one-signal rule and reuse existing
mechanisms rather than adding any.

- **`Collapsible(open, title)`.** One 0/1 `open` signal is the only state. The
  header toggles it; the chevron rotates on a paint-only `RotDeg` track and the
  body fades in through the enter-fade track, so the settling frames run no
  component and no effect (the test asserts both counters flat, then idle).
  No card chrome unless the caller passes `background` / `border_color`.
- **`Drawer(open, side, size)`.** An edge panel over a scrim: the panel pins to
  the chosen edge of a full-window `Overlay` (the scrim doubles as the host, as
  in `Dialog`), slides on a paint-only `XlateX` / `XlateY` track, swallows clicks
  inside, and closes on outside click or Escape. `SIDE.SideStart / SideEnd /
  SideTop / SideBottom`; the top / bottom edges are the sheet shape.
- Both follow the `Toast` shape for a container whose trailing block must land in
  a specific child: build a wrapper, push the fixed chrome and a hideable body
  into it, and **return the body**, so `f(…) { … }` attaches the block where it
  belongs. (This is the trick `Collapsible` needs and `Toast` already uses.)
- New `zeus_collapsible.loam` and `zeus_drawer.loam`; new golden `golden_drawer`
  (scrim, elevation-4 shadow, panel, content) — settled on the first frame,
  because the slide jumps to its final value at boot. The thirteen earlier
  goldens are unchanged.

Deliberately open: no exit animation (closing hides immediately, as with the
overlays); the drawer claims focus on open but does not trap `Tab`, and returns
focus nowhere on close.

### Phase 4 fix — anchored floaters inside a scroller (batch 6.1)

The gallery's Menu did not open past the fold. The cause was a coordinate-space
bug, not the component: `paint_node` accumulates a scroller's offset
(`draw_y` = `y` - `paint_sy`), while `place_node` stores an anchored floater in
*window* coordinates. Nested in a scroller the panel was therefore painted
`scroll_y` too high — 3183 px above the viewport for the gallery's menu, i.e.
invisible — while every headless assertion still passed, because `text_visible`
and `want_show` are tree predicates that never look at paint coordinates.

- **Paint.** An anchored floater (kind 9, or any node with `anchor > 0`) is now
  painted with the scrollers' offsets reset, matching the window coordinates
  `place_node` writes. Relative absolute floaters (`layout_abs`, e.g. the
  DatePicker panel and the chart tooltip) keep their parent's content
  coordinates and the offsets, so they are unchanged.
- **Hit test.** `hit_floaters` had the matching inconsistency (window `x`, content
  `y`); it now tests anchored floaters against the window pointer.
- **On-screen behaviour.** A floater stays next to its trigger: it is only kept
  inside the window while the trigger is on screen, and it re-places whenever the
  scroll moves the trigger (`arena.mark_scroll` marks layout while a floater is
  live — `arena.live_anchors`). When no floater is on screen, a scroll is still
  paint-only. Scrolling the trigger out of the viewport takes the floater with it
  instead of pinning it to a window edge.
- **Test helper.** `click_id` (behind `engine_click_text`) used content
  coordinates, so it missed a scrolled control; it now converts through
  `layout.screen_origin`.
- **Gallery.** The Menu demo's trigger was a plain button labelled "Actions" —
  which is also the overline of the Buttons section above it, so the obvious
  thing to click was inert and the trigger had no dropdown affordance. It is now
  a row with a chevron and an accessibility label ("Open menu"), and the section
  copy says to click the trigger.
- New golden `golden_anchor_scroll` locks the paint side (an open Menu inside a
  scroller scrolled by 152 is painted at window y 102; it was -50), and
  `zeus_anchor_scroll.loam` locks the follow side: the panel opens beside the
  trigger, tracks it exactly while the page scrolls, and goes off screen with it
  once the trigger leaves the viewport.

Still open: an open floater does not close itself on scroll — it follows its
trigger, so a trigger scrolled out of view takes the popup with it. If the
"dismiss on scroll" convention is wanted instead, the floater's open signal must
be written back to its closed value, which differs per component (`-1` for the
Menu's active index, `0` for the Popover's open flag).

### Phase 4 batch 7 outcome — Select + Combobox

Both are selection widgets, and both use the anchored floater the overlays and
the Menu already share.

- **`Select(value, labels)` rebuilt.** The signature is unchanged, so every call
  site (gallery, `myapp`, `greeninfer`) kept working. The menu is no longer a
  hardcoded `top = SELECT_DROP, left = 0` relative panel: it anchors below the
  trigger, flips when there is no room, follows the trigger on scroll, and has
  keyboard navigation (Up / Down move the highlight, Enter picks, Escape or an
  outside click closes). One internal `active` index (-1 = closed) is the only
  added state; the picked value stays in the caller's signal. `SELECT_DROP` is
  gone.
- **`Combobox(text, options)`.** A text field with a filtered option list below
  it, on the same floater. The options are filtered by a case-insensitive ASCII
  substring match (`ci_has`) into a `Signal<[]string>`, and the list is rendered
  with `zeus.For` keyed by label (so options should be unique). Typing filters
  and opens; Up / Down move the highlight; Enter writes the highlighted option
  back into the field; Escape or an outside click closes.
- **Why the keys are not in the keymap.** A focused text field consumes Enter
  before any chord resolves (`zeus_key_dispatch`'s text-capture check), so the
  list keys are handled in the field's `on_key_down` and read through the
  engine's staged `current_key()`.
- **`For` reachability (compiler fix, separate commit).** Landing `Combobox`
  exposed a DCE bug: `kfor_refresh<T>`'s instantiated body was emitted while its
  non-generic callees (`arena.key_count`, `key_node`, `key_set`, `key_clear`)
  were pruned, because the pass that walks pruned-but-instantiated generics ran
  *after* the reachability worklist had already finished. A test using `For`
  compiled, but a test merely *importing* a module that defines an
  `For`-using function did not. The worklist now runs last, to a fixed point.
- New `zeus_select.loam` (open / arrow / pick / Escape / outside click) and
  `zeus_combobox.loam` (filter, open on type, pick into the field). The gallery
  shows both. No draw golden changed: a closed `Select` paints identically, and
  the menu is hidden.

Deliberately open: the `Combobox` list is not virtualized (every match is a
mounted row, so a very long option list is built whole), and it has no
"this option" affordance beyond the highlight. `Select` still hardcodes its
trigger width at `SELECT_W`.

### Phase 4 batch 8 outcome — exit motion for overlays

Closing a floater used to drop it from paint in the same frame, because paint,
layout, hit-test, and the tree predicates all gated on one `want_show`. Paint
now has its own gate, `arena.want_paint`: `want_show`, *or* an `enter_fade`
node whose `show_amt` is still above zero. `sync_chrome` retargets the
`EnterFade` track to 0 (`MOTION.Fast`, standard ease) instead of snapping it,
so a closing panel fades out over the frames it takes the track to land;
everything that is not paint keeps reading `want_show`, so the panel is inert
from the first frame (a click passes through the transparent scrim at once,
`text_visible` says hidden, focus does not land). Reduced motion and cold boot
still jump. A floater re-opened mid-exit retargets from its current alpha.

- The enter-fade target is now `shown_in_tree`, not the node's own `want_show`.
  A Dialog / Drawer / Toast panel is hidden by its scrim's signal, never its
  own, so before this it faded in on the first open only; now every open fades
  in and every close fades out, scrim and panel together.
- An in-flow node hidden by its *own* signal (an Accordion body) still snaps:
  layout collapsed it this frame, and a fade painted over the neighbour that
  moved into its rect would read as a glitch. Only floaters, and nodes hidden
  by an ancestor, exit-fade.
- `Toast` now carries `enter_fade` on the card and sinks 12dp while fading
  out; `Drawer` slides off over `MOTION.Fast` instead of jumping to the edge.
- Known limit, later narrowed: there is no group opacity, so overlapping
  translucent fills still double-darken. Descendant text/svg/image now inherit
  `enter_fade` `show_amt` (batch 15). A Menu `on_select` snaps the exit so the
  panel is gone the same frame.
- Also in this batch: `Accordion` and `Menu` take their rows as data
  (`items = []AccordionRow` / `[]MenuRow`) with one `on_select(index)` callback
  on the Menu, instead of a trailing block of items. `Menu.active` is optional
  (`active__set`): pass one to observe or drive the menu; omitted, the menu
  owns it.
- `zeus_overlay_exit.loam` locks the draw-count shape across close, mid-fade,
  landed, reopen-mid-exit, dialog scrim, and reduced motion; the three
  Accordion / Menu fixtures were migrated to the data API.

### Phase 4 batch 9 outcome — reveal (the paint-only "animated height")

Animating a layout height per frame is exactly what the model forbids, so the
Accordion body did not move: it appeared at full height and faded. The deferred
piece was a paint-only clip, and that is what landed: `UiNode.reveal` opts a
node in (`zeus.enter_reveal`), `an_reveal` is the share of its laid-out height
paint shows (0..100, `APROP.RevealAmt`), and `paint_node` clips the box to that
share from the top and slides the content up by the rest, so a body comes down
from under its header. Layout gives the box its full height on the frame it is
shown — one discrete layout, as before — and only paint catches up, over
`MOTION.Base` with the emphasized ease.

- `sync_chrome` drives it beside `enter_fade`: settle on first sight (cold boot
  is full height), reveal on show, snap to 0 on hide so the next open starts at
  the top. There is no reveal-out: layout has already collapsed an in-flow node
  by the time paint could animate it, the same reason the exit fade skips them.
- `Accordion` bodies and `Collapsible` opt in. The clip is a plain rect; the
  card's own `overflow_hidden` still rounds the corners.
- Fixed on the way: `paint_node` wrapped its transform shell *before* the
  visibility check in `paint_node_body`, so a hidden node carrying a paint
  transform (a rotated chevron in a hidden row, a translated card in a closed
  Toast) emitted an empty save / xform / restore triple every frame. The check
  is now first, so a hidden node costs nothing. No golden moved: they are the
  settled first frame, where no such node exists.
- `zeus_accordion_reveal.loam` locks the draw-list shape at open (reveal shell
  present), settled (gone), close (body snaps; only the chevron in flight),
  re-open of another row (starts from zero), and reduced motion (no shell).

### Phase 4 fix — web host: paint units, a11y leak, wheel coast (batch 9.1)

Three gallery-on-wasm defects, all host-side; the headless draw list for the
same frames was correct throughout, which is why no golden caught them.

- **Shadows at 2x painted a black card-sized block.** `loader.js` draws a
  shadow by filling the rounded rect off-canvas to the left and letting only
  its blur land in place. Under the dpr-scaled CTM the path is in layout px
  but `shadowOffsetX` / `shadowBlur` are device px and ignore the CTM; the
  code moved the path by `off * sx` (layout space, so 2·off at 2x) and the
  shadow by `off * sx` (device, so off). The fill and its shadow disagreed by
  `off`, the Large card's solid `#000` fill landed on screen at x ≈ −68..417,
  and its shadow washed the Medium card grey. The offset also had to include
  `p.x`: `w + 2·blur + 64` does not clear the canvas edge for a rect that is
  not at the left margin. Now: path moves by `off`, shadow by `off * sx`,
  `off = p.x + p.w + 2·blur + 64`.
- **Borders were double width at 2x.** `stroke()` computed `lineWidth` as a
  device-pixel count, but `lineWidth` is in user space under the scaled CTM,
  so a 1dp hairline drew 2dp (4 device px). Now snapped to whole device pixels
  and expressed in layout units (`round(w·sx) / sx`). This also ate the inner
  radius of small outlined controls (icon buttons, chips).
- **The wasm heap grew without bound (~2 MB/min idle).** `zeus_a11y_sync`
  ran after every paint and called `engine_a11y_dump`, which builds a fresh
  ~16 KB string — and the runtime never frees strings (`loam_rt.h`: "no
  string ownership story"). The RAM chip's 500 ms tick forced a frame, so it
  leaked while idle. Two changes: `engine_a11y_bytes` returns the refcounted
  `[]int` the dump is built from, and the shim copies it out and
  `loam_vec_drop`s it (no string exists); and `engine_layout_gen` counts
  layout passes, so the shim re-dumps only when the tree could have changed
  (`NULL` = unchanged; the loader keeps its mirror). `zeus_heap_kb` is
  exported for the RAM chip and harnesses. Measured in the Deno harness on the
  gallery: 910 → 2250 KB over 1800 frames before; flat at 886 KB after,
  scrolling included. `wasm_smoke.ts` now asserts the live heap moves ≤ 8 KB
  over 600 frames with timers firing.
- **Wheel scrolling coasted twice.** Web and mac armed the engine's momentum
  for precise (trackpad) deltas, but the OS already delivers a trackpad's
  inertia as a stream of wheel events, so the engine coasted on top of it and
  painted a frame per coast step after the fingers lifted. Precise deltas now
  step exactly where they land (`scroll_step`). A **mouse notch** gets the
  browser's own feel instead: `scroll_smooth` moves a per-scroller target and
  `physics_step` eases the position 35 % of the gap per frame (~150 ms for
  a 100 px notch, lands exactly, retargets from the pending target so notches
  add up, clamps at the content, chains to the parent scroller at an edge,
  plain step under reduced motion). Bounded, unlike the coast: idle wants no
  frames once it lands. Web tells a notch by `wheelDeltaY % 120 == 0` (or
  line / page `deltaMode`); mac by `!hasPreciseScrollingDeltas`. Touch pans on
  iOS / Android keep the engine coast: a canvas gets no OS inertia there.
  `zeus_scroll_smooth.loam` locks the ease, landing, retarget, clamp, idle,
  and reduced-motion paths.

- **Scrollbar thumb shows and hides in one frame each.** It faded to hidden
  over 900 ms on a `BarFade` track that kept a paint frame every 16 ms running
  after each scroll. Now a scroll sets a *wall-clock* deadline (`bar_t`,
  `plat_now_ms` — the engine clock advances by a capped delta and lags after
  an idle sleep), the thumb paints until it, `bar_expire` hides it in one
  frame, and `engine_next_ms` reports the deadline so an idle host wakes once.
  No track, no fade, no frames while shown. The `BarFade` prop, `bar_amt`, and
  `anim_fade_from` are gone, and reduced motion no longer needs a scrollbar
  exception. `zeus_scrollbar.loam` locks show / no-track / single wake / hide.

Recorded, not fixed: any `{{ }}` interpolation or `string_from_bytes` that
runs per frame or per timer tick leaks for the process lifetime. That is a
language limit (strings have no drop), and the rule for Zeus code is the one
`engine_a11y_bytes` follows — build bytes into a `[]int`, hand the vec out,
never mint a string on a hot path.

### Phase 4 fix — anchored placement uses the safe-area origin (batch 9.2)

`place_node` already flipped against `arena.safe_t/l/r/b`, but the last-resort
clamp that keeps a panel on screen while its trigger is visible still used the
desktop origin `(0, 0)`. On a notched phone a `PlaceAbove` panel that fitted
neither side landed at `y = 7`, inside the 47pt notch. The clamp is now the
safe origin `(left, top)`, per axis, and only while the trigger is still in
that rect — a trigger scrolled away still takes the popup with it (batch
6.1b).

- `engine_set_insets` is the test hook that writes `plat_set_insets` so a
  layout pass sees a notched viewport. iOS and Android already called
  `zeus_set_insets` from the view; tests had no way to set them.
- `zeus_anchor_safe.loam` locks: a below-menu flips above the home indicator
  and stays glued to the trigger (not shifted by an extra `safe_t`); a tall
  `PlaceAbove` panel clamps to the notch edge (`y = 47`, not `0`); a
  scrolled-away trigger still takes the panel off screen; landscape left-inset
  is the menu's `x`.

Deliberately open: a too-tall panel can still extend into the home indicator
(the far edge is not clamped, the same as 6.1b on desktop). Overlay / Drawer /
Toast were already laid out in the safe rect (`layout_overlays`). No golden
moved: goldens have insets 0, so the clamp is the same `(0, 0)`.

### Phase 4 fix — EASE tokens are CSS cubic-béziers (batch 9.3)

Phase 3 shaped `EASE.Standard / Emphasized / Spring` with integer polynomials
(ease-out cubic, ease-out quint, a broken ease-out-back that overshot 4× at
the start). Those were stand-ins for the named curves. `anim_ease` now
evaluates the CSS cubic-bezier of each, in permille, with a 16-step binary
search on the parameter so `x(u) ≈ t` and then `y(u)` — no allocation, i64
only for the 1000³ product.

| Token | Curve | `cubic-bezier` |
|---|---|---|
| Linear | identity | — |
| Standard | ease-out cubic | `(0.215, 0.61, 0.355, 1)` |
| Emphasized | ease-out quint | `(0.23, 1, 0.32, 1)` |
| Spring | ease-out-back | `(0.34, 1.56, 0.64, 1)` |

Spring now overshoots ~9 % and settles; the old polynomial jumped to ~4× on
the first millisecond (a sign error on the cubic term) and was saved only by
the `t <= 0 → 0` clamp. Endpoints are still exact (`t <= 0` → 0, `t >= 1000`
→ 1000), so a settled track and a cold-boot golden are unchanged.

`zeus_ease.loam` locks the permille samples and that a live Standard track
paints the same helper. No golden moved.

### Phase 4 fix — layout-driving `animate` tweens the box (batch 9.4)

`zeus.animate` still moves the signal's *state* once (`get()` is the target,
effects run once, the tick writes no signal). The paint overlay is unchanged
(`Progress`, `anim_paint`). What changed: a layout prop the same write lands
on — width, height, min/max, or a pin — no longer jumps to the target on that
first effect. The node keeps `from`, and the SigVal track tweens the field
with the same ease, reflowing each frame until it lands (then idle wants no
frames). Hover / press still skip layout: they never push a layout-anim
binding. The table is a fixed 64-slot arena, filled once, no per-frame
allocation.

Zero duration, reduced motion, and cold boot still jump, so DRAW goldens
are the settled first frame. `zeus_anim_layout.loam` locks: layout stays at
`from` when `animate` is called, moves off it on the first tick, lands on
`to`, retargets from the current width, and jumps under zero-ms / reduced
motion. `zeus_anim.loam` / `zeus_anim_proof.loam` are unchanged (they drive
`Progress`, not a box size).

Deliberately open: padding / font / spacing still jump (not a continuous
length we committed to tween). A layout-driving animate is a layout frame,
unlike a hover fade.

### Phase 4 — arrow keys + roving tabindex on Tabs / RadioGroup (batch 10)

Menus (batch 5) and Select (batch 7) already moved a highlight with arrows
and roved focus onto the active row. Tabs and RadioGroup did not: every
tab / radio was its own tab stop, and arrows did nothing.

`Tab` / `Choice` / `Radio` now bind Left/Right (tabs, segmented) or
Up/Down + Left/Right (radios), clamp at both ends, and keep `focusable` only
on the selected item — which takes focus when it becomes selected, so the
next arrow lands on it. `roving_step` / `roving_focus` are the shared
helpers. `NavTab` and `ToggleGroup` are unchanged (not in this item).

`zeus_tabs_keys.loam` locks: arrows move and clamp on both widgets; of three
tabs only the selected one is a tab stop. No golden moved.

### Phase 4 — Dialog focus trap and Esc (batch 11)

`Dialog` / `AlertDialog` now bind Escape (`dialog.close`) and install a
modal focus trap on the card while `open != 0`. Tab / shift-tab collect
only that subtree (`arena.trap_id` / `focus_walk_root`), so page controls
behind the scrim are not stops. Opening moves focus to the first
focusable descendant (the Close control, or a body widget); closing
releases the trap and restores the previous focus. Select already closed
on Escape from batch 7 (`select.close` on the menu, which holds focus);
`zeus_dialog.loam` re-checks that and locks the trap.

No golden moved: the first frame is still settled and closed.

### Phase 4 — 44dp touch hit target (batch 12)

iOS / Android (`plat_overlay_scroll`) keep the authored layout and paint
size. `in_box` inflates the hit rect of a tappable node to 44×44 dp,
centered on the layout box, so `SIZE.Sm` (32), the dialog Close (28),
Switch (20 tall), and a 16dp checkbox row still receive the pointer.
Overlays and scrollers are not inflated. Desktop is unchanged.

`engine_set_overlay_scroll` is the test hook (same idea as insets).
`zeus_touch_hit.loam` locks: a 32² control misses 4dp above on desktop,
hits on touch, still misses 20dp above, and stays 32² in layout. No
golden moved.

### Phase 4 — skip hover-fade math on touch (batch 13)

`paint_hover_wash` already returned on overlay-scroll hosts, but `is_hot`
still ran `ptr_in` and compared `hover_amt` for every tappable node on
each pointer move, then `sync_chrome` scheduled HoverAmt tracks that
never painted. `is_hot` now returns 0 on those hosts, so there is no
hover state, no hover track, and no extra frames. Desktop is unchanged.
`zeus_touch_hover.loam` locks it.

### Phase 4 — Alert info/success, dismiss, action slot (batch 14)

`Alert` is the info tier (info line + ink, `IC_INFO`). `AlertSuccess` is
the missing success tier (`ok` edge, `IC_CHECK`). `dismiss` is a 0/1
signal: an X in the title row sets it to 0 and `visible` hides the card.
Trailing block is the action slot (end of the row).
`AlertDestructive` / `AlertWarning` are unchanged in structure.
`zeus_alert.loam` locks success, dismiss, and a Retry action.
`golden_scale_gallery` moved only the default Alert's stroke/icon/title
to the info tokens.

### Phase 4 — overlay exit: inherited fade + Menu pick snaps (batch 15)

Closing a floater faded only the panel fill; child `Text` stayed at alpha 255
until `show_amt` landed, so Menu / Popover labels sat on the heading after the
white box was gone. `paint_alpha` now multiplies ancestor `enter_fade`
`show_amt` (skipping the kind-9 scrim so Dialog does not square the fade),
and text ops carry that alpha. Hosts apply it through `plat_alpha` at
present-time — extra host calls, not extra draw-list ops, so
`zeus_overlay_exit.loam`'s `draw_count` still holds.

A Menu pick (`on_select` / Enter) jumps the exit instead: `snap_exit` writes
`show_amt` 0 on the panel and every enter_fade node that shares its hide
signal (the scrim). Escape and outside click still fade. `zeus_menu.loam`
locks that the pick frame matches the closed draw count and leaves no track.

### Phase 4 — Avatar status dot / group stack (batch 16)

`Avatar` takes `status` (`TINT`, `-1` = none): a corner pip with a plate
ring. `AvatarGroup(labels, size)` overlaps faces by a third of the diameter.
`User` forwards `status`. `zeus_avatar.loam` locks the pip role and the
overlap. Stat delta / Pagination ellipsis stay open.

### Phase 4 — Input clear + Combobox type-to-select (batch 17)

Mac/iOS type through `insertText`, not `key_ev`, so Combobox never opened on
real typing (the test used `engine_key_ev`). `insert_text` now replays the
field's `kdown` handler so the list opens and filters. The field is `z_index`
61 so clicks on the box and on a match hit the combobox, not the outside-click
scrim. Clicking a match writes it into the field.

`TextInput` / `TextArea` / `Field` / `Combobox` show a Clear (X) while the
value is non-empty. Cmd/Ctrl+A selects all (and no longer inserts "a") so
Backspace clears. `zeus_combobox.loam` locks insert-to-open, click-to-pick,
and Clear; `zeus_textarea.loam` locks Cmd+A and Field Clear.

Floaters now paint and hit-test in `z_index` order (not tree order). Select /
Combobox / Menu / Popover popups use 80, the trigger/field 50, the scrim 40,
so a Select menu is not covered by a Combobox field that comes later in the
card. `zeus_select.loam` clicks Medium through that overlap.

### Phase 4 fix — z-ordered hit test regressions (batch 17.1)

Two tests broke under batch 17: `zeus_focus` because the new Clear X was a
tab stop between a non-empty field and the next control (it is pointer-only
now; keyboard users clear with Cmd/Ctrl+A + Backspace), and
`zeus_picker_lazy` because `Overlay` defaults to z 50 while the DatePicker
sheet sat at z 20, so the z-sorted hit pass gave the scrim the day click.
The picker now uses `Z_SCRIM` / `Z_POPUP` like Select. `zf_collect` also
lost the scroll-offset accumulation and viewport gate the old tree walk did
for kind-11 ancestors; the hit pass carries both again.

### Phase 4 — Button states (batch 18)

`icon` slot, `loading` signal (spinner takes the slot, 70% dim, inert),
disabled fill swap, and press-scale: `press_scale` on the node schedules
`ScalePct` to 97 from `sync_chrome` beside the press wash. The engine now
swallows clicks and Enter / Space on disabled / loading nodes (`is_inert`);
before, `enabled = false` only dimmed and still fired `on_click`.
`zeus_button_states.loam` locks all four.

### Phase 4 — Badge / Chip (batch 19)

`Badge(dot = true)`, `BadgeCount(sig, kind, max)` (hidden at 0, `99+`),
`Chip(dot = false, on_remove = …)`. `zeus_badge_chip.loam`.

### Phase 4 — Stat delta / sparkline, Pagination ellipsis (batch 20)

`Stat(delta, trend, spark)`: `TREND` arrow + tint row, `Sparkline` is an
svg path built from a `Signal<[]int]` text thunk (a push repaints one node).
`Pagination` past seven pages is seven fixed slots reading `page_at(cur,
pages, slot)` — labels, chrome, and the click target are thunks of the page
signal, so the row never rebuilds; ellipsis slots are inert
(`More pages`). `zeus_stat_pagination.loam`.

### Phase 4 — Progress easing + indeterminate, Slider knob (batch 21)

`Progress` keeps the last state in `tween` and on a plain `set` calls
`arena.anim_paint_from(sid, last, MOTION.Base)`: the signal's state lands at
once (effects run once), the paint overlay tweens. A write made by
`zeus.animate` is detected through `anim_bind_on()` and left to its own
track (the anim bench relies on a 1,000,000 ms track surviving). A negative
value marks the node `spin` (frame-loop liveness, reduced-motion aware)
and kind 6 paints a sweeping 30% segment instead of rotating. Slider knob:
shadow + 2dp hover grow, wash confined to the knob. `zeus_progress.loam`.

### Phase 4 — Table (batch 22)

`TableRowProps` (`zebra`, `hover`, `on_click`), `TableRowCols(cols, spec)`
for per-column width + `align`, `TableCols(cols, sort = sig)` with
`sortable` columns cycling none → asc → desc and an arrow on the sorted
one. Zebra counts `row`-role siblings so a header does not shift the
stripe. `TableHead` now shares the row gutter so header and cells line up.
Truncation stays host-blocked. `zeus_table.loam`.

### Phase 4 fix — sorting actually reorders rows (batch 22.1)

Batch 22's header cycled the sort signal and nothing moved: the demo rows
were static. Now `Table(spec, rows, cells, sort = …)` derives the shown
order (`sort_cells`, numeric-aware, stable) and renders through a keyed
`For`, so a sort moves nodes; `VirtualTable` takes `sort` for its header
and the app sorts its `Signal<[]T>` (`sort_cells` / `sort_by`). Rows are
structs with a `cells` mapper, not `[][]string`: pushing an inner array
moves it out of the outer one in Loam, so nested arrays hollow out.

Two compiler findings on the way. (1) A closure that *calls* a captured
fn-typed local (`key(t)`) never recorded the capture — the call checker
resolved the callee name without the ident path's capture pass — so the C
body referenced an undeclared variable, in plain and generic fns alike.
Fixed in `typecheck.c` (`closure_calls_captured_fn` golden). (2) A struct
default that names a fn declared *later* in the file fails codegen with a
missing `__as_fn` trampoline; declare handler defaults before the struct.
`zeus_table_sort.loam`.

### Phase 4 — Card (batch 23)

`interactive` / `on_click` (role `button`, hover lift, press scale),
`lift`, `title` / `subtitle` header, `media` strip (negative side margins
bleed it to the edge), `footer` thunk. Hover lift is a node field: paint
blends the resting `elev` shadow toward `lift` on the eased hover amount
and skips the wash. Footer ordering needed a small engine addition: a
node's `slot_into` redirects `ui_push` so the caller's trailing block lands
in an inner body column while the returned node stays the shell.
`zeus_card.loam`.

### Phase 4 — Tabs indicator (batch 24)

One plate box under the selected trigger, first child of the tray. Its
`left` / `width` / `height` pins are signals bound through `prop_int`; a
selection change `animate`s left and width (layout-tweened pins, batch 9.4)
and the tray's `list_fn` refit hook re-places it with `ms = 0` after the
first layout and on resize (the second layout in `engine_layout` applies
it). Triggers pass `plain = 1` so `selected_chrome` only swaps the label
colour. `zeus_tabs_indicator.loam`; `zeus_selection.loam` now tracks the
label colour; `golden_scale_gallery` shows one indicator + shadow instead
of three tab fills.

### Phase 4 — Dialog scale-in + responsive width (batch 25)

The card rests at 96% (`animate_scale`, jump at boot) and an effect on
`open` scales it to 100 over `MOTION.Base` / back over `MOTION.Fast` under
the exit fade; the scrim's fade was already there. Width is `width_pct(92)`
capped by `max_width = 440`. `zeus_dialog_motion.loam`.

### Phase 4 — Skeleton directional shimmer (batch 26)

`Skeleton` now calls `pulse` (it never did — the gallery skeletons were
static). `paint_alpha` no longer runs the alpha sawtooth; `paint_shimmer`
clips to the box and sweeps two gradient halves (fill → plate → fill) 40%
wide on `shimmer_phase` (1.6 s, per-node offset). Reduced motion parks the
band and `pulse_live` idles. `zeus_skeleton.loam`; `golden_controls` gains
the five band ops.

### Phase 4 — text truncation (batch 27)

The one visible paint gap the audit flagged: a label wider than its box ran
out the side. `metrics.ellipsize` measures the ellipsis glyph once, then
binary-searches the longest grapheme prefix (via `unicode.grapheme_start`)
whose width plus the mark fits `max_w` — the cut is always a cluster boundary,
so a combining mark or a ZWJ emoji is never split, and an empty string is
returned only when not even the mark fits. It rides the existing measure
(`std:font` in-tree or the host), so **no host op is involved**.

`UiNode.ellipsis` is opt-in. `paint_label` truncates when `inner_w < tw` and
`apply_label_wrap` refuses to wrap such a label; `row_can_absorb` also lets it
take a row's deficit, so it narrows instead of pushing siblings onto a second
line. `Text(label, ellipsis = true)` is the API; `Table` cells, `Select`
triggers and rows, `Chip`, `Tab`, and `NavTab` opt in. Metrics only change
when a label actually overflows, so `golden_controls` / `golden_menu` /
`golden_feedback` are byte-identical.

### Phase 4 — gallery states + accent picker (batch 28)

Phase 5's gallery gap: it showed components, not the states they can be in.
The Components page gains a **States** matrix — every `Button` look resting
beside disabled, plus loading / icon; a field resting beside disabled and
invalid — and the navbar gains the **accent picker** (`set_accent` already
existed but nothing exposed it; the five brand ramps are now dots, and the
ring marks the one in effect). Hover, press, and focus are live — the engine
drives them from the pointer and the keyboard, and forcing them for a static
pose would need paint overrides the model is not built for — so the matrix
documents them as live rather than posing them.

### Phase 4 — per-corner radius + the loading spinner (batch 29)

`plat_fill4` existed on all four hosts but nothing emitted it, so no sheet,
tab-shape, grouped run, or rounded-top header was expressible. `UiNode` now
carries four `u16` corner overrides (`rad_tl/tr/br/bl`, `0xFFFF` = "use the
uniform `radius`"); `int` is 32-bit in this tree, so a packed four-lane value
would have needed `i64` and shift-safe lanes — four `u16`s cost the same and
need no shifts. `paint_fill_box` resolves the four corners and switches to
`plat_fill4` only when they differ, so an untouched node keeps its old op and
every uniform golden is byte-identical. A per-corner surface with a border has
no per-corner stroke op, so the border is painted as an outer `fill4` ring with
the background inset by it (exact for the opaque surfaces that carry corners);
the shadow uses the largest corner. `Box.radius_top` / `radius_bottom` and the
chainable `radius4` / `radius_corner` are the API. The `Drawer` sheet now
rounds only its inner edge, so the panel still meets the window edge square;
`golden_drawer` was regenerated (fill/stroke → `fill4` ring).

Same batch, the loading spinner never drew on any host: `IC_LOADER` was the one
icon whose path used an SVG **arc** (`a9 9 …`), and every host's SVG parser is a
line-only lucide subset that skips `A`/`a`. It is now a 19-chord 270° polyline
(`M`/`L` only), within a tenth of a pixel of the arc at 16px.

### Remaining work (living list)

The single maintained tracker of what is still open. Landed so far: phases 1–3;
phase 4 batches 1–26 (component upgrades), 27 (text truncation), 28 (gallery
states + accent picker), 29 (per-corner radius + loading spinner).

**Phase 4 — components still to build**

Absent entirely:

- [x] `Menu` / `DropdownMenu` — anchored command list; keyboard nav + roving focus. **(batch 5)**
- [x] `Drawer` / `Sheet` — edge-anchored panel (reuses the overlay floater + scrim). **(batch 6)**
- [x] `Collapsible` — single-open disclosure: the Accordion model without the card. **(batch 6)**
- [x] `Combobox` — a text field with a case-insensitively filtered option list;
  type to filter, Enter picks. **(batch 7)**

Rework, not new construction:

- [x] Rebuilt `Select` on the anchored floater (the `Popover` mechanism). It was
  hardcoded (`top = SELECT_DROP`), not edge-aware, and had no keyboard nav,
  typeahead, or Esc. **(batch 7)**

Component upgrades the audit calls out in §1 (all landed as of batch 26):

- [x] `Button`: press-scale, loading state, icon slot, disabled *styling*. **(batch 18)**
- [x] `Card`: hover lift, interactive variant, header / footer / media slots. **(batch 23)**
- [x] `Dialog`: scale-in + scrim fade, focus trap, Esc, responsive width.
  **(batches 8, 11, 25)**
- [x] `Tabs`: an indicator element + arrow-key nav. **(batches 10, 24)**
- [x] `RadioGroup`: roving focus **(batch 10)**; `Slider`: knob shadow / hover
  **(batch 21)**. The slider still uses the generic focus ring, not one on
  the knob.
- [x] `Progress`: easing + indeterminate mode. **(batch 21)**
- [x] `Table`: zebra, row hover, sort affordance, per-cell alignment
  **(batch 22)**. Truncation stays open under "Paint primitives" (no
  ellipsis measure).
- [x] Charts: gridlines, axis labels, crosshair, tooltip, legend. **(already
  landed in the chart rework; the line was stale)**
- [x] `Alert`: info / success tier, dismiss, action slot **(batch 14)**;
  `Badge` / `Chip`: dot / removable / count **(batch 19)**.
- [x] `Avatar`: status dot / group stack **(batch 16)**; `Stat`: delta /
  sparkline; `Pagination`: ellipsis **(batch 20)**.
- [x] `Skeleton` shimmer is a global frame-counter sawtooth, not directional.
  **(batch 26)**

**Paint primitives / host ops**

- [x] Per-corner radius. `plat_fill4` is implemented on all four hosts but
  *nothing emits it*; `UiNode.radius` is a single `u16`. Needs packed node
  fields (three more, or one packed value) — no new host op. **(batch 29: four
  `u16` corner overrides, `paint_fill_box` emits `plat_fill4` when they differ;
  `Box.radius_top` / `radius_bottom` / `radius4`; `Drawer` rounds its inner
  edge)**
- [ ] Group / layer opacity. Descendant fills and text inherit `enter_fade`
  `show_amt` (batch 15), so overlay labels fade with the panel; overlapping
  translucent fills still double-darken. A true save-layer is still open.
- [ ] Text ellipsis / truncation. None exists, so table cells, select triggers,
  chips, and nav labels overflow. Needs an ellipsis measure. **(batch 27:
  `metrics.ellipsize` + `Text(ellipsis = true)`; Table / Select / Chip / Tab /
  NavTab opt in. In-tree, no host op.)**
- [ ] Baseline alignment. `plat_text` anchors at the top-left of the line box, so
  mixed-size text on one row does not share a baseline.
- [ ] Line / polyline / dashed op. Charts hand-roll strokes as thin rects; no
  dashed lines.
- [ ] Gradients: only 2 stops and 2 axes; no radial, conic, angle, or >2 stops.
- [ ] Icon rendering: the SVG string is re-parsed per draw; no path cache, no
  stroke-width scaling, no two-tone. The set is 10 constants.
- [ ] **Host-blocked:** type weights + tabular figures. `plat_text` carries a
  pixel size only and `plat_set_font_family` is one global family; needs a host
  op or an app-supplied tabular font.

**Limits recorded when earlier batches landed**

- [x] Overlays (batch 3): no exit animation (close hides immediately). **(batch 8)**
- [x] Overlays (batch 3): anchored placement assumes the desktop inset origin, so
  it is off by the safe area on a notched mobile host. (Batch 6.1 fixed the paint
  space and made a floater follow its trigger on scroll.) **(batch 9.2:
  clamp to `safe_t/l`, not `(0, 0)`)**
- [x] Accordion (batch 2): the body fades; there is no animated height, because
  that needs a paint-only clip prop that does not exist yet. **(batch 9:
  `enter_reveal`)**
- [ ] Phase 3: the draw list is still a full-frame blit — no partial repaint /
  host damage contract.
- [x] `EASE` curves are integer approximations (cubic / quint ease-out,
  ease-out-back for Spring), not exact cubic béziers. **(batch 9.3)**
- [x] `zeus.animate` on a signal that drives layout interpolates the paint read
  only; the layout-visible value is already at the target. **(batch 9.4:
  width / height / pins tween and reflow)**

**Accessibility / input (§7)**

- [x] Arrow-key nav inside `Tabs` and `RadioGroup` (menus gained it in batch 5,
  `Select` in batch 7). Only `Slider` and `Pagination` otherwise bind arrows
  today. **(batch 10)**
- [x] Roving tabindex for radio / tab groups — every radio in a group is its own
  tab stop today. (Menu rows rove; only the active row is focusable.) **(batch 10)**
- [x] Focus trap in `Dialog`; Esc to dismiss `Dialog` and `Select`.
  **(batch 11; Select Esc was batch 7)**
- [x] A 44dp minimum hit target on touch hosts; iOS and Android get the desktop
  geometry verbatim (`SIZE.Sm` 32dp, dialog close 28dp, `Checkbox` / `Radio`
  16dp, `Pip` 8–10dp). **(batch 12: hit rect only; paint stays authored)**
- [x] `is_hot` still runs the hover-fade math for every node each frame on
  overlay-scroll hosts (hover is correctly suppressed, but the math is not).
  **(batch 13)**
- [ ] Strings are never freed (`loam_rt.h`). A `{{ }}` interpolation on a
  timer tick (the gallery's RAM chip, every 500 ms) leaks a few bytes per
  tick for the process lifetime; harmless at that rate, but a per-frame one
  is a real leak (batch 9.1 removed the one the web host had). Needs a string
  drop in the language; until then, hot paths build `[]int`, not strings.

**Phase 5 — gallery + docs**

- [ ] The gallery shows components, not *matrices*: no side-by-side
  hover / pressed / focused / disabled / loading / invalid states, and no accent
  picker. **(batch 28: accent picker + a States matrix for the posed states;
  hover / pressed / focused stay live — posing them needs a paint override the
  shared model does not have)**
- [ ] A `www/` design-system page.
- [ ] The remaining `spec.md` design-system coverage.
