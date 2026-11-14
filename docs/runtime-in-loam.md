# The runtime and packages in Loam

Moved out of [self-hosting-loam.md](self-hosting-loam.md) (§6) on
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

- **The trusted core.** About 1,000 lines: allocation, refcount retain and
  release (atomic for strings), vec buffers and copy-on-write, element and
  env drop hooks, the panic and trap path, and checked integer ops where C
  builtins do the work. These are the primitives safe Loam is built *from*.
  Writing them in Loam needs raw memory access, which safe Loam does not
  have. Until an `unsafe` that passes the rules in §4 exists, they stay C:
  small, in one file, fuzzed ([security.md](security.md) S2) and
  sanitizer-tested. Under the 2026-11-13 target they move too, as the last
  step (R6): through that `unsafe`, or emitted by codegen. Staying C is no
  longer the end state.
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
  Loam; what remains is core.
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
- **`unsafe`, allowed only if it fits essentialism and minimalism.** Loam
  may gain an `unsafe` keyword when it removes more trusted C than the
  language surface it adds. The rules:
  - **Minimal.** One keyword marking a fn or a block, and the fewest raw
    operations the core needs: read and write a value at an address, offset
    an address, and convert between an address and a handle. No pointer
    types in safe code, no unchecked indexing, no inline C.
  - **Essential.** It lands with the first core piece that needs it, which
    must delete C and lower the trusted-code count (C plus `unsafe` lines)
    in the same step. No speculative operations.
  - **Contained.** Safe callers cannot tell it is there: an `unsafe` fn is
    private to its module, as a `#[c]` fn is, and its module exposes only
    safe wrappers. The compiler and its tools stay entirely safe Loam
    (self-hosting §2.7).
  - **Visible.** `loamc check --ffi` ([security.md](security.md) S3) lists
    every `unsafe` fn and block with the `#[c]` bindings, and the R0
    counter counts its lines as trusted code.
  - **Checked as C is.** Every `unsafe` module is fuzzed (S2) and
    ASan/UBSan-clean, and matches the C it replaces in speed (gate 2 below).

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
      and lowered by each step since (now **8,700**, after R1.4).
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
      now written in `boundary.md` (bindings are module-private, so each sits
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
- [ ] R2.2 Files and dirs: `read_file`, `write_file`, `io_error`, directory
      walks, paths.
- [ ] R2.3 Process: `exec`, `spawn` / `wait` and the spawn table, `env`,
      `exit`, `os`, `arch`, `cpu_count`.
- [ ] R2.4 stdin: `read_stdin` / `read_line` buffering.
- [ ] R2.5 `watch_*` bookkeeping over kqueue / inotify / Win32 bindings.
- [ ] R2.6 Win32 paths through the same bindings; the Windows CI job is green.

**R3. Async and threads** (lands with T1, [threads.md](threads.md)).

- [ ] R3.1 The future table in Loam, keeping S0's typed cells.
- [ ] R3.2 The channel queues in Loam; mutex, condition variable and thread
      start are `#[c]` pthread bindings, which a thread worker may call.

**R4. Zeus platform** (`zeus_plat.c`, `zeus_rt.h`).

- [ ] R4.1 The signal byte store as a `[]u8` arena with S0's typed cells.
- [ ] R4.2 Frame and trace logic.
- [ ] R4.3 Recoverable traps (needs R6.2's trap path).
- [ ] R4.4 Image sizes and media state.
- [ ] R4.5 The `loam_zeus_engine_*` trampolines are gone: hosts call the
      Loam exports.
- [ ] R4.6 `maya_plat.c` and `raygui_plat.c`: the bridge logic moves to
      `packages/maya` and `packages/raygui`; raylib calls become `#[c]`
      bindings of those packages. `maya_mac.m` moves to the maya package as
      a host file.

**R5. Net** (`net.c`, `net_rt.h`).

- [ ] R5.1 Socket and `poll` bindings; the connection state, buffering,
      timeouts and non-blocking connect in Loam.
- [ ] R5.2 TLS: SecureTransport as `#[c]` bindings on macOS, and a binding
      for Linux and Windows.

**R6. The core: zero runtime.** What remains of `loam_rt.h`: allocation,
refcounts, owned strings, vec growth and copy-on-write, element and env drop
hooks, panics and traps, checked integer ops.

- [ ] R6.1 Checked integer ops emitted inline by codegen
      (`__builtin_*_overflow` in the generated C), only where used.
- [ ] R6.2 Panic and trap path: a Loam fn over `write` + `abort` bindings.
- [ ] R6.3 Decide, with a measured prototype, how allocation, refcounts and
      vec buffers leave C: the minimal `unsafe` of §4 in a `std:core`
      module, or emitted by codegen per type, with no extra memory per value
      over today's layout.
- [ ] R6.4 Move them, one primitive per change, each fuzzed and
      ASan/UBSan-clean (S2) and no slower than its C twin.
- [ ] R6.5 `zeus_wasm_libc.c` reduced to nothing: what wasm still needs
      (memory growth, `memcpy`) is Loam or a wasm import.
- [ ] R6.6 `packages/loam/runtime/` deleted; `loader.loam` and `loamc.loam`
      no longer name it; `nob trusted` reports zero runtime lines.

**R7. Hosts** (`packages/zeus/hosts`, outside the runtime directory).

- [ ] R7.1 Layout, event routing and state out of `linux.c`, `android.c`,
      `wasm.c` and `ios.m`; toolkit calls stay native.
- [ ] R7.2 Then `mac.m` and `apple_draw.m`.

Not in scope: rewriting vendored libraries, raw pointers or `unsafe`
beyond the rules of §4, or moving the C compiler `cc` itself. Loam still
emits C, and `cc` stays the backend.
