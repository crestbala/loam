# Zeus component model — state, controller, keys, view

How a Zeus component should be written, why the current one is hard to write,
and the compiler and runtime work that gets us there. The target is
React's *shape*: a component is a function, it has a clear anatomy, the tree is
written top-down in one place, and data flows one way from parent to child.
None of React's *machinery* comes with it: no VDOM, no re-render, no hooks
order, no dependency arrays (see `docs/loam_zeus_v2.md` Part 4). Zeus stays
a retained arena with fine-grained signals.

Line anchors are for `feat/zeus-arch-changes` after the rebase onto
`d37c026d` (Loam `Option<T>` / `Result<T>`).

---

## 0. Summary

A component is one function in four labelled sections, ending in a returned
tree:

```loam
fn Counter(p: CounterProps) -> Node {
    // state — props, signals, derived values, constants
    let count = signal(p.start)
    let even = computed(fn() => count.get() % 2 == 0)

    // controller — handlers, effects, plain helpers
    fn add(d: int) {
        count.set(count.get() + d)
    }

    // keys — keyboard actions on this component
    let keys = []KeyBind {
        chord("up", "counter.inc", fn() => add(1)),
        chord("down", "counter.dec", fn() => add(-1)),
    }

    // tree — the whole tree, nothing else
    return Row(keys = keys, focusable = true,
               style = Style(gap = 8, align = Align.Center)) {
        Button(label = "-", on_click = fn() => add(-1))
        Text("{{count.get()}}", style = Style(
            font = FONT_TITLE,
            color = if even.get() { ink() } else { accent() }))
        Button(label = "+", on_click = fn() => add(1))
    }
}
```

What changes:

| Today | Target |
|---|---|
| `let n = Box(...)` + `ui_push(n.id)` + `ui_pop()`, `slot(n, ...)`, trailing blocks — three ways to nest | Only trailing blocks, under one `return`ed element |
| `arena.nodes[id].a11y_role = ...`, `focusable(n)`, `grow(n, 1)`, `marginLeft(n, -px)` after the build | Everything is a prop on the element. Components never touch the arena |
| A flat 91-field `BoxProps`, re-declared in parts by 39 `*Props` structs | Element props (`ref`, `role`, `label`, `keys`, …), one shared `Style`, handlers `on_*` |
| `-1` / `""` sentinels and 96 `px_or_default` / `str_or_default` calls | `Option<T>` fields and one `merge(base, over)` |
| 58 `foo__set: bool` companions | `Option<fn()>` — `__set` goes away from the language |
| The 5-field key block copied 17 times, then `apply_key(n, spec, action, fn, true, false)` | `keys = []KeyBind { chord(...) }` |
| `selected_chrome(node, label, is_on, on_bg, off_bg, on_fg, off_fg)` | Reactive `style`: `background = if active() { plate() } else { raised() }` |
| `show: bool` (read once) vs `visible: Signal<int>` vs `If(sig)` vs `show_eq/ne/ge/le` | `Show(when = ...)` and `Style(display = ...)`, both reactive |
| `slot_into` redirect to place the trailing block above a footer | Named slots: `header = || { }`, `footer = || { }` |
| `AlertDestructive`, `AlertWarning`, … as separate functions | `Alert(tone = Tone.Danger)` |
| snake_case for everything (`align_direction`, `padding_x`, `a11y_role`) | camelCase for props and style keys; snake_case only for handlers (`on_click`) and functions |

---

## 1. What is there today

Numbers are over `packages/zeus-components/std/*.loam` plus
`packages/zeus/std/zeusbase.loam`.

### D1. Three ways to build one tree

The parser turns `C(...) { body }` into `slot(C(...), || { body })`
(`packages/loam/src/parser.c:715-728`). That part is right. But a recipe often
needs a handle to the node it is building, so it falls back to doing it by hand:

```loam
// molecules.loam:696 — Tab
let tab = Box(align_direction = DIRECTION.Row, ..., cursor = "pointer")
ui_push(tab.id)
let text = Text(label, font = FONT_BODY, ellipsis = true)
ui_pop()
selected_chrome(tab, text, || { selected.get() == index }, plate(), raised(), ink(), mute())
apply_key(tab, "right", "tab.next", fn() { roving_step(selected, index, count, 1) }, true, false)
apply_key(tab, "left", "tab.prev", fn() { roving_step(selected, index, count, -1) }, true, false)
roving_focus(tab, selected, index)
```

There are 46 `ui_push` calls in the library and 29 in `examples/zeus` (the
dracula `NavLink` copies the Tab pattern). `dialog_shell`
(`organisms.loam:1050`) mixes all three styles in one function: `ui_push(dim.id)`,
`slot(panel, || { Box(...) { ... } })`, then `ui_pop()`. You can only see the
tree by reading the push/pop pairs.

**Cause:** a handle is needed for anything that is not a prop. That covers
selection chrome, key bindings, roving focus, a11y role, focusability,
measuring and motion. Make those props (or a `ref`) and the hand-built version
has no reason to exist.

### D2. Props, style, events and keys in one flat bag

`BoxProps` (`zeusbase.loam:1929`) has 91 fields. Layout, paint, text, a11y,
events, key bindings, binding and list-rebuild hooks are all side by side.
Every component then declares its own subset by hand. `ButtonProps`,
`IconButtonProps`, `AlertProps` and `FieldProps` each repeat the same 12-line
"Style props" block with the same doc comment, and 17 structs repeat the same
5-field key block. Forwarding is also by hand: `Field` passes 12 style fields
to `TextInput` one at a time (`molecules.loam:627`), and forgetting one silently
drops it.

Naming is not consistent either. `padding_x` sits next to `marginX`,
`set_padding` next to `margin`, `align_direction` (a field) next to `DIRECTION`
(the enum) and `flex_row()` (a fn). There is `a11y_role` and also
`arena.nodes[...].a11y_role`.

### D3. Sentinels instead of absence

"Unset" is spelled `-1` for numbers and `""` for strings. A recipe layers its
own look under the caller's with `px_or_default(p.radius, RAD_MD)`, which
appears 96 times. Two real costs:

- **A negative value cannot be a prop.** `Card` bleeds its media strip with
  `marginLeft(strip, -px)` *after* the build. The comment there says so: "Props
  drop negatives (-1 = unset), so the bleed is set directly" (`atoms.loam:2988`).
- **The cascade lives inside each recipe.** Every component re-implements
  "caller wins, else mine" field by field.

### D4. `__set` companions

`typecheck.c:2849` sets `foo__set = true` when the caller passed `foo`. It
exists because a function value cannot be compared with `noop`, so there was
no way to tell "handler given" from "default". There are 58 of these fields.
Each one is declared by hand, forwarded by hand
(`on_click__set = p.on_click__set`), and drives behaviour: `Card` becomes
interactive only when `p.on_click__set`, `apply_key` falls back to `on_click`,
and `Button`'s press scale keys on it.

Loam now has `Option<T>` (`std:result`). `__set` is a workaround for something
the language can express directly, so it goes away (§7).

### D5. Imperative patching after the build

There are 349 direct `arena.nodes[...]` accesses in the library. Many are
legitimate engine code (list reconciliation, measuring), but many are
component code doing what a prop should do:

```loam
arena.nodes[hd.id].a11y_role = "heading"       // atoms.loam Card
arena.nodes[n.id].press = u8(1)                // Card
arena.nodes[btn.id].focusable = 0              // input_clear
arena.nodes[n.id].slot_into = body.id          // Card footer, atoms.loam:3023
```

On top of these, every call like `grow(head, 1)`, `width_pct(panel, 92)`,
`focusable(close)`, `enter_fade(card)` or `z_index(scrim, Z_SCRIM)` is a style
or behaviour applied out of line, away from the element it changes.

### D6. Key bindings

Key binding is "a prop on every widget". In practice that means five fields
(`key`, `key_action`, `on_key`, `on_key__set`, `key_global`) copied into 17
structs, each followed by
`apply_key(__n, p.key, p.key_action, p.on_key, p.on_key__set, p.key_global)`.
A widget with more than one binding (Tab, Choice, Select, Dialog) calls
`apply_key` positionally with `true, false` literals. A widget's own bindings
and the caller's are not kept apart, and you cannot list a component's
shortcuts without reading the code.

`key` also collides with the name every other UI system uses for list
identity (React `key`, our own `For(items, key, ...)`).

### D7. Two reactivity rules for props

