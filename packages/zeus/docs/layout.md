# Layout, floating, culling and the pointer

Zeus lays out from one flat node arena with a measure pass and a placement
pass. Floating boxes are attached by points, render commands are emitted only
for what is on screen, and pointer queries are answered from the final boxes
in paint order. Everything is written with `Box`, `Text` and `Style(...)`.

## The frame pipeline

```
signal writes ──► measure (natural sizes, cached per node; text cached by string, px, width)
              ──► layout   (flex / grid / absolute, top-down, always from the root)
              ──► place    (anchored + floating boxes, window space)
              ──► paint    (culled, retained draw list; patched per dirty node)
              ──► pointer  (floaters first by z, then the page; clips honoured)
```

| Step | Runs when | Cost on a scroll frame |
|---|---|---|
| measure + layout | a layout prop changed, a resize, a structural change | **none** — a scroll never re-measures |
| place | layout ran, or a scroll moved with floaters on screen | the floaters only (`layout.replace_anchored`) |
| paint | anything paint-dirty | the scroller's run, culled to its viewport |

A layout frame always starts from the root. It stays cheap for three reasons:
clean subtrees keep their measure stamp; text measures come from the `metrics`
cache; and the layout memo skips any subtree called with the same box as last
time when nothing inside it was invalidated (`ZEUS_LAYOUT_VERIFY=1` checks the
memo against a full solve). A whole-tree solve of 5k nodes is 0.75 ms at
release flags. A label write visits under 2% of that tree. A write that names
its nodes is a *targeted* frame: every box is snapshotted before the solve and
only the nodes that moved are paint-marked afterwards. Paint keeps the retained
ops everywhere else, and damage covers only what moved. A resize or a
structural mark is *untargeted*: the whole surface is repainted.
| pointer | a move / click / wheel | one pass over the hover-bound nodes |

The rule: a scroll offset is applied when boxes are *positioned*, never by
solving the layout again. It is why a 2000-row list scrolls with a
popup open at 0 full layouts and ~50 draw ops a frame
(`tests/compile_pass/zeus_scroll_cull.loam`).

## Responsive layout

A layout depends only on the current window size, never on how the window got
there. The tools are percent sizes, `grow` / `shrink`, wrapping rows and text,
auto-column grids (`GridConfig.autoColumns`), `ellipsis`, `on_resize`
breakpoints, and `view` apps that read `zeus.window_size()`. Every layout cache
is keyed so that a resize cannot leave a stale box:

- a resize bumps the layout epoch, so the memo re-lays everything that depends
  on the width;
- wrapped text is cached per (string, px, width);
- a subtree is translated only when its size is unchanged.

`tests/compile_pass/zeus_responsive.loam` drags a screen through 20 widths
(320 to 1280 dp, crossing a 640 dp breakpoint both ways). At each checkpoint it
requires every box to equal a fresh build at that size, and the draw list to
equal a full repaint.

A live drag costs 0.3 to 1.3 ms of engine time a frame (1.9k to 9k nodes) at
release flags.

## Feature reference

| Feature | Zeus |
|---|---|
| element identity | `let r = ref()` + `Box(ref = r)`; a node is its `id` |
| layout direction | `Style(direction = Direction.Row / Column)` |
| padding, child gap | `padding = edges(..)` / `edges2` / `edges4`, `gap` (`rowGap`, `columnGap`) |
| child alignment | `justify` / `align` (main / cross axis, `Align.*`), `alignSelf` |
| fixed size | `width = px(n)` |
| percent size | `width = pct(p)` |
| fit to content (min / max) | the default; `minWidth`, `maxWidth` |
| grow to fill (min / max) | `grow = 1` (+ `shrink`, `minWidth`, `maxWidth`) |
| aspect ratio | `aspect = ratio(16, 9)` |
| background, corner radius | `background`, `radius = corners(..)` / `corners4` |
| per-side border | `border = Border(color = c, sides = edges4(t, r, b, l))` — paint only, takes no space |
| rules between children | `Border(between = n)` — a rule centred in each gap |
| uniform CSS border (insets content) | `border = line(w, c)` |
| scroll container | `zeus.Scroll` / `raw_scroll`; `overflow = Overflow.Hidden` to clip without scrolling |
| scroll offset | the scroller's `scroll_x` / `scroll_y` (`zui.scroll_to`, `bind`) |
| float on the parent | `floating = float_parent(at, on, x, y)` |
| float on the window | `floating = float_root(at, on, x, y)` |
| float on another element | `floating = float_anchor(at, on, x, y)` + `zeus.float_on(r, target)` in `on_mount` |
| attach points | `Float.at` / `Float.on`, `Attach.LeftTop` … `Attach.RightBottom` |
| float offset, expand | `Float.x`, `Float.y`, `Float.expandW`, `Float.expandH` |
| float stacking | `zIndex` (any floating box is a floater; `zIndex` orders them) |
| pointer passthrough | `Float(passthrough = true)`, or `zeus.pointer_through(r)` |
| clip a float to its parent | `Float(clip = true)` |
| flip to stay on screen | `Float(flip = true)` (on by default in `float_anchor`) |
| font size, text color | `font`, `color` |
| text wrapping | `wrap = true` / default; `ellipsis = true` truncates |
| text alignment | `textAlign = TextAlign.Left / Center / Right` (single-line labels) |
| image / vector | `zeus.Image`, `zeus.Svg` |
| is the pointer over a node | `zeus.pointer_over(id)` |
| hover state / handler | `zeus.hover(node, sig)`, `hoverStyle`, `watch_hover(r, sig)` |
| on-screen box of a node | `zeus.screen_x(id)`, `screen_y(id)`, `node_width(id)`, `node_height(id)` |
| top element under the pointer | `zeus.engine_hit(x, y)`; `zeus.is_under(id, top)` |
| scroll input (drag, momentum) | built in: wheel, smooth wheel, pan with momentum, thumb drag, nested chaining |
| culling of off-screen elements | built in (see below) |
| debug view | `std:devtools` |

