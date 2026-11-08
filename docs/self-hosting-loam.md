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

**Current status**: Not self-hosted. The items in §1 are still open.

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

- [ ] `[]T` field inside `T` (directly and through another struct).
- [ ] `Box<T>` field inside `T`, plus an "empty" box: a recursive `Box` must be
      able to end. Either `Option<Box<T>>` working as a field, or a nullable
      box form.
- [ ] Copy/drop/size analysis that terminates on cycles (the hang is almost
      certainly a recursive walk over field types with no visited set).
- [ ] Generated C that forward-declares the struct before the vector/box type
      that refers to it.

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

- [ ] A builder: `std:strbuf` (or `[]u8` with `push_str(&mut buf, s)` /
      `push_byte`) with amortized O(1) append and one final
      `string_from_bytes`.
- [ ] Write a builder straight to a file without first materializing one
      giant `string` (`sys.write_file` takes a `string`; add a
      `write_bytes(path, &[]u8)` or an append/stream API).

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

- [ ] `std:conv` (or in `std:str`): `int_to_str`, `i64_to_str`, `u64_to_str`,
      `parse_int`/`parse_u64` returning `Result`, radix variants.
- [ ] Float formatting/parsing. Exact shortest round-trip is real work
      (Ryu/Grisu or `strtod` correctness); acceptable first step is a C hook
      in `loam_rt` over `snprintf("%.17g")` / `strtod`.
- [ ] Fix unsigned printing and the `u64(<max>)` literal-in-conversion case.

### 1.4 Character literals

`'a'` is a lex error (`expected expression (got unknown)`). A hand-written
lexer is hundreds of byte comparisons; writing `c == 97` instead of
`c == 'a'` makes the self-hosted lexer unreadable and error-prone.

- [ ] `'a'`, `'\n'`, `'\\'`, `'\''`, `'\x7f'` as an untyped integer literal
      (byte value; a `u8` in `u8` context). No `char` type needed.

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

- [ ] `sys.mtime(path) -> i64`.
- [ ] `fmt.ewrite` / `fmt.eprintln` (stderr).
- [ ] `sys.read_stdin(n) -> []u8` and a line read (needed for the LSP only;
      `loamc` itself can ship without it).
- [ ] Spawn without the shell + wait, or at least `exec` with argv quoting
      (`str.shell_quote` exists) and a background/parallel form. Optional for
      a first self-host — serial `cc` is correct, just slower. Must work on
      Windows (no `fork`).
- [ ] Read a file as bytes with an explicit error (today `read_file` returns
      `""` on failure, indistinguishable from an empty file).
- [ ] Path separators and absolute-path rules that work on Windows
      (`\` vs `/`, drive letters). Prefer `/` everywhere in the language
      and normalize in the runtime.

### 1.6 Cross-platform runtime and host shims (Linux + Windows)

The existing runtime is oriented around macOS (Cocoa) + Linux X11 + WASM.
Self-hosting the compiler itself mainly needs a solid CLI path, but the
language must not regress.

Needed:

- [ ] Windows host shim: console I/O, process spawn, file timestamps, and
      basic path handling without requiring WSL.
- [ ] Linux path that does not assume X11 for non-GUI tools (the compiler
      and `nob` must build and run headless).
- [ ] Consistent `sys` surface so the same Loam source for `loamc` and
      `nob` works on macOS, Linux, and Windows.
- [ ] Document which C compilers are supported on each platform
      (clang, gcc, MSVC cl.exe) and how `loamc` locates them.

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

- [ ] Integer-keyed map (`IntMap<T>`) for type-id / node-id lookups;
      building a decimal string per lookup is a large constant cost.
- [ ] Confirm `Map<T>` with a non-Copy `T` (e.g. `Map<[]int>`) works and
      mutating a value in place is possible without get/put copies.

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
- [ ] **Fixed-point check** in `nob`: C-`loamc` builds Loam-`loamc` (stage 1);
      stage 1 builds itself (stage 2); stage 1 and stage 2 must emit
      byte-identical C for the compiler and for every test in `compile_pass`
      / `golden`. Run the check on at least one Linux and one Windows CI
      runner.
- [ ] **Deterministic output**: no pointer-address ordering, no hash-order
      iteration in emitted C (iterate maps by insertion order or sorted keys).
- [ ] **Differential testing**: for each `tests/compile_pass`, `golden`, and
      `compile_fail` file, both compilers must produce the same C / the same
      diagnostics. This is the main safety net during the port.
- [ ] Decide what moves and what stays C: `lsp.c` and `fmt.c` can follow
      later; the formatter is an easy early port (lexer + printer only).
- [ ] **Windows CI**: at minimum a GitHub Actions or equivalent job that
      builds the C seed, runs the self-host stages, and executes the
      language tests.

---

## 4. Suggested order

1. **Language fixes**: recursive types (§1.1), char literals (§1.4),
   `u64` printing (§1.3). Each gets `compile_pass` tests.
2. **Std + cross-platform + C FFI**: string builder (§1.2), number conversion (§1.3),
   stderr, `mtime`, int-keyed map (§2.3), Windows/Linux path and process
   surface (§1.5, §1.6), and the C header import / `.c` linking support (§1.7).
3. **Port the lexer** to Loam; differential-test its token stream against
   `lexer.c` over every `.loam` file in the repo.
4. **Port the parser** (arena AST) and `loam-fmt` on top of it — the
   formatter's output is an exact, easily diffed check of the parser.
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
- [ ] Recursive types work without crash or hang (§1.1)
- [ ] Character literals `'a'`, `'\n'`, etc. (§1.4)
- [ ] Correct `u64` printing and literal parsing (§1.3)
- [ ] String builder with O(1) append + write-bytes-to-file (§1.2)
- [ ] `int`/`u64`/`float` ↔ string conversion in std (§1.3)
- [ ] `fmt.eprintln` / stderr (§1.5)
- [ ] `sys.mtime` and file-read that returns an error (§1.5)
- [ ] Portable process spawn/wait and path handling for Linux + Windows (§1.5, §1.6)

### Compiler port
- [ ] Lexer in Loam, differential-tested against `lexer.c`
- [ ] Parser in Loam (arena AST), differential-tested via formatter or AST dump
- [ ] Typecheck, borrowck, boundscheck, IR, and codegen ported
- [ ] Driver (module loading, package resolution, `cc` invocation) in Loam
- [ ] Compiler itself uses only safe Loam (arenas + integer indices; §2.7)

### Fixed-point and tooling
- [ ] Stage 0 (C) builds stage 1 (Loam)
- [ ] Stage 1 builds stage 2, byte-identical C output
- [ ] Differential tests pass for `compile_pass`, `golden`, `compile_fail`
- [ ] Deterministic emission (no pointer or hash-order dependence)
- [ ] Fresh checkout builds on macOS, Linux, and Windows from the C seed
- [ ] CI runs the fixed-point check on at least one Linux and one Windows runner

When every box above is checked, Loam is 100% self-hosted.