- `text` and `label` are `fn() -> string`, and the compiler wraps a plain
  expression in a closure that runs it later (`typecheck.c:1124`,
  `wants_thunk`). So `Text("n = {{count.get()}}")` is reactive for free.
- Every other style prop is plain data read once at build. A fill that follows
  state therefore needs an effect, and `selected_chrome` is that effect with
  seven positional arguments, called from Tab, Choice, SelectRow, NavLink and
  the page buttons.
- Visibility has four spellings: `show: bool` (read once), `visible:
  Signal<int>`, `If(Signal<int>)`, and `show_eq/ne/ge/le(node, sig, v)`.
- 0/1 `Signal<int>` stands in for booleans everywhere (`open`, `on`,
  `loading`), with `1 - on.get()` to toggle.

At app level this turns into "generation counters": the dracula app keeps
plain module globals and bumps a `Signal<int>` so that a
`Match(gen, fn(g) { if g >= 0 { paint() } })` rebuilds
(`examples/zeus/dracula/components/stocks.loam`,
`routes/backtest/page.loam`). That is a coarse rebuild, used because the state
is not in signals.

### D8. Slots

A component has exactly one place for children, the trailing block. `Card`
needs a body *between* its header and footer, so it builds an inner column and
redirects the caller's block into it with `slot_into` (`atoms.loam:3023`).
Popover, Menu and Tooltip take a pre-built `trigger: Node` as a positional
argument instead, which means the caller builds the trigger first, outside any
tree.

### D9. Variants as functions, flags as ints

`Alert`, `AlertDestructive`, `AlertWarning`, `AlertSuccess`, `AlertInfo` are five
functions with one body, plus a `kind` prop that overrides them. `Tab(..., plain:
int = 0)`, `Navbar(brand, dark: int = 1)`, `multiline: int`, `lazy: int` and
`wrap: int` are booleans typed as `int`.

### D10. Ownership is implicit in the build stack

`effect_owner()` (`zeusbase.loam:152`) is "the top of `ui_parents`". A signal or
effect created *before* a component's first `ui_push` is therefore owned by the
**parent's** slot. It outlives the component and is freed only when the parent
rebuilds. That is why ownership and push/pop are tangled: the push/pop pairs
are also scope markers.

---

## 2. Goals

1. **The tree is written once, top-down, in one block.** No handles needed to
   assemble it, and no push/pop.
2. **Every component has the same anatomy.** State, controller, keys, tree, in
   that order. A reader always knows where to look.
3. **Props, style and handlers are separate parameters,** the same split as
   `<button disabled style={{...}} onClick={...}>`.
4. **CSS vocabulary, kept small.** Flexbox, a minimal grid, box model,
   position, paint, text. About 35 style keys, not 91. If CSS has a name for
   something, use that name.
5. **Every style value can be reactive,** with no API difference between
   static and reactive values. Reactivity stays per attribute.
6. **Absence is `Option`.** No sentinels in public types, no `__set`.
7. **Components never touch the arena.** `ui_push`, `ui_pop`, `slot`,
   `arena.nodes[...]`, `intern_*`, `prop_int` and `Component(raw_*())` are
   element-layer internals (§6).

Non-goals: CSS strings or class names, selectors, a cascade beyond
`color` / `font` inheritance, `!important`, a style-sheet object. Composition
is the reuse mechanism, as in React Native.

---

## 3. Anatomy of a component

```loam
/// One-line purpose. What the trailing block is, if there is one.
fn Name(p: NameProps) -> Node {
    // ── state ──────────────────────────────────────────────
    //   props unpacked, signal(...), computed(...), ref(), constants

    // ── controller ─────────────────────────────────────────
    //   handlers (fn on_x() { ... }), effect(...), on_mount(...),
    //   plain helpers that read or write the state above

    // ── keys ───────────────────────────────────────────────
    //   let keys = []KeyBind { chord(...), ... }

    // ── tree ───────────────────────────────────────────────
    return Element(...) {
        ...children...
    }
}
```

Rules:

- **Order is fixed.** State → controller → keys → tree. A small component may
  leave sections out, but never reorders them.
- **The tree is a return.** `return Element(...) { ... }` — the single element
  the component builds, with its children as a trailing block. In the tree you
  can call elements and components, use `Show`, `For`, `Match`, `Loop`, and
  write plain `if` / `let` for *build-time* decisions. You **cannot** declare
  signals, effects, named `fn` handlers or key tables there. Those go above.
- **One root.** The returned element is the one root. A component that
  wants several siblings returns a `Row` / `Column`.
- **No void components.** A component always returns `Node`. A function that
  builds UI must `return` it; there is no "emit into the caller's slot and
  return nothing" form. Framework callbacks that mount subtrees
  (`Loop` / `For` / `Match` / `Boundary`, `Route.build`, `zeus.App`) all take
  `fn(...) -> Node` for the same reason — a plain `fn` returning nothing is
  still accepted there, but a real component is `-> Node`.
- **Handlers are local `fn` declarations** in the controller:
  `fn add(d: int) { ... }`, not `let add = fn(d: int) { ... }`. An inline
  `fn() => x.set(1)` in the tree is fine for a one-liner.
- **`View` is gone.** There is no `View` keyword: a component returns its
  element directly, and `View` is an ordinary identifier like any other.
- **Components are `TitleCase`.** Anything that builds UI is named like the
  elements it returns (`Box`, `Card`, `Page`, `Layout`, `NotFound`). Route
  handlers follow the same rule: `Page`, `PageLive`, `Layout`, `Error`,
  `Loading`, `NotFound`. Only non-UI helpers stay `snake_case` — node setters
  (`show`, `grow`), pure helpers (`button_height`), and the data-returning route
  hooks (`meta`, `paths`, `load`).
- **Helpers stay plain functions.** Small pure functions (`button_height(size)`)
  go below the component, snake_case, as today.

### 3.1 State

```loam
let open = signal(false)                       // Signal<bool>, not Signal<int>
let query = signal("")
let matches = computed(fn() => filter(p.options, query.get()))
let trigger = ref()                            // filled when the view builds
let count = p.labels.len                       // plain constant
```

- `Signal<bool>` replaces 0/1 `Signal<int>`. A boolean prop that follows state
  is a `fn() -> bool` thunk, so a plain expression is wrapped automatically.
  *Landed* for every on/off state in the library: `Switch`, `Checkbox`,
  `Toggle`, `Collapsible.open`, `Popover.open`, `Drawer`, `Toast`, `Dialog`,
  `AlertDialog`, `Button.loading`, `Alert.dismiss`, and `DatePicker`'s sheet.
  Indices stay `Signal<int>` (`Menu.active`, `Accordion`, `Tabs`, `Select`).
  Where the engine reads an int signal raw (`visible`, `bind`, the
  enter / exit tracks), the component keeps a 0/1 mirror with
  `flag_int(b)`. The engine's Escape now dismisses an overlay through its
  own `on_click` when it has one, so it writes the component's `bool`
  rather than the mirror. The light / dark switch is `ThemeSwitch()`:
  `appearance()` is an engine-level int signal.
- **Controlled vs uncontrolled**, as in React: a value prop that is
  `Option<Signal<T>>` is controlled when present. When absent, the component
  creates its own:
  `let value = some_or(p.value, signal(p.initial))`.
- `ref()` returns a `Ref`, a Copy handle (`struct Ref { id: int }`) that the
  element with `ref = r` fills. It is how the controller measures or focuses a
  node. It replaces `let n = Box(...)` used only to get an id.

### 3.2 Controller

```loam
fn close() { open.set(false) }
fn pick(i: int) {
    p.value.set(i)
    close()
    emit_with(p.on_change, i)                 // Option<fn(int)>: a no-op when absent
}
effect(|| {
    if open.get() { focus(trigger) }          // reads a signal → re-runs
})
on_mount(|| { place_indicator(0) })          // once, after the view is attached
on_cleanup(|| { release_trap(panel) })        // when the component is freed
```

- **Local `fn`.** `fn name(params) -> R { body }` inside a function body is
  sugar for `let name = fn(params) -> R { body }`. It is an ordinary Loam
  closure, with the same rules: it captures by copy when it is declared (signals
  and refs are handles, so they stay live), it may call any local `fn` declared
  *before* it, and it cannot call itself (no recursion. Use a top-level `fn`
  for that). Passing it on is just its name: `on_click = close`.
- `emit(h)` / `emit_with(h, v)` invoke an optional handler (*landed*). They
  replace `if p.on_x__set { p.on_x() }`.
