# Zeus design audit

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
| `TextInput` / `TextArea` / `Field` / `Input` | focus ring, caret, selection highlight | Caret is a 1px rect that **never blinks**. Selection highlight is a hardcoded `11053224` literal, not a theme role — wrong in dark. Placeholder exists via `raw_input`. No error/invalid state, no help text, no prefix/suffix slots, no clear button, no character count. |
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

**Absent entirely** (grep returns zero definitions): `Accordion`, `Toast`,
`Tooltip`, `HoverCard`, `Popover`, `Menu`/`DropdownMenu`, `Empty` state,
`Spinner`/`Loader`, `Drawer`/`Sheet`, `Collapsible`, `Combobox`. Six of the
eleven transitions §3.5 requires belong to components that do not exist yet.

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
| **Soft shadow** | absent | No elevation is expressible at all. Cards, dialogs, popovers, switch thumbs, and menus are flat by construction. This is the single biggest reason the kit reads plain. Requires a new host op on all four backends. |
| **Per-corner radius** | absent — `radius` is one `u16` | No top-only rounded sheets, no tab-shaped triggers, no grouped button runs, no rounded-top table headers. |
| **Anti-aliased strokes** | absent | `border_w` is faked: `paint_fill_box` draws a **full-bleed filled rect in the border color underneath**, then insets the background fill by `border_w`. Consequences: a bordered node cannot have a translucent background (the border color shows through), a bordered node cannot have a gradient background *and* a correct border, and a border cannot be drawn without a background. There is no line/polyline op, so charts draw strokes as thin rects or hand-written SVG. No dashed lines. |
| **Gradients** | 2 stops, 2 axes, **no radius** | `paint_fill_box` calls `fill_g(x, y, w, h, rgb, c1, 0)` — it drops `inner_r` entirely, so **any gradient background renders with square corners**. No radial, no conic, no >2 stops, no angle. |
| **Clipping** | axis-aligned rect only | Cannot clip to a rounded rect, so an image or gradient inside a rounded card has square corners. §3.3's accordion plan (animate a clip rect) works, but a rounded accordion body will not. |
| **Opacity layers** | per-node only | `paint_alpha` multiplies a node's own alpha; there is **no group/layer opacity**, so fading a subtree fades each node independently and overlapping children double-darken. Dialog enter/exit and toast fades both need this. |
| **Transforms** | none except `text_rot` | No scale, translate, or rotate for boxes. §3.5's press-scale (0.98), dialog scale-in, tabs-indicator slide/scale, toast translate, and switch-thumb spring **all require this**. Today the only way to move something is to change `x`/`y`/`w`/`h`, which is a *layout* change — exactly what §3.3 forbids. |
| **Text measure / ellipsis** | measure yes, ellipsis **no** | `metrics.measure` / `measure_wrap` are solid (in-tree via `std:font` when bound, host otherwise). But grep for `ellips`/`truncate` across zeus returns nothing. Overflowing text either wraps or overflows its box. Every table cell, select trigger, chip, and nav label is at risk. |
| **Baseline alignment** | absent | `plat_text` anchors at the top-left of the line box; `paint_label` centers vertically by arithmetic (`(h - pad - th) / 2`). Mixed-size text on one row does not sit on a shared baseline — visible in `Stat`, `Badge`+`Text` rows, and chart axis labels. |
| **Icon rendering** | SVG string re-parsed per draw | Each `Icon` ships a ~200-byte markup string through the draw list every frame, parsed by the host. No path cache, no stroke-width scaling with size, no two-tone icons. |

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

`Palette` is 34 named roles × 2 appearances (`LIGHT` / `DARK`, xAI-derived),
mirrored into `ROLE` slots 1–36 and resolved at paint. Good bones; wrong shape
for a design system.

| §2 requirement | Today |
|---|---|
| 12-step neutral scale | **No scale.** Nine flat neutrals (`bg`, `card`, `raised`, `wash`, `line`, `line_strong`, `text`, `muted`, `faint`). No way to ask for "zinc-700". |
| Accent scale | **No accent at all** in the design sense — `accent` is `#0a0a0a` light / `#ffffff` dark, i.e. neutral ink. The brand hues (`sunset`, `dusk`, `twilight`, `breeze`, `midnight`) are loose `fn`s outside the role system, usable only for charts. An accent *picker* is not expressible. |
| success / warning / danger | Present (`success`, `warn`, `danger` + `_fill` / `_edge` / `_solid` variants). |
| **info** | **Missing.** |
| Live theme switch | ✅ Already ideal — `appearance()` signal + paint-time role resolution. Keep this. |
| Radius none/sm/md/lg/xl/full | `RAD_SM 6 / MD 8 / LG 10 / XL 14 / FULL 999`. **No `RAD_NONE`**, and the ramp is 6→8→10→14 (compressed at the low end). |
| 4pt spacing grid | `SPACE` is `0, 2, 6, 8, 12, 16, 20, 24, 32, 48, 62, 72`. **`6`, `2`, and `62` are off-grid.** |
| Type: sizes, weights, line heights, tabular numerals | Sizes only (`FONT_DISPLAY 30`…`FONT_OVERLINE 11`). **No weights** — `plat_text` carries a pixel size and nothing else, and `plat_set_font_family` is explicitly documented as one global family. **No line heights**, **no tabular numerals** for the data components. |
| Elevation 0–4 | **Absent**, and not expressible without a shadow op. |
| Motion fast/base/slow + easings | **Absent.** Durations are per-site integer literals in `scene.step`. |
| Focus ring color/width/offset | `ROLE.FocusRing` exists (= foreground, both appearances) and `kb_focus` correctly gates it to keyboard focus. **No width token, no offset token** — the ring is drawn inline in `paint_node` at a fixed inset. |
| WCAG AA on every pair | Unverified; no contrast checker in tree. `faint #a9b2bc` on `bg #ffffff` is ≈2.2:1 — **fails AA for text** and is used for captions, overlines, and placeholders. |

---

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
- **No focus trap** in `Dialog`, and **no Esc to dismiss** on `Dialog` or
  `Select`.
- **No 44dp minimum hit target** on touch hosts. `SIZE.Sm` is 32dp,
  `SIZE.Md` 36dp, the dialog close button is 28dp, `Checkbox`/`Radio` boxes are
  16dp, and `Pip` is 8–10dp. iOS and Android get the desktop geometry verbatim.
- Hover is already correctly suppressed on overlay-scroll hosts
  (`paint_hover_wash` early-returns), so "pointer hover only where there is a
  pointer" is half-solved — but `is_hot` still runs the fade math for every node
  on those hosts each frame.

---

## 8. Gallery and fixtures

`examples/zeus/gallery/screen.loam` (500 lines) is three pages — Foundations,
Components, Patterns — behind `NavTab`, with a `Switch(zeus.appearance())` for
dark mode and a live RAM chip. It demonstrates components but not *matrices*:
`Button` shows 6 looks × 3 sizes, but nothing else shows more than one size, and
**no component shows its hover / pressed / focused / disabled / loading /
invalid states side by side**. There is no accent picker (there is no accent to
pick). Five per-host launchers exist (`macos`, `ios`, `android`, `frontend`,
`backend`) plus `ZEUS_HEADLESS=1`.

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
   an ellipsis measure. None of §3.5 or §4 can land first. Four new host ops,
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
