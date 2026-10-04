# Loam — goal and phases

**Repo:** https://github.com/crestbala/loam  
**Updated:** 2026-10-05

## Goal

Build cross-platform GUI apps in one language: Loam.

- One source tree. Zeus paints. Hosts only give a window, input, clock, and blit.
- Same program on Linux, macOS, WASM, iOS shell, Android shell. Not UIKit, Material, or HTML widgets.
- Green: no GC, owned values drop, no string or closure leaks, idle instead of spin, 32-bit defaults.
- Secure in the realistic sense: no use-after-free, no data races, no overflow or index undefined behavior in Loam. One named C seam. Not “zero bugs forever.”
- Fast compile and a small project: keep the C11 compiler that emits C99 and calls `cc`. No LLVM, no 1GB build tree.
- Multi-threaded: workers exist. The UI thread owns Zeus. Spawn takes plain data only, and a channel brings the result back.
- One Loam file in, one C file out. `loamc` compiles and tests the whole
  `.loam` program; the source is never required to be split. This is also the
  road to **self-hosting**: the goal is for `loamc` itself to be written in
  Loam, building and testing itself from whole `.loam` files.

Not the goal: a kernel, bare metal, Flutter, Rust traits, a package registry, splitting the Loam source into many files.

## What is already done

- Odin-like syntax, ownership, no garbage collector.
- Compiler path: Loam → IR → C99 → `cc`.
- Zeus runs on macOS (Cocoa), Linux (X11, `hosts/desktop/linux.c`), WASM
  Canvas2D, iOS Simulator, and an Android canvas shell.
- Overflow and out-of-bounds trap. `Res<T>` must be checked (`#[must_check]`).
- `string` is owned and refcounted by default (`--string-owns`); the last
  reference frees the buffer. `--no-string-owns` restores the old leak for
  bisecting.
- Closures free their heap environment; Box / `[]T` drop at scope exit. A
  closure may **move** an owned value into its env (the source is consumed).
- Keyed list rows: `Each` / `For`, and `zui.VirtualListKeyed` for a windowed
  list (a scroll moves rows, not rebuilds them).
- `std:json` documents own their node pool; two live docs never alias.
- `loamc --cost` reports functions, allocs, drops, traps, and emitted C bytes.
- The Send discipline is a compiler pass (`threadcheck.c`), not a comment: a
  worker may not touch module globals, a `[]T` refcount, a `fn` value of
  unknown origin, a signal/UI node (`#[no_send]`), or any C-seam symbol outside
  the pure allowlist.
- `std:thread` exists on native. WASM spawn does nothing.
- C is only the binding layer, through bodyless functions and `loam_rt`.

## What is still missing

- Errors are `Res<T>` fields, not a real `enum Result` you cannot ignore.
  (Phase 4.)
- Wayland / musl are untested on Linux; there is no Linux CI yet.
- Elision of provably-safe bounds/overflow checks (Phase 5), and splitting a
  large program into several C files.
- Hosts do not yet sleep when there is no input and no timer (Phase 5).

---

## Phases

Each phase has one exit. Do not start the next until `make test` is green.

### Phase 1 — Linux is a real host

- [x] Compile CLI programs with the host `cc` on Linux (glibc and musl).
- [x] Zeus paints into one X11 window and blits
      (`packages/zeus/hosts/desktop/linux.c`; links libX11, no GTK).
- [x] `install.sh` recognizes Linux and gives distro-aware toolchain guidance
      (cc/make, clang+lld for wasm32, libx11-dev, node) instead of dying.
- [ ] CI builds Linux first; Wayland/musl are untested.

**Exit:** the same counter app builds and draws on Linux and macOS (build +
host wiring done; a Linux CI run is the remaining proof).

### Phase 2 — Drop what you own

- [x] `string` is owned and freed on last drop (`loam_str_retain` /
      `loam_str_release`, refcounted; literals are immortal).
- [x] Fix the compiler bug that corrupts a copied struct which holds a slice.
      A `[]T` whose element owns anything (a nested `[]T`, a string leaf, a
      `Box`) now routes through the buffer-scoped element hooks
      (`loam_vec_drop_owned` / `_push_owned`) for **every** owning element, not
      only string-owning ones. Before, a shared `[]Node`-of-`[]int` parameter
      drop zeroed the caller's element slots and the copy lost its nested
      slices. Regression: `tests/golden/slice_struct_copy.loam`,
      `tests/compile_pass/slice_struct_return.loam`.
      **JSON pool:** the workaround's *reason* is gone; moving the pool into
      `JsonDoc` is still open (it changes every accessor + the `#[json]` derive
      signatures), and `std/json` documents the one-live-document limitation it
      leaves until then.
- [x] Keyed list rows, so scrolling does not allocate for every visible row
      every frame. `zui.VirtualListKeyed(items, row_h, key, build)` moves the
      rows whose key stays in the window, builds only the row entering it, and
      reuses the end spacers. `Each` / `For` were already keyed.
      Regression: `tests/compile_pass/zeus_virt_keyed.loam`.
