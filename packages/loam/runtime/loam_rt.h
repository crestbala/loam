/* loam_rt.h — panics, allocation, strings and vecs. Pasted at the top of generated C. */
#ifndef LOAM_RT_H
#define LOAM_RT_H

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#ifdef _WIN32
/* Windows host shim (MinGW-w64 / clang targeting *-windows-gnu). The runtime
   is written against POSIX; this block supplies the few pieces Windows lacks
   so the same CLI surface builds there. `/` works as a separator in every
   Win32 file API, so paths stay `/` on the Loam side. */
#include <io.h>
#include <direct.h>
#include <process.h>
#define lstat stat
#endif
#ifndef __wasm32__
#include <time.h>
#include <setjmp.h>
#endif
/* Recoverable traps. A panic that would abort instead winds the stack back to
   the innermost arm installed through `loam_trap_guard`, which returns the
   panic message in place of its body's result.

   The state is part of the runtime and the runtime defines it (see the
   LOAM_RT_DEFINE_JMP block below), not the library that happens to want a
   recoverable region: `loam_panic` is *here*, so the target it longjmps to has
   to be here too. Before this moved, the only writer of `loam_jmp_top` in the
   whole tree was `zeus_plat.c` — a library translation unit owning a runtime
   symbol, and a panic from any other library silently aborting because nobody
   had installed an arm.

   Exactly one translation unit defines the storage: the generated program
   (codegen_c.c emits `#define LOAM_RT_DEFINE_JMP`). On wasm there is no
   setjmp, so only the message exists: a panic records it and traps, and the
   host (the web loader) catches the trap at the export boundary and asks the
   program to recover (`zeus_recover`), which reads it back. An arm leaves the
   state NULL, so a trap with no guard still aborts. */
#ifndef __wasm32__
typedef struct LoamJmp {
    jmp_buf jb;
    struct LoamJmp *prev;
} LoamJmp;
#endif
#ifdef LOAM_RT_DEFINE_JMP
#ifndef __wasm32__
LoamJmp *loam_jmp_top = NULL;
#endif
char loam_jmp_msg[512];
void loam_jmp_set_msg(const char *msg, int64_t len) {
    int64_t n = len;
    if (!msg) n = 0;
    if (n < 0) n = 0;
    if (n > (int64_t)sizeof(loam_jmp_msg) - 1) n = (int64_t)sizeof(loam_jmp_msg) - 1;
    if (n > 0) memcpy(loam_jmp_msg, msg, (size_t)n);
    loam_jmp_msg[n] = 0;
}
#else
#ifndef __wasm32__
extern LoamJmp *loam_jmp_top;
#endif
extern char loam_jmp_msg[512];
void loam_jmp_set_msg(const char *msg, int64_t len);
#endif

/* The rest of the runtime's process-wide state (the command line, the last
   `sys.exec` status, the spawn, future and channel tables) has one definition
   too, in the same unit as the trap state. A shard build compiles the program
   as many translation units, and each would otherwise get its own copy of a
   `static`: `sys.arg(0)` read "" outside the unit holding `main`, and a spawn
   handle or a future made in one unit did not exist in the next.
   `LOAM_RT_STATE` declares, and `LOAM_RT_INIT(v)` gives the initial value only
   where the storage is defined. */
#ifdef LOAM_RT_DEFINE_JMP
#define LOAM_RT_STATE
#define LOAM_RT_INIT(...) = __VA_ARGS__
#else
#define LOAM_RT_STATE extern
#define LOAM_RT_INIT(...)
#endif

typedef struct {
    const char *ptr;
    int64_t len;
    /* 1 when `ptr` is a heap buffer this value owns and must release; 0 when
       `ptr` aliases static storage (a literal), where release is a no-op.
       Every compound literal in the tree omits this field and so defaults to
       0, which is exactly the old (never-freed) behavior. Kept as a field
       rather than a header before `ptr` so `loam_str_release` never reads
       memory it does not own. */
    int64_t own;
} loam_str;

typedef struct {
    void *fn;
    void *env;
    /* Byte size of `env` when it is a closure heap/stack record. intern_fn
       memcpy's this so the handler outlives the value that was interned. */
    size_t env_size;
} loam_fn;

