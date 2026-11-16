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

static inline void loam_fmt_write(loam_str s) {
    if (!s.ptr || s.len <= 0) return;
    loam_write_bytes(s.ptr, (size_t)s.len);
}

/* The stderr twins of the writers above, for `fmt.eprintln` / `fmt.ewrite`:
   diagnostics must not mix with output a program sends to stdout. */
static inline void loam_fmt_ewrite(loam_str s) {
    if (!s.ptr || s.len <= 0) return;
    loam_ewrite_bytes(s.ptr, (size_t)s.len);
}

static inline bool loam_fmt_eq(loam_str a, loam_str b) {
    if (a.len != b.len) return false;
    if (a.len <= 0) return true;
    if (!a.ptr || !b.ptr) return a.ptr == b.ptr;
    return memcmp(a.ptr, b.ptr, (size_t)a.len) == 0;
}

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
   translation unit, which is the one inversion docs/boundary.md forbids. */
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

static inline void loam_drop(void **p) {
    if (p && *p) {
        free(*p);
        *p = NULL;
    }
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

/** std:str `slice`: `s[a..b]`, bounds clamped to the string, in one copy. */
static inline loam_str loam_str_slice(loam_str s, int32_t a, int32_t b) {
    int64_t lo = a < 0 ? 0 : a;
    int64_t hi = b > s.len ? s.len : b;
    if (lo >= hi) return (loam_str){"", 0, 0};
    return loam_str_copy(s.ptr + lo, hi - lo);
}

/* gcc follows the literal path (`own == 0`) into the refcount update and the
   free after inlining, and warns that it writes before a literal or frees
   one. The `own` test makes that path dead; clang does not warn. Silenced
   here only, for these two functions. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif

/** Take another reference to `s` (no-op for a literal). */
static inline void loam_str_retain(loam_str s) {
    if (s.own && s.ptr)
        __atomic_add_fetch(loam_str_rc(s.ptr), 1, __ATOMIC_RELAXED);
}

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

static inline void *loam_move_ptr(void **src) {
    void *p = src ? *src : NULL;
    if (src) *src = NULL;
    return p;
}

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

static inline loam_vec loam_vec_retain(const loam_vec *src) {
    loam_vec v = loam_vec_new();
    if (!src) return v;
    v = *src;
    if (v.ptr) {
        int64_t *rc = loam_vec_rc(v.ptr);
        (*rc)++;
    }
    return v;
}

static inline loam_vec loam_vec_move(loam_vec *src) {
    loam_vec v;
    v.ptr = NULL;
    v.len = 0;
    v.cap = 0;
    if (src) {
        v = *src;
        src->ptr = NULL;
        src->len = 0;
        src->cap = 0;
    }
    return v;
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

#if defined(__GNUC__) || defined(__clang__)
#define LOAM_NOINLINE __attribute__((noinline, unused))
#define LOAM_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define LOAM_NOINLINE
#define LOAM_LIKELY(x) (x)
#endif

/* The push that grows or un-shares the buffer: out of line, so the common
   push below stays small enough to inline with its element size known. */
static LOAM_NOINLINE void loam_vec_push_slow(loam_vec *v, const void *elem, size_t esz,
                                             const char *f, int l) {
    if (!v) loam_panic(f, l, "push on null array");
    loam_vec_reserve(v, v->len + 1, esz, f, l);
    if (esz && elem) memcpy((char *)v->ptr + (size_t)v->len * esz, elem, esz);
    v->len++;
}

static inline void loam_vec_push(loam_vec *v, const void *elem, size_t esz, const char *f, int l) {
    /* An owned buffer with room: one element copy. */
    if (LOAM_LIKELY(v && v->ptr && v->len < v->cap && *loam_vec_rc(v->ptr) == 1)) {
        if (esz && elem) memcpy((char *)v->ptr + (size_t)v->len * esz, elem, esz);
        v->len++;
        return;
    }
    loam_vec_push_slow(v, elem, esz, f, l);
}

/* A fresh list holding a copy of `n` plain elements, for a constant table. */
static inline loam_vec loam_vec_of(const void *src, int64_t n, size_t esz, const char *f, int l) {
    loam_vec v = loam_vec_new();
    loam_vec_reserve(&v, n, esz, f, l);
    memcpy(v.ptr, src, (size_t)n * esz);
    v.len = n;
    return v;
}

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

static inline void loam_vec_unique_owned(loam_vec *v, size_t esz, loam_elem_fn retain,
                                         const char *f, int l) {
    int64_t *rc;
    void *raw;
    size_t bytes;
    int64_t i;
    if (!v || !v->ptr) return;
    rc = loam_vec_rc(v->ptr);
    if (*rc <= 1) return;
    bytes = sizeof(int64_t) + (esz ? (size_t)v->cap * esz : 1);
    raw = malloc(bytes);
    if (!raw) loam_panic(f, l, "out of memory");
    loam_alloc_note(bytes);
    *(int64_t *)raw = 1;
    if (v->len > 0 && esz) {
        memcpy((char *)raw + sizeof(int64_t), v->ptr, (size_t)v->len * esz);
        if (retain)
            for (i = 0; i < v->len; i++)
                retain((char *)raw + sizeof(int64_t) + (size_t)i * esz);
    }
    (*rc)--;
    v->ptr = (char *)raw + sizeof(int64_t);
}

static inline void loam_vec_reserve_owned(loam_vec *v, int64_t n, size_t esz,
                                          loam_elem_fn retain, const char *f, int l) {
    int64_t cap;
    void *raw;
    if (!v) return;
    /* As loam_vec_reserve: a writer past `len` must own the buffer. */
    loam_vec_unique_owned(v, esz, retain, f, l);
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

static LOAM_NOINLINE void loam_vec_push_owned_slow(loam_vec *v, const void *elem, size_t esz,
                                                   loam_elem_fn retain, const char *f, int l) {
    loam_vec_reserve_owned(v, v->len + 1, esz, retain, f, l);
    if (esz && elem) memcpy((char *)v->ptr + (size_t)v->len * esz, elem, esz);
    v->len++;
}

static inline void loam_vec_push_owned(loam_vec *v, const void *elem, size_t esz,
                                       loam_elem_fn retain, const char *f, int l) {
    if (!v) loam_panic(f, l, "push on null array");
    if (retain) retain((void *)elem);
    /* An owned buffer with room: one element copy (see loam_vec_push). */
    if (LOAM_LIKELY(v->ptr && v->len < v->cap && *loam_vec_rc(v->ptr) == 1)) {
        if (esz && elem) memcpy((char *)v->ptr + (size_t)v->len * esz, elem, esz);
        v->len++;
        return;
    }
    loam_vec_push_owned_slow(v, elem, esz, retain, f, l);
}

/* As loam_vec_push_owned, for an element handed over whole (a temporary or
   a moved value): the slot takes the references it holds, so nothing is
   retained. `retain` still copies the elements if the buffer is shared. */
static inline void loam_vec_push_take(loam_vec *v, const void *elem, size_t esz,
                                      loam_elem_fn retain, const char *f, int l) {
    if (!v) loam_panic(f, l, "push on null array");
    if (LOAM_LIKELY(v->ptr && v->len < v->cap && *loam_vec_rc(v->ptr) == 1)) {
        if (esz && elem) memcpy((char *)v->ptr + (size_t)v->len * esz, elem, esz);
        v->len++;
        return;
    }
    loam_vec_push_owned_slow(v, elem, esz, retain, f, l);
}

static inline void loam_vec_drop_owned(loam_vec *v, size_t esz, loam_elem_fn release) {
    int64_t *rc;
    int64_t i, n;
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
    n = v->len;
    if (release)
        for (i = 0; i < n; i++) release((char *)v->ptr + (size_t)i * esz);
    free(rc);
    v->ptr = NULL;
    v->len = 0;
    v->cap = 0;
}

static inline void loam_vec_pop(loam_vec *v, void *out, size_t esz, const char *f, int l) {
    if (!v || v->len <= 0) loam_panic(f, l, "pop from empty array");
    loam_vec_unique(v, esz, f, l);
    v->len--;
    if (esz && out) memcpy(out, (char *)v->ptr + (size_t)v->len * esz, esz);
}

/* Pop an element that owns a string. The copy-on-write reallocation retains
   every element it duplicates, so the popped slot's own reference transfers to
   `out` (the slot is now past `len`, so the buffer never releases it) and the
   remaining elements stay owned by the buffer. Without the retain hook the
   shared element is released by both buffers. */
static inline void loam_vec_pop_owned(loam_vec *v, void *out, size_t esz, loam_elem_fn retain,
                                      const char *f, int l) {
    if (!v || v->len <= 0) loam_panic(f, l, "pop from empty array");
    loam_vec_unique_owned(v, esz, retain, f, l);
    v->len--;
    if (esz && out) memcpy(out, (char *)v->ptr + (size_t)v->len * esz, esz);
}

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

static inline loam_fn loam_fn_move(loam_fn *src) {
    loam_fn v;
    v.fn = NULL;
    v.env = NULL;
    v.env_size = 0;
    if (src) {
        v = *src;
        src->fn = NULL;
        src->env = NULL;
        src->env_size = 0;
    }
    return v;
}

static inline void loam_fn_drop(loam_fn *f) {
    if (!f) return;
    if (f->env) free(f->env);
    f->env = NULL;
    f->fn = NULL;
}

/* --- env lifetime: copy a closure env, and drop one -----------------------

   A `fn` value's `env` is a stack record owned by the frame that created the
   closure, so storing the `loam_fn` alone gives a dangling env the moment that
   frame returns. `loam_fn_move` only *steals* — it is the move path, and it
   leaves the source empty. Anything that must keep a `fn` beyond its frame
   (interning a handler or a reactive thunk, a deferred callback) needs a copy
   the runtime allocates and the same code later frees.

   These two are that pair, and they are deliberately generic: they say nothing
   about widgets, handlers, or props, and any library that stores a `fn` value
   wants exactly this. Each takes and returns the raw env pointer so the caller
   never has to know whether `loam_fn.env` is a heap record or NULL. */
static inline void *loam_env_copy(const void *env, int64_t n) {
    void *p;
    if (!env || n <= 0) return NULL;
    p = loam_new((size_t)n, "env.copy", 0);
    memcpy(p, env, (size_t)n);
    return p;
}

static inline void loam_env_drop(void *env) {
    if (env) free(env);
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
   libc's own allocator statistic (`zeus_wasm_libc.c`). */
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

/* Takes ownership of `b` (IR_CALL steals []int). Trailing NUL for C hosts. */
static inline loam_str loam_string_from_bytes(loam_vec b) {
    int64_t n = b.len > 0 ? b.len : 0;
    char *p = loam_str_bytes(n);
    int32_t *el = (int32_t *)b.ptr;
    int64_t i;
    for (i = 0; i < n; i++)
        p[i] = (char)(el ? (el[i] & 255) : 0);
    p[n] = 0;
    loam_vec_drop(&b);
    return loam_str_owned(p, n);
}

/** `string_from_bytes` for a `[]u8`. A vec and a string share a layout, so
 *  an unshared buffer with room for the NUL becomes the string: no copy. */
static inline loam_str loam_string_from_u8(loam_vec b) {
    if (b.ptr && b.cap > b.len && *loam_vec_rc(b.ptr) == 1) {
        ((char *)b.ptr)[b.len] = 0;
        return loam_str_owned((char *)b.ptr, b.len);
    }
    int64_t n = b.len > 0 ? b.len : 0;
    char *p = loam_str_bytes(n);
    if (n) memcpy(p, b.ptr, (size_t)n);
    p[n] = 0;
    loam_vec_drop(&b);
    return loam_str_owned(p, n);
}

/* --- std:buf — byte builders over `[]u8` ----------------------------------
   One memcpy per append, amortized O(1) growth from loam_vec_reserve: N pieces
   cost O(total bytes), not the O(N^2) of re-concatenating strings. */
static inline void loam_buf_push_str(loam_vec *b, loam_str s) {
    if (!b || !s.ptr || s.len <= 0) return;
    loam_vec_reserve(b, b->len + s.len, 1, "buf.push_str", 0);
    memcpy((char *)b->ptr + b->len, s.ptr, (size_t)s.len);
    b->len += s.len;
}

static inline void loam_buf_push_bytes(loam_vec *b, const loam_vec *src) {
    /* The borrow rules keep `src` from being `b`; a copy-on-write sibling of
       `b` keeps its own reference, so its bytes outlive the reserve below. */
    if (!b || !src || !src->ptr || src->len <= 0) return;
    int64_t n = src->len;
    loam_vec_reserve(b, b->len + n, 1, "buf.push_bytes", 0);
    memcpy((char *)b->ptr + b->len, src->ptr, (size_t)n);
    b->len += n;
}

/* std:decimal hands on `d[first..20]`, digits it made on the stack. */
static inline int64_t loam_decimal_digits_len(int32_t first) {
    if (first < 0 || first > 20) loam_panic("decimal", 0, "digits out of range");
    return 20 - (int64_t)first;
}

static inline loam_str loam_decimal_text_of_digits(const uint8_t *d, int32_t first) {
    return loam_str_copy((const char *)d + first, loam_decimal_digits_len(first));
}

static inline void loam_decimal_push_digits(loam_vec *b, const uint8_t *d, int32_t first) {
    int64_t n = loam_decimal_digits_len(first);
    if (!b) return;
    loam_vec_reserve(b, b->len + n, 1, "buf.push_int", 0);
    memcpy((char *)b->ptr + b->len, d + first, (size_t)n);
    b->len += n;
}

static inline void loam_decimal_write_digits(const uint8_t *d, int32_t first, bool to_err) {
    int64_t n = loam_decimal_digits_len(first);
    if (to_err) loam_ewrite_bytes((const char *)d + first, (size_t)n);
    else loam_write_bytes((const char *)d + first, (size_t)n);
}

/** A string copy of the bytes; the buffer stays usable. */
static inline loam_str loam_buf_to_string(const loam_vec *b) {
    if (!b || !b->ptr || b->len <= 0) return (loam_str){"", 0};
    return loam_str_copy((const char *)b->ptr, b->len);
}

/* `buf.prefix_to_string(b, n)`: the first `n` bytes (clamped to the length)
   as a string, one exact copy. For a buffer the kernel filled part of. */
static inline loam_str loam_buf_prefix_to_string(const loam_vec *b, int64_t n) {
    if (!b || !b->ptr || n <= 0) return (loam_str){"", 0};
    return loam_str_copy((const char *)b->ptr, n < b->len ? n : b->len);
}

/* `buf.with_len(n)`: one allocation of `n` zeroed bytes, for a buffer C fills
   (`fread`, `getcwd`), instead of a push loop's check per byte. The spare
   byte keeps `string_from_bytes` copy-free when `n` is a power of two. */
static inline loam_vec loam_buf_with_len(int64_t n) {
    loam_vec v = loam_vec_new();
    if (n > 0) {
        loam_vec_reserve(&v, n + 1, 1, "buf.with_len", 0);
        memset(v.ptr, 0, (size_t)n);
        v.len = n;
    }
    return v;
}

/* `std:key` reads a float's bytes for its content key. A value conversion
   would round (`1.5 as u32` is 1); only `memcpy` sees the raw bytes. */
static inline uint32_t loam_conv_f32_bits(float v) {
    uint32_t out = 0;
    memcpy(&out, &v, sizeof out);
    return out;
}

/* std:floattext: a double's bits both ways, and the text it made on the
   stack, `d[0..len]`. */
static inline uint64_t loam_floattext_bits_of(double v) {
    uint64_t out = 0;
    memcpy(&out, &v, sizeof out);
    return out;
}
static inline double loam_floattext_of_bits(uint64_t u) {
    double out = 0;
    memcpy(&out, &u, sizeof out);
    return out;
}
static inline int64_t loam_floattext_text_len(int32_t len) {
    if (len < 0 || len > 32) loam_panic("floattext", 0, "text out of range");
    return (int64_t)len;
}
static inline loam_str loam_floattext_text_of(const uint8_t *d, int32_t len) {
    return loam_str_copy((const char *)d, loam_floattext_text_len(len));
}
static inline void loam_floattext_write_text(const uint8_t *d, int32_t len, bool to_err) {
    int64_t n = loam_floattext_text_len(len);
    if (to_err) loam_ewrite_bytes((const char *)d, (size_t)n);
    else loam_write_bytes((const char *)d, (size_t)n);
}

/* `sys.os` / `sys.arch` / `sys.cpu_count` / `sys.target` live in
   `std/sys.loam` (R2.3b): one `#[os]` / `#[arch]` variant per target, and
   `cpu_count` over raw `sysconf`. */

#ifdef __wasm32__
/* `sys.env` / `sys.env_set` / `sys.exit` live in `std/sys.loam` (R2.3c);
   wasm's `exit` traps through this hook. */
static inline void loam_sys_wasm_exit(void) { abort(); }
/* No process and no command line in wasm: the host calls the entry with no
   arguments, so `sys.argc()` is 0 and `sys.arg(i)` is empty. */
static inline void loam_argv_set(int argc, char **argv) {
    (void)argc;
    (void)argv;
}
static inline int64_t loam_sys_argc(void) { return 0; }
static inline loam_str loam_sys_arg(int64_t i) {
    (void)i;
    return (loam_str){"", 0};
}
/* There is no filesystem in wasm: nothing exists, nothing lists, nothing
   removes. A host that has one (a worker with a virtual FS) would implement
   these rather than Loam guessing. `read_file` / `write_file` / `exists` /
   `is_dir` / `realpath` / `dir_count` / `dir_entry` / `remove_path` /
   `write_bytes` / `mtime` / `stamp` answer from Loam `#[os("wasm")]` variants
   in `std/sys.loam` now (R2.2), so the stubs are gone. */
/* `sys.mkdir` / `sys.rename` / `sys.cwd` answer from Loam `#[os("wasm")]`
   variants too (R2.2): nothing is created, renamed, or listed. */
#else
#ifndef _WIN32
#include <sys/wait.h>
#endif

/* The process's command line, handed over by the generated entry before any
   other statement runs (`emit_main_args` in codegen_c.c). Zero on iOS and
   Android, where the host owns `main` and passes nothing. */
LOAM_RT_STATE int loam_argc;
LOAM_RT_STATE char **loam_argv;

static inline void loam_argv_set(int argc, char **argv) {
    loam_argc = argc;
    loam_argv = argv;
#ifdef _WIN32
    /* Bytes out are the bytes written: text mode would turn every `\n` into
       `\r\n`, and generated C must be identical on every platform. */
    _setmode(1, 0x8000 /* _O_BINARY */);
    _setmode(2, 0x8000);
#endif
}

static inline int64_t loam_sys_argc(void) { return (int64_t)loam_argc; }

/* Points straight into the process's own argument vector: it lives exactly as
   long as the process does, so there is nothing to copy and nothing to free. */
static inline loam_str loam_sys_arg(int64_t i) {
    if (i < 0 || i >= (int64_t)loam_argc || !loam_argv || !loam_argv[i])
        return (loam_str){"", 0};
    return (loam_str){loam_argv[i], (int64_t)strlen(loam_argv[i])};
}

/* `sys.env` / `sys.env_set` / `sys.exit` live in `std/sys.loam` (R2.3c):
   raw `getenv` / `exit`, the 256-byte name rule in Loam. */

/* `sys.write_file` / `sys.write_bytes` live in `std/sys.loam` too (R2.2):
   same stdio bindings, one `fwrite` of the text or the buffer. */

/* `sys.exec` / `sys.exec_status` live in `std/sys.loam` (R2.3d): raw
   `popen` / `fread` / `pclose`, the wait status decoded in Loam. */

/* The development-host seam (`std:host`): hand the process to a `zeli serve` host
   that loads the app as a shared image. Declared here because that is where a
   bodyless std fn is seen by generated code; `zeus_plat.c` defines it — as a
   no-op outside a host build, so an app that calls it still runs its own loop. */
void loam_host_run(loam_str path);

/* `sys.read_file` / `sys.io_error` / `sys.write_file` live in `std/sys.loam`
   now (R2.2): stdio `#[c]` bindings over `FILE*`-as-`i64` (`fopen` / `fread` /
   `fwrite` / `fclose` raw from libc) plus one `strerror` binding and a
   module-level error slot. The wasm stubs answer from Loam. */

/* `strerror(errno)` for the `sys` Loam bindings: the error is always the
   current one, so the binding takes no argument and C reads `errno` itself.
   A `const char *`, the `#[c]` string-return ABI: the wrapper copies it (a
   `loam_str` return would come back through a struct-return slot the wrapper
   never passes). Declared here, defined once in the owning unit (the
   `LOAM_RT_DEFINE_JMP` block below), like the trap state: the `#[c]` wrapper
   references it as a symbol. */
const char *loam_sys_strerror(void);
#ifdef LOAM_RT_DEFINE_JMP
const char *loam_sys_strerror(void) {
    return strerror(errno);
}
#endif

/* `sys.write_bytes` lives in `std/sys.loam` too (R2.2): same stdio bindings,
   one `fwrite` of the buffer. */

#include <sys/stat.h>

/* One `stat(2)` for the `sys` Loam bindings: the layout differs per OS, so
   Loam cannot read the struct itself. `out` takes five slots
   (mtime-sec, mtime-nsec, size, is-regular, is-dir); `1` when the path
   stats, else `0`. The sub-second field is per target (macOS `st_mtimespec`,
   Linux `st_mtim`, elsewhere whole seconds); the type tests use `S_ISREG` /
   `S_ISDIR`, which are macros Loam cannot name. Declared here, defined once
   in the owning unit (see `loam_sys_strerror` above): the `#[c]` wrapper
   references it as a symbol. */
int loam_sys_stat(const char *path, int64_t *out);
#ifdef LOAM_RT_DEFINE_JMP
int loam_sys_stat(const char *path, int64_t *out) {
    struct stat st;
    if (!path || !out || stat(path, &st) != 0) return 0;
#if defined(__APPLE__)
    out[0] = (int64_t)st.st_mtimespec.tv_sec;
    out[1] = (int64_t)st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    out[0] = (int64_t)st.st_mtim.tv_sec;
    out[1] = (int64_t)st.st_mtim.tv_nsec;
#else
    out[0] = (int64_t)st.st_mtime;
    out[1] = 0;
#endif
    out[2] = (int64_t)st.st_size;
    out[3] = S_ISREG(st.st_mode) ? 1 : 0;
    out[4] = S_ISDIR(st.st_mode) ? 1 : 0;
    return 1;
}
#endif


/* `sys.mtime` / `sys.stamp` live in `std/sys.loam` too (R2.2): the same five-
   slot `loam_sys_stat` shim, read as numbers and (for `stamp`) formatted in
   Loam. */

#include <dirent.h>
/**
 * Names in `dirpath`, `.` and `..` excluded, *unsorted*: sorting moved to
 * Loam (`sys.list_dir`, R2.2), over a raw `strcmp` binding. Each name and the
 * array belong to the caller (see `loam_dir_free`); `*out` is NULL when the
 * directory cannot be read, which is not an error worth reporting — an
 * unreadable directory is an empty one as far as a walk is concerned.
 * Declared here, defined once in the owning unit (see `loam_sys_strerror`
 * above): the `#[c]` wrappers reference these as symbols.
 */
int64_t loam_dir_names(const char *dirpath, char ***out);
const char *loam_dir_name_at(char **names, int64_t i);
void loam_dir_free(char **names, int64_t n);
#ifdef LOAM_RT_DEFINE_JMP
int64_t loam_dir_names(const char *dirpath, char ***out) {
    DIR *d = opendir(dirpath);
    struct dirent *e;
    char **names = NULL;
    int64_t n = 0, cap = 0;
    if (!d) {
        *out = NULL;
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        size_t len;
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = (char **)realloc(names, (size_t)cap * sizeof *names);
            if (!names) {
                closedir(d);
                *out = NULL;
                return 0;
            }
        }
        len = strlen(e->d_name);
        names[n] = (char *)malloc(len + 1);
        if (!names[n]) break;
        memcpy(names[n], e->d_name, len + 1);
        n++;
    }
    closedir(d);
    *out = names;
    return n;
}

const char *loam_dir_name_at(char **names, int64_t i) {
    return names ? names[i] : "";
}

void loam_dir_free(char **names, int64_t n) {
    int64_t i;
    for (i = 0; i < n; i++) free(names[i]);
    free(names);
}
#endif

/* `sys.exists` / `sys.is_dir` live in `std/sys.loam` too (R2.2): the same
   `loam_sys_stat` shim, read as numbers. */

/* `sys.realpath`, `sys.cwd` and `sys.mkdir` live in `std/sys.loam` (R2.2,
   R2.6): raw `realpath` / `getcwd` / `mkdir`, and on Windows `_fullpath` /
   `_getcwd` / `_mkdir`, the `/` fix in Loam. */

/* `sys.dir_count` / `sys.dir_entry` live in `std/sys.loam` too (R2.2): one
   listing per call through the same shims, freed at once; the sort is Loam.
   The rescan-per-call costs what the C paid, and no stale cache can lie. */

/* One `lstat(2)` for `remove_path`: like `loam_sys_stat` (same gate) but a
   symlink stays a link — `stat` would follow it into its target's tree. */
int loam_sys_lstat(const char *path, int64_t *out);
#ifdef LOAM_RT_DEFINE_JMP
int loam_sys_lstat(const char *path, int64_t *out) {
    struct stat st;
    if (!path || !out || lstat(path, &st) != 0) return 0;
    out[0] = 1;
    out[1] = S_ISDIR(st.st_mode) ? 1 : 0;
    return 1;
}
#endif

/* `sys.remove_path` lives in `std/sys.loam` too (R2.2): the `lstat`-and-
   -recurse walk in Loam — a linked directory removes the link, never its
   target — over raw `remove` / `rmdir` bindings. */
/* `sys.rename` lives in `std/sys.loam` too (R2.2): POSIX replaces the target,
   and the Loam side removes-then-retries for Windows, like the C did. */


/* `sys.spawn` / `sys.wait` live in `std/sys.loam` (R2.3e): the spawn table
   and the Windows quoting are Loam, over raw `waitpid` / `_spawnvp` /
   `_cwait`. Three shims are left. `loam_sys_argv_fill` lays a `char **` over
   Loam-owned slots (8 bytes each, so any pointer width fits);
   `loam_sys_spawnp` exists because `environ` is a variable, which no binding
   can name; `loam_sys_errno` lets `waitpid` retry on `EINTR` alone. */
void loam_sys_argv_fill(char *block, const int64_t *offsets, int64_t n, int64_t *slots);
int loam_sys_errno(void);
#ifndef _WIN32
#include <spawn.h>
extern char **environ;
int loam_sys_spawnp(int32_t *pid, const char *file, int64_t *argv);
#endif
#ifdef LOAM_RT_DEFINE_JMP
void loam_sys_argv_fill(char *block, const int64_t *offsets, int64_t n, int64_t *slots) {
    char **av = (char **)slots;
    for (int64_t i = 0; i < n; i++) av[i] = block + offsets[i];
    av[n] = NULL;
}
int loam_sys_errno(void) { return errno; }
#ifndef _WIN32
int loam_sys_spawnp(int32_t *pid, const char *file, int64_t *argv) {
    pid_t p = 0;
    int r = posix_spawnp(&p, file, NULL, NULL, (char **)argv, environ);
    *pid = (int32_t)p;
    return r;
}
#endif
#endif

/* `sys.read_stdin` / `sys.read_line` live in `std/sys.loam` (R2.4), over
   this one hook and raw `read`. `sys.read_fd(fd, n)`: up to `n` bytes of
   `fd` into a fresh buffer whose length is what `read` returned, retried on
   `EINTR`; no byte is unwritten and none is zeroed first. */
static inline loam_vec loam_sys_read_fd(int64_t fd, int64_t n) {
    loam_vec v = loam_vec_new();
    if (n <= 0) return v;
    loam_vec_reserve(&v, n + 1, 1, "sys.read_fd", 0);
    while (v.len < n) {
        ssize_t r = read((int)fd, (char *)v.ptr + v.len, (size_t)(n - v.len));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        v.len += r;
    }
    return v;
}

/* `sys.watch_*` live in `std/sys.loam` (R2.5): kqueue on macOS / iOS and
   inotify on Linux, as raw bindings; everywhere else `watch_open` is 0 and a
   caller polls. */
#endif

/* --- typed handle cells -------------------------------------------------
   `Future<T>`, `Chan<T>` and `Signal<T>` are plain `{ id: int }` structs, so a
   program can build one at any `T`. Each cell records the key of the type it
   was made for (a hash of the type's spelling, a constant in the generated
   C; never 0), and every access passes the key it expects. A mismatch is a
   forged handle: reading the cell's bytes as another type could take an int
   for a pointer, so it panics instead. */
__attribute__((noinline, noreturn, cold, unused)) static void loam_handle_wrong_type(const char *what) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s handle used at the wrong type", what);
    loam_panic(__FILE__, __LINE__, msg);
    abort();
}

#ifdef __wasm32__
/* `std:async` binds `clock_gettime` / `nanosleep` itself on native (`#[c]`);
   wasm binds no libc, so the two wasm bridges live here (like `sys.wasm_exit`
   bridges `abort`): the clock is the `zeus_js_now_ms` host import
   (`loam_now_ms`, above), and the sleep is its spin. */
static inline int64_t loam_async_wasm_now_ms(void) { return loam_now_ms(); }
static inline void loam_async_wasm_sleep(int64_t ms) {
    int64_t t0 = loam_now_ms();
    while (loam_now_ms() - t0 < ms) {
    }
}
#else
/* Native binds `clock_gettime` / `nanosleep` through `#[c]` in `std:async`;
   nothing of the clock or sleep remains in C. */
#endif

/* --- thread + channel: std/thread.loam. The ABI is a detached pthread and a ---
   --- bounded FIFO of byte payloads guarded by a mutex + two condvars. All ---
   --- queue/dispatch policy lives in Loam; this is only the synchronization. --- */

#ifndef __wasm32__
#include <pthread.h>

LOAM_RT_STATE int64_t loam_thread_live;          /* detached workers still running */
LOAM_RT_STATE pthread_mutex_t loam_thread_mtx LOAM_RT_INIT(PTHREAD_MUTEX_INITIALIZER);
LOAM_RT_STATE pthread_mutex_t loam_ch_tab_mtx LOAM_RT_INIT(PTHREAD_MUTEX_INITIALIZER);

/* The table lock for `std:thread`'s Loam id table: held across every table
   read and write there, so a worker fetching a cookie races nothing. Plain
   C functions (not `static inline`): the `#[c]` wrappers reference them as
   symbols, like `loam_sys_stat` below. */
int loam_ch_tab_lock(void);
int loam_ch_tab_unlock(void);
#ifdef LOAM_RT_DEFINE_JMP
int loam_ch_tab_lock(void) {
    return pthread_mutex_lock(&loam_ch_tab_mtx);
}
int loam_ch_tab_unlock(void) {
    return pthread_mutex_unlock(&loam_ch_tab_mtx);
}
#endif

/* --- channel: std/thread.loam. The ABI is a detached pthread and a bounded
   FIFO of byte payloads guarded by a mutex + two condvars. R3.2: the id
   table and S0 keys live in Loam (`ch_slots`); C owns one sync block per
   channel (below), named by an opaque cookie, plus one atomic section per
   op. A 0 cookie names nothing: the section no-ops, as a dead id did. */

typedef struct {
    unsigned char *buf;     /* cap * esz ring */
    size_t esz;             /* payload byte size (sizeof T) */
    int64_t cap;            /* slots */
    int64_t head;           /* first live slot */
    int64_t count;          /* live slots */
    pthread_mutex_t mtx;
    pthread_cond_t has_data, has_space;
} loam_ch;

/* The sync block for `cap` slots of `esz` bytes. The cookie is the block
   address; Loam files it in its id table under the payload type's key.
   Declared here, defined once in the owning unit (like `loam_sys_stat`):
   the `#[c]` wrapper references it as a symbol. */
int64_t loam_ch_block_alloc(int64_t cap, size_t esz);
#ifdef LOAM_RT_DEFINE_JMP
int64_t loam_ch_block_alloc(int64_t cap, size_t esz) {
    loam_ch *c;
    if (cap < 1) cap = 1;
    if (esz < 1) esz = 1;
    c = (loam_ch *)loam_new(sizeof(loam_ch), "channel", 0);
    c->buf = (unsigned char *)loam_new((size_t)cap * esz, "channel", 0);
    c->esz = esz;
    c->cap = cap;
    c->head = 0;
    c->count = 0;
    pthread_mutex_init(&c->mtx, NULL);
    pthread_cond_init(&c->has_data, NULL);
    pthread_cond_init(&c->has_space, NULL);
    return (int64_t)(intptr_t)c;
}
#endif

static inline loam_ch *loam_ch_from_cookie(int64_t cookie) {
    if (!cookie) return NULL;
    return (loam_ch *)(intptr_t)cookie;
}

static inline void loam_ch_do_send(int64_t cookie, const void *val, size_t sz) {
    loam_ch *c = loam_ch_from_cookie(cookie);
    if (!c || !val) return;
    pthread_mutex_lock(&c->mtx);
    while (c->count == c->cap) pthread_cond_wait(&c->has_space, &c->mtx);
    if (sz > c->esz) sz = c->esz;
    if (sz)
        memcpy(c->buf + (size_t)((c->head + c->count) % c->cap) * c->esz, val, sz);
    c->count++;
    pthread_cond_signal(&c->has_data);
    pthread_mutex_unlock(&c->mtx);
}

static inline int64_t loam_ch_do_try_send(int64_t cookie, const void *val, size_t sz) {
    loam_ch *c = loam_ch_from_cookie(cookie);
    if (!c || !val) return 0;
    pthread_mutex_lock(&c->mtx);
    if (c->count == c->cap) {
        pthread_mutex_unlock(&c->mtx);
        return 0;
    }
    if (sz > c->esz) sz = c->esz;
    if (sz)
        memcpy(c->buf + (size_t)((c->head + c->count) % c->cap) * c->esz, val, sz);
    c->count++;
    pthread_cond_signal(&c->has_data);
    pthread_mutex_unlock(&c->mtx);
    return 1;
}

static inline void loam_ch_do_recv(int64_t cookie, void *out, size_t sz) {
    loam_ch *c = loam_ch_from_cookie(cookie);
    if (!c || !out) return;
    pthread_mutex_lock(&c->mtx);
    while (c->count == 0) pthread_cond_wait(&c->has_data, &c->mtx);
    if (sz > c->esz) sz = c->esz;
    if (sz) memcpy(out, c->buf + (size_t)c->head * c->esz, sz);
    c->head = (c->head + 1) % c->cap;
    c->count--;
    pthread_cond_signal(&c->has_space);
    pthread_mutex_unlock(&c->mtx);
}

static inline int64_t loam_ch_do_pop(int64_t cookie, void *out, size_t sz) {
    loam_ch *c = loam_ch_from_cookie(cookie);
    if (!c || !out) return 0;
    pthread_mutex_lock(&c->mtx);
    if (c->count == 0) {
        pthread_mutex_unlock(&c->mtx);
        return 0;
    }
    if (sz > c->esz) sz = c->esz;
    if (sz) memcpy(out, c->buf + (size_t)c->head * c->esz, sz);
    c->head = (c->head + 1) % c->cap;
    c->count--;
    pthread_cond_signal(&c->has_space);
    pthread_mutex_unlock(&c->mtx);
    return 1;
}

/* --- channels with owning payloads --------------------------------------
   A queued slot aliases the payload bytes, so the channel takes a reference
   when it enqueues one; `recv`/`pop` copy the slot out and with it the
   reference (the head advances past the slot, so the channel never releases
   it). A payload still queued when the channel dies is not released — bounded,
   and only when a producer enqueues what no consumer drains. */
static inline void loam_ch_do_send_owned(int64_t cookie, const void *val, size_t sz,
                                         loam_elem_fn retain) {
    loam_ch *c = loam_ch_from_cookie(cookie);
    if (!c || !val) return;
    pthread_mutex_lock(&c->mtx);
    while (c->count == c->cap) pthread_cond_wait(&c->has_space, &c->mtx);
    if (retain) retain((void *)val);
    if (sz > c->esz) sz = c->esz;
    if (sz)
        memcpy(c->buf + (size_t)((c->head + c->count) % c->cap) * c->esz, val, sz);
    c->count++;
    pthread_cond_signal(&c->has_data);
    pthread_mutex_unlock(&c->mtx);
}

static inline int64_t loam_ch_do_try_send_owned(int64_t cookie, const void *val, size_t sz,
                                                loam_elem_fn retain) {
    loam_ch *c = loam_ch_from_cookie(cookie);
    if (!c || !val) return 0;
    pthread_mutex_lock(&c->mtx);
    if (c->count == c->cap) {
        pthread_mutex_unlock(&c->mtx);
        return 0;
    }
    if (retain) retain((void *)val);
    if (sz > c->esz) sz = c->esz;
    if (sz)
        memcpy(c->buf + (size_t)((c->head + c->count) % c->cap) * c->esz, val, sz);
    c->count++;
    pthread_cond_signal(&c->has_data);
    pthread_mutex_unlock(&c->mtx);
    return 1;
}

static inline int64_t loam_ch_do_ready(int64_t cookie) {
    loam_ch *c = loam_ch_from_cookie(cookie);
    int64_t r;
    if (!c) return 0;
    pthread_mutex_lock(&c->mtx);
    r = c->count;
    pthread_mutex_unlock(&c->mtx);
    return r;
}

typedef struct {
    loam_fn cb;
} loam_thread_arg;

static void *loam_thread_entry(void *p) {
    loam_thread_arg *a = (loam_thread_arg *)p;
    loam_fn cb = a->cb;
    free(a);
    ((void (*)(void *))cb.fn)(cb.env);
    pthread_mutex_lock(&loam_thread_mtx);
    loam_thread_live--;
    pthread_mutex_unlock(&loam_thread_mtx);
    return NULL;
}

/* Bodyless `thread.spawn(cb: fn())`. Detached: results come back through
   channel<T>, never through join. The fn env is leaked by design (fn values
   are never freed), so the worker outlives the spawn callsite safely. */
static inline void loam_thread_spawn(loam_fn cb) {
    pthread_t t;
    loam_thread_arg *a;
    if (!cb.fn) return;
    a = (loam_thread_arg *)loam_new(sizeof(loam_thread_arg), "thread.spawn", 0);
    a->cb = cb;
    if (pthread_create(&t, NULL, loam_thread_entry, a) != 0) {
        free(a);
        loam_panic(__FILE__, __LINE__, "thread.spawn: pthread_create failed");
    }
    pthread_detach(t);
    pthread_mutex_lock(&loam_thread_mtx);
    loam_thread_live++;
    pthread_mutex_unlock(&loam_thread_mtx);
}

static inline int64_t loam_thread_running(void) {
    int64_t n;
    pthread_mutex_lock(&loam_thread_mtx);
    n = loam_thread_live;
    pthread_mutex_unlock(&loam_thread_mtx);
    return n;
}
#else /* __wasm32__: no threads; channel ops are inert so std:thread compiles */
/* The cookies name nothing: the Loam table still hands out ids and checks
   keys, but no bytes ever move. */
static inline void loam_ch_do_send(int64_t cookie, const void *val, size_t sz) {
    (void)cookie;
    (void)val;
    (void)sz;
}
static inline int64_t loam_ch_do_try_send(int64_t cookie, const void *val, size_t sz) {
    (void)cookie;
    (void)val;
    (void)sz;
    return 0;
}
static inline void loam_ch_do_send_owned(int64_t cookie, const void *val, size_t sz,
                                         loam_elem_fn retain) {
    (void)retain;
    loam_ch_do_send(cookie, val, sz);
}
static inline int64_t loam_ch_do_try_send_owned(int64_t cookie, const void *val, size_t sz,
                                                loam_elem_fn retain) {
    (void)retain;
    return loam_ch_do_try_send(cookie, val, sz);
}
static inline void loam_ch_do_recv(int64_t cookie, void *out, size_t sz) {
    (void)cookie;
    if (out) memset(out, 0, sz);
}
static inline int64_t loam_ch_do_pop(int64_t cookie, void *out, size_t sz) {
    (void)cookie;
    if (out) memset(out, 0, sz);
    return 0;
}
static inline int64_t loam_ch_do_ready(int64_t cookie) {
    (void)cookie;
    return 0;
}
static inline void loam_thread_spawn(loam_fn cb) {
    (void)cb; /* wasm has no threads; spawn is a no-op */
}
static inline int64_t loam_thread_running(void) { return 0; }
#endif

#endif /* LOAM_RT_H */
