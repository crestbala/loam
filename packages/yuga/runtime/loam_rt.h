/* loam_rt.h — panics, overflow, bounds. Included at the top of generated C. */
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
#include <sys/uio.h>
#ifndef __wasm32__
#include <time.h>
#include <setjmp.h>
/* Recoverable traps (zeus.Boundary). The generated program defines the state
   (exactly one TU); the zeus seam (zeus_plat.c) sets the `jmp_buf` target and
   reads the message. A program that does not link that seam leaves it NULL,
   so a trap still aborts. */
typedef struct LoamJmp {
    jmp_buf jb;
    struct LoamJmp *prev;
} LoamJmp;
#ifdef LOAM_RT_DEFINE_JMP
LoamJmp *loam_jmp_top = NULL;
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
extern LoamJmp *loam_jmp_top;
extern char loam_jmp_msg[512];
void loam_jmp_set_msg(const char *msg, int64_t len);
#endif
#endif

typedef struct {
    const char *ptr;
    int64_t len;
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

/* Length-based stdout. No malloc, no printf, no NUL requirement. */
static inline void loam_write_bytes(const char *p, size_t n) {
    while (n) {
        ssize_t w = write(STDOUT_FILENO, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (w == 0) return;
        p += (size_t)w;
        n -= (size_t)w;
    }
}

static inline void loam_writev_all(struct iovec *iov, int n) {
    while (n > 0) {
        if (iov->iov_len == 0) {
            iov++;
            n--;
            continue;
        }
        ssize_t w = writev(STDOUT_FILENO, iov, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (w == 0) return;
        size_t left = (size_t)w;
        while (n > 0 && left >= iov->iov_len) {
            left -= iov->iov_len;
            iov++;
            n--;
        }
        if (n > 0 && left) {
            iov->iov_base = (char *)iov->iov_base + left;
            iov->iov_len -= left;
        }
    }
}

static inline void loam_fmt_write(loam_str s) {
    if (!s.ptr || s.len <= 0) return;
    loam_write_bytes(s.ptr, (size_t)s.len);
}

static inline loam_str loam_fmt_itoa(char buf[24], int64_t v) {
    char *end = buf + 24;
    char *p = end;
    uint64_t u;
    if (v < 0)
        u = (uint64_t)(-(v + 1)) + 1;
    else
        u = (uint64_t)v;
    if (u == 0) {
        *--p = '0';
    } else {
        while (u) {
            *--p = (char)('0' + (u % 10));
            u /= 10;
        }
        if (v < 0) *--p = '-';
    }
    loam_str s;
    s.ptr = p;
    s.len = (int64_t)(end - p);
    return s;
}

static inline void loam_fmt_write_int(int64_t v) {
    char buf[24];
    loam_str s = loam_fmt_itoa(buf, v);
    loam_write_bytes(s.ptr, (size_t)s.len);
}

static inline void loam_fmt_write_bool(bool v) {
    if (v) loam_write_bytes("true", 4);
    else loam_write_bytes("false", 5);
}

static inline loam_str loam_fmt_ftoa(char buf[64], double v) {
    int n = snprintf(buf, 64, "%.15g", v);
    if (n < 0) n = 0;
    if (n > 63) n = 63;
    loam_str s;
    s.ptr = buf;
    s.len = (int64_t)n;
    return s;
}

static inline void loam_fmt_write_float(double v) {
    char buf[64];
    loam_str s = loam_fmt_ftoa(buf, v);
    loam_write_bytes(s.ptr, (size_t)s.len);
}

static inline void loam_fmt_writeln(void) { loam_write_bytes("\n", 1); }

static inline bool loam_fmt_eq(loam_str a, loam_str b) {
    if (a.len != b.len) return false;
    if (a.len <= 0) return true;
    if (!a.ptr || !b.ptr) return a.ptr == b.ptr;
    return memcmp(a.ptr, b.ptr, (size_t)a.len) == 0;
}

static inline void loam_panic(const char *file, int line, const char *msg) {
    fflush(stdout);
    fprintf(stderr, "%s:%d: panic: %s\n", file, line, msg);
#ifndef __wasm32__
    if (loam_jmp_top) {
        loam_jmp_set_msg(msg, (int64_t)strlen(msg));
        longjmp(loam_jmp_top->jb, 1);
    }
#endif
    abort();
}

/* `panic(msg)` — like loam_panic but with a runtime string, so `std:test` and
   other Loam code can build the message. */
static inline void loam_panic_str(const char *file, int line, loam_str msg) {
    fflush(stdout);
    fprintf(stderr, "%s:%d: panic: %.*s\n", file, line,
            (int)(msg.len > 0 ? msg.len : 0), msg.ptr ? msg.ptr : "");
#ifndef __wasm32__
    if (loam_jmp_top) {
        loam_jmp_set_msg(msg.ptr ? msg.ptr : "", msg.len);
        longjmp(loam_jmp_top->jb, 1);
    }
#endif
    abort();
}

static inline int64_t loam_add_i64(int64_t a, int64_t b, const char *f, int l) {
    int64_t r;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_add_overflow(a, b, &r)) loam_panic(f, l, "integer overflow");
    return r;
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        loam_panic(f, l, "integer overflow");
    return a + b;
#endif
}

static inline int64_t loam_sub_i64(int64_t a, int64_t b, const char *f, int l) {
    int64_t r;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_sub_overflow(a, b, &r)) loam_panic(f, l, "integer overflow");
    return r;
#else
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
        loam_panic(f, l, "integer overflow");
    return a - b;
#endif
}

static inline int64_t loam_mul_i64(int64_t a, int64_t b, const char *f, int l) {
    int64_t r;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_mul_overflow(a, b, &r)) loam_panic(f, l, "integer overflow");
    return r;
#else
    if (a != 0 && b != 0) {
        if (a > 0 && b > 0 && a > INT64_MAX / b) loam_panic(f, l, "integer overflow");
        if (a > 0 && b < 0 && b < INT64_MIN / a) loam_panic(f, l, "integer overflow");
        if (a < 0 && b > 0 && a < INT64_MIN / b) loam_panic(f, l, "integer overflow");
        if (a < 0 && b < 0 && a < INT64_MAX / b) loam_panic(f, l, "integer overflow");
    }
    return a * b;
#endif
}

