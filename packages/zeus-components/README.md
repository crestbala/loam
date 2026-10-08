# zeus-components

The Zeus **design system**, split out of `packages/zeus/std/zeus.loam` so that
three concerns with three different rates of change stop sharing one ~11k-line
file:

| Layer | Where | Rate of change |
|---|---|---|
| Reactive core + layout | `packages/zeus/std/zeuscore/*` | almost never |
| Primitives + public vocabulary | `packages/zeus/std/zeusbase.loam` | rarely |
| **Design system** (this package) | `packages/zeus-components/std/*` | constantly (restyle / re-theme) |

Dependency direction is one-way: `zeuscore` ← `zeusbase` ← `zeus-components` ←
`zeus` (facade). Components import `std:zeusbase`; they never import `std:zeus`.

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

## Sizes and the component contract

Every control reads one table in `spacing.loam`, keyed by `Size` (Sm / Md /
Lg): `control_h` (32 / 36 / 44), `control_pad_x`, `control_font`,
`control_icon`, `control_gap`. Height and padding follow the global
`density()` signal in `colors.loam` (`Density.Compact` / `Standard` /
`Comfortable`, −4 / 0 / +4 px). `IconSize`, `AvatarSize`, and `Elevation`
(in `zeus:enums`) are their own px / engine shadow level, so a raw number
still works where one was passed before.

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
`M.name`, and a call also requires importing `M`. `std:zui`
(`packages/zeus-components/std/zui.loam`) is the facade. It imports the three
tiers and declares nothing, so one import reaches every widget:

```
import "std:zui"
zui.Button("Save", look = zeus.Look.Solid)
zui.Tree(items, selected, open)
```

`packages/zeus/std/zeus.loam` is the runtime (signals, `Box`, `Text`, layout,
input). It does not forward the design-system widgets. `Look`, `Size`, `Tint`,
and the `IC_*` icon strings stay on `std:zeus`, because those declarations live
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
