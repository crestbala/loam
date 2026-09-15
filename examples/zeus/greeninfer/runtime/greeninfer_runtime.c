/* greeninfer_runtime.c — GreenInfer OS/hardware seam. Deliberately minimal:
 *
 *   gi_neon_dot_product()  the Apple Silicon NEON SIMD kernel (float32x4_t /
 *                          vmlaq_f32 / vaddvq_f32)
 *   gi_sys_mmap() / gi_sys_munmap()   POSIX zero-copy file mapping
 *
 * Plus the smallest possible trampolines to let Loam use them safely
 * (docs/boundary.md). Loam is memory-safe and has no raw pointers, so a map
 * is an opaque int handle into the registry below; every entry re-validates
 * its handle and row bounds against the 64-byte on-disk header before
 * touching the mapping. ALL policy lives in Loam (engine.loam): capacity
 * checks, count mirrors, the per-row search sweep, top-k, timing. This file
 * only maps files, stores one row of doubles (converting to f32 once), and
 * scores two mapped rows. The file layout (also engine-owned in spirit):
 *
 *   [64 B header: magic "GIM1", u32 dim, u64 cap, u64 count]
 *   [row 0..cap-1:  u32 id + dim f32]      stored vectors
 *   [row cap:       scratch]               query staging for the Loam sweep
 *
 * Linked automatically by loam for native targets when this file sits in
 * runtime/ next to the entry program (driver.c). Requires -I runtime dir.
 */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE /* clock_gettime on macOS (10.12+) */
#endif
#include "loam_rt.h"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdint.h>

#define GI_DIM 384
#define GI_MAX_MAPS 8
#define GI_MAGIC 0x47494d31u /* "GIM1" */
typedef struct { uint32_t magic, dim; uint64_t cap, count; uint8_t pad[40]; } GiHdr;

static struct { int used, fd; uint8_t *base; size_t bytes; } g_m[GI_MAX_MAPS];

/* ---- required core: POSIX zero-copy mapping ----------------------------- */

static void *gi_sys_mmap(int fd, size_t size) {
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); /* pages fault in; no read() */
}
static int gi_sys_munmap(void *ptr, size_t size) { return munmap(ptr, size); }

/* ---- required core: Apple Silicon NEON dot product ---------------------- */

/* dim f32 in float32x4_t quads. Four accumulators keep four FMA chains in
 * flight (vmlaq_f32 latency ~4 cycles; ILP hides it); vaddvq_f32 reduces. */
float gi_neon_dot_product(const float *a, const float *b, size_t dim) {
#if defined(__aarch64__)
    float32x4_t s0 = vdupq_n_f32(0), s1 = s0, s2 = s0, s3 = s0;
    size_t i = 0;
    for (; i + 16 <= dim; i += 16) {
        s0 = vmlaq_f32(s0, vld1q_f32(a + i),      vld1q_f32(b + i));
        s1 = vmlaq_f32(s1, vld1q_f32(a + i + 4),  vld1q_f32(b + i + 4));
        s2 = vmlaq_f32(s2, vld1q_f32(a + i + 8),  vld1q_f32(b + i + 8));
        s3 = vmlaq_f32(s3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    }
    for (; i < dim; i++) s0 = vmlaq_n_f32(s0, vld1q_dup_f32(a + i), b[i]);
    return vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3)));
#else
    double s = 0; /* non-ARM64 fallback keeps the seam portable */
    for (size_t i = 0; i < dim; i++) s += (double)a[i] * (double)b[i];
    return (float)s;
#endif
}

/* ---- tiny layout trampolines --------------------------------------------- */

static int64_t slot_new(int fd, uint8_t *base, size_t bytes) {
    for (int i = 0; i < GI_MAX_MAPS; i++)
        if (!g_m[i].used) {
            g_m[i].used = 1; g_m[i].fd = fd; g_m[i].base = base; g_m[i].bytes = bytes;
            return (int64_t)(i + 1); /* map handle = slot + 1; 0 = none */
        }
    return 0;
}

/* Handle -> base, header + bounds check in one step. Returns 0 or an error
 * code, so a bad handle or corrupt file can never reach the mapping. */
static int64_t slot_of(int64_t map, GiHdr *h, uint8_t **base) {
    int i = (int)(map - 1);
    if (i < 0 || i >= GI_MAX_MAPS || !g_m[i].used) return -1;
    memcpy(h, g_m[i].base, sizeof *h);
    if (h->magic != GI_MAGIC || h->dim <= 0 || h->dim > 4096 || h->cap > (1u << 22)) return -2;
    *base = g_m[i].base;
    return 0;
}