typedef struct {
    void *ptr;
    int64_t len;
    int64_t cap;
} loam_vec;

/* Length-based writes to a descriptor. No malloc, no printf, no NUL
   requirement. Stdout is `fmt.println`; stderr is `fmt.eprintln`. */
static inline void loam_write_fd(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (w == 0) return;
        p += (size_t)w;
        n -= (size_t)w;
    }
}

static inline void loam_write_bytes(const char *p, size_t n) { loam_write_fd(STDOUT_FILENO, p, n); }
static inline void loam_ewrite_bytes(const char *p, size_t n) { loam_write_fd(STDERR_FILENO, p, n); }

/* Panics never return; out of line, a check is just a compare and branch. */
#if defined(__GNUC__) || defined(__clang__)
#define LOAM_COLD __attribute__((noinline, noreturn, unused))
#else
#define LOAM_COLD
#endif

/* The panic path is Loam (`std:panic`: flush, print, unwind or abort).
   These hand it C's strings as views; the hooks below are what it cannot
   do itself. */
void loam_panic_report(loam_str file, int32_t line, loam_str msg);
void loam_panic_trap(loam_str msg);
static inline loam_str loam_panic_view(const char *s) {
    loam_str v = {s ? s : "", s ? (int64_t)strlen(s) : 0, 0};
    return v;
}

static LOAM_COLD void loam_panic(const char *file, int line, const char *msg) {
    loam_panic_report(loam_panic_view(file), line, loam_panic_view(msg));
    abort();
}

/* `panic(msg)`: the message is a Loam string. */
static LOAM_COLD void loam_panic_str(const char *file, int line, loam_str msg) {
    loam_panic_report(loam_panic_view(file), line, msg);
    abort();
}

static inline void loam_panic_record(loam_str msg) { loam_jmp_set_msg(msg.ptr, msg.len); }
#ifndef __wasm32__
static inline void loam_panic_unwind(void) { if (loam_jmp_top) longjmp(loam_jmp_top->jb, 1); }
#else
static inline void loam_panic_unwind(void) {}
static inline void loam_panic_wasm_abort(void) { abort(); }
#endif

/* A web release has thousands of checked operations, and a file and line for
   each one's panic path came to a tenth of the download. They all share this
   instead, and the panic keeps its message. */
#ifdef LOAM_NO_TRAP_LOC
__attribute__((noinline, noreturn, cold)) static void loam_trap(const char *msg) {
    loam_panic_trap(loam_panic_view(msg));
    abort();
}
#define loam_panic(f, l, msg) loam_trap(msg)
#endif

/* Checked, wrapping and saturating integer ops, narrowing conversions and the
   bounds check are not here: codegen defines each one a program calls, right
   after this header (codegen.loam, `int_helper_defs`). */

/* Allocation accounting for the animation proof harness (phase 3). Only a Zeus
   program turns this on (`-DLOAM_ALLOC_TRACE`): the counter is the runtime's own
   `loam_alloc_note`, which `zeus.proof_allocs()` reads back through
   `loam_alloc_count`. Every other program keeps the no-op inline.

   The storage lives here, with the counter it belongs to, and the generated
   program defines it (`codegen_c.c` emits `#define LOAM_RT_DEFINE_ALLOC` beside
   the trap state). It used to be defined inside `zeus_plat.c` while this header
   declared it `extern` — the language runtime's accounting depending on a Zeus
   translation unit, an inversion the C seam forbids. */
#ifdef LOAM_ALLOC_TRACE
#ifdef LOAM_RT_DEFINE_ALLOC
int64_t loam_alloc_count = 0;
#else
extern int64_t loam_alloc_count;
#endif
static inline void loam_alloc_note(size_t sz) {
    (void)sz;
    loam_alloc_count++;
}
static inline int64_t loam_alloc_total(void) { return loam_alloc_count; }
#else
static inline void loam_alloc_note(size_t sz) { (void)sz; }
static inline int64_t loam_alloc_total(void) { return 0; }
#endif

static inline void *loam_new(size_t sz, const char *f, int l) {
    void *p = malloc(sz ? sz : 1);
    if (!p) loam_panic(f, l, "out of memory");
    loam_alloc_note(sz);
    return p;
}

