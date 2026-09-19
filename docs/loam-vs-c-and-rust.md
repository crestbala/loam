# Loam vs C and Rust

Loam is **Odin-shaped syntax with Rust-shaped ownership** ([downsides.md](downsides.md)).
`loam` is written in C11, typechecks a program, lowers it to IR, emits **C99**,
and invokes `cc` ([loam.md](loam.md)). C is the *platform binding target*, not
the language's semantics ([boundary.md](boundary.md)).

This document compares Loam with C and Rust, and is deliberately blunt about
**where Loam is worse**. Advantages first, disadvantages in depth. It describes
the tree in this repository; where a claim is an observation from working in the
code (not a documented rule) it is marked *(observed)*.

Companion docs:
[loam.md](loam.md) — architecture and how to write programs ·
[boundary.md](boundary.md) — the C seam ·
[downsides.md](downsides.md) — accepted language limits and the phase list ·
[numeric-widths.md](numeric-widths.md) — `int` = `i32`, overflow, packing ·
[../packages/zeus/docs/spec.md](../packages/zeus/docs/spec.md) — the UI framework.

---

## 1. What Loam is, in one screen

| Thing | Loam |
|---|---|
| Input | `.loam` source, quoted imports (`std:`, relative, `pkg:`) |
| Compiler | C11, whole-program: lex → parse → typecheck → borrowck → boundscheck → IR → codegen C99 → `cc` |
| Output | C99, compiled by the system C compiler (native, wasm, iOS, Android) |
| Types | `int` / `float` / `bool` / `string` / `[]T` / `[N]T` / `Box<T>` / `&T` / `&mut T` / `fn`; structs; C-style `enum` as named ints |
| Ownership | exclusive vs shared, moves, auto-borrow, deterministic drops; `Box<T>` not Copy; `[]T` Copy when `T` is Copy (refcounted, copy-on-write on `push`) |
| Memory management | no GC: deterministic drops + refcounted arrays; strings are heap-allocated and **never freed** today |
| Generics | monomorphized (`struct Pair<T>`, `fn id<T>`, nested calls, constant defaults) |
| Concurrency | OS threads + `Chan<T>` (`std:thread`); `async fn` / `await` over a hand-driven pump |
| Errors | `Res<T>` + `#[must_check]`; no `?`, no language `Option` / `Result` |
| Extensibility | no traits, no `impl`, no operator overloading, no macros, no `unsafe`, no lifetimes |
| Interop | bodyless `fn` **is** the C hook; the compiler assigns `loam_<module>_<name>` |

Design stance, from [downsides.md](downsides.md): *features land only when a
program needs them*. Missing traits, lifetime syntax, algebraic enums, macros,
and iterators are treated as the point, not as a backlog.

---

## 2. At a glance

| Axis | C | Rust | Loam |
|---|---|---|---|
| Memory safety | manual; UB on error | strong (borrowck, lifetimes) | moderate (borrowck, simpler; no lifetimes) |
| Bounds checks | none | checked, statically elided | checked, statically elided (`sema/boundscheck.c`) |
| Integer overflow | wraps (UB) | checked in debug, wraps in release (configurable) | checked by default (`__builtin_*_overflow`) |
| Reclamation | manual `free` | deterministic drops + `Rc`/`Arc`/GC-free | deterministic drops + refcounted `[]T`; strings leak |
| GC | none | optional, not default | none |
| Compile target | native | LLVM: native + wasm | C99 → `cc`: native + wasm + iOS + Android |
| C interop | native | good, needs `extern "C"` / `repr(C)` | native (same shape; symbol naming) |
| Generics | none | traits, associated types, lifetimes | monomorphized structs/fns; no bounds |
| Sum types / matching | none | `enum` with exhaustive `match` | none; `match` is a C-style switch |
| Error handling | return codes | `Result` + `?` | `Res<T>` + `#[must_check]` |
| Toolchain size | tiny | large | tiny (compiler is readable C11) |
| Ecosystem | huge | huge | young; no package registry |
| Debugger story | native | native | you debug **generated C** (`#line` maps back) |
| Platform reach | everywhere | everywhere | C targets; GUI hosts are narrow (Cocoa / UIKit / JNI / Canvas2D / Linux X11) |

