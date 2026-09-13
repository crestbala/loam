/* yuga_rt.h — panics, overflow, bounds. Included at the top of generated C. */
#ifndef YUGA_RT_H
#define YUGA_RT_H

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
#endif

typedef struct {
    const char *ptr;
    int64_t len;
} yuga_str;

typedef struct {
    void *fn;
    void *env;
    /* Byte size of `env` when it is a closure heap/stack record. intern_fn
       memcpy's this so the handler outlives the value that was interned. */
    size_t env_size;
} yuga_fn;

typedef struct {
    void *ptr;
    int64_t len;
    int64_t cap;
} yuga_vec;

/* Length-based stdout. No malloc, no printf, no NUL requirement. */
static inline void yuga_write_bytes(const char *p, size_t n) {
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

static inline void yuga_writev_all(struct iovec *iov, int n) {
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

static inline void yuga_fmt_write(yuga_str s) {
    if (!s.ptr || s.len <= 0) return;
    yuga_write_bytes(s.ptr, (size_t)s.len);
}

static inline yuga_str yuga_fmt_itoa(char buf[24], int64_t v) {
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
    yuga_str s;
    s.ptr = p;
    s.len = (int64_t)(end - p);
    return s;
}

static inline void yuga_fmt_write_int(int64_t v) {
    char buf[24];
    yuga_str s = yuga_fmt_itoa(buf, v);
    yuga_write_bytes(s.ptr, (size_t)s.len);
}

static inline void yuga_fmt_write_bool(bool v) {
    if (v) yuga_write_bytes("true", 4);
    else yuga_write_bytes("false", 5);
}

static inline yuga_str yuga_fmt_ftoa(char buf[64], double v) {
    int n = snprintf(buf, 64, "%.15g", v);
    if (n < 0) n = 0;
    if (n > 63) n = 63;
    yuga_str s;
    s.ptr = buf;
    s.len = (int64_t)n;
    return s;
}

static inline void yuga_fmt_write_float(double v) {
    char buf[64];
    yuga_str s = yuga_fmt_ftoa(buf, v);
    yuga_write_bytes(s.ptr, (size_t)s.len);
}

static inline void yuga_fmt_writeln(void) { yuga_write_bytes("\n", 1); }

static inline bool yuga_fmt_eq(yuga_str a, yuga_str b) {
    if (a.len != b.len) return false;
    if (a.len <= 0) return true;
    if (!a.ptr || !b.ptr) return a.ptr == b.ptr;
    return memcmp(a.ptr, b.ptr, (size_t)a.len) == 0;
}

static inline void yuga_panic(const char *file, int line, const char *msg) {
    fflush(stdout);
    fprintf(stderr, "%s:%d: panic: %s\n", file, line, msg);
    abort();
}

static inline int64_t yuga_add_i64(int64_t a, int64_t b, const char *f, int l) {
    int64_t r;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_add_overflow(a, b, &r)) yuga_panic(f, l, "integer overflow");
    return r;
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        yuga_panic(f, l, "integer overflow");
    return a + b;
#endif
}

static inline int64_t yuga_sub_i64(int64_t a, int64_t b, const char *f, int l) {
    int64_t r;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_sub_overflow(a, b, &r)) yuga_panic(f, l, "integer overflow");
    return r;
#else
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
        yuga_panic(f, l, "integer overflow");
    return a - b;
#endif
}

static inline int64_t yuga_mul_i64(int64_t a, int64_t b, const char *f, int l) {
    int64_t r;
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_mul_overflow(a, b, &r)) yuga_panic(f, l, "integer overflow");
    return r;
#else
    if (a != 0 && b != 0) {
        if (a > 0 && b > 0 && a > INT64_MAX / b) yuga_panic(f, l, "integer overflow");
        if (a > 0 && b < 0 && b < INT64_MIN / a) yuga_panic(f, l, "integer overflow");
        if (a < 0 && b > 0 && a < INT64_MIN / b) yuga_panic(f, l, "integer overflow");
        if (a < 0 && b < 0 && a < INT64_MAX / b) yuga_panic(f, l, "integer overflow");
    }
    return a * b;
#endif
}

static inline int64_t yuga_div_i64(int64_t a, int64_t b, const char *f, int l) {
    if (b == 0) yuga_panic(f, l, "division by zero");
    if (a == INT64_MIN && b == -1) yuga_panic(f, l, "integer overflow");
    return a / b;
}

static inline int64_t yuga_mod_i64(int64_t a, int64_t b, const char *f, int l) {
    if (b == 0) yuga_panic(f, l, "division by zero");
    if (a == INT64_MIN && b == -1) return 0;
    return a % b;
}

static inline int64_t yuga_wrapping_add(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a + (uint64_t)b);
}

static inline int64_t yuga_wrapping_shr(int64_t a, int64_t n) {
    if (n < 0 || n > 63) return 0;
    return (int64_t)((uint64_t)a >> (unsigned)n);
}

static inline int64_t yuga_wrapping_shl(int64_t a, int64_t n) {
    if (n < 0 || n > 63) return 0;
    return (int64_t)((uint64_t)a << (unsigned)n);
}

