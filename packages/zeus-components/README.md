# zeus-components

The Zeus **design system**, split out of `packages/zeus/src/zeus.loam` so that
three concerns with three different rates of change stop sharing one ~11k-line
file:

| Layer | Where | Rate of change |
|---|---|---|
| Reactive core + layout | `packages/zeus/src/zeuscore/*` | almost never |
| Primitives + public vocabulary | `packages/zeus/src/zeusbase.loam` | rarely |
| Control flow (`Each`, `Match`, `Show`, `Boundary`, `Scope`, `VirtualList*`) | `packages/zeus/src/flow.loam` | rarely |
| **Design system** (this package) | `packages/zeus-components/src/*` | constantly (restyle / re-theme) |

Dependency direction is one-way: `zeuscore` ← `zeusbase` / `flow` ←
`zeus-components` ← `zeus` (facade). Components import `zeus:zeusbase` and
`zeus:flow`; they never import `zeus` or `zeuscore/*` (the `nob` arena lint
fails the build if they do). `atoms` imports `flow`, so `zui.Each` /
`zui.Match` resolve through the facade as before.

## Taxonomy

```
packages/zeus-components/
  std/
    atoms.loam        one visual unit each (no component inside)
    molecules.loam    a small group of atoms acting as one control
    organisms.loam    a whole section of a screen
```

- **`atoms.loam`** — type scale (`Overline`, `Display`, `Heading`, `Title`,
  `Body`, `Note`, `SectionHeader`, `SectionTitle`, `Label`), `Icon`, `Kbd`,
  `Pip`, `Separator` / `Divider`, `Badge` / `BadgeCount`, `Chip`, `Avatar` /
  `AvatarGroup`, `Skeleton` (+ `SkeletonText` / `SkeletonAvatar` /
  `SkeletonCard`), `Spinner`, `Progress`, `ProgressRing`, `Card` /
  `CardHeader` / `CardFooter`, `Tag`, `DotBadge`, `Link`, `Code`,
  `Blockquote`, `KbdCombo`, `ScrollShadow` / `ScrollArea`, `Container`,
  `AspectRatio`, `SimpleGrid` / `GridCell`, `Gauge`, `UsageBar`, `Surface`,
  `Empty`.
- **`molecules.loam`** — `FormField`, `Field`, `TextInput`, `TextArea`,
  `Switch`, `Slider`, `Checkbox`,
  `Radio` / `RadioGroup`, `Toggle` / `ToggleGroup`, `Segmented`, `Choice`,
  `Select`, `Tabs` / `Tab`, `Alert` (+ variants), `Stat` /
  `StatCount`, `Breadcrumbs`, `User`, `Collapsible`, `Pagination`,
  `ButtonGroup`, `CopyButton`, `NumberInput` / `Stepper`, `SearchInput`,
  `PasswordInput`, `InputGroup`, `CheckboxGroup`, `RangeSlider`, `Banner` /
  `Callout` / `InlineMessage`, `DescriptionList`, `Steps`, `MultiSelect`,
  `TagInput`, `PinInput`, `Rating`, `CheckboxCard` / `RadioCard`, `List` /
  `ListItem`, `ListBox`, `Timeline`, `TrendIndicator`, `KpiCard`,
  `EmptyState`, `ErrorView`, `LoadMore`, `TableOfContents`.
- **`organisms.loam`** — `Comp`, `Combobox`, `Menu` (+ rows), `Dialog` /
  `AlertDialog`, `Drawer`, `Popover`, `Tooltip`, `HoverCard`, `Toast`,
  `Accordion`, `Navbar` / `NavTab`, `Table` (+ rows /
  cols), the chart shell (`BarChart` / `LineChart` / `AreaChart`),
  `DatePicker`, `Page`, `SplitButton`, `DropdownMenu`, `ContextMenu`,
  `ConfirmDialog` / `PromptDialog`, `Sheet`, `Tree`, `Sidebar` /
  `NavGroup` / `NavItem`, `AppShell` / `MasterDetail`, `SplitPane`,
  `Notification` / `NotificationCenter`, `CommandPalette`.

## The four scales (essentialism)

`spacing.loam` is the whole vocabulary of chrome. Every metric a recipe draws
comes from one of four scales, and a raw number in a component is a bug — the
`design_lint` stanza fails the build on one and names the line.

| Axis | Scale | Read as |
|---|---|---|
| **space** | `Space.Exxxs` 4 · `Space.Exs` 8 · `Space.Xs` 12 · `Space.Sm` 16 · `Space.Md` 20 · `Space.Lg` 24 · `Space.Xl` 32, plus the two steps the enum omits: `GAP_HAIR` 2 and `GAP_SNUG` 6 | `gap =`, every `padding*` / `margin*` |
| **type** | `FONT_OVERLINE` 10 · `FONT_CAPTION` 12 · `FONT_CONTROL` 13 · `FONT_BODY` 14 · `FONT_TITLE` 18 · `FONT_HEADING` 22 · `FONT_DISPLAY` 32 | `font =` |
| **radius** | `RAD_SM` 2 · `RAD_MD` 4 · `RAD_LG` 6 · `RAD_XL` 8 · `RAD_FULL` (a circle, clamped by the host to half the short side) · `RAD_NONE` 0 | `radius =` |
| **stroke** | `HAIRLINE` 1 · `RING_W` 2 | `borderWidth =` |