static inline int64_t loam_div_i64(int64_t a, int64_t b, const char *f, int l) {
    if (b == 0) loam_panic(f, l, "division by zero");
    if (a == INT64_MIN && b == -1) loam_panic(f, l, "integer overflow");
    return a / b;
}

static inline int64_t loam_mod_i64(int64_t a, int64_t b, const char *f, int l) {
    if (b == 0) loam_panic(f, l, "division by zero");
    if (a == INT64_MIN && b == -1) return 0;
    return a % b;
}

static inline int64_t loam_wrapping_add(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a + (uint64_t)b);
}

static inline int64_t loam_wrapping_shr(int64_t a, int64_t n) {
    if (n < 0 || n > 63) return 0;
    return (int64_t)((uint64_t)a >> (unsigned)n);
}

static inline int64_t loam_wrapping_shl(int64_t a, int64_t n) {
    if (n < 0 || n > 63) return 0;
    return (int64_t)((uint64_t)a << (unsigned)n);
}

static inline int64_t loam_wrapping_or(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a | (uint64_t)b);
}

static inline int64_t loam_wrapping_and(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a & (uint64_t)b);
}

static inline int64_t loam_saturating_add(int64_t a, int64_t b) {
    int64_t r;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_add_overflow(a, b, &r)) return b > 0 ? INT64_MAX : INT64_MIN;
    return r;
#else
    if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
    return a + b;
#endif
}

/* ---- sized integer arithmetic (Phase 9) ----------------------------------
 * One helper family per width. `int`/`float` keep their existing i64 helpers
 * above so generated code for already-64-bit programs is unchanged.
 *
 * Overflow is checked with __builtin_*_overflow, which computes in infinite
 * precision and reports whether the result fits the destination type exactly —
 * C's own promoting rules never decide a result here (§3.5 rules 1–2). */
#define LOAM_DEF_INT_ASM(SFX, T)                                                  \
static inline T loam_add_##SFX(T a, T b, const char *f, int l) {                  \
    T r; if (__builtin_add_overflow(a, b, &r)) loam_panic(f, l, "integer overflow"); \
    return r;                                                                     \
}                                                                                 \
static inline T loam_sub_##SFX(T a, T b, const char *f, int l) {                  \
    T r; if (__builtin_sub_overflow(a, b, &r)) loam_panic(f, l, "integer overflow"); \
    return r;                                                                     \
}                                                                                 \
static inline T loam_mul_##SFX(T a, T b, const char *f, int l) {                  \
    T r; if (__builtin_mul_overflow(a, b, &r)) loam_panic(f, l, "integer overflow"); \
    return r;                                                                     \
}                                                                                 \
static inline T loam_neg_##SFX(T a, const char *f, int l) {                       \
    T r; if (__builtin_sub_overflow((T)0, a, &r)) loam_panic(f, l, "integer overflow"); \
    return r;                                                                     \
}

#define LOAM_DEF_INT_S(SFX, T, MINV) LOAM_DEF_INT_ASM(SFX, T)                    \
static inline T loam_div_##SFX(T a, T b, const char *f, int l) {                  \
    if (b == 0) loam_panic(f, l, "division by zero");                            \
    if (a == (T)(MINV) && b == (T)-1) loam_panic(f, l, "integer overflow");      \
    return (T)(a / b);                                                            \
}                                                                                 \
static inline T loam_mod_##SFX(T a, T b, const char *f, int l) {                  \
    if (b == 0) loam_panic(f, l, "division by zero");                            \
    if (a == (T)(MINV) && b == (T)-1) return (T)0;                                \
    return (T)(a % b);                                                            \
}