static inline int64_t yuga_wrapping_or(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a | (uint64_t)b);
}

static inline int64_t yuga_wrapping_and(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a & (uint64_t)b);
}

static inline int64_t yuga_saturating_add(int64_t a, int64_t b) {
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
#define YUGA_DEF_INT_ASM(SFX, T)                                                  \
static inline T yuga_add_##SFX(T a, T b, const char *f, int l) {                  \
    T r; if (__builtin_add_overflow(a, b, &r)) yuga_panic(f, l, "integer overflow"); \
    return r;                                                                     \
}                                                                                 \
static inline T yuga_sub_##SFX(T a, T b, const char *f, int l) {                  \
    T r; if (__builtin_sub_overflow(a, b, &r)) yuga_panic(f, l, "integer overflow"); \
    return r;                                                                     \
}                                                                                 \
static inline T yuga_mul_##SFX(T a, T b, const char *f, int l) {                  \
    T r; if (__builtin_mul_overflow(a, b, &r)) yuga_panic(f, l, "integer overflow"); \
    return r;                                                                     \
}                                                                                 \
static inline T yuga_neg_##SFX(T a, const char *f, int l) {                       \
    T r; if (__builtin_sub_overflow((T)0, a, &r)) yuga_panic(f, l, "integer overflow"); \
    return r;                                                                     \
}

#define YUGA_DEF_INT_S(SFX, T, MINV) YUGA_DEF_INT_ASM(SFX, T)                    \
static inline T yuga_div_##SFX(T a, T b, const char *f, int l) {                  \
    if (b == 0) yuga_panic(f, l, "division by zero");                            \
    if (a == (T)(MINV) && b == (T)-1) yuga_panic(f, l, "integer overflow");      \
    return (T)(a / b);                                                            \
}                                                                                 \
static inline T yuga_mod_##SFX(T a, T b, const char *f, int l) {                  \
    if (b == 0) yuga_panic(f, l, "division by zero");                            \
    if (a == (T)(MINV) && b == (T)-1) return (T)0;                                \
    return (T)(a % b);                                                            \
}

#define YUGA_DEF_INT_U(SFX, T) YUGA_DEF_INT_ASM(SFX, T)                           \
static inline T yuga_div_##SFX(T a, T b, const char *f, int l) {                  \
    if (b == 0) yuga_panic(f, l, "division by zero");                            \
    return (T)(a / b);                                                            \
}                                                                                 \
static inline T yuga_mod_##SFX(T a, T b, const char *f, int l) {                  \
    if (b == 0) yuga_panic(f, l, "division by zero");                            \
    return (T)(a % b);                                                            \
}

YUGA_DEF_INT_S(i8, int8_t, INT8_MIN)
YUGA_DEF_INT_S(i16, int16_t, INT16_MIN)
YUGA_DEF_INT_S(i32, int32_t, INT32_MIN)
YUGA_DEF_INT_U(u8, uint8_t)
YUGA_DEF_INT_U(u16, uint16_t)
YUGA_DEF_INT_U(u32, uint32_t)
YUGA_DEF_INT_U(u64, uint64_t)
static inline int64_t yuga_neg_i64(int64_t a, const char *f, int l) {
    int64_t r;
    if (__builtin_sub_overflow((int64_t)0, a, &r)) yuga_panic(f, l, "integer overflow");
    return r;
}

/* Shifts lower through the unsigned type so a signed left shift is never UB,
 * and a count at or past the width traps (§3.5 rule 3). */
#define YUGA_DEF_SHIFT(SFX, T, UT)                                                \
static inline T yuga_shl_##SFX(T a, uint64_t n, const char *f, int l) {           \
    if (n >= (uint64_t)(sizeof(T) * 8)) yuga_panic(f, l, "shift amount out of range"); \
    return (T)((UT)a << n);                                                       \
}                                                                                 \
static inline T yuga_shr_##SFX(T a, uint64_t n, const char *f, int l) {           \
    if (n >= (uint64_t)(sizeof(T) * 8)) yuga_panic(f, l, "shift amount out of range"); \
    return (T)(a >> n);                                                           \
}

YUGA_DEF_SHIFT(i8, int8_t, uint8_t)
YUGA_DEF_SHIFT(i16, int16_t, uint16_t)
YUGA_DEF_SHIFT(i32, int32_t, uint32_t)
YUGA_DEF_SHIFT(i64, int64_t, uint64_t)
YUGA_DEF_SHIFT(u8, uint8_t, uint8_t)
YUGA_DEF_SHIFT(u16, uint16_t, uint16_t)
YUGA_DEF_SHIFT(u32, uint32_t, uint32_t)
YUGA_DEF_SHIFT(u64, uint64_t, uint64_t)