Three rules fall out of it:

- **One number, one spelling.** A step that `Space` already has is read as
  `Space.*`; `GAP_HAIR` and `GAP_SNUG` exist only because the enum has no 2 and
  no 6. Re-aliasing `Space.Exs` as some other name would put two words on one
  number, which is the drift the scales exist to stop.
- **Zero is a reset, not a size.** `padding = 0`, `gap = 0`, `radius = 0` are
  allowed everywhere, because "none" is a statement about intent.
- **A token nothing reads is also a failure.** `LEAD_*` sat declared and unused
  while every stacked label guessed its own leading; the lint now fails on a
  dead scale token, so a scale cannot quietly rot.

### Type: size, leading, and the tracking gap

Sizes are spaced so a step is *visible*, not merely present. The first cut ran
10 / 11 / 12 and 14 / 17 — a pixel apart, which the eye reads as one size under
three names, so the scale carried no hierarchy. `zeus_control_tokens.loam`
asserts a minimum jump between every pair, so it cannot collapse again.

Leading is paired with size (`lead_for` / `line_box`): display and heading are
`LEAD_TIGHT`, body `LEAD_NORMAL`, caption and overline `LEAD_SNUG` — a caption
stays welded to what it captions.

**Letter spacing is not available, and that is a host limit, not an omission.**
The text op in the frame format is `text x y str rgb font`: it carries a pixel
size and nothing else, and `LookRec.font` holds one number. Per-glyph tracking
therefore needs a new field through the node record, the frame format, and all
six host decoders (`zeus_replay` in C, `decodeFrame` in JS, the four native
hosts). The software rasterizer in `zeusraster` could do it alone, but hosting
it there and not in the decoders would make the same button render differently
per host — the exact inconsistency the design system exists to remove. Until
the format carries tracking, the small steps separate themselves by role
instead: an overline is upper-case, a caption is `dim()`, a control label is
`ink()` — size is one of three signals.


## Motion: a property of the interaction

Motion is not a component's choice, so it is not a `Motion.*` token in a recipe.
There are only a few interactions in the kit, so there are only a few motions, and
each is named for the interaction it answers (`spacing.loam`):

| Token | Answers | Is |
|---|---|---|
| `MOTION_STATE` | a control changing its own state — a check, a dot, a chevron | `Motion.Snap` (a firm spring, no visible overshoot) |
| `MOTION_HOVER` | a pointer settling on something — a hover wash, an underline | `Motion.Fast` (a short time; only opacity moves) |
| `MOTION_ENTER` | a surface arriving — a panel, an accordion body | `Motion.Glide` (long and heavy) |
| `MOTION_EXIT` | a surface leaving | `Motion.Fall` (at once, lands in time) |

Entry is named by the *kind of surface*, not by its flag bits, because that is
what a reader needs to know:

| Token | Surface | Flags |
|---|---|---|
| `ENTER_SURFACE` | a popup that came from a control | `Fade + Pop` |
| `ENTER_SHEET` | a sheet or dialog from an edge | `Fade + Pop + Rise` |
| `ENTER_REVEAL` | a body that unfolds in place (accordion) | `Fade + Reveal` |
| `enter_edge(place)` | a toast, from the edge it lives on | `Fade + Drop/Rise` |

**There is no exit token, and that is the point.** The engine derives an exit
from the entry motion (`enter_motion_leave`), so a surface always leaves the way
it arrived. Writing an exit separately is how the two drift apart.

Two rules the `design_lint` stanza enforces:

- A recipe that writes `transition = Motion.X` or sums `Enter.*` fails the build;
  it must name the interaction.
- A motion token nothing reads also fails, the same as a dead scale token. This
  is why there is no `SHIMMER_PERIOD` here: the skeleton's shimmer is a paint
  sweep the engine owns (`arena.SHIMMER_MS`), so a copy would be a second source
  of truth for a number no recipe reads.

Every motion is paint-only and honours reduced motion, and nothing animates on
first paint — a retained tree that mounts into a live one animates; a cold boot
does not.

## Charts

`BarChart`, `LineChart`, `AreaChart` share one x/y shape (`ChartData`: labels ×
names × flat vals) and one y domain. `DonutChart` is the exception and takes its
own shape (`DonutData`: one label and one value per slice), because a doughnut
has no x axis to share — padding a category grid to describe a list would be the
kind of over-generalisation the rest of this package avoids.

