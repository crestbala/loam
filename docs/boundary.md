# The Loam ABI boundary

There is exactly one native runtime in the tree: **`loam_rt`**
(`packages/loam/runtime/loam_rt.h`, linked as a native object into every
program). No library gets its own runtime object as a place
to hide protocol or widget logic. Zeus layout/paint/hit-test and http
encode/dispatch live in Loam. Hosts (Cocoa / UIKit / Canvas / JNI) own the
event loop and call back into `engine_*` trampolines.

## Convention

A **boundary module** (today: `packages/zeus/src/zeuscore/platform.loam`, empty `fn`s in
`packages/loam/std/fmt.loam` / `packages/loam/std/net.loam` / `packages/loam/std/sys.loam`) declares plain, **bodyless** functions:

```loam
fn plat_set_window(title: string, width: int, height: int) {}
```

The compiler assigns `loam_<module>_<name>` (e.g. `loam_platform_plat_set_window`)
and emits a direct call to that symbol following the platform C ABI
(`is_intrinsic`: no Loam body emitted). The matching native symbol is linked
from `loam_rt` or from a **host** that drives the event loop — not from a
library-specific runtime object that contains retries, parsers, or widget code.

No `impl`. Method chaining is UFCS (`n.pad(8)` → `zeus.pad(n, 8)`).
`Node` is `{ id: int }`, not `Node<Backend>`. Generics are allowed in `zeus.loam`
where they help (`each<T>`); they are not a substitute for a
backend type parameter.

The other direction: C calls compiled Loam as `loam_zeus_engine_paint`,
`loam_http_dispatch`, and so on.

## Do I need C to run Loam?

No. A Loam binary executes like any native binary: the OS loader maps it
(Mach-O / ELF / PE) and jumps to its entry point. Pure-Loam code —
`fmt.println`, vectors, maps, borrowck, traps, `zeus` layout — calls itself
with Loam's own internal convention. No C semantics leak in.

The C ABI is required only at the edges:

- the single `main` handshake, if you link against the platform startup
  objects instead of owning `_start` (freestanding/embedded skips even this);
- calls **out** through `#[c]` (§Calling C below);
- calls **in** from C-ABI languages through future `#[export]`.

## Calling C (and C-ABI) libraries

Loam speaks the platform C ABI natively — calls follow the platform calling
convention and need no C compiler in the loop. (Vendored C *sources* still
need a C compiler to build — see `#![c_source]` below. That compiles the
C library; it never touches Loam code.) A package that wraps
a C library declares the native symbols it needs with `#[c]`, and lists its
sources beside it:

```loam
#![c_source("vendor/sqlite3.c", "-DSQLITE_OMIT_LOAD_EXTENSION")]
#![c_link("-lm")]

#[c("sqlite3_libversion")]
fn libversion() -> string {}

#[c("sqlite3_close")]
fn close(db: i64) -> i32 {}
```

`#[c("sym")]` goes on a bodyless, non-generic fn. The compiler emits a direct
machine-code call to `sym` following the C ABI. A module's `#![c_source]` and `#![c_link]` count only while one
of its `#[c]` fns is in use, so an unused binding links nothing.

- `#![c_source("path", flags...)]`: a path relative to the `.loam` file, inside its
  directory — e.g. `packages/sqlite/src/vendor/sqlite3.c` or a small shim
  beside the binding module. It is compiled with a C compiler to a cached
  native object, rebuilt when the source, a header beside it, or the flags
  change. **This is the only place a C compiler is ever invoked**: it builds
  the vendored/third-party C library (or a few-line ABI shim where C types
  don't fit — `char **argv`, structs-by-value, Objective-C messages, as
  `packages/crypto` has for Argon2). Loam code itself is never emitted as C.
- `#![c_link(flags...)]`: flags for the link line, such as `"-lm"`, or
  `"-framework", "Security"`.

### Importing a C file or header (vendor flow)

To use a vendored C library — `vendor/sqlite3.c`, a HAL, a codec — the
package does three things, and nothing else:

1. **Drop the sources in**: `src/vendor/*.c` (+ their headers) next to the
   binding module, inside the package directory.
2. **Declare the build**: `#![c_source("vendor/foo.c", "-O2", ...)]` and any
   `#![c_link(...)]` at the top of the binding module. Only modules whose
   `#[c]` fns are actually used build and link — unused bindings cost nothing.
3. **Declare the symbols**: one bodyless `#[c("c_symbol_name")] fn` per C
   function, using the type mapping below. The compiler emits a direct
   machine-code call per the C ABI — no wrapper C, no header parsing.

