# Zeus components: minimal redesign — handoff

Status as of 2026-11-01. The working tree is **mid-refactor and does not compile
yet** (a few call sites still use the old helper signatures, listed below).
Nothing is committed. Finish Part A first, then B–D.

## The rules (from the user — follow them everywhere)

1. **No `merge` for the style prop in components.** A component passes its own
   recipe as `style = Style(...)` and the caller's style as `restyle = p.style`
   (`restyle = text_of(p.style)` for a label that should only take the caller's
   `color` / `font`). The engine layers `restyle` over `style` key by key.
   Recipe helpers with variations take a trailing props struct
   (`field_chrome(size, invalid = p.invalid, width = w)`,
   `floating_panel(MENU_MIN_W, width = w, cursor = "pointer")`) or return a
   single value (`invalid_edge(invalid) -> Border`, `row_fill(on) -> string`).
2. **Lifecycle (`effect`, `on_mount`) only for:** data fetching, cleanup of
   resources, state/prop changes that need a re-render (state sync, timers,
   filters), and performance. **Never** for style, ref, or animation
   manipulation. Never put an `effect` inside `on_mount`.
   - Motion is style: `opacity`, `rotate`, `shiftX`, `shiftY` + `transition`
     (ms, `Motion.*`). The first value lands at once; changes ease.
   - Behaviour is props on `Box` (declarative, applied by the engine once
     mounted): `anchor = Anchor(to = ref, place = Place.*, gap = n)`,
     `autofocus = cond`, `trap = cond`, `secure = cond` (also on `Input`),
     `passThrough = true`, `on_resize = fn(r: Rect) {...}`, `on_drag = fn`,
     `on_context_menu = fn`, `on_hover = fn(on: bool)`.
   - Show/hide floating panels with `visible = cond` (not `display` in style).
   - Event handlers (on_click, on_drag, key handlers) may still read refs
     (`rect(r)`, `focus(r)`) — that is event handling, not lifecycle.
3. Keep components small, simple, reusable, minimal in look.

## Already done

Core (`packages/zeus/std`):
- `zeuscore/style.loam`: `Style.rotate / shiftX / shiftY / transition`;
  `merge` doc says it is engine-only; `text_of(Option<fn() -> Style>)`.
- `zeusbase.loam`:
  - `BoxProps.restyle`, `anchor`, `autofocus`, `trap`, `secure`,
    `passThrough`, `on_resize`, `on_drag`, `on_context_menu`; `struct Anchor`.
  - `layered_style(p)` and `install_mounted(node, p)` in `Component`.
  - `st_motion` applies the motion keys in `style_apply`.
  - `TextProps.restyle`.
  - `edit` binding is two-way (a signal set from outside updates the field
    buffer) — so components no longer call `set_input`.
  - A bound native progress bar (kind 6) eases itself (`progress_tween` moved
    here from `flow.loam`).
  - Refs fill at component mount and `ref_node` is a tracked read (earlier
    session); `anim_schedule` and `set_input` ignore node 0.
  - Mount callbacks run `untrack`ed. NOTE: `untrack(mount_fns[i])` silently did
    not call the fn (compiler bug: passing an array element straight as a fn
    arg). Keep `let f = mount_fns[i]; untrack(f)`. Repro: `/tmp/zshot/fnarr.loam`.
- Scrollbar fixes (earlier session): one live "lead" bar per nested scroll
  chain, `input.hit_bar`, thin overlay thumb. Test `zeus_scrollbar_nested.loam`.

Components (`packages/zeus-components/std`):
- atoms: all `merge` removed; Progress (no effect), Link (`on_hover` +
  opacity transition), ScrollArea (`on_resize` props), AspectRatio (`aspect`
  style key), TooltipCaret (`rotate = 45`).