- `on_mount` is new. It runs after `View` has attached the root, so refs are
  live. It is where `roving_focus`, `claim_trap` and the first measure go.
  It replaces "wire it up after `ui_pop()`".
- `effect` / `computed` / `on_cleanup` keep today's semantics
  (`packages/zeus/docs/reactivity.md` phases 7–8). What changes is the owner (§6.3).

### 3.3 Keys

```loam
let keys = []KeyBind {
    chord("right", "tabs.next", fn() => select(p.selected.get() + 1)),
    chord("left",  "tabs.prev", fn() => select(p.selected.get() - 1)),
    chord("escape", "dialog.close", close),
    chord_global("cmd+k", "palette.open", fn() => open.set(true)),
}
```

*Landed in phase 3* (`zeusbase.loam`, "key bindings").

```loam
struct KeyBind {
    chord: string,                              // "cmd+s", "ctrl+shift+p", "g d"
    action: string = "",                        // remappable name (remap_key); "" = "key.<chord>"
    run: Option<fn()> = none,                   // absent = the element's on_click
    global: bool = false,                       // fires wherever focus is
}
fn chord(spec: string, action: string, run: fn()) -> KeyBind
fn chord_global(spec: string, action: string, run: fn()) -> KeyBind
fn chord_click(spec: string) -> KeyBind        // presses the element: runs its on_click
fn keys_concat(own: []KeyBind, more: []KeyBind) -> []KeyBind
fn install_keys(node: Node, keys: []KeyBind) -> Node   // element layer only
```

- The struct is `KeyBind` because `Key` is already the key-code enum
  (`e.key == Key.K`), and the constructor is `chord` because `bind(node, sig)`
  already exists.
- An element takes `keys: []KeyBind`. That is the only key-related prop. The
  `key` / `key_action` / `on_key` / `on_key__set` / `key_global` block (17
  copies) and `apply_key` are gone. A button's shortcut is
  `Button(label = "Save", on_click = save, keys = []KeyBind { chord_click("cmd+s") })`.
- A component's own bindings and its caller's are **concatenated**:
  `keys_concat(own, p.keys)`. Both sets stay active.
- Popover / Menu / Drawer's close chord is the plain prop `closeKey`
  (default `"escape"`). It configures the component's own binding, so it is
  not a `KeyBind`.
- Because a component's bindings are now a list, devtools and a
  "keyboard shortcuts" sheet can list them per component.
- Converting this turned up three bugs, all fixed. Themed `Button` (every
  `look`), `IconButton`, `Card`, `Field` and `Image` declared the key props
  but never installed them, so their shortcuts were silently dead.
  `compile_pass/zeus_key_prop` had been passing by accident (the focusing
  click accounted for the count it expected from the chord). It now checks
  the click and the chord separately, and also covers `Card`, `IconButton`
  and `keys_concat`.
- `key` (singular) is kept for list identity only.

### 3.4 Tree

```loam
return Column(role = "dialog", ref = panel, keys = keys, focusable = true,
       style = Style(gap = 16, padding = edges(24), maxWidth = px(DIALOG_W),
                     width = pct(92), background = plate(),
                     border = line(HAIRLINE, rule()), radius = corners(RAD_LG),
                     elevation = 4)) {
    Row(style = Style(gap = 8, align = Align.Center)) {
        Title(p.title)
        Grow()
        if p.closable {                                  // build-time branch
            IconButton(icon = IC_X, label = "Close", on_click = close)
        }
    }
    p.children()                                         // the caller's block
}
```

What the tree can express:

| Need | Write | Re-runs when |
|---|---|---|
| Static branch | `if cond { A } else { B }` | never (build time) |
| Reactive show/hide, keep subtree | `Show(when = open.get()) { ... }` | `when` changes. Toggles layout only, the subtree is kept (today's `If`) |
| Reactive show/hide, build on demand | `Show(when = open.get(), lazy = true) { ... }` | Builds on first true, frees on false |
| One-of-N | `Match(tab, fn(t) { ... })` | `tab` changes. Only the current arm is mounted |
| Fixed list | `Loop(items, fn(i, it) { ... })` | never |
| Reactive list | `For(items, key = |it| it.id, fn(i, it) { ... })` | O(changes), rows move with their state |
| Reactive text | `Text("{{count.get()}} items")` | the signal |
| Reactive style | `style = Style(color = if on() { ink() } else { mute() })` | the signals it reads, per attribute (§4.4) |
| Children / named slots | `p.children()`, `p.footer()` | — |

`Show` replaces `If`, `show_eq/ne/ge/le`, `show: bool` and `visible: Signal<int>`.
`Show(when = tab.get() == 2)` is the general form, so the four comparison
helpers are not needed.

*Landed (R6):* `Show(when, lazy, style) { ... }` in `atoms.loam`. `If` is
removed. The host is a column that is out of layout (no gap slot) while
`when` is false. `lazy = true` puts a `Match` inside it, so the body is built
on the first true and freed on each false. `when` is turned into a 0/1 signal
by an effect, not a `computed`, because layout reads the visibility signal
raw and a memo only refreshes when pulled. Apps, `www` and the tests use
`Show` or a reactive `Style(display = ...)`. `show` / `show_eq` / `show_ne` /
the `visible` prop stay as element-layer internals (the chart's light/dark
series, `BadgeCount`). Test: `compile_pass/zeus_show`.

---

## 4. Props, style and events

An element call takes four kinds of argument, the same split React uses:

```loam
Box(
    ref = panel, role = "dialog", label = "Settings",   // element props
    focusable = true, disabled = !valid.get(),
    keys = keys,                                         // key bindings
    style = Style(gap = 16, padding = edges(24)),        // style
    hoverStyle = Style(background = raised()),           // state styles
    on_click = close,                                    // handlers
) { ...children... }
```

### 4.1 Element props (the "DOM attributes")

*Landed in phase 7.* `BoxProps` holds element props only; how an element looks
is its `style`. `Text`, `Input`, `Image`, `Icon`, `Grid`, `Overlay`, `Scroll` and
`Svg` take the same `style`, plus the few element props that apply to them.

| Prop | Type | Replaces |
|---|---|---|
| `style` | `Option<fn() -> Style>` | 48 flat style fields (`width` … `z_index`, `show`, `cursor`) |
| `ref` | `Option<Ref>` | `let n = Box(...)` for a handle |
| `keys` | `[]KeyBind` | the key block and `apply_key` |
| `role` | `string` | `a11y_role`, `arena.nodes[..].a11y_role` |
| `label` | `Option<fn() -> string>` (a plain string is lifted) | `a11y_label`, reactive a11y labels |
| `enabled` | `bool` | dims to 40% and disables (a style `opacity` still wins) |
| `loading` | `Option<fn() -> bool>` | `loading(n, b)` |
| `focusable`, `roving` | `Option<fn() -> bool>` | `focusable(n)`, `arena.nodes[..].focusable`, `roving_focus` |
| `hoverOff`, `hoverFade`, `pressScale`, `lift`, `press` | `bool` / `int` | `hover_off`, `hover_fade`, `press_scale`, `hover_lift` — interim until the state styles (§4.6) |
| `enter` | `int` (`Enter.Fade + Enter.Reveal`) | `enter_fade(n)`, `enter_reveal(n)` |
| `visible`, `bind`, `text`, `edit` | `Option<…>` | the signal and text plumbing of elements |
| `multiline`, `fit`, `lazy`, `safe`, `rowHeight`, `rebuild` | data | element-kind specifics (input, image, safe area, list hosts) |
| `on_click`, `on_mouse_down`, `on_mouse_up`, `on_hover`, `on_key_down`, `on_key_up`, `on_key_press` | `Option<fn()>` (`on_hover: Option<fn(bool)>`) | the `on_*` + `on_*__set` pairs |

A component's props struct holds **only its own data** plus three shared fields:

```loam
struct TabsProps {
    selected: Signal<int>,                         // required: no default
    labels: []string,
    size: Size = Size.Md,
    on_change: Option<fn(int)> = none,
    style: Option<fn() -> Style> = none,           // merged onto the root
    keys: []KeyBind = no_keys(),                   // appended to its own
}
```

No component re-declares `width`, `radius`, `background`, and so on. They are all
inside `style`. Forwarding is one field: `Field` passes `style = p.style` to
`TextInput` instead of 12 fields. A recipe reads it with `style_of(p.style)`,
which is `no_style()` when absent. Optional fields default to a bare `none`
(C4), with no placeholder value. *Landed in phase 7* for every component
(Button, IconButton, Alert, TextInput, Field, Chip, Badge, Card, Empty,
Collapsible, Combobox, Accordion, Popover, Menu, Drawer, HoverCard, Toast).
Data that only looks like style stays data: a floater's `gap` (the anchor
offset), `Drawer`'s `radius` (the sheet's inner edge), `TableCol.width`,
`Spinner`'s `size` / `color`, `Card`'s `mediaHeight`.

