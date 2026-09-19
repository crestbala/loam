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
  `AvatarGroup`, `Skeleton`, `Spinner`, `Switch`, `Slider`, `Progress`.
- **`molecules.loam`** — `Field`, `TextInput`, `TextArea`, `Checkbox`,
  `Radio` / `RadioGroup`, `Toggle` / `ToggleGroup`, `Segmented`, `Choice`,
  `Select`, `Combobox`, `Tabs` / `Tab`, `Alert` (+ variants), `Stat` /
  `StatCount`, `Breadcrumbs`, `User`, `Tooltip`, `HoverCard`, `Toast`,
  `Surface`, `Empty`, `Accordion`, `Collapsible`, `Pagination`.
- **`organisms.loam`** — `Card`, `Comp`, `Menu` (+ rows), `Dialog` /
  `AlertDialog`, `Drawer`, `Popover`, `Navbar` / `NavTab`, `Table` (+ rows /
  cols), the chart shell (`BarChart` / `LineChart` / `AreaChart`),
  `DatePicker`, `Page`.

## Re-export caveat (why the facade exists)

Loam has **no re-export**: a symbol declared in module `M` is only reachable as
`M.name`, and function/method calls additionally require importing `M`. Struct
*type names* resolve transitively (two import levels, plus a global fallback),
but **enum members and globals reached with a module prefix do not** — a
qualified `zeus.LOOK` / `zeus.FONT_BODY` needs the name declared in the module
named `zeus`.

Consequence: `packages/zeus/std/zeus.loam` stays the public module and forwards
the component functions, so existing call sites (`zeus.Card`, `n.width(4)`)
keep working with no consumer changes. See issue notes in the repository for the
open decision on the few public enums/constants.

## Status

Scaffolding only. Code lives in `zeus.loam` until the extraction lands group by
group (text atoms → controls → molecules → organisms), each step verified
against the full `make test` suite and the DRAW goldens.