#define LOAM_DEF_INT_U(SFX, T) LOAM_DEF_INT_ASM(SFX, T)                           \
static inline T loam_div_##SFX(T a, T b, const char *f, int l) {                  \
    if (b == 0) loam_panic(f, l, "division by zero");                            \
    return (T)(a / b);                                                            \
}                                                                                 \
static inline T loam_mod_##SFX(T a, T b, const char *f, int l) {                  \
    if (b == 0) loam_panic(f, l, "division by zero");                            \
    return (T)(a % b);                                                            \
}

LOAM_DEF_INT_S(i8, int8_t, INT8_MIN)
LOAM_DEF_INT_S(i16, int16_t, INT16_MIN)
LOAM_DEF_INT_S(i32, int32_t, INT32_MIN)
LOAM_DEF_INT_U(u8, uint8_t)
LOAM_DEF_INT_U(u16, uint16_t)
LOAM_DEF_INT_U(u32, uint32_t)
LOAM_DEF_INT_U(u64, uint64_t)
static inline int64_t loam_neg_i64(int64_t a, const char *f, int l) {
    int64_t r;
    if (__builtin_sub_overflow((int64_t)0, a, &r)) loam_panic(f, l, "integer overflow");
    return r;
}

/* Shifts lower through the unsigned type so a signed left shift is never UB,
 * and a count at or past the width traps (§3.5 rule 3). */
#define LOAM_DEF_SHIFT(SFX, T, UT)                                                \
static inline T loam_shl_##SFX(T a, uint64_t n, const char *f, int l) {           \
    if (n >= (uint64_t)(sizeof(T) * 8)) loam_panic(f, l, "shift amount out of range"); \
    return (T)((UT)a << n);                                                       \
}                                                                                 \
static inline T loam_shr_##SFX(T a, uint64_t n, const char *f, int l) {           \
    if (n >= (uint64_t)(sizeof(T) * 8)) loam_panic(f, l, "shift amount out of range"); \
    return (T)(a >> n);                                                           \
}

LOAM_DEF_SHIFT(i8, int8_t, uint8_t)
LOAM_DEF_SHIFT(i16, int16_t, uint16_t)
LOAM_DEF_SHIFT(i32, int32_t, uint32_t)
LOAM_DEF_SHIFT(i64, int64_t, uint64_t)
LOAM_DEF_SHIFT(u8, uint8_t, uint8_t)
LOAM_DEF_SHIFT(u16, uint16_t, uint16_t)
LOAM_DEF_SHIFT(u32, uint32_t, uint32_t)
LOAM_DEF_SHIFT(u64, uint64_t, uint64_t)

/* Per-width explicit wrap (per §3.4). */
#define LOAM_DEF_WRAP(SFX, T, UT)                                                 \
static inline T loam_wrapping_add_##SFX(T a, T b) { return (T)((UT)a + (UT)b); }  \
static inline T loam_wrapping_sub_##SFX(T a, T b) { return (T)((UT)a - (UT)b); }  \
static inline T loam_wrapping_mul_##SFX(T a, T b) { return (T)((UT)a * (UT)b); }  \
static inline T loam_wrapping_neg_##SFX(T a) { return (T)((UT)0 - (UT)a); }       \
static inline T loam_wrapping_shl_##SFX(T a, UT n) {                              \
    return n >= (UT)(sizeof(T) * 8) ? (T)0 : (T)((UT)a << n);                     \
}                                                                                 \
static inline T loam_wrapping_shr_##SFX(T a, UT n) {                              \
    return n >= (UT)(sizeof(T) * 8) ? (T)0 : (T)((UT)a >> n);                     \
}                                                                                 \
static inline T loam_wrapping_and_##SFX(T a, T b) { return (T)(a & b); }          \
static inline T loam_wrapping_or_##SFX(T a, T b) { return (T)(a | b); }           \
static inline T loam_wrapping_xor_##SFX(T a, T b) { return (T)(a ^ b); }

LOAM_DEF_WRAP(i8, int8_t, uint8_t)
LOAM_DEF_WRAP(i16, int16_t, uint16_t)
LOAM_DEF_WRAP(i32, int32_t, uint32_t)
LOAM_DEF_WRAP(i64, int64_t, uint64_t)
LOAM_DEF_WRAP(u8, uint8_t, uint8_t)
LOAM_DEF_WRAP(u16, uint16_t, uint16_t)
LOAM_DEF_WRAP(u32, uint32_t, uint32_t)
LOAM_DEF_WRAP(u64, uint64_t, uint64_t)

