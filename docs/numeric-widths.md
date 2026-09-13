# Numeric widths — what is 32-bit, what stays 64-bit

Phase 10 flips the defaults: `int` is `i32` and `float` is `f32`. A value is
64-bit only when 32 bits cannot hold it. This is the audit for the tree at that
point, plus the before/after benchmark.

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
| SHA-1 in `std/httpcore/ws.yuga` | `u64` | 32-bit hash bit patterns with headroom before the mask |
| HPACK Huffman accumulator in `std/httpcore/huff.yuga` | `i64` | the bit buffer holds up to ~37 bits between codewords |
| FNV / LCG in `examples/zeus/greeninfer/engine.yuga` | `u64` | same |
| `zeus.scaled` intermediate product | `i64` | `v * scale` before the `/ 100`; the result is `int` |
| C seam: file sizes, mmap offsets, byte counts, `yuga_str.len` storage | `int64_t` | 2 GB is not a limit worth baking in |

Money is integer minor units, never a float.

`--int64-compat` restores the pre-Phase-10 aliases (`int` = `i64`,
`float` = `f64`) as a source-migration bridge. It is **not** a second runtime:
the Zeus C seam is 32-bit now, so programs that cross it need the new
signatures.

## Benchmark

`make bench` builds `packages/compiler/tests/bench/bench.yuga` (a fixed ~3000
node tree, 500 layout passes) with the pre-flip tree (`git HEAD`, `int` = i64)
and with the current default, and records arena counts, layout time, native
binary size, generated C size, and wasm size when `YUGA_WASM_CC` is set.

Representative run on macOS, headless, `-O0`:

| Metric | before (int = i64) | after (int = i32) |
|---|---|---|
| arena nodes | 3002 | 3002 |
| draw ops | 0 (paint not called) | 0 |
| layout, 500 passes | 6628 ms | 5941 ms |
| native binary | 352096 B | 335800 B |
| generated C | 856229 B | 785366 B |

The arena count is unchanged because Phase 10 only flips the language aliases;
the `Node` field layout is Phase 11. Binary and generated-C size drop because
32-bit arithmetic needs fewer instructions and narrower constants.

## Mirror types that had to change with the default

Two C-seam structures mirror a Yuga type and must use the same element width:

- `arena.sigs` (`[]int`) is written by the C signal allocators as `int32_t`
  elements now, and the rebuild-scope owner lists (`rec_sid` / `rec_owner`,
  also `[]int`) likewise. A 64-bit write into a 32-bit vector corrupted slot
  ids, which made `show` signals read 0 and leaked signal slots on rebuild.
- `string_from_bytes([]int)` reads `int32_t` elements for the same reason.

## Follow-ups (Phase 11)

- Narrow `Rect` / `Color` / `Node` / `Signal` and the draw-list ops, then
  regenerate the DRAW goldens in one reviewable commit.
- Revisit vector/string length storage at the C seam (currently `int64_t`).
