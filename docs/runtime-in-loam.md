# The runtime and packages in Loam

Moved out of the self-hosting plan on
2026-11-12. **This is not part of the 100% self-hosting claim.** The
compiler and every tool are Loam already; the runtime they link (and every
program links) is the C this plan moves. It starts after self-hosting is
declared, and it is a security goal as much as a language one: see
[security.md](security.md).

**Primary force, set 2026-11-13: a runtime as minimal as Rust's.** This
drives the whole migration; every other goal here serves it or gives way to
it. Rust has no runtime to speak of: a few lines of start-up, the panic
path, and calls to the system allocator. Everything else (`core`, `alloc`,
`std`) is ordinary library code, compiled with the program, inlined and
monomorphized where it is used, and dropped by the linker where it is not.
Loam holds itself to the same:

- **Nothing a program does not call.** No pasted header, no fixed set of
  helpers in every binary. A program that never formats a float carries no
  float formatting; one that never spawns a process carries no spawn table.
- **Library, not runtime.** Logic lives in `std` as Loam, and goes through
  the same checks, DCE and inlining as user code. A `std` fn only the
  program calls is private to it (`static` in the generated C), so `cc`
  inlines it as freely as it did the header's `static inline` C.
- **What cannot be a library is the smallest core there is:** allocation
  over the system allocator, refcounts, the panic path and checked
  arithmetic, and those emitted by codegen only where used (R6).
- **No cost for being Loam.** Each moved piece is as fast as the C it
  replaces (gate 2), by giving the compiler the proofs and the code shape C
  had (stack buffers, checks proven away, inlining), never by keeping C.
- **Measured.** The trusted-C count (R0) and the bytes a minimal program
  carries both go down at every step, and neither is allowed back up.

**Target, set 2026-11-13: zero runtime.** `packages/loam/runtime/` is
deleted. A program carries only the Loam it calls, because Loam goes
through DCE where a pasted header cannot. The only C left is:

- the OS and the GUI toolkits themselves (libc, Cocoa, JNI), called through
  `#[c]` bindings with no shim file in between;
- vendored libraries, which stay as they are;
- the generated C that `cc` compiles.

Where safe Loam cannot express something, the next choice is the smallest
memory overhead, not more C: a table of typed handles instead of a raw
pointer, or a few bytes per value instead of a hidden C struct. The checklist
in §5 runs in order, and each step is ticked here, with its new line count,
in the same change that lands it.

**Goal**: every line of runtime C that is *logic* becomes Loam, so it is
checked by the borrow checker and bounds checks like any program. C is left
only where Loam cannot or should not go:

- the trusted core of the memory model;
- the calls into the OS and the GUI toolkits;
- third-party libraries that are vendored as they are.

Security follows from the same work. Every line of C is trusted code that
nothing checks ([security.md](security.md) gaps 2–3), so the measure of
progress is **lines of trusted C**, counted by one `nob` command and
recorded at each step, next to what a minimal program carries.

## 1. What is C today

Counted on 2026-11-11 (`wc -l`, comments included):

| Where | Lines | What it is |
|---|---|---|
| `runtime/loam_rt.h` | 2,695 | Pasted into every program. Panics and traps, checked integer ops, allocation, owned strings, vec growth and copy-on-write, element hooks, closure envs; then fmt/str/conv/buf verbs, `sys` (files, dirs, process spawn, stdin, watch), async futures, threads and channels. |
| `runtime/zeus_plat.c` | 2,958 | Zeus platform layer: the signal byte store, image sizes, media playback, recoverable traps, the frame, the trace host. |
| `runtime/net.c` + `net_rt.h` | 1,346 | Sockets, `poll`, non-blocking connect, the macOS TLS client (SecureTransport). HTTP itself is already Loam (`packages/http`). |
| `runtime/zeus_wasm_libc.c` | 796 | The slice of libc the wasm build needs. |
| `runtime/zeus_key.c` + `.h` | 360 | Key codes and text-input mapping. |
| `runtime/` maya, raygui, `batch_run.c`, headers | ~1,570 | 3D and raygui bridges, the batched-test runner, `zeus_rt.h`. |
| `zeus/hosts/*` | 4,871 | Native hosts: Cocoa (`mac.m`, `apple_draw.m`, `apple_media.m`), iOS, Android (JNI), X11, wasm imports. |
| Vendored | ~294,000 | `sqlite3.c` (285k), raygui (5.5k), monocypher + argon2 shim (3.3k). |

## 2. What stays C, and why

- **The trusted core.** Allocation, refcount retain and release (atomic for
  strings), vec buffers and copy-on-write, and the element and env drop
  hooks. These are the primitives safe Loam is built *from*. Checked integer
  ops and the panic path have left this header (R6.1, R6.2). R6.3 measured
  both ways the rest could leave and chose codegen emission, per type, not
  a `std:core` module. `unsafe` is the C ABI. It
  is not how these helpers move. R6.4 moved every primitive only generated
  code calls: vec push and pop, the Box and vec moves and drops, the vec
  and string retains, copy-on-write and reserve of owning elements, and the
  constant table. Codegen emits each one a program calls. What stays is what
  other runtime C still calls (`zeus_plat.c`, `zeus_rt.h`, the hosts):
  allocation (`loam_new`), the refcount word, `loam_vec_reserve` /
  `loam_vec_unique`, `loam_vec_new`, `loam_vec_drop` and `loam_str_release`.
  They leave when their last C caller does (R6.5, R6.6, R7). The file stays
  small, fuzzed
  ([security.md](security.md) S2) and sanitizer-tested.
- **OS and GUI calls.** `read`, `write`, `open`, `posix_spawn`, `poll`,
  sockets, `pthread`, the Win32 equivalents, Cocoa / UIKit (Objective-C), JNI,
  X11, and wasm imports. Each becomes a thin binding: one `#[c]` fn per call
  where the types fit (numbers, `string`, `[]u8`; self-hosting §1.7), and a
  few-line C shim where they do not (`char **argv`, structs, Objective-C
  messages). No logic lives in a shim.
- **Vendored libraries stay as they are.** Rewriting sqlite or monocypher in
  Loam is not essential and would trade audited code for new code. They
  stay behind `#[c]` modules, private to those modules since 2026-11-11
  ([security.md](security.md)), with fuzzed wrappers (S2).

## 3. What moves to Loam

Most of the code above is logic over bytes and ints, which Loam writes
safely today:

- **`loam_rt.h` verbs**: fmt, str, conv and buf (the integer halves are Loam
  already), path handling, directory walks, `sys.read` / `write` error
  handling, the spawn table, `read_line` buffering, watch bookkeeping, the
  future table and the channel queue logic. The kernel call under each one
  stays a binding; the lock and condition-variable calls stay in the core.
- **`zeus_plat.c`**: the signal byte store (a `[]u8` arena that keeps the
  typed cells of [security.md](security.md) S0), the frame and trace logic,
  trap boundaries and media state. Calls into the host stay bindings.
- **`net.c`**: connection state, buffering, timeouts and the non-blocking
  state machine. The socket and `poll` calls and the TLS library stay
  bindings.
- **`zeus_key.c`**: all of it (tables and mapping).
- **`batch_run.c`**: the batched-test runner, a plain Loam program.
- **`zeus_wasm_libc.c`**: it shrinks as the code that needed libc moves to
  Loam; what remains is core. Gone in R6.5: the rest is bulk-memory
  instructions and C that codegen writes into wasm units only.
- **Hosts**: layout, event routing and state move to Loam; the toolkit calls
  stay native. `mac.m` is the largest (2,207 lines) and goes last.

## 4. What the language needs first

- **S0 (typed handle cells, [security.md](security.md)), done.** Handles are
  how Loam code refers to runtime state, so they had to stop being forgeable
  before more runtime moves behind them.
- **Opaque handles from bindings.** A binding that returns a C pointer gives
  an `i64` today, and the wrapper module must never let it escape. Make
  that checkable with no new syntax: the wrapper keeps pointers in a
  module-private table and hands out typed S0 handles. Add a lint for a
  `#[c]` module whose public fn returns or takes an `i64` that came from a
  binding.
- **FFI listing ([security.md](security.md) S3)**, so the trusted-C count is
  one command.
- No inline C. A piece that cannot be written in safe Loam belongs in the
  core, and the core gets a line-count budget.