### 4.2 Naming

| Kind | Case | Examples |
|---|---|---|
| Components / elements | PascalCase | `Box`, `Tabs`, `DatePicker` |
| Props and style keys | camelCase | `maxWidth`, `zIndex`, `hoverStyle`, `focusable` |
| Handlers (props that take a fn the component calls) | snake_case, `on_` prefix | `on_click`, `on_change`, `on_open_change` |
| Functions, helpers, signals, locals | snake_case | `button_height`, `place_indicator`, `open` |
| Enums / members | PascalCase | `Display.None`, `Direction.Row`, `Align.Center`, `Tone.Danger`, `Look.Solid`, `Size.Md`, `Space.Sm`, `Place.PlaceBelow`, `Motion.Base` (*landed in phase 7*; the engine-internal `SET` and `ROLE` keep their names) |
| Constants | SCREAMING | `RAD_MD`, `FONT_BODY` |

So at a call site, the snake_case names are exactly the ones that run code:
`Button(label = "Save", size = Size.Sm, on_click = save)`.

### 4.3 `Style`: the CSS subset

*Landed in phase 2:* `packages/zeus/std/zeuscore/style.loam`.

One struct, every key `Option<T>`. An absent key leaves the element's own value
and a present key wins, so a negative margin or a zero radius is an ordinary
value, not an "unset" sentinel. There are 33 keys, grouped as CSS groups them,
and only properties the engine actually implements are included:

```loam
struct Style {
    // display and flex  (display, flex-direction, flex-wrap, gap, align-items,
    //                    justify-content, align-self, flex-grow, flex-shrink)
    display: Option<int>,        // Display.Flex | Display.None (hidden, out of layout)
    direction: Option<int>,      // Direction.Row | Direction.Column
    wrap: Option<bool>,          // flex-wrap on a container, text wrap on a label
    gap: Option<int>,
    rowGap: Option<int>,
    columnGap: Option<int>,
    align: Option<int>,          // align-items: Align.*
    justify: Option<int>,        // justify-content: Align.*
    alignSelf: Option<int>,
    grow: Option<int>,
    shrink: Option<int>,

    // grid child  (grid-column: span n)
    span: Option<int>,

    // box  (width, height, min/max, aspect-ratio, padding, margin)
    width: Option<Len>,          // px(240) | pct(50)
    height: Option<Len>,
    minWidth: Option<int>,
    maxWidth: Option<int>,
    minHeight: Option<int>,
    maxHeight: Option<int>,
    aspect: Option<Ratio>,
    padding: Option<Edges>,
    margin: Option<Edges>,       // negatives allowed (a bleed)

    // position  (position, top/right/bottom/left, z-index, overflow)
    position: Option<int>,       // Position.Relative | Position.Absolute
    inset: Option<Edges>,
    zIndex: Option<int>,
    overflow: Option<int>,       // Overflow.Visible | Hidden | Scroll | Auto

    // paint  (background, color, opacity, border-radius, border, box-shadow)
    background: Option<string>,  // role token or hex; "" clears it
    color: Option<string>,       // inherited by text
    opacity: Option<int>,        // percent
    radius: Option<Corners>,
    border: Option<Border>,
    elevation: Option<int>,      // 0..4, the shadow scale

    // text  (font-size, text-overflow)
    font: Option<int>,           // inherited
    ellipsis: Option<bool>,

    // interaction
    cursor: Option<string>,      // CSS cursor name: "pointer", "text", ...
}
```

Value constructors. Loam has no overloading, so each CSS shorthand form gets
its own name:

```loam
px(12)  pct(50)                                   // Len
edges(8)  edges2(8, 12)  edges4(t, r, b, l)       // padding: 8px | 8px 12px | t r b l
Edges(top = 0, left = 12)                         // only some sides
corners(8)  corners_top(8)  corners_bottom(8)  corners4(tl, tr, br, bl)
line(1, rule())                                   // border: 1px solid <color>
ratio(16, 9)                                      // aspect-ratio
```

The compiler wraps a plain value where an `Option` is expected (§7, C2), so
the call site is plain data: `Style(gap = 8, width = px(200), background = plate())`.
App code writes `Style(...)` and `px(...)` unqualified, because `zeus` imports
the style module.

What is folded in, and what is dropped:

- `padding` / `padding_x` / `padding_y` / `padding_top..left` (7 fields) → `padding: Edges`. Same for `margin` (7 → 1) and `top/right/bottom/left` → `inset`.
- `radius` / `radius_top` / `radius_bottom` → `radius: Corners`.
- `border` + `border_color` → `border: Border`.
- `spacing` (an alias of `gap`) is dropped. `gap` is the only spelling.
- `show` / `visible` → `display = Display.None` (reactive) or `Show`.
- `align_direction` → `direction`. `Row` / `Column` set it for you.
- `safe`, `row_h`, `rebuild`, `bind`, `edit`, `multiline`, `fit`, `lazy`,
  `press`, `hover_off`, `text` stop being style. They are props of the
  element that uses them (`Input(multiline = true)`, `Image(fit = Fit.Cover)`)
  or internals of the list hosts.

Not yet included, because the engine does not implement them: font weight,
line height, text alignment, flex basis other than zero, reverse directions,
and grid track settings. Those stay on `Grid(config = ...)`. Each is added to
`Style` only together with its engine support.

**No inheritance in the engine.** A node's `color` / `font` do not flow into its
children. A component that takes the caller's text keys hands them to its own
labels with `text_part(style_of(p.style))` (and `text_color(...)` for an icon),
which is how `Button(style = Style(color = ...))` recolors both its label and
its icon.

**Border** is `Border { width: Option<int>, color: Option<string> }`:
`Border(color = rule())` recolors without changing the width; `line(w, c)` sets
both.

### 4.4 Reactive style, fine-grained

`style` is `fn() -> Style`, so the existing wrap-in-a-closure rule
(`wants_thunk`) applies and the call site writes plain data:

```loam
Box(style = Style(background = if active() { plate() } else { raised() },
                  padding = edges2(5, 12)))
```

The element runs the thunk in **one** effect (`install_style` in
`zeusbase.loam`). It diffs the new `Style` against the last one key by key and
writes only the keys whose value changed, through the same setters and
targeted invalidation every prop uses (`apply_int_value`, which is `apply_int`
without the "negative means unset" rule). So:

- A static style runs once. If the effect read no signal (`track.dep_count`),
  it is disposed right away and nothing is kept, so it costs no graph.
- Only a reactive style keeps its last `Style`, in an arena-side row
  (`arena.style_keep`). The node-to-row index lives beside the node table, not in
  `UiNode`, so the node record stays 520 bytes. A freed node returns its row.
- A key that goes from present to absent keeps its last value. Give both
  branches a value: `background = if on { a } else { b }`.
- `style` is installed last in `Component`, so a present key wins over the
  matching flat prop and over kind chrome.
- A reactive style touches only the attributes that moved. A paint-only change
  (background, color, opacity) still skips layout and damages one node, as
  phase 4 already guarantees (`reactivity.md`).
- `selected_chrome` is no longer needed. Its seven arguments become one `if`
  inside `style`.
- A theme switch still re-resolves role tokens at paint time, because
  `plate()` returns a token, not a colour.

### 4.5 Layering: `merge`

A recipe's own look sits under the caller's with one call:

```loam
style = merge(tab_base(p.size), style_of(p.style))
```

`merge(a, b)` takes `b`'s field wherever `b` has one (`Some`) and `a`'s
otherwise. That is the whole cascade. It replaces all 96 `px_or_default` /
`str_or_default` calls, and because `b` is a `Style`, the caller can override
*any* key, not only the 12 a recipe chose to expose.

### 4.6 State styles

`hoverStyle` / `pressStyle` / `focusStyle` / `disabledStyle` are `Style`
overlays merged over `style` while the state holds (in that order; later
wins).

*Landed (R4).* Each is `Option<fn() -> Style>`, so a plain `Style(...)` is
wrapped like `style` and an overlay that reads a signal follows it. They are
thunks rather than `Style` values for a second reason: `BoxProps` is passed
by value on every element, and four inline `Style`s (~3 KB) overflowed the
wasm stack in myapp. How it works:

