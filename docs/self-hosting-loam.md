# Self-hosting `loamc`: what is missing

**Goal**: `loamc` written in Loam, built by the current C `loamc` (stage 0), then
able to build itself (stage 1 → stage 2, byte-identical C output). The
architecture in [loam.md](loam.md) stays the same: one Loam program in, one C
file out, `cc` links it. Self-hosting means the compiler is Loam. The runtime
(`runtime/loam_rt.h`, host shims) stays C *for the fixed point*; moving it and
the packages' runtime C to Loam is the next goal, planned in §6. Afterwards the
only C left is a small trusted core, the OS and GUI bindings, and vendored
third-party libraries.

**Loam remains a memory-safe language.** Ownership, borrowing, move semantics,
and the borrow checker are not optional. The self-hosted compiler must itself
be written in memory-safe Loam (no `unsafe` escape hatches that reintroduce
use-after-free, double-free, or data races in the compiler). The C runtime
continues to implement the safe abstractions; the language surface stays
safe by construction. Any new features added for self-hosting or the lifecycle
goals below must preserve this guarantee.

**Essentialism with minimalism.** The path to self-hosting keeps only what is
strictly necessary for a correct, memory-safe, self-hosting compiler. No
feature is added because it is interesting or because another language has it.
Every addition must justify itself by removing a concrete blocker or by
making the fixed-point proof simpler and smaller. Prefer arenas and integer
indices over new language machinery. Prefer a short, auditable compiler over
a large one. Optional goals (C FFI, embedded targets, energy/carbon hooks,
LSP, richer diagnostics) stay outside the critical path until the compiler
builds itself. The smallest set of changes that produces a byte-identical
fixed point is the correct set.

Beyond raw compile-time speed, the self-hosted compiler and the language it
emits must address the full software lifecycle while remaining memory-safe:

- **Hardware efficiency** — predictable allocation, cache-friendly layouts,
  and control over when work happens, without sacrificing ownership checks.
- **Developer ergonomics** — readable diagnostics, fast iteration, tooling
  that stays out of the way.
- **Real-time energy profiling** — ability to measure and attribute energy
  use at compile time and (optionally) at runtime.
- **Carbon-aware execution** — optional hooks so long-running builds or
  applications can query grid intensity / prefer lower-carbon windows
  without forcing it on every program.

The compiler today is ~26.6k lines of C11 in `packages/loam/src/`
(typecheck 5.3k, codegen 4.5k, driver 2.5k, IR 2.4k, LSP 2.0k, parser 2.0k,
borrowck 1.6k, …). `nob.loam` (3.3k lines) shows Loam can already carry a real
tool: files, processes, argv, env, directory walks. What it cannot yet carry is
listed below, ordered by how hard each one blocks.

Each item was checked against the compiler on branch
`feat/loam-reference-escape` (commit `19baeb0`) with a small probe program,
not inferred from docs.

**Platform target**: the self-hosted toolchain and its runtime shims must work
on Linux (x86_64, aarch64) and Windows (x86_64, aarch64) in addition to the
existing macOS path. WASM, iOS, and Android remain secondary. Memory-safety
guarantees must hold on all of these platforms.

**Embedded & robotics target**: Loam must also target embedded controllers
and single-board computers (Raspberry Pi, Arduino-class MCUs, and similar
ARM/RISC-V boards) so it can be used for robotics, motor control, sensor
fusion, and embodied AI agents. The compiler itself does not need to run on
the MCU; the language and a minimal runtime must.

**C FFI**: Loam must support a practical C foreign-function interface so that
existing C libraries and headers can be imported and called from safe Loam
code. This is required both for the self-hosted compiler (which still relies
on the C runtime and host shims) and for general interoperability (especially
hardware peripheral libraries on embedded targets).

## Definition of 100% self-hosted

Loam is **100% self-hosted** only when all of the following are true:

1. The compiler (`loamc`) is written entirely in Loam (no remaining C in the
   compiler itself; the runtime and host shims may stay C).
2. A stage-0 C `loamc` can compile the Loam `loamc` to produce stage 1.
3. Stage 1 can compile itself to produce stage 2, and stage 1 and stage 2
   emit **byte-identical** C for the compiler and for the full test suite
   (`compile_pass`, `golden`, `compile_fail`).
4. The self-hosted compiler passes the same differential tests as the C
   compiler (same diagnostics, same generated C).
5. The build works on at least macOS, Linux, and Windows from a fresh
   checkout that contains only the C seed (or committed generated C).
6. Memory-safety guarantees of the language are preserved in the Loam
   compiler (arenas + integer indices; no `unsafe` that reintroduces
   use-after-free or aliasing bugs inside `loamc`).
7. The compiler is the smallest one that satisfies 1–6. No extra language
   features, standard-library modules, or runtime machinery were added
   solely to make the port easier if a simpler alternative existed.

C FFI, embedded targets, energy/carbon hooks, and LSP are **not** required
for the 100% self-hosted claim. They are valuable extensions that can land
after the fixed point. Under the essentialist rule they stay out of the
critical path.

**Current status**: Not yet 100% self-hosted. The compiler and every tool
are Loam, the fixed point holds, and the build works from the seed on macOS,
Linux and Windows. Left (definition items 5 and 6; listed in full at the top
of "Next changes (pending)"):

- **A memory-safety hole in generated code.** `Box::new` of a value with
  owned fields leaves the box pointing at freed memory once it outlives the
  fn that built it ([box-and-init-ownership.md](box-and-init-ownership.md)).
- **A leak:** temporaries of module `let` initializers are never dropped.
- **The CI run on this work** (Linux, Windows) of P5 and the formatter port.

History: Phase 1 (§1.1, §1.4, the §1.3 `u64`
bugs), phase 2 (§1.2, the rest of §1.3, stderr / `mtime` / read errors
from §1.5, §2.3) and phase 3 (spawn/wait, stdin, paths, the Linux/Windows
runtime shim — §1.5, §1.6; cross-compiled, and run by the `selfhost` CI
workflow) are done. Phase 4 (the lexer in Loam, §4 step 3) and phase 5 (the
parser in Loam, arena AST, diffed via `--dump-ast`; §4 step 4) are done; the
`selfhost` workflow ran green on Linux x86_64/aarch64 and Windows x86_64.
Phase 6 (typecheck in Loam) is done and diffed against `typecheck.c`;
phase 7 (borrowck, threadcheck, boundscheck) is done and diffed through
`--dump-checked`. Phase 8 (IR lowering) and phase 9 (dead-code elimination
and the C emitter) are done: from a module list, the Loam compiler now writes
the same C translation unit as `loamc`, byte for byte. Phase 10a (the module
loader) and 10b (the driver: `compiler/loamc.loam`, check, emit C, `cc`) are
done: the Loam compiler builds itself, and the C it emits for itself matches
the seed's byte for byte. The fixed point runs green in CI on Linux and
Windows (§3). The Loam compiler is now `bin/loamc` on every OS, built from
the committed seed, and the C compiler is deleted (Next changes 4, P6 and
P7); the per-pass differential stanzas named below went with it. The
language server and the formatter are Loam programs too (P5, and Next
change 5), so `packages/loam/src` is gone: every tool is built by
`bin/loamc`, and the only C `cc` compiles directly is the seed and the
runtime. Next: the rest of "Next changes (pending)" at the end.

---

## 1. Blockers (a compiler cannot be written without these)

### 1.1 Recursive types crash or hang the compiler

A compiler is trees all the way down: `Expr` holds `Expr`s, `Type` holds
`Type`s. Today:

| Probe                                                                   | Result                                |
| ----------------------------------------------------------------------- | ------------------------------------- |
| `struct Node { kind: int, kids: []Node, name: string }`, push one child | **`loamc` segfaults** (exit 139)      |
| `struct Tree { left: Box<Tree>, v: int }` (declared, never used)        | **`loamc` hangs** (killed after 15 s) |
| `struct A { b: []B }` / `struct B { x: int }` (non-recursive nesting)   | works                                 |

Needed:

- [x] `[]T` field inside `T` (directly and through another struct).
- [x] `Box<T>` field inside `T`, plus an "empty" box: a recursive `Box` must be
      able to end. `Option<Box<T>>` works as a field; bare `none` needs no
      placeholder payload.
- [x] Copy/drop/size analysis that terminates on cycles. The hang was
      `struct_type_of` / `make_struct_inst` publishing a type only after its
      fields resolved; both now publish first, `type_is_copy` keeps a visited
      set, and a struct that holds itself *inline* (by value or through
      `[N]T`) is rejected as infinite-size before any later walk sees it.
- [x] Generated C that forward-declares every struct typedef before any
      definition.

Tests: `compile_pass/recursive_types.loam`,
`compile_fail/struct_infinite_size*.loam`.

Workaround if this slips: an **arena of flat structs** addressed by `int`
indices (`[]Expr` in one pool, children as `[]int`). That is how a fast
compiler would be written anyway, and it fits second-class references
(§2.1), but even the arena needs `struct Expr { kids: []int }` inside a
`[]Expr`, which works today.

### 1.2 No efficient string building

`codegen_c.c` emits megabytes of C (a Zeus app is ~2.9 MB). Today:

- `+` on strings is an error (`arithmetic requires int or float`).
- The idiom is a hand-written `cat(a, b)` that copies both into a fresh
  `[]int` and calls `string_from_bytes` (`nob.loam:84`). Appending N pieces
  is O(N²) bytes copied.
- Bytes are pushed into `[]int` (4 bytes per byte) in most std code;
  `[]u8` + `string_from_bytes` works and should become the norm.

Needed:

- [x] A builder: `std:buf` over `[]u8` — `push_str`, `push_bytes`,
      `push_int` / `push_i64` / `push_u64` (one memcpy each, amortized O(1)
      growth), `push(b, u8(c))` for a byte, `to_string` at the end.
- [x] Write a builder straight to a file: `sys.write_bytes(path, &b)`.
      Test: `compile_pass/std_buf_conv.loam`.

### 1.3 No number ↔ text conversion in the library

The lexer must parse literals and codegen must print them exactly.

| Need                                                                                             | Today                                                                                                                   |
| ------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------- |
| int → string                                                                                     | none in std (`fmt.write_int` only prints to stdout; `fmt.str` does not exist). `nob.loam` and `date.loam` hand-roll it. |
| string → int (dec, hex, octal, binary, `_` separators, > `i64` max as bits — `ASTF_WRAPPED_LIT`) | none                                                                                                                    |
| float → string, round-trip exact (`%.17g` equivalent)                                            | none (only `fmt.write_float` to stdout)                                                                                 |
| string → float (`strtod` equivalent, exact)                                                      | none                                                                                                                    |
| `u64` printing                                                                                   | **wrong**: `let x: u64 = 18446744073709551615; fmt.println(x)` prints `-1`; a `0xcbf2…` hash prints negative            |
| `u64(18446744073709551615)`                                                                      | rejected: "integer literal is too large" (the typed `let x: u64 = …` form works)                                        |

Needed:

- [x] `std:conv`: `int_to_str`, `i64_to_str`, `u64_to_str`,
      `u64_to_radix` / `i64_to_radix`, and `parse_int` / `parse_i64` /
      `parse_u64` / `parse_u64_radix` returning `Result` (sign, `0x`/`0o`/`0b`,
      `_` separators, overflow reported). Pure Loam.
- [x] Float formatting/parsing: `f64_to_str` is the shortest of
      `%.15g`/`%.16g`/`%.17g` that reads back exactly (C hook);
      `parse_f64` checks the syntax in Loam, then `strtod`.
- [x] Fix unsigned printing and the `u64(<max>)` literal-in-conversion case
      (`println` and `{{x}}` print a `u64` unsigned; a same-type integer
      conversion is a plain copy). Test: `compile_pass/u64_print.loam`.

### 1.4 Character literals

`'a'` is a lex error (`expected expression (got unknown)`). A hand-written
lexer is hundreds of byte comparisons; writing `c == 97` instead of
`c == 'a'` makes the self-hosted lexer unreadable and error-prone.

- [x] `'a'`, `'\n'`, `'\\'`, `'\''`, `'\x7f'` as an untyped integer literal
      (byte value; a `u8` in `u8` context). No `char` type needed. Also in
      `loam-fmt`, the tree-sitter grammar, and the VS Code highlighter.
      Tests: `compile_pass/char_literals.loam`, `compile_fail/char_literal_*`.

### 1.5 Process and file-system APIs the driver needs

`driver.c` / `compile.c` / `pkgresolve.c` use things `std:sys` lacks. These
must also be portable to Linux and Windows (CreateProcess / WaitForSingleObject,
`_stat`, etc.).

| C use                                                      | Purpose                                       | `std:sys` today                                                |
| ---------------------------------------------------------- | --------------------------------------------- | -------------------------------------------------------------- |
| `stat` → `S_ISREG` / `S_ISDIR`                             | module resolution                             | `exists`, `is_dir` ✓                                           |
| `stat` → mtime (`driver.c:159`)                            | runtime `.obj` cache freshness                | **missing**                                                    |
| `fork` + `waitpid` (`driver.c:479`)                        | parallel `cc` of runtime objects              | **missing** (`exec` is blocking shell only)                    |
| `system`/`popen` with exit status                          | run `cc`, `pkg-config`                        | `exec` + `exec_status` ✓ (stdout only; stderr only via `2>&1`) |
| `clock_gettime` (`LOAM_TIME`)                              | timings                                       | `async.now_ms` (i64) — OK if usable outside async              |
| `unlink`, `rename`, `mkdir`                                | build outputs                                 | ✓                                                              |
| writing diagnostics to **stderr**                          | errors must not mix with `--emit-c` to stdout | **missing**: `fmt` writes stdout only                          |
| `fread(stdin, n)` / line reads, unbuffered (`lsp.c:31-53`) | LSP JSON-RPC                                  | **missing**                                                    |
| exit with a code                                           | ✓ `sys.exit`                                  |                                                                |

