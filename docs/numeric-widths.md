# Numeric widths — what is 32-bit, what stays 64-bit

Phase 10 flips the defaults: `int` is `i32` and `float` is `f32`. A value is
64-bit only when 32 bits cannot hold it. Phase 11 then packs the Zeus arena and
draw-list records by field width and narrows the fields that genuinely fit
`u16`/`u8`. This is the audit for the tree at those points, plus the before/after
benchmark and the per-step Phase 11 measurements.

## The rule

32-bit is the default for geometry, sizes, indices, counts, colors, handles,
and style/type values. 64-bit is reserved for the cases in
`docs/yuga_zeus_v2.md §3.6`, restated with the concrete sites here.

## Sites that stay 64-bit

| Site | Type | Why |
|---|---|---|
| `async.now_ms`, `async.Timer.at` | `i64` | monotonic / epoch millis overflow i32 in ~24 days |
| `http` call deadlines (`at` fields in the async/ws call records) | `i64` | same clock |
| `maya.plat_now_ms`, `mayacore.scene.t0_ms`, `scene.frame` | `i64` | same clock |
| SHA-1 in `packages/http/std/httpcore/ws.yuga` | `u64` | 32-bit hash bit patterns with headroom before the mask |
| HPACK Huffman accumulator in `packages/http/std/httpcore/huff.yuga` | `i64` | the bit buffer holds up to ~37 bits between codewords |
| FNV / LCG in `examples/zeus/greeninfer/engine.yuga` | `u64` | same |
| `zeus.scaled` intermediate product | `i64` | `v * scale` before the `/ 100`; the result is `int` |
| C seam: file sizes, mmap offsets, byte counts, `yuga_str.len` storage | `int64_t` | 2 GB is not a limit worth baking in |

Money is integer minor units, never a float.

`--int64-compat` restores the pre-Phase-10 aliases (`int` = `i64`,
`float` = `f64`) as a source-migration bridge. It is **not** a second runtime:
the Zeus C seam is 32-bit now, so programs that cross it need the new
signatures.

## Benchmark

`make bench` builds `packages/yuga/tests/bench/bench.yuga` (a fixed ~3000
node tree, 500 layout passes, then one paint) with the pre-flip tree (`git HEAD`,
`int` = i64) and with the current default, and records arena counts, layout time,
native binary size, generated C size, and wasm size when `YUGA_WASM_CC` is set.

Representative run on macOS, headless, `-O0`:

| Metric | before (int = i64) | after (int = i32, Phase 11 layout) |
|---|---|---|
| arena nodes | 3002 | 3002 |
| node record | (not available) | 416 B |
| arena bytes | — | 1,440,960 |
| draw ops | 1500 | 1500 |
| draw-op record | (not available) | 56 B |
| layout, 500 passes | 7060 ms | 6044 ms |
| native binary | 352096 B | 352616 B |
| generated C | 856259 B | 846977 B |

The `node_bytes` / `draw_op_bytes` rows come from the `__sizeof` diagnostics
added in Phase 11; the HEAD tree has no `__sizeof`, so it cannot report them.
The native size is flat — `-O0` code for the extra explicit conversions offsets
the smaller generated C. Layout time is the noisiest row; both columns come from
the same run so the comparison holds, but expect wider spread across runs.
`--emit-c` grew in Phase 12: the generated C is now interleaved with `#line`
directives (about +5% here), which map it back to the `.yuga` source.

## Phase 11 — Zeus representation and field layout

The `Node` arena record and the draw-list record are the two arrays that grow
with the UI, so their element size is what matters. Both are now packed
widest-field-first. Each step was measured on its own. The packing and narrowing
are value-preserving (DRAW goldens would otherwise be byte-identical); the one
intended behavior change — correcting the unset `border_w` sentinel — is called
out below and is why the goldens were regenerated.

`UiNode` (117 fields) as compiled with `int` = `i32`:

| Step | node bytes |
|---|---|
| HEAD field order, every field `int` | 544 |
| widest-first order, every field `int` | 536 |
| + nine flag/enum fields narrowed to `u8` | 512 |
| + sixteen length fields narrowed to `u16` | 480 |
| + twenty-two enum/ease/percent fields narrowed to `u8`/`i8`/`i16`/`u16` | 416 |

That is **24% off the record** on top of Phase 10's 32-bit flip, and the
ordering step is what makes the narrowing pay: scattering `u8`/`u16` fields
between `i32`s would have the compiler re-insert padding and save nothing.

The `u16` fields are `pad`, `padx`, `pady`, `pad_t`/`pad_r`/`pad_b`/`pad_l`,
`gap`, `gap_row`, `gap_col`, `radius`, `border_w`, `font`, `span`, `grid_cols`,
`grid_min`. They are style lengths stored unscaled in dp (the DPI scale is
applied at paint), and each is documented as `u16` in `arena.yuga`; a literal
that does not fit is a compile error at the literal (`literal 70000 does not fit
u16`). Values reach these fields through the props layer, where `-1` means
"unset" and `skip_unset` returns before the narrowed setter runs, so the
sentinel never reaches `u16`.

