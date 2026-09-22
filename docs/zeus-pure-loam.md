# Zeus in Loam — the C elimination

**Goal.** No C runtime for Zeus. The whole GUI library — layout, math, signals,
the UI layer — implemented in Loam. C is touched only for bindings: the host
event loop, window/present, text measurement and glyph rasterization, font and
image decode, and the language allocator.

**Status: the port is real and partially landed; the C cell store is not gone
yet.** This file records what is done, what was attempted and reverted, and the
exact evidence for the remaining step — so the next attempt starts from a
narrow question rather than a broad one.

---

## 1. Where the stack actually is

Measured, not asserted.

| Layer | Where | State |
|---|---|---|
| Tree, arena, layout, paint, hit-test, focus, scroll, input, rasterizer, glyph atlas, SVG, images, virtual lists | `packages/zeus/std/zeuscore/*.loam` (~22k lines) | Loam (as before) |
| Design system (~50 widgets) | `packages/zeus-components/std/*.loam` (~6k lines) | Loam (as before) |
| Reactive core (`signal`/`effect`/`computed`/`scope`/`batch`/context) | `zeusbase.loam` + `zeuscore/track.loam` | Loam (as before) |
| **Signal payload store, six kinds, lifetime, change detection** | `zeuscore/memory.loam` | ✅ **landed this branch** |
| **Runtime verbs (trap guard, clock, heap, env, alloc counter)** | `loam_rt.h` | ✅ **landed this branch** |
| **Signal id allocation, free list, `sigs` mirror growth** | `zeus_plat.c` | ❌ **still C** — a Loam version was written and removed as dead code |
| **Cell ABI the compiler emits (`loam_zeus_sig_*`)** | `zeus_plat.c` + `codegen_c.c` | ❌ **still C** |