- An element with any state style registers with the arena
  (`arena.state_watch`) and gets a 0..15 mask signal (hover, press,
  focus-visible, disabled). Once a frame, `arena.state_sync` recomputes each
  watched node's mask from the interaction state and writes the signal only on
  a real transition. The element's style effect reads it and merges the
  overlays.
- A key only an overlay sets goes back, when the state ends, to the node's
  value under its base style, read once at install (`PaintRest`).
- Only paint keys are honoured (`paint_only`: background, color, opacity,
  border, elevation). A layout key is dropped at run time, so hover never
  reflows. The compile-time warning is not done.
- An element with state styles is a hover target, and it paints no default
  hover wash or press overlay. Its overlays are its feedback. `pressStyle`
  turns on press tracking.
- The interim props (`hoverOff`, `hoverFade`, `pressScale`, `lift`,
  `press`) remain. The themed recipes (`Button`, `Card`) still use them and
  are not moved yet, because doing so changes the draw goldens.

Test: `compile_pass/zeus_state_style`.

### 4.7 Slots

The trailing block is `children`. Any other region is a named `fn()` prop:

```loam
struct CardProps {
    title: string = "",
    media: Option<string> = none,
    header: Option<fn()> = none,
    footer: Option<fn()> = none,
    children: fn() = noop,                  // filled from the trailing block
    on_click: Option<fn()> = none,
    style: Option<fn() -> Style> = none,
}

Card(title = "Equity", footer = || { Button(label = "Export") }) {
    LineChart(...)
}
```

*Landed in phase 6.* A trailing block on a call whose props struct has a
`children: fn()` field fills that field (§7, C5; `fold_children_block` in
`typecheck.c`). Everything else keeps the old lowering, where the block is
built into the returned node: elements (`Box`, `Row`, …) and components with
no `children` field. The component places `p.children()` wherever its view
wants it, so `slot_into` and `route_children` are deleted from the arena and
`zeusbase`. `p.children()` calls the field, not the node method of the same
name. Components with a `children` slot today: `Card` (the body, before an
optional `footer`), `Collapsible` (the part that hides), `Alert` (actions,
under the body), `Dialog` / `AlertDialog`, `Drawer`, `Toast`, `Popover`,
`HoverCard` and `Menu` (rows after `items`).

Triggers are slots too. A floater builds its trigger inside a host that holds
only the trigger in flow; the scrim and the panel float inside the same host,
so the component has one root. The panel anchors to the trigger itself
(`child_ref(host, 0)`), and `Menu` owns its click:

```loam
Popover(open = pop, width = 220, trigger = || {
    Button("Open popover", on_click = fn() => pop.set(1))
}) {
    Title("Details")
}

// One trigger, two floaters: the tooltip wraps the popover.
Tooltip(text = "Opens a popover", trigger = || {
    Popover(open = pop, trigger = || { Button("Open popover") }) { ... }
})

Menu(items = rows, on_select = fn(i: int) => run(i), trigger = || { Button("File") })
```

A floater returns its host. Code that needs the panel itself (tests measuring
it) reads child 2 of the host (child 1 for `Tooltip` / `HoverCard`).

### 4.8 Variants

One function per component, with `tone` / `variant` / `size` enums:

```loam
Alert("Payment failed", "...", tone = Tone.Danger)    // landed: no AlertDestructive & co.
Button(look = Look.Outline, size = Size.Sm, label = "Cancel")
```

*Landed for `Alert`:* `AlertDestructive`, `AlertWarning`, `AlertSuccess` and
`AlertInfo` are gone; `tone` is `Tone.Info` (default), `.Ok`, `.Warn` or
`.Danger`.

Booleans are `bool` (`plain`, `dark`, `multiline`, `lazy`, `safe`), not
`int`. *Landed:* `Navbar(dark)`, `Tab(plain)`, `InputProps.multiline`,
`ImageProps.lazy`, and `BoxProps.multiline` / `lazy` / `safe`.

---

## 5. Worked examples

### 5.1 Tab + Tabs

**Today**: `molecules.loam:696-790` is about 95 lines with `ui_push`/`ui_pop`,
`slot`, `selected_chrome`, two `apply_key`, `roving_focus`, `prop_int` ×3,
`set_prop_int`, `arena.nodes[...]` ×6, `intern_fn`, and `list_fn` hooks.

**Target:**

```loam
struct TabProps {
    label: string,
    active: fn() -> bool,
    on_click: Option<fn()> = none,
}

/// One tab trigger. Only the active tab is a tab stop (roving focus).
fn Tab(p: TabProps) -> Node {
    return Row(role = "tab", label = p.label, focusable = p.active,
        on_click = p.on_click,
        style = Style(gap = 4, align = Align.Center, padding = edges2(5, 12),
                      radius = corners(RAD_MD), cursor = "pointer",
                      color = if p.active() { ink() } else { mute() })) {
        Text(p.label, style = Style(font = FONT_BODY, ellipsis = true))
    }
}

struct TabsProps {
    selected: Signal<int>,
    labels: []string,
    on_change: Option<fn(int)> = none,
    style: Option<fn() -> Style> = none,
    keys: []KeyBind = no_keys(),
}

/// Tabs with a sliding indicator under the selected trigger.
fn Tabs(p: TabsProps) -> Node {
    // state
    let n = p.labels.len
    let tray = ref()
    let ind = signal(Rect {})

    // controller
    fn select(i: int) {
        let j = wrap_index(i, n)
        p.selected.set(j)
        emit_with(p.on_change, j)
    }
    fn place(ms: int) {
        let r = child_rect(tray, p.selected.get() + 1)    // +1: the indicator is child 0
        tween(ind, r, ms)
    }
    effect(fn() => place(Motion.Base))
    on_resize(tray, fn() => place(0))

    // keys
    let keys = keys_concat([]KeyBind {
        chord("right", "tabs.next", fn() => select(p.selected.get() + 1)),
        chord("left",  "tabs.prev", fn() => select(p.selected.get() - 1)),
    }, p.keys)

    // tree
    return Row(ref = tray, role = "tablist", keys = keys,
        style = merge(Style(gap = 2, padding = edges(4), align = Align.Center,
                            background = raised(), radius = corners(RAD_LG)),
                      style_of(p.style))) {
        Box(role = "presentation", style = Style(
            position = Position.Absolute,
            inset = Edges(top = 0, left = ind.get().x),   // unset edges stay auto
            width = px(ind.get().w), height = px(ind.get().h),
            background = plate(), radius = corners(RAD_MD), elevation = 1))
        Loop(p.labels, fn(i, label) {
            Tab(label = label, active = p.selected.get() == i,
                on_click = fn() => select(i))
        })
    }
}
```

Every behaviour is still there (roving focus, arrow keys, the animated
indicator, refit on resize), and the tree reads top to bottom. `on_resize(ref,
fn)` replaces the hand-built `list_fn` / `list_gen` refit hook in `Tabs`. It is
the same mechanism, behind a name.

### 5.2 Checkbox (controlled or uncontrolled)

```loam
struct CheckboxProps {
    label: string,
    checked: Option<Signal<bool>> = none,
    initial: bool = false,
    on_change: Option<fn(bool)> = none,
}

fn Checkbox(p: CheckboxProps) -> Node {
    // state
    let on = some_or(p.checked, signal(p.initial))

    // controller
    fn toggle() {
        on.set(!on.get())
        emit_with(p.on_change, on.get())
    }

    // keys
    let keys = []KeyBind { chord("space", "checkbox.toggle", toggle) }

    // tree
    return Row(role = "checkbox", label = p.label, focusable = true, keys = keys,
        on_click = toggle,
        style = Style(gap = 8, align = Align.Center, cursor = "pointer")) {
        Box(style = Style(width = px(16), height = px(16), radius = corners(RAD_SM),
                          border = line(HAIRLINE, accent()),
                          align = Align.Center, justify = Align.Center,
                          background = if on.get() { accent() } else { plate() })) {
            Show(when = on.get()) {
                Icon(IC_CHECK, size = 16, style = Style(color = on_ink()))
            }
        }
        Text(p.label, style = Style(color = ink(), font = FONT_BODY))
    }
}
```

### 5.3 App code: the dracula `NavLink`