static int64_t path_copy(const loam_str p, char *out) { /* Loam strings are not NUL-terminated */
    if (p.len <= 0 || (size_t)p.len >= 1024) return -1;
    memcpy(out, p.ptr, (size_t)p.len); out[p.len] = 0;
    return 0;
}

/* Error sentinel for gi_row_dot. Must be finite in f32 (the old -1e300
 * became -inf) and below any real dot product; engine.loam rejects rows
 * scoring under GI_DOT_FLOOR (-2e38). */
#define GI_DOT_ERR (-3.0e38f)

static size_t row_size(const GiHdr *h) { return 4 + (size_t)h->dim * 4; }
static uint8_t *row_ptr(const GiHdr *h, uint8_t *base, int64_t row) {
    return base + sizeof(GiHdr) + (size_t)row * row_size(h) + 4; /* f32 vector after the u32 id */
}

int32_t loam_engine_gi_map_create(loam_str path, int32_t cap) {
    char pb[1024];
    if (path_copy(path, pb) < 0 || cap <= 0 || cap > (1 << 22)) return 0;
    GiHdr h = { GI_MAGIC, GI_DIM, (uint64_t)cap, 0, {0} };
    size_t bytes = sizeof h + (size_t)(cap + 1) * row_size(&h); /* +1 scratch row */
    int fd = open(pb, O_RDWR | O_CREAT, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)bytes) != 0) { if (fd >= 0) close(fd); return 0; }
    uint8_t *base = (uint8_t *)gi_sys_mmap(fd, bytes); /* map before any write */
    if (base == MAP_FAILED) { close(fd); return 0; }
    memcpy(base, &h, sizeof h);
    return slot_new(fd, base, bytes);
}

int32_t loam_engine_gi_map_open(loam_str path) {
    char pb[1024]; struct stat st; GiHdr h;
    if (path_copy(path, pb) < 0) return 0;
    int fd = open(pb, O_RDWR);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof h) { if (fd >= 0) close(fd); return 0; }
    uint8_t *base = (uint8_t *)gi_sys_mmap(fd, (size_t)st.st_size);
    if (base == MAP_FAILED) { close(fd); return 0; }
    memcpy(&h, base, sizeof h);
    if (h.magic != GI_MAGIC) { gi_sys_munmap(base, (size_t)st.st_size); close(fd); return 0; }
    return slot_new(fd, base, (size_t)st.st_size);
}

int32_t loam_engine_gi_map_close(int32_t map) {
    int i = (int)(map - 1);
    if (i < 0 || i >= GI_MAX_MAPS || !g_m[i].used) return -1;
    msync(g_m[i].base, g_m[i].bytes, MS_SYNC); /* flush MAP_SHARED (count header too) */
    gi_sys_munmap(g_m[i].base, g_m[i].bytes);
    close(g_m[i].fd);
    g_m[i].used = 0;
    return 0;
}

int32_t loam_engine_gi_map_unlink(loam_str path) {
    char pb[1024];
    if (path_copy(path, pb) < 0) return -1;
    return remove(pb) == 0 ? 0 : -1;
}

/* Read the live header fields. The count is the append log: row_write keeps
 * it current in the mapped header, so a crashed process still reopens at the
 * last fully written row. */
int32_t loam_engine_gi_map_count(int32_t map) { GiHdr h; uint8_t *b; return slot_of(map, &h, &b) == 0 ? (int32_t)h.count : -1; }
int32_t loam_engine_gi_map_cap(int32_t map)   { GiHdr h; uint8_t *b; return slot_of(map, &h, &b) == 0 ? (int32_t)h.cap : -1; }
int32_t loam_engine_gi_map_bytes(int32_t map) { int i = (int)(map - 1); return (i >= 0 && i < GI_MAX_MAPS && g_m[i].used) ? (int32_t)g_m[i].bytes : -1; }

/* Store `vec` (exactly dim doubles) into mapped `row` under `rid`. Rows
 * 0..cap-1 are vectors; row == cap is the scratch row Loam stages queries
 * into. Append bookkeeping only: if `row` extends the written prefix, the
 * header count follows. */