---

## 3. Advantages of Loam

### 3.1 Over C

- **Memory and bounds safety by default.** `sema/borrowck.c` enforces exclusive
  vs shared access, moves, and place paths; `sema/boundscheck.c` proves in-range
  indices at compile time and leaves a trap otherwise. C offers neither.
- **Checked arithmetic.** Overflow traps instead of silently wrapping (the
  runtime uses `__builtin_*_overflow`), so a whole class of C bugs disappears by
  construction.
- **Modern ergonomics that keep the C ABI.** Closures (`loam_fn` = fn pointer +
  env), monomorphized generics, UFCS method sugar (`n.pad(8)` → `zeus.pad(n, 8)`),
  named arguments that fill a trailing props struct, trailing blocks for UI
  trees, arrays with refcounting/copy-on-write, and a proper `string`.
- **Deterministic destruction.** IR computes drops; `Box`/`[]T`/closures are
  freed at scope exit or last owner. No GC, predictable latency.
- **Portability via C.** Emitting C99 + `cc` means native, wasm, iOS, and
  Android for free, with generated C you can read and debug.
- **Cheap C interop.** A bodyless `fn` *is* the boundary; the compiler assigns
  the C symbol. Calling into a C framework (Cocoa, canvas, JNI) is the normal
  path, not a special one — the whole Zeus UI is built this way.

### 3.2 Over Rust

- **The compiler is small enough to read end to end.** A dozen C files
  (`lexer`, `parser`, `sema/*`, `ir.c`, `codegen_c.c`, `dce.c`), no LLVM, no
  proc-macros, no cargo-scale tooling. `make` bootstraps a seed compiler and
  `nob.loam` takes over.
- **C-first interop, by construction.** Rust's C interop needs `extern "C"`,
  `#[repr(C)]`, and care at every boundary; Loam's entire shape *is* the C
  boundary.
- **Fewer type-system battles in app code.** Go-ish conveniences (implicit
  module paths, copy-on-write slices, `must_check` results, UFCS chains) and a
  simpler borrow checker mean less ceremony for retained/UI/tooling code.
- **No trait/lifetime vocabulary to design around.** You write `fn`s and
  structs; method syntax is sugar over free functions.
- **Whole-program reachability (DCE).** `dce.c` prunes everything not reachable
  from the entry point plus the fixed C roots (`engine_*`, `get`/`set`/
  `signal`, …), which keeps small shipped binaries small.

---

## 4. Disadvantages of Loam

This is the important half. Each item is a real cost of choosing Loam over C or
Rust, not a hypothetical.

### 4.1 Safety is weaker than Rust, and the gaps are structural

- **No lifetimes, no `unsafe`, no traits.** The type system cannot encode the
  invariants Rust can. There is no escape hatch to express "this is fine,
  trust me" and no trait system to abstract over capabilities.
- **Borrow checking is simpler.** Borrows end at last use (described as NLL-ish
  in [loam.md](loam.md)), but [downsides.md](downsides.md) still lists
  **"borrowck is not NLL"** as a cost that bites. Expect Rust-grade
  reasoning to fail where you'd want it.
- **Closures capture Copy values only.** No closure can hold a borrow or a
  non-Copy owned value. This rules out patterns Rust users reach for routinely
  (callbacks that own a buffer, iterator chains over borrowed data).
  *(observed)* A closure that **mutates a captured `let mut` local traps** — I
  reproduced this on an unmodified checkout, so it is not a consequence of any
  particular change; treat mutable capture as unusable.
- **No memory-safety claim over the C seam.** Every host and boundary module is
  C; nothing in the type system protects that half of the program.
- **Strings leak.** `loam_rt` heap-allocates strings and never frees them — the
  runtime comment admits there is "no string ownership story". A long-running
  program that parses text grows without bound. C at least lets you free; Rust
  frees automatically.