/* Per-width saturating arithmetic. */
#define LOAM_DEF_SAT(SFX, T, MINV, MAXV)                                          \
static inline T loam_saturating_add_##SFX(T a, T b) {                             \
    T r; if (!__builtin_add_overflow(a, b, &r)) return r;                         \
    return (b > 0) ? (T)(MAXV) : (T)(MINV);                                       \
}                                                                                 \
static inline T loam_saturating_sub_##SFX(T a, T b) {                             \
    T r; if (!__builtin_sub_overflow(a, b, &r)) return r;                         \
    return (b < 0) ? (T)(MAXV) : (T)(MINV);                                       \
}                                                                                 \
static inline T loam_saturating_mul_##SFX(T a, T b) {                             \
    T r; if (!__builtin_mul_overflow(a, b, &r)) return r;                         \
    return ((a > 0) == (b > 0)) ? (T)(MAXV) : (T)(MINV);                          \
}

LOAM_DEF_SAT(i8, int8_t, INT8_MIN, INT8_MAX)
LOAM_DEF_SAT(i16, int16_t, INT16_MIN, INT16_MAX)
LOAM_DEF_SAT(i32, int32_t, INT32_MIN, INT32_MAX)
LOAM_DEF_SAT(i64, int64_t, INT64_MIN, INT64_MAX)
LOAM_DEF_SAT(u8, uint8_t, 0, UINT8_MAX)
LOAM_DEF_SAT(u16, uint16_t, 0, UINT16_MAX)
LOAM_DEF_SAT(u32, uint32_t, 0, UINT32_MAX)
LOAM_DEF_SAT(u64, uint64_t, 0, UINT64_MAX)

/* Numeric narrowing conversions trap on a value the destination cannot hold
 * (§3.4). `convu`/`satu` take a u64 source; the others an int64 source. */
#define LOAM_DEF_CONV(SFX, T, LO, HI)                                             \
static inline T loam_conv_##SFX(int64_t v, const char *f, int l) {                \
    if (v < (int64_t)(LO) || v > (int64_t)(HI))                                   \
        loam_panic(f, l, "integer conversion out of range");                      \
    return (T)v;                                                                  \
}                                                                                 \
static inline T loam_convf_##SFX(double v, const char *f, int l) {                \
    if (!(v >= (double)(LO) && v <= (double)(HI)))                                \
        loam_panic(f, l, "float conversion out of range");                        \
    return (T)v;                                                                  \
}                                                                                 \
static inline T loam_sat_##SFX(int64_t v) {                                       \
    if (v < (int64_t)(LO)) return (T)(LO);                                        \
    if (v > (int64_t)(HI)) return (T)(HI);                                        \
    return (T)v;                                                                  \
}

LOAM_DEF_CONV(i8, int8_t, INT8_MIN, INT8_MAX)
LOAM_DEF_CONV(i16, int16_t, INT16_MIN, INT16_MAX)
LOAM_DEF_CONV(i32, int32_t, INT32_MIN, INT32_MAX)
LOAM_DEF_CONV(u8, uint8_t, 0, UINT8_MAX)
LOAM_DEF_CONV(u16, uint16_t, 0, UINT16_MAX)
LOAM_DEF_CONV(u32, uint32_t, 0, UINT32_MAX)
static inline int64_t loam_conv_i64(int64_t v, const char *f, int l) {
    (void)f; (void)l; return v;
}
static inline int64_t loam_convf_i64(double v, const char *f, int l) {
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        loam_panic(f, l, "float conversion out of range");
    return (int64_t)v;
}
static inline int64_t loam_sat_i64(int64_t v) { return v; }
static inline uint64_t loam_conv_u64(int64_t v, const char *f, int l) {
    if (v < 0) loam_panic(f, l, "integer conversion out of range");
    return (uint64_t)v;
}
static inline uint64_t loam_convf_u64(double v, const char *f, int l) {
    if (!(v >= 0.0 && v < 18446744073709551616.0))
        loam_panic(f, l, "float conversion out of range");
    return (uint64_t)v;
}
static inline uint64_t loam_sat_u64(int64_t v) { return v < 0 ? (uint64_t)0 : (uint64_t)v; }

#define LOAM_DEF_CONVU(SFX, T, MAXV)                                              \
static inline T loam_convu_##SFX(uint64_t v, const char *f, int l) {              \
    if (v > (uint64_t)(MAXV)) loam_panic(f, l, "integer conversion out of range"); \
    return (T)v;                                                                  \
}                                                                                 \
static inline T loam_satu_##SFX(uint64_t v) {                                     \
    return v > (uint64_t)(MAXV) ? (T)(MAXV) : (T)v;                               \
}

LOAM_DEF_CONVU(i8, int8_t, INT8_MAX)
LOAM_DEF_CONVU(i16, int16_t, INT16_MAX)
LOAM_DEF_CONVU(i32, int32_t, INT32_MAX)
LOAM_DEF_CONVU(i64, int64_t, INT64_MAX)
LOAM_DEF_CONVU(u8, uint8_t, UINT8_MAX)
LOAM_DEF_CONVU(u16, uint16_t, UINT16_MAX)
LOAM_DEF_CONVU(u32, uint32_t, UINT32_MAX)