Needed:

- [x] `sys.mtime(path) -> i64` (milliseconds; `-1` when missing).
- [x] `sys.stamp(path) -> string`: `"<sec> <nsec> <size>"` of a regular file,
      `""` otherwise; the `.fresh` build cache compares these (10c).
- [x] `fmt.ewrite` / `fmt.eprintln` (stderr; `eprintln` is `println`'s
      compile-time expansion with the stderr writers).
- [x] `sys.read_stdin(n) -> string` (exactly `n` bytes, fewer at EOF) and
      `sys.read_line()` (with its `\n`; `""` is EOF), unbuffered.
- [x] Spawn without the shell + wait: `sys.spawn(&argv) -> handle`,
      `sys.wait(handle) -> exit code`, `sys.run(&argv)`. `posix_spawnp` on
      POSIX, `_spawnvp` with `CommandLineToArgvW`-style quoting on Windows;
      up to 256 children in flight, so `cc` can run in parallel.
      Test: `compile_pass/sys_spawn.loam`.
- [x] Read a file with an explicit error: `sys.read(path) -> Result<string>`
      (and `sys.io_error()` after a `read_file`); an empty file is `Ok("")`.
- [x] Path separators and absolute-path rules that work on Windows: `/`
      everywhere on the Loam side (`sys.cwd()` normalizes), `sys.is_abs`
      knows drive letters and UNC, `sys.mkdir` skips the drive, and
      `sys.os()` names the platform.

### 1.6 Cross-platform runtime and host shims (Linux + Windows)

The existing runtime is oriented around macOS (Cocoa) + Linux X11 + WASM.
Self-hosting the compiler itself mainly needs a solid CLI path, but the
language must not regress.

Needed:

- [x] Windows host shim (`_WIN32` block in `loam_rt.h`): console I/O
      (`writev` over `_write`), process spawn, `mtime`, `mkdir`, replacing
      `rename`, binary stdin — MinGW-w64, no WSL.
- [x] Linux path that does not assume X11 for non-GUI tools: the runtime,
      `nob`, and the seed compiler build headless.
- [x] Consistent `sys` surface: `nob.loam` and every non-GUI `compile_pass`
      fixture build and link unchanged for x86_64/aarch64 Linux and Windows
      (`tools/cross-check.sh`, `zig cc`), and CI (`selfhost.yml`) runs the
      full suite on Linux x86_64/aarch64 and the non-GUI fixtures natively
      on Windows x86_64. Known gap: `std:net` has no Windows port (not
      needed to self-host).
- [x] Document which C compilers are supported and how `loamc` finds them:
      [platforms.md](platforms.md). MSVC `cl.exe` is out (GNU extensions in
      the generated C). The seed `driver.c` is deliberately not ported to
      Windows: Windows bootstraps from the committed generated C of the Loam
      compiler, whose driver uses `sys.spawn`.

### 1.7 C FFI: import C source and headers

Loam already emits C and uses bodyless functions as extern hooks into the
runtime. A usable language also needs a way to consume existing C libraries
directly.

Needed:

- [ ] `import "c:path/to/file.h"` (or equivalent) that parses a C header
      (or a restricted subset) and exposes its declarations as Loam
      signatures. Pointers become `*mut T` / `*const T` or opaque handles;
      ownership is explicit at the boundary.
- [ ] Ability to list `.c` files that should be compiled and linked with
      the generated C (build-system support in `nob` / the driver).
- [ ] Calling conventions and type mapping that stay memory-safe on the
      Loam side: no raw pointer arithmetic in safe code; `unsafe` blocks
      (if any) are required only at the exact FFI call site and are
      audited.
- [ ] Support for common patterns: opaque structs, function pointers,
      integer/enum types, and null-terminated strings (with safe wrappers
      in std).
- [ ] Works on Linux and Windows (MSVC-compatible headers where possible,
      or a documented subset).
- [ ] The self-hosted compiler itself can continue to call the existing
      `loam_rt` hooks without rewriting them in Loam.

This does not replace the C runtime; it makes the boundary explicit and
usable for third-party C code.

### 1.8 Embedded targets and robotics support

Goal: write Loam programs that run on resource-constrained controllers and
SBCs for robotics and embodied AI.

Needed:

- [ ] **Bare-metal / no-OS mode**. A minimal runtime that does not assume
      POSIX, heap beyond a fixed arena, or dynamic linking. Panic can be a
      simple halt or watchdog reset.
- [ ] **Toolchain support**. Cross-compilation with `arm-none-eabi-gcc`,
      `avr-gcc`, RISC-V toolchains, or the Arduino/PlatformIO ecosystem.
      `loamc --target=embedded` (or similar) emits C that those toolchains
      can link. Linker scripts and startup files are supplied by the user
      or a small board package.
- [ ] **Hardware access via C FFI** (§1.7). GPIO, I2C, SPI, UART, PWM, ADC,
      and timers come from existing C HALs (STM32 HAL, Arduino core,
      Raspberry Pi `pigpio` / `wiringPi` / `lgpio`, or vendor SDKs). Loam
      provides thin safe wrappers where possible.
- [ ] **Memory model**. No unbounded `[]T` growth by default; static
      allocation and fixed-size arenas preferred. Optional `no_std`-style
      subset of the standard library.
- [ ] **Real-time considerations**. Document interrupt safety (what the
      borrow checker can and cannot prove about interrupt handlers).
      Avoid hidden allocations in the hot path.
- [ ] **Board packages** (optional, later). Example crates or modules for
      common boards (Raspberry Pi GPIO, Arduino Uno/Nano, ESP32, STM32)
      that wrap the C HAL and expose a Loam API.
- [ ] **Embodied AI / robotics use cases**. Ability to express control
      loops, sensor fusion, and simple state machines without pulling in
      a full desktop runtime. Integration points for higher-level AI
      (on-device inference via C libraries, or off-board communication
      over serial/network).

The self-hosted `loamc` continues to run on a host machine; only the
generated programs target the embedded device.

---

## 2. Design costs to plan around (not bugs; the compiler must be shaped for them)

### 2.1 Second-class references → handles, not pointers

The C compiler is a pointer graph: `Type*` interned and compared by pointer,
`AstNode*` stored in scopes, symbol tables pointing at declarations, IR blocks
pointing at each other. Loam forbids references in struct fields, `[]T`, and
`Box<T>` ([reference-escape.md](reference-escape.md)). So the Loam compiler
must use:

- **Type table**: `[]TypeInfo`, a type is an `int` id; interning by a
  structural key string in `Map<int>` gives pointer-equality-as-int-equality.
- **AST arena**: `[]Node` per module, children as index lists, parent links as
  ints.
- **Symbols**: `Map<int>` from name → decl index; scopes as a stack of maps or
  a `[]Sym` with scope marks.
- **Parser**: cannot hold `&string` source in its struct; hold the owned
  source (`string` is Copy/refcounted, so that is cheap) plus a cursor.

This is a rewrite in shape, not a line-by-line port of the C.

### 2.2 No sum types

AST/IR nodes in C are `kind` + `union`. Loam has C-style enums and no
per-variant payloads (deliberate, see [downsides.md](downsides.md)). Plan:
one wide struct per node family with a `kind` enum and the union of fields,
or a `kind` + index into a per-kind side table. `switch` on the kind works
(no exhaustiveness check — add a `default: panic(...)` everywhere).

### 2.3 Hash maps are string-keyed and `i32`-hashed

`std:map` is `Map<T>` with `string` keys only (`hash_str` returns `int`).

- [x] Integer-keyed map: `std:intmap` (`IntMap<T>`), the same
      table as `std:map` with an int key and a multiplicative hash. Kept a
      separate module on purpose: a generic key would need `==`/hash on a type
      parameter, i.e. new language machinery.
- [x] `Map<[]int>` / `IntMap<[]int>` work, and a value is mutated in place
      through its slot: `push(m.entries[map_find(&mut m, k)].value, x)`.
      Test: `compile_pass/std_intmap.loam`.

### 2.4 Memory and speed

- Capturing closures are never freed (known env leak). The compiler should
  avoid per-node closures: use `switch`-based visitors, not callback walkers.
- `[]T` copy-on-write: assigning a big vector then pushing to either copy
  duplicates it. Pass `&mut []T` everywhere in hot paths.
- `+ - *` trap on overflow and every index is bounds-checked unless proven.
  Fine for correctness; expect the Loam `loamc` to be slower than the C one
  at first. Target: stay within ~3× on `examples/zeus/gallery` check time.
- `int` is `i32`. Byte offsets in a multi-MB output and 64-bit literal values
  need `i64`/`u64` explicitly (see [numeric-widths.md](numeric-widths.md)).

### 2.5 Error handling without unwinding

`panic` aborts; there is no recovery. The C compiler collects diagnostics and
continues. The Loam one must do the same explicitly: a `Diag` list threaded
by `&mut`, `Result<T>` + `?` for I/O, and "error node" kinds in the AST so
parsing continues after a syntax error.

### 2.7 Memory safety is non-negotiable

The self-hosted `loamc` must be written entirely in safe Loam. This means:

- All AST, IR, and symbol-table structures use arenas + integer indices
  (or `Box`/`[]T` that the borrow checker can reason about) rather than
  raw pointers.
- No `unsafe` blocks that could introduce use-after-free or aliasing bugs
  in the compiler itself.
- The borrow checker, move checker, and drop analysis that exist today
  must continue to work (and eventually be implemented in Loam).
- Generated C remains correct with respect to the ownership model; the
  runtime continues to uphold the safety contracts.

Self-hosting must not become a reason to weaken the language's safety
guarantees. The same ownership and borrow rules apply on embedded targets;
the runtime simply implements them with a smaller allocator and no OS.

Raw speed is necessary but not sufficient. The self-hosted toolchain and
generated programs should be able to expose energy and carbon signals
without mandatory overhead.

Needed (can land after the fixed-point, but design the hooks early):

- [ ] Compile-time cost model already exists (`--cost`). Extend it with
      estimated energy (rough joules per stage) when the host provides
      power samples (Linux RAPL, Windows energy estimation APIs, or a
      simple user-supplied coefficient).
- [ ] Optional runtime hook `sys.energy_now() -> Option<i64>` (microjoules
      or similar) and `sys.carbon_intensity() -> Option<int>` (gCO₂/kWh)
      so long-running programs can decide whether to batch work.
- [ ] No forced instrumentation. Programs that do not call these APIs
      pay nothing.
- [ ] Document how a user can feed grid data (local file, environment
      variable, or a tiny std helper) so carbon-aware scheduling stays
      explicit and under user control.

---

## 3. Bootstrapping process (tooling, not language)

- [x] **Stage 0 seed**: keep the C compiler buildable as the seed, or commit
      the generated C of the Loam compiler (`loamc.c`) so a fresh checkout
      builds with only `cc` — as `nob.sh` already does for `nob`.
      Must produce working binaries on Linux and Windows. *Committed as
      `bootstrap/loamc.c` (4.0 MB, no absolute paths; the compiler finds std
      and the runtime from its own path).* `nob seed` regenerates it; it lags
      the sources and is refreshed only when it can no longer build the
      current compiler. On every OS `./nob.sh` builds it into `bin/loamc0`,
      which builds `bin/loamc` from `packages/loam/compiler` (Next changes 4,
      P6). `selfhost_seed` checks that `cc` builds it, that it builds the
      current compiler, and that the result reaches the fixed point. The
      Windows job ran green on it (run 43065909932, `9a6afcd`).
- [x] **Fixed-point check** in `nob`: C-`loamc` builds Loam-`loamc` (stage 1);
      stage 1 builds itself (stage 2); stage 1 and stage 2 must emit
      byte-identical C for the compiler and for every test in `compile_pass`
      / `golden`. Run the check on at least one Linux and one Windows CI
      runner. *Done in `nob` (`selfhost_driver`):* seed and stage 1 emit the
      same C for every `.loam` file in the repo, and stage 1 and stage 2 the
      same C for the compiler, so stage 2 is the same program as stage 1.
      CI: see Windows CI below. *Since the C compiler was deleted (Next
      changes 4, P7) the check is `selfhost_seed`:* the committed seed builds
      stage 1, stage 1 builds stage 2, and all three emit the same C for the
      compiler.
- [x] **Deterministic output**: no pointer-address ordering, no hash-order
      iteration in emitted C (iterate maps by insertion order or sorted keys).
- [x] **Differential testing**: for each `tests/compile_pass`, `golden`, and
      `compile_fail` file, both compilers must produce the same C / the same
      diagnostics. This is the main safety net during the port.
- [x] Decide what moves and what stays C. *Decided 2026-11-12:* every tool
      moves. `lsp.c` became `compiler/lsp.loam` (P5) and `fmt.c` became
      `compiler/loamfmt.loam` (Next change 5); both are deleted, with the
      rest of the C front end. What stays C: the committed seed
      (`bootstrap/loamc.c`, generated by the Loam compiler, so not
      hand-written C), and the runtime, host shims and vendored libraries,
      which move or stay by the plan in §6.