/* Per-width explicit wrap (per §3.4). */
#define YUGA_DEF_WRAP(SFX, T, UT)                                                 \
static inline T yuga_wrapping_add_##SFX(T a, T b) { return (T)((UT)a + (UT)b); }  \
static inline T yuga_wrapping_sub_##SFX(T a, T b) { return (T)((UT)a - (UT)b); }  \
static inline T yuga_wrapping_mul_##SFX(T a, T b) { return (T)((UT)a * (UT)b); }  \
static inline T yuga_wrapping_neg_##SFX(T a) { return (T)((UT)0 - (UT)a); }       \
static inline T yuga_wrapping_shl_##SFX(T a, UT n) {                              \
    return n >= (UT)(sizeof(T) * 8) ? (T)0 : (T)((UT)a << n);                     \
}                                                                                 \
static inline T yuga_wrapping_shr_##SFX(T a, UT n) {                              \
    return n >= (UT)(sizeof(T) * 8) ? (T)0 : (T)((UT)a >> n);                     \
}                                                                                 \
static inline T yuga_wrapping_and_##SFX(T a, T b) { return (T)(a & b); }          \
static inline T yuga_wrapping_or_##SFX(T a, T b) { return (T)(a | b); }           \
static inline T yuga_wrapping_xor_##SFX(T a, T b) { return (T)(a ^ b); }

YUGA_DEF_WRAP(i8, int8_t, uint8_t)
YUGA_DEF_WRAP(i16, int16_t, uint16_t)
YUGA_DEF_WRAP(i32, int32_t, uint32_t)
YUGA_DEF_WRAP(i64, int64_t, uint64_t)
YUGA_DEF_WRAP(u8, uint8_t, uint8_t)
YUGA_DEF_WRAP(u16, uint16_t, uint16_t)
YUGA_DEF_WRAP(u32, uint32_t, uint32_t)
YUGA_DEF_WRAP(u64, uint64_t, uint64_t)

/* Per-width saturating arithmetic. */
#define YUGA_DEF_SAT(SFX, T, MINV, MAXV)                                          \
static inline T yuga_saturating_add_##SFX(T a, T b) {                             \
    T r; if (!__builtin_add_overflow(a, b, &r)) return r;                         \
    return (b > 0) ? (T)(MAXV) : (T)(MINV);                                       \
}                                                                                 \
static inline T yuga_saturating_sub_##SFX(T a, T b) {                             \
    T r; if (!__builtin_sub_overflow(a, b, &r)) return r;                         \
    return (b < 0) ? (T)(MAXV) : (T)(MINV);                                       \
}                                                                                 \
static inline T yuga_saturating_mul_##SFX(T a, T b) {                             \
    T r; if (!__builtin_mul_overflow(a, b, &r)) return r;                         \
    return ((a > 0) == (b > 0)) ? (T)(MAXV) : (T)(MINV);                          \
}

YUGA_DEF_SAT(i8, int8_t, INT8_MIN, INT8_MAX)
YUGA_DEF_SAT(i16, int16_t, INT16_MIN, INT16_MAX)
YUGA_DEF_SAT(i32, int32_t, INT32_MIN, INT32_MAX)
YUGA_DEF_SAT(i64, int64_t, INT64_MIN, INT64_MAX)
YUGA_DEF_SAT(u8, uint8_t, 0, UINT8_MAX)
YUGA_DEF_SAT(u16, uint16_t, 0, UINT16_MAX)
YUGA_DEF_SAT(u32, uint32_t, 0, UINT32_MAX)
YUGA_DEF_SAT(u64, uint64_t, 0, UINT64_MAX)

/* Numeric narrowing conversions trap on a value the destination cannot hold
 * (§3.4). `convu`/`satu` take a u64 source; the others an int64 source. */
#define YUGA_DEF_CONV(SFX, T, LO, HI)                                             \
static inline T yuga_conv_##SFX(int64_t v, const char *f, int l) {                \
    if (v < (int64_t)(LO) || v > (int64_t)(HI))                                   \
        yuga_panic(f, l, "integer conversion out of range");                      \
    return (T)v;                                                                  \
}                                                                                 \
static inline T yuga_convf_##SFX(double v, const char *f, int l) {                \
    if (!(v >= (double)(LO) && v <= (double)(HI)))                                \
        yuga_panic(f, l, "float conversion out of range");                        \
    return (T)v;                                                                  \
}                                                                                 \
static inline T yuga_sat_##SFX(int64_t v) {                                       \
    if (v < (int64_t)(LO)) return (T)(LO);                                        \
    if (v > (int64_t)(HI)) return (T)(HI);                                        \
    return (T)v;                                                                  \
}

