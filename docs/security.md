# Security plan: what holds, what does not, and what to do

Moved out of [self-hosting-loam.md](self-hosting-loam.md) (§5.2) on
2026-11-12. **None of this is part of the 100% self-hosting claim.** That
claim needs memory safety from safe Loam (definition item 6), and it holds
since the `Box` fix ([box-and-init-ownership.md](box-and-init-ownership.md)).
The work here hardens what surrounds it: checker soundness found by a
generator rather than by accident, the trusted C, FFI, the shell, servers,
the binary and the seed. It is checked after self-hosting is declared.
The runtime's C moving to Loam, which shrinks the trusted C of gap 2, has
its own plan in [runtime-in-loam.md](runtime-in-loam.md).

Every item names the probe or test that closes it, and follows the
essentialist rule. S1 and S2 come first: they look for holes like the ones
below, and any hole found in safe Loam is fixed at once, whatever else is
in flight.

## What holds and what does not

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

Two holes reachable from safe Loam were found while writing this plan
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

Fixed (2026-11-12): **`Box::new` of a value with owned fields dangled**
once the box outlived its fn: `Box::new(Node { name: "n{{n}}" })`
returned from a fn read a freed string. Codegen copied the value into the
box without taking references (`Op.Alloc`), and a box's drop was shallow, so
the two hid each other. Both halves, the fix and its checks (the
`ownership_asan` stanza, the compiler under ASan) are in
[box-and-init-ownership.md](box-and-init-ownership.md) (item 0a of "Next
changes (pending)" in [self-hosting-loam.md](self-hosting-loam.md)).

## Gaps, by how much they matter

1. **The checkers are not proven sound.** The port found five holes, each
   fixed in both compilers while both existed:
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
6. **Unbounded memory growth.** Capturing closures are never freed
   ([self-hosting-loam.md](self-hosting-loam.md) §2.4),
   so a long-running server that makes one per request grows until it is
   killed.
7. **Hash flooding.** `std:map` hashes with unkeyed FNV, truncated to 26
   bits. A program that keys a map by untrusted input (headers, JSON object
   keys) can be pushed to quadratic time.
8. **No hardening of the binary.** Native builds use `-O1` with no
   `-fstack-protector-strong`, `-D_FORTIFY_SOURCE`, or full RELRO / PIE
   flags. These are defence in depth for the trusted C and FFI code
   (gaps 2–3), not for safe Loam.
9. **Supply chain of the seed.** The seed (`bootstrap/loamc.c`) is the
   compiler's generated C. Nobody reviews 4.4 MB of C by eye, so its trust
   must come from the fixed point: anyone can regenerate it from the Loam
   sources with the previous seed and compare bytes.

## Plan, in order

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
      - Both codegens (the C one, since deleted, and `codegen.loam`) emit the
        key beside the `sizeof` they already pass (`handle_key`), so the
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
      that also covers moves, globals, closures, threads and vec growth, and
      run the generated programs under ASan + UBSan. Keep a fixed seed range
      as a quick stanza, and a long run on a schedule. Every hole it finds
      becomes a `compile_fail` or `compile_pass` fixture and is fixed in the
      compiler. `Box` construction and drops belong in the generator (the hole
      of [box-and-init-ownership.md](box-and-init-ownership.md) was found by
      accident). Write the borrow rules in
      [reference-escape.md](reference-escape.md) as a numbered list, each with
      its fixtures, so a missing case is visible.
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
      cost on the compiler building itself and a Zeus app first; dev builds
      keep their flags.
- [ ] **S6. Stack overflow with a message.** An alternate signal stack and a
      `SIGSEGV` handler that checks the fault address against the guard page
      and prints `panic: stack overflow` before aborting (the same on worker
      threads). On Windows, the `EXCEPTION_STACK_OVERFLOW` filter. Recursive
      parsers in std (`std:json`) get a depth limit that returns an error.
- [ ] **S7. Keyed map hash.** Seed `std:map`'s hash per process from the OS
      random source, so an attacker cannot predict collisions. Emitted C
      stays deterministic, because the compiler never iterates a map
      ([self-hosting-loam.md](self-hosting-loam.md), Fixed-point and
      tooling). Check the cost on the compiler building itself.
- [ ] **S8. Fix the closure env leak** (self-hosting §2.4) before recommending
      Loam for long-running servers. Then any per-request allocation is
      bounded by the request.
- [ ] **S9. Trap containment for servers**, with no unwinding (self-hosting
      §2.5): the T4 process-per-worker model (self-hosting §5.1) already
      confines a trap to one worker, whose parent restarts it. Document this
      as the answer to "a panic kills the server" instead of adding recovery
      to the language.
- [ ] **S10. Reproducible seed.** A `nob` check that regenerates the
      committed seed C from the Loam sources with the previous seed and
      fails on any byte difference (this is the fixed point, run against the
      committed file). Combined with S1/S2, it makes the seed trustworthy
      without a line-by-line review.