static inline int64_t loam_idx(int64_t i, int64_t n, const char *f, int l) {
    if (i < 0 || i >= n) loam_panic(f, l, "index out of bounds");
    return i;
}

static inline void *loam_new(size_t sz, const char *f, int l) {
    void *p = malloc(sz ? sz : 1);
    if (!p) loam_panic(f, l, "out of memory");
    return p;
}

static inline void loam_drop(void **p) {
    if (p && *p) {
        free(*p);
        *p = NULL;
    }
}

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
    *(int64_t *)raw = 1;
    if (v->len > 0 && esz)
        memcpy((char *)raw + sizeof(int64_t), v->ptr, (size_t)v->len * esz);
    (*rc)--;
    v->ptr = (char *)raw + sizeof(int64_t);
}

static inline void loam_vec_reserve(loam_vec *v, int64_t n, size_t esz, const char *f, int l) {
    int64_t cap;
    void *raw;
    if (!v || n <= v->cap) return;
    loam_vec_unique(v, esz, f, l);
    cap = v->cap ? v->cap : 8;
    while (cap < n) {
        if (cap > INT64_MAX / 2) loam_panic(f, l, "out of memory");
        cap *= 2;
    }
    if (!v->ptr) {
        raw = malloc(sizeof(int64_t) + (esz ? (size_t)cap * esz : 1));
        if (!raw) loam_panic(f, l, "out of memory");
        *(int64_t *)raw = 1;
        v->ptr = (char *)raw + sizeof(int64_t);
        v->cap = cap;
        return;
    }
    raw = realloc((char *)v->ptr - sizeof(int64_t),
                  sizeof(int64_t) + (esz ? (size_t)cap * esz : 1));
    if (!raw) loam_panic(f, l, "out of memory");
    v->ptr = (char *)raw + sizeof(int64_t);
    v->cap = cap;
}

static inline void loam_vec_push(loam_vec *v, const void *elem, size_t esz, const char *f, int l) {
    if (!v) loam_panic(f, l, "push on null array");
    loam_vec_reserve(v, v->len + 1, esz, f, l);
    if (esz && elem) memcpy((char *)v->ptr + (size_t)v->len * esz, elem, esz);
    v->len++;
}