- [x] **Windows CI**: at minimum a GitHub Actions or equivalent job that
      builds the C seed, runs the self-host stages, and executes the
      language tests. *Green since run 42804061060 (`260ab67`):* `selfhost.yml` runs the suite
      on Linux (x86_64, aarch64; it includes `selfhost_driver`, the fixed
      point) and the fixtures natively on Windows from C emitted on Linux.
      `tools/win-selfhost.sh` then builds the Loam compiler on Windows from
      its Linux C with MinGW gcc, checks that it emits the same C as Linux
      for itself and every bundled fixture, builds itself through its own
      driver (stage 2, same C again), and builds and runs every fixture. It
      cross-compiles and links for x86_64 and aarch64 Windows (`zig cc`); the
      box is ticked once the job has run green. For this the loader splits
      `PATH` / `LOAM_PATH` on `;` on Windows, absolute-path tests use
      `sys.is_abs` (so `C:/…` std paths are cut to their file name in
      `#line`, as `/…` ones are), and `.gitattributes` checks text out with
      LF everywhere: the runtime header is pasted into the C, and a CRLF
      checkout would change its bytes.

---

## 4. Suggested order

1. **Language fixes**: recursive types (§1.1), char literals (§1.4),
   `u64` printing (§1.3). Each gets `compile_pass` tests.
2. **Std + cross-platform + C FFI**: string builder (§1.2), number conversion (§1.3),
   stderr, `mtime`, int-keyed map (§2.3), Windows/Linux path and process
   surface (§1.5, §1.6), and the C header import / `.c` linking support (§1.7).
3. **Port the lexer** to Loam; differential-test its token stream against
   `lexer.c` over every `.loam` file in the repo.
4. **Port the parser** (arena AST), diffed against the C parser through an
   AST dump (`--dump-ast`). (`loam-fmt` is token-based, so it waits; see §3.)
5. **Port typecheck → borrowck → boundscheck → IR → codegen**, one stage at
   a time, with the C stages behind as a fallback via an AST/IR dump format
   if a mixed pipeline is needed.
6. **Driver** (module loading, package resolution, `cc` invocation). Make
   sure it works headless on Linux and native on Windows.
7. **Fixed point** (§3), then make the Loam `loamc` the default and keep the
   C one only as the seed. Add Linux + Windows CI. *Done; the seed is the
   Loam compiler's own committed C (`bootstrap/loamc.c`), and the C compiler
   is deleted (Next changes 4, P6 and P7).*
8. **Energy / carbon hooks** (§2.6) — optional APIs and `--cost` extensions.
9. **Embedded runtime & board packages** (§1.8) — bare-metal subset, cross-toolchain
   examples, and first robotics wrappers (after the host self-host is solid).
10. LSP last.

---

## 5. Threads and security (after the fixed point)

Neither is needed to call the compiler self-hosted, and both follow the
essentialist rule. Every item names the probe or test that closes it. The
exceptions are S1 and S2 (checker soundness, and the trusted C under it). Each
hole they find is fixed in both compilers at once, as the port did, so they
run alongside the remaining self-hosting work.

### 5.1 Threads: what exists

Checked on 2026-11-11 against `std/thread.loam`, `src/threadcheck.c` /
`compiler/threadcheck.loam` and `runtime/loam_rt.h`:

- `thread.spawn(cb)` runs a closure or a named fn on a new detached pthread.
  `thread.chan<T>(cap, init)` makes a bounded FIFO with blocking `send` /
  `recv` and non-blocking `try_send` / `try_recv` / `ready`, guarded by a
  mutex and two condition variables in the runtime.
- The Send discipline is checked at compile time, in both compilers. A
  spawned closure may capture only plain data: `int`, `float`, `bool`,
  `string`, `Chan<T>`, and structs of those. `[]T`, `Box`, `fn` and borrows are
  refused, and `#[no_send]` UI handles (`Signal<T>`, `Node`, `Future<T>`) are
  refused. The spawned code and everything it calls may not read or write a
  module global or call the C seam, except a pure allowlist (string
  plumbing, the clock, sleep, the channel ops). This is the proof that there
  are no data races: shared mutable state is unreachable from a worker.
- String refcounts are atomic, which is why a `string` is Send. Vec refcounts
  are not, which is why `[]T` is not.
- On wasm, `spawn` does nothing and channels are inert. The program still
  runs, on one thread.

Limits:

- Bulk data cannot cross threads. A `[]T` is not Send, so a worker gets its
  input as scalars or strings and returns results one message at a time.
- A worker cannot do I/O. Every `sys.*` call is off the allowlist (some of
  its hooks keep global state, such as `sys.io_error` and the spawn table),
  so a file read, a process run or a socket happens only on the main thread.
  The HTTP server is one thread.
- There is no join and no thread handle. A caller learns a worker is done
  only from a message, or from `thread.running()` for all workers at once.
- No sanitizer has checked the runtime's thread code. The 15 thread
  fixtures prove what the checker refuses, not that the channel and
  refcount C is race-free.
- No mutexes or atomics in the language. This is deliberate: channels plus
  the Send rule are the whole model, and that stays.

Plan, smallest first:

- [ ] **T1. ThreadSanitizer stanza.** Build the thread fixtures and a small
      stress program (N workers, M messages each, strings in both
      directions) with `-fsanitize=thread`, and fail on any report. It is
      cheap (seconds) and covers the C that the Send proof trusts.
- [ ] **T2. Send a `[]T` by move.** `send(c, v)` already consumes `v` in
      borrowck. Make a vec of Send elements Send when it is *moved* into a
      channel or a capture: the runtime takes the buffer if its refcount is
      1 and copies it otherwise, so the two threads never share a buffer
      and no refcount needs to be atomic. Test: `compile_pass/thread_vec_move.loam`
      (round trip, and a shared copy on the sender stays intact), plus
      `compile_fail` cases for a use after the move and for a non-Send
      element.
- [ ] **T3. I/O on workers.** Make the per-call state of the `sys` hooks
      thread-local (`sys.io_error`, `exec_status`) and give the spawn table
      its own lock. Then allow, hook by hook, the ones that touch no other
      shared state: `read`, `write_bytes`, `stamp`, `run`, `spawn` / `wait`.
      Each hook added to the allowlist gets a T1 stress case.
- [ ] **T4. A multi-core HTTP server**, without new language surface: N
      processes sharing one listening socket (`SO_REUSEPORT` on Linux and
      macOS, a handed-over socket on Windows). Each process stays
      single-threaded, so handlers keep their globals, and a crash takes down
      one worker, not the server. Threads would need T3 plus moving
      handler state off globals; do that only if processes measure worse.
- [ ] **T5. Join, only if T3/T4 need it**: `spawn` returns a `#[no_send]`
      handle and `thread.join(h)` blocks until it is done. A channel does this
      today, so this item stays empty unless a real program needs more.
- Not planned: a multi-threaded compiler. Every pass already matches or
  beats its C twin on one thread, and the build time is `cc`, which already
  runs in parallel through `sys.spawn`.

### 5.2 Security: what holds and what does not

"Bulletproof" is not a property any language has. The goal is narrower and
checkable: **no memory-safety bug reachable from safe Loam**, and every other
risk is on a short, explicit list with a test or a documented boundary.

Holds today (probed on 2026-11-11 with a built program, not taken from the
docs). Each of these traps with a message instead of corrupting memory:

- `+ - *` overflow;
- an out-of-range integer conversion (`int(i64(3000000000))`);
- division by zero;
- a shift past the width;
- an index out of bounds;
- out of memory (every runtime `malloc` / `realloc` is checked).

Also in place:

- Ownership, moves and borrows are checked. References are second-class, and
  the language has no `unsafe` and no null.
- `sys.spawn` / `sys.run` take an argument list, with no shell.
- Vendored packages are pinned by checksum in `loam.lock`.

Two holes reachable from safe Loam were found while writing this section
(2026-11-11), each by a program that compiled cleanly and then broke memory:

- **Fixed: a `#[c]` binding could be called from any module.** `sqlite` and
  `crypto` bind raw C that takes an address as an `i64` (`free`, a `memcpy`
  from a column pointer). An importer could call `crypto.c_free(i64(12345678))`;
  the program checked and then aborted in `free`. A `#[c]` fn is now private
  to the module that declares it. A call by name, through
  `import { c_fn }`, as a method, or as a fn value from anywhere else is an
  error: "#[c] fn 'c_free' is private to module 'crypto'; call one of its
  safe wrappers instead". Both compilers report it identically
  (`compile_fail/c_attr_private.loam`, with `tests/mod/c_binding.loam`), and
  no program in the repo called another module's binding. The trusted code
  of a package is now exactly its `#[c]` modules and their C sources.
- **Fixed: a handle could be forged at another element type.** `Chan<T>`,
  `Future<T>` (`std:async`) and `Signal<T>` (Zeus) are plain `{ id: int }`
  structs whose `T` no field uses, and the runtime kept each cell's bytes by
  id, untyped. `Chan<string> { id: pairs.id }` over a channel of
  `struct { i64, i64 }` read both words back as a string's pointer and
  length: `s.len` was the chosen 64, and `s[3]` read address 4099 and
  crashed. A forged `Future<[]string>` over a `[]int` future read 32 bytes
  out of an 8-byte buffer, silently. Each cell now records a type key and
  every access checks it (S0 below). The fix is in the runtime and the
  codegens, not the language: apps rely on `Signal { id: 0 }` as a
  placeholder (53 uses), and Zeus builds signals outside the module that
  declares them.

Open (2026-11-12): **`Box::new` of a value with owned fields dangles**
once the box outlives its fn: `Box::new(Node { name: "n{{n}}" })`
returned from a fn reads a freed string. Codegen copies the value into the
box without taking references (`Op.Alloc`), and a box's drop is shallow, so
the two hid each other. Both halves and the fix are in
[box-and-init-ownership.md](box-and-init-ownership.md). It is item 0 of
"Next changes (pending)".

Gaps, by how much they matter:

1. **The checkers are not proven sound.** The port found five holes, each
   fixed in both compilers:
   - a borrow of a global did not survive a call that grows it;
   - `[]T` copy-on-write was missed when the buffer had spare room;
   - a move out of a global was accepted;
   - `&` of a temporary reached IR lowering;
   - a dropped temporary leaked when pushed.

   There are likely more. `nob fuzz-borrow` exists (ASan over generated
   programs) but is run by hand, and only on borrowck.
2. **Trusted C.** `runtime/loam_rt.h` and the host C (~7k lines in
   `loam_rt.h`, `net.c` and `zeus_plat.c` alone) carry every safety
   guarantee. The generated C trusts them, and nothing fuzzes them.
3. **FFI is trusted code.** A `#[c]` binding or a `#![c_source]` file can
   break every guarantee. Since 2026-11-11 only its own module can call a
   binding, so the trust ends at that module's wrappers, but nothing yet
   lists those modules for review (S3).
4. **`sys.exec` runs through the shell.** A caller that interpolates input
   into the command has a shell injection. (The compiler's one call is a
   constant `pkg-config` line.)
5. **Every trap is a denial of service.** A `panic` aborts the process, so
   one bad request kills a server. A stack overflow ends as a bare
   `SIGSEGV` from the guard page. That is memory-safe, but it prints no Loam
   message, and deep input (nested JSON, a deep expression) can trigger it.
6. **Unbounded memory growth.** Capturing closures are never freed (§2.4),
   so a long-running server that makes one per request grows until it is
   killed.
7. **Hash flooding.** `std:map` hashes with unkeyed FNV, truncated to 26
   bits. A program that keys a map by untrusted input (headers, JSON object
   keys) can be pushed to quadratic time.
8. **No hardening of the binary.** Native builds use `-O1` with no
   `-fstack-protector-strong`, `-D_FORTIFY_SOURCE`, or full RELRO / PIE
   flags. These are defence in depth for the trusted C and FFI code
   (gaps 2–3), not for safe Loam.
9. **Supply chain of the seed.** Next change 1 commits the compiler's
   generated C as the seed. Nobody reviews 3.9 MB of C by eye, so its trust
   must come from the fixed point: anyone can regenerate it from the Loam
   sources with the previous seed and compare bytes.

Plan, in order:

