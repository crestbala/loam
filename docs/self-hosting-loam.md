# Self-hosting `loamc`: what is missing

**Goal**: `loamc` written in Loam, built by the current C `loamc` (stage 0), then
able to build itself (stage 1 → stage 2, byte-identical C output). The
architecture in [loam.md](loam.md) stays the same: one Loam program in, one C
file out, `cc` links it. The runtime (`runtime/loam_rt.h`, host shims) **stays
C**. Self-hosting means the compiler is Loam, not the runtime.

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

**Current status**: Not self-hosted. Phase 1 (§1.1, §1.4, the §1.3 `u64`
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
Windows (§3). Next: finish 10c (speed and memory of the whole pipeline),
a faster `./nob.sh test`, and the rest of "Next changes (pending)" at the end.

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

- [ ] **Stage 0 seed**: keep the C compiler buildable as the seed, or commit
      the generated C of the Loam compiler (`loamc.c`) so a fresh checkout
      builds with only `cc` — the same move `make` already makes for `nob`.
      Must produce working binaries on Linux and Windows.
- [x] **Fixed-point check** in `nob`: C-`loamc` builds Loam-`loamc` (stage 1);
      stage 1 builds itself (stage 2); stage 1 and stage 2 must emit
      byte-identical C for the compiler and for every test in `compile_pass`
      / `golden`. Run the check on at least one Linux and one Windows CI
      runner. *Done in `nob` (`selfhost_driver`):* seed and stage 1 emit the
      same C for every `.loam` file in the repo, and stage 1 and stage 2 the
      same C for the compiler, so stage 2 is the same program as stage 1.
      CI: see Windows CI below.
- [x] **Deterministic output**: no pointer-address ordering, no hash-order
      iteration in emitted C (iterate maps by insertion order or sorted keys).
- [x] **Differential testing**: for each `tests/compile_pass`, `golden`, and
      `compile_fail` file, both compilers must produce the same C / the same
      diagnostics. This is the main safety net during the port.
- [ ] Decide what moves and what stays C: `lsp.c` and `fmt.c` can follow
      later; the formatter is an easy early port (lexer + printer only). It
      works on tokens, not the AST (the parser drops comments), so it is no
      check of the parser: phase 5 used the AST dump instead and left
      `fmt.c` for after the fixed point.
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
   C one only as the seed. Add Linux + Windows CI.
8. **Energy / carbon hooks** (§2.6) — optional APIs and `--cost` extensions.
9. **Embedded runtime & board packages** (§1.8) — bare-metal subset, cross-toolchain
   examples, and first robotics wrappers (after the host self-host is solid).
10. LSP last.

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
- [ ] Driver (module loading, package resolution, `cc` invocation) in Loam
      (phase 10). Plan: 10a the loader (`compile.c`, `pkgresolve.c`); 10b
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
        wasm / iOS / Android, `test`, `--emit-ir`, `--cost`, `--dump-*`, and
        the `LOAM_BATCH` / `LOAM_SHARD` / `LOAM_SERVER_SPLIT` / `LOAM_HOST` /
        `LOAM_NO_DCE` builds. The `.fresh` build cache moved to 10c (done there).
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
- [ ] Compiler itself uses only safe Loam (arenas + integer indices; §2.7)

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
- [ ] Fresh checkout builds on macOS, Linux, and Windows from the C seed.
      macOS: done (above). Linux: done; CI checks out fresh and builds with
      `./nob.sh` (run 42804061060). Windows: not yet from the checkout alone.
      The Windows job builds the Loam compiler from C that the Linux job
      emitted (`win-bundle`), because the C seed's driver is POSIX. To close
      this, commit the Loam compiler's generated C (§3 Stage 0 seed), or make
      the seed build on Windows.
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
      They re-run `sys.arg(0)` now.

A clone has `packages/tooling/tree-sitter-loam` as an empty directory: the
gitlink has no `.gitmodules` entry, and the grammar repo has no remote. The
build and the suite never read it, but `git submodule status` fails, and so
do `nob grammar` and the Zed extension, in any checkout other than this one.

### `./nob.sh test` is slow (open)

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
   445 s from a cold cache and 403 s warm (615 checks). The warm cache saves
   only ~40 s, so the remaining cost is elsewhere, most likely
   `selfhost_driver`, which has no cache (not yet timed alone). Load stays
   near 25 on 8 cores while the self-hosting stanzas overlap.
2. Cap total parallelism at the core count, across stanzas and inside the
   gates (one shared pool, not `-j 8` × one worker per core). Not decided:
   one capped run took 473 s, but under different cache and machine
   conditions than the numbers above.
3. Cache the C side by its inputs only (the `bin/loamc` sum plus the module
   sources, as now). Persist `tests/tmp/selfhost_tc_ref` in CI with
   `actions/cache`.
4. Run the self-hosting gates only when `packages/loam/src/`,
   `packages/loam/compiler/`, std or the runtime changed, or give them their
   own `nob selfhost` target that CI runs. Keep `selfhost_driver` in the
   default suite.

### Next changes (pending)

1. Speed up `./nob.sh test` (above).
2. Windows from a fresh checkout: commit the Loam compiler's generated C as
   the stage-0 seed (§3), or make the seed build on Windows. Then tick the
   fresh-checkout box.
3. Finish phase 10c (the Driver box under Compiler port): speed and memory
   parity of the whole pipeline against the C driver, then tick it.
4. Audit that the compiler uses only safe Loam (arenas + integer indices;
   §2.7) and tick that box.
5. Make the Loam `loamc` the default and keep the C one only as the seed
   (§4 step 7). Decide what stays C for now: `lsp.c`, `fmt.c` (§3).
6. `packages/tooling/tree-sitter-loam`: replace the remote-less gitlink.
   Either vendor the grammar as plain files (recommended; the pre-commit hook
   and the commit-nested-repos rule assume gitlinks, so update them) or push
   it to a remote and add `.gitmodules`.
7. `std:net` has no Windows port (`runtime/net.c` defines nothing on
   `_WIN32`). This is not needed to self-host. The 12 networking fixtures
   are skipped on Windows.

When every box above is checked, Loam is 100% self-hosted.
