# Threads: what exists and what to add

Moved out of [self-hosting-loam.md](self-hosting-loam.md) (§5.1) on
2026-11-12. **This is not part of the 100% self-hosting claim.** The
compiler is single-threaded and needs nothing here; it is checked after the
claim. Related plans: [security.md](security.md) (S9 relies on T4) and
[runtime-in-loam.md](runtime-in-loam.md) (R3 lands with T1).

## What exists

Checked on 2026-11-11 against `std/thread.loam`, `src/threadcheck.c` (since
deleted) / `compiler/threadcheck.loam` and `runtime/loam_rt.h`:

- `thread.spawn(cb)` runs a closure or a named fn on a new detached pthread.
  `thread.chan<T>(cap, init)` makes a bounded FIFO with blocking `send` /
  `recv` and non-blocking `try_send` / `try_recv` / `ready`, guarded by a
  mutex and two condition variables in the runtime.
- The Send discipline is checked at compile time (`threadcheck.loam`). A
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

## Limits

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

## Plan, smallest first

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
