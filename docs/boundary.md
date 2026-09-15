# The C / Loam boundary

There is exactly one C runtime in the tree: **`loam_rt`** (`packages/yuga/runtime/loam_rt.h`,
included into generated C). No library gets a `*_rt.c` of its own as a place
to hide protocol or widget logic. Zeus layout/paint/hit-test and http
encode/dispatch live in Loam. Hosts (Cocoa / UIKit / Canvas / JNI) own the
event loop and call back into `engine_*` trampolines.

## Convention

A **boundary module** (today: `packages/zeus/std/zeuscore/platform.loam`, empty `fn`s in
`packages/yuga/std/fmt.loam` / `packages/yuga/std/net.loam` / `packages/yuga/std/sys.loam`) declares plain, **bodyless** functions:

```yuga
fn plat_set_window(title: string, width: int, height: int) {}
```

The compiler assigns `loam_<module>_<name>` (e.g. `loam_platform_plat_set_window`)
and emits a declaration only (`is_intrinsic`). The matching C symbol is linked
from `loam_rt` or from a **host** that drives the event loop — not from a
library-specific runtime file that contains retries, parsers, or widget code.

No `extern` keyword. No `impl`. Method chaining is UFCS (`n.pad(8)` → `zeus.pad(n, 8)`).
`Node` is `{ id: int }`, not `Node<Backend>`. Generics are allowed in `zeus.loam`
where they help (`each<T>`); they are not a substitute for a
backend type parameter.

The other direction: C calls compiled Loam as `loam_zeus_engine_paint`,
`loam_http_dispatch`, and so on.

## What belongs where

| Layer | C | Loam |
|---|---|---|
| Language (`loam_rt`) | Allocator, panic/trap, overflow, wrapping bit ops, `string_from_bytes`, `loam_fn` `{fn, env, env_size}`, `env_set` / `exit` | — |
| net (`packages/yuga/std/net`) | TCP connect/read/write/close, listen/accept/peek, wasm `fetch_rpc` | — |
| Zeus | Host event loop + replay of the draw list; `plat_intern_fn` memcpy's a `loam_fn` env so click/styled handlers outlive the interned value | Tree, layout, signals, design system, `platform.loam` signatures |
| Maya | Host event loop + present (Cocoa blit / 2D discs) | Scene, orbits, camera, CPU tracer, 2D map layout, `sin`/`cos` |
| raygui | Host window + event pump (raylib) and a thin raygui trampoline (`runtime/raygui_plat.c`); no widget logic | API, frame loop, and all retained state (`packages/raygui/std/raygui.loam`) |
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