- [x] **S0. Typed handle cells.** Done. Each channel, future and signal
      cell records a type key when it is made or first bound. Every send,
      try_send, recv, pop, load, store, bind and change test passes the key
      it expects, and a mismatch panics: "channel (future, signal) handle
      used at the wrong type". One compare per access; no app changes.
      - The key is FNV-1a (32-bit, never 0) over the type's spelling: its C
        type, with what C erases spelled in (the element of `[]T`, `[N]T`
        and `Box<T>`, a fn's signature, a handle's `T`). Without that,
        `[]int` and `[]string` share `loam_vec`, and that forgery breaks
        memory too. A type is resolved once in its generic instance and
        then walked as it is: zeus records a `get<[]T>` instance (`T` bound
        to `[]T`, emitted but never called), and resolving at each level
        unrolled it forever.
      - Both codegens emit the key beside the `sizeof` they already pass
        (`handle_key` in `codegen_c.c` and `codegen.loam`), so the
        stage-1 = seed gates still hold. The runtime's own int-signal paths
        use `LOAM_HANDLE_KEY_INT`. A signal cell no bind has reached (the
        `Signal { id: 0 }` placeholder) still loads as zeros at any type.
        Wasm channels are inert and hold nothing to check; they gained the
        `_owned` send stubs, which were missing.
      - Tests: `compile_pass/handle_forge_{chan,future,signal}.loam`. A
        `<name>.trap` beside a fixture makes `nob` expect a panic whose
        stderr holds the file's text (the `trap` job, the `oob` job
        generalized).
      - Cost, measured (median of 5–6 runs): a Zeus frame is unchanged
        (`zeus_anim_bench`: 110 µs per frame before and after). Loops that
        do nothing but handle ops pay the compare, about 0.75 ns an access:
        2M channel send/recv pairs 116 → 120 ms, 2M future store/load pairs
        74 → 77 ms, 500k string-signal set/get pairs 129 → 132 ms. A build
        with the compares disabled ran at the old speed, so this is the
        compare, not the extra argument. The check sits inside
        `loam_ch_get`: a separate wrapper was not inlined at `-O1` and cost
        a second call per op. Codegen on dracula (240 keyed calls): Loam
        0.13 s, C 0.23 s, same C.
- [ ] **S1. Soundness fuzzing in CI.** Grow `fuzz-borrow` into a generator
      that also covers moves, globals, closures, threads and vec growth,
      and run the generated programs under ASan + UBSan. Keep a fixed seed
      range as a quick stanza, and a long run on a schedule. Every hole it
      finds becomes a `compile_fail` or `compile_pass` fixture, fixed in
      both compilers. Write the borrow rules in [reference-escape.md](reference-escape.md)
      as a numbered list, each with its fixtures, so a missing case is
      visible.
- [ ] **S2. Fuzz the trusted C.** Small libFuzzer (or AFL) drivers for the
      runtime entry points that take outside bytes: `std:conv` parsing,
      `parse_f64` / `strtod`, `str` slicing, UTF-8 (`std:unicode`),
      `std:json`, the HTTP/1 and HTTP/2 parsers, and `read_line` /
      `read_stdin`. All under ASan + UBSan. The fuzzers are dev tools, not a
      suite cost; the corpus found becomes regression fixtures.
- [ ] **S3. Mark the FFI boundary.** No new keyword: a `#[c]` fn or a
      `#![c_source]` module is listed by `loamc check --ffi` (module, symbol,
      source file), and `loam.lock` records which packages use FFI, so a
      review can find all trusted code in one command.
- [ ] **S4. Shell-free by default.** Deprecate `sys.exec` for new code in
      favour of `sys.run` / `sys.spawn` with a stdout capture
      (`sys.run_output(&argv) -> Result<string>`). Port the compiler's one
      call. Keep `exec` for scripts, documented as "runs a shell".
- [ ] **S5. Hardened native builds.** Release builds add
      `-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -pie` and
      `-Wl,-z,relro,-z,now` where the linker supports them (Linux; the macOS
      and MinGW equivalents per [platforms.md](platforms.md)). Measure the
      cost with `passbench` and a Zeus app first; dev builds keep their
      flags.
- [ ] **S6. Stack overflow with a message.** An alternate signal stack and a
      `SIGSEGV` handler that checks the fault address against the guard page
      and prints `panic: stack overflow` before aborting (the same on worker
      threads). On Windows, the `EXCEPTION_STACK_OVERFLOW` filter. Recursive
      parsers in std (`std:json`) get a depth limit that returns an error.
- [ ] **S7. Keyed map hash.** Seed `std:map`'s hash per process from the OS
      random source, so an attacker cannot predict collisions. Emitted C
      stays deterministic, because the compiler never iterates a map
      (Fixed-point and tooling). Check the cost with `passbench`.
- [ ] **S8. Fix the closure env leak** (§2.4) before recommending Loam for
      long-running servers. Then any per-request allocation is bounded by
      the request.
- [ ] **S9. Trap containment for servers**, with no unwinding (§2.5): the
      T4 process-per-worker model already confines a trap to one worker,
      whose parent restarts it. Document this as the answer to "a panic
      kills the server" instead of adding recovery to the language.
- [ ] **S10. Reproducible seed.** A `nob` check that regenerates the
      committed seed C from the Loam sources with the previous seed and
      fails on any byte difference (this is the fixed point, run against the
      committed file). Combined with S1/S2, it makes the seed trustworthy
      without a line-by-line review.

---

## 6. Runtime and packages in Loam (after the fixed point)

**Goal**: every line of runtime C that is *logic* becomes Loam, so it is
checked by the borrow checker and bounds checks like any program. C is left
only where Loam cannot or should not go:

- the trusted core of the memory model;
- the calls into the OS and the GUI toolkits;
- third-party libraries that are vendored as they are.

This is a security goal first. Every line of C is trusted code that nothing
checks (§5.2 gaps 2–3), so the measure of progress is **lines of trusted C**,
counted by one `nob` command and recorded at each step.

### 6.1 What is C today

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

### 6.2 What stays C, and why

- **The trusted core.** About 1,000 lines: allocation, refcount retain and
  release (atomic for strings), vec buffers and copy-on-write, element and
  env drop hooks, the panic and trap path, and checked integer ops where C
  builtins do the work. These are the primitives safe Loam is built *from*.
  Writing them in Loam would need raw pointers and `unsafe`, which the
  language does not have and will not get (§2.7, "Essentialism"). Keeping
  them small, in one file, fuzzed (§5.2 S2) and sanitizer-tested is the
  safer choice.
- **OS and GUI calls.** `read`, `write`, `open`, `posix_spawn`, `poll`,
  sockets, `pthread`, the Win32 equivalents, Cocoa / UIKit (Objective-C),
  JNI, X11, and wasm imports. Each becomes a thin binding: one `#[c]` fn per
  call where the types fit (numbers, `string`, `[]u8`; §1.7), and a few-line
  C shim where they do not (`char **argv`, structs, Objective-C messages).
  No logic lives in a shim.
- **Vendored libraries stay as they are.** Rewriting sqlite or monocypher in
  Loam is not essential and would trade audited code for new code. They
  stay behind `#[c]` modules, private to those modules since 2026-11-11
  (§5.2), with fuzzed wrappers (S2).

### 6.3 What moves to Loam

Most of the code above is logic over bytes and ints, which Loam writes
safely today:

- **`loam_rt.h` verbs**: fmt, str, conv and buf (the integer halves are Loam
  already), path handling, directory walks, `sys.read` / `write` error
  handling, the spawn table, `read_line` buffering, watch bookkeeping, the
  future table and the channel queue logic. The kernel call under each one
  stays a binding; the lock and condition-variable calls stay in the core.
- **`zeus_plat.c`**: the signal byte store (a `[]u8` arena that keeps the
  typed cells of §5.2 S0), the frame and trace logic, trap boundaries
  and media state. Calls into the host stay bindings.
- **`net.c`**: connection state, buffering, timeouts and the non-blocking
  state machine. The socket and `poll` calls and the TLS library stay
  bindings.
- **`zeus_key.c`**: all of it (tables and mapping).
- **`batch_run.c`**: the batched-test runner, a plain Loam program.
- **`zeus_wasm_libc.c`**: it shrinks as the code that needed libc moves to
  Loam; what remains is core.
- **Hosts**: layout, event routing and state move to Loam; the toolkit calls
  stay native. `mac.m` is the largest (2,207 lines) and goes last.

### 6.4 What the language needs first

- **§5.2 S0 (typed handle cells), done.** Handles are how Loam code refers
  to runtime state, so they had to stop being forgeable before more runtime
  moves behind them.
- **Opaque handles from bindings.** A binding that returns a C pointer gives
  an `i64` today, and the wrapper module must never let it escape. Make
  that checkable with no new syntax: the wrapper keeps pointers in a
  module-private table and hands out typed S0 handles. Add a lint for a
  `#[c]` module whose public fn returns or takes an `i64` that came from a
  binding.
- **FFI listing (§5.2 S3)**, so the trusted-C count is one command.
- No `unsafe`, no raw pointers, no inline C. If a piece cannot be written
  without them, it belongs in the core, and the core gets a line-count
  budget.

### 6.5 Order and gates

Each step moves one area, deletes its C, and must pass all of these before
it lands:

1. **Same behaviour.** The full suite and the self-hosting stanzas pass; the
   draw goldens are byte-identical; `win-selfhost` and the Linux jobs stay
   green.
2. **Same speed or better**, area by area, as with the compiler passes: the
   area's hot path is benchmarked against its C twin, and a Zeus frame and
   `passbench` do not regress. Binary size is checked too. Loam code goes
   through DCE, so unused runtime should drop out, where the pasted header
   cannot.
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
      thread start stay core. This lands with T1 (ThreadSanitizer) from §5.