static inline void loam_vec_pop(loam_vec *v, void *out, size_t esz, const char *f, int l) {
    if (!v || v->len <= 0) loam_panic(f, l, "pop from empty array");
    loam_vec_unique(v, esz, f, l);
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

/* `{{ }}` interpolation. Each piece is converted to a loam_str and the
   pieces are concatenated left to right. The result is heap-allocated with a
   trailing NUL and, like loam_string_from_bytes, is never freed — the
   language has no string ownership story to hook into yet. */
static inline loam_str loam_str_concat(loam_str a, loam_str b) {
    int64_t an = a.len > 0 ? a.len : 0;
    int64_t bn = b.len > 0 ? b.len : 0;
    if (!an) { if (bn) return b; return (loam_str){ .ptr = "", .len = 0 }; }
    if (!bn) return a;
    char *p = (char *)loam_new((size_t)(an + bn) + 1, "str_concat", 0);
    if (a.ptr) memcpy(p, a.ptr, (size_t)an);
    if (b.ptr) memcpy(p + an, b.ptr, (size_t)bn);
    p[an + bn] = 0;
    return (loam_str){ .ptr = p, .len = an + bn };
}

static inline loam_str loam_str_of_int(int64_t v) {
    char buf[24];
    loam_str s = loam_fmt_itoa(buf, v);
    char *p = (char *)loam_new((size_t)s.len + 1, "str_of_int", 0);
    memcpy(p, s.ptr, (size_t)s.len);
    p[s.len] = 0;
    return (loam_str){ .ptr = p, .len = s.len };
}

static inline loam_str loam_str_of_float(double v) {
    char buf[64];
    loam_str s = loam_fmt_ftoa(buf, v);
    char *p = (char *)loam_new((size_t)s.len + 1, "str_of_float", 0);
    memcpy(p, s.ptr, (size_t)s.len);
    p[s.len] = 0;
    return (loam_str){ .ptr = p, .len = s.len };
}

static inline loam_str loam_str_of_bool(bool v) {
    return v ? (loam_str){ .ptr = "true", .len = 4 } : (loam_str){ .ptr = "false", .len = 5 };
}

static inline loam_str loam_str_of_string(loam_str s) { return s; }

/* Takes ownership of `b` (IR_CALL steals []int). Trailing NUL for C hosts. */
static inline loam_str loam_string_from_bytes(loam_vec b) {
    int64_t n = b.len > 0 ? b.len : 0;
    char *p = (char *)loam_new((size_t)n + 1, "string_from_bytes", 0);
    int32_t *el = (int32_t *)b.ptr;
    int64_t i;
    for (i = 0; i < n; i++)
        p[i] = (char)(el ? (el[i] & 255) : 0);
    p[n] = 0;
    loam_vec_drop(&b);
    return (loam_str){ .ptr = p, .len = n };
}

/** Compile target: "wasm" | "ios" | "android" | "native". Used by
 *  `http.client()` to pick a default RPC address. */
static inline loam_str loam_sys_target(void) {
#ifdef __wasm32__
    return (loam_str){"wasm", 4};
#elif defined(LOAM_ANDROID)
    return (loam_str){"android", 7};
#elif defined(LOAM_IOS)
    return (loam_str){"ios", 3};
#else
    return (loam_str){"native", 6};
#endif
}

#ifdef __wasm32__
static inline int64_t loam_sys_env_set(loam_str name) {
    (void)name;
    return 0;
}
static inline void loam_sys_exit(int64_t code) {
    (void)code;
    abort();
}
static inline loam_str loam_sys_env(loam_str name) {
    (void)name;
    return (loam_str){"", 0};
}
static inline int64_t loam_sys_write_file(loam_str path, loam_str body) {
    (void)path;
    (void)body;
    return 1;
}
static inline loam_str loam_sys_exec(loam_str cmd) {
    (void)cmd;
    return (loam_str){"", 0};
}
static inline int64_t loam_sys_exec_status(void) { return 1; }
static inline loam_str loam_sys_read_file(loam_str path) {
    (void)path;
    return (loam_str){"", 0};
}
static inline int64_t loam_sys_mkdir(loam_str path) {
    (void)path;
    return 1;
}
static inline int64_t loam_sys_rename(loam_str from, loam_str to) {
    (void)from;
    (void)to;
    return 1;
}
#else
#include <sys/wait.h>

static int loam_sys_last_status = 1;

static inline int64_t loam_sys_env_set(loam_str name) {
    char buf[256];
    const char *v;
    if (!name.ptr || name.len <= 0 || name.len >= 256) return 0;
    memcpy(buf, name.ptr, (size_t)name.len);
    buf[name.len] = 0;
    v = getenv(buf);
    if (!v || !v[0]) return 0;
    return 1;
}
static inline void loam_sys_exit(int64_t code) { exit((int)code); }

static inline loam_str loam_sys_env(loam_str name) {
    char buf[256];
    const char *v;
    size_t n;
    char *p;
    if (!name.ptr || name.len <= 0 || name.len >= 256) return (loam_str){"", 0};
    memcpy(buf, name.ptr, (size_t)name.len);
    buf[name.len] = 0;
    v = getenv(buf);
    if (!v) return (loam_str){"", 0};
    n = strlen(v);
    p = (char *)loam_new(n + 1, "sys_env", 0);
    memcpy(p, v, n + 1);
    return (loam_str){p, (int64_t)n};
}

static inline int64_t loam_sys_write_file(loam_str path, loam_str body) {
    char pbuf[4096];
    FILE *f;
    if (!path.ptr || path.len <= 0 || path.len >= 4095) return 1;
    memcpy(pbuf, path.ptr, (size_t)path.len);
    pbuf[path.len] = 0;
    f = fopen(pbuf, "wb");
    if (!f) return 1;
    if (body.ptr && body.len > 0) {
        if (fwrite(body.ptr, 1, (size_t)body.len, f) != (size_t)body.len) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static inline loam_str loam_sys_exec(loam_str cmd) {
    char cbuf[8192];
    FILE *p;
    size_t cap = 4096;
    size_t n = 0;
    char *out;
    int st;
    const size_t max_out = 262144;
    if (!cmd.ptr || cmd.len <= 0 || cmd.len >= 8191) {
        loam_sys_last_status = 1;
        return (loam_str){"", 0};
    }
    memcpy(cbuf, cmd.ptr, (size_t)cmd.len);
    cbuf[cmd.len] = 0;
    p = popen(cbuf, "r");
    if (!p) {
        loam_sys_last_status = 1;
        return (loam_str){"", 0};
    }
    out = (char *)loam_new(cap, "sys_exec", 0);
    for (;;) {
        size_t r;
        if (n + 512 >= cap) {
            size_t ncap = cap * 2;
            char *nbuf;
            if (ncap > max_out + 512) ncap = max_out + 512;
            nbuf = (char *)loam_new(ncap, "sys_exec", 0);
            memcpy(nbuf, out, n);
            free(out);
            out = nbuf;
            cap = ncap;
        }
        if (n >= max_out) break;
        r = fread(out + n, 1, 512, p);
        n += r;
        if (r < 512) break;
    }
    st = pclose(p);
    if (st == -1) loam_sys_last_status = 1;
    else if (WIFEXITED(st)) loam_sys_last_status = WEXITSTATUS(st);
    else loam_sys_last_status = 1;
    out[n] = 0;
    return (loam_str){out, (int64_t)n};
}

static inline int64_t loam_sys_exec_status(void) { return loam_sys_last_status; }

static inline loam_str loam_sys_read_file(loam_str path) {
    char pbuf[4096];
    FILE *f;
    long sz;
    char *p;
    size_t n;
    const size_t max_n = 16u * 1024u * 1024u;
    if (!path.ptr || path.len <= 0 || path.len >= 4095) return (loam_str){"", 0};
    memcpy(pbuf, path.ptr, (size_t)path.len);
    pbuf[path.len] = 0;
    f = fopen(pbuf, "rb");
    if (!f) return (loam_str){"", 0};
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return (loam_str){"", 0};
    }
    sz = ftell(f);
    if (sz < 0 || (size_t)sz > max_n) {
        fclose(f);
        return (loam_str){"", 0};
    }
    rewind(f);
    p = (char *)loam_new((size_t)sz + 1, "sys_read_file", 0);
    n = fread(p, 1, (size_t)sz, f);
    fclose(f);
    p[n] = 0;
    return (loam_str){p, (int64_t)n};
}

#include <sys/stat.h>

static int loam_sys_cpath(loam_str path, char *buf, size_t cap) {
    if (!path.ptr || path.len <= 0 || (size_t)path.len + 1 > cap) return 1;
    memcpy(buf, path.ptr, (size_t)path.len);
    buf[path.len] = 0;
    return 0;
}

static inline int64_t loam_sys_mkdir(loam_str path) {
    char buf[4096];
    size_t i, n;
    if (loam_sys_cpath(path, buf, sizeof buf)) return 1;
    n = (size_t)path.len;
    for (i = 1; i <= n; i++) {
        char save;
        if (i != n && buf[i] != '/') continue;
        save = buf[i];
        buf[i] = 0;
        if (buf[0] && mkdir(buf, 0755) != 0 && errno != EEXIST) return 1;
        buf[i] = save;
    }
    return 0;
}

static inline int64_t loam_sys_rename(loam_str from, loam_str to) {
    char a[4096], b[4096];
    if (loam_sys_cpath(from, a, sizeof a) || loam_sys_cpath(to, b, sizeof b)) return 1;
    return rename(a, b) == 0 ? 0 : 1;
}
#endif

/* --- async: std/async.loam. Timers/queues live in Loam; the C seam is a  ---
   --- monotonic clock, a blocking sleep, and typed Future<T> mailboxes.  --- */

typedef struct {
    void *ptr;
    size_t sz;
    int64_t ready;
} loam_fut_slot;

static loam_fut_slot *loam_fut_slots;
static size_t loam_fut_n;
static size_t loam_fut_cap;

static inline int64_t loam_fut_push(const void *val, size_t sz) {
    loam_fut_slot s;
    if (loam_fut_n == loam_fut_cap) {
        size_t nc = loam_fut_cap ? loam_fut_cap * 2 : 16;
        loam_fut_slot *next = (loam_fut_slot *)realloc(loam_fut_slots, nc * sizeof(loam_fut_slot));
        if (!next) loam_panic(__FILE__, __LINE__, "out of memory");
        loam_fut_slots = next;
        loam_fut_cap = nc;
    }
    s.sz = sz;
    s.ready = 0;
    s.ptr = malloc(sz ? sz : 1);
    if (!s.ptr) loam_panic(__FILE__, __LINE__, "out of memory");
    if (val && sz) memcpy(s.ptr, val, sz);
    else memset(s.ptr, 0, sz ? sz : 1);
    loam_fut_slots[loam_fut_n] = s;
    return (int64_t)loam_fut_n++;
}

static inline int loam_fut_live(int64_t id) {
    return id >= 0 && (size_t)id < loam_fut_n && loam_fut_slots[id].ptr != NULL;
}

static inline int64_t loam_fut_ready(int64_t id) {
    if (!loam_fut_live(id)) return 0;
    return loam_fut_slots[id].ready;
}

static inline void loam_fut_clear(int64_t id) {
    if (!loam_fut_live(id)) return;
    loam_fut_slots[id].ready = 0;
}

static inline void loam_fut_load(int64_t id, void *out, size_t sz) {
    size_t n;
    if (!out) return;
    if (!loam_fut_live(id)) {
        memset(out, 0, sz);
        return;
    }
    n = sz < loam_fut_slots[id].sz ? sz : loam_fut_slots[id].sz;
    if (n) memcpy(out, loam_fut_slots[id].ptr, n);
    if (sz > n) memset((char *)out + n, 0, sz - n);
}

static inline void loam_fut_store(int64_t id, const void *val, size_t sz) {
    if (!loam_fut_live(id) || !val) return;
    if (sz != loam_fut_slots[id].sz) {
        free(loam_fut_slots[id].ptr);
        loam_fut_slots[id].ptr = malloc(sz ? sz : 1);
        if (!loam_fut_slots[id].ptr) loam_panic(__FILE__, __LINE__, "out of memory");
        loam_fut_slots[id].sz = sz;
    }
    if (sz) memcpy(loam_fut_slots[id].ptr, val, sz);
    loam_fut_slots[id].ready = 1;
}

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("now_ms")))
int64_t zeus_js_now_ms(void);
static inline int64_t loam_async_now_ms(void) { return zeus_js_now_ms(); }
static inline void loam_async_sleep(int64_t ms) {
    int64_t t0 = loam_async_now_ms();
    while (loam_async_now_ms() - t0 < ms) {
    }
}
#else
static inline int64_t loam_async_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}
static inline void loam_async_sleep(int64_t ms) {
    struct timespec req;
    if (ms <= 0) return;
    req.tv_sec = (time_t)(ms / 1000);
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}
#endif