Not covered yet: `letterSpacing` and `lineHeight` need each host's text
renderer to take them (Canvas2D, Core Text, Android), and `textAlign` applies
to single-line labels only — wrapped text is laid out by the host.

## Floating boxes

```loam
// A badge centred on its parent's top-right corner.
Box(style = Style(width = px(10), height = px(10), background = "#f00",
                  floating = float_parent(Attach.CenterCenter, Attach.RightTop, 0, 0)))

// A FAB 16dp in from the window's bottom-right corner.
Box(style = Style(width = px(48), height = px(48),
                  floating = float_root(Attach.RightBottom, Attach.RightBottom, -16, -16)))

// A dropdown under its trigger, flipping above near the window's bottom.
let trigger = ref()
let panel = ref()
on_mount(|| float_on(panel, trigger))
Box(ref = panel, style = Style(floating = float_anchor(Attach.LeftTop, Attach.LeftBottom, 0, 4)))
```

A floating box:

- takes no space in its parent's flow (it is sized like an absolute child, or
  against the window for `FloatTo.Root`);
- is placed in window coordinates after layout, so it follows its target
  through every scroll with no re-layout;
- paints above the page, in `zIndex` order, unclipped unless `clip = true`;
- takes the pointer first, also in `zIndex` order, and occludes hover beneath
  it unless `passthrough = true`.

`zeus.anchor(r, target, Place.*, gap)` (menus, selects, popovers) is the same
mechanism with side-based placement codes.

## Culling

Every clip container (a scroller, an `overflow` box) narrows a cull rect for
its subtree, and `paint_node` skips any node wholly outside it, with its
subtree, plus 48dp of slack for shadows and focus rings. A culled node keeps
an empty span in the retained draw list at the place it would have recorded;
if it later needs ops, the patch sees a length change from zero and the frame
re-records in full, which re-decides every cull. Two kinds of node are never
culled: a node with an animated transform (it paints away from its box) and a
floater (it paints in its own pass). `zeus.culled_count()` reports how many
nodes were skipped.

## One pointer model

Hit-testing, hover, cursor and wheel routing all follow the same rules, which
are the paint rules:

1. **Floaters first.** Overlays, anchored popups, floating boxes and z-indexed
   nodes are tested top `zIndex` first. The tree walk skips them, so a popup is
   never hit at the position it would have had in flow.
2. **Window space.** A floater placed in window coordinates is tested in
   window coordinates. Scroll offsets apply only to what a scroller paints.
3. **Clips clip.** A point outside a scroller's viewport or an `overflow` box
   reaches nothing inside it, and `Float(clip = true)` clips a floater to its
   parent's clip.
4. **Occlusion.** The topmost capturing floater under the pointer
   (`arena.ptr_cover`) hides everything outside its subtree from hover. An
   overlay captures only when it paints a scrim or takes clicks, so a toast
   layer does not blank the page. Passthrough floaters never capture.
5. **Wheel.** The wheel drives a scroller inside the topmost floater under the
   pointer. When that floater has no scroller, the page under it stays put.

## Porting to another project

The layout core has no host dependency beyond a text measurer:

| File | Role |
|---|---|
| `std/zeuscore/nodes.loam` | the node tables (eight per-concern tables indexed by node id: `tree`, `lay`, `pin`, `box`, `look`, `scrl`, `anim`, `ui`) and the queries that read only them; imports nothing |
| `std/zeuscore/arena.loam` | signals, dirty bits, frame clock, pointer model |
| `std/zeuscore/metrics.loam` | text measurement and its cache |
| `std/zeuscore/layout.loam` | measure, flex / grid / absolute layout, targeted-frame box diff, floating placement |
| `std/zeuscore/scene.loam` | the draw list (`Draw` records), culling, retained spans and patching |
| `std/zeuscore/input.loam` | hit-testing, hover, wheel / drag routing |
| `std/zeuscore/style.loam` | `Style`, `Float`, `Border`, `Edges`, enums |
| `std/zeuscore/platform.loam` | the seam: text metrics, insets, clipboard, timers |

A new host implements `platform.loam`'s calls and replays the draw list's
records (fill, rounded fill, stroke, text, clip, save / restore, image, svg,
shadow, gradient).
Everything above `zeuscore` (`zeus.loam`, `zeusbase.loam`, `zeus-components`)
is optional sugar.