The second narrowing pass moves twenty-two more fields out of the `i32` block.
Fifteen are enums or flags whose whole value set is enumerable from the source
(`kind` tops out at 13; `dir`, `justify`, `pos`, `wrap`, `reverse`,
`click_mode`, `click2_mode`, `focusable`, `pulse`, `enter_fade`, `safe` are
0/1 or a small enum) plus the three eases `press_amt` / `hover_amt` /
`show_amt`, which `scene.yuga` clamps to `0..100` explicitly. Four take a `-1`
"unset" sentinel and so go to `i8` rather than `u8`: `w_pct`, `h_pct`,
`align_self`, `hover_fade`. `opacity` and `z_index` go to `i16` (both signed,
neither clamped at its setter), and `text_rot` to `u16` (0..359).

Deliberately **not** narrowed: `bg` / `bg2` / `fg` / `border_c` are 24-bit
colors carrying a `-1` sentinel; the geometry fields are unbounded in practice
(`max_w` legitimately reaches 100000 — `Upload` in the gallery sets exactly
that); and `*_sig` / `*_fn` / `parent` / `nid` / `key_ctx` / `edit_slot` /
`list_*` are arena indices, where a 16-bit ceiling would be a hard cap on tree
size rather than a width saving. `grow` / `shrink` are left alone because
`shrink_weight` composes them arithmetically.

The pass is value-preserving: all eight DRAW goldens are byte-identical, so no
regeneration was needed (unlike the `border_w` fix below).

`border_w` narrowing exposed a latent bug that had to be fixed in the same
commit. `SET.Border` was in `skip_unset`'s always-apply set, and every container
carries the props layer's `-1` "unset" default, so `content_x`/`content_y` added
`-1` to the content origin: every nested container's content was inset by one
pixel per nesting level. `SET.Border` no longer always-applies (unset `-1` is
skipped, leaving the node's 0), and the direct `border()` setter clamps a
non-positive width to 0. Content now lands at the authored padding —
`Box(padding = 20)` puts its child at 20, not 19 — so the DRAW output shifts by
one pixel per nesting level and all seven goldens were regenerated. The diff is
a pure translation: DRAW counts, colors, fonts, and op order are unchanged.

`Draw` (draw-list op): `text` is an 8-byte-aligned slice, so interleaving it with
the ten `int` fields cost 8 bytes of padding per op. Grouping it first drops the
record from 64 to 56 bytes — 12.5% off the draw list, which is rebuilt every
frame.

The four grammar conversions the compiler needed (explicit `u16(...)` at the
setter, `i32(...)` at every int-using read) live in `zeus.yuga`,
`zeuscore/arena.yuga`, `zeuscore/layout.yuga`, `zeuscore/scene.yuga`, and
`zeuscore/input.yuga`.

### Not done in this pass

- **Struct-of-arrays geometry.** The doc's bigger win, but it rewrites every
  `n.x` in layout/scene/input to `arena.xs[n.id]` (hundreds of sites) and needs
  accessor fns before it is readable. It is a separate phase-sized change; the
  benchmark above is the "before" it should be measured against.
- **`f32` geometry.** Layout in this tree is integer-pixel throughout, so
  moving `x`/`y`/`w`/`h` to `f32` is a behavior change (fractional geometry),
  not a width change, and would regenerate every DRAW golden. Left for its own
  phase.

## Mirror types that had to change with the default

Two C-seam structures mirror a Yuga type and must use the same element width:

- `arena.sigs` (`[]int`) is written by the C signal allocators as `int32_t`
  elements now, and the rebuild-scope owner lists (`rec_sid` / `rec_owner`,
  also `[]int`) likewise. A 64-bit write into a 32-bit vector corrupted slot
  ids, which made `show` signals read 0 and leaked signal slots on rebuild.
- `string_from_bytes([]int)` reads `int32_t` elements for the same reason.

## Follow-ups (Phase 11)

Landed: draw-list ops re-typed to the 32-bit default; `UiNode` and `Draw` packed
widest-field-first; `u8`/`u16` narrowing for flags, enums, and style lengths,
each measured (see the table above) and with a compile-time range error at the
literal. The unset `border_w` sentinel bug is fixed alongside `border_w`'s
narrowing, so the DRAW goldens were regenerated once, in that commit.

Still open, in the doc's order:

- **Struct-of-arrays for the geometry arrays only** (`xs[]`, `ys[]`, `ws[]`,
  `hs[]`, `parent[]`, `flags[]`). The larger remaining win; a phase-sized change
  because every geometry access site moves behind an accessor. The 480 B record
  above is its before-number.
- **`f32` inside for geometry.** Blocked on layout moving off integer pixels;
  a behavior change, so it must land with regenerated DRAW goldens.
- **Vector/string length storage at the C seam** (currently `int64_t`).
