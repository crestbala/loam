/* Freestanding libc for Zeus WASM (Canvas2D host, no WASI). */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/uio.h>

__attribute__((import_module("zeus"), import_name("write")))
void zeus_js_write(const char *p, int32_t n);

__attribute__((import_module("zeus"), import_name("panic")))
void zeus_js_panic(const char *p, int32_t n);

int errno;
static FILE file_out = {1};
static FILE file_err = {2};
FILE *stdout = &file_out;
FILE *stderr = &file_err;

#define HEAP_SIZE (64 * 1024 * 1024)
#define HDR 8u
#define FTR 8u
#define FREE_BIT ((size_t)1)

_Alignas(8) static unsigned char heap[HEAP_SIZE];
static size_t heap_off = HDR;
/* Free blocks live on a doubly linked list (payload[0] = next, payload[1] =
   prev). Every block also carries a footer duplicating its header bits, so
   free() can coalesce with the previous block without walking the heap.
   malloc only ever walks FREE blocks — never the allocated heap — so total
   cost stays near-linear instead of quadratic in the number of live
   allocations (the old bump scan made a gallery-size build take ~40 s). */
static size_t free_head; /* offset of first free block; 0 = none */

static size_t align8(size_t n) { return (n + 7u) & ~7u; }
static size_t payload_sz(size_t bits) { return bits & ~FREE_BIT; }
static int is_free(size_t bits) { return (int)(bits & FREE_BIT); }

static size_t *hdr_at(size_t off) { return (size_t *)(heap + off); }
static size_t *ftr_at(size_t off) { return (size_t *)(heap + off + HDR + payload_sz(*hdr_at(off))); }
static void *payload(size_t *h) { return (void *)((unsigned char *)h + HDR); }
static size_t *hdr_of(void *p) { return (size_t *)((unsigned char *)p - HDR); }

static size_t fl_next(size_t o) { return ((size_t *)payload(hdr_at(o)))[0]; }
static size_t fl_prev(size_t o) { return ((size_t *)payload(hdr_at(o)))[1]; }
static void fl_set_next(size_t o, size_t v) { ((size_t *)payload(hdr_at(o)))[0] = v; }
static void fl_set_prev(size_t o, size_t v) { ((size_t *)payload(hdr_at(o)))[1] = v; }

static void fl_unlink(size_t o) {
    size_t p = fl_prev(o), nx = fl_next(o);
    if (p) fl_set_next(p, nx);
    else free_head = nx;
    if (nx) fl_set_prev(nx, p);
}

static void fl_insert_head(size_t o) {
    fl_set_next(o, free_head);
    fl_set_prev(o, 0);
    if (free_head) fl_set_prev(free_head, o);
    free_head = o;
}

/* Mark a free block (header + footer) with size `pay` and list it. */
static void free_make(size_t off, size_t pay) {
    *hdr_at(off) = pay | FREE_BIT;
    *ftr_at(off) = pay | FREE_BIT;
    fl_insert_head(off);
}

void *malloc(size_t n) {
    size_t off, bits, pay, next, leftover;
    size_t *h;
    if (!n) n = 1;
    n = align8(n);
    if (n < 16) n = 16; /* free blocks store two list links in their payload */

    off = free_head;
    while (off) {
        if (off < HDR || off >= heap_off) { /* corrupt list: drop it */
            free_head = 0;
            break;
        }
        h = hdr_at(off);
        bits = *h;
        pay = payload_sz(bits);
        if (pay >= n) {
            fl_unlink(off);
            leftover = pay - n;
            if (leftover >= HDR + 16 + FTR) { /* a new free block fits */
                *h = n;
                *ftr_at(off) = n;
                next = off + HDR + n + FTR;
                free_make(next, leftover - HDR - FTR);
            } else {
                *h = pay;
                *ftr_at(off) = pay;
            }
            return payload(h);
        }
        off = fl_next(off);
    }

    if (heap_off + HDR + n + FTR > HEAP_SIZE) return NULL;
    h = hdr_at(heap_off);
    *h = n;
    *(size_t *)(heap + heap_off + HDR + n) = n;
    heap_off += HDR + n + FTR;
    return payload(h);
}