int32_t loam_engine_gi_row_write(int32_t map, int32_t row, int32_t rid, loam_vec v) {
    GiHdr h; uint8_t *base;
    if (slot_of(map, &h, &base) != 0 || v.len != h.dim) return -1;
    if (row < 0 || row > (int64_t)h.cap) return -1; /* cap = last legal row (scratch) */
    const float *src = (const float *)v.ptr;        /* Loam float = f32 */
    uint8_t *rp = row_ptr(&h, base, row) - 4;
    *(uint32_t *)rp = (uint32_t)(rid & 0xffffffff);
    float *dst = (float *)(rp + 4);
    for (int64_t d = 0; d < h.dim; d++) dst[d] = src[d]; /* already f32 — straight copy */
    if (row < (int64_t)h.cap && row >= (int64_t)h.count) {
        h.count = (uint64_t)row + 1;
        memcpy(base, &h, sizeof h); /* keep the on-disk append log live */
    }
    return 0;
}

/* The u32 id stored in `row` (the 4 bytes before its f32 vector). */
int32_t loam_engine_gi_row_id(int32_t map, int32_t row) {
    GiHdr h; uint8_t *base;
    if (slot_of(map, &h, &base) != 0) return -1;
    if (row < 0 || row > (int64_t)h.cap) return -1;
    return (int32_t)*(const uint32_t *)(row_ptr(&h, base, row) - 4);
}

/* NEON dot of two mapped rows (both f32; no conversion, no copy). Row `a`
 * and `b` may be the same. The Loam search sweep calls this once per row. */
float loam_engine_gi_row_dot(int32_t map, int32_t row_a, int32_t row_b) {
    GiHdr h; uint8_t *base;
    if (slot_of(map, &h, &base) != 0) return GI_DOT_ERR;
    if (row_a < 0 || row_a > (int64_t)h.cap || row_b < 0 || row_b > (int64_t)h.cap) return GI_DOT_ERR;
    return gi_neon_dot_product((const float *)row_ptr(&h, base, row_a),
                               (const float *)row_ptr(&h, base, row_b), (size_t)h.dim);
}

/* Monotonic microseconds — the search budget is measured on this clock. */
int32_t loam_engine_gi_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int32_t)((int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

/* ---- file data-movement trampolines (watcher.loam) -----------------------
 * Reading, listing, and mtimes are OS I/O; *what* to read, recurse into, or
 * exclude is Loam policy (walk, filters, manifest diff all in watcher.loam).
 * Fresh allocations each call: Loam strings are never freed, and the caller
 * (the walk) holds several of these at once, so no static buffer. */

static loam_str loam_empty_str(void) { return (loam_str){ .ptr = "", .len = 0 }; }

/* Whole file as a Loam string (NUL-terminated, len = bytes). Empty on error
 * or when the file is larger than 256 MB. */
loam_str loam_watcher_gi_read_file(loam_str path) {
    char pb[1024]; struct stat st;
    if (path_copy(path, pb) < 0) return loam_empty_str();
    int fd = open(pb, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); return loam_empty_str(); }
    if (st.st_size < 0 || st.st_size > (1 << 28)) { close(fd); return loam_empty_str(); }
    size_t n = (size_t)st.st_size;
    char *buf = (char *)malloc(n + 1);
    if (!buf) { close(fd); return loam_empty_str(); }
    size_t got = 0;
    while (got < n) { /* regular files read fully; loop for short reads */
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;
    return (loam_str){ .ptr = buf, .len = (int64_t)got };
}

/* One directory level as "name\n" lines; directories carry a trailing '/'
 * so Loam can recurse without stat'ing. "." and ".." are omitted. */
loam_str loam_watcher_gi_dir_list(loam_str path) {
    char pb[1024];
    if (path_copy(path, pb) < 0) return loam_empty_str();
    DIR *d = opendir(pb);
    if (!d) return loam_empty_str();
    size_t cap = 1024, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { closedir(d); return loam_empty_str(); }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        size_t nl = strlen(de->d_name);
        int isdir = de->d_type == DT_DIR;
        if (len + nl + 2 > cap) { /* grow */
            size_t ncap = cap * 2;
            while (len + nl + 2 > ncap) ncap *= 2;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) { free(buf); closedir(d); return loam_empty_str(); }
            buf = nb; cap = ncap;
        }
        memcpy(buf + len, de->d_name, nl);
        len += nl;
        if (isdir) buf[len++] = '/';
        buf[len++] = '\n';
    }
    closedir(d);
    buf[len] = 0;
    return (loam_str){ .ptr = buf, .len = (int64_t)len };
}

/* Modification time in whole seconds since the epoch; -1 on error. */
int32_t loam_watcher_gi_file_mtime(loam_str path) {
    char pb[1024]; struct stat st;
    if (path_copy(path, pb) < 0) return -1;
    if (stat(pb, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}