/* --- owned strings ------------------------------------------------------
   A heap `loam_str` carries an int64 refcount in the word before `ptr` and is
   freed when the last reference goes away. `own` marks a value that holds that
   header; a literal leaves it 0 and is immortal, so retain/release are no-ops
   and no header is ever read for static storage. Sharing is a counter bump,
   never a byte copy, so a `Signal<string>` read costs one atomic increment —
   the same deal `[]T` gets — and the user never writes `.clone()`. The counter
   is atomic because a string is still a Send payload: it may move to a worker,
   and whichever reference is freed last releases the buffer. */
static inline int64_t *loam_str_rc(const char *ptr) {
    return ptr ? ((int64_t *)ptr - 1) : NULL;
}

/** Heap bytes for `n` characters, refcount 1, trailing NUL. */
static inline char *loam_str_bytes(int64_t n) {
    int64_t m = n > 0 ? n : 0;
    int64_t *blk = (int64_t *)loam_new(sizeof(int64_t) + (size_t)m + 1, "str", 0);
    blk[0] = 1;
    return (char *)(blk + 1);
}

/* `.len` is a Loam `int` (i32), so no string or vec may hold more. */
#define LOAM_LEN_MAX INT32_MAX

static inline loam_str loam_str_owned(char *p, int64_t n) {
    if (n > LOAM_LEN_MAX) loam_panic("string", 0, "longer than an int can index");
    loam_str s;
    s.ptr = p;
    s.len = n;
    s.own = 1;
    return s;
}

/** An owned copy of `n` bytes at `src`. Every runtime fn that hands Loam
 *  bytes it produced goes through here (or `loam_str_bytes`): a bare
 *  `(loam_str){malloc'd, n}` has `own == 0`, so the drop the compiler emits
 *  is a no-op and the buffer leaks. Empty for `n <= 0`. */
static inline loam_str loam_str_copy(const char *src, int64_t n) {
    char *p;
    if (n <= 0 || !src) return (loam_str){"", 0, 0};
    p = loam_str_bytes(n);
    memcpy(p, src, (size_t)n);
    p[n] = 0;
    return loam_str_owned(p, n);
}