/* --- thread + channel: std/thread.loam. The ABI is a detached pthread and a ---
   --- bounded FIFO of byte payloads guarded by a mutex + two condvars. All ---
   --- queue/dispatch policy lives in Loam; this is only the synchronization. --- */

#ifndef __wasm32__
#include <pthread.h>

static int64_t loam_thread_live;            /* detached workers still running */
static pthread_mutex_t loam_thread_mtx = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    unsigned char *buf;     /* cap * esz ring */
    size_t esz;             /* payload byte size (sizeof T) */
    int64_t cap;            /* slots */
    int64_t head;           /* first live slot */
    int64_t count;          /* live slots */
    pthread_mutex_t mtx;
    pthread_cond_t has_data, has_space;
} loam_ch;

static loam_ch **loam_chs;
static size_t loam_ch_n, loam_ch_cap;
static pthread_mutex_t loam_ch_tab_mtx = PTHREAD_MUTEX_INITIALIZER;

static int64_t loam_ch_alloc(int64_t cap, size_t esz) {
    loam_ch *c;
    size_t i;
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
    pthread_mutex_lock(&loam_ch_tab_mtx);
    if (loam_ch_n == loam_ch_cap) {
        size_t nc = loam_ch_cap ? loam_ch_cap * 2 : 16;
        loam_ch **next = (loam_ch **)realloc(loam_chs, nc * sizeof(loam_ch *));
        if (!next) loam_panic(__FILE__, __LINE__, "out of memory");
        loam_chs = next;
        loam_ch_cap = nc;
    }
    i = loam_ch_n;
    loam_chs[i] = c;
    loam_ch_n++;
    pthread_mutex_unlock(&loam_ch_tab_mtx);
    return (int64_t)i;
}