void *realloc(void *p, size_t n) {
    void *q;
    size_t old;
    if (!p) return malloc(n);
    old = payload_sz(*hdr_of(p));
    if (n <= old) return p;
    q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old);
    free(p);
    return q;
}

void *calloc(size_t n, size_t sz) {
    size_t t = n * sz;
    void *q = malloc(t);
    if (q) memset(q, 0, t);
    return q;
}

void free(void *p) {
    size_t *h, *f;
    size_t off, bits, pay, prev_hdr;
    if (!p) return;
    h = hdr_of(p);
    off = (size_t)((unsigned char *)h - heap);
    bits = *h;
    pay = payload_sz(bits);
    /* Coalesce with the following block if it is free. */
    if (off + HDR + pay + FTR < heap_off) {
        size_t *nx = hdr_at(off + HDR + pay + FTR);
        if (is_free(*nx)) {
            fl_unlink(off + HDR + pay + FTR);
            pay += HDR + payload_sz(*nx) + FTR;
        }
    }
    /* Coalesce with the previous block via its footer. */
    if (off >= HDR + FTR) {
        f = (size_t *)(heap + off - FTR);
        if (is_free(*f)) {
            prev_hdr = off - FTR - HDR - payload_sz(*f);
            fl_unlink(prev_hdr);
            pay += HDR + payload_sz(*f) + FTR;
            off = prev_hdr;
        }
    }
    free_make(off, pay);
}

/* Heap bytes currently in use (live), not the watermark: heap_off minus the
   bytes sitting on the free list. The wasm side of the RAM chip. */
int32_t zeus_heap_used(void) {
    size_t o, free_bytes = 0;
    o = free_head;
    while (o) {
        free_bytes += HDR + payload_sz(*hdr_at(o)) + FTR;
        o = fl_next(o);
    }
    return (int32_t)(heap_off - free_bytes);
}

void abort(void) {
    zeus_js_panic("abort", 5);
    /* The host import normally throws; if it returns, trap rather than spin. */
    __builtin_trap();
}