The hover rule is drawn *after* an area chart's fills, not before: an area path
closes down to the plot floor, so a cursor under it is erased below the series
line and reads as a stub. A bar chart leaves its gaps empty and can draw the
cursor first. `zeus_chart_area_cursor.loam` pins both halves of that.

Doughnut slices are separate SVG layers walked as short chords
(`DONUT_STEP` = 6°), the shape every host parser already handles; the ring is
hollow at `DONUT_HOLE` (62%) of the radius.


Every control reads one table in `spacing.loam`, keyed by `Size` (Sm / Md /
Lg): `control_h` (32 / 36 / 44), `control_pad_x`, `control_font` (caption /
body / title), `control_icon` (`IconSize` Sm / Md / Lg), `control_gap`. Height
and padding follow the global `density()` signal in `colors.loam`
(`Density.Compact` / `Standard` / `Comfortable`, −4 / 0 / +4 px); type and
icons do not, because a size change there would reflow the row it sits in.
`control_font` and `control_icon` are *derived* from the scales rather than
hardcoded, so a control's label shares a step with the text around it.

Public components follow one shape (`docs/zeus-components-plan.md` §3):
`Name(<bound state>, <required content>, p: NameProps)`, with `size`,
`disabled`, `label`, and `style` in the props where they apply; named
arguments only reach a trailing props struct, so optional knobs never go in
plain default parameters. `Alert` takes a `Tint`; `Tone` is a deprecated
alias.

Text fields (`TextInput`, `TextArea`, `SearchInput`, `PasswordInput`,
`InputGroup`, `Select`, `Combobox`, `DatePicker`) all take `disabled` (dims
the frame and inerts it), `invalid` (the danger edge), and `label` (the
accessible name; empty falls back to the placeholder).

## Engine behaviour the components rely on

- **Percent widths in a wrapping row share its gaps** (`layout.wrap_child_w`):
  `n` children at `100 / n` percent fill one line exactly, so `GridCell(2)`
  pairs sit side by side under a `SimpleGrid` gap. Outside wrapping rows a
  percent is a plain share of the width.
- **Placeholders paint at half alpha** (`scene.paint_input`), so an empty
  field's hint never reads as a typed value.
- **Width defaults to 100% in a column.** A label that must centre in a
  centred column sets `width = pct(-1)` (content width), as `Gauge`,
  `ProgressRing`, `Empty`, and `EmptyState` do; a track that must fill a row
  adds `grow = 1`, as `Progress` and `Slider` do.

## Host hooks the components rely on

- `copy_text` (`CopyButton`) → `plat_clipboard_write`: the system pasteboard on
  macOS (`zeus_set_clipboard_hook`) and `navigator.clipboard` on the web.
  Other hosts only remember the text; `zeus.clipboard_last()` reads it back.
- `apple_keys()` (`KbdCombo`) → `plat_apple_keys`: Apple builds and Apple
  browsers spell chords ⌘⇧P.
- `on_context_menu` (`ContextMenu`) → `zeus_handle_context_click`: right-click
  and ctrl-click on macOS, `contextmenu` on the web.
- `on_drag` (`RangeSlider`): press and move inside a node, through the
  existing drag path.
- `set_secure` (`PasswordInput`) → `plat_edit_set_secure`: the edit slot is
  drawn and hit-tested as bullets.

## Public entry

Loam has **no re-export**: a symbol declared in module `M` is only reachable as
`M.name`, and a call also requires importing `M`. `zui`
(`packages/zeus-components/src/zui.loam`) is the facade. It imports the three
tiers and declares nothing, so one import reaches every widget:

```
import "zui"
zui.Button("Save", look = zeus.Look.Solid)
zui.Tree(items, selected, open)
```

`packages/zeus/src/zeus.loam` is the runtime (signals, `Box`, `Text`, layout,
input). It does not forward the design-system widgets. `Look`, `Size`, `Tint`,
and the `IC_*` icon strings stay on `zeus`, because those declarations live
in the modules that file imports.

Struct type names used by the widgets (`TreeItem`, `Notice`, `CommandItem`,
`TimelineItem`, `MenuRow`) resolve through the `zui` import. Enum members such
as `zui.Trend.Up` resolve the same way.

## Status

Phase 0 (the size table), Phase 1 (the props contract), Tier A, and Tier B are
in the three tier files. Tier C in `docs/zeus-components-plan.md` is not built.
Headless coverage is `packages/loam/tests/compile_pass/zeus_control_sizes.loam`,
`zeus_control_tokens.loam`, `zeus_control_props.loam`, `zeus_contract.loam`,
`zeus_tier_a_actions.loam`, `zeus_tier_a_display.loam`, `zeus_tier_a_inputs.loam`,
`zeus_tier_b_inputs.loam`, and `zeus_tier_b_layout.loam`. The gallery's
Components page includes the same widgets.