- **Thread safety is a discipline, not a proof.** `std:thread` + `Chan<T>` are
  built to keep workers off module state ("Send discipline"), but there is no
  `Send`/`Sync` checking — the guarantee is by convention.

### 4.2 The type system and language are intentionally minimal

These are choices, not bugs, but they are **costs**:

- **No algebraic enums, no language `Option` / `Result`.** You get a C-style
  `enum` (named ints) and `Res<T>` in `std:result`. Sum types are emulated with
  fields + conventions.
- **No pattern matching.** `match` is a switch over literals, enum members, and
  `_`; there are no binding patterns, destructuring, or exhaustiveness over
  structured data.
- **No operator overloading, no traits, no `impl`.** No `Display`, no generic
  algorithms over a trait; abstraction is free functions and monomorphized
  generics only.
- **No macros, no comptime, no attributes beyond a few** (`#[must_check]`,
  `#[proto]`, `#[test]`).
- **Iteration is `for i in lo..hi` only** — no iterators, no `for x in xs`, no
  labeled break.
- **No `?` operator.** Error plumbing is an explicit `if !r.ok { … }`, and
  `#[must_check]` only forces you to *look*, not to propagate.
- **Auto-borrow and moves are implicit.** Convenient, but the rules that decide
  when a value moved vs. was copied live in the compiler's reasoning, not in
  syntax you can read off the page.

### 4.3 Generics are monomorphized but shallow *(observed)*

Working in the tree, the generic edges show:

- **A generic struct whose type parameter does not appear in a field cannot be
  inferred or constructed.** `struct Context<T> { id: int }` fails to build;
  you must add a `T`-typed field to anchor inference.
- **Returning a user-defined generic struct from a generic function can fail**
  with `return type Box<T> does not match Box<T>` for structurally identical
  instantiations.
- **Everything is monomorphized**, so a generic function used at many types is
  emitted many times: code-size blow-up with no `dyn` escape hatch.

The stdlib works around this by specializing where needed (e.g. `computed` /
`computed_str` / `computed_bool` rather than one generic), which is a real
ergonomic tax.

### 4.4 The C99 backend leaks C into Loam

Because the backend is C, C's constraints surface in a language that does not
otherwise have them:

- **C keywords are forbidden identifiers.** A parameter named `default` fails:
  `invalid parameter name: 'default' is a keyword`. You are coding in C's
  namespace whether you intended to or not. *(observed)*
- **Source names couple to the C symbols.** The backend emits
  `loam_<module>_<name>` for globals and boundary functions; a global the host
  C reads (e.g. Zeuss `arena.sigs` → `loam_arena_sigs`) cannot be renamed
  without editing the host. *(observed)*
- **A codegen edge can surface as a C error, not a Loam error.** *(observed)*
  With a generic function referencing a module global, the generated C
  referenced helper names the runtime does not define (`loam_push` / `loam_pop`),
  so the failure came from `cc`, not from Loam's own diagnostics.
- **You debug generated C.** [downsides.md](downsides.md) lists this under
  costs: "C99 + `cc` backend (debuggers see generated C)". `#line` directives map
  back, but the mental model is C's.
- **A C compiler is a hard build dependency**, and the emitted C can be large
  (`--emit-c` grew further when `#line` interleaving landed; see
  [numeric-widths.md](numeric-widths.md)).

### 4.5 Performance is C-shaped, not Rust-shaped — with its own tax

- **No zero-cost abstractions at Rust's level.** No traits, no generic bounds,
  no compiler inlining story comparable to LLVM. You get `cc`'s optimizer over
  generated C.
- **Checks cost something at runtime.** Bounds and integer overflow are on by
  default; only statically-provable indices are elided.
- **Monomorphization duplicates code** (see 4.3).
- **`int` is `i32` by default** after the Phase 10 width flip, with a
  `--int64-compat` migration switch that is explicitly "not a second runtime".
  Code that assumed 64-bit `int` must be audited; crossing the (now 32-bit) C
  seam needs the new signatures ([numeric-widths.md](numeric-widths.md)).