*Landed.* Before: `zeus.Box(...)`, `zeus.ui_push(item.id)`, `zeus.Text(...)`,
`zeus.ui_pop()`, then
`zeus.selected_chrome(item, text, || { ... }, colors.raised(), "", zeus.ink(), colors.mute())`.
Now (`examples/zeus/dracula/components/nav.loam`, and the same in `myapp`):

```loam
fn NavLink(label: string, href: string, deep: int = 0) -> Node {
    let path = router.path_signal()
    fn active() -> bool {
        return lit(path.get(), href, deep)
    }
    return zeus.Box(
        style = Style(height = px(36), direction = Direction.Row, gap = Space.Exxs,
                      align = Align.Center, padding = Edges(right = 12, left = 12),
                      radius = corners(spacing.RAD_MD), cursor = "pointer",
                      background = if active() { colors.raised() } else { "" }),
        on_click = fn() => router.navigate(href),
        role = "button",
        label = label) {
        zeus.Text(label, style = Style(font = spacing.FONT_BODY,
                                       color = if active() { zeus.ink() } else { colors.mute() }))
    }
}
```

The colour sits on the `Text`, not the row: the engine has no `color`
inheritance (§4.3). The old version also had a bug the style form fixes:
`selected_chrome` skipped an empty `off_bg`, so a link kept its fill after
navigating away; `background = ""` clears it. `selected_chrome` is deleted.

The route `Layout`s (dracula, myapp, myapp's blog, and the `zeli new`
templates) were the other `zeus.slot` users. They return their tree now:
`return zui.Page() { nav.Navbar(); build() }`.

### 5.4 App state: signals, not generation counters

The dracula pattern of `let mut hit_syms = []string {}` plus `bump_search()`
plus `Match(search_gen, ...)` becomes a store of signals:

```loam
struct Hit { sym: string, name: string, date: int, close: string }

let hits: Signal<[]Hit> = ...                  // created in start()
let search_state: Signal<Load> = ...           // Idle | Loading | Ready | Failed

// tree
Match(search_state, fn(s) {
    match s {
        Load.Ready => { For(hits, key = |h| h.sym, fn(i, h) { HitRow(hit = h) }) }
        Load.Failed => { ServerDown(retry = search) }
        _ => { Spinner() }
    }
})
```

`std:res` already models `Loading | Ready | Error`. The point for the guide is
that a component reads signals, never module globals behind a bump counter.

*Landed for the backtest screens.* `components/lab.loam` keeps each run's
result in one value: `Signal<BacktestView>` and `Signal<WalkView>`. The
pages `Match` on those signals and paint from the value they are handed:
`Match(lab.backtest(), fn(v) { ... paint_result(v) ... })`. `bt_gen`,
`wf_gen`, `bump_bt`, `bump_wf` and the fourteen display globals are gone.
What is left is run bookkeeping (which rule and scheme the shown result is
for), which the screens never read. `book.loam`'s `load_gen` /
`scheme_gen` / `search_gen` remain (§10).

---

## 6. The element layer and the arena

### 6.1 Two layers, one rule