YUGA_DEF_CONV(i8, int8_t, INT8_MIN, INT8_MAX)
YUGA_DEF_CONV(i16, int16_t, INT16_MIN, INT16_MAX)
YUGA_DEF_CONV(i32, int32_t, INT32_MIN, INT32_MAX)
YUGA_DEF_CONV(u8, uint8_t, 0, UINT8_MAX)
YUGA_DEF_CONV(u16, uint16_t, 0, UINT16_MAX)
YUGA_DEF_CONV(u32, uint32_t, 0, UINT32_MAX)
static inline int64_t yuga_conv_i64(int64_t v, const char *f, int l) {
    (void)f; (void)l; return v;
}
static inline int64_t yuga_convf_i64(double v, const char *f, int l) {
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        yuga_panic(f, l, "float conversion out of range");
    return (int64_t)v;
}
static inline int64_t yuga_sat_i64(int64_t v) { return v; }
static inline uint64_t yuga_conv_u64(int64_t v, const char *f, int l) {
    if (v < 0) yuga_panic(f, l, "integer conversion out of range");
    return (uint64_t)v;
}
static inline uint64_t yuga_convf_u64(double v, const char *f, int l) {
    if (!(v >= 0.0 && v < 18446744073709551616.0))
        yuga_panic(f, l, "float conversion out of range");
    return (uint64_t)v;
}
static inline uint64_t yuga_sat_u64(int64_t v) { return v < 0 ? (uint64_t)0 : (uint64_t)v; }

#define YUGA_DEF_CONVU(SFX, T, MAXV)                                              \
static inline T yuga_convu_##SFX(uint64_t v, const char *f, int l) {              \
    if (v > (uint64_t)(MAXV)) yuga_panic(f, l, "integer conversion out of range"); \
    return (T)v;                                                                  \
}                                                                                 \
static inline T yuga_satu_##SFX(uint64_t v) {                                     \
    return v > (uint64_t)(MAXV) ? (T)(MAXV) : (T)v;                               \
}

YUGA_DEF_CONVU(i8, int8_t, INT8_MAX)
YUGA_DEF_CONVU(i16, int16_t, INT16_MAX)
YUGA_DEF_CONVU(i32, int32_t, INT32_MAX)
YUGA_DEF_CONVU(i64, int64_t, INT64_MAX)
YUGA_DEF_CONVU(u8, uint8_t, UINT8_MAX)
YUGA_DEF_CONVU(u16, uint16_t, UINT16_MAX)
YUGA_DEF_CONVU(u32, uint32_t, UINT32_MAX)

static inline int64_t yuga_idx(int64_t i, int64_t n, const char *f, int l) {
    if (i < 0 || i >= n) yuga_panic(f, l, "index out of bounds");
    return i;
}

static inline void *yuga_new(size_t sz, const char *f, int l) {
    void *p = malloc(sz ? sz : 1);
    if (!p) yuga_panic(f, l, "out of memory");
    return p;
}

static inline void yuga_drop(void **p) {
    if (p && *p) {
        free(*p);
        *p = NULL;
    }
}

static inline void *yuga_move_ptr(void **src) {
    void *p = src ? *src : NULL;
    if (src) *src = NULL;
    return p;
}

static inline yuga_vec yuga_vec_new(void) {
    yuga_vec v;
    v.ptr = NULL;
    v.len = 0;
    v.cap = 0;
    return v;
}

static inline int64_t *yuga_vec_rc(void *ptr) {
    return ptr ? ((int64_t *)ptr - 1) : NULL;
}

static inline yuga_vec yuga_vec_retain(const yuga_vec *src) {
    yuga_vec v = yuga_vec_new();
    if (!src) return v;
    v = *src;
    if (v.ptr) {
        int64_t *rc = yuga_vec_rc(v.ptr);
        (*rc)++;
    }
    return v;
}