static inline loam_ch *loam_ch_get(int64_t id) {
    loam_ch *c;
    /* Readers may run on worker threads while the UI thread grows the table
       (realloc). Lock the table for the fetch so the entry pointer is read
       race-free; the entry itself is stable once published. */
    pthread_mutex_lock(&loam_ch_tab_mtx);
    if (id < 0 || (size_t)id >= loam_ch_n)
        c = NULL;
    else
        c = loam_chs[(size_t)id];
    pthread_mutex_unlock(&loam_ch_tab_mtx);
    return c;
}

static inline void loam_ch_send(int64_t id, const void *val, size_t sz) {
    loam_ch *c = loam_ch_get(id);
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

static inline int64_t loam_ch_try_send(int64_t id, const void *val, size_t sz) {
    loam_ch *c = loam_ch_get(id);
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

static inline void loam_ch_recv(int64_t id, void *out, size_t sz) {
    loam_ch *c = loam_ch_get(id);
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

static inline int64_t loam_ch_pop(int64_t id, void *out, size_t sz) {
    loam_ch *c = loam_ch_get(id);
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

static inline int64_t loam_ch_ready(int64_t id) {
    loam_ch *c = loam_ch_get(id);
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
static inline int64_t loam_ch_alloc(int64_t cap, size_t esz) {
    (void)cap;
    (void)esz;
    return -1;
}
static inline void loam_ch_send(int64_t id, const void *val, size_t sz) {
    (void)id;
    (void)val;
    (void)sz;
}
static inline int64_t loam_ch_try_send(int64_t id, const void *val, size_t sz) {
    (void)id;
    (void)val;
    (void)sz;
    return 0;
}
static inline void loam_ch_recv(int64_t id, void *out, size_t sz) {
    (void)id;
    (void)sz;
    if (out) memset(out, 0, sz);
}
static inline int64_t loam_ch_pop(int64_t id, void *out, size_t sz) {
    (void)id;
    (void)sz;
    if (out) memset(out, 0, sz);
    return 0;
}
static inline int64_t loam_ch_ready(int64_t id) {
    (void)id;
    return 0;
}
static inline void loam_thread_spawn(loam_fn cb) {
    (void)cb; /* wasm has no threads; spawn is a no-op */
}
static inline int64_t loam_thread_running(void) { return 0; }
#endif

#endif /* LOAM_RT_H */