```loam
// packages/mylib/src/mylib.loam
#![c_source("vendor/mylib.c", "-O2")]
#![c_link("-lm")]

#[c("mylib_init")]
fn init(seed: i64) -> i32 {}

#[c("mylib_process")]
fn process(data: &[]u8, len: int) -> i32 {}
```

There is no `#include` of C headers and no C-header importer: the binding
module *is* the header, written in Loam with Loam types. C structs cross only
as opaque handles (`i64` carrying the pointer — never `int`, which truncates
it) or as `[]`-of-numbers buffers C may write through; C functions taking
structs by value need a few-line C shim in the package (no logic, just the
trampoline). Header constants become Loam `const`s by hand — auditable, one
line each, and part of the reviewed trusted surface (`S3: loamc check --ffi`
lists every `#[c]`/`c_source` in the tree).

| Loam | C | Notes |
|---|---|---|
| `i8`..`i64`, `u8`..`u64`, `int` (32 bits) | The same width | |
| `float` (32 bits), `f64` | `float`, `double` | |
| `bool` | `bool` | |
| `i64` | Any pointer | Never `int`, which truncates a pointer |
| `string` argument | `const char *` | A NUL-terminated copy of every byte, freed after the call |
| `string` result | `const char *` | Copied up to its first NUL. NULL becomes `""`. C keeps ownership |
| `[]` of numbers | `T *` | C may write through it. An empty one passes NULL |
| none | `void` | Only as a result |

Bytes with NULs inside:

- Into C, pass a `string` and its length to a function that takes both. The copy
  lives for the call only, so C must copy the bytes if it keeps them.
- Out of C, take the pointer as an `i64`, `memcpy` into a `[]u8`, and call
  `string_from_bytes`.
- A `[]u8` holds one byte per element. A `[]int` holds four.

Structs, closures, `[]string`, and C variadics do not cross. Neither does a call
back from C into Loam yet (a future `#[export]` will expose Loam fns with C
linkage). A C function that takes a struct by value needs a small
trampoline in the package, with no logic, as `packages/crypto` has for Argon2. A thread worker cannot call a `#[c]` fn.

Native (`#[c]`) calls link on native builds only. A wasm, iOS, or Android build refuses a `#[c]` fn it
can reach, and one only a `#[server]` body calls leaves nothing in that build.

Any language speaking the platform C ABI — C, C++, Rust (`extern "C"`),
Zig, Swift, D, and the OS toolkits (Cocoa/UIKit Objective-C, JNI, X11,
Win32) — can both **call** compiled Loam (`#[export]` entry points such as
`loam_zeus_engine_paint`, `loam_http_dispatch`) and **be called** from Loam
(`#[c]` bindings). The direction Loam → C works today; C → Loam entry
points land with `#[export]`. ABI conformance is pinned by
`packages/loam/tests/abi/` (calling-convention fixtures per target).

## What belongs where

| Layer | Native (C ABI) | Loam |
|---|---|---|
| Language (`loam_rt`) | Allocator, panic/trap, overflow, wrapping bit ops, `string_from_bytes`, `loam_fn` `{fn, env, env_size}`, `env_set` / `exit` | — |
| net (`packages/loam/std/net`) | TCP connect/read/write/close, listen/accept/peek, wasm `fetch_rpc` | — |
| Zeus | Host event loop + replay of the draw list; `runtime.intern_fn` memcpy's a `loam_fn` env so click/styled handlers outlive the interned value | Tree, layout, signals, design system, `platform.loam` signatures |
| Maya | Host event loop + present (Cocoa blit / 2D discs) | Scene, orbits, camera, CPU tracer, 2D map layout, `sin`/`cos` |
| raygui | Host window + event pump (raylib) and a thin raygui trampoline (`runtime/raygui_plat.c`); no widget logic | API, frame loop, and all retained state (`packages/raygui/src/raygui.loam`) |
| http | — | unary gRPC: `#[proto]`, dispatch, path/frame, HTTP/1.1 parse, h2c, `listen`, client `rt_call` / `h2_call`, loopback `h1_read_unary` / `h2_read_unary`, HEADLESS selftest |

A new runtime object outside `loam_rt`, or a `loam_rt` stub that knows Zeus widgets
or protobuf field numbers, is the old per-library runtime sneaking back.

Bodyless `fn`s must stay bodyless: no branches, retries, or transforms in the
boundary declaration. That logic goes in the ordinary Loam function that calls
them.

## Review

- New library-specific native code that is not a host event loop or a generic ABI trampoline — reject.
- Boundary `fn` with a real body — reject (unless you are *moving* logic *out* of C into Loam, which is the goal).
- “This library needs its own runtime” — it needs bodyless signatures; extend `loam_rt` only for a reusable ABI.