- [ ] **R4. Zeus platform.** The `zeus_plat.c` logic, the signal store first
      (keeping S0's typed cells), then frame, trace, traps, media.
- [ ] **R5. Net.** The socket state machine in Loam over socket and poll
      bindings. TLS stays a binding: SecureTransport on macOS. Linux and
      Windows need a TLS binding of their own, and `std:net` has no Windows
      port yet (Next change 6).
- [ ] **R6. Hosts.** The logic out of `linux.c`, `android.c`, `wasm.c`,
      `ios.m`, then `mac.m` and `apple_draw.m`; toolkit calls stay native.
- [ ] **R7. The core, reviewed.** Only the core, bindings, shims and
      vendored code remain. The core is fuzzed and sanitizer-clean (S2, T1),
      within its budget, and documented line by line in
      [boundary.md](boundary.md).

Not in scope: rewriting vendored libraries, adding raw pointers or `unsafe`
to the language, or moving the C compiler `cc` itself. Loam still emits C,
and `cc` stays the backend.

---

## Summary table

| #   | Missing                                                             | Kind         | Blocks self-hosting?                |
| --- | ------------------------------------------------------------------- | ------------ | ----------------------------------- |
| 1.1 | Recursive struct types (`[]T` / `Box<T>` in `T`) -- crash/hang today | compiler bug | Yes (or force arena design)         |
| 1.2 | String builder / O(1) append / write bytes to file                  | std          | Yes (performance)                   |
| 1.3 | int/float <-> string conversion; `u64` prints signed                | std + bug    | Yes                                 |
| 1.4 | Character literals `'a'`                                            | lexer        | Practically yes                     |
| 1.5 | stderr, `mtime`, stdin reads, file read errors, spawn/wait          | std:sys      | Partly (stderr yes; spawn/stdin no) |
| 1.6 | Linux + Windows runtime/host surface for the compiler               | runtime      | Yes for multi-platform claim        |
| 1.7 | C FFI: import `.h` headers and link `.c` files                      | language + driver | No for pure self-host; yes for interop |
| 1.8 | Embedded / bare-metal targets (RPi, Arduino, robotics)              | runtime + toolchain | No for pure self-host; yes for robotics |
| 2.1 | References in data structures                                       | design       | No -- use ids/arenas                |
| 2.2 | Sum types                                                           | design       | No -- kind + wide struct            |
| 2.3 | Int-keyed map                                                       | std          | No, but big perf win                |
| 2.4 | Closure env leak, CoW copies, overflow/bounds cost                  | runtime/perf | No                                  |
| 2.5 | No unwinding                                                        | design       | No -- explicit diagnostics          |
| 2.6 | Energy profiling & carbon-aware hooks                               | lifecycle    | No for first self-host; yes for the stated goal |
| 2.7 | Memory safety of the compiler itself (arenas, no unsafe)            | design       | Yes -- must remain a memory-safe language |
| 3   | Seed, fixed-point, determinism, differential tests, multi-OS CI     | tooling      | Yes, to call it "self-hosted"       |
| 5.1 | Threads: TSan, `[]T` by move, I/O on workers, multi-core server     | runtime + std | No (the Send rule already rules out data races) |
| 5.2 | Security: forged handles (fixed, S0), soundness fuzzing, fuzzed runtime C, FFI listing, hardening, keyed hash, stack-overflow message | tooling + runtime | S1–S2 alongside; rest after the fixed point |
| 6   | Runtime and packages' runtime C in Loam (trusted core + bindings + vendored stay C) | runtime + std | No for self-hosting; yes for the memory-safety goal |

---

## Remaining work for 100% self-hosting

These are the concrete items that must be finished before Loam can be called
100% self-hosted. Everything else (C FFI, embedded, energy/carbon, LSP) can
come later. Apply the essentialist rule: implement the smallest change that
unblocks the next item; do not add language features or library surface
beyond what the checklist requires.

### Language & std fixes (must land first)
- [x] Recursive types work without crash or hang (§1.1)
- [x] Character literals `'a'`, `'\n'`, etc. (§1.4)
- [x] Correct `u64` printing and literal parsing (§1.3)
- [x] String builder with O(1) append + write-bytes-to-file (§1.2)
- [x] `int`/`u64`/`float` ↔ string conversion in std (§1.3)
- [x] `fmt.eprintln` / stderr (§1.5)
- [x] `sys.mtime` and file-read that returns an error (§1.5)
- [x] Portable process spawn/wait and path handling for Linux + Windows (§1.5, §1.6)
      — built for both via `tools/cross-check.sh`, run on both in CI

### Compiler port
- [x] Lexer in Loam, differential-tested against `lexer.c`:
      `packages/loam/compiler/lexer.loam` (+ `tokdump.loam`) matches
      `loamc --dump-tokens` token for token (kind, span, start/end position)
      on all 917 `.loam` files in the repo and 400 fuzzed byte soups (NUL,
      high bytes, unterminated strings/comments). Gated by the `selfhost_lex`
      host stanza in `nob test`. Speed: 2 MB in 0.084 s vs 0.098 s for C.
- [x] Parser in Loam (arena AST), differential-tested via an AST dump:
      `packages/loam/compiler/parser.loam` (+ `ast.loam`, the `[]Node` arena:
      kind enum, one wide struct, children as index lists, `Error` nodes so a
      failed parse never leaves an invalid index; `astdump.loam`, the printer)
      matches `loamc --dump-ast` (`src/ast_dump.c`, which documents the
      format) on all 921 `.loam` files: trees, diagnostics, and exit status,
      `compile_fail` included. Also matched on ~1,500 truncated or
      line-deleted copies (~930 with parse errors) during the port. Gated by
      the `selfhost_parse` host stanza. Speed: on par with C (0.018 s vs
      0.020 s parse + dump of a 250 KB file).
- [x] Typecheck ported
      - Typecheck (phase 6). 6a done: `loamc --dump-modules`
        (what the C loader resolved; loading and package resolution stay C
        until the driver phase) and `--dump-typed` (every module's tree
        with types, C names, resolved targets and call markers, then the
        mono / struct-instance / global tables); the Loam side has
        `types.loam` (type arena, `type.c`'s naming byte for byte),
        typecheck slots on the node arena, `tcdump.loam` (parses the listed
        modules and replays the loader's edits) and the
        `selfhost_typecheck` stanza.
      - 6b–6f done: `compiler/typecheck.loam` ports all of `typecheck.c`
        (the core checker, generics and monomorphization, closures and
        captures, Zeus call sugar, codecs and attributes) and matches
        `--dump-typed` on all 511 loadable entry files (`compile_pass`,
        `compile_fail` diagnostics included, `draw_golden`,
        `examples/language`): trees, mono / struct-instance / global tables,
        diagnostics and exit status. The `unported` escape is gone and the
        gate is strict: any difference fails. (19 files C itself cannot
        load are counted apart.) The port surfaced a codegen bug: a borrow
        of an indexed place passed beside an argument that grows the same
        vec (`push(NODES[n].kids, new_node())`) took the address first and
        wrote through it after the reallocation; such borrows are now
        lowered last (`compile_pass/borrow_arg_realloc.loam`).
      - Typecheck speed: the Loam checker beats C on a Zeus entry (0.42 s
        vs 0.60 s for `--dump-typed`); the gate caches results by checksum
        of both binaries and every input, so a warm run re-checks nothing
        that did not change (~25 s, from ~70 s).
      - The port surfaced a memory-safety hole: a borrow of a global did
        not survive a call that grows the global, another module's global
        was not tracked, and a reference into a global could be returned.
        Fixed in `borrowck.c` (reference-escape.md, rules 5–6, phase 9);
        no existing program needed a change.
- [x] Borrowck, threadcheck, boundscheck ported (phase 7). 7a:
      `loamc --dump-checked` (`--dump-typed` after threadcheck, borrowck
      and boundscheck: their diagnostics, exit 3, and node flags) and
      `tcdump --checked`; the gate worker takes the dump mode. 7b:
      `compiler/boundscheck.loam` matches on all 396 entries C checks
      cleanly (78 index-safe marks on a Zeus entry, none without the pass).
      7c: `compiler/threadcheck.loam` matches all 15 thread fixtures (the 9
      it rejects: diagnostics and exit 3). `threadcheck.c` now counts its
      errors with `loam_error_count`, so `--dump-checked` (no capture) sees
      them too.
      - 7d: `compiler/borrowck.loam` ports `sema/borrowck.c` (ownership and
        moves, loans and their paths, reborrows, reference escape, globals
        across calls, drop marks). With the other two passes it matches
        `--dump-checked` on all 518 loadable entries: trees, the moved /
        needs-drop / index-safe flags, diagnostics and exit status, the 63
        `compile_fail` entries C rejects after typecheck included.
      - The strict `selfhost_check` host stanza gates it (any difference
        fails); it shares the gate, worker and result cache of
        `selfhost_typecheck`, keyed by dump mode. A one-character change to
        a borrowck diagnostic in the Loam port fails it.
      - 7e, speed: every Loam pass now beats its C twin on a Zeus entry
        (`zeus_show`, per pass, best of repeated runs): threadcheck 1.5 ms
        vs 3.5, borrowck 35.5 ms vs 49.7, boundscheck 1.1 ms vs 3.6; the
        whole `--dump-checked` run 0.27 s vs 0.38 s. They started at 15, 115
        and 14 ms. Measured, not guessed — a sampling profile per step, and
        each change kept only if it moved the number:
        - The AST arena is split hot/cold: `Node` (in `NODES`) is the 56
          bytes every walk reads (kind, flags, children, resolution, type),
          `NodeInfo` (in `INFO`, same index) the rest; nodes are built whole
          as `NodeBuild` and stored by `ast.add`. The same walk over a
          Zeus entry's 353k nodes took 14.3 ms at 376 bytes a node and
          3.2 ms at 56: a tree walk is bound by the bytes it streams.
        - Codegen, for every Loam program: a string read out of a place and
          only compared (`BINDINGS[i].name == name`) takes no reference
          (`load.borrow` in the IR); a return drops only the locals some
          path to it could have set; and an owning local is declared
          `= {0}` only where a read or drop may reach it before it is
          surely assigned (a recursive `check_expr` zeroed 37 locals per
          call, now 4).
        - Runtime: `push` is an inline fast path (owned buffer with room:
          one element copy of a known size) with growth and copy-on-write
          out of line.
        - Port source: list walkers read `kids` in place instead of copying
          the vec, `types.is_copy` stops rebuilding its walk stack, borrowck
          memoizes Copy per type and empties its bindings arena per fn
          (peak memory 325 → 232 MB).
      - The speed work surfaced a memory-safety hole: a push onto a `[]T`
        copy un-shared the buffer only when it had to grow, so two copies
        with spare capacity wrote the same slot (`let b = a; push(a, x);
        push(b, y)` gave `a[1] == y`, and an owning element was overwritten
        unreleased). `loam_vec_reserve*` now take their own buffer first
        (`compile_pass/vec_cow_push.loam`).
      - 7f, memory: peak memory of `tcdump --checked` on that entry went
        from 237 MB to about 63 MB, below C's 65 MB (median of 10 runs,
        `/usr/bin/time -l`). The dump is byte-identical, every gate matches,
        and the run got a bit faster (0.37 s vs 0.39 s; C 0.64 s):
        - The dump streams out in 64 KB pieces (`dump.flush_big`) instead of
          building its 28 MB text and copying it before the write.
        - `NodeInfo` keeps only the fields most nodes use (88 bytes). Text,
          docs, C names, call cells and captures live in `NodeExtra`
          (`EXTRA`), made only for the ~5% of nodes that set one.
        - No node per struct-literal field. A `StructLit`'s `kids` are the
          values, and the names sit in one pool, `ast.LIT_NAMES`, as C keeps
          a `FieldInit` array in the literal. This removed 97k `Init` nodes;
          260k nodes remain.
        - All literals share one names pool instead of a `[]string` each,
          because a small vec starts at 8 slots (200 bytes), which was 9 MB
          over 43k literals.
        - Typecheck appends a literal's default fields in one step. One at a
          time, each default made an `Option` literal in between, which moved
          the literal's names to the end of the pool again: quadratic in the
          field count.
        - Arena growth was not the cause, so chunked arenas are not needed.
          Sampling the footprint every 0.2 ms shows it rising steadily to its
          peak at the end of the run, with no spike when an arena doubles. A
          large `realloc` frees the old block as it moves, and pages past
          `len` are never touched, so reserving up front could not help. The
          arenas end over 99% full.
- [x] IR and codegen ported
      - IR lowering (phase 8). 8a done: `loamc --dump-ir` checks as
        `--dump-checked` does, lowers to IR (no DCE, as `--emit-ir`), and
        prints everything codegen reads from the IR (`ir_dump` in `ir.c`):
        per fn its signature, closure id, env local, captures and init
        module; per local its type, param / global / C name and drop / zero
        flags; per instruction its operands, place (with each step's type),
        constants, callee, args, operator, cast mode, overflow and
        borrowed-load flags, template-default fields, constant tables, type
        and source line; per block its terminator and switch table. Then
        `verify ok` / `bad`. Types and strings are spelled as in
        `--dump-checked`. Deterministic; all 320 `compile_pass` entries dump
        and verify.
      - 8b–8c done: `compiler/ir.loam` ports all of `ir.c`: places, calls
        and their builtins, control flow, drops at exits, closures, `__init`
        fns, generic instances (recording new ones as `ir.c` does), borrowed
        string loads and the `= {0}` analysis, and the verifier.
        `irdump.loam` prints it; `tcdump --ir` runs it. It matches
        `--dump-ir` on all 518 loadable entries, and the strict `selfhost_ir`
        host stanza gates it (same gate, worker and cache as
        `selfhost_check`, keyed by dump mode).
      - The IR is arenas with int ids, like the AST. `FNS`, then per fn a
        range of `LOCALS` and of `BLOCKS`, per block a range of `INSTS`.
        Places, call arguments, template defaults, int tables and switch
        tables sit in pools, not a small vec per instruction. A fn is
        lowered into a reused scratch list, linked per block because
        lowering moves between blocks, then copied out block by block.
      - 8d, speed: the lowering pass now beats `ir_lower` on a Zeus entry
        (41 ms vs 54 ms, best of 7; it started at 92 ms). The whole
        `--dump-ir` run takes 0.38 s and ~75 MB, against 0.49 s and ~114 MB
        for C. Each change was profiled and kept only if it moved the number:
        - Struct literals match their fields to the struct's in one pass,
          searching from the last match, instead of comparing every pair
          of names. Zeus props structs have ~66 fields.
        - Instructions hold no string: callee names and string constants
          sit in a `TEXTS` pool. A global's local holds its `GVARS` index,
          not copies of its name and C name. A module's globals are found
          through a name map, not pushed on every fn's scope stack.
        - `needs_drop` is memoized per type. Bitsets use shifts, not checked
          division. The drops-at-exits analysis tracks only the locals that
          own something (a local's drop depends on its bit alone), and the
          dataflow loops read instructions in place.
      - The port surfaced a leak in `ir.c`. The exit-drop analysis only
        visited a block once something flowed into it. A fn with no params,
        in a module with no globals, whose entry block touches nothing
        (`fn f() { while … }`) never visited the blocks after it, so its
        exits dropped nothing. Every reachable block is now visited at least
        once, in both compilers
        (`compile_pass/drop_after_empty_entry.loam`; it leaked 58 MB before).
      - Codegen (phase 9). Plan: 9.0 trims and fixes `codegen_c.c` before it
        is ported; 9a ports DCE behind a `--dump-dce` gate; 9b ports codegen,
        diffed byte for byte against `--emit-c` by a `selfhost_codegen`
        stanza; 9c brings it to speed and memory parity.
      - 9.0 done. Every fn body is emitted from IR: `ir.c` lowers everything
        typecheck accepts (no entry in the tests, the draw goldens, the
        language examples or the Zeus apps has a partially lowered fn). So
        the AST emission path is gone, and a fn the IR could not lower is an
        internal error. `codegen_c.c` went from 4,542 to 3,332 lines, and
        the port needs one emission path, not two.
      - 9.0 also fixed a bug the IR port surfaced: a parameter, struct field
        or capture named with a word C reserves (`signed`, `register`,
        `stdout`…) was written into the C as it is, and the C did not
        compile. Every such name now goes through one helper, `cid`, which
        spells a reserved name `loam_kw_<name>`
        (`compile_pass/c_reserved_names.loam`; 20 C errors before).
      - 9a done: `loamc --dump-dce` lists the fn declarations the dead-code
        pass keeps, per module in declaration order (default build: no test
        runner, no server split). `compiler/dce.loam` ports `dce.c` and
        matches on all 520 loadable entries, gated by the strict
        `selfhost_dce` stanza. Its sets are flags per node, where `dce.c`
        scans an array on every insert: 2–3 ms against 9 ms on a Zeus entry.
        Two cleanups in `dce.c`: `has_closure` was dead (it only called
        itself), and a host root was listed twice.
      - 9b done: `compiler/codegen.loam` ports `codegen_c.c` whole: the
        runtime paste and headers, struct typedefs and definitions in
        dependency order, element hooks and ownership helpers, globals and
        their constant initializers, prototypes, closure envs (per instance
        for closures in generic fns), `#[c]` bindings, fn-value trampolines,
        every IR instruction and terminator, `#line` directives, `__init`
        order, and the literal default templates. `loamc --dump-c` writes
        the C a default build emits (no test runner, server split, shards or
        batch table; the same bytes as `--emit-c`) to stdout, and `tcdump
        --c` writes the Loam compiler's. The strict `selfhost_codegen` stanza
        diffs them on all 521 loadable entries (same gate, worker and cache
        as the others). They also match on the 109 other loadable programs
        with a `main` in `examples/` and `packages/` (Zeus apps, `nob`, `zeli`,
        the Loam compiler itself).
      - One addition to std: `conv.f64_to_g17`, C's `%.17g`, the spelling of
        a float constant in generated C. `f64_to_str` is the shortest
        round-trip text, which is not the same bytes.
      - The port surfaced a bug in the Loam typecheck: a negative float
        literal was folded as `0.0 - x`, which is `+0.0` for `-0.0` (and for
        `-0` where a float is expected). C negates, as it should. No gate
        entry had one; a Zeus app's math tests did
        (`compile_pass/neg_zero_float.loam`).
      - 9c, speed and memory: the emitter already beats `codegen_c.c` alone,
        timed between the end of IR lowering and the last byte written
        (median of 5): 32 ms vs 50 ms on `zeus_show`, 74 ms vs 113 ms on the
        dracula app (5.3 MB of C). DCE plus lowering: 45 vs 65 ms and 81 vs
        109 ms. Peak memory of the whole run is below C's (~110 MB vs 120 MB,
        ~140 MB vs 163 MB). What keeps it there: a type's C spelling and
        `needs_drop` are memoized per type id; inside a generic instance only
        a type naming a parameter is substituted (`subst_type` allocates a
        new type per call); each local's spelling is made once per fn; the
        text streams out in 64 KB pieces.
- [x] Driver (module loading, package resolution, `cc` invocation) in Loam
      (phase 10). Done for host builds (10c closed 2026-11-11, numbers at the
      end of this item). The refused builds (wasm, iOS, Android, `--cost`,
      batch, shard, server split, `LOAM_HOST`) are ported under Next changes
      4, before the C compiler goes. Plan: 10a the loader (`compile.c`, `pkgresolve.c`); 10b
      `compiler/loamc.loam`, the whole pipeline in one process with `-o`,
      `--emit-c`, `--check` and the `cc` calls (host targets only: wasm, iOS,
      Android, shards, `--cost` and the LSP stay C until after the fixed
      point); 10c speed and memory parity.
      - 10a, in progress. `compiler/loader.loam` ports `compile.c`'s loading
        and all of `pkgresolve.c`: std lookup, vendored packages with
        `loam.lock` checksums, `pkg:` imports, workspaces, `[package]`
        manifests, `name:module`, `LOAM_PATH`, module names, cycles, the
        implicit `http` / `std:json` imports, load ranks. `moddump.loam`
        prints the list as `loamc --dump-modules` does; `tcdump` now also
        takes a `.loam` entry and loads it itself. The strict
        `selfhost_load` stanza diffs the two with every `.loam` file in the
        repo as the entry (946 match), and `tests/loader/` holds the package
        cases (good and bad checksums, unlisted vendor dirs, workspaces,
        cycles, std-name clashes).
      - The Loam compiler finds its files from `LOAM_HOME`, or two levels up
        from its binary (`<home>/bin/loamc`), instead of paths baked in at
        build time, so the generated C carries no absolute path.
      - Two std additions: `sys.realpath` (the loader canonicalizes package
        dirs as C does) and `str.slice` as a one-copy runtime hook (it pushed
        byte by byte). `str` joins the modules whose bodyless fns are hooks
        (`is_ffi_mod`, in both checkers); without it the hook compiled as an
        empty Loam fn and the C did not build.
      - Two fixes in `pkgresolve.c`: vendored packages are read in name order,
        not `readdir` order (which package wins a name must not depend on the
        file system), and an unterminated quote in a workspace `members` list
        no longer loops forever.
      - Speed: loading dracula's 90 modules (1.9 MB) now takes 55 ms in Loam
        vs 59 ms in C, whole process, min of 21. It took ~120 ms, almost all
        of it parsing (phase 5's parity was measured as parse plus dump). Lex
        and parse alone, best of 20 rounds in one process: 38–39 ms vs C's
        38.6 ms; the lexer alone 13 ms vs 22 ms. Each change was profiled and
        kept only if it moved the number:
        - Common nodes skip `NodeBuild` (`ast.add_plain` / `ast.add_ops`),
          which cost a retain and a release of ten owning fields per copy.
        - The lexer steps over identifier, digit and line-comment runs with
          one column update per run (none of those bytes is a newline), and
          over whitespace, strings and block comments without a bounds-checked
          `at` per byte. Keywords are found by first byte, not a chain of 21
          compares per identifier. 28 ms → 13 ms.
        - The parser compares words in place (`cur_is_word` copied the token
          to test for `await` on every unary expression), reads number
          literals in place, and unquotes a string literal with no backslash
          as one slice (it pushed byte by byte, then copied).
        - `parse_postfix` keeps its call-argument and member branches in
          helpers. A fn's exit drops every owning local some path may have
          set, and most expressions leave the postfix loop at once: it ran 19
          releases per primary expression.
        - Left out: the arenas double from empty, and `realloc` copies them
          (~4 ms of the 38). Reserving from the source size would remove it,
          but needs a `reserve` builtin, new language surface the loader does
          not need to beat C. Checked by reserving `NODES` and `INFO` in the
          generated C: the load goes from 55 to 51 ms, the whole `tcdump --c`
          run from 505 to 497 ms (C's `--dump-c`: 540 ms), output unchanged.
          Worth it only if 10c finds the same cost in the IR and type arenas.
      - 10b done. `compiler/loamc.loam` is the whole compiler in one process:
        load, check, emit C, and build with the C compiler. It takes
        `driver.c`'s command line (`build`, `check`, `-o`, `--emit-c` / `-S`,
        `--run`, `--int64-compat`, `--[no-]string-owns`, `-h`) and gives the
        same stdout, stderr, exit status and C. Native builds as the seed does
        them: the `-O1` (or headless / dev `-O0` with FastISel on arm64 macOS)
        flags, runtime objects cached in `<runtime>/.obj/` by mtime, the plain,
        http / net, maya, raygui and Zeus (Cocoa, X11, headless) link lines,
        `.dylib` images, the app's `runtime/<stem>_runtime.c`, `#![c_source]` /
        `#![c_link]`, `LOAM_LINK_EXTRA`, `LOAM_PROGRESS`, `LOAM_TIME`.
      - The C compiler is `CC` (split on spaces), else the first of `cc`,
        `clang`, `gcc` on `PATH`, run through `sys.spawn` with an argument
        list: no shell, so nothing in a path is interpreted, and the same code
        runs on Windows. Codegen keeps the unit whole and writes it once
        (`codegen.SINK`); no file-append API was needed. One std addition,
        `sys.arch()`, for the arm64-only flag.
      - Refused by name until after the fixed point (the seed keeps them):
        wasm / iOS / Android, `--cost`, `--dump-*`, and the `LOAM_BATCH` /
        `LOAM_SHARD` / `LOAM_SERVER_SPLIT` / `LOAM_HOST` builds. The `.fresh`
        build cache moved to 10c (done there).
      - Ported since: `--emit-ir`, `loam test` and `LOAM_NO_DCE`.
        `compiler/irprint.loam` ports `ir_print`, the readable IR that
        `--emit-ir` writes (to `-o` or stdout; exit 1 when the verifier objects),
        with one std hook, `conv.f64_to_g6` (C's `%g`, how it spells a float).
        It matches the seed on all 1,026 `.loam` files in the repo: exit
        status, stdout, stderr and IR bytes. `loam test` leaves the program's
        `main` out, keeps the `test` module's fns in DCE, and emits a runner
        that calls the module inits and then every kept `#[test]` fn between
        `test.begin` and `test.ok`. The runner binary goes to a temp path and is
        removed after the run unless `-o` names it. `LOAM_NO_DCE` skips the
        pass, so every fn is kept. On the 34 `std:test` entries the runner C,
        the `LOAM_NO_DCE` C and the test output match the seed.
        `selfhost_driver` gates all three: `--emit-ir` on `zeus_stream` and
        `drop_nested`, which between them print every op and modifier the
        lowering makes (the lowering itself is gated by `selfhost_passes`),
        and `loam test` on the in-language test entries.
      - The port surfaced a seed bug. `loam test` without `-o` reserved its
        binary's name with `mkstemp` and left the empty file in `$TMPDIR`
        when the build failed after that point. It now unlinks the file at
        once and keeps only the name; `cc` creates the file.
      - The strict `selfhost_driver` stanza: over all 972 `.loam` files,
        `check` prints and exits the same as the seed, and where it passes
        `--emit-c -o` writes the same C; the 34 `compile_pass` fixtures with a
        `.expected` are built by the Loam driver and run. Then the Loam
        compiler built by the seed (stage 1) builds itself (stage 2), and the
        C that seed, stage 1 and stage 2 emit for the compiler (3.9 MB) is the
        same bytes.
      - Building the compiler itself (`LOAM_TIME`): check 0.10 s vs the
        seed's 0.08 s, codegen 0.08 s vs 0.12 s, `cc` 14.5 s either way. The
        check gap (loading plus the passes in one process) is 10c's to close.
      - The port surfaced a compiler crash: `&` of a temporary
        (`count(&[]string { "a" })`, `&f()`) passed typecheck and then
        stopped IR lowering with an internal error, since `ir.c` will not hand
        out the address of a temporary. Both checkers now reject it: "cannot
        borrow a temporary; bind it to a name with `let` first"
        (`compile_fail/borrow_temporary.loam`). No program in the repo used it.
      - 10c, in progress. Per pass, in one process, median of 11–21 runs
        (`passbench.loam` vs the C passes timed the same way), Loam vs C:

        | entry | load | typecheck | borrowck |
        |---|---|---|---|
        | `zeus_show` | 30 vs 28 | 116 vs 132 | 29 vs 49 |
        | dracula | 46 vs 43 | 151 vs 178 | 42 vs 64 |
        | the compiler itself | 23 vs 21 | 26 vs 25 | 19 vs 24 |

        Every other pass was already ahead. Each change was profiled and kept
        only if it moved the number:
        - Typecheck: one scope stack of parallel arrays instead of a vec per
          scope (`SC_*`, `SCOPE_START`), and a `NodeView` of the plain fields
          where `ast.full` copied ten owning ones. Telling a global from a
          local scanned every global on each identifier, in C as well. A
          module `let` now carries `M_GLOBAL_VAR`, so the test is one load.
          29 → 26 ms on the compiler.
        - Borrowck: each fn pushes every module global as a binding, and on
          the compiler 87% of the entries a branch snapshot copied were
          globals. Globals sit at the bottom of `ACTIVE` (`N_GLOBALS`) and
          never change state, so snapshots, the NLL scan and the dangling
          scan stop there, and `find_b` looks them up by name in a per-module
          map. Snapshots share one pool and store a root name as an int.
          26 → 19 ms on the compiler, 64 → 42 ms on dracula.
        - The port surfaced a memory-safety hole, in both checkers: a move out
          of a module global (`let x = G`, `G.f`, `mod.G`) was accepted. A
          `Box` global was left NULL for the next reader, and `mod.G.f`
          copied the pointer without clearing it, so two owners freed one
          box. It is now an error ("cannot move out of global 'G'; other fns
          still read it — borrow it with `&` instead";
          `compile_fail/move_out_of_global.loam`). No program in the repo did
          it. This is also why globals never change state in borrowck.
        - `push` of a temporary moves it in (both compilers, every program).
          `push(NODES, Node { … })` retained each refcounted field for the
          slot, then released the temporary at the end of the statement
          (~20 ns a node in `ast.add_plain`). Now a struct literal or call
          result, and any element holding a `Box` (moved by borrowck), is
          handed over whole: the IR marks the push `moved` (`call … move` in
          `--emit-ir`, `moved=1` in the IR dumps), the temp loses its drop,
          and codegen calls `loam_vec_push_take`, which skips the retain
          (its hook still copies the elements if the buffer is shared). A
          named element keeps its own reference, as before. Load on dracula
          29 → 28 ms, 1 ms or less elsewhere, in the timing below.
        - This also fixed a leak: an element holding a `Box` was moved
          into the slot, but its strings were still retained, so
          `push(rows, Boxed { b, text })` leaked `text` on every push
          (`compile_pass/drop_pushed_temps.loam` now covers it).
        - Big vecs grow 4× (runtime, every program). Growing the arenas by
          doubling copied ~150 MB on dracula (AST ~85 MB, IR ~30 MB, types
          6 MB). Now a push that overflows a full buffer of 256 KB or more
          grows it 4× (`loam_vec_grown_cap`), so a big table is copied about
          half as often on its way up. A first buffer, an exact-size request
          (`loam_vec_of`) and a small vec still double, so constant tables
          and small lists keep their size. Peak RSS went down, not up, since
          fewer old copies are live at once: dracula 103 → 101 MB,
          `zeus_show` 80 → 68 MB, the compiler 39 → 33 MB (C on dracula:
          156 MB). Load 28 → 26 ms on dracula.
        - Timed on a quiet machine with `LOAM_HOME` set (Loam vs C, median
          of 21), load / typecheck / borrowck: dracula 26 vs 26, 88 vs 105,
          25 vs 39; `zeus_show` 17–18 vs 17, 68–69 vs 78, 17 vs 30; the
          compiler 13 vs 13, 16 vs 15, 11 vs 15. A `stat` cache was tried
          and dropped: 852 → 321 calls, no time change.
        - The duplicate-declaration check keeps each name's first
          declaration in a map. It compared each top-level name with every
          earlier one in the module (C still does), which is quadratic, and
          each compare copied a refcounted name; the compiler's own modules
          hold thousands of declarations. Typecheck on the compiler, mean of
          61 runs: 25.5 → 23.8 ms (C: 24.9 ms). Same errors in the same
          order (`compile_fail/duplicate_decls.loam`, matched against C by
          `selfhost_typecheck`). With this, every pass in one process
          matches or beats its C twin on all three entries.
      - The `.fresh` build cache is in the Loam driver. A native build with
        `-o` writes `<output>.fresh`, which holds a key line, the output's
        stamp, and one stamp per file the build read. The same command run
        again reuses the output while every stamp still matches, and prints
        `loam: fresh` under `LOAM_TIME`. `LOAM_NO_FRESH=1` turns it off, and
        `check` / `--emit-c` never use it. The key hashes the arguments, the
        environment that changes a build, the compiler binary, and the runtime,
        host and app `runtime/` directories. One std addition, `sys.stamp`
        (seconds, nanoseconds and size, as the seed's `stat` gives them), since
        `sys.mtime` is in milliseconds and has no size.
        - Three bugs fixed in both drivers. The stamp listed only the `.loam`
          modules, so an edit to a `#![c_source]` file, a header beside it, or
          a `LOAM_LINK_EXTRA` input still counted as fresh; those files are
          stamped now. A compiler run from `PATH` (`loamc`, not `bin/loamc`)
          failed to `stat` its `argv[0]`, so a rebuilt compiler kept its old
          outputs; the key now finds the binary on `PATH`. And the `.obj`
          cache compared mtimes to the second, so a C source edited in the
          same second its object was built kept the stale object; a source is
          now stale unless it is strictly older, at the finest time the file
          system records.
        - `tests/fresh/check.sh` holds 17 cases: reuse; a same-size module
          edit; a C source edit; an env var change; a flag change; a new file
          in `runtime/`; `LOAM_NO_FRESH`; a deleted output. `check` and
          `--emit-c` must leave no stamp. The `fresh_cache` stanza runs the
          cases on the seed, and `selfhost_driver` on stage 1.
      - 10c closed. The whole pipeline, one process per run, best of 5,
        C compiler vs Loam compiler (macOS arm64, 2026-11-11):

        | entry | check | `--emit-c` | peak RSS check / emit |
        |---|---|---|---|
        | `zeus_show` | 0.23 vs 0.17 s | 0.36 vs 0.25 s | 65 vs 68 / 114 vs 99 MB |
        | dracula | 0.31 vs 0.23 s | 0.55 vs 0.37 s | 85 vs 84 / 156 vs 150 MB |
        | the compiler | 0.09 vs 0.07 s | 0.20 vs 0.14 s | 27 vs 37 / 62 vs 60 MB |

        Faster everywhere; memory at or under C's except `check` on the
        compiler itself (+10 MB). That one entry's peak is the 4× growth of
        its arenas past 256 KB (see the vec growth note above): over-reserved
        but small, and `--emit-c`, the run that builds anything, is under C.
- [x] Compiler itself uses only safe Loam (arenas + integer indices; §2.7).
      Audited 2026-11-11 over `packages/loam/compiler/`: no `#[c]` fn, no
      `#![c_source]` / `#![c_link]` (the matches are strings it parses or
      emits for user programs); the AST, types, IR and scopes are arenas of
      plain structs addressed by `int`, with `Box` only in a few fields
      (allowed by §2.7); the C it trusts is the shared runtime and std hooks,
      which every Loam program trusts (§6). One `sys.exec`, a fixed
      `pkg-config` string for raylib with nothing interpolated; S4 replaces it
      with a spawn that captures output.

### Fixed-point and tooling
Checked on a fresh `git clone` of `e5c0657` (macOS, 2026-11-10): `./nob.sh`
built the seed and tools, and every self-hosting stanza passed
(`selfhost_lex`, `_load`, `_parse` on 966 files; `_typecheck`, `_check`,
`_ir`, `_dce`, `_codegen` on 521 entries plus 23 that C cannot load;
`selfhost_driver`; `fresh_cache`). The five checker stanzas are now one,
`selfhost_passes` (see the slow-suite section below); it reports the same
five verdicts.

- [x] Stage 0 (C) builds stage 1 (Loam) (`selfhost_driver`)
- [x] Stage 1 builds stage 2, byte-identical C output. `selfhost_driver`
      checks this for the compiler's own C (seed = stage 1 = stage 2), so
      stage 2 is the same program as stage 1. A one-off run also compared
      stage 1 with stage 2 over the whole corpus: same `check` output on all
      966 files, and the same C on the 782 that emit any.
- [x] Differential tests pass for `compile_pass`, `golden`, `compile_fail`:
      `selfhost_driver` compares `check` (diagnostics and exit status) and
      `--emit-c` with the seed on every `.loam` file in the repo, and the
      per-pass gates compare each stage's dump.
- [x] Deterministic emission: the Loam compiler never iterates a map
      (`map_keys` / `keys` have no callers under `compiler/`), and Loam has
      no addresses to order by. Emitting each file twice gives the same bytes.
- [x] Fresh checkout builds on macOS, Linux, and Windows from the C seed.
      macOS: done (above). Linux: done; CI checks out fresh and builds with
      `./nob.sh` (run 42804061060). Windows: done; the `windows` and `same-c`
      jobs are green in run 43065909932 (`9a6afcd`).
      The C compiler's driver is POSIX, so the Loam compiler's generated C is
      committed as `bootstrap/loamc.c` (§3 Stage 0 seed), and on Windows
      `./nob.sh` builds `bin/loamc` from it with MinGW gcc (`nob` itself does
      not run there: `sys.exec` goes through `cmd.exe`). The Windows job no
      longer downloads anything from Linux. It builds from the seed, emits
      the fixtures' C, builds and runs them, and has the seed build the current
      compiler, which reaches the fixed point (`tools/win-selfhost.sh`). A
      `same-c` job then compares that C with Linux's, byte for byte. Checked
      locally: `zig cc -target x86_64-windows-gnu` builds the seed into a
      Windows binary through `nob.sh`'s Windows branch, and `selfhost_seed`
      passes on macOS in 18 s.
- [x] CI runs the fixed-point check on at least one Linux and one Windows
      runner. Green in run 42804061060 (`260ab67`): the suite, including
      `selfhost_driver`, on Linux x86_64 and aarch64. On Windows x86_64,
      `win-run` passes 90/90, and `win-selfhost` gets stage 1 = stage 2 =
      Linux (3,933,147 bytes of C) and builds and runs 90/90 fixtures with
      the Loam compiler. The first run of the job
      run (on `0a646c3`) got stage 1 = stage 2 = Linux on Windows, but 9
      fixtures failed to build there: the drivers link `runtime/net.c` for
      any program that loads the http package, and `net.c` had no Windows
      branch (`poll.h`). On Windows it now defines nothing, so a program that
      never calls `loam_net_*` links. `tools/cross-check.sh` links `net.c`
      into every fixture, as the drivers can, so it catches this on macOS.
      The same run found three fixtures (`http_stream`, `zeus_stream`,
      `zeus_async_rpc`) that re-ran `tests/tmp/<name>` as their server child.
      That file does not exist when the fixture runs inside a batch binary.
      They re-run `sys.arg(0)` now. Locally they still failed batched: the
      zeus batch is a shard build, and `loam_rt.h` kept `argv` (and the exec
      status, spawn, future and channel tables) as `static`, one copy per
      unit, so `sys.arg(0)` read "" outside the unit holding `main`. That
      state now has one definition, in the unit that owns the trap state.

A clone has `packages/tooling/tree-sitter-loam` as an empty directory: the
gitlink has no `.gitmodules` entry, and the grammar repo has no remote. The
build and the suite never read it, but `git submodule status` fails, and so
do `nob grammar` and the Zed extension, in any checkout other than this one.

### `./nob.sh test` is slow (fixed: the self-hosting stanzas moved out)

The suite took 1142 s on the `ubuntu-24.04` runner in run 42798804290, and
1091 s of that was the `setup` pool. The self-hosting stanzas are almost all
of it. Typecheck, check, ir, dce and codegen each ran 6–8 minutes,
concurrently, and `selfhost_driver` ran about 5 minutes after them. The
fixture batches, serve and cases phases together took under a minute. Each
gate runs its first ~240 of 544 entries in about 11 s, then slows to about
1.5 s per entry. Local runs are slow for the same reason. Not yet measured
locally (the per-entry timing probe was stopped). What the code shows:

- Each gate entry (`tools/selfhost-tc-one.sh`) runs three full front ends:
  `loamc --dump-modules`, `tcdump <flags>` (load, parse and every pass up to
  its stage), and `loamc <dump flag>`. The five gates repeat all of it over
  the same 544 entries: about 8,000 compiler runs, mostly the same
  load + parse + typecheck work.
- The result cache (`tests/tmp/selfhost_tc_ref`) is always cold in CI. Its
  key includes the checksums of `bin/loamc` and `tcdump`, so locally any
  compiler edit misses every entry too, which happens on almost every run
  during the port.
- Oversubscription: `nob test -j 4` runs up to four stanzas at once, and
  each gate starts `xargs -P 8`, so CI's 4-core runner has up to ~32 compiler
  processes. `selfhost_driver` uses one per core on top.
- The slow tail is likely the entries with big module sets (zeus,
  `draw_golden`, examples). This is not measured.
- Minor: the 60 s watchdog's `kill` hits the subshell and leaves its
  `sleep 60` running, one per entry. This is harmless but should be checked.

Candidate fixes, in order of expected payoff (measure first: time one small
and one zeus entry for each of the three runs):
1. Done. One process per side per entry: `tcdump --all <dir>` writes all
   five dumps from one pipeline run, and `loamc --dump-all <dir>` all five C
   dumps from one. The five gates are one stanza, `selfhost_passes`
   (`tools/selfhost-passes-one.sh`, one worker per core), which still
   reports a verdict per stage. The cache holds C's five stage checksums per
   input key, and a match of all five per `tcdump` sum; a new seed empties
   it. Full local suite, uncapped, on a quiet 8-core Mac (2026-11-10):
   445 s from a cold cache and 403 s warm (615 checks).
2. Not done: capping parallelism made it slower. Timed alone (warm), the
   stanzas take lex 6 s, parse 8 s, load 25 s (one serial loop), passes 44 s
   and driver 100 s (one worker per core each), but together they ended at
   18, 22, 208, 316 and 398 s; every other setup check was done by 3 s.
   Capping each stanza's pool at 3 workers gave 572 s, and at 4 gave 907 s.
3. Cache the C side by its inputs only (the `bin/loamc` sum plus the module
   sources, as now). Persist `tests/tmp/selfhost_tc_ref` in CI with
   `actions/cache`.
4. Done. `./nob.sh test` no longer runs them: `nob selfhost [stanza]` runs
   `selfhost_lex`, `_parse`, `_load`, `_passes`, `_driver` and `_seed`
   (all six, or one), and CI's `selfhost` job runs each on its own runner beside the test
   job (`selfhost_driver` on x86_64 and aarch64), so the workflow takes about
   as long as the slowest stanza. Local `./nob.sh test`: 403 s → 15 s (606
   checks). Once the C compiler is gone (Next changes, 1 and 4), the per-pass
   stanzas go with it and only the fixed point stays.

### Next changes (pending)

Every box still unchecked in this file, in the order to do them. Done since
the last list: §5.2 S0, typed handle cells (2026-11-11).

**To call Loam 100% self-hosted: what is left** (2026-11-12). Against the
definition at the top, items 1–4 and 7 hold:

- the compiler and every tool are Loam;
- the committed seed builds stage 1;
- the seed, stage 1 and stage 2 emit the same C for the compiler, so stage 1
  and stage 2 are the same program on every input;
- the differential runs against the C compiler passed before it was
  deleted.

What is left is item 6 (memory safety) and a re-run of item 5 (every OS from
a fresh checkout) on the newest work:

- [ ] 0a. **Fix the `Box` ownership hole** (§5.2,
      [box-and-init-ownership.md](box-and-init-ownership.md), bug 1).
      `Op.Alloc` must give the box its own references (as `move_inst` does),
      and a box's drop must release its value through the element hook. Land
      it with:
      - a `compile_pass` fixture per shape, under `leaks` / ASan;
      - the compiler building itself under ASan;
      - the full suite, `nob selfhost`, and a new seed if the compiler's own
        C changes.
      Until then a safe program can read freed memory, so the §2.7 claim does
      not hold.
- [ ] 0b. **Fix the module-`let` temporary leak** (same doc, bug 2):
      `emit_drops_at_exits()` in `lower_init`. Not a safety issue, but every
      reassigned global leaks its first value; it lands with 0a's gates.
- [ ] 0c. **CI on this work:** the `selfhost` workflow (a pull-request run:
      Linux x86_64 and aarch64, Windows) green on the commit with P5 and the
      formatter port, then again on 0a and 0b. Since P5, the Linux test job
      also builds `loam-lsp` and `loam-fmt` with `bin/loamc`.
- Then item 6 of "Security" below (S1), so the next hole of this kind is
  found by a generator, not by accident: `Box` construction and drops belong
  in it.

Done, in order:

1. Done (2026-11-11): Windows from a fresh checkout. The seed is committed
   (`bootstrap/loamc.c`, `nob seed`, `selfhost_seed`), and the Windows job
   builds from the checkout alone; it and `same-c` are green in run
   43065909932. S10 (reproducible seed) can later check the lagging seed
   against the sources it was made from.
2. Done (2026-11-11): phase 10c, whole-pipeline parity (Driver box).
3. Done (2026-11-11): the compiler uses only safe Loam (§2.7 box).
4. Make the Loam `loamc` the default and delete the C compiler: the seed
   is the Loam compiler's committed C (item 1), not the C sources (§4 step
   7). The per-pass `selfhost_*` stanzas go with it; the fixed point stays.
   First the Loam driver takes the builds it still refuses, in phases:
   - [x] P1 (2026-11-11): `LOAM_BATCH`, `LOAM_SHARD`, `LOAM_HOST`,
     `LOAM_SERVER_SPLIT`. Codegen marks each definition for the cutter, makes
     what units share `extern`, puts the runtime's and the globals' storage
     under `LOAM_SHARD_DEFS`, emits the batch program's per-case init table,
     and gives a client build's `#[server]` fns trap bodies. The driver cuts
     the C into units by marker, caches them by content hash in the shard
     dir, builds the header once as a PCH, compiles the missing units one
     per core (`sys.spawn` / `sys.wait`; one std addition, `sys.cpu_count`),
     links `@rsp`, and prunes what it did not use; on failure it compiles the
     whole program. A host build compiles the platform layer and desktop
     host with `LOAM_HOST_BUILD` into `.host` objects. Checked against the
     seed: the same C for batch programs (`suite_plain`, 135 fixtures, and
     `suite_zeus`) and for split builds (dracula, every `#[server]` file);
     a shard build of `zeus_carousel` compiles the same 25 units byte for
     byte, links, runs the same, and a rebuild compiles 0 units and prunes a
     stray file; a host build runs the same compiles with the same flags; a
     batch binary of `suite_plain` gives the same output on all 128 cases
     whose output is deterministic (the other 7 print OS-chosen ports).
     `selfhost_driver` now also compares the batch and split C.
   - [x] P2 (2026-11-11): the wasm32 target, what `zeli serve` and the wasm
     stanzas build: `--target wasm32`, `--wasm-release` (and its loader
     warning on a full bare-package import in the entry, the only warning the
     seed has), `--wasm-split-input`, the path strip (`codegen.PATH_STRIP`),
     server split, the `#[c]`-reachable error, the wasm32 clang search
     (`LOAM_WASM_CC`, Homebrew LLVM), `-ffile-prefix-map`s, the Canvas2D
     `loader.js` copy, and sharded wasm builds with the runtime and web host
     as cached units. Checked against the seed: the same C (dracula,
     `zeus_carousel`, `trap_app`, plain and release); the same `.wasm` bytes
     plain, release, split-input (and the same kept C) and sharded (the same
     30 units); the same warning. `selfhost_driver` compares the wasm C.
   - [x] P3 (2026-11-11): `--cost`. Lowers once, counts fns, blocks, IR
     insts, allocs, drops, moves, bounds checks, overflow traps, calls,
     closures and string literals, sizes the C through the build's own
     codegen, and bills each fn's share of the C to `std`, a package
     (`loader.package_label`) or `(entry)`. The same report as the seed on
     arrays, `zeus_carousel`, dracula, myapp, `http_min`, `nob.loam` and the
     compiler; `selfhost_driver` compares three of them.
   - [x] P4 (2026-11-11): `--target ios` and `--target android`. iOS: the
     simulator SDK through `xcrun`, the `.app` with its `Info.plist`, the
     runtime and UIKit host as cached objects per SDK and arch, shard builds,
     ad-hoc `codesign`, and `--run` on a booted simulator. Android: the
     Gradle project (the Java host, manifest and Gradle files, the program as
     `app.c`, its CMake and app build files, `local.properties` when an SDK
     is found) and `--run` through Gradle and adb. Simulator and Gradle steps
     are shell pipelines through `sh -c`, as in the seed; compiles use
     `sys.run` argument lists. Checked against the seed: the same Android
     tree (11 files) and messages, and for iOS the same seven `xcrun`
     commands (temp names aside) and `Info.plist`; `selfhost_driver` compares
     the Android tree. The Loam driver now refuses only `--dump-*`, the
     seed's per-pass dumps that `tcdump`, `tokdump` and `moddump` mirror.
   - [x] P5 (2026-11-12): `loam-lsp` on the Loam front end.
     `compiler/lsp.loam` ports `lsp.c` whole: JSON-RPC over
     `sys.read_line` / `read_stdin`, open-document tracking, incremental
     `contentChanges`, diagnostics per module, hover (types, docs, module
     docs), go-to-definition, completion (module members, methods by
     receiver type, struct fields, enum variants, locals, params, keywords,
     `std:` and package import paths) and semantic tokens.
     - The front end keeps its state in module globals, so each pass module
       gained a `reset()` (`ast`, `dump`, `loader`, `types`, `typecheck`,
       `threadcheck`, `borrowck`, `boundscheck`) and the server empties
       them all before each check. The loader takes the editor buffer in
       place of the file (`loader.OVERRIDE_PATH` / `OVERRIDE_SRC`).
     - One parser flag, `parser.EDITING`, ported C's editor-only rule: a
       trailing `recv.` before `}` / `)` / `,` / `]` / `;` / end of file is
       not a syntax error, so completion still gets a typed tree.
     - Definitions are rebuilt from what typecheck already stores (`res`,
       `def_line` / `def_col`): a parameter's span is found among its fn's
       `Param` nodes, so `NodeInfo` grew no field.
     - Checked against the C server before it was deleted. On the smoke
       session (`tests/lsp_smoke.py`) all 43 replies are the same bytes,
       except that `std:` import completions are now sorted by name, not
       listed in `readdir` order. A sweep of 10 files (hover and definition
       at every identifier, completion after every `.`, semantic tokens,
       a broken edit and its undo, close) gave 3,716 identical replies
       (0.63 s vs C's 0.72 s). A 142-file sweep (`compile_pass`,
       `compile_fail`, dracula, `nob.loam`) matched on the first 15,279
       replies, where C hung rechecking a Zeus file after the broken edit;
       the Loam server finished all 119,434 in 56 s. The 4 differences in
       that prefix are a C bug: the method completion for `SwitchTrack`
       showed `fn() -> Style`, a type from inside the fn, where Loam shows
       its signature.
     - Memory over a long session is flat: after 12 and after 80 rechecks
       of a Zeus fixture the live heap at exit is the same 151,845 blocks
       (76 MB), with about 31 MB physical footprint; `leaks` finds a constant
       5 KB, whatever the number of rechecks.
     - `nob` builds `bin/loam-lsp` with `bin/loamc` (`build_loam_tool`, with
       the build-mode variables cleared as `nob.sh` clears them for the
       compiler); the `.fresh` stamp makes an unchanged rebuild free. Gone:
       `lsp.c`, `lexer.c`, `parser.c`, `ast.c`, `compile.c`,
       `pkgresolve.c`, `diagnostics.c`, `threadcheck.c`, `sema/*` and their
       headers (16,266 lines), and `obj/`. `tools/cross-check.sh`
       stopped compiling them for Linux: the seed it stood for is now
       `bootstrap/loamc.c`, which CI builds natively.
   - [x] P6 (2026-11-11): `bin/loamc` is the Loam compiler on every OS.
     `./nob.sh` has `cc` build the seed into `bin/loamc0` (only when the
     seed is newer), and `bin/loamc0` build `bin/loamc` from
     `packages/loam/compiler/loamc.loam`; the `.fresh` cache makes a repeat
     run 0.1 s, and a cold bootstrap takes about 30 s (14 s of `cc`, 17 s
     for stage 1). The build-mode variables of the environment
     (`ZEUS_HEADLESS`, `LOAM_BATCH`, …) are cleared for that step: they are
     part of the `.fresh` key, and a headless test run would otherwise
     rebuild the compiler unoptimized. `nob` no longer compiles `driver.c`
     or links a C `loamc`; it still builds `bin/loam-lsp` (`lsp.c` and the C
     front end as its library, until P5) and `bin/loam-fmt`. `nob seed` has
     `bin/loamc` emit its own C. `nob selfhost` and CI's selfhost job run
     only `selfhost_seed` (x86_64 and aarch64): the per-pass stanzas need a
     C `loamc` and go in P7. Full `./nob.sh test` with the Loam compiler:
     610 passed, 0 failed in 25 s (macOS, 8 cores).
   - [x] P7 (2026-11-11): the C compiler is deleted. Gone: `driver.c`,
     `codegen_c.c`, `ir.c`, `dce.c`, `ast_dump.c` and their headers (9,800
     lines; nothing but the driver used them), the C `--dump-*` modes, the
     per-pass stanzas (`selfhost_lex`, `_parse`, `_load`, `_passes`,
     `_driver`) and their tools (`tcdump`, `tokdump`, `moddump`, `astdump`,
     `irdump`, `passbench`, `tools/selfhost-passes-one.sh`); `dump.loam`
     keeps only the diagnostics text. Nothing else used `--dump-*` (the
     grammar check reads the tree-sitter parser only). `selfhost_seed`
     stays and took over what was not differential in `selfhost_driver`:
     the seed builds stage 1, stage 1 builds stage 2, the seed, stage 1 and
     stage 2 emit the same C for the compiler, and the `.fresh` cache cases
     hold on stage 1. Two things only the differential runs covered now
     have their own checks in `nob test`: `loader_cases` (`loamc check` on
     the 24 entry files of `tests/loader` against a recorded
     `expected.txt`: lock checksums, workspaces, cycles, missing packages)
     and `android_project` (the Gradle tree `--target android` writes).
     CI's selfhost job runs `selfhost_seed` on x86_64 and aarch64. The rest
     of the C front end (`lexer.c`, `parser.c`, `sema/*`, `compile.c`,
     `pkgresolve.c`, …) is linked only by `loam-lsp` and goes with P5;
     `fmt.c` (standalone) stays until it is ported.
5. Done (2026-11-12): what else moves and what stays C (§3). Every tool is
   Loam, built by `bin/loamc`, the default compiler on every OS:
   - `loam-fmt` is `compiler/loamfmt.loam`, a port of `fmt.c` with its own
     token lexer (it must keep comments). It matches C byte for byte (stdout,
     stderr, exit status) on all 1,028 `.loam` files in the repo and on
     1,200 truncated, line-deleted or reindented copies, and in every mode
     (`-w`, `--check`, stdin, bad options, missing files).
   - Speed, `--check` over the corpus six times, CPU time over 11
     interleaved runs: best 1.800 s vs C's 1.792 s, median 1.822 s vs
     1.844 s. To get there: operators are matched as byte pairs, keywords by
     length once per word into a token flag, one-byte tokens skip the width
     scan, and token text is compared and copied in place, never sliced.
   - `fmt.c` is deleted, and `packages/loam/src` with it.
   - What stays C: the seed (`bootstrap/loamc.c`, the compiler's own
     generated C) and the runtime, host shims and vendored libraries (§6).
   - The examples' `run.sh` scripts looked for a `Makefile` that no longer
     exists; they look for `nob.sh` and run it now. `make` is used nowhere.

**Security, alongside items 1–4** (every hole found is fixed in both
compilers while both still exist):

6. §5.2 S1: soundness fuzzing in CI (moves, globals, closures, threads, vec
   growth; ASan + UBSan), and the borrow rules as a numbered list.
7. §5.2 S2: fuzz the trusted C (`std:conv`, `parse_f64`, `str` slicing,
   UTF-8, `std:json`, HTTP/1 and HTTP/2, `read_line` / `read_stdin`).

**Housekeeping, not needed to self-host:**

8. `packages/tooling/tree-sitter-loam`: replace the remote-less gitlink.
   Either vendor the grammar as plain files (recommended; the pre-commit hook
   and the commit-nested-repos rule assume gitlinks, so update them) or push
   it to a remote and add `.gitmodules`.
9. `std:net` has no Windows port (`runtime/net.c` defines nothing on
   `_WIN32`). The 12 networking fixtures are skipped on Windows. R5 needs it.

**After the fixed point:**

10. Threads (§5.1): T1 ThreadSanitizer stanza, T2 send a `[]T` by move, T3
    I/O on workers, T4 a multi-core HTTP server; T5 join only if T3/T4 need
    it.
11. The rest of the security plan (§5.2): S3 FFI listing, S4 shell-free by
    default, S5 hardened native builds, S6 stack overflow with a message, S7
    keyed map hash, S8 the closure env leak, S9 trap containment for
    servers, S10 reproducible seed.
12. The runtime and the packages' runtime C in Loam (§6.5): R0 the
    trusted-C counter, R1 pure logic, R2 `sys`, R3 async and threads, R4 the
    Zeus platform, R5 net, R6 hosts, R7 the core reviewed. Each step must
    match the C it replaces in behaviour and speed and lower the trusted-C
    count.

**Extensions, not part of the self-hosting claim:**

13. C FFI (§1.7): import C headers, link listed `.c` files, memory-safe type
    mapping, common patterns (opaque structs, fn pointers, C strings),
    Linux and Windows.
14. Embedded and robotics targets (§1.8): bare-metal runtime, cross
    toolchains, HAL access through FFI, a fixed-arena memory model, real-time
    notes, board packages, control-loop examples.
15. Energy and carbon hooks (§2.6): `--cost` energy estimates,
    `sys.energy_now` / `sys.carbon_intensity`, no forced instrumentation,
    documented grid-data inputs.

When every box above is checked, Loam is 100% self-hosted.