/* gcc follows the literal path (`own == 0`) into the refcount update and the
   free after inlining, and warns that it writes before a literal or frees
   one. The `own` test makes that path dead; clang does not warn. Silenced
   here only, for release (codegen's `loam_str_retain` does the same). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif

/** Drop one reference; the last one frees the buffer. */
static inline void loam_str_release(loam_str *s) {
    if (!s) return;
    if (s->own && s->ptr) {
        if (__atomic_sub_fetch(loam_str_rc(s->ptr), 1, __ATOMIC_ACQ_REL) == 0)
            free(loam_str_rc(s->ptr));
    }
    s->ptr = NULL;
    s->len = 0;
    s->own = 0;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static inline loam_vec loam_vec_new(void) {
    loam_vec v;
    v.ptr = NULL;
    v.len = 0;
    v.cap = 0;
    return v;
}

static inline int64_t *loam_vec_rc(void *ptr) {
    return ptr ? ((int64_t *)ptr - 1) : NULL;
}

static inline void loam_vec_unique(loam_vec *v, size_t esz, const char *f, int l) {
    int64_t *rc;
    void *raw;
    size_t bytes;
    if (!v || !v->ptr) return;
    rc = loam_vec_rc(v->ptr);
    if (*rc <= 1) return;
    bytes = sizeof(int64_t) + (esz ? (size_t)v->cap * esz : 1);
    raw = malloc(bytes);
    if (!raw) loam_panic(f, l, "out of memory");
    loam_alloc_note(bytes);
    *(int64_t *)raw = 1;
    if (v->len > 0 && esz)
        memcpy((char *)raw + sizeof(int64_t), v->ptr, (size_t)v->len * esz);
    (*rc)--;
    v->ptr = (char *)raw + sizeof(int64_t);
}

/* The capacity a vec of `cap` slots grows to so it holds `n`. A push that
   overflows a full buffer of 256 KB or more grows it 4x, not 2x: a big
   table (a compiler's AST, a log) is copied about half as many times on its
   way up, and its spare room stays under 3/4 of what it holds. Any other
   request (a first buffer, an exact size, a small vec) doubles up to `n`. */
#define LOAM_VEC_GROW4_BYTES ((int64_t)256 * 1024)
static inline int64_t loam_vec_grown_cap(int64_t cap, int64_t n, size_t esz, const char *f, int l) {
    /* Capacity stops at the limit too: a push into spare room skips this. */
    if (n > LOAM_LEN_MAX) loam_panic(f, l, "more elements than an int can index");
    if (cap > 0 && n == cap + 1 && esz && cap >= LOAM_VEC_GROW4_BYTES / (int64_t)esz &&
        cap <= INT64_MAX / 4)
        cap = cap * 4;
    else {
        if (cap <= 0) cap = 8;
        while (cap < n) cap *= 2;
    }
    return cap < LOAM_LEN_MAX ? cap : LOAM_LEN_MAX;
}

static inline void loam_vec_reserve(loam_vec *v, int64_t n, size_t esz, const char *f, int l) {
    int64_t cap;
    void *raw;
    if (!v) return;
    /* Whoever reserves writes past `len` next, so it must own the buffer even
       when it has room: a copy-on-write sibling with the same spare capacity
       would otherwise see (and overwrite) the same slots. */
    loam_vec_unique(v, esz, f, l);
    if (n <= v->cap) return;
    cap = loam_vec_grown_cap(v->cap, n, esz, f, l);
    if (!v->ptr) {
        raw = malloc(sizeof(int64_t) + (esz ? (size_t)cap * esz : 1));
        if (!raw) loam_panic(f, l, "out of memory");
        loam_alloc_note((size_t)cap * esz);
        *(int64_t *)raw = 1;
        v->ptr = (char *)raw + sizeof(int64_t);
        v->cap = cap;
        return;
    }
    raw = realloc((char *)v->ptr - sizeof(int64_t),
                  sizeof(int64_t) + (esz ? (size_t)cap * esz : 1));
    if (!raw) loam_panic(f, l, "out of memory");
    loam_alloc_note((size_t)cap * esz);
    v->ptr = (char *)raw + sizeof(int64_t);
    v->cap = cap;
}

/* Vec push (`loam_vec_push`, `_owned`, `_take`), vec pop (`loam_vec_pop`,
   `_owned`), and the moves, retains and drops of owning values
   (`loam_vec_move`, `loam_vec_retain`, `loam_vec_drop_owned`, `loam_vec_of`,
   `loam_vec_unique_owned`, `loam_vec_reserve_owned`, `loam_move_ptr`,
   `loam_drop`, `loam_str_retain`) are not here: codegen defines each one a
   program calls, right after this header (`vec_push_defs`, `vec_pop_defs`,
   `core_op_defs`). A fn value is copy and never dropped, so it has no move
   or drop. */

/* --- container element lifetime -----------------------------------------
   A `[]T` buffer owns one reference to each element that itself owns a string.
   So a copy (a bumped refcount) shares the buffer and its element references
   unchanged, a copy-on-write reallocation must retain every element it
   duplicates, and the elements are released exactly once — when the buffer's
   last reference goes, never per handle. The `_owned` variants take the
   element retain/release hook the compiler emits for `T`;
   `loam_vec_drop_owned` is what a shared vec parameter must use, so dropping
   it does not zero slots the caller still reads. */
typedef void (*loam_elem_fn)(void *elem);

static inline void loam_vec_drop(loam_vec *v) {
    int64_t *rc;
    if (!v) return;
    if (!v->ptr) {
        v->len = 0;
        v->cap = 0;
        return;
    }
    rc = loam_vec_rc(v->ptr);
    if (--(*rc) > 0) {
        v->ptr = NULL;
        v->len = 0;
        v->cap = 0;
        return;
    }
    free(rc);
    v->ptr = NULL;
    v->len = 0;
    v->cap = 0;
}

/* --- recoverable traps ---------------------------------------------------

   Run `build` on the stack of an installed arm. Returns "" when the body
   completed, else the panic message — the same contract a library-level
   boundary wants, expressed as one runtime verb rather than as library code in
   C. Nesting works: the arm chains to the previous target, so the inner-most
   guard wins and an outer one stays installed.

   On wasm there is no setjmp, so the body runs unprotected and a trap aborts.
   That is the honest answer for the platform, not a missing implementation: a
   wasm trap is a host-visible abort either way, and faking recovery over one
   would cost more than it buys.

   `build`'s env is borrowed, not copied: the call is synchronous, so the frame
   that owns the env is still live for the whole body. */
static inline loam_str loam_trap_guard(loam_fn build) {
    loam_str none;
    none.ptr = "";
    none.len = 0;
    none.own = 0;
#ifndef __wasm32__
    LoamJmp b;
    b.prev = loam_jmp_top;
    loam_jmp_top = &b;
    loam_jmp_msg[0] = 0;
    if (setjmp(b.jb) == 0) {
        if (build.fn) ((void (*)(void *))build.fn)(build.env);
        loam_jmp_top = b.prev;
        return none;
    }
    loam_jmp_top = b.prev;
    {
        loam_str msg;
        msg.ptr = loam_jmp_msg;
        msg.len = (int64_t)strlen(loam_jmp_msg);
        msg.own = 0;
        return msg;
    }
#else
    if (build.fn) ((void (*)(void *))build.fn)(build.env);
    return none;
#endif
}

/* --- runtime verbs libraries read ----------------------------------------

   The numbers a UI library needs and should not have to own: the monotonic
   clock its deadlines run on, the live heap, and the allocation counter above.
   Each is defined once, here, so a library asks the runtime instead of reaching
   into a sibling translation unit.

   On wasm the clock and the heap come from the host, which declares the two
   imports below. Both are declared *once* in this file with their import
   attributes: the host's own C declares them too, and a second declaration
   without the attribute makes wasm-ld reject the pair with "import module
   mismatch for symbol: zeus_js_now_ms" (one object says `zeus`, the other
   `env`). The `__wasm32__` ordering matters as well — the import block lives
   further down, so the attribute-bearing declaration is the one that wins. */
#if defined(__wasm32__)
/* `zeus_js_now_ms` is the browser's `performance.now` import, whose attribute
   must appear exactly once for the whole link — the host's own C declares it
   too, and a second declaration without the attribute makes wasm-ld reject the
   pair with "import module mismatch for symbol: zeus_js_now_ms" (one object
   says `zeus`, the other `env`), which is why it is declared here and not
   redeclared below. `zeus_heap_used` is not an import at all: it is the wasm
   libc's own allocator statistic (codegen.loam, `wasm_libc_defs`). */
__attribute__((import_module("zeus"), import_name("now_ms")))
int64_t zeus_js_now_ms(void);
int32_t zeus_heap_used(void);
static inline int64_t loam_now_ms(void) { return zeus_js_now_ms(); }
static inline int64_t loam_heap_used(void) { return (int64_t)zeus_heap_used(); }
#else
static inline int64_t loam_now_ms(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}
/* No process footprint on a native host here: the platform layer answers that
   through its own OS call (mach task_info, /proc), so this stays 0 as the
   "unknown" answer `plat_mem_kb` documents. */
static inline int64_t loam_heap_used(void) { return 0; }
#endif

#if !defined(__wasm32__) && !defined(_WIN32)
#include <sys/wait.h>
#endif
#ifndef __wasm32__
#include <sys/stat.h>
#include <dirent.h>
/* The development-host seam (`std:host`): hand the process to a `zeli serve` host
   that loads the app as a shared image. Declared here because that is where a
   bodyless std fn is seen by generated code; `zeus_plat.c` defines it — as a
   no-op outside a host build, so an app that calls it still runs its own loop. */
void loam_host_run(loam_str path);
#endif

/* The fmt writers, `str.slice`, `string_from_bytes`, the `buf` / `decimal` /
   `conv` / `floattext` bridges, `sys`'s argv, read and C shims, the wasm async
   bridges, and threads and channels are not here: codegen defines each group
   a program calls, right after this header (codegen.loam, `rt_verb_defs`). */
#endif /* LOAM_RT_H */