- **Copy-on-write arrays** are ergonomic but can copy unexpectedly on `push`
  when shared, and refcount traffic is not free.

### 4.6 Tooling, build, and ecosystem are young

- **No package registry.** Dependencies are vendored (`import "pkg:name"` →
  `vendor/`) or path-based; nothing resolves versions for you.
- **Hand-maintained syntax support.** The editor grammar
  (`tooling/tree-sitter-loam`) is maintained separately from the compiler and
  "rots silently while loam moves on" unless `make grammar` is run; there is no
  compiler-generated grammar.
- **The pipeline is whole-program** — one generated C file plus whole-program
  DCE and monomorphization — rather than a separate-compilation model.
- **A small standard library and no ecosystem.** `std` is `fmt`, `net`, `sys`,
  `thread`, `math`, `str`, `time`, `kv`, `json`, `result`, `test`; the rest is
  the frameworks in this repo. There is no crate-scale library base to draw on.
- **`make` is only a bootstrap;** the real build is `nob.loam`, so build logic
  is itself a program you have to be able to build.

### 4.7 Portability and platform coverage are narrow where it counts

- **Native GUI hosts are limited:** Cocoa (macOS), UIKit (iOS), Android JNI, and
  Canvas2D (web), plus an X11 fill/text host on Linux. This is not a
  cross-platform widget toolkit.
- **wasm is second-class in places:** `thread.spawn` is a no-op on wasm
  ([downsides.md](downsides.md)); TLS/`call_async` behavior differs by host.
- **The HTTP server is single-threaded** ([downsides.md](downsides.md)).
- **The UI is single-threaded by design** — completion returns to the one UI
  thread; "not a multi-threaded UI" is an explicit non-goal.
- **Touch vs pointer still differs per host** (hover, CSS `cursor` are skipped
  on iOS/Android), so "write once" has per-host caveats.

### 4.8 Async is hand-rolled and constrained

- **No CPS split.** `await` is a typed call that re-enters a pump; control flow
  around it must "just work" (it does, but it is the language's job that it
  does, and the machinery is a pump, not a runtime).
- **Async TLS is host-specific.** It was not in the base design; Phase 8 added
  it on native/Android via non-blocking SecureTransport, and wasm delegates to
  browser `fetch`.
- **`Future<T>` / `Chan<T>` are typed mailboxes over C seams**, not a general
  scheduler; there is no task priority, cancellation, or structured concurrency.

### 4.9 Maturity

Loam is a young, opinionated language used to build this repository's tooling
and UI. It has an LSP, a formatter, a test suite (compile_pass / compile_fail /
golden / draw_golden / inlang / bench), and a real app framework — but no
ecosystem, no broad third-party code, and fewer magic-bullet diagnostics than
Rust or mature C toolchains. Betting on it means betting on a small language
you may have to fix yourself.

---

## 5. When to choose which

- **C** — you need total control, the broadest ABI reach, or to audit every
  byte, and you accept that safety is entirely your problem.
- **Rust** — you want the strongest correctness guarantees, expressive
  abstractions (traits / `Result` / pattern matching), and a large ecosystem,
  and you can pay the compile-time and complexity costs.
- **Loam** — you are building a **C-adjacent app/UI or tooling layer** where
  portability and C interop dominate, you want a small compiler you can read and
  change, deterministic memory, and enough safety to remove the worst bugs — and
  you do **not** need Rust's guarantees or C's absolute control. Accept the
  costs in §4: no traits/lifetimes, leaking strings, a C-namespace backend (and
  C's keyword restrictions), shallow generics, and a narrow platform/ecosystem
  base.

---

## 6. Keeping this honest

The disadvantages above are load-bearing: several are listed as accepted costs in
[downsides.md](downsides.md) ("Costs that still bite"), and the ones marked
*(observed)* were reproduced against this tree. When a cost here is fixed, move
it — do not let this doc drift into marketing. Roughly, the ones most likely to
change soon are the generics edges (§4.3), the string-freeing story (§4.1), and
the mutable-closure trap (§4.1); the type-system and backend items (§4.2, §4.4)
are stated as deliberate non-goals.
