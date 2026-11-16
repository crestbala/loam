# The C / Loam boundary

There is exactly one C runtime in the tree: **`loam_rt`** (`packages/loam/runtime/loam_rt.h`,
included into generated C). No library gets a `*_rt.c` of its own as a place
to hide protocol or widget logic. Zeus layout/paint/hit-test and http
encode/dispatch live in Loam. Hosts (Cocoa / UIKit / Canvas / JNI) own the
event loop and call the engine's Loam exports (`loam_zeus_engine_*`)
directly, with no C layer in between.

## Convention

A **boundary module** (today: `packages/zeus/src/zeuscore/platform.loam`, empty `fn`s in
`packages/loam/std/fmt.loam` / `packages/loam/std/net.loam` / `packages/loam/std/sys.loam`) declares plain, **bodyless** functions:

```loam
fn plat_set_window(title: string, width: int, height: int) {}
```

The compiler assigns `loam_<module>_<name>` (e.g. `loam_platform_plat_set_window`)
and emits a declaration only (`is_intrinsic`). The matching C symbol is linked
from `loam_rt` or from a **host** that drives the event loop — not from a
library-specific runtime file that contains retries, parsers, or widget code.

No `impl`. Method chaining is UFCS (`n.pad(8)` → `zeus.pad(n, 8)`).
`Node` is `{ id: int }`, not `Node<Backend>`. Generics are allowed in `zeus.loam`
where they help (`each<T>`); they are not a substitute for a
backend type parameter.

The other direction: C calls compiled Loam as `loam_zeus_engine_paint`,
`loam_http_dispatch`, and so on. A `string` parameter of such an export is
borrowed for the call, so C may pass a `loam_str` view over its own buffer
(`own = 0`); an export that keeps the text copies it. A returned `string` is
the caller's to `loam_str_release`.

## Calling C libraries

The boundary module above is for the runtime and its hosts. A package that wraps
a C library declares the C it needs with `#[c]`, and lists its sources beside it:

```loam
#![c_source("vendor/sqlite3.c", "-DSQLITE_OMIT_LOAD_EXTENSION")]
#![c_link("-lm")]

#[c("sqlite3_libversion")]
fn libversion() -> string {}

#[c("sqlite3_close")]
fn close(db: i64) -> i32 {}
```

`#[c("sym")]` goes on a bodyless, non-generic fn. The compiler writes a wrapper
that calls `sym`. A module's `#![c_source]` and `#![c_link]` count only while one
of its `#[c]` fns is in use, so an unused binding links nothing.

- `#![c_source("path", flags...)]`: a path relative to the `.loam` file, inside its
  directory. It compiles to a cached object, rebuilt when the source, a header
  beside it, or the flags change.
- `#![c_link(flags...)]`: flags for the link line, such as `"-lm"`, or
  `"-framework", "Security"`.

| Loam | C | Notes |
|---|---|---|
| `i8`..`i64`, `u8`..`u64`, `int` (32 bits) | The same width | |
| `float` (32 bits), `f64` | `float`, `double` | |
| `bool` | `bool` | |
| `i64` | Any pointer | Never `int`, which truncates a pointer |
| `string` argument | `const char *` | A NUL-terminated copy of every byte, freed after the call |
| `string` result | `const char *` | Copied up to its first NUL. NULL becomes `""`. C keeps ownership |
| `[]` of numbers or bools | `T *` | Owned. Retained for the call and dropped after. C may write through it. An empty one passes NULL |
| `&[]T` / `&mut []T` of numbers or bools | `T *` | Borrowed. No retain and no drop; the caller keeps the buffer. C writes only through `&mut`. An empty one passes NULL |
| none | `void` | Only as a result |

Bytes with NULs inside:

- Into C, pass a `string` and its length to a function that takes both. The copy
  lives for the call only, so C must copy the bytes if it keeps them.
- Out of C, take the pointer as an `i64`, `memcpy` into a `[]u8`, and call
  `string_from_bytes`.
- A `[]u8` holds one byte per element. A `[]int` holds four.