`ZeusCell` (the `malloc`'d per-signal buffer, free list, and `owns_str` flag) is
**still in `zeus_plat.c`**. Two attempts to remove it are recorded below.

## 2. What landed this branch

### Runtime verbs moved out of Zeus (step 1)

| Before | After |
|---|---|
| `loam_alloc_count` **defined** in `zeus_plat.c`, `extern` in `loam_rt.h` | defined in `loam_rt.h` under `LOAM_RT_DEFINE_ALLOC`, emitted by codegen |
| the only `setjmp` arm in the tree lived in `zeus_plat.c` | `loam_trap_guard` in `loam_rt.h`; the seam is one line |
| `plat_now_ms` reached into `loam_async_now_ms` | `loam_now_ms` — one monotonic clock in the runtime |
| `extern zeus_heap_used()` declared inside a seam function | `loam_heap_used`, a documented runtime verb |

One library's C no longer defines a language-runtime symbol. New generic verbs:
`loam_env_copy` / `loam_env_drop` (for an interned handler's env) and
`loam_trap_guard`.

### Equality on a comparable type parameter (Option B)

`==` / `!=` now work on a generic `T` bound to `int` / `float` / `bool` /
`string`. Ordering stays restricted to numbers. `check_mono_cmp_bounds` enforces
the bound **at the call site**, where the type argument is concrete — a generic
body is checked once with `T` still symbolic, so it cannot know its own bound.
A non-comparable argument is a Loam diagnostic, not a C compile error.

### The cell store, in Loam (`zeuscore/memory.loam`)

`cell_new` / `cell_get_*` / `cell_write_*` / `cell_same_*` / `cell_drop` for six
kinds, backed by the compiler's typed builtins (`__sig_push`, `__sig_load`,
`__sig_store`, `__sizeof`). Lifetime is **not** the engine's business: codegen
already retains/drops `string` and `[]T` payloads per instantiation, and
`zeus_cell.loam` proves it — a list written into a cell survives the caller's
buffer being dropped.

**One deliberate behaviour change**, pinned by `zeus_cell.loam`:

```
moved=1  same=0  same_str=0  diff_str=1
```

`same_str=0` — writing an **equal string** is now a no-op. The old C `memcmp`
compared the `loam_str { ptr, len, own }` header, so equal text in a different
buffer always counted as a write and re-ran every effect. This is not
byte-identical to the old behaviour, and the decision is stated rather than left
to whatever the goldens produced.

### One insets read instead of four

`plat_inset_get(&mut []int)` replaces four C globals and four getters, so a host
cannot hand layout a torn rectangle. `zeus_anchor_safe` (47/34 notch) passes.

### `Signal<T>` over a user's own struct — proven

`zeus_signal_types.loam` now carries a user struct through a signal **and** an
effect that re-runs when it changes. This was the open question behind
"reactivity over different data types"; the answer is that the generic path
already carried user types, and now a test says so.

### Compiler and std fixes this branch needed

| Fix | Why |
|---|---|
| **Generic struct literals didn't parse** — `Pair<int> { … }` read as `Pair < int > { … }`, surfacing as a misleading `expected expression (got {)` | `ident_targs_then_brace` lookahead takes the angles only when a `{` follows the `>`, so `if a < b {` is untouched. AST gained `struct_lit.targs`; typecheck prefers explicit targs over the expected type |
| **Monomorphized generic structs as `[]` elements didn't codegen** — `[]Entry<T>` inside a generic emitted a hook body naming `Entry__T`, which is correctly never defined | `collect_elem_hooks` registered element types from the type pool, including generic shapes. Hook bodies are emitted *after* the function loop, where `subst_names` is cleared. Now registers only `struct_targs_concrete` elements |
| **`Box` as a struct name misparsed silently** | `Box` is the heap-box keyword, so `Box<int>` is a box *of* int; the failure surfaced three stages later as `Box<int> does not match Box<int>`. Now a diagnostic at the declaration |
| **`std/map.loam`** — the language had no keyed container | open addressing, linear probing, tombstones, 0.75 load; content hashing; covers insert/replace, a stored `0` vs absent, a run-time-built key finding a literal's entry, growth over 500 keys, and churn not growing `len` |

## 3. Language facts the port ran into

Not preferences — constraints, each found by compiling something that failed.

**1. `int` is `i32`, not `i64`.** An out-parameter `[]int` is `int32_t`-elemented;
the generated C shows `loam_vec_push(..., sizeof(int32_t), ...)`. Writing
`int64_t *p` into it corrupts the buffer and silently loses the values —
`zeus_anchor_safe` is what caught it. Every new out-array binding must use
`int32_t`.

**2. A generic return position does not drive inference.** `cell_get<T>(c)` fails
with "cannot infer type parameter 'T'" even when the caller writes
`let n: int = cell_get(c)`. Thin forwarders fail the same way, so the readers and
comparisons are per-kind and each spells `__sig_load` itself — which is what
codegen used to do when it emitted the typed call. **Generics are viable where a
*value argument* binds `T`** (`cell_write_impl<T>(c, value, n)`).

**3. `&mut` does not coerce to a shared borrow at a call.** `cell_write(a, v)`
fails with "cannot match Cell to &mut Cell"; it must be `cell_write(&mut a, v)`.
The same applies to `std:map`'s readers, which take `&mut Map<T>`.

**4. Multiplication is checked, not wrapping.** Loam's `int` traps on overflow,
so FNV's offset basis (`2166136261`) does not fit and the prime cannot be applied
at full width. `std/map.loam` computes the product in `i64` and folds to 26 bits;
the provenance is FNV, the width is ours, and the code says so.

**5. A generic struct literal cannot stand mid-expression.** `Map<T> { … }` in an
expression position parses as a block, so construction goes through named helpers
(`map_of`, `entry_empty`).

**6. String retention is not expressible outside `__sig_store`.** `std:map`
therefore borrows its keys rather than owning them — a documented limitation, and
the next thing worth fixing if `Map` should own them.

## 4. The reverted attempts, and why

**Attempt 1 — the dual-emit cell ABI.** A codegen mode where `loam_sig_push` /
`_load` / `_store` call functions the *generated program* defines in Loam, rather
than the C ABI. This is the right design — it is how `LOAM_HOST_BUILD` and the
`engine_*` table already work here — and it compiled. It was reverted because it
had grown to a new codegen branch, a naming convention, and per-kind forwarders
in `zeusbase`, all unverified at once. `docs/boundary.md` says C should own only
a host loop or an ABI trampoline; this is the change that enforces it, and it
should be done as its own verified pass.

**Attempt 2 — the generation counter, in Loam.** The counter itself worked:

```
n.gen: 0 → 1     # read back through both arena and track's injected reader
```

but `computed` stopped recomputing:

```
FAIL computed recomputed got 0 want 6
```

Instrumenting the comparison showed the inputs were *correctly* detected as
changed:

```
edge sig=1  recorded=0  live=1     # they differ
doubled=0                          # and the memo still did not recompute
```

So the break is **downstream of the comparison** — in whichever of
`pull` → `memo_recompute` → `step` returns early (`memo_state` gating,
`produced_by`, `memo_out`). Reverted rather than shipped: a wrong `memo_state`
transition does not fail loudly, it silently stops recomputing in every widget,
and one test catching it is luck rather than safety. The Loam allocator written
alongside it was removed as dead code, since nothing calls it while the callers
sit on the C path.

## 5. What remains

| # | Work | Notes |
|---|---|---|
| 1 | **Land the dual-emit cell ABI** (attempt 1) | the design is settled; needs its own verified pass |
| 2 | **Find the `memo_state` break** (attempt 2's blocker) | narrow: instrument the four early returns in `memo_recompute` and see which fires |
| 3 | Remove `ZeusCell` from `zeus_plat.c` | follows 1 and 2 |
| 4 | Interning table in Loam over `loam_env_copy` | needs 1 |
| 5 | Edit machine in Loam over `[]u8` + `std:unicode` | the host text ABI must use `int32_t` for an out-array |
| 6 | Key chords in Loam | — |
| 7 | Rename `platform.loam` → `bindings.loam` with `bnd_*` | — |
| 8 | Enforcement gate in `nob test` | reject library C that is not a host loop or an ABI trampoline |

`plat_sig_free` is the one piece that genuinely belongs in C: releasing a payload
by id needs the bound type, and the type is a compile-time property of the call
site that bound it. Everything else about a signal cell is policy, and policy
belongs in Loam.




---