- **`unsafe`, for the C ABI.** Landed 2026-11-16. One word, like `async`,
  on a top-level fn or a block. The operations are `raw_addr`, `raw_read`,
  and `raw_write`: the address of a string or a
  number buffer, and a `memcpy` of one number, bool, or plain struct.
  Offset is ordinary `i64` arithmetic. No pointer type in safe code, no
  unchecked index, no inline C. An address-to-handle conversion was not
  added: the piece below does not need one.
  It landed by deleting `loam_net_string_bytes_at` (the address of a
  string's byte, which `std:net` now gets with `raw_addr`). **Runtime:
  5,006 → 4,992** (−14). `nob trusted` also counts the `unsafe` Loam that
  was already the FFI surface: `std:net` 612, `crypto` 45, `sqlite` 17,
  674 in total. Those lines were trusted before and uncounted. The runtime
  budget is the C, and it only shrinks.
  The rules the compiler checks:
  - **Minimal.** The three operations above, and only in a module that
    declares a `#[c]` binding. A region that does no raw op and calls no
    unsafe fn is an error. Regions do not nest.
  - **Contained.** An `unsafe fn` is private to its module, as a `#[c]` fn
    is, and it is not a value. The module exposes a safe wrapper. The
    compiler, `nob.loam`, and the tools stay safe Loam (self-hosting §2.7);
    `nob trusted` fails if they use `unsafe`.
  - **Visible.** `loamc check --ffi` lists every `#[c]` binding (and whether
    callers must be unsafe), every unsafe fn, and every unsafe block, with
    paths relative to the working directory. `nob trusted` counts the lines.
  - **Checked.** `compile_pass/unsafe_raw.loam` round-trips a byte, an
    `i64`, and a plain struct through C memory, and calls another module's
    safe wrapper. `compile_pass/unsafe_raw_name.loam` keeps a user fn named
    `raw_read`. `compile_fail/unsafe_*.loam` rejects an empty unsafe fn, a
    module with no binding, a region that does nothing unsafe, nesting, a
    raw op outside `unsafe`, a safe call of an unsafe fn, an unsafe fn used
    as a value, a cross-module call, and `raw_addr` of an int.
- **`#[c]` that handles C completely, so no package writes a C file or a
  header of its own** (set 2026-11-15). Rust has no C shims: `extern "C"`,
  `#[repr(C)]` and checked layouts reach any C library. Loam's bindings grow
  the same way, one feature per change, each landing with the C it deletes
  (the "Essential" rule above). The rules that stay:
  - **Bindings are trusted code.** A struct transcribed with the wrong
    layout, or a buffer shorter than C expects, corrupts memory exactly as
    the C did. Bindings stay module-private behind safe wrappers, count as
    trusted in `nob trusted` and `check --ffi`, and every transcribed layout
    is checked against the real headers by a `nob` stanza (`_Static_assert`
    of `sizeof` / `offsetof` in a probe compile; `c_layouts`, since R4.6),
    so no C ships and a wrong field still fails CI.
  - **Some C has no symbol to bind.** Macros (`S_ISDIR`, `WIFEXITED`,
    `mach_task_self()`), `static inline` header fns and C++ are rewritten in
    Loam with per-OS constants (as R2.3d did). A header-only library is
    vendored code, compiled as itself (`#![c_source("vendor/raygui.h",
    "-x", "c", "-DRAYGUI_IMPLEMENTATION")]`), not wrapped in a shim.

  The features, in order of the C each removes:

  | Feature | Deletes |
  |---|---|
  | a1. Plain-data structs by value as `#[c]` parameters (every field a number, a bool or such a struct), and `[]bool` buffers. **Done with R4.6.** | the raygui shim (`Rectangle`, `Color`, `bool *`) |
  | a2. Plain-data struct returns, and `&mut S` out-parameters | the `[]i64` slot layouts of `stat` (R2.2), `kevent` (R2.5), `task_info` / `getrusage`; `GetColor`-style returns |
  | b. `#[c]` on a module `let`: an extern C global | `loam_sys_spawnp` (exists only for `environ`), `stdout` |
  | c. `#[os]` / `#[arch]` on structs and consts | per-OS layouts (`stat`, `rusage`) written once per target, not as offsets |
  | d. Variadic bindings (numbers and strings only): `#[c("fcntl", "variadic after 2")]`. **Done with R5.1.** | wrappers over `open` / `fcntl` / `ioctl` and the arm64 Apple variadic ABI trap |
  | e. Env-less fn pointers: a `#[c]` parameter of a fn type of numbers and bools takes a named module-level fn, passed as its own C symbol. **Done with R5.2.** | callback shims (`qsort`-style, run loops, library callbacks) |
  | e2. Borrowed buffers: a `#[c]` parameter may be `&[]T` or `&mut []T` of numbers and bools. The wrapper passes the element pointer and does not retain or drop. **Done** (after R5.2). | no runtime C. The cost was a retain and a drop on every binding call that passed a module buffer |
  | e3. Binding libraries: `#[c("SSL_new", "link -lssl -lcrypto")]` (or `-framework NAME`), linked only when a kept binding that builds here names them. **Done with R5.3.** | the always-on Security and CoreFoundation link of every `std:net` program; a module-wide `#![c_link]` would have made every TCP program on Linux link libssl |
  | f. Typed `objc_msgSend` bindings and selectors (`std:objc`) | the Cocoa and UIKit hosts (`mac.m`, `ios.m`, maya's `hosts/mac.m`; R7) |

## 5. Order and gates

Each step moves one area, deletes its C, and must pass all of these before
it lands:

1. **Same behaviour.** The full suite and `nob selfhost` pass; the
   draw goldens are byte-identical; `win-selfhost` and the Linux jobs stay
   green.
2. **Same speed or better**, area by area, as with the compiler passes: the
   area's hot path is benchmarked against its C twin, and a Zeus frame and the
   compiler building itself do not regress. Binary size is checked too. Loam
   code goes through DCE, so unused runtime should drop out, where the pasted
   header cannot.
3. **Less trusted C, less carried.** The trusted-C count goes down, a
   minimal program's binary does not grow, and the step records both
   numbers here.

Order, smallest risk and most logic first. Each box is one change, ticked
here with the date and the new count when it lands.

Starting count, 2026-11-13 (`wc -l`): `packages/loam/runtime/` is 9,812
lines in 14 files (`loam_rt.h` 2,762, `zeus_plat.c` 2,973, `net.c` 1,295,
`zeus_wasm_libc.c` 796, `zeus_rt.h` 598, `maya_mac.m` 345, `zeus_key.c` 307,
`raygui_plat.c` 282, `batch_run.c` 159, the rest headers), plus 118 lines of
`wasm_inc` headers. Hosts are 4,871; vendored code is 294,490.

**R0. Measure.**

- [x] R0.1 `nob trusted`: lines of trusted C per file, grouped as runtime,
      hosts and vendored, with a total per group. Done 2026-11-13. It counts
      tracked files only (`git ls-files`); the seed and test fixtures are
      not counted. First count: runtime 9,939 (the 14 files plus
      `wasm_inc`), hosts 4,871, vendored 346,122 (sqlite, monocypher, raygui
      and the generated tree-sitter parser).
- [x] R0.2 The budget: `RUNTIME_LINE_BUDGET` in `nob.loam`, set at 9,939
      and lowered by each step since (now **4,296**, after R6.5).
      `nob trusted` and the unfiltered `nob test` fail if `runtime/` grows
      past it. Each step below lowers it to its new count in the same
      change.

**R1. Pure logic, no new language feature.**

- [x] R1.1 `zeus_key.c` + `zeus_key.h` → Loam (`zeuscore/keymap.loam`,
      463 lines): interned names, chord parsing, the keymap, handlers and
      dispatch. Done 2026-11-13. `engine_key_ev` is Loam end to end, and
      `zeus_handle_key_ev` (what the hosts call) is a one-line trampoline
      to it, bound in the host-mode app table. The five C-only focus
      trampolines (`key_apply`, `fill_focus`, `focus_depth` / `node` /
      `ctx`, `focus_step`) left the DCE roots and the app table; the
      handler table reuses its slots, so a view rebuild allocates nothing,
      as before. `mac.m` keeps the five key and modifier codes it uses.
      Not benchmarked: the keymap runs once per key press, never per
      frame. **Runtime: 9,939 → 9,456** (−483: the two files, plus the
      trampolines in `zeus_plat.c` and `zeus_rt.h`).
- [x] R1.2 `batch_run.c` → `std:batch` (206 lines). Done 2026-11-13. The
      fork-per-case runner is a std module whose `serve` compiles to
      `loam_batch_serve`, the name the generated batch `main` already
      calls; nob's suite program imports it and DCE keeps it as a C root.
      Its kernel calls are `#[c]` bindings straight to libc, with no shim
      (`fork`, `wait`, `dup2`, `close`, `setenv`, `unsetenv`, `alarm`,
      `_exit`, `fflush`); the variadic `open` is only ever called with its
      two fixed arguments, and the output file uses `creat`. It runs before
      any module initializes, so it reads no module-level `let`. This also
      shows R2.1 is mostly there: a std module can bind libc with no
      `#![c_source]`. Speed: the suite's serve phase is 8–9 s against the
      C runner's 8 s, and the full suite still passes (617).
      **Runtime: 9,456 → 9,297** (−159).
- [x] R1.3a Integers to decimal text (`std:decimal`, 107 lines). Done
      2026-11-13. `fill` writes a value's digits least significant first
      into a 20-byte array on the stack (every index proven in range by the
      new `ranges` pass, so the loop keeps no check), then one of three
      body-less `loam_rt.h` hooks hands the bytes on: `text_of_digits` (a
      string), `push_digits` (the end of a `[]u8`) or `write_digits` (stdout
      or stderr, so `println(n)` allocates nothing). `println` / `eprintln`
      of an `int` lower to `decimal.write_int` / `write_u64` / `ewrite_int` /
      `ewrite_u64` (plus `BY_CNAME` DCE and threadcheck roots),
      `buf.push_int` / `push_i64` / `push_u64` and `fmt.write_int` call it,
      and the C `str_of_int` / `u64`, `fmt_write_int` / `u64`,
      `ewrite_int` / `u64`, `buf_push_*` and `fmt_utoa` are deleted
      (`loam_fmt_itoa` stays for the `loam_key_*` family, a later step).
      `string_from_bytes` adopts an unshared `[]u8` with its capacity, no
      copy (borrowck consumes a local argument; the IR marks it `I_MOVED`).
      Whole-unit builds emit kept non-root fns of `LOAM_STD_DIR` modules
      `static`, so `cc` inlines them like the old `static inline` C;
      package `src` modules keep external linkage for `zeus_plat` and hosts.
      `&arr` on a fixed array decays to an element pointer. The new `ranges`
      pass (464 lines) also proves unsigned `x % k` in `[0, k - 1]`,
      unsigned `x / k` with `k > 0` never traps, borrowed `&[N]T` indices,
      and `C[C.len - 1 - i]` inside `for i in 0..C.len`. Safety fix: `.len`
      is an `i32`, so `loam_str_owned` and `loam_vec_grown_cap` now panic
      past `INT32_MAX` (`LOAM_LEN_MAX`), and capacity clamps there.
      `loam_panic` / `loam_panic_str` are `LOAM_COLD` (noinline, noreturn,
      unused; no `cold`, which grew binaries). Speed against HEAD's C
      (median M cycles): interpolation 182 vs 178 (+2%), `buf.push_int` 80
      vs 78.5 (+2%; the per-digit loop bound check; pair tables, two digits
      per step and width-bounded loops were all slower), `println` at
      parity, self-emit 153 ms vs 154 ms. Fixtures: `golden/decimal_edges`
      (expected generated by the old C, so also a differential check) plus
      `ranges_*`, `boundscheck_reverse_*` and `string_from_bytes_*`.
      Binaries (new vs HEAD): nob 599,184 vs 599,528 (−344), loamc
      1,860,704 vs 1,927,632 (−66,928), greeninfer 1,044,688 vs 1,200,144
      (−155,456), tiny int-printing program 34,272 vs 34,176 (+96: one
      unwind entry for the out-of-line `loam_panic`). Full suite 634
      passed; `selfhost_seed`, `ownership_asan`, `wasm_smoke`,
      `android_project`, `cost` and `lsp` all green. `nob trusted`: 9,270.
      **Runtime: 9,297 → 9,270** (−27).
- [x] R1.3b Content keys for `Each` (`std:key`, 90 lines). Done
      2026-11-14. `__value_key(x)` lowers to one `[]u8` that `std:key`
      appends to (`push_vec_tag`, `push_str`, `push_f32` / `push_f64`,
      `push_addr`), then `string_from_u8` adopts it: no piece strings and
      no concatenation, where the C built every piece and re-joined them
      (O(n²) in pieces). Constant pieces (struct and array tags `R2{` /
      `A3[`, integer prefixes `i32s:`, `b1`, `}`, `~`) are literals from
      codegen; integers then go through `decimal.push_int` / `push_u64`.
      A string alone is its own key (a retain, no allocation): the rows of
      one `Each` share a type, so it needs no tag; inside a compound it is
      still length-prefixed. Every other key is byte-identical to the C.
      Hex digits come from a string literal indexed by `x % 16`, so the
      `ranges` pass proves every step and the fns keep no check. The two
      allowed hooks are `conv.f32_bits` / `f64_bits` (a `memcpy` bit-cast).
      `loam_key_*`, `loam_fmt_itoa` and `loam_u64_dec` are deleted. The
      loader's implicit `std:decimal` / `std:key` imports carry
      `F_IMPLICIT` and `module_imported` skips them, so they are no name in
      the program: a local `key` (as in `loamc.loam`) stays a local, and a
      user module named `key` (mune) still resolves. Bugs found in the
      staged version and fixed: `fbits` hexed one digit per byte and in
      the wrong byte order, `u64` max panicked in a checked `as u64`, and
      the borrowed `cat` leaked both inputs. Bootstrapped from HEAD's seed
      (stage 0 builds the new compiler, which rebuilds itself); `nob seed`
      unchanged. Speed against HEAD's C (median M cycles, 300k keys):
      string keys 4.5 vs 22.9 (−80%), struct/vec keys 268.5 vs 462.6
      (−42%); greeninfer headless 31.9 vs 32.5; self-emit 165.6 ms vs
      164.2 ms (+0.9%, closed by R1.3c). Binaries (new vs HEAD): greeninfer 1,044,656
      vs 1,044,688 (−32; `__text` −352, no key code left), nob, loamc and
      the tiny int program unchanged. Fixture: `golden/valuekey_edges`
      (expected from the old C, but for the five lone-string lines).
      **Runtime: 9,270 → 9,127** (−143).
- [x] R1.3c Interpolation in one exact allocation. Done 2026-11-14. Two or
      more parts are one `loam_interp` call over the raw parts (no
      `decimal.of_int` temporaries), which codegen expands in place: an
      integer's digits go onto the stack through `decimal.fill`, the
      lengths are summed, then one `loam_str_bytes` and a `memcpy` per
      part, as the C key did; a float is a temporary `str_of_float`, a
      bool `true` / `false`. One part keeps its old lowering. No new C.
      `std:key` (and with it `conv` and `buf`) is now loaded only when a
      module parses `__value_key` (the parser notes it); R1.3b had loaded
      it into every program, which cost a tiny program 13% compile time
      and exposed `conv.parse_int` returning `Result<i32>` under
      `--int64-compat` (fixed: `int(r.val)`). Module ranks stay a
      permutation with the entry last. Speed against HEAD (median M
      cycles): `"S{{s.len}}:{{s}}"` 22.3 vs 72.7 (−69%, 132 vs 434 M
      instructions; the C key was 22.7), one-part `"{{n}}"` 182.8 vs
      182.0 (same lowering), string keys 4.5 vs 22.7, struct/vec keys
      248.1 vs 453.9 (−45%); self-emit 151.8 ms vs 168.4 ms (−10%); a
      tiny program compiles in 8.7 vs 9.0 ms. Binaries (new vs HEAD): nob
      533,224 vs 599,192 (−65,968), loamc 1,795,608 vs 1,861,416
      (−65,808), greeninfer 1,044,480 vs 1,044,688 (−208), tiny int
      program unchanged. The seed's C grows (4,799,032 bytes: each site is
      expanded in place) while every binary shrinks. Full suite 635
      passed; `selfhost_seed`, `ownership_asan`, `wasm_smoke`,
      `android_project` and `lsp` green.
- [x] R1.3d The `str` verbs. Done 2026-11-14. Every interpolation is
      one `loam_interp` (R1.3c) unless it is a lone string (itself) or a
      lone integer (`decimal.of_int`); codegen writes a lone bool as the
      static literal `true` / `false` (nothing allocated, as before) and a
      lone float as `str_of_float` (R1.4 moves float text). So
      `loam_str_concat` and `loam_str_of_string` had no caller left and
      `loam_str_of_bool` none after this; all three are deleted, with the
      checker's concat fold. `loam_str_slice` stays as core: three lines
      clamping the bounds over `loam_str_copy`, the allocation primitive;
      a Loam version would need a new copy hook (no gain) or a byte-by-byte
      `[]u8` (slower, over-allocated). Output identical to HEAD for lone and
      mixed bool / float / string / int parts, no leaks. Binaries
      unchanged against R1.3c but loamc −64 bytes; full suite 635 passed,
      `selfhost_seed`, `ownership_asan`, `wasm_smoke`, `android_project`,
      `lsp` green. **Runtime: 9,127 → 9,091** (−36).
- [x] R1.3e The `fmt` verbs. Done 2026-11-14. `write_bool`, `writeln`
      and their stderr twins `ewrite_bool` / `ewriteln` are Loam over
      `write` / `ewrite`; `println` lowering still calls them by C name, so
      DCE keeps them with `M_PRINTLN` (as the `decimal` writers). Two-stage
      bootstrap: the old compiler, with HEAD's std and runtime as its home,
      built stage 1, which built the compiler here. Output identical to HEAD
      on stdout and stderr; `println` 6,665.5 vs 6,672.8 M cycles (same
      instructions); binaries unchanged. Full suite 635 passed, all host
      stanzas green. **Runtime: 9,091 → 9,079** (−12).
- [x] R1.3 `loam_rt.h` fmt / str / conv / buf verbs → `std` Loam fns, one
      verb family per change (R1.3a–e). Done 2026-11-14. What is left of
      these families is either float text (R1.4: `ftoa`, `write_float`,
      `str_of_float`, `f64_to_*`, `strtod`) or core byte movement and
      comparison, each a bounds clamp or length test over `memcpy` /
      `memcmp` / `write(2)`: `str_slice`, `buf.push_str` / `push_bytes` /
      `to_string`, the three `decimal` digit hooks, `fmt.write` / `ewrite`
      and `fmt.eq` (string `==`). A Loam body for those would copy byte by
      byte; they move only if a checked byte-copy intrinsic lands, so the
      compiler can lower a Loam loop to `memcpy`.
- [x] R1.4 Float text and parsing (`std:floattext`, 961 lines). Done
      2026-11-14. `text(v, p)` is C's `%.<p>g` (p in 1..17, half to even on
      the exact value), `shortest` the fewest of 15 / 16 / 17 digits that
      read back, `parse` C's `strtod`; NaN prints `nan` (as macOS, not
      glibc's `-nan`). Both directions take one fast path with a proof: 10^q
      is a coarse 128-bit power (10^27j, 26 entries) times an exact 64-bit
      one (10^r = 5^r·2^r, 27 entries), every step truncates, so the exact
      product is in `[R, R + 4)` units of its last bit; only when the bits
      under the last digit (or the 53-bit mantissa) are within that of a
      half does it take the exact base-1e9 bignum, ported from the wasm
      libc. Parsing first tries Clinger (≤ 2^53, |e| ≤ 22), then a 64-bit
      power (one product, about 1 in 100 undecided), then the 128-bit one;
      subnormals round in the fast path too. `println` / interpolation of a
      float lower to `floattext.write` / `ewrite` / `of_float`;
      `conv.f64_to_str` / `f64_to_g17` / `f64_to_g6` / `strtod` /
      `f64_bits` are Loam over it, and `fmt.write_float` (no caller) is gone.
      The module loads only when a loaded module has a float literal or the
      word `float` / `f32` / `f64` (the parser notes it, as `__value_key`),
      so a float-free program neither parses nor checks it (importing ~600
      lines costs a tiny program +30% compile time). New in the compiler: a
      fixed-array literal of plain data in a module-level binding is static
      data (`ir.static_data`), not init code, and an immutable binding of
      plain data is no module state to threadcheck, so workers may format
      floats and the tables are right before module init. Four hooks
      (`bits_of`, `of_bits`, `text_of`, `write_text`: two bit casts, two
      buffer hand-offs) replace `loam_fmt_ftoa`, `write_float`,
      `ewrite_float`, `str_of_float`, `loam_conv_f64_to_*`, `strtod` and
      `f64_bits`; the wasm-float block (324 lines), the `%g` branch of the
      wasm `snprintf` and its `strtod` declaration are deleted. Differential
      test: `float_diff.c` now writes the host libc's answers and
      `float_check.loam` checks them (420,026 checks in the `wasm_float`
      stanza, 6.3M by hand, 0 mismatches); `golden/floattext_edges`
      (expected from HEAD's C, byte-identical). Speed against macOS libc
      (median ns per call): `%.15g` 65 vs 254 (typical values) and 72 vs
      629 (random bits), `%.17g` 73 vs 284 / 73 vs 687, shortest 111 vs 418
      / 168 vs 1,576; parse of shortest text 17 vs 24, but of 17-digit
      `%.17g` text with large exponents 45 vs 31 (1.4x slower: the digit
      loop is one dependent multiply per digit, and 8-byte loads are not in
      the language; a native 128-bit multiply would save only 7 ns, not
      worth a builtin). Self-emit 153 ms vs 149 ms, with 870 more lines of
      std to emit. Binaries (new vs HEAD): tiny int program unchanged
      (33,688), nob 533,216 vs 533,224, loamc 1,813,160 vs 1,795,544
      (+17,616) and a float-printing program 51,336 vs 34,360 (+16,976):
      a program that formats floats now carries its formatter instead of
      calling libc's. Fixtures: golden 135, zeus 445, greeninfer passed;
      `selfhost_seed`, `wasm_float`, `wasm_smoke`, `wasm_trap`,
      `ownership_asan`, `android_project`, `cost`, `lsp`, `fresh_cache`
      green. **Runtime: 9,079 → 8,700** (−379).

**R2. `sys` over direct bindings.** Every kernel call is a `#[c]` binding
straight to libc, so no C shim.

- [x] R2.1a Per-OS fns in Loam (`#[os]`). Done 2026-11-14. One name may have
      several fn variants in a module when their masks are disjoint and their
      signatures equal; calls resolve to the first, all share one C name, and
      codegen wraps each variant's prototype, body and `#[c]` wrapper in `#if`
      for its targets (`apple`, `linux`, `windows`, `wasm`, `posix` = apple |
      linux | other Unix; `posix` emits `!defined(_WIN32) &&
      !defined(__wasm32__)`). Attributes stack (`#[os("posix")]
      #[c("getpid")]`); a bodyless runtime hook must be every variant or none.
      DCE keeps all variants; only the as-value trampoline comes from the
      primary. `client_c_ok` skips non-wasm variants on wasm (and std `#[c]`
      on iOS/Android); the committed seed still builds unchanged on Linux and
      Windows. Tree-sitter `repeat($.attribute)` regenerated via `nob grammar`
      (src/parser.c, grammar.json, zed extension.toml). Fixtures:
      `compile_pass/os_variants` (prints `posix posix true true`),
      `compile_fail/os_overlap`, `os_signature`, `os_unknown`,
      `os_plain_twice`; `loam-fmt` leaves them unchanged. Suite: `os_`,
      `compile_fail` pass; `selfhost_seed` (seed = stage 1 = stage 2,
      unchanged), `wasm_smoke`, `wasm_float`, `lsp`, `ownership_asan`,
      `android_project`, `cost`, `fresh_cache` green. **Runtime: 8,700**
      (unchanged: compiler-only, no runtime moved).
- [x] R2.1 `#[c]` usable from `std` itself. Done 2026-11-14. No compiler
      change was needed: raw libc symbols already link with no `#![c_source]`
      (proven by `compile_pass/c_ffi`'s `memcpy` / `floor` / `strlen` and by
      `std:batch`'s nine bindings since R1.2); the blockers were convention,
      now written down (bindings are module-private, so each sits
      beside a safe wrapper; per-OS shape is `#[os]` variants; Windows keeps
      a hook behind a wrapper until R2.6). First production binding:
      `sys.stdout_tty` is Loam over `#[os]` variants — posix `#[c("isatty")]`,
      a wasm stub returning `0` (today's answer), a Windows wrapper over the
      one-line `loam_sys_stdout_tty_win` hook macro — and `loam_rt.h` loses
      `loam_sys_stdout_tty` plus its wasm stub. No compiler change: the R2.1a
      machinery (`#[os]`+`#[c]` stacking, `#if`-guarded wrappers, `client_c_ok`
      skipping non-wasm variants) already covered it; a probe combining all
      three confirmed native and `--target wasm32` builds before touching std.
      Fixture: `compile_pass/sys_stdout_tty` (prints `tty: true`). `stdout_tty` runs once per process, so no hot-path
      benchmark (same single `isatty` syscall either way). Binaries (new vs
      HEAD): tiny int program 16,936 unchanged, nob 533,216 unchanged, loamc
      1,814,408 unchanged (no compiler change). **Runtime: 8,700 → 8,699** (−1: two C lines deleted, one hook
      macro added).
- [x] R2.2 Files and dirs. Done 2026-11-14. `read_file` / `write_file` /
      `write_bytes` / `io_error` are Loam over stdio `#[c]` bindings
      (`FILE*`-as-`i64`; `fopen` / `fread` / `fwrite` / `ferror` / `fclose`
      raw from libc) plus a `strerror(errno)` shim and a module-level error
      slot; `stat` is a five-slot shim (`loam_sys_stat`: sec, nsec, size,
      is-regular, is-dir — the layout and the `S_IS*` macros cannot cross)
      behind one `stat` per call for `mtime` / `stamp` / `exists` / `is_dir`;
      dirents are read by `loam_dir_names` + `loam_dir_name_at` shims with
      the insertion sort in Loam over a raw `strcmp` binding; `remove_path` is
      the `lstat`-and-recurse walk (symlinks never followed) over raw
      `remove` / `rmdir`; `mkdir -p` is a Loam walk over a `mkdir` shim that
      hands back `errno` (`EEXIST` is 17 on all three targets); `rename` is
      raw with the Windows remove-then-retry in Loam; `realpath` is raw
      `realpath(3)` into a 4096-byte Loam buffer on posix (libc allocates
      nothing) and a same-shape `_fullpath` hook on Windows; `cwd` is
      `getcwd` into a buffer trimmed at the first NUL. New primitive:
      `buf.with_len(n)` (one zeroed allocation with a spare byte, so
      `string_from_bytes` takes it without a copy). The shims are defined
      once in the owning unit (`LOAM_RT_DEFINE_JMP`), like the trap state.
      `write_bytes` now takes `b: []u8` by value (one refcount bump, no byte
      copy; four compiler call sites and one fixture dropped their `&`). All
      wasm variants answer from Loam; the wasm stub block loses the moved
      fns. Zeus's `plat_font_bytes` no longer falls back to the C reader:
      with no host hook it answers `""` and `zeus.use_font` reads the path
      with `sys.read_file`.
      **Bug found on the way:** a `#[c]` fn returning `string` is called as
      returning `const char *` (the wrapper copies it). The first draft's
      `loam_sys_strerror` returned a `loam_str`, which arm64 hands back
      through a struct-return pointer (x8) that the wrapper never set, so
      every failed `fopen` / `stat` wrote 24 bytes through a stray pointer
      and the fresh loamc aborted with a damaged free list on any file. The
      draft also bound `realpath` with one argument (the second was register
      garbage, and the `malloc`'d answer leaked) and gave the Windows hook a
      `loam_str` parameter where the wrapper passes `char *`. All three now
      use the `#[c]` ABI (`const char *` in and out, caller buffers).
      Behaviour matches the C exactly, checked against HEAD's source: `"empty
      path"` / `"path too long"` (≥ 4095) / `"not a regular file"` / `"file
      too large (over 1 GiB)"` / `"read error"` (only with `ferror`; a short
      read keeps its bytes), `"no filesystem"` on wasm, rescan-per-call
      listings, overlong children skipped. A probe of every moved fn
      (including 2,000 rounds of failed reads, `realpath`, and `list_dir`) prints
      what the C printed and has 0 leaks. The wasm `abort` is declared `noreturn` now
      (it ends in `__builtin_trap`), so the `-Winvalid-noreturn` warnings on
      `loam_panic` are gone from wasm builds. Benchmark (same compiler,
      `LOAM_HOME` switched to HEAD's std and runtime; 3 alternating rounds,
      steady state): 50 MB `read_file` 11–12 ms Loam vs 7 ms C (~1.6×: the
      gap is `with_len` zeroing 50 MB, kept so a buffer never exposes
      uninitialized bytes; `calloc` was tried and only helps a read on fresh
      pages, so it was not kept); `mtime` ×200k 384–390 vs 368–373 ms (+4%:
      one `[]i64` and `Result` per call); `realpath` + `cwd` ×20k 480–489 vs
      468–501 ms (+2%); `list_dir` ×2000 69 vs 1,172–1,186 ms (17× faster:
      the C version rescanned the directory once per entry). Compiler `--emit-c`
      of itself 0.15 vs 0.15 s. Binaries (new vs HEAD): tiny int program
      33,688 vs 33,688, nob 533,912 vs 533,224 (+688), loamc 1,831,608 vs
      1,814,408 (+17,200, +0.9%: the moved fns are out-of-line Loam plus
      their `Result` monomorphs, not `static inline` C). Seed 4,977,434 →
      5,035,141 bytes, seed = stage 1 = stage 2. Fixtures `fs`, `sys`,
      `sys_stamp`, `std_buf_conv`, `c_ffi`, `os_`, `buf`, `compile_fail`,
      `golden_font_metrics` and all 59 `draw_golden` pass; `selfhost_seed`,
      `wasm_smoke`, `wasm_float`, `wasm_trap`, `lsp`, `ownership_asan`,
      `android_project`, `cost`, `fresh_cache` green, each run alone.
      **Runtime: 8,699 → 8,540** (−159).
- [x] R2.3 Process: `exec`, `spawn` / `wait` and the spawn table, `env`,
      `exit`, `os`, `arch`, `cpu_count`. Done 2026-11-14 (8,540 → 8,286
      lines). In steps: R2.3a target facts in the
      compiler, R2.3b `os` / `arch` / `cpu_count` / `target`, R2.3c `env` /
      `env_set` / `exit`, R2.3d `exec`, R2.3e `spawn` / `wait`.
- [x] R2.3a `#[os]` tells macOS, iOS and Android apart, and `#[arch]` picks
      by CPU. Done 2026-11-14. `sys.os()` answers `ios` / `android` and
      `cpu_count` is `1` there, and `arch()` needs the CPU, none of which a
      variant could say. This is Rust's `cfg(target_os)` / `cfg(target_arch)`
      shape. `#[os]` gains `macos`, `ios`, `android` (`apple` = macOS + iOS,
      `linux` = Linux + Android, `posix` = both plus other Unix);
      `#[arch("x86_64" | "aarch64" | "wasm32" | "other")]` is new, and a
      variant may carry both. OS bits sit low and arch bits high in the one
      `numb` mask; the constants moved to `ast.loam`, so codegen and loamc
      lose their bare `19` / `8`. Overlap is pairwise (a shared OS *and* CPU),
      since an OS-only and an arch-only variant cover different slices.
      Codegen tests the plain `__APPLE__` / `__linux__` macro when a mask
      names both halves, so every existing variant emits byte-identical C
      (the compiler's own `#if` set matches the seed's exactly); split halves
      add `LOAM_IOS` / `__ANDROID__` / `LOAM_ANDROID` tests. Fixtures:
      `compile_pass/os_arch_variants` (checked against `sys.os()` /
      `sys.arch()`, so it holds on every host), `compile_fail/arch_overlap`,
      `arch_unknown`, `os_apple_overlap`. Compile speed: the compiler emitting
      itself, 40 alternating runs, min 157.9 vs 157.1 ms and median 159.3 vs
      158.5 ms against HEAD (+0.5%, noise; it also compiles its own new code).
      loamc 1,832,456 vs 1,831,608 (+848), seed 5,035,141 → 5,053,218 bytes,
      seed = stage 1 = stage 2. Suite: `os_`, `arch_`, `compile_fail` (164),
      `c_ffi`, `grammar-check`; `selfhost_seed`, `wasm_smoke`, `wasm_float`,
      `wasm_trap`, `lsp`, `ownership_asan`, `android_project`, `cost`,
      `fresh_cache` green. **Runtime: 8,540** (unchanged: compiler only).
- [x] R2.3b `os` / `arch` / `cpu_count` / `target` in Loam. Done 2026-11-14.
      One `#[os]` / `#[arch]` variant per target returning its literal;
      `cpu_count` is raw `sysconf` with the per-OS `_SC_NPROCESSORS_ONLN`
      (58 in the macOS SDK, 84 in glibc and musl, read from the headers via
      zig), Windows parses `NUMBER_OF_PROCESSORS` as `strtol` did, and wasm /
      iOS / Android answer `1`. An unknown Unix (`other`) now answers `1`
      too: its constant differs per BSD and no BSD is a supported target. R2.3a's names were corrected here before anything
      used them: as in Rust, `linux` is Linux only, `android` separate, and
      `other` (any other Unix) is nameable; naming both `linux` and
      `android` still emits plain `__linux__`. Two-stage bootstrap (compiler
      with HEAD's std, seed, then the new std). Preprocessing the emitted C
      with zig for x86_64 Linux, Android, x86_64 Windows, iOS and wasm keeps
      exactly one variant of each fn with the right value. Pre-existing bug
      fixed (from R2.1): Windows `sys.stdout_tty` bound a `#define` macro as
      a link symbol, so four fixtures failed to link in `cross-check.sh`;
      Windows now binds `_isatty` raw, and `stdout_tty` normalizes the CRT's
      nonzero answer to the documented `1`. Benchmark vs HEAD's C: the
      fact checks constant-fold away in both (0 ms for 50M), `cpu_count`
      ×200k 100–109 ms both. Suite: `os_`, `arch_`, `sys`, `compile_fail`,
      `c_ffi`, `http`; the 8 stanzas plus `wasm_trap`; `cross-check.sh` Linux
      x86_64 / aarch64 122/122 and Windows x86_64 / aarch64 110/110 (was 4
      failed). Seed 5,053,218 → 5,062,375 bytes, seed = stage 1 = stage 2. **Runtime: 8,540 → 8,479** (−61).
- [x] R2.3c `env` / `env_set` / `exit` in Loam. Done 2026-11-14. `env` is
      raw `getenv` (the wrapper copies its answer; NULL is `""`), the
      256-byte name rule kept in Loam; `env_set` takes `getenv` as an address
      and asks `memchr(p, 0, 1)` whether the first byte is the NUL, so it
      never copies the value; `exit` is raw `exit`, and wasm keeps one
      `abort` hook (`loam_sys_wasm_exit`: wasm builds may reach no `#[c]`).
      **Codegen win for every `#[c]` binding:** a `string` argument is now
      NUL-terminated in a 256-byte stack buffer in the wrapper, with the heap
      only for longer strings, behind an out-of-line cold helper. It used to
      be a `malloc` + `free` per argument. With the hot path that small, the
      chain inlines, and a literal's copy folds into a store as the old C's
      did. This also speeds up R2.2's `fopen` / `stat` / `mkdir` bindings.
      R2.2 debt paid on the way: `c_stat` / `c_lstat` built a `strerror`
      message on every miss that no caller read; misses no longer build it.
      Behaviour matches HEAD (set, empty, unset, empty name, 256-byte name;
      `exit` from 50 frames deep gives the same code). `compile_fail/
      module_builtin_fn_value` now uses `sys.argc`, since `sys.exit` is no
      longer a bodyless hook (a Loam fn may be a value). Benchmark vs HEAD's
      C (3 rounds): `env` ×2M 118–130 vs 119–120 ms (parity); `env_set` unset
      ×4M 333–347 vs 333–336 (parity); `env_set` set ×4M 115–117 vs 108–111
      (+1.5 ns per call: one `memchr` call where C loaded the byte inline,
      the one residual this step leaves); R2.2's `mtime` ×400k 754–762 vs
      729–747 (+2–3%, was +4%) and `exists` miss 581–590 vs 555–569 (+4%, was
      +9%). The rest is the per-call `[]i64` slot buffer, R2.2 speed work.
      Compiler emitting itself, same input, 40 alternating runs: median
      158.8 vs 158.6 ms. Seed 5,062,375 → 5,071,275 bytes, seed = stage 1 =
      stage 2. Suite: `sys`, `fs`, `sys_stamp`, `std_buf_conv`, `c_ffi`,
      `os_`, `exit`, `compile_fail` (164), `http`; the 8 stanzas plus
      `wasm_trap`; `cross-check.sh` Linux 122/122 and Windows 110/110 on both
      CPUs. **Runtime: 8,479 → 8,448** (−31).
- [x] R2.3d `exec` / `exec_status` in Loam. Done 2026-11-14. Raw `popen`
      / `fread` / `pclose` (`_popen` / `_pclose` on Windows, as variants), the
      status in a module `let` (`1` until a run). `WIFEXITED` / `WEXITSTATUS`
      are macros, so their bits are spelled in Loam (exited when the low
      seven bits are clear, the code in the next byte; the same on Linux and
      macOS); Windows `_pclose` hands back the code itself. Same limits as
      the C: an empty command or one of 8191+ bytes gives status `1`, and
      output is read in 512-byte `fread`s up to 256 KiB. A full chunk is one
      `push_bytes`, only the last short one goes byte by byte, and
      `string_from_bytes` takes over the buffer where the C copied it once
      more at the end. Windows' `WIFEXITED` / `WEXITSTATUS` / `popen` /
      `pclose` compat defines go with it. A probe of 12 cases (output, exit
      3 / 255 / 127, a signal, empty, 8191 bytes, 300 KB against the cap, an
      exact 1024, NUL bytes, stderr) is byte-identical to HEAD's C, with 0
      leaks. New fixture `compile_pass/sys_exec` (commands meaning the same in
      `sh` and `cmd.exe`, so it also runs on Windows; HEAD's C prints the
      same). Benchmark vs HEAD's C, 3 rounds (process start dominates and is
      noisy, ±60 ms per round): `exec("true")` ×300 626–663 vs 623–750 ms, a
      256 KiB `cat` ×100 316–334 vs 312–352, `echo` ×300 632–684 vs 623–736.
      The ranges overlap fully. Seed 5,071,275 → 5,076,827 bytes, seed =
      stage 1 = stage 2. Suite: `sys_exec`, `sys`, `fs`, `os_`,
      `compile_fail` (164), `http`; the 8 stanzas plus `wasm_trap`;
      `cross-check.sh` Linux 123/123 and Windows 111/111 on both CPUs.
      **Runtime: 8,448 → 8,389** (−59).
- [x] R2.3e `spawn` / `wait` in Loam. Done 2026-11-14. The spawn table
      is a module `[]i64` (pid or process handle per slot, `0` free; a
      handle is slot + 1), and the slot search, the argument packing and the
      Windows quoting (`CommandLineToArgvW` rules) are Loam. `waitpid` /
      `_spawnvp` / `_cwait` are raw bindings, and the status decode is the
      `WIFEXITED` bit math from R2.3d (a signal is `-1` here, as the C had
      it). Three shims are left, 25 lines in all. `loam_sys_argv_fill` lays
      a `char **` over Loam-owned `[]i64` slots, so C allocates nothing (8
      bytes a slot, so any pointer width fits; Loam never reads them).
      `loam_sys_spawnp` exists because `environ` is a variable no binding can
      name. `loam_sys_errno` lets `waitpid` retry on `EINTR` (4 on Linux and
      macOS) and nothing else; a blind retry would spin on `ECHILD`. The
      old C made one `malloc` per argument plus a `calloc`; this makes four
      Loam buffers per spawn. Checks: `sys_spawn` unchanged; a probe
      (256-child limit with a 257th refused, bad handles, double wait, a
      signal-killed child, slot reuse, 200 arguments, an empty argument, an
      empty argv) is identical to HEAD with 0 leaks. The Windows quoting,
      copied into a scratch program, matches HEAD's `loam_win_quote` byte
      for byte on 17 edge cases (empty, spaces, tabs, quotes, trailing and
      inner backslash runs, `C:\Program Files\x\`). Benchmark vs HEAD's C,
      medians of 7 rounds (process start dominates, ±5–10% per round):
      serial `/usr/bin/true` spawn+wait ×500 470 vs 467 ms (+0.6%), 64 in
      flight ×512 110 vs 107 (+3%), 200 arguments ×300 285 vs 283 (+0.7%),
      all inside the noise. Seed 5,076,827 → 5,097,807 bytes, seed = stage 1
      = stage 2. Suite: `sys_spawn`, `sys_exec`, `sys`, `os_`,
      `compile_fail` (164), `thread_spawn`, `async_await`, `http`; the 8
      stanzas plus `wasm_trap`; `cross-check.sh` Linux 123/123 and Windows
      111/111 on both CPUs. **Runtime: 8,389 → 8,286** (−103).
- [x] R2.4 stdin: `read_stdin` / `read_line`. Done 2026-11-15. Both stay
      unbuffered, as the C was: a JSON-RPC peer mixes them, and a child that
      shares stdin must get the very next byte. `read_line` is Loam over raw
      `read` (`_read` on Windows, which takes an `unsigned` and gives an
      `int`, so each OS has its exact signature: a 32-bit count where
      `size_t` is expected would leave the upper register bits undefined).
      It reads one byte per call into a 1-byte buffer kept between calls,
      grows a line buffer that is emptied (not freed) per line, retries on
      `EINTR` via `c_errno`, and stops at 8192 bytes as before. Windows
      binary mode is one `_setmode` behind a module flag. `read_stdin` is
      `string_from_bytes(read_fd(0, n))` over a new public hook,
      `sys.read_fd(fd, n) -> []u8`: it reads until `n` bytes or the end and
      sets the length to what `read` returned, so no byte is unwritten and
      none is zero-filled first. It is memory-safe as a public fn (a bad
      `fd` reads nothing), and it is the primitive the deferred R2.2
      `read_file` speed fix needs. Module buffers are race-free: workers
      cannot reach `sys` (checked: every I/O `sys` call, now Loam over
      `#[c]`, is still refused in a worker, at its binding or its global;
      only the pure `sys.os()` passes). Probe vs HEAD's C (CRLF, blank line,
      mixed `read_stdin` / `read_line`, 0 and negative `n`, a 9000-byte line
      split at 8192, NUL bytes, a child sharing stdin, 2.5 MB, EOF), from a
      file and through a pipe: identical, 0 leaks. Benchmark vs HEAD's C:
      `read_stdin` 100 MB in 64 KiB calls 11–13 vs 10–13 ms, in one call
      16–17 vs 17–20 ms (the buffer becomes the string with no copy).
      `read_line` over 1M lines, one syscall per byte in both: median 23.93
      vs 23.56 s (+1.6%), user CPU +0.25 s (about 8 ns a byte; kernel time
      equal). That residual is safe vec bookkeeping per byte (refcount,
      bounds and copy-on-write checks on the buffers) against C's stack
      array; closing it needs borrowed `[]u8` params for `#[c]` (landed
      with the borrowed-buffer step below: the retain is gone, and the
      wall time was the syscall). Seed
      5,097,807 → 5,097,820 bytes, seed = stage 1 = stage 2. Suite:
      `sys_spawn` (pipes a child's stdin), `sys_exec`, `sys`, `compile_fail`
      (164), `thread`, `http`; the 8 stanzas plus `wasm_trap` (`lsp` speaks
      JSON-RPC through both); `cross-check.sh` Linux 123/123 and Windows
      111/111 on both CPUs. **Runtime: 8,286 → 8,249** (−37).
- [x] R2.5 `watch_*` over kqueue and inotify bindings. Done 2026-11-15.
      Same four calls, no caller changed. macOS / iOS: raw `kqueue` / `open`
      / `kevent` / `close`, with `struct kevent` laid out by hand in four
      `i64` slots (ident; filter, flags and fflags little-endian; data;
      udata), the layout and every flag value read from the SDK's headers;
      the event and timeout slots are kept between calls (a drain asks
      thousands of times). **Linux gets a watcher for the first time:**
      raw `inotify_init1` / `inotify_add_watch` / `inotify_rm_watch` / `poll`
      / `read`, events parsed in Loam (16-byte header, then the name). The
      bookkeeping: an id packs handle and watch (`handle * 2^20 + wd`), so
      `watch_close(id)` still needs no handle and no table; events one
      `read` brings beyond the first wait in a module queue, so `watch_next`
      answers one per call as kqueue does, and a repeat of an id already
      waiting is folded (kqueue's `EV_CLEAR`). The mask (`IN_MODIFY`,
      `IN_DELETE_SELF`, `IN_MOVE_SELF`, `IN_CREATE`, `IN_DELETE`,
      `IN_MOVED_FROM`, `IN_MOVED_TO`) is what kqueue reports for a file and a
      directory alike; `IN_IGNORED` is skipped. Layouts and constants read
      from glibc's and musl's headers via zig (identical on x86_64 and
      aarch64). Windows, Android, wasm and other Unix keep `watch_open` = 0
      (the caller polls). The one change: a BSD (`other`) loses kqueue, its
      `struct kevent` differs from Apple's and no BSD is a supported target.
      Win32 (`ReadDirectoryChangesW`) moves to R2.6 with the Win32 paths.
      Checks: the macOS probe (edit, create, remove, rename of a watched
      file, bad paths and handles, timeouts, closes) is identical to HEAD's
      C, 0 leaks; `watch`, `serve_native`, `thread_call_sys` (a worker still
      cannot reach it) pass. **Linux is checked as far as this Mac can:** the
      watcher fixture and the probe link for x86_64 / aarch64 glibc and musl
      with `-Wall`, no warnings; the parsing, queueing, folding and id
      decoding run here against a hand-built kernel buffer (order kept,
      duplicate folded, `IN_IGNORED` skipped, names of 16 and 32 bytes
      stepped over, handles kept apart, closes drop the right waiting
      events). The kernel calls themselves first run on the Linux CI job.
      `cross-check.sh` Linux 123/123 and Windows 111/111 on both CPUs; the
      8 stanzas plus `wasm_trap` green.
      Benchmark vs HEAD's C (macOS, 3 rounds): `watch_add` ×2000 24–30 vs
      24–28 ms, `watch_next(0)` ×200k 2489–2510 vs 2484–2491 ms (noise),
      `watch_close` ×2000 18–19 vs 17–19 ms. The dead `loam_sys_cpath`
      helper goes with it. Seed 5,098,988 → 5,095,924 bytes, seed = stage 1
      = stage 2. **Runtime: 8,249 → 8,136** (−113).
- [x] R2.6a Win32 paths through the same bindings. Done 2026-11-15. The
      last path hooks go: Windows `realpath` is raw `_fullpath` (which makes a
      path absolute without touching the disk) with the `/` fix and the
      existence check in Loam; `cwd` is raw `getcwd` on posix and `_getcwd`
      on Windows (its size is an `int`, so it has its own exact signature),
      the `/` fix in Loam; `mkdir -p` binds raw `mkdir` / `_mkdir` and reads
      `EEXIST` through R2.3e's `c_errno`, which drops the mkdir errno shim
      and the Windows `#define mkdir`. `writev` (core output) and the
      `stat` / `lstat` layout shims stay. A POSIX probe (`mkdir -p` over
      existing levels, over a file, `/`, `""`, a 300-byte path past the
      wrapper's stack buffer; `realpath` through `..`, missing and empty;
      `cwd`) is identical to HEAD's C. New fixture `compile_pass/sys_paths`
      prints the same on every OS (absolute, `/`-separated, `""` for what
      does not exist, from a relative scratch directory since Windows has no
      `/tmp`), so the Windows CI job runs the Windows path code natively; it
      links for both Windows CPUs with `-Wall`, no warnings, and HEAD's C
      prints the same. `cross-check.sh` Linux 124/124 and Windows 112/112 on
      both CPUs; the 8 stanzas plus `wasm_trap` green. Seed 5,095,924 →
      5,098,656 bytes, seed = stage 1 = stage 2. **Runtime: 8,136 → 8,077**
      (−59).
- [ ] R2.6b A `ReadDirectoryChangesW` watcher; the Windows CI job is green.

**R3. Async and threads** (lands with T1, [threads.md](threads.md); T1 is
done, so every R3 step is checked by `thread_tsan`).

- [x] R3.1a Clock and sleep through raw bindings. Done 2026-11-15.
      `async.now_ms` / `sleep` are Loam over `#[c]` `clock_gettime` /
      `nanosleep` (posix + Windows, via `[]i64` out-words; EINTR retried on
      the Loam side) with `#[os("wasm")]` Loam wrappers over two header
      bridges (`loam_async_wasm_now_ms/sleep`, over the `zeus_js_now_ms`
      import — a `#[c]` binding cannot reach a wasm build at all, and
      `#[os]` variants must be bodyless hook or Loam body together). The
      worker allowlist names the new seam (`loam_async_now_ms_os` /
      `sleep_os`); all 14 thread/async `compile_fail` fixtures still reject.
      `async_*`, `thread_*`, forge traps (messages byte-identical), 5-way
      TSan, wasm32 `trap_app` green. **Runtime: 8,077 → 8,068** (−9).
- [x] R3.1b The future table in Loam, keeping S0's typed cells. Done
      2026-11-15. `fut_data: []u8` arena + `fut_slots` (`off/len/ready/key`)
      in `std:async`; codegen lowers `__fut_*` to Loam table calls plus an
      inline `memcpy` per type (heterogeneous `T` never crosses Loam as a
      value), with the Copy-vec retain/drop dance and dead-id zero-fill
      unchanged. S0 holds: the slot records `handle_key(T)` at push, every
      access checks it in Loam (`fut_check` panics like before). A
      future-free binary links no table at all (DCE via `mark_cname`, as
      `std:key` does); before, the C table shipped in every binary.
      `async_*`, `thread_*`, forge traps (messages byte-identical), 5-way
      TSan, wasm32 `trap_app` green; 14 `compile_fail` fixtures still reject.
      **Runtime: 8,068 → 7,994** (−74).
- [x] R3.1c Worker-called `async.sleep` / `now_ms` still check. Done
      2026-11-16. R3.1a moved the clock/sleep behind Loam wrappers over raw
      `#[c]` bindings, which took them off the worker allowlist — a worker
      calling `async.sleep` no longer compiled. The raw symbols
      (`clock_gettime`, `nanosleep`, `loam_sys_errno`) are allowlisted for
      workers (module-private to `std:async`, so only the wrappers name
      them). Probe: a worker that sleeps then sends, drained on the UI.
- [x] R3.2 The channel queues in Loam; mutex, condition variable and thread
      start stay C by the §2 rule (lock and condvar calls stay in the core).
      Done 2026-11-16 via R3.2a below: the table and S0 keys are Loam, the
      per-op sections stay one atomic C unit each. Turning the section
      *bodies* into `#[c]` calls would just rename C, not move logic
      (closed as analyzed — no code change).
- [x] R3.2a The channel id table and S0 keys in Loam. Done 2026-11-16.
      `ch_slots` (`cookie` + `key`) in `std:thread` under a C table lock
      (`loam_ch_tab_mtx`, beside `loam_thread_mtx` — the lock itself stays
      C because Loam vec refcounts are not atomic; passing the mutex as a
      Loam value raced by construction, and TSan caught the first shape of
      this). Codegen lowers `__ch_*` to `ch_begin` (S0 panic on forged ids,
      silent 0 on dead ones, as the C table did) plus one atomic C section
      per op (`loam_ch_do_*`, taking the cookie: lock, ring math, bytes,
      counts, signal, unlock — inseparable across a Loam boundary, so they
      stay C). Per-channel sync blocks are C-allocated; the wasm stubs turn
      inert (`memset` zero-fill stays, over the Loam table's ids). 5-way
      TSan clean, forge traps byte-identical, worker-sleep probe green.
      A channel-free binary links no table. **Runtime: 7,994 → 7,981** (−13).
- [x] R3.2b Pthread calls as `#[c]` bindings. Scoped 2026-11-16: no further
      C moves without breaking atomicity or the worker proof — folded into
      the R3.2 item above. The sections must hold the mutex across
      `cond_wait` (one C unit); the ring math needs head/count/cap under
      that same lock; splitting either across a Loam boundary reintroduces
      exactly the races TSan gates. The `#[c]`-visible surface (tab-lock
      shims, `block_alloc`, section entry points) is already the minimal
      binding shape. Closed as analyzed — no code change.

**R4. Zeus platform** (`zeus_plat.c`, `zeus_rt.h`).

- [x] R4.1a Signal id free-list in Loam. Done 2026-11-16. `free_sigs` +
      `sig_reserve` / `sig_release` / `sig_alloc` / `sig_free_cell` in
      `zeuscore/arena.loam` replace the C free-list; `sig_slot_ensure` grows
      the cell table + arena mirror for a Loam-reserved id. Codegen
      `__sig_push` and captured-`let mut` state signals reserve through
      Loam (`mark_cname` keeps the reserve fn, as R3.1b does for futures).
      `zeus_state`, signal fixtures, both drop goldens, forge trap
      byte-identical, wasm32 `trap_app` warning-free. **Runtime: 7,981 →
      7,961** (−20).
- [x] R4.1b The signal byte store as a `[]u8` arena with S0's typed cells.
      Done 2026-11-15. `zeuscore/sigcells.loam` (a leaf module: `arena`,
      `track` and `flow` all read it) holds every value in one `[]u8` and a
      `SigCell` slot per id (offset, capacity, bound width, generation, S0
      key, and what the bytes own). Codegen lowers `__sig_push` / `load` /
      `store` to `sigcells.bind_at` / `load_at` / `cmp_at`, which return an
      offset, then one `memcpy` against `loam_sigcells_bytes.ptr` with no
      Loam call in between (R3.1b's shape, minus the retain/drop of the
      arena). The S0 check is Loam, same message. Freeing a cell that owns a
      string, a list or a struct with owned leaves goes through
      `loam_sigcells_payload_drop`, which codegen emits as a switch over its
      own release hooks: the cell records the kind and the hook *id*, so no C
      function pointer is kept in Loam. Ids, the `int` mirror, scope records
      and the int paths (`sig_new_int`, `state_get`, `store_sig`) are
      `arena.loam`; the C `Signal` handle keeps three one-line trampolines
      in `zeus_rt.h` (R4.5 removes them). A slot keeps its bytes across a
      free, so a recycled id reuses them; a wider bind moves it to the end of
      the arena, and since a slot's capacity only grows, the bytes left
      behind are bounded. Deleted: the `ZeusCell` table and every
      `loam_zeus_sig_*` fn, the `runtime.sig_*` hooks, and the staged
      `rt_mem_*` bindings, which were never wired up and were a safety hole
      anyway (a raw pointer as an `int`, unchecked offsets).
      **Bugs found on the way:** UBSan flagged misaligned in-place releases
      (byte offsets, where `malloc` had aligned the C cells), so every cell
      now starts 8-aligned. DCE's mark for captured-`let mut` state sat
      inside the `Call` branch and never fired; it is now its own check.
      `arena.sig_reserve` now runs `ensure()` first, so a captured `let mut`
      that is a program's first signal can never get id 0 (the null signal).
      A wasm-only helper (`history_path`) is now under `#ifdef __wasm32__`,
      which removes the Linux `-Wunused-function` warning in `zeus_plat.c`.
      An `int` push now binds once (the C bound it twice and counted 2
      generations). Benchmark vs HEAD's C, release `-O1`, 2M set+get each
      (ms, 5 rounds, steady): `int` 97 → 70, `string` 163 → 85, struct with
      a string 164 → 79, `[]int` 156 → 80, `float` 99 → 65, 200k fresh
      signals 15 → 8. Headless `-O0` builds are 30–40% slower (out-of-line
      bounds checks and struct copies through `memcpy`; tracking still
      dominates there). A probe of strings, string lists, owning structs
      and int lists, freed and recycled 200k times: same output as HEAD,
      flat RSS (4.2 vs 4.4 MB), 0 leaks, ASan/UBSan clean. Binaries (new vs
      HEAD): tiny int program 33,688 vs 33,696, greeninfer 1,046,312 vs
      1,045,272 (+0.1%; one inline `memcpy` per signal site), counter
      1,011,160 vs 1,010,024; compile time within noise. Seed 5,123,474 →
      5,128,304 bytes, seed = stage 1 = stage 2. Suite: `zeus` (445),
      `golden` (135), `signal`, `drop`, `handle` (forge traps unchanged),
      `state`, `each`, `async`, `thread`; `wasm_smoke`, `wasm_trap`,
      `wasm_float`, `ownership_asan`, `android_project`, `cost`, `lsp`,
      `fresh_cache`, `arena_lint`, `api_freeze`, `one_surface`,
      `stale_capture`, `server_split`, `modules` green; Zeus C compiles for
      x86_64 glibc / aarch64 musl Linux with `-Wall`, no warnings.
      **Runtime: 7,961 → 7,612** (−349).
- [x] R4.2 Frame and trace logic. Done 2026-11-15. `zeuscore/replay.loam`
      decodes a frame into the host's `ZeusDraw` calls: the op switch, the
      field unpacking (packed shadow offsets and corner radii), the 24-bit
      colour mask, the range checks, and every fallback for a callback the
      host left NULL (gradient → solid fill, stroke → four `fill_a` edges,
      `fill4` → largest radius, rotated → plain text, shadow / xform / image
      skipped). C keeps a capability mask (`draw_caps`: one bit per non-NULL
      callback, plus a bound frame sink) and one forwarder per callback,
      each re-checking its own pointer; a text or image op hands over the
      frame's string table and an index, checked in C, so no refcount is
      touched per op, and the text is NUL-terminated in a 256-byte stack
      buffer where the C `malloc`'d and freed one per op. `plat_frame` now
      only hands a frame whole to a bound sink (the web host). The trace host
      is Loam too (`replay.trace_draw`, one shared line formatter): its
      2,089-line golden is byte-identical. Deleted: `zeus_replay`, the
      `op_*` fallbacks and the 15 `tr_*` printers. **Compiler change found on
      the way:** a module `const` / immutable `let` holding a literal number
      or bool was emitted as a mutable C global, so `cc` could fold none of
      them (`const STRIDE: int = 11` stayed a load and a checked multiply in
      every index). Such globals are now C `const`, which `cc` folds even
      with external linkage; no new warnings in the compiler's, nob's or
      greeninfer's C. Benchmark vs HEAD (release `-O1`, the gallery's
      sections repainted 20,000 times into a host whose callbacks only
      count, 5 rounds): a frame 1,331 vs 1,368 ms (−2.7%), the same
      14,040,702 host calls and 85,444,272 text bytes; with no host bound
      1,129 vs 1,187 ms (−4.9%, the `const` folding). The replay alone is
      about 200 vs 180 ms of that, so the Loam decode is ~15% slower than
      the C one (bounds and overflow checks per field) and the frame is
      faster overall. The real Cocoa host's 2x snapshot of the gallery
      (`ZEUS_SNAPSHOT`) is pixel-identical to HEAD's but for the RAM
      readout. Binaries (new vs HEAD): tiny program 33,688 both, gallery
      2,077,496 vs 2,071,944 (+0.3%), greeninfer 1,053,672 vs 1,046,312
      (+0.7%; the checked Loam decode is larger than the C). Compile time
      within noise. Seed 5,128,304 → 5,132,237 bytes, seed = stage 1 =
      stage 2. Suite: `zeus` (445), `golden` (135), `compile_pass` (700),
      `compile_fail` (164); `wasm_smoke`, `wasm_trap`, `wasm_float`,
      `ownership_asan`, `android_project`, `cost`, `lsp`, `fresh_cache`,
      `arena_lint`, `api_freeze`, `one_surface`, `stale_capture`,
      `server_split`, `modules` green; `zeus_plat.c` builds for x86_64
      glibc / aarch64 musl Linux with `-Wall`, no warnings.
      **Runtime: 7,612 → 7,410** (−202).
- [x] R4.3 Recoverable traps. Done 2026-11-15. The arm stays in
      `loam_rt.h` until R6.2 (`loam_trap_guard`, `setjmp`, `loam_jmp_msg`).
      What moved is the policy. `guard_fn` decides in Loam: an id outside
      the intern table, a slot with no function, and a slot of another
      signature are `""`, and only the last of those is counted and logged.
      `boundary` and `trap_msg` are the same calls they were. The wasm
      restart table is 16 Loam slots (`preset_seq`, `preset_text`,
      `preset_count`), constant initializers, so `__init` does not write
      them and a store from the web loader before `main` is still there.
      `preset_store` is a DCE c_root of `runtime` (only
      `zeus_boundary_preset_add` calls it). C keeps the intern-table
      storage, the mismatch counter and its stderr line, and one-line
      forwarders: the arm, the slot bits, the panic-buffer view, and the
      preset add (copy, call, release — the string parameter is borrowed
      and the store retains what it keeps). A preset read retains the
      slot, so the text survives the caller dropping its copy. Fixture
      `zeus_trap_guard` was printed by HEAD's compiler (`ZEUS_HEADLESS=1`)
      before the port and still matches. Benchmark vs HEAD, release
      `-O1`, no Cocoa host, 2,000,000 calls, 5 rounds (ms, new vs HEAD).
      A void handler that returns: guard 487–501 vs 485–494, boundary
      477–501 vs 477–479; the medians are 489 vs 486 and 479 vs 477,
      and the sums are 0.
      Binaries (new vs HEAD): tiny `fmt.println(42)` 33,688 both, gallery
      2,079,160 vs 2,078,960 (+200), greeninfer 1,071,912 vs 1,071,704
      (+208). ASan+UBSan on the fixture and on a pre-main preset probe
      (first match, empty message, embedded NUL, a `{{` in the text, the
      16-cap): stdout matches, sanitizer stderr empty. Seed 5,132,477 →
      5,133,189 bytes, seed = stage 1 = stage 2. Suite: zeus batch (296,
      including the 59 draw goldens), `compile_fail` (164); `wasm_smoke`,
      `wasm_trap`, `wasm_float`, `ownership_asan`, `android_project`,
      `cost`, `lsp`, `fresh_cache`, `arena_lint`, `api_freeze`,
      `one_surface`, `stale_capture`, `server_split`, `modules`,
      `design_lint`, `loader_cases`, `thread_tsan` green; `zeus_plat.c`
      builds for x86_64 glibc / aarch64 musl Linux with `-Wall`, no
      warnings; wasm32 `zeus_cell` warning-free.
      **Runtime: 7,216 → 7,204** (−12).
- [x] R4.4 Image sizes and media state. Done 2026-11-15.
      `zeuscore/imagesize.loam` reads a PNG, GIF, JPEG (SOF scan, including
      progressive SOF2) or WebP (VP8X, VP8, VP8L) header. A `data:` URI
      base64-decodes the first 4096 bytes after `base64,`, skipping bytes
      outside the alphabet; `file://` is stripped and a bare path is a file,
      and only the first 4096 bytes are read; `http://` and `https://` stay
      0. `measure` asks the host hook first and keeps that size when both
      sides are positive. C keeps the hook (`loam_zeus_plat_image_size`
      writes 0, 0 when none is bound), `image_host_bound` (cached in Loam
      after the first call) and `read_head` (fopen / fread / fclose, an
      empty vec on wasm or on failure). `layout` calls `imagesize.measure`.
      `zeuscore/mediastate.loam` holds the null player (8 slots; open
      refuses slot 0; `""` is state 0, anything else is state 2 at position
      0; commands 1, 2, 3 and 8; a command on state 0 is ignored), the
      fullscreen flag, and the pick queue (64, later picks dropped, the
      first pick published at once and redrawn, promote on get(7) only when
      the queue is non-empty and the text was taken or `told == seq`, text
      cut at 4095 and at an embedded NUL). `media_host_caps` is one bit per
      non-NULL `ZeusMedia` callback, plus a wasm bit. A set callback is
      forwarded; the null player runs otherwise. On native, get(7) and
      text(1) stay in Loam even when the host table is set. On wasm, open,
      cmd, get, text, pick and fullscreen go to JS, voice returns 0, and
      pick-audio uses the pick import. C keeps one forwarder per callback
      (a direct call, strings through `cstr_in` / `cstr_done`) and the wasm
      imports. `zeus_media_picked` copies both C strings into `loam_str`
      and calls the Loam export `engine_media_picked`; Loam decides the
      redraw. The macOS host binds that export next to `picked_image`.
      **On the way:** a signed base64 accumulator hits the overflow check
      where C's unsigned one wraps, so the decode uses `wrapping_shl` /
      `wrapping_add` and the bits match. Once `plat_media_*` had Loam
      bodies, codegen's `int32_t` prototypes clashed with the old
      `int64_t` declarations, so those declarations left `zeus_rt.h`.
      `engine_media_picked` is a DCE c_root of `zeus` (next to
      `engine_picked_image`); without it the C one-liner does not link.
      The header scan stays level with C by a base64 lookup table, a
      one-byte search for `base64,`, a module-level 4096-byte buffer
      (constant offsets need no bounds check) and skipping the host hook
      when none is bound. Fixtures `zeus_image_size` and `zeus_media_null`
      were printed by HEAD's compiler (`ZEUS_HEADLESS=1`) before the port
      and still match, including a real PNG read with and without
      `file://`, and 70 picks. Benchmark vs HEAD, release `-O1`, no Cocoa
      host, 5 rounds. Image, 200k `measure` calls per data URI (ms, new
      vs HEAD): png 59–60 vs 59–61, gif 19–20 vs 20–21, jpeg 28 vs 28–29,
      progressive jpeg 23–24 vs 23–24, VP8 34–35 vs 34–35, VP8L 35 vs
      35–36, VP8X 34 vs 34–35; the width+height sums match. Media, 2M
      `get(1, 1)` / `get(1, 3)` with no host: 5 ms vs 16–17 ms, sum
      4,000,000. Binaries (new vs HEAD): tiny `fmt.println(42)` 33,688
      both, gallery 2,078,968 vs 2,077,504 (+0.07%), greeninfer 1,071,704
      vs 1,053,672 (+1.7%). ASan+UBSan on both fixtures: empty stderr,
      stdout matches the goldens. Seed 5,132,237 → 5,132,477 bytes, seed
      = stage 1 = stage 2. Suite: `zeus` (449), `golden` (135),
      `compile_fail` (164); `wasm_smoke`, `wasm_trap`, `wasm_float`,
      `ownership_asan`, `android_project`, `cost`, `lsp`, `fresh_cache`,
      `arena_lint`, `api_freeze`, `one_surface`, `stale_capture`,
      `server_split`, `modules` green; `zeus_plat.c` builds for x86_64
      glibc / aarch64 musl Linux with `-Wall`, no warnings; wasm32
      `zeus_cell` warning-free.
      **Runtime: 7,410 → 7,216** (−194).
- [x] R4.5 The `loam_zeus_engine_*` trampolines are gone: hosts call the
      Loam exports. Done 2026-11-15. Every `zeus_handle_*` forwarder,
      `zeus_layout`, `zeus_step`, `zeus_over_button`, `zeus_cursor`,
      `zeus_focus_captures_text`, `zeus_window_drag_at`, `zeus_picked_image`
      and `zeus_media_picked` left `zeus_plat.c` and `zeus_rt.h`; the Cocoa,
      iOS, Android, X11 and web hosts call `loam_zeus_engine_*` themselves
      (the web exports went through two C layers before). `zeus_paint` stays:
      it binds the host's draw table and context, which is platform state,
      not a forward. The contract is written once in `zeus_rt.h`
      and `host-contract.md`: a `loam_str` argument is borrowed
      for the call (a host passes `(loam_str){ptr, len, 0}` over its own
      buffer, `za_str` on Apple), the engine copies what it keeps, and a
      returned `loam_str` is the host's to release. A host with a float `dt`
      passes `dt * 1000 + 0.5` ms to `engine_step_dt`, which already maps
      `<= 0` to 16, so every host steps by the same milliseconds as before
      (the web host does it in integer ms). `engine_picked_image` now redraws
      itself through `mediastate.host_redraw`, as C did after it. In a
      `zeli serve` host build the loader binds no-op stubs for the optional
      entries an older image may lack (`context_click`, `window_drag_at`,
      `set_reduced_motion`), so no call site tests for NULL. The three
      `Signal` trampolines go too: a captured `let mut int` lowers to
      `arena.sig_new_int` / `state_get` / `store_sig` on the handle's `id` (a
      field load and a struct literal in the IR), so `zeus_rt.h` no longer
      has any `static inline` code.
      **Bugs found on the way.** (1) Typed text was not copied: the C
      forwarders handed Loam an `own = 0` view of the host's buffer, and
      `insert_text` parked it in `arena.ev_text`, so a key handler that kept
      `e.text` kept a pointer into a buffer the host reused (the web text
      buffer) or freed (Cocoa's autoreleased `UTF8String`). It is copied now;
      fixture `zeus_host_text_copy` drives it from a C shim that reuses its
      buffer after the call (HEAD prints `kept=XYZ`, this prints `kept=abc`).
      (2) Leaks at the old seam: `zeus_cursor` and mac's accessibility dump
      never released the string the export returned (a fresh dump per
      VoiceOver query), and `zeus_picked_image` / `zeus_media_picked` passed
      copies to borrowed parameters and never released them. (3) The X11
      host did not compile: `linux_fill_g` had lost the `radius` / `alpha`
      parameters `ZeusDraw.fill_g` gained, which clang rejects; it now fills
      with the radius. (4) Compiler: a method chained onto a call that fills
      default props (`zeus.Text("a").focusable()`) failed with a cascade of
      `cannot infer type parameter 'T'`, because the UFCS receiver was
      checked a second time as argument 0 and its props' `none` defaults were
      refilled with no expected type. A call receiver is now checked once
      (`M_RECV_TYPED`); fixture `zeus_chain_on_call` (HEAD: 222 errors).
      Checks: the gallery's 2x `ZEUS_SNAPSHOT` through the real Cocoa host is
      byte-identical to HEAD's; the `LOAM_HOST` build of mac.m links; the iOS
      Simulator gallery builds; `linux.c` and `zeus_plat.c` build for x86_64
      glibc and aarch64 musl with `-Wall`, no warnings; `android.c` type-checks
      for aarch64 against the JDK's `jni.h` (no NDK here); the wasm32 gallery
      and `zeus_cell` build warning-free. Benchmark vs HEAD (release `-O1`, 7
      rounds, ms): captured `let mut` bump + read ×2M 77–78 vs 78–79; host
      input from C, `engine_hover` + `engine_scroll_step` ×1M 274–276 vs
      274–275; the compiler emitting itself, median of 10, 177.6 vs 176.6
      (min 175.3 vs 175.7). Binaries (new vs HEAD): tiny `fmt.println(42)`
      33,688 both, gallery 2,078,216 vs 2,079,184 (−968), greeninfer
      1,070,904 vs 1,071,912 (−1,008). Seed 5,133,189 → 5,138,643 bytes, seed
      = stage 1 = stage 2. Suite: `zeus` (455), `golden` (135),
      `compile_pass` (710), `compile_fail` (164), `signal`, `state`;
      `selfhost_seed`, `wasm_smoke`, `wasm_trap`, `wasm_float`,
      `android_project`, `lsp`, `ownership_asan`, `cost`, `fresh_cache`,
      `arena_lint`, `api_freeze`, `one_surface`, `stale_capture`,
      `server_split`, `modules`, `design_lint`, `loader_cases`,
      `thread_tsan`, `serve_native` green; `cross-check.sh` Linux 125/125 and
      Windows 113/113 on both CPUs. Hosts grow 4,869 → 4,903 (+34: the cursor
      and string helpers, the reload stubs, the `fill_g` fix), so trusted C
      falls by 104 in all. **Runtime: 7,204 → 7,066** (−138).
- [x] R4.6 `maya_plat.c` and `raygui_plat.c`: the bridge logic moves to
      `packages/maya` and `packages/raygui`; raylib calls become `#[c]`
      bindings of those packages. `maya_mac.m` moves to the maya package as
      a host file. Done 2026-11-15. The rule for the moved files: none
      includes `loam_rt.h`, `zeus_rt.h` or any runtime header, and they cross
      the boundary only with the plain `#[c]` ABI. No package has a shim.
      **Maya:** `maya_mac.m` is `packages/maya/src/hosts/mac.m`, with no
      Loam types: it calls int-only `engine_*` exports, and Loam calls its
      primitives (`maya_mac_stars` / `clear` / `ring` / `disc` / `label` /
      `hud` / `blit` / `window` / `run`) through `#[os("macos")] #[c]`
      bindings. `maya.loam` names it in `#![c_source]` and links Cocoa with
      `#![c_link]`. The draw order (`engine_present`, `present_map`), the
      headless rule and the clock (`async.now_ms`) are Loam; other OSes get
      `{ return }` variants (an empty body would be a bodyless hook).
      `scene.fb_rgba` builds the RGBA frame where C called `fb_at` once per
      pixel. **raygui:** `vendor/raygui.h` is compiled as itself
      (`#![c_source("vendor/raygui.h", "-x", "c", "-DRAYGUI_IMPLEMENTATION",
      "pkg-config:raylib")]`), and `raygui.loam` binds raylib and raygui
      directly: `Bounds` / `Rgba8` are `Rectangle` / `Color` by value (§4 a1),
      and the stateful controls share reused `[]bool` / `[]i32` / `[]f32`
      cells. Window state, the headless rule, the `RAYGUI_FRAMES` cap (an
      `atoi`-like `leading_int`, read once per run where C read it every
      frame) and the guards are Loam, and `rgba8` replaces `GetColor`. The RAM
      probe is Loam over raw bindings: on macOS `task_self_trap` (cached; it
      is what the `mach_task_self()` macro reads) and `task_info(TASK_VM_INFO,
      93)`, `phys_footprint` at word 18; on Linux `getrusage`, `ru_maxrss` at
      word 4. Both kernel buffers are made once and reused.
      **Compiler:** `#[c]` params may be plain-data structs (every field a
      number, a bool or such a struct) and `[]bool`. `pkg-config:NAME` in
      `#![c_source]` / `#![c_link]` expands to `NAME_PREFIX`, else
      `pkg-config`, else the Homebrew prefixes; the object key hashes the
      expanded flags. A module brings its C sources and link flags only if a
      kept `#[c]` fn of it builds for this machine (`mask_builds_here`), so
      Linux never compiles `hosts/mac.m`. The maya and raygui link special
      cases, `raylib_flags`, `raygui_dir`, the `maya_rt.h` / `raygui_rt.h`
      includes and the maya struct skip are deleted. `nob trusted` gains a
      `bindings` group (package and example C outside `vendor/` and
      `hosts/`), so greeninfer's 276 lines, which no group counted before,
      are counted now; package `hosts/` files count as hosts. New stanza
      `c_layouts`: a generated probe `_Static_assert`s every hand-transcribed
      layout and constant (`timespec`, `kevent` and its flags, `task_vm_info`,
      `inotify_event` and the watch mask, `rusage`, `CLOCK_MONOTONIC`,
      `_SC_NPROCESSORS_ONLN`, raylib's `Rectangle` / `Color`) natively and,
      with zig, for x86_64 glibc and aarch64 musl Linux (0.4 s; a wrong
      offset fails on the target it belongs to).
      **Bugs found on the way.** `maya_mac.m` leaked the HUD and sprite-name
      strings every frame. The old raygui seam cut text at 128, 512 or 1,024
      bytes; bindings pass the whole string. The first port of the RAM probe
      built its 47-word buffer by `push` every frame (12% of a headless frame)
      and the first `fb_rgba` stored through the global, which reloaded it
      after every byte and checked each conversion (1.9x the C). Both were
      fixed before landing, as below.
      Checks: the solar and studio windows and the raygui example render as
      HEAD's do (screenshots; only the animation clock differs); raygui
      headless prints its 3 frames with flat RAM; with no `pkg-config` on
      `PATH` it builds through `RAYLIB_PREFIX` and through the Homebrew
      fallback; the emitted maya C compiles for x86_64 glibc and aarch64 musl
      Linux with no `maya_mac` symbol, and `raygui.h` and the raygui
      example's C compile for both. Fixtures: `compile_pass/c_ffi` passes
      `Rect` / `Rgba` by value, a nested struct with a bool and padding
      between scalars, and a `[]bool` C writes; `compile_fail/
      c_attr_struct_param` rejects a struct with a `string` field.
      Benchmark vs HEAD (release `-O1`, 5 rounds): raygui headless, 1M
      frames, 1,517–1,525 vs 1,707–1,713 ms (−11%; RAM growth 96 vs 192 KB);
      the RGBA frame, 960x600 x300, 332–346 vs 343–349 ms; the compiler
      emitting itself, median of 10, 182.3 vs 183.4 ms (min 181.0 vs 181.1).
      Binaries (new vs HEAD): tiny `fmt.println(42)` 33,688 both, raygui
      example 90,648 vs 90,008 (+640), studio 96,904 vs 96,136 (+768), solar
      97,128 vs 96,376 (+752): checked Loam where the C was. Seed 5,138,643 →
      5,134,758 bytes, seed = stage 1 = stage 2. Suite: `compile_pass` (710),
      `compile_fail` (165), `zeus` (455), `golden` (135), `maya`, `raygui`,
      `c_ffi`, `language`; `selfhost_seed`, `wasm_smoke`, `wasm_trap`,
      `wasm_float`, `android_project`, `lsp`, `ownership_asan`, `cost`,
      `fresh_cache`, `modules`, `loader_cases`, `thread_tsan`,
      `serve_native`, `c_layouts` green; `cross-check.sh` Linux 125/125 and
      Windows 113/113 on both CPUs. Hosts 4,903 → 5,253 (+350: maya's host
      moved in), bindings 276 (newly counted); trusted C outside vendored
      code 11,969 → 11,777. **Runtime: 7,066 → 6,248** (−818).

**R5. Net** (`net.c`, `net_rt.h`).

- [x] R5.1 Socket and `poll` bindings; the connection state, buffering,
      timeouts and non-blocking connect in Loam. Done 2026-11-15. First, §4
      feature d: `#[c("sym", "variadic after N")]` declares the first N
      parameters and passes the rest through C's `...`. The binding's call
      casts to `ret (*)(fixed..., ...)`, so `cc` applies the real variadic
      ABI (on Apple arm64 the varargs go on the stack, where a fixed-arity
      call leaves them in registers the callee never reads). Each call shape
      is its own binding (`fcntl(fd, F_GETFD)` and `fcntl(fd, F_SETFD, x)`);
      N may equal the parameter count. A variadic parameter must be a type
      `...` reads unchanged: `i32`, `u32`, `i64`, `u64`, `f64`, `string` or a
      buffer, never `f32`, `bool`, a narrow int or a struct, which C
      promotes. The second argument is a string, so the grammar and
      tree-sitter are unchanged. Then `std:net`'s TCP is Loam over raw
      bindings (`socket`, `connect`, `bind`, `listen`, `accept`, `recv`,
      `poll`, `setsockopt`, `getsockopt`, `getsockname`, `fcntl`, `close`):
      IPv4 parsing (as `inet_pton`), `localhost` for connects, a `struct
      sockaddr_in` as a 16-byte `[]u8` per OS (Apple's length byte first),
      a `pollfd` as two `i32`s, close-on-exec and `O_NONBLOCK` through the
      variadic `fcntl`, `SO_NOSIGPIPE` / `MSG_NOSIGNAL`, the 5 s accept read
      timeout, the non-blocking connect's `EINPROGRESS` rule, `EINTR`
      retries, the would-block flag, the 60 s poll cap, and the growing poll
      set. Reads `recv` into one reused 64 KiB buffer and copy out only the
      bytes read (new core hook `buf.prefix_to_string`, 4 lines), where the
      C `malloc`'d 64 KiB per read. `poll` binds per OS (`nfds_t` is 32 bits
      on Apple, 64 on Linux). Two pieces stay C. `send_at` is one `send`
      from an offset into a string: Loam has no pointer into a string, and a
      `#[c]` string argument would copy the rest on every resume, which is
      quadratic for a large body (the R1.3 rule for core byte movement). The
      macOS TLS client stays too (R5.2 needs §4 a2 and e), and Loam reaches
      it through seven `tls_handle_*` ops for a handle at or above
      `TLS_BASE`, called only from `#[os("apple")]` code, so Linux and wasm
      link none. Every cell starts empty, so a program that imports `http`
      only to parse allocates nothing at start-up. Windows: TCP answers `-1`
      in Loam and `net.c` compiles to its stubs, so every fixture now links
      there (`cross-check.sh` drops its skip list; `win-bundle.sh` still
      leaves out the 12 that open sockets, now found by a kept `tcp_` /
      `tls_` fn, since a module's globals ship with every importer).
      `c_layouts` gains `sockaddr_in`, `pollfd`, `socklen_t`, `nfds_t`,
      `timeval` and every socket, `fcntl` and `errno` constant, per OS.
      **Bugs found on the way:** a first draft packed the address into an
      `i32`, which panicked for any address from 128.0.0.0 up, and polled fd
      `-1` for a closed TLS handle (HEAD answers 1 at once, through
      `POLLNVAL`). HEAD's `tcp_send` never set `tcp_wouldblock`, against its
      doc; it does now. Helpers that call bindings must be `#[os("posix")]`
      too, or the wasm build sees undeclared calls (`wasm_smoke` builds no
      net app, so this check was a wasm build of `zeus_async_rpc`).
      Checks: a probe of 15 address strings, bad ports, refused blocking and
      non-blocking connects, `so_error`, send offsets, would-block, peek,
      poll timeouts, a 200-entry poll set, EOF, bad fds, a stale TLS handle,
      zero sizes and a 256 KiB non-blocking transfer prints what HEAD's C
      prints, with 0 leaks and clean ASan+UBSan. HTTPS (sync and async)
      against a live host works. Fixtures: `c_ffi` calls a `va_arg` C fn
      (ints, a double, a string) and libc `fcntl` (without the variadic
      shape the `va_arg` call hangs on Apple arm64: the count is garbage);
      `compile_fail/c_attr_variadic_count`, `_type`, `_shape`.
      Benchmark vs HEAD (release `-O1`, 7 rounds, ms): 100k request/reply
      round trips 1,803–1,996 vs 1,792–1,982; 1 GiB in 64 KiB writes
      399–424 vs 391–439, and 4 GiB retires 5.93 vs 5.94 G instructions
      (the time is all `recvfrom` / `sendto`); a 1,000-entry poll set
      ×2,000 117–125 vs 118–124; 2,000 connect+accept+close 85–100 vs
      86–106. Self-emit 180.0 vs 181.1 ms (min of 10). Binaries (new vs
      HEAD): tiny `fmt.println(42)` 33,688 vs 33,696; `http_parse` `__text`
      +96 bytes (net's `__init` of empty cells); `http_listen` `__text`
      151,504 vs 144,600 (+4.8%, mostly `cc` inlining the now-static std fns
      into the server loop) and its file +16 KiB (one page); `zeli` `__text`
      509,224 vs 504,440 (+0.9%). Follow-up: DCE keeps every module `let`
      initializer, so a module's globals and `__init` ship with every
      importer; dropping unreferenced pure globals would remove even those
      96 bytes. Seed 5,134,758 → 5,155,152 bytes, seed = stage 1 = stage 2.
      Suite: `compile_pass` (710), `compile_fail` (168), `zeus` (455),
      `golden` (135), `http` (39), `net_`, `c_ffi`, `buf`; `selfhost_seed`,
      `wasm_smoke`, `wasm_trap`, `wasm_float`, `android_project`, `lsp`,
      `ownership_asan`, `cost`, `fresh_cache`, `modules`, `loader_cases`,
      `thread_tsan`, `serve_native`, `serve_boot`, `serve_build`,
      `server_split`, `watch`, `c_layouts` and the lint stanzas green;
      `cross-check.sh` Linux 125/125 and Windows 125/125 (was 113 + 12
      skipped) on both CPUs. The Linux socket calls are link-checked and
      layout-checked here; they first run on the Linux CI job.
      **Runtime: 6,248 → 5,813** (−435: `net.c` 1,295 → 862, `net_rt.h`
      51 → 42, `loam_rt.h` +7).
- [x] R5.2 TLS: SecureTransport as `#[c]` bindings on macOS. Done
      2026-11-15. First, §4 feature e: a `#[c]` parameter may have a fn type
      whose parameters and result are numbers and bools, and its argument
      must name a module-level fn that is not generic, not a `#[c]` binding
      and not a body-less hook. Codegen passes that fn's own C symbol (its
      Loam C signature is the C one), not its `loam_fn` trampoline, so C
      calls it with the C ABI. The raw value exists only as that argument:
      closures, fn values held in variables, and calling such a binding
      through a fn value are compile errors. Each `#[os]` variant's
      `__as_fn` trampoline is now emitted under that variant's own `#if`
      (only the primary's was emitted, unguarded, so a wasm build of an
      Apple-only fn used as a value called a fn that did not exist there).
      Then the TLS client is Loam (`#[os("apple")]`; other targets answer
      `-1` in Loam):
      - The 8-slot table (state, socket, `SSLContextRef` as an opaque
        `i64`).
      - `getaddrinfo` walked by copying each 48-byte `addrinfo` into an
        `[]i64` with `memcpy`, and the 10 s socket timeouts.
      - The blocking handshake with its three-idle-polls rule, and the
        non-blocking connect and `tls_nb_ready` state machine.
      - Read, write, send, close; context set-up (SNI and name check, TLS
        1.2 floor).
      - SecureTransport's two I/O callbacks, `recv` / `send` into the
        buffer C hands over, the `size_t` length read and written through
        C's pointer, and `errno` mapped to `OSStatus`.

      `send_at` gives way to one smaller hook, `string_bytes_at` (a string's
      byte address for a binding to read in the same call), so `send` and
      `SSLWrite` are plain bindings. What `net.c` keeps: that hook, and the
      wasm bridges with their native stubs. `c_layouts` gains `addrinfo`,
      the `OSStatus` values, the SecureTransport enums and the callbacks'
      errnos. Fixtures: `c_ffi` calls back into Loam from a vendored C fn and
      sorts with libc `qsort` and a Loam comparator;
      `compile_fail/c_attr_callback_closure`, `_variable`, `_type`,
      `_binding`, `_as_value`. Checks: a probe (bad arguments, an
      unresolvable host, a refused port, a peer that answers in plaintext,
      eight pending handshakes and a ninth refused, double and stale closes,
      a blocking and a non-blocking HTTPS exchange with `example.com`)
      prints what HEAD's C prints; ASan+UBSan clean; `leaks` reports the
      same 6 blocks as HEAD, all inside Security.framework
      (`CryptKit::FEEKeyInfoProvider`). Benchmark vs HEAD: an HTTPS GET of
      `example.com` retires 14.6–14.8 M instructions against 14.6–14.9 M
      (3 runs of 20); TCP request/reply and 1 GiB sent by offsets at parity
      (1,785–1,828 vs 1,773–1,834 ms, 398–399 vs 397–424 ms). Binaries (new
      vs HEAD, `__text`): tiny 308 both; `http_parse` 16,340 vs 16,244 (the
      TLS table's empty cells in `__init`); `http_listen` 148,476 vs
      151,504 (−2%, `send_at` gone); `zeli` 510,884 vs 509,132 (+0.3%);
      `http_https_live` 12,804 vs 7,252 (+5.5 KB: the client is about three
      times its C's size, from checked conversions, bounds checks and a
      retain/drop around each binding call that passes a module buffer).
      Follow-up: borrowed `&[]T` / `&mut []T` binding parameters would drop
      those retain/drop pairs from every binding (done in the next entry:
      `http_https_live` `__text` 12,804 → 11,860). Seed 5,155,152 →
      5,176,131 bytes, seed = stage 1 = stage 2. Suite: `compile_pass`
      (710), `compile_fail` (173), `zeus` (455), `golden` (135), `http`
      (39), `net_`, `c_ffi`, `os_`; every integration stanza listed for
      R5.1 green; `cross-check.sh` Linux and Windows 125/125 on both CPUs.
      **Runtime: 5,813 → 5,359** (−454: `net.c` 862 → 419, `net_rt.h`
      42 → 31).
- [x] Borrowed `#[c]` buffers (`&[]T` / `&mut []T`). Done 2026-11-15.
      §4 e2. A `#[c]` parameter may be `&[]T` or `&mut []T` of numbers and
      bools. The wrapper passes `->ptr` and does not retain or drop. An
      owned `[]T` still does both, so the "[]T argument released" case
      stays. A binding C writes is `&mut`; one that only reads is `&[]`.
      A borrowed buffer is a pointer, so it also goes through `...`
      unchanged (the Apple arm64 vararg path; `c_ffi`'s `vptr`). A borrow
      of a string, a number, a `[]string` or a fixed array is rejected
      (`compile_fail/c_attr_borrow`). Call sites auto-borrow a mutable
      place; an immutable one cannot be passed as `&mut`.
      `std:net`, `std:sys`, `std:async` and the raygui bindings take the
      borrow wherever C only needs the element pointer for that call.
      Public fns that own a buffer (`write_bytes`, `read_fd`) stay owned.
      No C deleted: `string_bytes_at` stays, because a string is not a
      buffer. **Runtime stays 5,359**, and `RUNTIME_LINE_BUDGET` stays
      5,359.

      This closes the R5.2 follow-up and the R2.4 note. `read_line` over
      30,000 lines (540,000 bytes, one `read` a byte) is syscall-bound:
      wall 371–377 vs 375–378 ms, user 0.08 s both. Instructions
      2.449 B vs 2.455–2.459 B, about 12 fewer a byte, which is the
      retain/drop. Cycles overlap (734–748 M vs 739–748 M).

      Behaviour: a probe (clock, mkdir, realpath, a TCP echo, poll, three
      `read_line`s) prints what HEAD prints. An HTTPS GET of
      `example.com` returns the same 931 bytes. ASan+UBSan clean on the
      probe, on `c_ffi` and on that GET (`detect_leaks` is unsupported
      on this Mac; `leaks` is the leak check). `leaks` on the probe is 0
      both. On the GET, 3 blocks / 96 bytes both, all
      `CryptKit::FEEKeyInfoProvider` inside Security.framework during
      `SSLHandshake` (the earlier 6 was a different probe). Benchmark vs
      HEAD: TCP request/reply 1,809–1,891 vs 1,779–1,880 ms (3 runs) and
      1 GiB sent by offsets 403–404 vs 399–431 ms, both moving
      6,400,000 and 1,073,741,824 bytes. HTTPS, per successful GET,
      14.7–15.0 M instructions against 14.6–15.3 M (6 runs of 20;
      17–20 connects succeeded). Self-emit 0.16–0.22 s vs 0.18–0.21 s.
      Binaries (new vs HEAD, `__text`): tiny 308 both, file 33,688 both;
      `http_parse` 16,340 both, file 70,432 both; `http_https_live`
      11,860 vs 12,804 (−944) and its file 38,976 vs 55,496 (one page);
      `http_listen` 147,328 vs 148,476 and its file 211,552 vs 228,112;
      `zeli` 512,296 vs 513,584, file 649,336 both; raygui 39,568 vs
      40,108, file 90,648 both. The HTTPS client's C has 49 retains and
      76 drops against HEAD's 65 and 92. Seed 5,176,131 → 5,179,265
      bytes, seed = stage 1 = stage 2. Checks: `c_ffi`, `c_attr` (18,
      including `c_attr_borrow`), `net_listen`, `sys_` (5), `async_`
      (4 pass, 2 rejected), `http` (39), raygui headless (3 frames, RAM
      growth 0). `wasm_smoke`, `wasm_trap`, `lsp`, `c_layouts`;
      `cross-check.sh` Linux and Windows 125/125 on both CPUs. A wasm32
      build of `zeus_async_rpc.loam` links.
- [x] R5.3 TLS on Linux: OpenSSL as `#[c]` bindings. Done 2026-11-16.

      **Compiler: binding libraries (§4 e3).** Per-target link flags did not
      need §4 c. A binding names its libraries:
      `#[c("SSL_new", "link -lssl -lcrypto")]`. The driver links them only
      when a kept binding that builds here names them, each once, in
      declaration order. Only `-lNAME` and `-framework NAME` are accepted
      (`compile_fail/c_attr_link_flag`). A module-wide `#![c_link]` could
      not do this, because it applies whenever any binding of the module is
      kept, and `std:net` cannot be split in two (imports cannot be
      cyclic). The libraries ride in the Fn node's unused `cbuiltin` slot,
      so no node grows. The SecureTransport bindings now name Security and
      CoreFoundation too. The driver's fixed `-framework Security
      -framework CoreFoundation` for every `net` and headless Zeus build is
      gone, so a macOS program that uses TCP but never TLS links only
      libSystem. That cuts start-up from 3.5 to 2.5 ms a run (200 runs,
      fork and exec included).

      **One TLS layer, two backends.** The slot table, the `getaddrinfo`
      walk, the blocking handshake with its three-idle-polls rule, the
      non-blocking state machine, and blocking and non-blocking write, read
      and close are now shared by Apple and Linux. Each backend gives seven
      ops over its opaque context: `tls_new`, `tls_shake`, `tls_put`,
      `tls_get`, `tls_buffered`, `tls_end` and `tls_free`. On Linux:
      - One process-wide `SSL_CTX` (`TLS_client_method`), with
        `SSL_VERIFY_PEER` against the default CA paths, a TLS 1.2 floor, and
        partial and moving writes (the contract `SSLWrite` gave the shared
        layer).
      - Per connection, an `SSL` with SNI (`SSL_ctrl` code 55, which is what
        the `SSL_set_tlsext_host_name` macro expands to) and
        `SSL_set1_host`.
      - A BIO whose read, write and ctrl are Loam callbacks (§4 e), so every
        `send` carries `MSG_NOSIGNAL`. `SSL_set_fd` would `write()` and
        raise `SIGPIPE` on a reset peer.
      - `ERR_clear_error` before each call, so `SSL_get_error` reads only
        that call's error.

      The `addrinfo` walk reads `ai_addr` at word 3 on Linux and word 4 on
      Apple, where `ai_canonname` comes first. `SO_SNDTIMEO` is per OS.

      **TLS only when used.** The TCP calls reach a TLS handle through four
      fn-value hooks (send, read, info, close). They answer as for a closed
      handle until the first TLS connect installs the client. With direct
      calls, `tcp_close` / `tcp_read` / `tcp_poll` kept every TLS binding,
      so every TCP program on Linux would have linked libssl. The hooks also
      merge the six Apple+Linux / Android+other TCP variants into one POSIX
      variant each.

      **Bug fixed on the way.** `tcp_poll` (readable) on a TLS handle now
      answers 1 when the TLS layer holds decrypted bytes (`SSL_pending`,
      `SSLGetBufferedReadSize`). The socket alone cannot show those bytes,
      so a poll could wait out its timeout with data ready. HEAD had this on
      macOS too.

      **Layouts and links.** `c_layouts` gains Linux `addrinfo` and
      `SO_SNDTIMEO`, and, when OpenSSL's headers are found (the system's or
      Homebrew's), every `SSL_ctrl`, mode, error and BIO code, the three
      BIO callback types (`-Werror=incompatible-pointer-types`, which gcc knows too),
      and that every symbol a binding links from OpenSSL is a declared
      function, not a macro (a missing header failed it once). This runs
      natively and with zig for x86_64 glibc and aarch64 musl.
      `cross-check.sh` links a stub `libssl` / `libcrypto` on Linux (zig
      ships no OpenSSL), made from the bindings' own `"link"` lists, so the
      rest of each link is still checked. CI's Linux jobs install
      `libssl-dev`; `install.sh` and the README name it.

      **Checks.** A probe covered bad ports, an empty host, an unresolvable
      host, a refused port, a wrong-name, an expired and a self-signed
      certificate (badssl.com), a blocking GET, double and stale closes,
      eight non-blocking handshakes plus a refused ninth, and a non-blocking
      GET. A second probe covered local peers: one answering the ClientHello
      in plaintext, a silent one, one resetting with the hello unread, and
      one closing at once. Both print what HEAD's C prints in three builds:
      HEAD, this change on SecureTransport, and this change with the
      backends swapped in a scratch copy so that OpenSSL (Homebrew 3.6)
      runs on this Mac. All four probe builds were ASan+UBSan clean.
      `leaks` reports 0 for OpenSSL. SecureTransport leaks only inside
      Security.framework (`FEEKeyInfoProvider`, `SSLRecordReadInternal`),
      as on HEAD, and the count varies from run to run. A TCP-only program
      keeps only the TCP bindings and links neither library. The emitted C
      of `http_https_live` compiles for x86_64 glibc and aarch64 musl and
      references 30 of the 31 bound OpenSSL symbols (it never polls, so
      `SSL_pending` drops out). **Not yet run on
      Linux:** the first native run is the selfhost CI job (`http`
      fixtures, `http_https_live`, on ubuntu x86_64 and arm64).

      **Benchmark vs HEAD** (release `-O1`, macOS). 100k TCP round trips
      plus 1 GiB sent by offsets: 8.15–8.57 G instructions against
      8.25–8.46 G, syscall-bound. An HTTPS GET of `example.com`:
      14.7–14.8 M instructions per successful GET against 14.9–16.8 M (3
      runs of 20; failed connects vary with the network). OpenSSL 3 on the
      same Mac: 22.2–22.4 M. The compiler emitting itself: median of 10,
      200 ms both.

      **Binaries** (`__text`, new vs HEAD):
      - tiny `fmt.println(42)`: 308 both.
      - `http_parse`: 16,532 vs 16,500 (the hook cells).
      - `http_https_live`: 13,244 vs 13,616.
      - `http_listen`: 146,060 vs 149,564, file 211,984 vs 228,256 (the
        server keeps no TLS code now).
      - a TCP-only program: 2,876 vs 3,052.
      - `zeli`: 507,764 vs 509,392.

      Seed 5,179,265 → 5,207,343 bytes, seed = stage 1 = stage 2.

      **Suite.** `compile_pass` (710), `compile_fail` (175), `zeus` (455),
      `golden` (135), `http` (39), `c_ffi`, `c_attr` (19), `net_`, `async_`,
      `sys_`; `selfhost_seed`, `wasm_smoke`, `wasm_trap`, `wasm_float`,
      `lsp`, `c_layouts`, `serve_native`, `serve_boot`, `serve_build`,
      `server_split`, `android_project`, `ownership_asan`, `cost`,
      `fresh_cache`, `modules`, `loader_cases`, `thread_tsan` and `watch`
      green; `cross-check.sh` Linux and Windows 125/125 on both CPUs.
      No C deleted: **Runtime stays 5,359.**
- [x] R5.4 Winsock: `std:net`'s TCP on Windows. Done 2026-11-16.

      **Approach.** The POSIX TCP logic is shared, widened from
      `#[os("posix")]` to `#[os("posix", "windows")]`. The `c_*` calls it
      makes get Windows variants with the same Loam signatures, each over a
      `ws2_32` binding:
      - `SOCKET` is 64 bits and is checked to fit an `int` handle below
        `TLS_BASE`.
      - Lengths are `int`; a send is capped at 1 GiB, and the callers
        already send the rest.
      - `closesocket`; `ioctlsocket(FIONBIO)` for non-blocking;
        `SetHandleInformation` so a spawned child does not inherit a socket
        (POSIX's close-on-exec).
      - A read timeout's `timeval` becomes Winsock's `DWORD` of ms.
      - `WSAGetLastError`, with `WSAEINTR` read as `EINTR`. `EWOULDBLOCK`
        stands for `EINPROGRESS`, since a non-blocking `connect` on Windows
        answers that.
      - `WSAStartup` on the first socket.
      - `c_poll` copies the `pollfd` set into 16-byte `WSAPOLLFD`s
        (`POLLRDNORM` / `POLLWRNORM`) and maps the results back. An empty
        set sleeps, as `poll` does, where `WSAPoll` would reject it.
      - No `SO_REUSEADDR` on Windows, where it would let another process
        bind the same port.

      Every binding names `"link -lws2_32"` (§4 e3), so only a program
      that opens a socket links it. The per-OS stubs that answered `-1`
      are now wasm-only. No TLS on Windows yet (R5.5).

      **Scripts.** `fixture-deps.sh` reads each binding's `#[os]` and
      `"link"` into one table. `win-bundle.sh` adds a fixture's Windows
      libraries to its `.deps`, and `runtime/net.c` when its C calls
      std:net (as the driver links it; the first CI run failed 9 links
      without it), and no longer leaves the socket fixtures out
      (125 go to Windows, was 104). `cross-check.sh` links MinGW's
      `ws2_32` on Windows and, on Linux, stubs only of the libraries Linux
      bindings name. `c_layouts` gains `x86_64-windows-gnu`: `SOCKET`,
      `WSAPOLLFD`, `sockaddr_in`, `WSADATA` within its 512-byte cell,
      `u_long`, `DWORD`, and every option, poll, error and `FIONBIO` code.
      The POSIX-only includes are guarded, and raylib's check is skipped
      there (its `Rectangle` and `CloseWindow` clash with `windows.h`).

      **What the first Windows runs found** (121 of 125 passed at first):
      - Winsock's `send` overcommits. A non-blocking send takes any size
        whenever the buffer has room, so a 3.2 MB reply left at once and a
        stalled reader was never dropped. Each send is now offered at most
        64 KiB (`http_listen`).
      - `server_stop` closed requests and the listener but not open streams,
        so a stopped server's stream clients waited forever. Fixed for every
        OS. `http_stream` now runs its server in-process (it spawned a child
        through `sh` and `kill`ed it) and fails against the old
        `server_stop`.
      - The analytics server's `wall_ms` read `gettimeofday` as two 64-bit
        words, but Windows' `timeval` is two 32-bit `long`s. It now reads
        `clock_gettime(CLOCK_REALTIME)` (`c_layouts` checks
        `CLOCK_REALTIME` and the `timespec` layout).
      - The HTTP server closed a connection right after its reply. With
        request bytes unread, or more on the way (the rest of a refused
        70 KB body, an h2c client's SETTINGS ACK), the kernel answers with a
        reset. Windows then drops the reply the client had not read yet
        (`www_analytics`'s 413, `http_listen`'s h2c). The server now does a
        lingering close on every OS: `net.tcp_shutdown` (new; `SHUT_WR` /
        `SD_SEND`), then reads and drops input until the client closes, for
        at most 2 s or the server timeout, on a list of its own
        (`Server.lingering`, capped at `max_conns`). A connection dropped
        for a timeout or an error still closes at once.
      - `win-run.sh` now runs fixtures headless, as `nob test` does
        (`http_min` served forever), caps each at 60 s, and shows a failing
        fixture's stdout.

      **Checks.** Native suites are unchanged: `http` (39), `net_`,
      `async_`, `zeus_async`, `zeus_stream`, `os_variants`, `c_layouts`,
      `wasm_smoke`, `wasm_trap`, `server_split`, `serve_native`,
      `serve_boot` and `lsp` green. `cross-check.sh` Linux and Windows
      125/125 on both CPUs. The Winsock code first runs on the Windows CI
      job (`win-run.sh`, PR #71). No C deleted: **Runtime stays 5,359.**
- [x] R5.5 TLS on Windows: SChannel as `#[c]` bindings (secur32). Done
      2026-11-16. Its first Windows CI run printed `tls ok`: badssl.com's
      four bad certificates refused, blocking and non-blocking GETs
      complete. Linux printed it on both CPUs too.

      **A third backend** of the shared TLS layer, which now builds for
      Windows too. SChannel does no socket I/O: the backend sends and
      receives itself, and SChannel only turns bytes into records and back.
      Its structs hold pointers, so each connection is one zeroed C block
      (`calloc`) whose address is the context the slot keeps. The block
      holds the `CtxtHandle`, the state and counters, the `SecBuffer`
      arrays and descriptors, the host name, a 64 KiB input area and a
      20 KiB output area. Loam reads and writes it a word at a time through
      `memcpy` bindings, as the R5.2 `addrinfo` walk does. In detail:
      - **Credential:** one per process, `AcquireCredentialsHandleA` with a
        `SCHANNEL_CRED`. `SCH_CRED_AUTO_CRED_VALIDATION` checks the chain
        and the host name; strong crypto, no default client credentials.
        TLS 1.2 only, since the legacy struct cannot enable 1.3
        (`SCH_CREDENTIALS` would).
      - **Handshake:** `InitializeSecurityContextA` as a step function, the
        same on blocking and non-blocking sockets. It flushes pending
        output, receives on `SEC_E_INCOMPLETE_MESSAGE`, keeps
        `SECBUFFER_EXTRA` leftovers (application data in the last flight
        too), and reads the record sizes once done.
      - **Writes:** `EncryptMessage` per record (header, data, trailer). A
        record the socket will not take whole is parked, and the caller's
        retry with the same bytes (OpenSSL's contract) finishes it and
        counts them then.
      - **Reads:** `DecryptMessage` in place. Plaintext the caller has not
        taken stays in the block, so `tcp_poll` sees it as buffered.
        `SEC_I_CONTEXT_EXPIRED` (close_notify) is the end.
      - **Close:** `ApplyControlToken(SCHANNEL_SHUTDOWN)` and one more call
        give the close_notify, sent once; then `DeleteSecurityContext`.
      - **Shared layer on Windows:** Winsock's `getaddrinfo` (after
        `WSAStartup`; it may be the first Winsock call), `connect` to an
        `ai_addr`, `recv` into C memory, `SO_SNDTIMEO`, and Windows'
        `addrinfo` (`ai_addr` at word 4, as on Apple).

      **Checks.** `c_layouts` gains, for `x86_64-windows-gnu`: `SecBuffer`,
      `SecBufferDesc`, `CtxtHandle`, `CredHandle`, `SCHANNEL_CRED` (80
      bytes, protocols at 56, flags at 72), `SecPkgContext_StreamSizes`,
      Winsock's `addrinfo`, and every status, buffer, request, credential
      and protocol code. New `tests/live/net_tls_live.loam`, kept out of
      the suite because it spends seconds on the network:
      - it must refuse badssl.com's wrong-host, expired, self-signed and
        untrusted-root certificates;
      - it must complete a blocking and a non-blocking GET of example.com;
      - closed handles must stay closed.

      CI requires its `tls ok` on both Linux CPUs (OpenSSL) and on Windows
      (SChannel). The Linux step replaces the one that required
      `http_https_live`'s `https ok`. That step's first run showed a real
      OpenSSL handshake on both CPUs, linking `libssl.so.3` (932 and 928
      bytes). On macOS it prints `tls ok` too (SecureTransport). Native
      `http` (39), `net_`, `async_`, `www_analytics`, `zeus_async`,
      `zeus_stream`, `c_ffi` and `c_layouts` green; `cross-check.sh` Linux
      and Windows 125/125 on both CPUs. No C deleted: **Runtime stays
      5,359.**
- [x] R5.6 TLS on Android through Java's TLS (JNI). Done 2026-11-16.
      On the yuga emulator (Android 14, arm64) it printed `tls ok`:
      badssl.com's wrong-host, expired, self-signed and untrusted-root
      certificates were refused, and a blocking and a non-blocking GET of
      example.com both completed.

      **A fourth backend** of the shared TLS layer. The engine is
      `javax.net.ssl.SSLEngine` in client mode, in `LoamTls`, which the
      compiler copies into the generated Gradle project. It uses the
      system trust store, SNI from the host passed to `createSSLEngine`,
      HTTPS endpoint identification, and TLS 1.2 at least. Loam owns the
      socket and the poll. `wrap` and `unwrap` turn bytes into records.
      Pending output is parked, and the caller's retry with the same bytes
      counts it. Plaintext the caller has not taken is kept, so
      `tls_buffered` reports it. The same contract as SChannel.

      Loam cannot call through JNI's function table, so the calls live in
      the existing Android host (`android.c`): `loam_android_tls_open`,
      `status`, `wrap`, `unwrap`, `pending`, `close` and `free`. They copy
      between caller-owned buffers. `std:net` binds them with
      `#[os("android")] #[c]`. No new runtime C file.

      Bionic's `addrinfo` matches Apple (`ai_addr` at word 4).
      `SO_SNDTIMEO` is 21 and `SO_RCVTIMEO` is 20, as on Linux. `nfds_t`
      is 4 bytes, so Android's `poll` takes a `u32` count. `getaddrinfo`
      and `freeaddrinfo` cover Android.

      **Checks.** `c_layouts` compiles the bionic asserts with NDK 26's
      aarch64 clang (zig ships no bionic headers): `addrinfo` is 48 bytes,
      `ai_canonname` at 24, `ai_addr` at 32, `ai_next` at 40, `nfds_t` is
      4, and the socket constants match. `android_project` finds
      `LoamTls.java`. NDK clang (`aarch64-linux-android34`) linked a Zeus
      program that calls `tls_connect` into `libzeus.so`, defining the
      seven `loam_android_tls_*` symbols. `gradle assembleDebug` compiled
      `android.c`, `LoamTls.java` and that program's `app.c`, and
      `installDebug` ran the checks above. Native `net_`, `http_` (39),
      `os_variants` and `c_layouts` are green. `cross-check.sh` is Linux
      and Windows 125/125 on both CPUs. `nob seed` and `nob selfhost`:
      seed = stage 1 = stage 2 (5,209,438 bytes). No runtime C added:
      **Runtime stays 5,359.** Hosts are 5,451 (`android.c` 437 to 635,
      +198). `LoamTls.java` is 299 lines and is outside that C count.

      The live APK was linked at minSdk 28. The generated project stays
      minSdk 26. A Gradle link at 26 stops on `posix_spawnp`, which bionic
      exports from API 28. That symbol is `std:sys`'s spawn helper. The
      TLS objects compiled, and the NDK android-34 link of the same
      objects succeeded.

**R6. The core: zero runtime.** What remains of `loam_rt.h`: allocation,
refcounts, owned strings, vec growth and copy-on-write, element and env drop
hooks, the trap arm (checked integer ops left with R6.1, the panic path
with R6.2).

- [x] R6.1 Checked integer ops emitted by codegen, only where used. Done
      2026-11-16. Every integer helper left `loam_rt.h`: checked `+ - * / %`,
      unary `-`, `<<` / `>>`, the `wrapping_*` and `saturating_*` builtins,
      narrowing conversions (`conv`, `convf`, `convu`, `sat`, `satu`) and the
      bounds check (`loam_idx`). Codegen notes each helper as it writes a call
      to it (a family and a width, one `[]bool` slot each). Once the unit is
      built, it writes a definition for every noted helper, in a fixed order,
      between the runtime header and the rest. The definitions are
      templates in `codegen.loam` (`int_helper_def`), the same C the header
      had, so a call still inlines into a compare and a branch. Numeric
      builtin calls carry a new IR bit (`I_NUMERIC_BUILTIN`), so codegen
      notes them too. The header waits in `PRELUDE` until the unit is
      built. The 64 KB streaming to stdout (`flush_big`) is gone: every
      caller sets a file `SINK`, so it never ran. `fatal` now writes no
      partial C.
      Gone with the helpers: six unsuffixed `int64_t` `wrapping_*` /
      `saturating_add` fns that nothing called (and their names in
      threadcheck's worker list), and the non-GNU fallbacks of the three
      `i64` ops: every supported compiler has the builtins, and the
      sized helpers had used them unguarded all along.
      **Bootstrap:** the old seed pastes whatever `loam_rt.h` is on disk but
      cannot write the helpers, so stage 1 was built by the old compiler
      with HEAD's std and runtime as its home. It then built itself, and
      `nob seed` wrote the seed.
      **Speed:** generated programs are unchanged, since the helpers are the
      same C. The compiler emitting HEAD's sources takes 160.5 vs 160.1 ms
      (median of 12, both whole-unit builds). Emitting its own sources takes
      163.3 vs 160.0 ms, because it compiles its own 230 new lines.
      **Binaries** (new vs HEAD): tiny `fmt.println(42)` 33,688 both, `nob`
      618,248 both, gallery 2,608,104 both with the same `__text`. The HEAD
      compiler's sources built by either compiler have the same `__text`.
      The new compiler's `__text` grows 5,668 bytes (+0.3%): its own
      template code.
      **Generated C:** a tiny program's falls from 103,869 to 90,901 bytes
      (−12.5%; it defines 6 helpers, where the header had over 200), and the
      HEAD compiler's from 5,208,498 to 5,200,497. Seed 5,208,498 →
      5,241,692 bytes (the new code), seed = stage 1 = stage 2. `-Wall`
      warnings fall (tiny 787 → 588, the compiler 2,013 → 1,851): no unused
      helpers.
      **Suite:** `compile_pass` (710), `golden` (135), `compile_fail` (175),
      `zeus` (455), `http` (39), `thread`, `async`, `net_`, `c_ffi`,
      `c_attr`, `sys_`, `os_`, `maya`, `raygui`; `selfhost_seed`,
      `wasm_smoke`, `wasm_trap`, `wasm_float`, `ownership_asan`,
      `thread_tsan`, `lsp`, `cost`, `fresh_cache`, `android_project`,
      `serve_native` green; `cross-check.sh` Linux and Windows 125/125 on
      both CPUs. **Runtime: 5,359 → 5,080** (−279).
- [x] R6.2 Panic and trap path: a Loam fn over `write` + `abort` bindings.
      Done 2026-11-16. `std:panic` (loaded into every program like
      `std:decimal`; `report` and `trap` are DCE C roots) does what
      `loam_panic` did: `fflush(NULL)` and `abort` as raw bindings
      (`#[os("wasm")]` variants: nothing to flush, and the wasm `abort` hook),
      `file:line: panic: msg` on stderr through `fmt.ewrite`, the message
      kept for the hosts (`record`), then the jump to the innermost arm
      (`unwind`) or abort. It allocates nothing (an out-of-memory panic comes
      through it), reads no module `let` (panics come before module init and
      on workers), and keeps no check that could panic again: the line is
      written by its own sign-free digit loop, which `ranges` proves.
      `loam_panic` / `loam_panic_str` / the web release's `loam_trap` are now
      two-line C trampolines that turn C strings into views, so no call
      site changes. What stays C: the `setjmp` arm (`loam_trap_guard`; it
      must run in the frame it returns to), the 512-byte message buffer the
      hosts read, and two one-line hooks over them.
      **Also deleted:** `loam_writev_fd` / `loam_writev_all`, which nothing
      called, with the Windows `writev` shim, `sys/uio.h` and wasm's
      `writev` and `wasm_inc/sys/uio.h`; and the wasm `fprintf`'s panic
      special case, which read `panic(msg)`'s length as the message pointer
      (a bug, now gone with the path).
      **Bug fixed on the way:** `--cost` counted every `for` loop's
      `i + 1` latch as an overflow trap, but codegen always wrote it plain
      (the Bin had no type, so no helper). It cannot overflow (the head
      tested `i < hi`), so the IR no longer marks it checked, and the `cost`
      stanza expects 0 traps, not 1. A first draft wrote the line through
      `decimal.ewrite_int`, which kept `magnitude_of` and `fill`'s sign
      checks in every program. Moving the sign into `fill`'s loop removed
      them but made negative numbers 7% slower, so `fill` is unchanged and
      the panic path has its own loop.
      **Checks:** a probe (overflow, index, division by zero, `panic` with
      an interpolated, a 700-byte and an empty message, narrowing, shift,
      stdout to a file and through a pipe) and a panic on a worker thread
      print the same stdout and stderr, with the same exit code, as HEAD's
      compiler; ASan+UBSan clean. `--int64-compat` caught a first draft
      whose `line: int` was 64 bits against the header's `int32_t` (`line`
      is `i32` now).
      **Speed:** negative integers to text (20M `buf.push_int` + 3M
      interpolations) 367 vs 366 ms (median of 7). Compile: a program with
      no imports 5.4 vs 5.2 ms (it now loads `std:fmt`), the compiler on
      HEAD's sources 165.3 vs 165.4 ms.
      **Binaries** (new vs HEAD): tiny `fmt.println(42)` 33,688 both with
      the same `__text` (the linker drops the uncalled path). A program that
      can panic carries the path as Loam where it called libc's `fprintf`:
      `__text` +700 to +830 bytes, while its file shrinks 32 to 64 bytes.
      Seed 5,241,692 → 5,249,370 bytes, seed = stage 1 = stage 2.
      **Suite:** `compile_pass` (710), `golden` (135), `compile_fail` (175),
      `zeus` (455; one run of five had one failure whose name I did not
      capture, and four clean runs followed), `http` (39), `thread`,
      `async`, `trap`, `boundary`, `decimal`, `c_ffi`, `c_attr`, `sys_`,
      `os_`, `maya`, `raygui`; `selfhost_seed`, `cost`, `fresh_cache`, the
      three wasm stanzas, `ownership_asan`, `thread_tsan`, `lsp`,
      `android_project`, the `serve_*` stanzas, `server_split`, `modules`,
      `loader_cases`, `api_freeze`, `one_surface`, the lint stanzas,
      `c_layouts` and `watch` green; `cross-check.sh` Linux and Windows
      125/125 on both CPUs. **Runtime: 5,080 → 5,006** (−74).
- [x] R6.3 Decide, with a measured prototype, how allocation, refcounts and
      vec buffers leave C: the minimal `unsafe` of §4 in a `std:core`
      module, or emitted by codegen per type, with no extra memory per value
      over today's layout. Done 2026-11-16. **Codegen emits them, per type,
      as `static inline` C. Not a called `std:core`.**
      The layout stays what the header has. A string is `ptr`, `len`, `own`.
      A vec is `ptr`, `len`, `cap`. The `i64` refcount is the word before
      the bytes, not a field of the value. A probe built that header with
      `malloc` and `raw_read` / `raw_write`: the word at `raw_addr(s) - 8`
      was the refcount, the sums of both vecs were `N * (N - 1) / 2`, and
      no value grew a field.
      **Speed**, Darwin arm64, `-O0` (what a dev build uses), median of 5.
      Pushing 20,000,000 `i32`s: the C helper 32 ms, the same pushes written
      out with `raw_write` 28 ms, a called `unsafe fn` around those ops 88 ms
      (2.75×). A `std:core` fn is a call at `-O0`. It matches the C only when
      codegen emits it `static inline`, which is the integer-helper shape
      from R6.1, so the helper is not a fn in the runtime at all.
      String retain is the other half. 2,000,000 retains and releases: the
      C atomic 16 ms, a non-atomic `raw_read` / `raw_write` of the same word
      4 ms inline and 2 ms as a call. The raw ops are a `memcpy`. They are
      not an atomic add, and a correct string counter has to be one, because
      a string can move to a worker. Emitting `__atomic_add_fetch` is
      codegen. A new raw op for it would exist only so the emitter could
      call it.
      Trusted count: templates in `codegen.loam` delete the helpers from
      `loam_rt.h` and add no `unsafe` lines. A `std:core` port would add
      those lines in the language library for the same behaviour. R6.4 moves
      one helper at a time this way. The runtime count is unchanged by the
      decision: **4,992**.
- [x] R6.4 Move them, one primitive per change, each fuzzed and
      ASan/UBSan-clean (S2) and no slower than its C twin. Done
      2026-11-16.
      Vec push landed 2026-11-16. `loam_vec_push`, `loam_vec_push_owned`
      and `loam_vec_push_take`, and their out-of-line slow paths, left
      `loam_rt.h`. Codegen writes the same `static inline` C, and only the
      variant the unit calls, beside the integer helpers. Reserve,
      copy-on-write and the refcount word stay in the header: the slow
      path still calls them. Nothing else in the runtime called push.
      **Speed**, Darwin arm64, `-O0`, 20,000,000 `i32` pushes: the header
      helper 70 ms, the emitted helper 60 ms (median user time of 5; every
      run after the first sat in 60–70 ms). **ASan+UBSan** clean on plain
      push, a shared buffer, string push, and a moved struct (`_take`).
      A program that never pushes loses the three functions from its C
      (84,430 → 81,834 bytes). One that only pushes `i32` keeps plain push
      and drops the other two (93,357 → 91,883). **Runtime: 4,992 → 4,934**
      (−58). **Suite:** 678 passed, 0 failed. **Seed:** 5,333,714 bytes,
      seed = stage 1 = stage 2.
      Vec pop landed the same day. `loam_vec_pop` and `loam_vec_pop_owned`
      left the header. Codegen writes the same `static inline` C, and only
      the variant the unit calls. The body still calls `loam_vec_unique` /
      `loam_vec_unique_owned`, which stay with the refcount word.
      **Speed**, same machine and flags, push then pop of 20,000,000 `i32`s:
      50 ms with the header pop and 50 ms with the emitted one (median user
      time of 5). **ASan+UBSan** clean on plain pop, a shared buffer, string
      pop, and a popped struct. A program that only pops `i32` keeps plain
      pop and drops the owned one (92,372 → 91,682 bytes). **Runtime:
      4,934 → 4,914** (−20). **Suite:** 678 passed, 0 failed. **Seed:**
      5,338,753 bytes, seed = stage 1 = stage 2.
      The core ops closed it the same day. `loam_vec_move`,
      `loam_vec_retain`, `loam_vec_drop_owned`, `loam_vec_of`,
      `loam_vec_unique_owned`, `loam_vec_reserve_owned`, `loam_move_ptr`,
      `loam_drop` and `loam_str_retain` left the header. Codegen notes each
      as it writes a call (`core_op`, one `[]bool` slot per op, like the
      integer helpers) and writes the same `static inline` C for those only
      (`core_op_defs`, before the pushes and pops, which call the two owned
      ops). `loam_str_retain` keeps gcc's `-Wstringop-overflow` pragma.
      **Deleted, not moved:** `loam_fn_move` and `loam_fn_drop` were
      unreachable. A fn type is always copy (`types.is_copy`) and never
      needs a drop (`types.needs_drop`), so `move_inst` and `drop_place`
      never wrote either. Their branches are gone. A closure env is a
      borrowed record, so the `free(f->env)` would have been a bug had it
      ever run. `loam_env_copy` / `loam_env_drop` had no caller anywhere.
      **Stays C, for now:** everything another runtime file still calls.
      `loam_vec_reserve` (and the `loam_vec_unique` it calls), `loam_new`,
      `loam_vec_new` are called from `zeus_plat.c`. `loam_vec_drop` is
      called from the wasm host. `loam_str_release` is called from
      `zeus_rt.h`, `zeus_plat.c` and the hosts. Moving them now would mean
      a second copy for separately built C. They leave with that C (R6.5,
      R6.6, R7).
      **Bootstrap:** as R6.1. Stage 1 was built against the old header,
      it wrote the seed, and `nob seed` then reported it unchanged.
      **Checks:** a probe (fn values, Box build/move/drop, a vec of
      strings copied and then pushed and popped through a shared buffer,
      a constant table, string copies into a vec) prints the hand-checked
      results under ASan+UBSan, and `leaks` reports 0 bytes. gcc-15 `-O2
      -Wall` reports no stringop or free-nonheap warning.
      **Speed**, Darwin arm64, default build, 2,000,000 rounds of a vec
      copy, a copy-on-write push of strings, a constant table and a Box:
      0.88 s with the header ops and 0.88 s emitted (median user time of
      7, the second of two rounds; the first round was 0.89 vs 1.00).
      Emitting HEAD's compiler sources takes 0.17 s with either compiler
      (both whole builds), and the two outputs have the same `__text`
      (1,568,440 bytes).
      **Binaries:** tiny `fmt.println(42)` 33,688 bytes with the same
      `__text` either way. Its C falls from 93,049 to 88,731 bytes. The
      probe's C falls from 110,577 to 109,859, and the bench binary is
      34,376 bytes either way. The new compiler's own `__text` grows by
      11,680 bytes, which is its template code. **Runtime: 4,914 → 4,745**
      (−169). **Suite:** 678 passed, 0 failed (25 s). **Seed:** 5,373,610
      bytes, seed = stage 1 = stage 2.

- [x] R6.5 `zeus_wasm_libc.c` reduced to nothing: what wasm still needs
      (memory growth, `memcpy`) is Loam or a wasm import. Done 2026-11-16,
      in two commits.
      The first took the file from **439 to 215 lines**. The
      inventory linked all 15 wasm programs in the repo (the wasm stanza
      apps, the Zeus examples, both galleries, `www`, `oob`). It stubbed
      the libc out to see which symbols any link needs. It also linked a
      `std:net` and an `http` program.
      **Deleted, unused:** `exit`, `atoi`, `calloc`, `strncmp`, `strcpy`,
      `strncpy`, `strchr`, `strrchr`, `printf`, `fflush`, and
      `wasm_inc/strings.h`.
      **`memcpy` / `memmove` / `memset` are instructions:**
      `wasm_inc/string.h` maps them to the clang builtins, and `loamc`
      passes `-mbulk-memory`, so each is one `memory.copy` / `memory.fill`,
      not a call into a byte loop. `-ffreestanding` stays (without it clang
      may rewrite `fprintf` and similar into calls nothing defines). Bulk
      memory is in every engine since 2021 (Safari 15).
      **No stdio on wasm:** `getenv`, `strcmp`, `fprintf`, `snprintf` and
      the `FILE` objects are gone. Their callers were `zeus_plat.c`. Its
      `getenv` reads are `__wasm32__`-guarded (wasm had no environment,
      so they always answered NULL). Its out-of-memory and
      handler-kind messages build their text and go out through
      `loam_ewrite_bytes`. The measured int is `plat_put_int`, not `%lld`.
      **Bug fixed:** wasm's `fprintf` printed its format string unformatted,
      so a handler-kind mismatch showed a literal `%d` where the ids belong.
      **Also deleted:** `loam_handle_wrong_type` in `loam_rt.h`, which
      nothing had called since the handle-key check moved to Loam (R3, R4).
      **Sizes** (dev builds unless marked): most modules shrink by 26 to
      1,400 bytes (`oob` 2,031 → 2,005, dracula 963,267 → 961,909). mune
      grows by 2,988 bytes (+0.3%) and `www` by 2,748 (+0.2%), and the
      `www` release by 2,102 (+0.14%): a small copy of known size is now
      inline loads and stores, not a call.
      **Checks:** all 15 programs plus the `std:net` and `http` ones link,
      dev and release. `wasm_smoke`, `wasm_trap` and `wasm_float` (420,026
      checks, 0 mismatches) pass. Suite 678 passed, 0 failed. Seed
      5,373,383 bytes, seed = stage 1 = stage 2. **Runtime: 4,745 → 4,511**
      (−234).
      **Then the file was deleted.** The rest was the allocator (`malloc` /
      `realloc` / `free` / `zeus_heap_used` over a fixed 64 MB array),
      `abort`, `memcmp`, `strlen`, `write`, `errno` and `__multi3`.
      Homebrew LLVM ships no wasm32 compiler-rt, and a checked `i64`
      multiply lowers to a `__multi3` call. Following R6.3, codegen now
      writes that C (`wasm_libc_defs`) when the unit is `--target wasm32`
      (`codegen.WASM`). Native units get none of it. In a shard build the
      head is every unit's precompiled header, so the code sits under
      `LOAM_SHARD_DEFS` and one unit defines it. `loamc` no longer links a
      libc file.
      **The heap grows.** It starts at `__heap_base` (after the 4 MB stack
      and the data) and grows the memory with `memory.grow`, at least 1 MB
      at a time. It does not reserve 64 MB in every module. The free-list
      design is the old one: a header and footer per block, `free`
      coalesces both ways, and `malloc` walks free blocks only. `malloc`
      now refuses a size over half the address space (the old one could
      wrap in `align8`). `zeus_heap_used` reports live bytes, 0 when empty
      (the old one counted the 8-byte reserved offset). The JS hosts
      rebuild their `memory.buffer` views on each access, so growth
      invalidates nothing they hold.
      **Fuzzed:** 20 seeds × 50,000 random `malloc` / `realloc` / `free`
      operations, as wasm under Deno, every block filled with a pattern
      and checked, plus one 80 MB block past the old ceiling. Every seed
      ends with the heap counter back at its start. The same run passes
      built with `-fsanitize=undefined -fsanitize-trap=undefined`.
      **Speed and memory**, an allocation-heavy plain wasm program (300
      rounds of 2,000 strings and 2,000 ints, copied on write and
      dropped) under Deno: 52.3–52.5 ms vs 52.4–60.5 ms with the old libc,
      and peak memory **1,089 pages (68 MB) → 81 pages (5.2 MB)**.
      **Size:** modules change by −543 to +2,433 bytes (`www` dev
      1,497,681 → 1,500,114, +0.16%; the `www` release is 1,456,208). The
      new `malloc` is 494 bytes of `-Oz` code, because it carries growth.
      `noinline` on the allocator, `memcmp` or `strlen` changed no byte,
      so it is not there.
      **Fixed on the way:** a non-Zeus wasm link passed a trailing `-x
      none`, and clang warned.
      **Checks:** all 15 wasm programs (dev, plus release and shard builds
      of `www`, the gallery and mune) and the `std:net` and `http` ones
      link. `wasm_smoke`, `wasm_trap`, `wasm_float` (420,026 checks),
      `build_exits`, `build_partial`, `serve_build`, `serve_boot`,
      `server_split` and `fresh_cache` pass. Suite 678 passed, 0 failed.
      Seed 5,410,759 bytes, seed = stage 1 = stage 2. **Runtime: 4,511 →
      4,296** (−215; −449 for all of R6.5).
- [ ] R6.6 `packages/loam/runtime/` deleted; `loader.loam` and `loamc.loam`
      no longer name it; `nob trusted` reports zero runtime lines.

**R7. Hosts** (`packages/zeus/hosts`, outside the runtime directory).

- [ ] R7.1 Layout, event routing and state out of `linux.c`, `android.c`,
      `wasm.c` and `ios.m`; toolkit calls stay native.
- [ ] R7.2 Then `mac.m` and `apple_draw.m`.

Not in scope: rewriting vendored libraries, raw pointers or `unsafe`
beyond the rules of §4, or moving the C compiler `cc` itself. Loam still
emits C, and `cc` stays the backend.