static inline yuga_vec yuga_vec_move(yuga_vec *src) {
    yuga_vec v;
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

static inline void yuga_vec_unique(yuga_vec *v, size_t esz, const char *f, int l) {
    int64_t *rc;
    void *raw;
    size_t bytes;
    if (!v || !v->ptr) return;
    rc = yuga_vec_rc(v->ptr);
    if (*rc <= 1) return;
    bytes = sizeof(int64_t) + (esz ? (size_t)v->cap * esz : 1);
    raw = malloc(bytes);
    if (!raw) yuga_panic(f, l, "out of memory");
    *(int64_t *)raw = 1;
    if (v->len > 0 && esz)
        memcpy((char *)raw + sizeof(int64_t), v->ptr, (size_t)v->len * esz);
    (*rc)--;
    v->ptr = (char *)raw + sizeof(int64_t);
}

static inline void yuga_vec_reserve(yuga_vec *v, int64_t n, size_t esz, const char *f, int l) {
    int64_t cap;
    void *raw;
    if (!v || n <= v->cap) return;
    yuga_vec_unique(v, esz, f, l);
    cap = v->cap ? v->cap : 8;
    while (cap < n) {
        if (cap > INT64_MAX / 2) yuga_panic(f, l, "out of memory");
        cap *= 2;
    }
    if (!v->ptr) {
        raw = malloc(sizeof(int64_t) + (esz ? (size_t)cap * esz : 1));
        if (!raw) yuga_panic(f, l, "out of memory");
        *(int64_t *)raw = 1;
        v->ptr = (char *)raw + sizeof(int64_t);
        v->cap = cap;
        return;
    }
    raw = realloc((char *)v->ptr - sizeof(int64_t),
                  sizeof(int64_t) + (esz ? (size_t)cap * esz : 1));
    if (!raw) yuga_panic(f, l, "out of memory");
    v->ptr = (char *)raw + sizeof(int64_t);
    v->cap = cap;
}

static inline void yuga_vec_push(yuga_vec *v, const void *elem, size_t esz, const char *f, int l) {
    if (!v) yuga_panic(f, l, "push on null array");
    yuga_vec_reserve(v, v->len + 1, esz, f, l);
    if (esz && elem) memcpy((char *)v->ptr + (size_t)v->len * esz, elem, esz);
    v->len++;
}

static inline void yuga_vec_pop(yuga_vec *v, void *out, size_t esz, const char *f, int l) {
    if (!v || v->len <= 0) yuga_panic(f, l, "pop from empty array");
    yuga_vec_unique(v, esz, f, l);
    v->len--;
    if (esz && out) memcpy(out, (char *)v->ptr + (size_t)v->len * esz, esz);
}

static inline void yuga_vec_drop(yuga_vec *v) {
    int64_t *rc;
    if (!v) return;
    if (!v->ptr) {
        v->len = 0;
        v->cap = 0;
        return;
    }
    rc = yuga_vec_rc(v->ptr);
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

static inline yuga_fn yuga_fn_move(yuga_fn *src) {
    yuga_fn v;
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

static inline void yuga_fn_drop(yuga_fn *f) {
    if (!f) return;
    if (f->env) free(f->env);
    f->env = NULL;
    f->fn = NULL;
}

/* `{{ }}` interpolation. Each piece is converted to a yuga_str and the
   pieces are concatenated left to right. The result is heap-allocated with a
   trailing NUL and, like yuga_string_from_bytes, is never freed — the
   language has no string ownership story to hook into yet. */
static inline yuga_str yuga_str_concat(yuga_str a, yuga_str b) {
    int64_t an = a.len > 0 ? a.len : 0;
    int64_t bn = b.len > 0 ? b.len : 0;
    if (!an) { if (bn) return b; return (yuga_str){ .ptr = "", .len = 0 }; }
    if (!bn) return a;
    char *p = (char *)yuga_new((size_t)(an + bn) + 1, "str_concat", 0);
    if (a.ptr) memcpy(p, a.ptr, (size_t)an);
    if (b.ptr) memcpy(p + an, b.ptr, (size_t)bn);
    p[an + bn] = 0;
    return (yuga_str){ .ptr = p, .len = an + bn };
}

static inline yuga_str yuga_str_of_int(int64_t v) {
    char buf[24];
    yuga_str s = yuga_fmt_itoa(buf, v);
    char *p = (char *)yuga_new((size_t)s.len + 1, "str_of_int", 0);
    memcpy(p, s.ptr, (size_t)s.len);
    p[s.len] = 0;
    return (yuga_str){ .ptr = p, .len = s.len };
}

static inline yuga_str yuga_str_of_float(double v) {
    char buf[64];
    yuga_str s = yuga_fmt_ftoa(buf, v);
    char *p = (char *)yuga_new((size_t)s.len + 1, "str_of_float", 0);
    memcpy(p, s.ptr, (size_t)s.len);
    p[s.len] = 0;
    return (yuga_str){ .ptr = p, .len = s.len };
}

static inline yuga_str yuga_str_of_bool(bool v) {
    return v ? (yuga_str){ .ptr = "true", .len = 4 } : (yuga_str){ .ptr = "false", .len = 5 };
}

static inline yuga_str yuga_str_of_string(yuga_str s) { return s; }

/* Takes ownership of `b` (IR_CALL steals []int). Trailing NUL for C hosts. */
static inline yuga_str yuga_string_from_bytes(yuga_vec b) {
    int64_t n = b.len > 0 ? b.len : 0;
    char *p = (char *)yuga_new((size_t)n + 1, "string_from_bytes", 0);
    int32_t *el = (int32_t *)b.ptr;
    int64_t i;
    for (i = 0; i < n; i++)
        p[i] = (char)(el ? (el[i] & 255) : 0);
    p[n] = 0;
    yuga_vec_drop(&b);
    return (yuga_str){ .ptr = p, .len = n };
}

/** Compile target: "wasm" | "ios" | "android" | "native". Used by
 *  `http.client()` to pick a default RPC address. */
static inline yuga_str yuga_sys_target(void) {
#ifdef __wasm32__
    return (yuga_str){"wasm", 4};
#elif defined(YUGA_ANDROID)
    return (yuga_str){"android", 7};
#elif defined(YUGA_IOS)
    return (yuga_str){"ios", 3};
#else
    return (yuga_str){"native", 6};
#endif
}

#ifdef __wasm32__
static inline int64_t yuga_sys_env_set(yuga_str name) {
    (void)name;
    return 0;
}
static inline void yuga_sys_exit(int64_t code) {
    (void)code;
    abort();
}
static inline yuga_str yuga_sys_env(yuga_str name) {
    (void)name;
    return (yuga_str){"", 0};
}
static inline int64_t yuga_sys_write_file(yuga_str path, yuga_str body) {
    (void)path;
    (void)body;
    return 1;
}
static inline yuga_str yuga_sys_exec(yuga_str cmd) {
    (void)cmd;
    return (yuga_str){"", 0};
}
static inline int64_t yuga_sys_exec_status(void) { return 1; }
static inline yuga_str yuga_sys_read_file(yuga_str path) {
    (void)path;
    return (yuga_str){"", 0};
}
static inline int64_t yuga_sys_mkdir(yuga_str path) {
    (void)path;
    return 1;
}
static inline int64_t yuga_sys_rename(yuga_str from, yuga_str to) {
    (void)from;
    (void)to;
    return 1;
}
#else
#include <sys/wait.h>

static int yuga_sys_last_status = 1;

static inline int64_t yuga_sys_env_set(yuga_str name) {
    char buf[256];
    const char *v;
    if (!name.ptr || name.len <= 0 || name.len >= 256) return 0;
    memcpy(buf, name.ptr, (size_t)name.len);
    buf[name.len] = 0;
    v = getenv(buf);
    if (!v || !v[0]) return 0;
    return 1;
}
static inline void yuga_sys_exit(int64_t code) { exit((int)code); }

static inline yuga_str yuga_sys_env(yuga_str name) {
    char buf[256];
    const char *v;
    size_t n;
    char *p;
    if (!name.ptr || name.len <= 0 || name.len >= 256) return (yuga_str){"", 0};
    memcpy(buf, name.ptr, (size_t)name.len);
    buf[name.len] = 0;
    v = getenv(buf);
    if (!v) return (yuga_str){"", 0};
    n = strlen(v);
    p = (char *)yuga_new(n + 1, "sys_env", 0);
    memcpy(p, v, n + 1);
    return (yuga_str){p, (int64_t)n};
}

static inline int64_t yuga_sys_write_file(yuga_str path, yuga_str body) {
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

static inline yuga_str yuga_sys_exec(yuga_str cmd) {
    char cbuf[8192];
    FILE *p;
    size_t cap = 4096;
    size_t n = 0;
    char *out;
    int st;
    const size_t max_out = 262144;
    if (!cmd.ptr || cmd.len <= 0 || cmd.len >= 8191) {
        yuga_sys_last_status = 1;
        return (yuga_str){"", 0};
    }
    memcpy(cbuf, cmd.ptr, (size_t)cmd.len);
    cbuf[cmd.len] = 0;
    p = popen(cbuf, "r");
    if (!p) {
        yuga_sys_last_status = 1;
        return (yuga_str){"", 0};
    }
    out = (char *)yuga_new(cap, "sys_exec", 0);
    for (;;) {
        size_t r;
        if (n + 512 >= cap) {
            size_t ncap = cap * 2;
            char *nbuf;
            if (ncap > max_out + 512) ncap = max_out + 512;
            nbuf = (char *)yuga_new(ncap, "sys_exec", 0);
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
    if (st == -1) yuga_sys_last_status = 1;
    else if (WIFEXITED(st)) yuga_sys_last_status = WEXITSTATUS(st);
    else yuga_sys_last_status = 1;
    out[n] = 0;
    return (yuga_str){out, (int64_t)n};
}

static inline int64_t yuga_sys_exec_status(void) { return yuga_sys_last_status; }

static inline yuga_str yuga_sys_read_file(yuga_str path) {
    char pbuf[4096];
    FILE *f;
    long sz;
    char *p;
    size_t n;
    const size_t max_n = 16u * 1024u * 1024u;
    if (!path.ptr || path.len <= 0 || path.len >= 4095) return (yuga_str){"", 0};
    memcpy(pbuf, path.ptr, (size_t)path.len);
    pbuf[path.len] = 0;
    f = fopen(pbuf, "rb");
    if (!f) return (yuga_str){"", 0};
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return (yuga_str){"", 0};
    }
    sz = ftell(f);
    if (sz < 0 || (size_t)sz > max_n) {
        fclose(f);
        return (yuga_str){"", 0};
    }
    rewind(f);
    p = (char *)yuga_new((size_t)sz + 1, "sys_read_file", 0);
    n = fread(p, 1, (size_t)sz, f);
    fclose(f);
    p[n] = 0;
    return (yuga_str){p, (int64_t)n};
}

#include <sys/stat.h>

static int yuga_sys_cpath(yuga_str path, char *buf, size_t cap) {
    if (!path.ptr || path.len <= 0 || (size_t)path.len + 1 > cap) return 1;
    memcpy(buf, path.ptr, (size_t)path.len);
    buf[path.len] = 0;
    return 0;
}

static inline int64_t yuga_sys_mkdir(yuga_str path) {
    char buf[4096];
    size_t i, n;
    if (yuga_sys_cpath(path, buf, sizeof buf)) return 1;
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

static inline int64_t yuga_sys_rename(yuga_str from, yuga_str to) {
    char a[4096], b[4096];
    if (yuga_sys_cpath(from, a, sizeof a) || yuga_sys_cpath(to, b, sizeof b)) return 1;
    return rename(a, b) == 0 ? 0 : 1;
}
#endif

/* --- async: std/async.yuga. Timers/queues live in Yuga; the C seam is a  ---
   --- monotonic clock, a blocking sleep, and typed Future<T> mailboxes.  --- */

typedef struct {
    void *ptr;
    size_t sz;
    int64_t ready;
} yuga_fut_slot;

static yuga_fut_slot *yuga_fut_slots;
static size_t yuga_fut_n;
static size_t yuga_fut_cap;

static inline int64_t yuga_fut_push(const void *val, size_t sz) {
    yuga_fut_slot s;
    if (yuga_fut_n == yuga_fut_cap) {
        size_t nc = yuga_fut_cap ? yuga_fut_cap * 2 : 16;
        yuga_fut_slot *next = (yuga_fut_slot *)realloc(yuga_fut_slots, nc * sizeof(yuga_fut_slot));
        if (!next) yuga_panic(__FILE__, __LINE__, "out of memory");
        yuga_fut_slots = next;
        yuga_fut_cap = nc;
    }
    s.sz = sz;
    s.ready = 0;
    s.ptr = malloc(sz ? sz : 1);
    if (!s.ptr) yuga_panic(__FILE__, __LINE__, "out of memory");
    if (val && sz) memcpy(s.ptr, val, sz);
    else memset(s.ptr, 0, sz ? sz : 1);
    yuga_fut_slots[yuga_fut_n] = s;
    return (int64_t)yuga_fut_n++;
}

static inline int yuga_fut_live(int64_t id) {
    return id >= 0 && (size_t)id < yuga_fut_n && yuga_fut_slots[id].ptr != NULL;
}

static inline int64_t yuga_fut_ready(int64_t id) {
    if (!yuga_fut_live(id)) return 0;
    return yuga_fut_slots[id].ready;
}

static inline void yuga_fut_clear(int64_t id) {
    if (!yuga_fut_live(id)) return;
    yuga_fut_slots[id].ready = 0;
}

static inline void yuga_fut_load(int64_t id, void *out, size_t sz) {
    size_t n;
    if (!out) return;
    if (!yuga_fut_live(id)) {
        memset(out, 0, sz);
        return;
    }
    n = sz < yuga_fut_slots[id].sz ? sz : yuga_fut_slots[id].sz;
    if (n) memcpy(out, yuga_fut_slots[id].ptr, n);
    if (sz > n) memset((char *)out + n, 0, sz - n);
}

static inline void yuga_fut_store(int64_t id, const void *val, size_t sz) {
    if (!yuga_fut_live(id) || !val) return;
    if (sz != yuga_fut_slots[id].sz) {
        free(yuga_fut_slots[id].ptr);
        yuga_fut_slots[id].ptr = malloc(sz ? sz : 1);
        if (!yuga_fut_slots[id].ptr) yuga_panic(__FILE__, __LINE__, "out of memory");
        yuga_fut_slots[id].sz = sz;
    }
    if (sz) memcpy(yuga_fut_slots[id].ptr, val, sz);
    yuga_fut_slots[id].ready = 1;
}

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("now_ms")))
int64_t zeus_js_now_ms(void);
static inline int64_t yuga_async_now_ms(void) { return zeus_js_now_ms(); }
static inline void yuga_async_sleep(int64_t ms) {
    int64_t t0 = yuga_async_now_ms();
    while (yuga_async_now_ms() - t0 < ms) {
    }
}
#else
static inline int64_t yuga_async_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}
static inline void yuga_async_sleep(int64_t ms) {
    struct timespec req;
    if (ms <= 0) return;
    req.tv_sec = (time_t)(ms / 1000);
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}
#endif

/* --- thread + channel: std/thread.yuga. The ABI is a detached pthread and a ---
   --- bounded FIFO of byte payloads guarded by a mutex + two condvars. All ---
   --- queue/dispatch policy lives in Yuga; this is only the synchronization. --- */

#ifndef __wasm32__
#include <pthread.h>

static int64_t yuga_thread_live;            /* detached workers still running */
static pthread_mutex_t yuga_thread_mtx = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    unsigned char *buf;     /* cap * esz ring */
    size_t esz;             /* payload byte size (sizeof T) */
    int64_t cap;            /* slots */
    int64_t head;           /* first live slot */
    int64_t count;          /* live slots */
    pthread_mutex_t mtx;
    pthread_cond_t has_data, has_space;
} yuga_ch;

static yuga_ch **yuga_chs;
static size_t yuga_ch_n, yuga_ch_cap;
static pthread_mutex_t yuga_ch_tab_mtx = PTHREAD_MUTEX_INITIALIZER;

static int64_t yuga_ch_alloc(int64_t cap, size_t esz) {
    yuga_ch *c;
    size_t i;
    if (cap < 1) cap = 1;
    if (esz < 1) esz = 1;
    c = (yuga_ch *)yuga_new(sizeof(yuga_ch), "channel", 0);
    c->buf = (unsigned char *)yuga_new((size_t)cap * esz, "channel", 0);
    c->esz = esz;
    c->cap = cap;
    c->head = 0;
    c->count = 0;
    pthread_mutex_init(&c->mtx, NULL);
    pthread_cond_init(&c->has_data, NULL);
    pthread_cond_init(&c->has_space, NULL);
    pthread_mutex_lock(&yuga_ch_tab_mtx);
    if (yuga_ch_n == yuga_ch_cap) {
        size_t nc = yuga_ch_cap ? yuga_ch_cap * 2 : 16;
        yuga_ch **next = (yuga_ch **)realloc(yuga_chs, nc * sizeof(yuga_ch *));
        if (!next) yuga_panic(__FILE__, __LINE__, "out of memory");
        yuga_chs = next;
        yuga_ch_cap = nc;
    }
    i = yuga_ch_n;
    yuga_chs[i] = c;
    yuga_ch_n++;
    pthread_mutex_unlock(&yuga_ch_tab_mtx);
    return (int64_t)i;
}

static inline yuga_ch *yuga_ch_get(int64_t id) {
    yuga_ch *c;
    /* Readers may run on worker threads while the UI thread grows the table
       (realloc). Lock the table for the fetch so the entry pointer is read
       race-free; the entry itself is stable once published. */
    pthread_mutex_lock(&yuga_ch_tab_mtx);
    if (id < 0 || (size_t)id >= yuga_ch_n)
        c = NULL;
    else
        c = yuga_chs[(size_t)id];
    pthread_mutex_unlock(&yuga_ch_tab_mtx);
    return c;
}

static inline void yuga_ch_send(int64_t id, const void *val, size_t sz) {
    yuga_ch *c = yuga_ch_get(id);
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

static inline int64_t yuga_ch_try_send(int64_t id, const void *val, size_t sz) {
    yuga_ch *c = yuga_ch_get(id);
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

static inline void yuga_ch_recv(int64_t id, void *out, size_t sz) {
    yuga_ch *c = yuga_ch_get(id);
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

static inline int64_t yuga_ch_pop(int64_t id, void *out, size_t sz) {
    yuga_ch *c = yuga_ch_get(id);
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

static inline int64_t yuga_ch_ready(int64_t id) {
    yuga_ch *c = yuga_ch_get(id);
    int64_t r;
    if (!c) return 0;
    pthread_mutex_lock(&c->mtx);
    r = c->count;
    pthread_mutex_unlock(&c->mtx);
    return r;
}

typedef struct {
    yuga_fn cb;
} yuga_thread_arg;

static void *yuga_thread_entry(void *p) {
    yuga_thread_arg *a = (yuga_thread_arg *)p;
    yuga_fn cb = a->cb;
    free(a);
    ((void (*)(void *))cb.fn)(cb.env);
    pthread_mutex_lock(&yuga_thread_mtx);
    yuga_thread_live--;
    pthread_mutex_unlock(&yuga_thread_mtx);
    return NULL;
}

/* Bodyless `thread.spawn(cb: fn())`. Detached: results come back through
   channel<T>, never through join. The fn env is leaked by design (fn values
   are never freed), so the worker outlives the spawn callsite safely. */
static inline void yuga_thread_spawn(yuga_fn cb) {
    pthread_t t;
    yuga_thread_arg *a;
    if (!cb.fn) return;
    a = (yuga_thread_arg *)yuga_new(sizeof(yuga_thread_arg), "thread.spawn", 0);
    a->cb = cb;
    if (pthread_create(&t, NULL, yuga_thread_entry, a) != 0) {
        free(a);
        yuga_panic(__FILE__, __LINE__, "thread.spawn: pthread_create failed");
    }
    pthread_detach(t);
    pthread_mutex_lock(&yuga_thread_mtx);
    yuga_thread_live++;
    pthread_mutex_unlock(&yuga_thread_mtx);
}

static inline int64_t yuga_thread_running(void) {
    int64_t n;
    pthread_mutex_lock(&yuga_thread_mtx);
    n = yuga_thread_live;
    pthread_mutex_unlock(&yuga_thread_mtx);
    return n;
}
#else /* __wasm32__: no threads; channel ops are inert so std:thread compiles */
static inline int64_t yuga_ch_alloc(int64_t cap, size_t esz) {
    (void)cap;
    (void)esz;
    return -1;
}
static inline void yuga_ch_send(int64_t id, const void *val, size_t sz) {
    (void)id;
    (void)val;
    (void)sz;
}
static inline int64_t yuga_ch_try_send(int64_t id, const void *val, size_t sz) {
    (void)id;
    (void)val;
    (void)sz;
    return 0;
}
static inline void yuga_ch_recv(int64_t id, void *out, size_t sz) {
    (void)id;
    (void)sz;
    if (out) memset(out, 0, sz);
}
static inline int64_t yuga_ch_pop(int64_t id, void *out, size_t sz) {
    (void)id;
    (void)sz;
    if (out) memset(out, 0, sz);
    return 0;
}
static inline int64_t yuga_ch_ready(int64_t id) {
    (void)id;
    return 0;
}
static inline void yuga_thread_spawn(yuga_fn cb) {
    (void)cb; /* wasm has no threads; spawn is a no-op */
}
static inline int64_t yuga_thread_running(void) { return 0; }
#endif

#endif /* YUGA_RT_H */