- molecules: all `merge` removed. New `field_chrome(size, FieldChromeProps)`,
  `invalid_edge -> Border`, `floating_panel(min_w, PanelProps)`, `row_fill`,
  `page_style`. Tabs/Segmented: sliding measured indicator removed —
  `TrayItem` (shared by `Tab` / `Choice`) fills the selected item;
  `IndicatorTray` is a plain row. Select: no measuring/effects; menu uses
  `anchor` / `autofocus` / `visible`. Checkbox/Radio marks: opacity
  transition. `DisclosureHead` / `DisclosureBody` shared by Collapsible and
  AccordionItem (chevron is `rotate` + `transition`). PasswordInput uses
  `secure`. RangeSlider uses `on_resize` / `on_drag` props. MultiSelect list is
  an anchored floating panel. NumberInput/SearchInput/TagInput/PinInput/
  InputClear no longer call `set_input`. BtnLook: opacity in the recipe
  (`button_dim` removed).
- organisms: Combobox, ComboRow, Popover (anchors to `host`), Menu (host
  `on_click = toggle`, no `set_on_click`), MenuItem, Drawer (shift +
  transition + autofocus; `drawer_dock` / `drawer_corners` return values;
  `drawer_slide` removed), Tooltip / HoverCard (`hover_intent` helper, host
  `on_hover`, `anchor`, `passThrough`), AccordionItem.

## Part A — finish the refactor (build is broken until this is done)

Remaining sites in `organisms.loam` (grep `merge(\|on_mount\|trap_focus\|table_share`):

1. **DialogShell**: replace the trap `effect` with `trap = open.get()` on the
   panel Box; drop `let panel = ref()`.
2. **Table columns** (`fn` around line 1009 and 1151): replace
   `merge(Style(width = len_px(table_col_width(w)), ...), style_of(table_share(w)))`
   with one `Style(width = if w <= 0 { pct(0) } else { px(table_col_width(w)) }`
   … `grow = if w <= 0 { 1 } else { 0 }, ...)` (check `table_col_width` /
   `len_px` semantics), and delete `table_share`.
3. **DatePicker**: delete the `on_mount(anchor(...))`; trigger
   `style = field_chrome(size, invalid = p.invalid, width = SELECT_W, cursor = "pointer"), restyle = p.style`;
   sheet `style = floating_panel(0, width = DATE_SHEET_W, gap = Space.Exs, padding = 12)`,
   `anchor = Anchor(to = trigger, place = Place.PlaceBelow, gap = 4)`; drop `let sheet = ref()`.
4. **ContextMenu**: move the handler to `on_context_menu = fn() {...}` on the
   region Box; drop `on_mount` and `let region = ref()` (keep `host`, it is
   read in the handler via `rect(host)`).
5. **SplitPane**: `on_drag = fn() {...}` on the handle Box; drop `on_mount`
   and `let handle = ref()`.
6. **CommandPalette**: replace the focus/trap `effect` with `trap = open.get()`
   on the panel (the trap focuses the first focusable — the search input).
   Check golden `golden_w_command` still shows the caret.
7. Any other compile errors: old calls `field_chrome(size)` are fine (props
   default); `floating_panel(x)` is fine. `highlight(...)`, `tab_fill`,
   `page_chrome`, `tray_chrome`, `indicator_chrome`, `button_dim`,
   `drawer_slide` no longer exist.
8. `grep -n "on_mount\|ref_node\|animate_\|watch_hover\|merge(" packages/zeus-components/std/*.loam`
   must come back empty (except `ref_node` in event handlers if any).
9. Examples / tests that called removed helpers or `Tab(..., plain, ...)`
   (old signature had a `plain` bool; new: `Tab(label, sel, i, count, size, underline)`)
   must be updated: `grep -rn "zui\.\(Tab\|Choice\|highlight\|drawer_slide\)" examples packages/loam/tests`.

## Part B — API freeze file