- [x] Closures may move owned values, not only copy them. A non-Copy capture
      (`Box`, or a struct/array/vec of one) is **moved** into the closure env:
      borrowck records the source as consumed (`clone`-free, like a `let c = b`
      move) and the codegen steals the value in, so the env owns it and a
      closure returned from a function keeps it live. References stay
      uncapturable. Regression: `tests/golden/closure_move_capture.loam`,
      `tests/compile_fail/closure_move_use_after.loam`,
      `tests/compile_fail/closure_capture_ref.loam`.
- [x] Remove the JSON global-pool workaround: `JsonDoc` owns its pool
      (`doc.nodes`), every accessor takes the doc/pool, and the `#[json]` derive
      generates `json_decode_X(nodes, id, path)`. Two live documents no longer
      alias. Regression: `json_codec.loam`'s two-document case.

**Exit:** a 500-frame scroll keeps a flat process footprint. **Green** for the
arena and nodes: `membench scroll` (a keyed windowed list, 500 rows, 500
one-row frames) holds live nodes flat (37 → 35) and allocates ~2.3× less than
the rebuild-all path. Strings no longer grow forever. Closures carry owned
payloads; workers still take Send (plain) data only, by design.

### Phase 3 — Threads that cannot touch the UI

Multi-threaded, without Rust’s trait system.

- [x] UI thread: Zeus, signals, paint, input, the async pump.
- [x] Worker: plain data in, plain data out, `channel` back.
- [x] `thread.spawn` accepts only plain data: ints, floats, bools, strings, and
      structs of those; `[]T` and `fn` values are rejected.
- [x] It rejects signals and UI nodes (`#[no_send]` on `Signal<T>` / `Node`),
      module globals, and anything that reaches the C seam (`threadcheck.c`).
- [x] WASM stays single-threaded until that whitelist is real. No fake threads.
- [x] `sys` handles: `std:sys` exposes no handle *type* — every handle is a
      bare `int`/`string` (watcher fd, path), so there is nothing to mark. The
      guarantee is the call-graph rule: every `loam_sys_*` call is off the
      worker allowlist. Completed by marking the one remaining plain-bytes
      UI-thread handle type `#[no_send]` (`Future<T>`), documenting the rule,
      and locking both with `tests/compile_fail/thread_call_sys.loam` and
      `tests/compile_fail/thread_capture_future.loam`.

**Exit:** a compile-fail test for “spawn a Zeus signal,” and a passing test that
a worker returns a computed value on a channel. **Green**
(`tests/compile_fail/thread_spawn_signal.loam` and
`tests/compile_fail/thread_chan_signal.loam`; `tests/compile_pass/thread_chan.loam`
returns a value on a channel).

### Phase 4 — Errors you cannot drop

- [x] Add `Option` and `Result` in `std:result` as `#[must_check]` tagged
      containers: `Result<T> { tag, val, err }` with the `ResultTag` enum, and
      `Option<T> { tag, val }` with `OptionTag`. Constructors `res_ok` /
      `res_err` / `some` / `none`, accessors `is_ok` / `is_err` / `is_some` /
      `unwrap` / `unwrap_or` / `unwrap_some` / `some_or`.
- [x] `match` is exhaustive, and now **enum-aware**: a `match` whose arms name
      every variant of an enum is accepted with no `_` (not just `bool`), so
      `match r.tag { ResultTag.Ok => …, ResultTag.Err => … }` reads as a real
      switch over the outcome. Regression:
      `tests/golden/enum_match_exhaustive.loam`,
      `tests/compile_fail/match_enum_incomplete.loam`.
- [x] `?` only for `Result` / `Option`. No exceptions, no unwinding: `expr?`
      lowers to an early `return` of the whole container on the bad tag, and the
      enclosing `fn` must return the same type. Regression:
      `tests/golden/result_question.loam`,
      `tests/compile_fail/question_non_result.loam`,
      `tests/compile_fail/question_wrong_return.loam`.
- [x] One error style. `Res<T>` (`{ ok, val, err }`) stays only at the HTTP edge;
      the RPC `Result<T>` in `std:http` was renamed `RpcResult<T>` so the
      language's `Result<T>` is the one name, not shadowed.

**Exit:** discarding a `Result` is a compile error — **green**
(`tests/compile_fail/result_discard.loam`; `#[must_check]` on `Result<T>` and
`Option<T>`), and existing tests pass.

### Phase 5 — Fast compile, small tree

Keep the compiler small. Measure before adding anything.

The **Loam file is the unit** the compiler always sees and tests. `loamc`
typechecks, lowers, emits, and runs the whole `.loam` program; it does not
require the source to be pre-split into many files first. That keeps `loamc` the
single source of truth (its tests, goldens, and `--cost` all read the `.loam`
directly) and keeps the path to a **self-hosting Loam** open: when the compiler
is written in Loam, it must still build and test itself from Loam sources, not
from a hand-split C layout. So splitting the *emitted C* for size is a
non-goal for now; if it ever lands it stays an opt-in detail of the C backend,
invisible to the language and to `loamc`'s test surface.