void exit(int c) {
    (void)c;
    abort();
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

int atoi(const char *s) {
    int v = 0, sign = 1;
    if (!s) return 0;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    size_t i;
    for (i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    size_t i;
    if (dd < ss) {
        for (i = 0; i < n; i++) dd[i] = ss[i];
    } else {
        for (i = n; i > 0; i--) dd[i - 1] = ss[i - 1];
    }
    return d;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *dd = (unsigned char *)d;
    size_t i;
    for (i = 0; i < n; i++) dd[i] = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *aa = (const unsigned char *)a;
    const unsigned char *bb = (const unsigned char *)b;
    size_t i;
    for (i = 0; i < n; i++) {
        if (aa[i] != bb[i]) return (int)aa[i] - (int)bb[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    size_t i;
    if (!a) a = "";
    if (!b) b = "";
    for (i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

char *strcpy(char *d, const char *s) {
    char *o = d;
    while ((*d++ = *s++)) {
    }
    return o;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char *strchr(const char *s, int c) {
    unsigned char ch = (unsigned char)c;
    if (!s) return NULL;
    for (; *s; s++)
        if ((unsigned char)*s == ch) return (char *)s;
    if (ch == 0) return (char *)s;
    return NULL;
}

char *strrchr(const char *s, int c) {
    unsigned char ch = (unsigned char)c;
    const char *last = NULL;
    if (!s) return NULL;
    for (; *s; s++)
        if ((unsigned char)*s == ch) last = s;
    if (ch == 0) return (char *)s;
    return (char *)last;
}

ssize_t write(int fd, const void *p, size_t n) {
    (void)fd;
    if (p && n) zeus_js_write((const char *)p, (int32_t)n);
    return (ssize_t)n;
}

ssize_t writev(int fd, const struct iovec *iov, int n) {
    int i;
    ssize_t t = 0;
    for (i = 0; i < n; i++) {
        write(fd, iov[i].iov_base, iov[i].iov_len);
        t += (ssize_t)iov[i].iov_len;
    }
    return t;
}

static void js_print(const char *s) {
    if (s) zeus_js_write(s, (int32_t)strlen(s));
}

int fflush(FILE *f) {
    (void)f;
    return 0;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    char buf[256];
    (void)f;
    if (fmt && fmt[0] == '%' && fmt[1] == 's' && fmt[2] == ':' && fmt[3] == '%') {
        const char *file;
        int line;
        const char *msg;
        va_start(ap, fmt);
        file = va_arg(ap, const char *);
        line = va_arg(ap, int);
        msg = va_arg(ap, const char *);
        va_end(ap);
        snprintf(buf, sizeof buf, "%s:%d: panic: %s\n", file ? file : "?", line,
                 msg ? msg : "");
        js_print(buf);
        return 0;
    }
    va_start(ap, fmt);
    (void)ap;
    va_end(ap);
    js_print(fmt);
    return 0;
}

int printf(const char *fmt, ...) {
    js_print(fmt);
    return 0;
}


/* --- exact float <-> decimal (BEGIN wasm-float) --------------------------
   The wasm build has no libc of its own, so `%g` and `strtod` live here, and
   they give the same answers as a native libc: digits are computed from the
   double's exact value with a small base-1e9 bignum, then rounded half-even,
   and parsing picks the nearest double by comparing against the exact
   midpoints. Slow next to Ryu, but exact, short, and only on this path. */

#define WF_LIMBS 320 /* 1e9^320: past 5^1100 * 10^800, the largest product */

typedef struct {
    uint32_t d[WF_LIMBS]; /* little-endian, base 1e9 */
    int n;
} wf_big;

static void wf_set_u64(wf_big *b, uint64_t v) {
    b->n = 0;
    while (v) {
        b->d[b->n++] = (uint32_t)(v % 1000000000u);
        v /= 1000000000u;
    }
}

static void wf_mul_small(wf_big *b, uint32_t f) {
    uint64_t carry = 0;
    for (int i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->d[i] * f + carry;
        b->d[i] = (uint32_t)(t % 1000000000u);
        carry = t / 1000000000u;
    }
    while (carry && b->n < WF_LIMBS) {
        b->d[b->n++] = (uint32_t)(carry % 1000000000u);
        carry /= 1000000000u;
    }
}

static void wf_mul_pow(wf_big *b, uint32_t base, int k) {
    /* 5^13 and 2^29 are the largest powers below 2^31. */
    uint32_t step = base == 5 ? 1220703125u : 536870912u;
    int chunk = base == 5 ? 13 : 29;
    while (k >= chunk) {
        wf_mul_small(b, step);
        k -= chunk;
    }
    uint32_t r = 1;
    while (k-- > 0) r *= base;
    if (r != 1) wf_mul_small(b, r);
}

static void wf_add_small(wf_big *b, uint32_t v) {
    uint64_t carry = v;
    for (int i = 0; carry && i < WF_LIMBS; i++) {
        if (i == b->n) b->d[b->n++] = 0;
        uint64_t t = (uint64_t)b->d[i] + carry;
        b->d[i] = (uint32_t)(t % 1000000000u);
        carry = t / 1000000000u;
    }
}

static int wf_cmp(const wf_big *a, const wf_big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

/* Decimal digits of b into out (no leading zeros); returns the count. */
static int wf_digits(const wf_big *b, char *out) {
    int o = 0;
    if (b->n == 0) {
        out[0] = '0';
        return 1;
    }
    char tmp[10];
    uint32_t top = b->d[b->n - 1];
    int t = 0;
    do {
        tmp[t++] = (char)('0' + top % 10);
        top /= 10;
    } while (top);
    while (t) out[o++] = tmp[--t];
    for (int i = b->n - 2; i >= 0; i--) {
        uint32_t v = b->d[i];
        for (int k = 8; k >= 0; k--) {
            out[o + k] = (char)('0' + v % 10);
            v /= 10;
        }
        o += 9;
    }
    return o;
}

static uint64_t wf_bits(double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    return u;
}

static double wf_from_bits(uint64_t u) {
    double v;
    memcpy(&v, &u, 8);
    return v;
}

/* `%.<prec>g` of v into out (at least 40 bytes); returns the length. */
static int wf_fmt_g(double v, int prec, char *out) {
    static char dig[1200];
    wf_big n;
    uint64_t u = wf_bits(v);
    int o = 0, ex = (int)((u >> 52) & 0x7ff);
    uint64_t frac = u & ((1ull << 52) - 1);
    if (prec <= 0) prec = 1;
    if (prec > 17) prec = 17;
    if (u >> 63) out[o++] = '-';
    if (ex == 0x7ff) {
        memcpy(out + o, frac ? "nan" : "inf", 3);
        return o + 3;
    }
    if (ex == 0 && frac == 0) {
        out[o++] = '0';
        return o;
    }
    uint64_t m = ex ? frac | (1ull << 52) : frac;
    int e = ex ? ex - 1075 : -1074, k = 0;
    wf_set_u64(&n, m);
    if (e >= 0) wf_mul_pow(&n, 2, e);
    else {
        wf_mul_pow(&n, 5, -e);
        k = -e; /* value = n / 10^k */
    }
    int len = wf_digits(&n, dig);
    int x = len - 1 - k; /* decimal exponent of the first digit */
    if (len > prec) {
        int up = 0, d = dig[prec] - '0';
        if (d > 5) up = 1;
        else if (d == 5) {
            int rest = 0;
            for (int i = prec + 1; i < len; i++)
                if (dig[i] != '0') rest = 1;
            up = rest || ((dig[prec - 1] - '0') & 1);
        }
        len = prec;
        if (up) {
            int i = prec - 1;
            while (i >= 0 && dig[i] == '9') dig[i--] = '0';
            if (i >= 0) dig[i]++;
            else {
                dig[0] = '1';
                for (int j = 1; j < prec; j++) dig[j] = '0';
                x++;
            }
        }
    }
    while (len > 1 && dig[len - 1] == '0') len--;
    if (x < -4 || x >= prec) {
        out[o++] = dig[0];
        if (len > 1) {
            out[o++] = '.';
            for (int i = 1; i < len; i++) out[o++] = dig[i];
        }
        out[o++] = 'e';
        out[o++] = x < 0 ? '-' : '+';
        int ax = x < 0 ? -x : x;
        if (ax >= 100) out[o++] = (char)('0' + ax / 100);
        out[o++] = (char)('0' + ax / 10 % 10);
        out[o++] = (char)('0' + ax % 10);
    } else if (x >= 0) {
        for (int i = 0; i <= x; i++) out[o++] = i < len ? dig[i] : '0';
        if (len > x + 1) {
            out[o++] = '.';
            for (int i = x + 1; i < len; i++) out[o++] = dig[i];
        }
    } else {
        out[o++] = '0';
        out[o++] = '.';
        for (int i = 0; i < -x - 1; i++) out[o++] = '0';
        for (int i = 0; i < len; i++) out[o++] = dig[i];
    }
    return o;
}

/* Compare D * 10^E (sticky: plus a little) with c * 2^f. */
static int wf_cmp_dec_bin(const char *dig, int nd, int E, int sticky, uint64_t c, int f) {
    static wf_big l, r;
    l.n = 0;
    for (int i = 0; i < nd; i += 9) {
        int take = nd - i < 9 ? nd - i : 9;
        uint32_t part = 0, scale = 1;
        for (int k = 0; k < take; k++) {
            part = part * 10 + (uint32_t)(dig[i + k] - '0');
            scale *= 10;
        }
        wf_mul_small(&l, scale);
        wf_add_small(&l, part);
    }
    wf_set_u64(&r, c);
    int a2 = E - f;
    if (a2 >= 0) wf_mul_pow(&l, 2, a2);
    else wf_mul_pow(&r, 2, -a2);
    if (E >= 0) wf_mul_pow(&l, 5, E);
    else wf_mul_pow(&r, 5, -E);
    int cmp = wf_cmp(&l, &r);
    return cmp == 0 && sticky ? 1 : cmp;
}

static int wf_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int wf_word(const char *p, const char *w) {
    while (*w)
        if (wf_lower(*p++) != *w++) return 0;
    return 1;
}

double strtod(const char *s, char **end) {
    static char dig[800];
    const char *p = s;
    int neg = 0, nd = 0, sticky = 0, any = 0;
    long E = 0;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (wf_word(p, "inf")) {
        p += wf_word(p, "infinity") ? 8 : 3;
        if (end) *end = (char *)p;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if (wf_word(p, "nan")) {
        if (end) *end = (char *)(p + 3);
        return __builtin_nan("");
    }
    for (; *p >= '0' && *p <= '9'; p++) {
        any = 1;
        if (nd == 0 && *p == '0') continue;
        if (nd < (int)sizeof dig) dig[nd++] = *p;
        else {
            E++;
            if (*p != '0') sticky = 1;
        }
    }
    if (*p == '.') {
        p++;
        for (; *p >= '0' && *p <= '9'; p++) {
            any = 1;
            if (nd == 0 && *p == '0') {
                E--;
                continue;
            }
            if (nd < (int)sizeof dig) {
                dig[nd++] = *p;
                E--;
            } else if (*p != '0') sticky = 1;
        }
    }
    if (!any) {
        if (end) *end = (char *)s;
        return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0;
        long x = 0;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        if (*q >= '0' && *q <= '9') {
            for (; *q >= '0' && *q <= '9'; q++)
                if (x < 100000) x = x * 10 + (*q - '0');
            E += eneg ? -x : x;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    while (nd > 0 && dig[nd - 1] == '0') {
        nd--;
        E++;
    }
    double zero = neg ? -0.0 : 0.0;
    if (nd == 0) return zero;
    if (nd + E > 310) return neg ? -__builtin_inf() : __builtin_inf();
    if (nd + E < -326) return zero;
    /* A first guess from up to 19 digits, then walk to the nearest double. */
    uint64_t lead = 0;
    int nl = nd < 19 ? nd : 19;
    for (int i = 0; i < nl; i++) lead = lead * 10 + (uint64_t)(dig[i] - '0');
    double a = (double)lead;
    long sc = E + (nd - nl);
    while (sc > 0) {
        int st = sc > 22 ? 22 : (int)sc;
        double pw = 1.0;
        for (int i = 0; i < st; i++) pw *= 10.0;
        a *= pw;
        sc -= st;
    }
    while (sc < 0) {
        int st = -sc > 22 ? 22 : (int)-sc;
        double pw = 1.0;
        for (int i = 0; i < st; i++) pw *= 10.0;
        a /= pw;
        sc += st;
    }
    uint64_t u = wf_bits(a) & ~(1ull << 63);
    if (u >= 0x7ff0000000000000ull) u = 0x7fefffffffffffffull;
    for (int iter = 0; iter < 4096; iter++) {
        int ex = (int)(u >> 52);
        uint64_t m = (u & ((1ull << 52) - 1)) | (ex ? 1ull << 52 : 0);
        int e = ex ? ex - 1075 : -1074;
        /* Upper midpoint (2m+1) * 2^(e-1). */
        int cu = wf_cmp_dec_bin(dig, nd, (int)E, sticky, 2 * m + 1, e - 1);
        if (cu > 0 || (cu == 0 && (m & 1))) {
            if (u == 0x7fefffffffffffffull) return neg ? -__builtin_inf() : __builtin_inf();
            u++;
            continue;
        }
        if (u == 0) break;
        /* Lower midpoint: half the gap below, which is narrower at a power of two. */
        int cl = (m == (1ull << 52) && ex > 1)
                     ? wf_cmp_dec_bin(dig, nd, (int)E, sticky, 4 * m - 1, e - 2)
                     : wf_cmp_dec_bin(dig, nd, (int)E, sticky, 2 * m - 1, e - 1);
        if (cl < 0 || (cl == 0 && (m & 1))) {
            u--;
            continue;
        }
        break;
    }
    double r = wf_from_bits(u);
    return neg ? -r : r;
}
/* --- (END wasm-float) ---------------------------------------------------- */

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    size_t o = 0;
    const char *p;
    if (!buf || !n) return 0;
    va_start(ap, fmt);
    for (p = fmt; p && *p && o + 1 < n; p++) {
        if (*p != '%') {
            buf[o++] = *p;
            continue;
        }
        p++;
        if (*p == '%') {
            buf[o++] = '%';
            continue;
        }
        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "";
            while (*s && o + 1 < n) buf[o++] = *s++;
        } else if (*p == 'd' || *p == 'i') {
            int v = va_arg(ap, int);
            char tmp[16];
            int ti = 0, neg = 0;
            unsigned u;
            if (v < 0) {
                neg = 1;
                u = (unsigned)(-(v + 1)) + 1;
            } else
                u = (unsigned)v;
            if (!u) tmp[ti++] = '0';
            while (u && ti < 15) {
                tmp[ti++] = (char)('0' + (u % 10));
                u /= 10;
            }
            if (neg && o + 1 < n) buf[o++] = '-';
            while (ti && o + 1 < n) buf[o++] = tmp[--ti];
        } else if (*p == 'l') {
            p++;
            if (*p == 'l') p++;
            if (*p == 'd' || *p == 'i' || *p == 'u') {
                long long v = va_arg(ap, long long);
                char tmp[24];
                int ti = 0, neg = 0;
                unsigned long long u;
                if (v < 0) {
                    neg = 1;
                    u = (unsigned long long)(-(v + 1)) + 1;
                } else
                    u = (unsigned long long)v;
                if (!u) tmp[ti++] = '0';
                while (u && ti < 23) {
                    tmp[ti++] = (char)('0' + (u % 10));
                    u /= 10;
                }
                if (neg && o + 1 < n) buf[o++] = '-';
                while (ti && o + 1 < n) buf[o++] = tmp[--ti];
            }
        } else if (*p == 'g' || (*p == '.' && (p[1] == '*' || (p[1] >= '0' && p[1] <= '9')))) {
            /* `%g`, `%.Ng`, `%.*g` (exact, see wasm-float above); `%.*s`. */
            int prec = 6;
            if (*p == '.') {
                p++;
                if (*p == '*') {
                    prec = va_arg(ap, int);
                    p++;
                } else {
                    prec = 0;
                    while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
                }
            }
            if (*p == 's') {
                const char *s = va_arg(ap, const char *);
                int i;
                if (!s) s = "";
                for (i = 0; i < prec && s[i] && o + 1 < n; i++) buf[o++] = s[i];
            } else if (*p == 'g') {
                char tmp[48];
                int tl = wf_fmt_g(va_arg(ap, double), prec, tmp), i;
                for (i = 0; i < tl && o + 1 < n; i++) buf[o++] = tmp[i];
            }
        }
    }
    va_end(ap);
    buf[o] = 0;
    return (int)o;
}

/* compiler-rt i128 helpers. wasm32 clang emits these as `env.*` imports
   unless we define them. Do not multiply `__int128` here — that recurses. */

static void mulu64(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32;
    uint64_t b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p0 = a0 * b0;
    uint64_t p1 = a0 * b1;
    uint64_t p2 = a1 * b0;
    uint64_t p3 = a1 * b1;
    uint64_t mid = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
    *lo = (p0 & 0xffffffffu) | (mid << 32);
    *hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

void __multi3(uint64_t out[2], uint64_t a_lo, uint64_t a_hi, uint64_t b_lo, uint64_t b_hi) {
    uint64_t lo, hi, t, t_hi;
    mulu64(a_lo, b_lo, &lo, &hi);
    mulu64(a_lo, b_hi, &t, &t_hi);
    hi += t;
    mulu64(a_hi, b_lo, &t, &t_hi);
    hi += t;
    out[0] = lo;
    out[1] = hi;
}