`packages/zeus/docs/api-freeze.md` is checked by `make test` (`api_freeze`
lint). Removing a name requires deleting its line on purpose. Remove:
`zui.highlight`, `zui.tab_fill`, `zui.tray_chrome`, `zui.indicator_chrome`,
`zui.page_chrome`, `zui.table_share` (after A.2), `zui.drawer_slide`,
`zui.button_dim`, `zeus.mount_effect` (if listed), plus any the lint reports.
Append new names under the right headings: `zui.TrayItem`, `zui.row_fill`,
`zui.page_style`, `zui.hover_intent`, `zui.DisclosureHead`,
`zui.DisclosureBody`, `value.text_of`, `Style.rotate`, `Style.shiftX`,
`Style.shiftY`, `Style.transition`.

## Part C — natural colours (not started)

User: colours look dark/grey, "not true colour" — wants colours as they look
in the natural world. In `packages/zeus-components/std/colors.loam`:
- Neutrals: warm stone instead of pure grey (e.g. page `#f7f6f3`, card
  `#ffffff`, rule `#e4e2dc`, text `#1f1d1a`; dark page `#121110`, card
  `#1b1a18`, text `#ecebe7`). Keep the 12-step `NEUTRAL_L/D` ramps.
- Status tiers (`OK_*`, `WARN_*`, `DANGER_*`, `INFO_*`): true natural hues —
  leaf green, lake blue, sunflower amber, poppy red — not the current muddy
  `#2e5c44` / `#7a3030`. Each `ink` must clear 4.5:1 on the page and on its
  own `bg`; `fill` must give white (or near-black for amber) text ≥ 4.5:1.
- Accents (`ACCENT`): keep `Ink` as default, add natural ones (e.g. Ocean
  blue, Forest green, Clay terracotta) with full 12-step light/dark ramps
  (step 9 = solid, 11 = AA text on steps 1–3). Update `accent_count`,
  `accent_name`, `accent_scale_light/dark`.
- Chart series `chart1..5` / `fill1..5` in `LIGHT` / `DARK`: natural palette.
- Run `packages/loam/tests/inlang/zeus_token_tests.loam` (contrast report must
  have no `FAIL`). A quick contrast helper: WCAG relative luminance in Python.

## Part D — verify

- Rebuild goldens after reviewing diffs: there is no bless flag. Script used:
  `/tmp/zgold/one.sh <stem>` compiles+runs one `draw_golden/<stem>.loam` with
  `ZEUS_HEADLESS=1 MAYA_HEADLESS=1` set at **compile and run** time (or goldens
  drift 1–2px), output in `/tmp/zgold/<stem>.out`; copy over the `.txt` once
  the diff is understood.
- Expected golden changes: no sliding tab/select indicator (fill on the item
  instead), MultiSelect list floating, checkbox/radio/chevron via motion keys.
- `mkdir -p out` at repo root (git-ignored; `zeus_raster_blit` needs it).
- Run the full suite once at the end: `make test NOB_ARGS=-j8` (~5 min).
  Do not rebuild `loamc` while it runs.
- Visual check without a window: build a probe with `zeus.window` +
  `zeus.root` + `engine_layout` + `engine_paint` + `raster_ensure` +
  `scene_rasterize` + `raster_dump_ppm`, then `ffmpeg -i x.ppm x.png`
  (text is not rasterized headless; shapes/colours are). Example probes:
  `/tmp/zshot/shot.loam` (light), `/tmp/zshot/shot2.loam` (dark + open menu).
- Update docs: `packages/zeus/docs/component-model.md` (effect vs on_mount
  rule already written; add the declarative props and `restyle`) and
  `packages/zeus/docs/spec.md` (motion keys).

## Don'ts

- Don't write the word "Clay" anywhere in zeus code/docs/tests.
- Don't use `loam-fmt` on these files (it mangles closure indentation).
- Loam compares strings with `fmt.eq`, not `==`.
- `.val` on a *local* `Option<fn>` mis-types; read `p.field.val` directly.
- A component builder must have a single `return` at the end (early returns
  skip mount hooks).