- [x] `loamc --cost` prints functions, blocks, IR insts, allocs (malloc), drops
      (frees), moves, bounds checks, overflow traps, calls, closures, string
      literals, C bytes, and the largest fn.
- [x] Elide bounds and overflow checks only when the compiler already proved
      them: `for i in 0..C.len { … C[i] … }` drops the index trap (proven in
      range, matched by resolution so a shadowed `i` cannot fool it), and a
      literal op literal folds, dropping the trap only when the exact result
      fits the operand type. Regression: the `cost` host check
      (`nob`/`ig_cost`).
- [x] "Split one Loam file into one C file" **reversed**: one Loam program stays
      one compiler input, always. The generated C may be large (~2.9 MB for a
      Zeus app), but the compiler never asks you to split the Loam source, and
      no LLVM is added. Splitting the emitted C, if it is ever wanted, is an
      opt-in C-backend option only — never a requirement on the source, so a
      future self-hosting compiler still builds from whole `.loam` files.
- [x] Android SDK, Xcode, and NDK stay on the machine, not in the repo.
- [x] Hosts sleep when there is no input and no timer. Cocoa already idles on
      `engine_next_ms`; the Linux X11 host now blocks in `poll` on the X fd
      (waking only for input or the next async/animation deadline) instead of
      spinning, and headless/`ZEUS_HEADLESS` paths already sleep.

**Exit:** a Zeus app still compiles in seconds on an M2, and `--cost` names the
biggest alloc. **Green** for `--cost` (it names the largest fn, the alloc/drop
counts, and the emitted C size) and for the elision (the `cost` host check
reports 0 bounds/overflow traps for proven-safe code). The compile path stays
"one Loam file in, one C file out, `cc` links it" — the shape a self-hosting
`loamc` will keep.

### Phase 6 — One program, five surfaces

The phone hosts stay shells. They are not apps.

- The counter, gallery, and www each carry **one shared `ui.loam`** (plus
  `screen.loam`) and four **thin host entries** — `macos` (native), `ios`,
  `android`, `frontend` (wasm). Every entry is the same six lines: a doc
  comment, `import "../ui.loam"`, and `fn main() { ui.start() }`.
- [x] Linux and macOS: **one** native stub. `--target native` picks the host by
  OS — Cocoa (`hosts/desktop/mac.m`) on macOS, X11 (`hosts/desktop/linux.c`) on
  Linux — so the same `macos/app.loam` builds both. No second copy.
- [x] WASM: the browser **Canvas2D canvas host** (`--target=wasm32`), named in
  `--help` as the canvas host and explicitly distinct from a future CLI/WASI
  target. Builds from the shared `ui.loam` (verified: counter 814 KB, gallery
  1.5 MB `.wasm`).
- [x] iOS: Simulator `.app` that only blits (verified building
  `counter/ios/app.loam --target=ios`). Device signing stays a host script.
- [x] Android: `native-lib.so` (JNI `hosts/android/android.c`) plus a dummy
  `ZeusActivity` (27 lines — it only creates the Zeus view) and a canvas
  blit/touch surface. No Android widgets.
- [x] Same `ui.loam` on all five; host files are **six lines**, not "a few
  dozen". Pinned by the `one surface` host check (`nob`/`ig_one_surface`), which
  fails if any host entry grows past a stub, stops importing `../ui.loam`, stops
  calling `ui.start()`, or starts painting with `zeus.*`.

**Exit:** one UI change shows up on Linux, macOS, WASM, iOS sim, and Android
without editing five copies. **Green**: the shared `ui.loam` + four six-line
stubs build for native (macOS/Linux), wasm Canvas2D, iOS sim, and Android, and
the `one surface` check forbids a forked UI.

---

## Do not change

- Zeus paints; the host does not draw widgets.
- Ownership without lifetime syntax.
- Bodyless `fn` as the only foreign call.
- 32-bit `int` and `float` by default.
- One async pump. No second runtime.
- No package registry, no traits, no LLVM, no kernel.
- One Loam file in, one C file out. The compiler never asks you to split the
  source; `loamc` stays the single tool that compiles, tests, and runs a whole
  `.loam` program. Keep it self-hostable from whole Loam sources.

## Done when

A Loam GUI program builds quickly, stays a small tree, runs on the five surfaces, drops its memory, and cannot send UI state to a worker — all from **one `.loam` file** that `loamc` compiles, tests, and runs as a unit. That is the goal. Everything else waits.

And after that: **Loam compiles Loam**. `loamc` is written in Loam, builds and tests itself from whole `.loam` files through the same one-file-in–one-C-file-out path, and no longer depends on a C-only bootstrap for the language's own compiler.