| Layer | Lives in | May use the arena? |
|---|---|---|
| **Elements**: `Box`, `Row`, `Column`, `Grid`, `Text`, `Input`, `Image`, `Svg`, `Scroll`, `Portal` (today's `Overlay`), plus the hosts `Show`, `For`, `Match`, `Boundary`, `VirtualList` | `zeusbase` / `atoms` | Yes. This is the only code that calls `raw_*`, `Component`, `slot`, `ui_push`, `intern_*`, `arena.*` |
| **Components**: everything else in `zeus-components`, and all app code | `atoms` / `molecules` / `organisms` / apps | **No.** State, controller, keys, view only |

The rule can be checked mechanically: a lint that fails when a file under
`zeus-components/std/` outside the element list, or any file under
`examples/`, mentions `ui_push`, `ui_pop`, `slot(`, `arena.`, `intern_`,
`prop_int`, `set_prop_` or `raw_`.

*Landed:* `ig_arena_lint` in `nob.loam` is a `make test` host check.
`examples/zeus` and `www` may not use any of these names (comment lines and
`build/` are skipped). In `zeus-components/std/{atoms,molecules,organisms,colors}`,
a hit fails unless its enclosing top-level fn is on that file's element-layer
allow-list: the list hosts, charts, native controls, `Icon`, `Image`,
`Overlay`, `Scroll`, `Grid` and so on. A new component that reaches into the
arena fails the build. `show(` and `show_eq/ne/ge/le(` are on the forbidden
list too, so they are element-layer only. `BadgeCount` uses a reactive
`display`, the tests use `visible =` or `Show`, and `show_ge` / `show_le`
were deleted. `Checkbox` draws its tick with `Icon`, not `raw_svg`.

Behaviours that need the arena reach components in two forms (*landed in
phase 4*, `zeusbase.loam`):

- **Element props** for node flags: `ref`, `focusable`, `roving` (a roving tab
  stop that takes focus as it becomes true), `hoverFade`, `pressScale`, `lift`,
  `enter` (`Enter.Fade + Enter.Reveal`), `label` (reactive a11y name), `loading`,
  `on_hover`. The interaction-flag props are interim until the state styles
  (§4.6) replace them.
- **Ref verbs**, called from the controller (usually `on_mount`): `focus(r)`,
  `trap_focus(r)` / `release_focus_trap(r)`, `rect(r)`, `child_rect(r, i)`,
  `child_total(r)`, `on_resize(r, fn)`, `anchor(r, target, side, gap)`,
  `skip_exit(r)`, `child_ref(r, i)`, `set_on_click(r, h)` and
  `watch_hover(r, sig)`. (`route_children`, the interim before named slots,
  was removed in phase 6.)

`roving_focus`, `tabs_place`, `selected_chrome` (in components) and every
`arena.nodes[...]` write in a converted component are gone.

### 6.2 `View { }` lowering

*Landed in phase 4; the `View { }` wrapper was removed in phase 7.* A
component's tree is the element it returns. `return Element(...) { children }`
lowers (in `parse_postfix`) to `return slot(Element(...), || { children })`, and
`parse_fn` (`frame_component`) recognizes a tail `return slot(...)` and wraps
the whole body in a frame:

```loam
fn Name(p: NameProps) -> Node {
    let __zv = __view_begin()                    // first statement: open the frame
    ...state / controller / keys, unchanged...
    return __view_end(__zv, || { slot(Element(...), || { children }) })
}
```

A fn whose tail is *not* a returned element with a trailing block — `return
SomeComponent(...)`, `return node`, or a fragment that emits into its caller's
slot — needs no frame, and gets none. That is the whole replacement for the
`View` marker: the returned element is the marker.

`__view_begin` / `__view_end` live in `zeusbase.loam`. `__view_end` builds the
body, then:

1. checks that exactly one element was attached to the frame's parent slot
   (a trap otherwise),
2. runs the `on_mount` callbacks while the frame is still open, so what they
   create is owned by the root too (refs are filled by then),
3. moves every record the frame's pending owner holds onto the root
   (`arena.own_end`),
4. rebinds the effects it allocated that were bound to the parent slot onto
   the root (`track.log_end`),
5. returns the root.

- A void component is not a thing: a component returns `Node`. A function
  that emits several children into its caller's slot with no `return` is a
  fragment (`TableRow`: a row then a separator), and that stays legal.
- The lowercase `zeus.view(build)` (the rebuild-per-frame app mode) is
  unrelated to the removed PascalCase `View` keyword.

### 6.3 Ownership: fixing D10

*Landed in phase 4.* `arena.own_begin()` returns a *pending owner* token, and
the frame makes it the build owner. Every `signal`, `computed`,
`effect`, interned handler and `on_cleanup` created until `own_end` is
recorded against it, through the existing `arena.owner_now()`
(`reactivity.md` phase 8). `own_end(token, root_id)` moves the whole record
onto the root node and marks the root as owning records; freeing that node
releases them (`arena.own_release` in `free_node`). The node record stays 520
bytes: the mark lives beside the node table. Consequences:

- A component's state dies with the component's root node, even state created
  before the first element. This is the leak in D10.
- Ownership no longer depends on `ui_parents`, so the build stack is only about
  structure and is entirely the compiler's business.
- Nested components nest owners naturally. A `For` row's scope still owns
  everything its `build` creates (phase 2), now including that row's
  components' state.

---

## 7. Compiler and runtime work

Probed on this branch with `bin/loamc` rebuilt at `d37c026d`. Phases 1 to 7 have landed (C1 to C17, R1 to R7), and C18 / R8 are new. Tests added since phase 7: `golden/option_none`, `compile_pass/zeus_show`, `compile_pass/zeus_state_style`, `compile_pass/zeus_mut_capture`, `compile_pass/zeus_struct_shadow`. Tests: `golden/option_props`, `golden/props_positional`, `golden/local_fn`, `compile_fail/local_fn_untyped_param`.

| # | Change | Why | Probe result |
|---|---|---|---|
| **C1** | Emit a struct that another struct holds by value (a generic instance such as `Option__int`, or a fixed array of one) **before** the struct that holds it. `codegen_c.c` collects every definition, then emits it depth-first over its inline fields (`se_emit`) | A props/style struct can have an `Option<T>` field | **Landed (phase 1).** Was `unknown type name 'Option__int'` |
| **C2** | Where an `Option<T>` is expected (an argument or a struct-literal field), a plain value is wrapped as `Option { tag: Some, val: arg }`, after the thunk rule when `T` is `fn() -> U`. An argument that already is an Option (a forwarded `p.on_click`, a call declared to return `Option`) passes through. A generic struct literal whose type arguments are known now gives each field its concrete expected type | `on_click = save`, `width = 200`, `label = "n = {{n}}"` need no `some(...)` | **Landed (phase 1).** Was `field 'on_click' has type Option<fn()>, got fn()` |
| **C3** | Delete the `__set` rule (`typecheck.c:2849`) once C1–C2 land and the library is migrated | No hidden naming convention in the type checker | **Landed (phase 5).** A field named `x__set` is an ordinary field now |
| **C13** | C7 tightened: positionals fill a props struct only when the spare argument's type is known and is not a struct or an unresolved generic. `f(a, b, result.some_or(o, d))` against `fn f(.., s: Signal<int>)` had been read as a props fill | No false props fills | **Landed (phase 5)** |
| **C14** | One monomorphized fn per C name: two type objects that print the same (`Signal<int>` built two ways) produced two definitions of `loam_result_is_some__Signal_int_` | Generic helpers over handle types | **Landed (phase 5)** |
| **C4** | `none` default without a payload: where an `Option<T>` is expected (an argument, a struct field or default, a parameter default) and no binding named `none` is in scope, a bare `none` is `Option { tag: None }` with a zeroed payload (`struct_lit.zero_rest`; IR lowers the left-out field to a zero-initialized temp). Parameter defaults are now checked with the call-site rules | `result.none(noop)` needs a dummy value, and generic return-position inference cannot supply one | **Landed.** The 132 `= result.none(...)` defaults in the library are `= none`. Test: `golden/option_none` |
| **C5** | A trailing block on a call whose props struct has a `children: fn()` field fills that field instead of lowering to `slot(...)`; a `children` struct field called as `p.children()` is the field, not the node method | Named slots, delete `slot_into` (§4.7) | **Landed (phase 6)**. Test: `golden/children_slot` |
| **C6** | The view prologue/epilogue in §6.2, opened off a tail `return Element(...) { }` (no `View` keyword since phase 7) | One element for the tree, owner handling | **Landed (phase 4, `View` removed phase 7)** |
| **C7** | Leading positionals fill a props struct with **no** named argument: `Tab("Home")` → `Tab(TabProps { label: "Home" })`. Applies only when the first spare argument is known not to be the struct itself (`Tab(props)` and `Tab(make_props())` still pass a value) and every field it does not fill has a default | One-line calls for the common case | **Landed (phase 1).** Was `expected CardProps, got string` |
| **C8** | Not a codegen bug. In a user (non-std) module, a fn with an empty body `{}` declares a **C seam** implemented by the app's runtime C (`typecheck.c`, `user_seam_mod`), so `fn noop() {}` has no Loam body. Write `fn noop() { return }`. The real gap, that a seam fn could not be passed as a value, is fixed: user seams now get a `__as_fn` trampoline, and a missing C body becomes a link error that names it | Clear failure instead of `undeclared identifier 'loam_noop__as_fn'` | **Landed (phase 1)** |
| **C9** | Local `fn name(...) { }` as a statement, lowered to `let name = fn(...) { }`. Every parameter needs a type (there is no expected type to infer from) | Controller handlers read as functions (§3.2) | **Landed (phase 1).** Was `expected expression (got fn)` |
| **C15** | The cheap type peek behind C2 sees through calls (`style_of(p.style).width` is an `Option`, so it is not wrapped again) | Reading a caller's style key | **Landed (phase 7)** |
| **C16** | A fn's signature is resolved in the module that declares it, not in the caller's, so an app that declares its own `ChipProps` no longer changes what `zui.Chip` takes | Same type names in app and library | **Landed (phase 7)** |
| **C17** | Struct names are global in the generated C: an app struct named like a library struct (`ChipProps`) still clashes at the C level. Module-qualify struct C names | Same type names in app and library | **Landed.** A struct in a user module whose name another module also declares gets `Type.cname = <module>__<Name>`, and it is a distinct type (`type_eq` compares `cname`). A struct literal built against an expected type (the C7 props fill) uses that type's declaration. Library structs keep their names, because the runtime C uses them. Test: `compile_pass/zeus_struct_shadow` |
| **C18** | Monomorph and generic-struct C names dropped `[` `]`, so `Option<[]string>` and `Option<string>` shared one C struct, and so did `Option<Signal<[]int>>` and `Option<Signal<int>>`. Both manglers (`type_c_name`, `cname_append_ty`) now spell brackets `L` / `R` | Found while testing C4 | **Landed** |
| **R7** | Runtime bugs the phase-7 volume of static styles exposed: a disposed static effect's interned handler was freed twice (once by `dispose_eid`, again with its owner, after the id was reused, e.g. by a `For` host's rebuild hook), and effect-table compaction left disposed rows as permanent holes (one leaked row per mount). Both fixed (`arena.forget_intern`, compaction of `fn_ids == 0` rows) | Correctness | **Landed (phase 7)** |
| R1 | `Style`, `Len`, `Edges`, `Corners`, `Border`, `merge`, value constructors | §4.3–4.5 | **Landed (phase 2)** |
| R2 | Style effect with field diff; a zero-dependency effect is dropped after its first run | §4.4 | **Landed (phase 2)** |
| R3 | `KeyBind`, `chord`, `chord_global`, `chord_click`, `keys_concat`, `install_keys`; delete the key block and `apply_key` | §3.3 | **Landed (phase 3)** |
| R4 | State-style overlays on the interaction engine | §4.6 | **Landed.** Test: `compile_pass/zeus_state_style` |
| R8 | `Signal<bool>` for on/off state (`flag_int` mirror, Escape through the overlay's `on_click`); a signal write whose effects only forwarded to an attributed write counts as attributed (`arena.targeted_writes`), so a `Switch` toggle still syncs just its bound nodes | §3.1 | **Landed** |
| R5 | `Ref`, `ref()`, ref verbs, `on_mount`, `on_resize`, `own_begin` / `own_end`, behaviour props | §3.1, §6 | **Landed (phase 4)**. Test: `compile_pass/zeus_view` (frame, `on_mount`, refs, keys, and flat tables over 20 mount/unmount cycles) |
| **C10** | A closure whose return type is inferred, ending in `if c { f() } else { g() }`, returned a `void` value: generated C declared `void _l2`. A tail `if` whose value is void is now a statement | Every handler written as a local fn with an `if` in it | **Landed (phase 4)**. Test: `golden/local_fn` |
| **C11** | `Option<Signal<int>>` lexes its closers as one `>>`. The parser splits it where a `>` closes a type-argument list (`consume_gt`) | Nested generic props | **Landed (phase 4)**. Test: `golden/nested_generic_close` |
| **C12** | A captured `let mut x: int` in a function is silently promoted to a `Signal` ("component state", `cap_type_for` in `typecheck.c`). In a fn returning `Node` it lowered to `loam_zeus_hook_signal`, which the runtime no longer defines, so it failed to link | One rule for state | **Fixed, not removed.** The promotion is how a closure writes an outer local; 16 tests rely on it (`let mut id = 0` set inside a build closure). Both paths now lower to `loam_zeus_signal`, owned by the enclosing frame in a component. Test: `compile_pass/zeus_mut_capture` |
| R6 | `Show(when, lazy)`. Remove `If`, `show_*`, `show`, `visible` | §3.4 | **Landed** for `Show` / `If`. `show_*` and `visible` remain as element-layer internals. Test: `compile_pass/zeus_show` |

---

## 8. Migration

Each step keeps `make test` and the DRAW goldens byte-identical, as the
extraction in `zeus-components/README.md` did.

1. **Compiler fixes C1, C2, C7, C8, C9.** Pure enablers, nothing in Zeus changes. *Landed.*
2. **Style types (R1, R2) next to `BoxProps`.** `Component` accepts both. A
   `style` given wins per field. No call sites change yet. *Landed:*
   `zeuscore/style.loam`, `BoxProps.style` / `TextProps.style`, the style
   effect in `zeusbase.loam`, and the setters it needed (`SET.AlignSelf` …
   `SET.WrapFlag`). It replaces the unused `BoxStyle`. Test:
   `compile_pass/zeus_style`.
3. **Keys (R3).** Add `keys` to `BoxProps`, then rewrite the 17 key blocks and
   every `apply_key` call. Remove the five fields. *Landed.*
4. **Refs, `on_mount`, ownership (R5) and `View` (C6).** Convert atoms, then
   molecules, then organisms, one file per commit. Each conversion deletes that
   file's `ui_push` / `arena.nodes` / `selected_chrome` calls. The lint from
   §6.1 is turned on per file as it goes green. *Landed:* every component in
   `molecules`, the recipes in `atoms` (Avatar, AvatarGroup, Badge, Chip, Card,
   Empty, Stat) and `organisms` (Combobox, Accordion, MenuItem, Drawer, Toast,
   Dialog, Navbar, NavTab, the table family, the calendar and DatePicker) are
   `View` components. `selected_chrome` has no callers in the library.
   What remains is element layer by design (the list hosts, `Progress`,
   `Sparkline`, `Skeleton`, the chart internals, `Comp`) plus the four
   anchored floaters, `Popover`, `Menu`, `Tooltip` and `HoverCard`. Those take
   a pre-built `trigger: Node` and emit a scrim and a panel side by side, so
   they move in step 6, when the trigger becomes a slot. Fixed along the way:
   the `Select` / `Combobox` highlight never cleared (a row stayed raised after
   the keyboard moved off it).
5. **`Option` for handlers.** Replace `on_x` + `on_x__set` with `Option<fn()>`.
   When the last one is gone, apply C3. *Landed:* all 40 pairs (`BoxProps`,
   the component props, `Menu.on_select` too) are `Option<T>` fields, read with
   `result.is_some` / `.val`, invoked with `emit(h)` / `emit_with(h, v)`, and
   installed only when present (`handler_if(on, h)` for a conditional one).
   Callers did not change: a plain value is wrapped in `Some` by C2. C3 is
   applied.
6. **Slots (C5) and variants.** `Card`, `Popover`, `Menu`, `Tooltip`,
   `HoverCard`, `Alert*`. *Landed:* C5; `children` on Card, Collapsible, Alert,
   Dialog, Drawer, Toast, Popover, HoverCard, Menu; trigger slots on the four
   floaters (now `View` components, so `organisms` has no arena access left);
   `slot_into` / `route_children` deleted; `Alert(tone = ...)`. Callers moved:
   the floater tests and draw goldens, the gallery, dracula, myapp. Fixed on
   the way: the `www` docs app had not compiled (a closure passed to the
   string `background` prop); it uses a reactive `style` now.
7. **Shrink `BoxProps`** to the element props in §4.1. The flat style fields
   go. `zeus.loam` keeps forwarding the component names, so `zeus.Card` call
   sites only change at the props. *Landed*, in five green steps:
   - 7.1 a codemod moved the flat style arguments of 614 element calls into
     `style = Style(...)`, in the library, tests, draw goldens, examples and
     `www` (`edges2(8, 12)`, `corners(6)`, `line(1, rule())` in app code;
     helpers for values that might still be a sentinel in library code);
   - 7.2 component props lost their style fields; each recipe merges a base
     `Style` with the caller's (`merge(base, style_of(p.style))`), and a second
     codemod pass moved 74 component call sites;
   - 7.3 the flat fields left `BoxProps`, `TextProps`, `InputProps` and the
     element props; `Component` sets only element defaults (a column for
     containers, 40% when disabled) and the style does the rest;
   - 7.4 renames: `a11y_role` / `a11y_label` → `role` / `label`, `row_h` →
     `rowHeight`, `hover_off` → `hoverOff`, `media_height` → `mediaHeight`,
     `is_separated` → `isSeparated`, `label_every` → `labelEvery`,
     `auto_columns` → `autoColumns`, `RouterProps.not_found` → `notFound`, and
     the twelve SCREAMING enums (`DIRECTION` → `Direction`, `ALIGN` → `Align`,
     `ALERT` → `Tone`, `LOOK`, `SIZE`, `TINT`, `SPACE`, `PLACE`, `SIDE`,
     `MOTION`, `IMAGE_FIT` → `Fit`, `TREND`);
   - 7.5 docs (`zeus.loam` header, READMEs, `spec.md`).
   The style writer reproduces the flat props' node writes exactly (uniform
   radius on the shared field, `Padding` / `PaddingX` / `PaddingY` for equal
   sides, border width only on boxes and buttons, direction only on
   containers, a growing element shrinks by default), so every draw golden is
   unchanged.
