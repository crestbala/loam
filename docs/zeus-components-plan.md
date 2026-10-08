# zeus-components: plan to grow and tighten the design system

Written 2026-10-08 on branch `feat/zeus-ui-v2`.

Reference: [Ely GPUI Components](https://github.com/ZacharyZhang-NY/Ely-GPUI-Components/tree/main/src)
([site](https://elygpui.com/components/)). Ely is a Rust/GPUI library with about 40
modules. Some are general UI (buttons, forms, overlays, layout, data display,
feedback, navigation, menus, motion, typography). Others are whole products
(editor, terminal, finance, git, maps, mail, canvas, and so on).

Goal: more components, consistent sizes, and every component reusable. The aim
is **not** to port Ely. We take its *system* (size tokens, variants, and a
consistent component contract) and the general-purpose part of its catalogue,
then express them in Zeus's own visual language: pill controls, generous radii,
and a 14 px body.

---

## 1. Where we are

Component functions today (atoms / molecules / organisms):

- **Text:** Overline, Display, Heading, Title, Body, Note, Label, SectionHeader, SectionTitle, Kbd
- **Display:** Icon, Pip, Separator/Divider, Badge, BadgeCount, Chip, Avatar, AvatarGroup, User, Stat, StatCount, Sparkline, Skeleton, Spinner, Progress, Meter, Empty, Surface, Card
- **Inputs:** Button, IconButton, TextInput, TextArea, Field, Checkbox, Radio/RadioGroup, Switch, ThemeSwitch, Slider, Toggle/ToggleGroup, Segmented, Choice, Select, Combobox, DatePicker
- **Navigation:** Tabs/Tab, Breadcrumbs, Pagination, Navbar/NavTab, Menu (+ item, label, separator)
- **Overlays:** Tooltip, HoverCard, Popover, Dialog, AlertDialog, Drawer, Toast
- **Disclosure:** Accordion, Collapsible
- **Data:** Table (+ head, rows, cols), VirtualList (×3), VirtualTable, Bar/Line/AreaChart
- **Feedback:** Alert

### Problems found in the code

| # | Problem | Evidence |
|---|---|---|
| P1 | `Size` (Sm/Md/Lg) is used **only by `Button`**. Every other control has a single hard-coded size. | `grep Size\.` in `zeus-components/std` has 6 hits, all in `button_*` |
| P2 | Control heights disagree. A Button next to a Select or TextInput does not line up. | Button 34/40/46 (`molecules.loam:120`); Toggle/Segmented row 36 (`molecules.loam:970,1016`); page buttons 36 (`:1116`); TextInput is padding-derived (10+line+10 ≈ 41); Chip 26 (`atoms.loam:1180`); Badge 18–20 |
| P3 | Widths are fixed where they should flow. | Slider `width = px(220)` (`molecules.loam:263`); Progress `px(220)` (`atoms.loam:1321`); Popover/Menu 200/280/320 literals in `organisms.loam` |
| P4 | Sizes are raw ints instead of tokens. | `Avatar(initials, size: int, …)`; `Icon(markup, px: int, …)`; Switch 36×20 duplicated in `Switch` and `ThemeSwitch` |
| P5 | Signatures are inconsistent. Some components take a props struct, some only positional args, and some have no `style` hook. | `Switch(on)`, `Slider(value)`, and `Checkbox(label, on)` have no props, so there is no `disabled`, `size`, `style`, or a11y label |
| P6 | Missing states. `disabled` exists on Button only, and `invalid`/`error` exists nowhere except as `Field` text. | n/a |
| P7 | There is no density or scale knob. Ely has `Density` (±4 px on controls), `font_scale`, and `radius_scale`. | `spacing.loam` has constants only |

---

## 2. Phase 0: the size system (do this first; everything else builds on it)

Ely's model, from `src/theme/tokens.rs`:

| Token | Ely values |
|---|---|
| `ControlSize` Sm/Md/Lg | height 24/28/32, pad-x 8/12/16, density ±4 / ±2 |
| `IconSize` Xs…Xxl | 12/14/16/20/24/32 × font_scale |
| `AvatarSize` Xs…Xl | 20/24/32/40/56 |
| `TextSize` Xs…Display | 11/12/13/14/16/20/24/32 |
| `Radius` Sm…Xl | 4/6/8/12 × radius_scale |
| `Elevation` | Raised / Floating / Modal (layered shadows, separate for dark) |
| One-off metrics | check 16, radio dot 6, switch 32×18, slider track 4 / thumb 16, meter track 6, progress ring 48, menu min-w 200, tooltip max-w 256, dialog 440, toast 360, table row 28/36/44 by density |

Ely is compact desktop chrome (28 px controls). Zeus is roomier and pill-shaped,
so we keep **our own numbers** and copy the **structure**.

### 2.1 New tokens in `spacing.loam`

```loam
// --- control scale: one table every control reads ------------------------
// Sm / Md / Lg. Button, TextInput, Select, Combobox, Segmented, Toggle,
// Tabs (pill), Pagination, DatePicker trigger, NumberInput all share it.
fn control_h(size: int) -> int      // 32 / 36 / 44   (+ density delta)
fn control_pad_x(size: int) -> int  // 12 / 16 / 24   (+ density delta / 2)
fn control_font(size: int) -> int   // 13 / 14 / 15
fn control_icon(size: int) -> int   // 14 / 16 / 18
fn control_gap(size: int) -> int    // 6 / 8 / 8
```

- Proposed Md = **36**, because most non-button controls already use 36. Button
  Md moves from 40 to 36, and Lg moves from 46 to 44. **Decision needed:** keep
  Button at 40 and raise everything else to 40 instead. Either way there is
  *one* number.
- Add `enum Density { Compact, Standard, Comfortable }` in `enums.loam`, plus
  a global `density()` signal in `colors.loam` next to `appearance()`. The delta
  is −4 / 0 / +4 px. Reading it inside style closures keeps it live.
- Add `enum IconSize { Xs, Sm, Md, Lg, Xl, Xxl }` → 12/14/16/20/24/32, and
  `icon_px(s)`.
- Add `enum AvatarSize { Xs, Sm, Md, Lg, Xl }` → 20/24/32/40/56, plus a `2xl` 72
  for profile headers.
- Add `enum Elevation { Raised, Floating, Modal }` → `shadow_of(level)`. Card,
  Popover/Menu, and Dialog stop choosing shadows ad hoc. This rides on the
  cached nine-slice shadow from `zeus-ui-v2-pending.md` §1.
- Name the one-off metrics: `CHECK = 16`, `RADIO_DOT = 8`, `SWITCH_W/H`
  per size (Sm 28×16, Md 36×20, Lg 44×24), `SLIDER_TRACK = 4`, `SLIDER_THUMB = 16`,
  `METER_TRACK = 6`, `RING = 48`, `MENU_MIN_W = 200`, `TOOLTIP_MAX_W = 256`,
  `TOAST_W = 360`, `SHEET_W = 380`, `PROSE_W = 320`, `TABLE_ROW(density)` 32/40/48.
- Optional, later: `radius_scale` / `font_scale` signals. Hold off until a real
  theme needs them.

### 2.2 Migrate existing controls onto the table

| Component | Change |
|---|---|
| Button / IconButton | `button_height/padding/font/icon` → `control_*`. IconButton gets `size` and becomes square at `control_h`. |
| TextInput / TextArea / Combobox / Select | Fixed `height = control_h(size)` instead of padding-derived; add `size` prop. |
| Segmented / Toggle / ToggleGroup / Tabs (pill) / Pagination | Replace `px(36)` with `control_h(size)`. |
| Chip / Badge | Keep them smaller than controls, but token-driven: Chip `control_h(Sm) - 6`, Badge 18/20/22 by size. |
| Switch / ThemeSwitch | Use `SWITCH_W/H(size)`; ThemeSwitch calls `Switch`, so the duplicate goes away. |
| Checkbox / Radio | `CHECK`, `RADIO_DOT`; label font from `control_font(size)`. |
| Slider / Progress / Meter | Default `grow = 1` (fill parent); width only via `style`. |
| Avatar | `size: int` → `AvatarSize` enum. Keep an `px` escape hatch in props. |
| Icon | Accept `IconSize` (keep the raw-px overload for SVG art). |

**Verification:** `make test`, plus re-baselining the DRAW goldens in a single
commit that contains only the size change, so the diff is easy to review. Add
`tests/compile_pass/zeus_control_sizes.loam`, which asserts that every control's
measured height at Sm/Md/Lg equals `control_h`.

---

## 3. Phase 1: the reusable-component contract

Every public component follows the same shape. This is the rule set for both
new and migrated code. Ely's builder pattern (`.size().variant().disabled()`)
maps onto our props structs.

1. **Signature:** `Name(<bound state>, <required content>, p: NameProps)`. Bound
   state is a `Signal<T>` first (`value`, `open`, `on`). Everything optional
   lives in `NameProps` with defaults, so a call site never needs more than the
   essentials.
2. **Every props struct has** `size: int = Size.Md` (where meaningful),
   `disabled: bool = false`, `label: string = ""` (a11y name),
   `style: Option<fn() -> Style> = none`, and, for form controls,
   `invalid: bool = false`.
3. **Variant axis:** reuse `Look` (Solid/Soft/Outline/Ghost/Danger/Link). Add
   `Subtle` and `Success` (Ely has both). Use `Tint` for status colour. Do not
   add a component-private enum when `Look`/`Tint`/`Tone` already fits. Merge
   `Tone` (atoms) into `Tint` so there is one status enum.
4. **States are drawn, not left to the caller:** hover, pressed, focus-visible
   ring (`FOCUS_WIDTH`/`FOCUS_OFFSET`), disabled (dim + no pointer + skipped by
   roving keys), invalid (danger edge), and loading where an action is async
   (Button `loading`, which keeps its width).
5. **No fixed outer widths.** A component sizes to its content or fills its
   parent. A width is a `style` choice made by the caller. Named defaults
   (`SELECT_W`) are allowed only as `min_width`.
6. **Composition over flags:** build big pieces from small exported ones.
   `Card` = `CardHeader` + body + `CardFooter`; `Field` wraps *any* control
   (label, description, error, required mark) instead of only `TextInput`.
7. **Keyboard + roles:** every interactive component has a role and keys.
   Use `roving_keys` for groups (already in `atoms.loam`) and
   `claim_roving_focus` semantics (no focus stealing; see
   `zeus-ui-v2-pending.md` §2).
8. **Theme only through role accessors** (`ink()`, `plate()`, `tint_*`). No hex
   literals in components; `sunset()` and the other brand constants stay
   demo-only.
9. **Each component ships with** one `compile_pass` test and one gallery entry
   (`examples/zeus/gallery/ui.loam`) showing every size and variant side by side.

### 3.1 Bring existing components up to the contract

- `Switch`, `Slider`, `Checkbox`, `Radio`: add props structs (`size`,
  `disabled`, `label`, `style`), and give Slider `min`/`max`/`step` (it is
  hard-wired to 0..100 today).
- `Checkbox`: add an `indeterminate` state (needed by table select-all and
  `CheckboxGroup`).
- `Field`: generic slot, plus `description`, `error`, and `required`. When
  `error` is set, pass `invalid` down.
- `Card`: split into `Card` / `CardHeader(title, description, action)` /
  `CardFooter`, and add `Elevation`.
- `Alert`: the `kind` int becomes a `Tint`; add `action` and `dismissible`.
- `Progress`: add `indeterminate`, `size`, and an optional label.
- `Tabs`: add a `Look` (pill / underline). Ely's underline uses a 2 px
  `tab_indicator`.
- `Menu` / `Popover` / `Select`: share one `floating_panel(min_w, elevation)`
  helper so radius, padding, and shadow match.

---

## 4. Phase 2+: new components

Picked from Ely's general-purpose modules (`buttons`, `forms`, `primitives`,
`overlays`, `layout`, `data_display`, `feedback`, `navigation`, `menus`,
`lists`, `motion`, `typography`, `interaction`). Each one lands in the tier
where it belongs. **Ely source** is the file to read for behaviour; the
visuals are ours.

### Tier A: high value, small (next)

| Component | Tier | Ely source | Notes |
|---|---|---|---|
| `ButtonGroup` | molecule | `buttons/group.rs` | Attached buttons; first/middle/last radii (Ely's `Slot`) |
| `SplitButton` | molecule | `menus/hosts.rs` | Button + caret that opens a `Menu` |
| `CopyButton` | molecule | `buttons/copy.rs` | Icon swaps to check for 1.5 s |
| `DropdownMenu` / `ContextMenu` | organism | `menus/hosts.rs` | Trigger + existing `Menu`; context = right-click at the pointer |
| `NumberInput` / `Stepper` | molecule | `forms/number.rs`, `forms/stepper.rs` | ± buttons, arrow keys, min/max/step, clamp on blur |
| `SearchInput` | molecule | `forms/search.rs` | Leading icon, clear, `/` or ⌘K focus hint |
| `PasswordInput` | molecule | `forms/input.rs` | Reveal toggle |
| `InputGroup` / addons | molecule | `forms/group.rs` | Prefix/suffix text or icon inside the TextInput chrome |
| `CheckboxGroup` | molecule | `forms/check.rs` | Uses the new `indeterminate` state |
| `RangeSlider` | molecule | `forms/slider.rs` | Two thumbs on the shared Slider track |
| `Tag` / `DotBadge` | atom | `data_display/badge.rs` | Removable tag; status dot + label |
| `Link` | atom | `typography/link.rs` | Inline, underline on hover, external icon |
| `Code` / `Blockquote` / `KbdCombo` | atom | `typography/inline.rs` | KbdCombo renders `⌘⇧P` vs `Ctrl+Shift+P` per platform |
| `Banner` / `Callout` / `InlineMessage` | molecule | `feedback/messages.rs` | Alert siblings: full-width bar, boxed note, one-line form message |
| `ProgressRing` | atom | `motion/progress.rs` | 48 px ring with optional percentage |
| `SkeletonText` / `SkeletonAvatar` / `SkeletonCard` | atom | `motion/skeleton.rs` | Presets over the existing `Skeleton` |
| `ConfirmDialog` / `PromptDialog` | organism | `overlays/dialogs.rs` | Thin wrappers over `DialogShell` |
| `Sheet` | organism | `layout/sheet.rs` | Bottom sheet with grip (36×4); reuses `Drawer` `SideBottom` |
| `DescriptionList` | molecule | `data_display/records.rs` | 160 px label column; wraps on narrow widths |
| `Steps` | molecule | `navigation/steps.rs` | Horizontal/vertical, done/current/upcoming |

### Tier B: richer, medium effort

| Component | Tier | Ely source | Notes |
|---|---|---|---|
| `MultiSelect` / `TagInput` | organism | `forms/multi.rs`, `forms/tags.rs` | Chips inside the field; builds on Combobox filtering |
| `PinInput` | molecule | `forms/pin.rs` | N cells, auto-advance, paste fills all |
| `Rating` | molecule | `forms/rating.rs`, `data_display/stars.rs` | Stars, half steps, read-only mode |
| `CheckboxCard` / `RadioCard` | molecule | `forms/card.rs` | Selectable cards for plan pickers |
| `ListBox` | molecule | `forms/listbox.rs` | Always-open selectable list; Select's menu re-uses it |
| `List` / `ListItem` | molecule | `lists/item.rs` | Leading/trailing slots; the base for menus, settings, and nav |
| `Tree` | organism | `lists/tree.rs` | Expand/collapse, 16 px indent, roving keys, lazy children |
| `Timeline` | molecule | `data_display/timeline.rs` | Dot + rail + content |
| `Gauge` / `UsageBar` | atom | `data_display/measures.rs` | Siblings of `Meter` |
| `KpiCard` / `TrendIndicator` | molecule | `data_display/stat.rs` | Extend `Stat` + `Sparkline` (64×18) |
| `Sidebar` / `NavGroup` / `NavItem` | organism | `layout/sidebar.rs`, `navigation/nav.rs` | 240 / 56 collapsed |
| `AppShell` / `MasterDetail` | organism | `layout/page.rs` | Page frames above `Page` |
| `SplitPane` | organism | `layout/split.rs` | Drag handle (6 px hit), 160 px min pane |
| `ScrollArea` + `ScrollShadow` | atom | `layout/scroll.rs`, `scroll_aids.rs` | Themed 6 px scrollbar, edge fades |
| `Container` / `AspectRatio` / `SimpleGrid` | atom | `layout/stack.rs`, `grid.rs` | Container widths 640/960/1200 |
| `EmptyState` / `ErrorView` | molecule | `feedback/states.rs` | Promote `Empty`: icon, title, prose (320 max), action |
| `Notification` / `NotificationCenter` | organism | `feedback/notification.rs` | Toast stack (360 wide) + history |
| `CommandPalette` | organism | `navigation/palette*` | 600×400 card, fuzzy filter, sections; reuses Combobox filter |
| `TableOfContents` | molecule | `navigation/toc.rs` | Scroll-spy over section anchors |
| `LoadMore` | molecule | `navigation/pages.rs` | Pagination alternative for feeds |

### Tier C: later, or when an app needs it

`Carousel`, `Lightbox`, `Gallery`, `Masonry`, `Tour` / `Spotlight`, `ColorPicker` /
`ColorSwatch`, `DropZone` / `Upload` / `UploadList`, `TimePicker` /
date-range, `Cascader`, `TransferList`, `SortableList` (drag reorder),
`SwipeableListItem`, `FloatingToolbar`, `AnimatedNumber`, `Typewriter`,
`Confetti`, `Watermark`, `QrCode`, `Countdown`/`Timer`.

### Out of scope

These Ely modules are full applications, not design-system pieces:
`editor`, `terminal`, `git`, `finance`, `maps`, `mail`, `chat`, `messaging`,
`canvas`, `documents`, `generative`, `debug`, `devtools`, `project`,
`calendar` (beyond DatePicker), `collab`, `shell`, `account`, `onboarding`,
and `misc`. If any of them is ever wanted, it goes in a separate
package that *consumes* zeus-components.

---

## 5. Ordering and checkpoints

1. **Phase 0, tokens** (§2.1): `spacing.loam` + `enums.loam` only, with no
   visual change. `make test` stays green.
2. **Phase 0, migration** (§2.2): one commit per family (buttons → fields →
   choice groups → switches/checks → avatar/icon). Re-baseline goldens in each
   commit. Add `zeus_control_sizes.loam`.
3. **Phase 1, contract** (§3.1): props structs on Switch/Slider/Checkbox/Radio,
   then Field, Card, Alert, Progress, Tabs, and the shared floating panel.
4. **Tier A**, in table order. Ship each component with a compile_pass test
   and a gallery row.
5. **Tier B**, then Tier C on demand.
6. Update `packages/zeus-components/README.md`, and add facade forwards in
   `packages/zeus/std/zeus.loam` for each new public name (needed because there
   is no re-export; see the README caveat).

## 5a. Status (audited 2026-10-08)

**Built:** Phase 0 (tokens and migration), Phase 1 (§3.1), Tier A, and Tier B.
Every component named in §4 Tiers A and B exists, has a `compile_pass`
test, and appears in the gallery (`examples/zeus/gallery/screen.loam`:
`Catalogue()` and `Primitives()`). Tier C is not started.

**Done differently from the plan, on purpose:**

- *Facade forwards in `zeus.loam`* (§5 step 6): replaced by `std:zui`, which
  imports the three tiers so one import reaches every widget. `zeus.loam`
  stays the runtime (see the README's "Public entry").
- *`icon_px(s)` and `shadow_of(level)`*: not needed. `IconSize` members are
  their px and `Elevation` members are the engine's shadow level, so the
  enum value is passed straight through.
- *Gallery in `ui.loam`* (§3 rule 9): the rows live in `screen.loam`;
  `ui.loam` is only the app entry.
- *Open decisions:* Md is 36 (Button moved to it); density is a global
  signal only; `Tone` stays as a deprecated alias of `Tint` (no example
  uses it any more); props structs, not builders.

**Contract gaps still open (§3 rule 2).** The text-field family now takes
`disabled` / `invalid` / `label` (`TextInput`, `TextArea`, `SearchInput`,
`PasswordInput`, `InputGroup`, `Select`, `Combobox`, `DatePicker`). These
still lack parts of rule 2:

| Missing | Components |
|---|---|
| `style` | `AccordionItem`, `Blockquote`, `ButtonGroup`, `CardHeader`, `CardFooter`, `CommandPalette`, `ConfirmDialog`, `PromptDialog`, `ContextMenu`, `Dialog` / `AlertDialog`, `DropdownMenu`, `SplitButton`, `KbdCombo`, `MenuItem`, `NavGroup`, `Pagination`, `ProgressRing`, `Sheet`, `Steps`, `Tabs`, `Tooltip`, `User`, `DescriptionList` |
| `disabled` / `label` | `Segmented`, `Toggle` / `ToggleGroup`, `IconButton` (also `size`) |
| props struct at all | `RadioGroup` |
| `label` (a11y) | `Checkbox`, `Radio`, `CheckboxCard` / `RadioCard`, `CheckboxGroup` |

## 6. Open decisions

- **Control Md height:** 36 (match the existing fields) or 40 (match the
  existing Button)?
- **Density:** global signal only, or also a per-subtree override (e.g. a
  compact table inside a standard page)?
- **`Tone` → `Tint` merge:** this touches Alert/Badge call sites in the
  examples. Do it now or with a deprecation alias?
- **Builder vs props:** keep props structs (current Loam idiom). Ely's chained
  builders do not map onto Loam's named-arg calls, so props is the recommended
  choice.
