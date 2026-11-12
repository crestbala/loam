# The runtime and packages in Loam

Moved out of [self-hosting-loam.md](self-hosting-loam.md) (§6) on
2026-11-12. **This is not part of the 100% self-hosting claim.** The
compiler and every tool are Loam already; the runtime they link (and every
program links) is the C this plan moves. It starts after self-hosting is
declared, and it is a security goal as much as a language one: see
[security.md](security.md).

**Goal**: every line of runtime C that is *logic* becomes Loam, so it is
checked by the borrow checker and bounds checks like any program. C is left
only where Loam cannot or should not go:

- the trusted core of the memory model;
- the calls into the OS and the GUI toolkits;
- third-party libraries that are vendored as they are.

This is a security goal first. Every line of C is trusted code that nothing
checks ([security.md](security.md) gaps 2–3), so the measure of progress is
**lines of trusted C**, counted by one `nob` command and recorded at each
step.

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
  sanitizer-tested. With such an `unsafe`, they may move to Loam too, as
  the last step.
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
3. **Less trusted C.** The count goes down, and the step records the new
   number here.

Order, smallest risk and most logic first:

- [ ] **R0.** The trusted-C counter in `nob` (core, bindings and shims,
      vendored; per file) and a budget for the core.
- [ ] **R1. Pure logic.** `zeus_key.c`, `batch_run.c`, and the fmt / str /
      conv / buf verbs left in `loam_rt.h`.
- [ ] **R2. `sys`.** Files, dirs, paths, stdin, spawn bookkeeping and watch
      over per-call bindings, on POSIX and Win32. The tests are the existing
      `sys` fixtures plus the Windows CI job.
- [ ] **R3. Async and threads.** The future table and the channel queues in
      Loam, keeping S0's typed cells; the mutex, condition variable and
      thread start stay core. This lands with T1 (ThreadSanitizer,
      [threads.md](threads.md)).
- [ ] **R4. Zeus platform.** The `zeus_plat.c` logic, the signal store first
      (keeping S0's typed cells), then frame, trace, traps, media.
- [ ] **R5. Net.** The socket state machine in Loam over socket and poll
      bindings. TLS stays a binding: SecureTransport on macOS. Linux and
      Windows need a TLS binding of their own, and `std:net` has no Windows
      port yet (self-hosting, "Next changes (pending)", housekeeping).
- [ ] **R6. Hosts.** The logic out of `linux.c`, `android.c`, `wasm.c`,
      `ios.m`, then `mac.m` and `apple_draw.m`; toolkit calls stay native.
- [ ] **R7. The core, reviewed.** Only the core, bindings, shims and
      vendored code remain. The core is fuzzed and sanitizer-clean (S2, T1),
      within its budget, and documented line by line in
      [boundary.md](boundary.md).
- [ ] **R8. The core in `unsafe` Loam, optional.** Only if an `unsafe` that
      meets the rules of §4 is worth it: core pieces move one at a time,
      each deleting its C, and each through the same gates as R1–R6, with
      the trusted-code count (C plus `unsafe` lines) going down. If no piece
      earns it, the core stays C and Loam has no `unsafe`.

Not in scope: rewriting vendored libraries, raw pointers or `unsafe`
beyond the rules of §4, or moving the C compiler `cc` itself. Loam still
emits C, and `cc` stays the backend.