8. **Examples.** dracula, gallery and counter, plus the generation-counter
   pattern (§5.4). *Moved with step 7* (all apps build on the new props).
   Since then: `NavLink` and the route layouts have no `ui_push` / `slot`
   (§5.3), every `show_*` call in the apps is a `Show`, and the apps are
   linted (§6.1). The backtest screens read `Signal<BacktestView>` /
   `Signal<WalkView>` instead of bump counters (§5.4).

Order matters at steps 2 and 5. Both old and new forms are accepted until the
last caller moves, so no step is a flag day.

---

## 9. Checklist for a new component

- [ ] `fn Name(p: NameProps) -> Node`, sections in order: state, controller, keys, tree.
- [ ] `NameProps` has only this component's data, plus `style`, `keys`, and its `on_*` handlers as `Option<fn(...)>`. Optional fields default to `= none`.
- [ ] No `__set`, no `-1` / `""` sentinels in public fields.
- [ ] Root style is `merge(base, style_of(p.style))`. Keys are `keys_concat(own, p.keys)`.
- [ ] Booleans are `bool` / `Signal<bool>` / `fn() -> bool`.
- [ ] Reactive visuals go in `style` (and `hoverStyle` & co. for interaction states), reactive presence in `Show` / `Match` / `For`.
- [ ] No `ui_push`, `ui_pop`, `slot(`, `arena.`, `intern_`, `prop_int`, `raw_`.
- [ ] A handle, when needed, is a `ref()`, used in `on_mount` / `effect` through ref verbs.
- [ ] Extra regions are named `fn()` slots. The trailing block is `children`.
- [ ] Every interactive element has `role`, `label`, and `focusable` when it takes keys.

---

## 10. What is still missing

Checked against the code on this branch, after the R4 / C4 / C12 / C17 /
`Signal<bool>` / lint pass. In rough order of value:

1. **dracula's data store** (`components/book.loam`, `data.loam`,
   `stocks.loam`). These still invalidate through `load_gen` / `scheme_gen` /
   `search_gen` bump counters. That is about 1,700 lines of mutable caches
   (series, fetch states, holdings). Moving them to signals is an app
   redesign, not a mechanical step, so the backtest screens (§5.4) were done
   first.
2. **Themed recipes on state styles.** `Button`, `Card`, `Collapsible` and
   the menu rows still use `hoverFade` / `pressScale` / `lift` / `press`.
   Moving them to `hoverStyle` / `pressStyle` changes the draw goldens, so it
   needs the goldens re-recorded and checked by eye. After that, the interim
   props can go.
3. **Compile-time check for layout keys in a state style.** They are dropped
   at run time (`paint_only`). A warning would need the checker to see a
   `Style(...)` literal passed to `hoverStyle`.
4. **`BoxProps` size.** It is passed by value on every element call, and the
   wasm stack is the limit (four inline `Style`s overflowed it). Keep new
   fields small (thunks, handles), or pass `BoxProps` by reference in
   `Component`.
5. **`visible` as a public prop.** It is still an element prop taking
   `Signal<int>`. Components use `Show` or `display`. Removing it needs
   `Overlay`'s callers (Dialog, Drawer, Toast, Popover) to move to a
   reactive `display`, and the engine's Escape path keyed on something other
   than `show_sig`.
6. **Doc samples.** §0 and §5.2 use `some_or(p.checked, signal(p.initial))`
   and `p.style()`. The first works now; `p.style()` on an `Option` still
   reads `style_of(p.style)`.