C must not keep a pointer it was handed. A plain-data struct (every field a
number, a bool, or such a struct) passes by value. A variadic binding
(`#[c("sym", "variadic after N")]`) may pass an `i32`, `u32`, `i64`, `u64`,
`f64`, string, or a number/bool buffer, owned or borrowed, through `...`. A
parameter that is a fn of numbers and bools takes a named module-level fn,
and C calls that symbol. Closures, `[]string`, and any other struct do not
cross; a struct that is not plain data still needs a trampoline in the
package, as `packages/crypto` has for Argon2. A thread worker cannot call a
`#[c]` fn.

A binding can name the libraries its symbol lives in:
`#[c("SSL_new", "link -lssl -lcrypto")]`, or `"link -framework Security"`.
Only `-lNAME` and `-framework NAME` are accepted. The driver links them only
when a kept binding that builds for this target names them, each library
once, in the order bindings are declared. So a program that never makes a
TLS connection links neither libssl nor Security.framework, even though
`std:net` binds both. A module-wide `#![c_link]` applies whenever any
binding of the module is kept.

C runs on native builds only. A wasm, iOS, or Android build refuses a `#[c]` fn it
can reach, and one only a `#[server]` body calls leaves nothing in that build.

## Bindings in std

`std` modules bind libc directly, with no `#![c_source]`: the symbol comes
from the C library every program already links. `std:batch` binds `fork` /
`wait` / `open` / `creat` this way, and `std:sys` binds `isatty` for
`stdout_tty`. The same rules as package bindings apply, plus two:

- A `#[c]` fn is private to its module, so the binding sits beside a safe
  wrapper in the same `std` module, and importers call only the wrapper.
  Loam-side per-OS differences are `#[os]` variants (`posix` binds the libc
  symbol, `wasm` is a Loam stub matching today's runtime answer, `windows`
  wraps a differently named hook until its own binding lands).
- Until R2.6, Windows keeps its C behind a `#[os("windows")]` Loam wrapper
  fn calling a differently named hook, so the committed seed still builds
  unchanged on Linux and Windows without running the Windows CI job.

## What belongs where

| Layer | C | Loam |
|---|---|---|
| Language (`loam_rt`) | Allocator, panic/trap, overflow, wrapping bit ops, `string_from_bytes`, `loam_fn` `{fn, env, env_size}`, `env_set` / `exit` | — |
| net (`packages/loam/std/net`) | `string_bytes_at` (a string's byte address for a binding to read in the same call), the wasm fetch / WebSocket / stream bridges | TCP over raw libc bindings (R5.1); the TLS client over SecureTransport bindings on Apple (R5.2) and OpenSSL bindings on Linux (R5.3), its I/O callbacks (SecureTransport's, OpenSSL's BIO) Loam fns C calls through function pointers |
| Zeus | Host event loop + replay of the draw list; `runtime.intern_fn` memcpy's a `loam_fn` env so click/styled handlers outlive the interned value | Tree, layout, signals, design system, `platform.loam` signatures |
| Maya | The package's Cocoa host (`packages/maya/src/hosts/mac.m`): event loop and draw primitives (blit, discs, rings, labels), int-only both ways | Scene, orbits, camera, CPU tracer, 2D map layout, draw order and the RGBA frame, `sin`/`cos` |
| raygui | raylib and raygui themselves (vendored `raygui.h` compiled as itself); no shim | API, frame loop, window state, headless rule, RAM probe and every retained value; raylib/raygui reached by `#[c]` bindings (`packages/raygui/src/raygui.loam`) |
| http | — | unary gRPC: `#[proto]`, dispatch, path/frame, HTTP/1.1 parse, h2c, `listen`, client `rt_call` / `h2_call`, loopback `h1_read_unary` / `h2_read_unary`, HEADLESS selftest |

A new `*_rt.c` outside `loam_rt`, or a `loam_rt` stub that knows Zeus widgets
or protobuf field numbers, is the old per-library runtime sneaking back.

Bodyless `fn`s must stay bodyless: no branches, retries, or transforms in the
boundary declaration. That logic goes in the ordinary Loam function that calls
them.

## Review

- New library-specific C that is not a host event loop or a generic ABI trampoline — reject.
- Boundary `fn` with a real body — reject (unless you are *moving* logic *out* of C into Loam, which is the goal).
- “This library needs its own runtime” — it needs bodyless signatures; extend `loam_rt` only for a reusable ABI.
