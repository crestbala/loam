/* zeus_plat.c — Cocoa/headless seam for the Loam zeus library.
 *
 * packages/zeus/std/zeus.loam owns the node arena, layout, paint, and hit-test. This file
 * is the other side of that seam: window title/size, text measure, present,
 * and the zeus_* entry points the Cocoa/iOS/web hosts already call. Keyboard chords live
 * in zeus_key.c; focus chain and Tab walk the Loam node tree.
 */
#include "zeus_rt.h"
#include "zeus_key.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

extern loam_vec loam_arena_sigs;
/* Rebuild-scope ownership (arena.loam): while `scope_node` is non-zero, a
   signal allocation is recorded against it so teardown can recycle the id. */
extern int32_t loam_arena_scope_node;
extern loam_vec loam_arena_rec_sid;
extern loam_vec loam_arena_rec_owner;
void loam_arena_ensure(void);
void loam_arena_store_sig(int32_t id, int32_t value);
/* `fn note_read(sid: int)` / `fn notify(sid: int)` in zeuscore/track.loam, and
   `fn note_write(sid: int)` in zeuscore/arena.loam — the channel generated code
   calls after it binds a non-`int` value, and `int` is i32 since the Phase 10
   width flip. Declaring these i64 is a silent truncation on native but a hard
   signature mismatch under wasm-ld, which then links a trapping stub in place of
   the call. */
void loam_track_note_read(int32_t sid);
void loam_track_notify(int32_t sid);
void loam_arena_note_write(int32_t sid);

typedef struct {
    void *p;
    size_t n;
    int64_t gen;
    /* 1 when the payload is an owned `loam_str` holding a reference the cell
       must release when the cell dies or is reused for another type. */
    int owns_str;
} ZeusCell;

static ZeusCell *g_cells;
static size_t g_ncells;
static size_t g_ccells;

static void cell_grow(int64_t id) {
    size_t need;
    if (id < 0) return;
    need = (size_t)id + 1;
    if (need <= g_ncells) return;
    if (need > g_ccells) {
        size_t nc = g_ccells ? g_ccells : 16;
        ZeusCell *next;
        while (nc < need) nc *= 2;
        next = (ZeusCell *)realloc(g_cells, nc * sizeof(ZeusCell));
        if (!next) {
            fprintf(stderr, "zeus: out of memory\n");
            abort();
        }
        memset(next + g_ccells, 0, (nc - g_ccells) * sizeof(ZeusCell));
        g_cells = next;
        g_ccells = nc;
    }
    g_ncells = need;
}

void loam_zeus_sig_bind(int64_t id, const void *src, int64_t n) {
    ZeusCell *c;
    size_t nn;
    if (id < 0 || n < 0) return;
    cell_grow(id);
    c = &g_cells[id];
    nn = (size_t)n;
    if (c->n != nn) {
        free(c->p);
        c->p = nn ? malloc(nn) : NULL;
        if (nn && !c->p) {
            fprintf(stderr, "zeus: out of memory\n");
            abort();
        }
        c->n = nn;
    }
    if (nn && src && c->p) memcpy(c->p, src, nn);
    /* A plain bind takes over the cell for a non-string payload; the caller
       has already released any string it held (see the store path). */
    c->owns_str = 0;
    c->gen++;
}

/* Bind an owned string: the cell keeps a reference and releases it when the
   cell is freed or rebound to another type. The caller has already taken the
   reference (`loam_str_retain`) and released the one the cell held. */
void loam_zeus_sig_bind_str(int64_t id, const void *src, int64_t n) {
    loam_zeus_sig_bind(id, src, n);
    if (id >= 0 && (size_t)id < g_ncells) g_cells[id].owns_str = 1;
}

void loam_zeus_sig_load(int64_t id, void *dst, int64_t n) {
    size_t nn, k;
    if (!dst || n <= 0) return;
    nn = (size_t)n;
    memset(dst, 0, nn);
    if (id < 0 || (size_t)id >= g_ncells) return;
    if (!g_cells[id].p) return;
    k = g_cells[id].n < nn ? g_cells[id].n : nn;
    memcpy(dst, g_cells[id].p, k);
}

int64_t loam_zeus_sig_changed(int64_t id, const void *src, int64_t n) {
    if (id < 0 || !src || n <= 0) return 1;
    if ((size_t)id >= g_ncells || !g_cells[id].p || g_cells[id].n != (size_t)n)
        return 1;
    return memcmp(g_cells[id].p, src, (size_t)n) != 0;
}

int64_t loam_zeus_sig_gen(int64_t id) {
    if (id < 0 || (size_t)id >= g_ncells) return 0;
    return g_cells[id].gen;
}

static int64_t sig_slot_zero(void);

/* Captured-`let mut` (state) signals: `signal(0)`-style decls inside a
   rebuild scope must be recycled and freed like every other signal, or a
   chart refit leaks a slot per captured mutable (zone strips, bar heights,
   ...). sig_slot_zero recycles freed ids and records the scope owner, so
   arena teardown (release_sigs) frees them with the rest of the subtree. */
Signal loam_zeus_signal(int64_t value) {
    Signal s;
    int32_t v32 = (int32_t)value;
    loam_arena_ensure();
    s.id = sig_slot_zero();
    ((int32_t *)loam_arena_sigs.ptr)[s.id] = v32;
    loam_zeus_sig_bind(s.id, &v32, (int64_t)sizeof(v32));
    return s;
}

int64_t loam_zeus_get(Signal sig) {
    int32_t v = 0;
    loam_arena_ensure();
    loam_track_note_read(sig.id);
    loam_zeus_sig_load(sig.id, &v, (int64_t)sizeof(v));
    return (int64_t)v;
}

void loam_zeus_set(Signal sig, int64_t value) {
    loam_arena_ensure();
    loam_arena_store_sig((int32_t)sig.id, (int32_t)value);
}

void loam_platform_plat_sig_bind_int(int64_t id, int64_t value) {
    int32_t v32 = (int32_t)value;
    loam_zeus_sig_bind(id, &v32, (int64_t)sizeof(v32));
}

int64_t loam_platform_plat_sig_gen(int64_t id) {
    return loam_zeus_sig_gen(id);
}

int64_t loam_platform_plat_sig_alloc_zero(void) {
    return loam_zeus_sig_alloc_zero();
}

/* Freed signal slots, recycled by the allocators below. An id is freed only
   when nothing reachable from the live tree can reference it (subtree
   teardown), so recycling never aliases a live signal. */
static int64_t *g_free_sigs;
static size_t g_nfree_sigs;
static size_t g_cfree_sigs;

static void free_sig_push(int64_t id) {
    if (g_nfree_sigs == g_cfree_sigs) {
        size_t nc = g_cfree_sigs ? g_cfree_sigs * 2 : 64;
        int64_t *next = (int64_t *)realloc(g_free_sigs, nc * sizeof(int64_t));
        if (!next) {
            fprintf(stderr, "zeus: out of memory\n");
            abort();
        }
        g_free_sigs = next;
        g_cfree_sigs = nc;
    }
    g_free_sigs[g_nfree_sigs++] = id;
}

static int64_t free_sig_pop(void) {
    if (g_nfree_sigs <= 0) return -1;
    return g_free_sigs[--g_nfree_sigs];
}

/* Reserve a slot: recycle a freed id or append a fresh one (mirror 0).
   Allocations made while a rebuild scope is open are recorded against the
   scope node so the loam teardown can free them. */
static int64_t sig_slot_zero(void) {
    int64_t id;
    int32_t zero = 0;
    loam_arena_ensure();
    id = free_sig_pop();
    if (id >= 0) {
        ((int32_t *)loam_arena_sigs.ptr)[id] = 0;
    } else {
        loam_vec_push(&loam_arena_sigs, &zero, sizeof(int32_t), __FILE__, __LINE__);
        id = loam_arena_sigs.len - 1;
    }
    if (loam_arena_scope_node > 0) {
        int32_t sid32 = (int32_t)id;
        int32_t owner32 = loam_arena_scope_node;
        loam_vec_push(&loam_arena_rec_sid, &sid32, sizeof(int32_t), __FILE__, __LINE__);
        loam_vec_push(&loam_arena_rec_owner, &owner32, sizeof(int32_t), __FILE__, __LINE__);
    }
    return id;
}

int64_t loam_zeus_sig_alloc_int(int64_t value) {
    int32_t v32 = (int32_t)value;
    int64_t id = sig_slot_zero();
    ((int32_t *)loam_arena_sigs.ptr)[id] = v32;
    loam_zeus_sig_bind(id, &v32, sizeof(v32));
    return id;
}

int64_t loam_zeus_sig_alloc_zero(void) {
    return sig_slot_zero();
}

void loam_zeus_sig_free(int64_t id) {
    if (id <= 0 || (size_t)id >= g_ncells) return;
    if (!g_cells[id].p && g_cells[id].n == 0 && g_cells[id].gen == 0) return;
    if (id < loam_arena_sigs.len) ((int32_t *)loam_arena_sigs.ptr)[id] = 0;
    /* Release the reference the cell held to an owned string before freeing
       the slot that stored the handle. */
    if (g_cells[id].owns_str && g_cells[id].p)
        loam_str_release((loam_str *)g_cells[id].p);
    free(g_cells[id].p);
    g_cells[id].p = NULL;
    g_cells[id].n = 0;
    g_cells[id].gen = 0;
    g_cells[id].owns_str = 0;
    free_sig_push(id);
}

void loam_platform_plat_sig_free(int64_t id) {
    loam_zeus_sig_free(id);
}

/* Allocation accounting (phase 3 proof harness). `loam_rt.h` increments this
   from `loam_new` / array growth when the program was compiled with
   `-DLOAM_ALLOC_TRACE` (all Zeus programs are); `zeus.proof_allocs()` reads it. */
#ifdef LOAM_ALLOC_TRACE
int64_t loam_alloc_count = 0;
#endif

int64_t loam_platform_plat_alloc_count(void) {
#ifdef LOAM_ALLOC_TRACE
    return loam_alloc_count;
#else
    return 0;
#endif
}

static void (*plat_run)(void);
static void (*plat_measure)(const char *s, int64_t px, int64_t *w, int64_t *h);

/* One global font family, not a per-node field: the text ABI carries a pixel
   size only, and threading a family through it would change every host
   (Cocoa / Canvas2D / Android / iOS). A design system uses one typeface, so
   the host reads the current family here when it measures and draws. "" =
   the host default. */
static char font_family[128];
static int (*plat_load_font_fn)(const char *family, const char *src);
static void (*plat_set_family_fn)(const char *family);

const char *zeus_font_family(void) { return font_family; }

void zeus_set_font_hooks(int (*load)(const char *family, const char *src),
                         void (*set_family)(const char *family)) {
    plat_load_font_fn = load;
    plat_set_family_fn = set_family;
}
static void (*plat_redraw)(void);
/* Phase 4: the host's one-blit callback. NULL until a host registers one, so
   blitting is a no-op everywhere else (Linux, iOS, Android, web, headless). */
static void *blit_ctx;
static ZeusBlit blit_fn;
static void (*plat_pick_image)(char *out, int cap, int64_t *w, int64_t *h);
/* Intrinsic size of a decoded image, in layout points. NULL or a 0 result
   means "not decoded yet"; layout then keeps the box width-only (an image
   with neither width nor height stays empty until the host reports a size). */
static void (*plat_image_size_fn)(const char *src, int64_t *w, int64_t *h);

static char *win_title;
static void (*plat_title_fn)(const char *);
void zeus_set_title_hook(void (*fn)(const char *)) { plat_title_fn = fn; }
static int64_t win_w = ZEUS_DEFAULT_WIN_W, win_h = ZEUS_DEFAULT_WIN_H;

/* Desktop "initial view" providers: the size the host will open its window
   at (device screen work area). Only consulted before the window exists and
   only on real (non-headless) runs, so headless layout stays deterministic. */
static int64_t (*plat_initial_w)(void);
static int64_t (*plat_initial_h)(void);
static int opened_window;

static void *paint_ctx;
static ZeusDraw paint_draw;
static int have_draw;

static char *dup_ys(loam_str s) {
    size_t n = s.len < 0 ? 0 : (size_t)s.len;
    char *p = (char *)malloc(n + 1);
    if (!p) {
        fprintf(stderr, "zeus: out of memory\n");
        abort();
    }
    if (s.ptr && n) memcpy(p, s.ptr, n);
    p[n] = '\0';
    return p;
}

static void measure_default(const char *s, int64_t px, int64_t *w, int64_t *h) {
    size_t n = s ? strlen(s) : 0;
    if (px < 8) px = 8;
    *w = (int64_t)n * (px * 6 / 10);
    *h = px + 4;
}

void zeus_set_platform(void (*run)(void),
                      void (*measure)(const char *s, int64_t px, int64_t *w, int64_t *h),
                      void (*redraw)(void)) {
    plat_run = run;
    plat_measure = measure;
    plat_redraw = redraw;
}

void zeus_set_pick_image(void (*pick)(char *out, int cap, int64_t *w, int64_t *h)) {
    plat_pick_image = pick;
}

void zeus_set_image_size(void (*size)(const char *src, int64_t *w, int64_t *h)) {
    plat_image_size_fn = size;
}

void zeus_set_blit(void *ctx, ZeusBlit fn) {
    blit_ctx = ctx;
    blit_fn = fn;
}

static loam_str pick_dup(const char *src) {
    size_t n = src ? strlen(src) : 0;
    char *p;
    if (n == 0) return (loam_str){"", 0};
    p = (char *)loam_new(n + 1, "pick_image", 0);
    memcpy(p, src, n);
    p[n] = 0;
    return (loam_str){p, (int64_t)n};
}

/* ---- intrinsic image size fallback ------------------------------------
   The host reports the size of a decoded image (remote loads answer a frame
   later). When it has nothing yet — headless layout, a host without the
   hook, or a source it has not decoded — parse the container header of the
   common web formats. Layout only needs the aspect ratio to give a
   widthless `Image` its height, so a best-effort read of the first bytes is
   enough; 0 means "unknown", which leaves the box exactly as it was. */

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t le16(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t be16(const unsigned char *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

static int parse_jpeg(const unsigned char *p, size_t n, int64_t *w, int64_t *h) {
    size_t i = 2;
    while (i + 9 <= n) {
        unsigned m;
        if (p[i] != 0xFF) {
            i++;
            continue;
        }
        m = p[i + 1];
        if (m == 0xFF || m == 0x00) {
            i++;
            continue;
        }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2;
            continue;
        }
        if (m == 0xD9 || m == 0xDA) return 0; /* end / start of scan, no SOF */
        /* SOF0..SOF15 hold the frame size; C4 (Huffman), C8, CC do not. */
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = (int64_t)be16(p + i + 5);
            *w = (int64_t)be16(p + i + 7);
            return *w > 0 && *h > 0;
        }
        i += 2 + be16(p + i + 2);
    }
    return 0;
}

static int parse_image(const unsigned char *p, size_t n, int64_t *w, int64_t *h) {
    if (n >= 24 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) {
        *w = (int64_t)be32(p + 16);
        *h = (int64_t)be32(p + 20);
        return *w > 0 && *h > 0;
    }
    if (n >= 10 && (!memcmp(p, "GIF87a", 6) || !memcmp(p, "GIF89a", 6))) {
        *w = (int64_t)le16(p + 6);
        *h = (int64_t)le16(p + 8);
        return *w > 0 && *h > 0;
    }
    if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) return parse_jpeg(p, n, w, h);
    if (n >= 30 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) {
        const unsigned char *q = p + 20;
        if (!memcmp(p + 12, "VP8X", 4)) { /* extended: 24-bit le canvas - 1 */
            *w = (int64_t)(q[4] | (q[5] << 8) | ((uint32_t)q[6] << 16)) + 1;
            *h = (int64_t)(q[7] | (q[8] << 8) | ((uint32_t)q[9] << 16)) + 1;
            return 1;
        }
        if (!memcmp(p + 12, "VP8 ", 4)) { /* lossy: start code, 14-bit dims */
            if (q[3] != 0x9D || q[4] != 0x01 || q[5] != 0x2A) return 0;
            *w = (int64_t)(le16(q + 6) & 0x3FFF);
            *h = (int64_t)(le16(q + 8) & 0x3FFF);
            return *w > 0 && *h > 0;
        }
        if (!memcmp(p + 12, "VP8L", 4) && n >= 25 && q[0] == 0x2F) { /* lossless */
            uint32_t bits = (uint32_t)q[1] | ((uint32_t)q[2] << 8) | ((uint32_t)q[3] << 16) |
                            ((uint32_t)q[4] << 24);
            *w = (int64_t)(bits & 0x3FFF) + 1;
            *h = (int64_t)((bits >> 14) & 0x3FFF) + 1;
            return 1;
        }
    }
    return 0;
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* 1 when `p` is a prefix of `s`; stops at `p`'s end, so a shorter `s` cannot
   be read past its terminator. */
static int is_prefix(const char *s, const char *p) {
    while (*p) {
        if (*s != *p) return 0;
        s++;
        p++;
    }
    return 1;
}

/* Base64 body of a `data:` URI → the first `cap` decoded bytes. */
static size_t data_head(const char *uri, unsigned char *out, size_t cap) {
    const char *p = uri;
    size_t n = 0;
    unsigned acc = 0;
    int bits = 0;
    while (*p) {
        if (is_prefix(p, "base64,")) {
            p += 7;
            break;
        }
        p++;
    }
    if (!*p) return 0;
    for (; *p && n < cap; p++) {
        int v = b64_val((unsigned char)*p);
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return n;
}

/* Best-effort size of a `data:` URI or a local file (never a remote URL).
   wasm has no filesystem and decodes through its own loader hook, so only
   the inline-`data:` path is compiled there. */
static int parse_source(const char *src, int64_t *w, int64_t *h) {
    unsigned char buf[4096];
    size_t n = 0;
    if (!src || !src[0]) return 0;
    if (is_prefix(src, "data:")) {
        n = data_head(src, buf, sizeof buf);
    }
#ifndef __wasm32__
    else {
        FILE *f;
        const char *path = src;
        if (is_prefix(src, "http://") || is_prefix(src, "https://")) return 0;
        if (is_prefix(src, "file://")) path = src + 7;
        f = fopen(path, "rb");
        if (!f) return 0;
        n = fread(buf, 1, sizeof buf, f);
        fclose(f);
    }
#endif
    if (n == 0) return 0;
    return parse_image(buf, n, w, h);
}

loam_str loam_zeus_plat_pick_image(int32_t *w, int32_t *h) {
    char buf[4096];
    int64_t lw = 0, lh = 0;
    buf[0] = 0;
    if (plat_pick_image) plat_pick_image(buf, (int)sizeof buf, &lw, &lh);
    if (w) *w = (int32_t)lw;
    if (h) *h = (int32_t)lh;
    return pick_dup(buf);
}

void loam_zeus_plat_image_size(loam_str src, int32_t *w, int32_t *h) {
    char *p = dup_ys(src);
    int64_t lw = 0, lh = 0;
    if (plat_image_size_fn) plat_image_size_fn(p, &lw, &lh);
    if (lw <= 0 || lh <= 0) parse_source(p, &lw, &lh);
    free(p);
    if (w) *w = (int32_t)lw;
    if (h) *h = (int32_t)lh;
}

void zeus_picked_image(const char *src, int64_t w, int64_t h) {
    loam_zeus_engine_picked_image(pick_dup(src), w, h);
    if (plat_redraw) plat_redraw();
}

#ifdef LOAM_HOST_BUILD
/* The table `zeus_rt.h` routes the host's engine calls through. Zeroed until
   the loader fills it from the image it just loaded. */
ZeusAppApi zeus_app_api;

static int host_mode;
static void (*plat_host_run_fn)(const char *path);

/* Called by a host once it is the one owning the loop, before it loads the
   app image. */
void zeus_set_host_mode(void) { host_mode = 1; }

/* The loader (a host provides one; see `mac.m`). */
void zeus_set_host_hooks(void (*run)(const char *path)) { plat_host_run_fn = run; }
#endif

void loam_zeus_plat_run(void) {
#ifdef LOAM_HOST_BUILD
    /* A host owns the loop: the app has registered its tree by now (`zeus.App`
       builds and roots it before `raw_run`), so this returns and the host drives
       the frames. */
    if (host_mode) return;
#endif
    if (plat_run) plat_run();
}

/* `host.run(path)` (std:host) — hand this process to a host that loads the app
   image at `path`. Only a host build has one; elsewhere this is a no-op, so a
   program that calls it still links and still runs its own loop. */
void loam_host_run(loam_str path) {
#ifdef LOAM_HOST_BUILD
    if (!plat_host_run_fn) return;
    char *p = dup_ys(path);
    plat_host_run_fn(p);
    free(p);
#else
    (void)path;
#endif
}

int64_t loam_zeus_plat_headless(void) {
    if (getenv("ZEUS_HEADLESS") || !plat_run) return 1;
    return 0;
}

void loam_zeus_plat_fill(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                        int64_t radius) {
    if (!have_draw || !paint_draw.fill) return;
    paint_draw.fill(paint_ctx, x, y, w, h, rgb & 0xFFFFFF, radius);
}

void loam_zeus_plat_fill_a(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                          int64_t radius, int64_t alpha) {
    if (!have_draw) return;
    if (paint_draw.fill_a)
        paint_draw.fill_a(paint_ctx, x, y, w, h, rgb & 0xFFFFFF, radius, alpha);
    else if (paint_draw.fill)
        paint_draw.fill(paint_ctx, x, y, w, h, rgb & 0xFFFFFF, radius);
}

void loam_zeus_plat_fill_g(int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                          int64_t c1, int64_t axis, int64_t radius, int64_t alpha) {
    if (!have_draw) return;
    if (paint_draw.fill_g)
        paint_draw.fill_g(paint_ctx, x, y, w, h, c0 & 0xFFFFFF, c1 & 0xFFFFFF, axis,
                          radius, alpha);
    else if (paint_draw.fill_a)
        paint_draw.fill_a(paint_ctx, x, y, w, h, c0 & 0xFFFFFF, radius, alpha);
    else if (paint_draw.fill)
        paint_draw.fill(paint_ctx, x, y, w, h, c0 & 0xFFFFFF, radius);
}

/* Elevation. A host without a blur skips the shadow outright rather than
   approximating it: a hard-edged rectangle under every card reads far worse
   than no shadow at all, and the design system's flat level (0) is a real
   level, so "no shadow" is always a valid rendering. */
void loam_zeus_plat_shadow(int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius,
                          int64_t rgb, int64_t alpha, int64_t blur, int64_t dx,
                          int64_t dy) {
    if (!have_draw || !paint_draw.shadow) return;
    if (alpha <= 0 || w <= 0 || h <= 0) return;
    paint_draw.shadow(paint_ctx, x, y, w, h, radius, rgb & 0xFFFFFF, alpha, blur, dx, dy);
}

/* Ring stroke. The fallback paints four edges with `fill_a`, which is square
   at the corners but keeps the border visible and — unlike the old underlay
   fill it replaces — does not require the node to have a background. */
void loam_zeus_plat_stroke(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                          int64_t radius, int64_t width, int64_t alpha) {
    if (!have_draw) return;
    if (width <= 0 || w <= 0 || h <= 0) return;
    if (paint_draw.stroke) {
        paint_draw.stroke(paint_ctx, x, y, w, h, rgb & 0xFFFFFF, radius, width, alpha);
        return;
    }
    if (!paint_draw.fill_a) return;
    if (width * 2 > w) width = w / 2 > 0 ? w / 2 : 1;
    if (width * 2 > h) width = h / 2 > 0 ? h / 2 : 1;
    paint_draw.fill_a(paint_ctx, x, y, w, width, rgb & 0xFFFFFF, 0, alpha);
    paint_draw.fill_a(paint_ctx, x, y + h - width, w, width, rgb & 0xFFFFFF, 0, alpha);
    paint_draw.fill_a(paint_ctx, x, y + width, width, h - width * 2, rgb & 0xFFFFFF, 0, alpha);
    paint_draw.fill_a(paint_ctx, x + w - width, y + width, width, h - width * 2,
                      rgb & 0xFFFFFF, 0, alpha);
}

void loam_zeus_plat_fill4(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                         int64_t alpha, int64_t tl, int64_t tr, int64_t br, int64_t bl) {
    if (!have_draw) return;
    if (paint_draw.fill4) {
        paint_draw.fill4(paint_ctx, x, y, w, h, rgb & 0xFFFFFF, alpha, tl, tr, br, bl);
        return;
    }
    {
        int64_t r = tl;
        if (tr > r) r = tr;
        if (br > r) r = br;
        if (bl > r) r = bl;
        if (paint_draw.fill_a)
            paint_draw.fill_a(paint_ctx, x, y, w, h, rgb & 0xFFFFFF, r, alpha);
        else if (paint_draw.fill)
            paint_draw.fill(paint_ctx, x, y, w, h, rgb & 0xFFFFFF, r);
    }
}

/* Paint-space transform. A NULL host leaves the widget at its settled
   position, which is the correct degradation: the transition is skipped,
   not half-applied. */
void loam_zeus_plat_xform(int64_t dx, int64_t dy, int64_t scale, int64_t rot,
                         int64_t ox, int64_t oy) {
    if (!have_draw || !paint_draw.xform) return;
    paint_draw.xform(paint_ctx, dx, dy, scale, rot, ox, oy);
}

void loam_zeus_plat_text(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font) {
    char *p;
    if (!have_draw || !paint_draw.text) return;
    p = dup_ys(s);
    paint_draw.text(paint_ctx, x, y, p, rgb & 0xFFFFFF, font);
    free(p);
}

void loam_zeus_plat_text_rot(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font,
                             int64_t deg) {
    char *p;
    if (!have_draw) return;
    if (!paint_draw.text_rot) {
        if (paint_draw.text) loam_zeus_plat_text(x, y, s, rgb, font);
        return;
    }
    p = dup_ys(s);
    paint_draw.text_rot(paint_ctx, x, y, p, rgb & 0xFFFFFF, font, deg);
    free(p);
}

void loam_zeus_plat_text_int(int64_t x, int64_t y, int64_t v, int64_t rgb, int64_t font) {
    char buf[32];
    if (!have_draw || !paint_draw.text) return;
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    paint_draw.text(paint_ctx, x, y, buf, rgb & 0xFFFFFF, font);
}

void loam_zeus_plat_svg(int64_t x, int64_t y, int64_t w, int64_t h, loam_str markup,
                       int64_t rgb, int64_t alpha) {
    char *p;
    if (!have_draw || !paint_draw.svg) return;
    p = dup_ys(markup);
    paint_draw.svg(paint_ctx, x, y, w, h, p, rgb & 0xFFFFFF, alpha);
    free(p);
}

void loam_zeus_plat_image(int64_t x, int64_t y, int64_t w, int64_t h, loam_str src,
                         int64_t radius, int64_t alpha, int64_t fit) {
    char *p;
    if (!have_draw || !paint_draw.image) return;
    p = dup_ys(src);
    paint_draw.image(paint_ctx, x, y, w, h, p, radius, alpha, fit);
    free(p);
}

void loam_zeus_plat_save(void) {
    if (have_draw && paint_draw.save) paint_draw.save(paint_ctx);
}

void loam_zeus_plat_alpha(int64_t a) {
    if (have_draw && paint_draw.alpha) paint_draw.alpha(paint_ctx, a);
}

void loam_zeus_plat_clip(int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius) {
    if (have_draw && paint_draw.clip) paint_draw.clip(paint_ctx, x, y, w, h, radius);
}

void loam_zeus_plat_restore(void) {
    if (have_draw && paint_draw.restore) paint_draw.restore(paint_ctx);
}

/* The one blit of a finished frame (Phase 4). The pixels were produced by the
   Loam rasterizer; this forwards the raw bytes to whatever host registered a
   hook. No hook means no display (headless, or a host that has not implemented
   it), which answers 0 rather than pretending a frame was presented. */
int64_t loam_zeus_plat_blit(const uint8_t *px, int64_t w, int64_t h, int64_t gen,
                           int64_t scale) {
    if (!blit_fn || !px || w <= 0 || h <= 0) return 0;
    return blit_fn(blit_ctx, px, w, h, gen, scale);
}

void loam_platform_plat_set_font_family(loam_str name) {
    char *p = dup_ys(name);
    if (p) {
        size_t n = strlen(p);
        if (n >= sizeof font_family) n = sizeof font_family - 1;
        memcpy(font_family, p, n);
        font_family[n] = 0;
        free(p);
    }
    /* Metrics change with the family, so anything cached is stale. A host
       that caches by size (Canvas2D) needs telling; one that reads
       zeus_font_family() per call (Cocoa) does not. */
    if (plat_set_family_fn) plat_set_family_fn(font_family);
    if (plat_redraw) plat_redraw();
}

int64_t loam_platform_plat_load_font(loam_str family, loam_str src) {
    char *f, *u;
    int ok = 0;
    if (!plat_load_font_fn) return 0;
    f = dup_ys(family);
    u = dup_ys(src);
    if (f && u) ok = plat_load_font_fn(f, u);
    free(f);
    free(u);
    return ok ? 1 : 0;
}

/* In-tree metrics (`packages/zeus/std/zeuscore/metrics.loam`) parse the font's tables, so
   they need its bytes. The default reads `src` as a path — desktop, iOS, and
   Android all have a filesystem. The web host has none and installs a hook
   that fetches the URL and pushes the bytes back (packages/zeus/hosts/web). */
static loam_str (*plat_font_bytes_fn)(loam_str src);

void zeus_set_font_bytes_hook(loam_str (*fetch)(loam_str src)) {
    plat_font_bytes_fn = fetch;
}

loam_str loam_platform_plat_font_bytes(loam_str src) {
    if (plat_font_bytes_fn) return plat_font_bytes_fn(src);
    return loam_sys_read_file(src);
}

void loam_zeus_plat_measure(loam_str s, int32_t px, int32_t *w, int32_t *h) {
    char *p = dup_ys(s);
    int64_t tw = 0, th = 0;
    if (plat_measure) plat_measure(p, px, &tw, &th);
    else measure_default(p, px, &tw, &th);
    if (w) *w = (int32_t)tw;
    if (h) *h = (int32_t)th;
    free(p);
}

void loam_zeus_plat_measure_int(int32_t v, int32_t px, int32_t *w, int32_t *h) {
    char buf[32];
    int64_t tw = 0, th = 0;
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    if (plat_measure) plat_measure(buf, px, &tw, &th);
    else measure_default(buf, px, &tw, &th);
    if (w) *w = (int32_t)tw;
    if (h) *h = (int32_t)th;
}

static void measure_span(const char *s, int64_t n, int64_t px, int64_t *w, int64_t *h) {
    char stack[256];
    char *tmp;
    if (n < 0) n = 0;
    if ((size_t)n + 1 <= sizeof stack) {
        tmp = stack;
    } else {
        tmp = (char *)malloc((size_t)n + 1);
        if (!tmp) abort();
    }
    if (n) memcpy(tmp, s, (size_t)n);
    tmp[n] = '\0';
    if (plat_measure) plat_measure(tmp, px, w, h);
    else measure_default(tmp, px, w, h);
    if (tmp != stack) free(tmp);
}

static int64_t wrap_line_h(int64_t px) {
    int64_t w = 0, h = 0;
    measure_span("Mg", 2, px, &w, &h);
    if (h < 1) h = px + 4;
    return h;
}

static void wrap_text(const char *s, int64_t n, int64_t px, int64_t max_w, int paint,
                      int64_t x0, int64_t y0, int64_t rgb, int64_t *out_w, int64_t *out_h) {
    int64_t i = 0, line_w = 0, maxlw = 0, lines = 0, y = y0;
    int64_t lh, sw = 0, sh = 0;
    if (!s) s = "";
    if (n < 0) n = 0;
    if (max_w < 1) max_w = 1;
    lh = wrap_line_h(px);
    measure_span(" ", 1, px, &sw, &sh);
    while (i < n) {
        int64_t j, ww = 0, wh = 0;
        if (s[i] == '\n') {
            if (line_w > maxlw) maxlw = line_w;
            line_w = 0;
            lines++;
            y += lh;
            i++;
            continue;
        }
        if (s[i] == ' ' || s[i] == '\t') {
            if (line_w > 0) {
                if (line_w + sw > max_w) {
                    if (line_w > maxlw) maxlw = line_w;
                    line_w = 0;
                    lines++;
                    y += lh;
                } else {
                    if (paint && have_draw && paint_draw.text)
                        paint_draw.text(paint_ctx, x0 + line_w, y, " ", rgb & 0xFFFFFF, px);
                    line_w += sw;
                }
            }
            i++;
            continue;
        }
        j = i;
        while (j < n && s[j] != ' ' && s[j] != '\t' && s[j] != '\n') j++;
        measure_span(s + i, j - i, px, &ww, &wh);
        if (line_w > 0 && line_w + ww > max_w) {
            if (line_w > maxlw) maxlw = line_w;
            line_w = 0;
            lines++;
            y += lh;
        }
        if (paint && have_draw && paint_draw.text) {
            char stack[256];
            char *tmp;
            int64_t wn = j - i;
            if (wn < 0) wn = 0;
            if ((size_t)wn + 1 <= sizeof stack) tmp = stack;
            else {
                tmp = (char *)malloc((size_t)wn + 1);
                if (!tmp) abort();
            }
            if (wn) memcpy(tmp, s + i, (size_t)wn);
            tmp[wn] = '\0';
            paint_draw.text(paint_ctx, x0 + line_w, y, tmp, rgb & 0xFFFFFF, px);
            if (tmp != stack) free(tmp);
        }
        line_w += ww;
        i = j;
    }
    if (n > 0) {
        if (line_w > maxlw) maxlw = line_w;
        lines++;
    }
    if (out_w) *out_w = maxlw;
    if (out_h) *out_h = lines * lh;
}

void loam_zeus_plat_measure_wrap(loam_str s, int32_t px, int32_t max_w, int32_t *w, int32_t *h) {
    int64_t ww = 0, hh = 0;
    wrap_text(s.ptr, s.len, px, max_w, 0, 0, 0, 0, &ww, &hh);
    if (w) *w = (int32_t)ww;
    if (h) *h = (int32_t)hh;
}

void loam_zeus_plat_text_wrap(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font,
                             int64_t max_w) {
    wrap_text(s.ptr, s.len, font, max_w, 1, x, y, rgb, NULL, NULL);
}

void loam_zeus_plat_set_window(loam_str title, int64_t width, int64_t height) {
    free(win_title);
    win_title = dup_ys(title);
#if defined(LOAM_IOS) || defined(LOAM_ANDROID)
    /* Phone hosts layout to the view every frame. Page(560, 520) must not
       replace that width or the UI is clipped on the right. */
    (void)width;
    (void)height;
#else
    if (width > 0) win_w = width;
    if (height > 0) win_h = height;
#endif
}

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("view_w")))
int32_t zeus_js_view_w(void);
__attribute__((import_module("zeus"), import_name("view_h")))
int32_t zeus_js_view_h(void);
#endif

int64_t loam_zeus_plat_view_width(void) {
#ifdef __wasm32__
    int32_t w = zeus_js_view_w();
    if (w > 0) return (int64_t)w;
#endif
    /* Desktop: before the window exists, report the size the host will open
       it at (full device screen) instead of the 640×480 placeholder, so an
       app that passes zeus.window_size() to zeus.App opens screen-filling. */
    if (!opened_window && !loam_zeus_plat_headless() && plat_initial_w) {
        int64_t w = plat_initial_w();
        if (w > 0) return w;
    }
    return win_w;
}

int64_t loam_zeus_plat_view_height(void) {
#ifdef __wasm32__
    int32_t h = zeus_js_view_h();
    if (h > 0) return (int64_t)h;
#endif
    if (!opened_window && !loam_zeus_plat_headless() && plat_initial_h) {
        int64_t h = plat_initial_h();
        if (h > 0) return h;
    }
    return win_h;
}

void zeus_set_window_size(int64_t width, int64_t height) {
    if (width > 0) win_w = width;
    if (height > 0) win_h = height;
}

void zeus_set_initial_view(int64_t (*width)(void), int64_t (*height)(void)) {
    plat_initial_w = width;
    plat_initial_h = height;
}

void zeus_window_opened(void) {
    opened_window = 1;
}

static int64_t g_inset_t, g_inset_r, g_inset_b, g_inset_l;

void zeus_set_insets(int64_t top, int64_t right, int64_t bottom, int64_t left) {
    g_inset_t = top < 0 ? 0 : top;
    g_inset_r = right < 0 ? 0 : right;
    g_inset_b = bottom < 0 ? 0 : bottom;
    g_inset_l = left < 0 ? 0 : left;
}

static int64_t g_overlay_scroll = -1;

void zeus_set_overlay_scroll(int64_t on) {
    g_overlay_scroll = on <= 0 ? 0 : 1;
}

int64_t loam_zeus_plat_overlay_scroll(void) {
    if (g_overlay_scroll >= 0) {
        return g_overlay_scroll;
    }
#if defined(LOAM_IOS) || defined(LOAM_ANDROID)
    return 1;
#else
    return 0;
#endif
}

int64_t loam_zeus_plat_inset_top(void) { return g_inset_t; }
int64_t loam_zeus_plat_inset_right(void) { return g_inset_r; }
int64_t loam_zeus_plat_inset_bottom(void) { return g_inset_b; }
int64_t loam_zeus_plat_inset_left(void) { return g_inset_l; }

/* packages/zeus/std/zeuscore/platform.loam is the GPUI-style platform leaf. Empty Loam stubs
   compile to loam_platform_plat_*; the Cocoa implementations stay as
   loam_zeus_plat_* so existing callers do not change. */
void loam_platform_plat_run(void) { loam_zeus_plat_run(); }
int64_t loam_platform_plat_headless(void) { return loam_zeus_plat_headless(); }
void loam_platform_plat_fill(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                             int64_t radius) {
    loam_zeus_plat_fill(x, y, w, h, rgb, radius);
}
void loam_platform_plat_fill_a(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                               int64_t radius, int64_t alpha) {
    loam_zeus_plat_fill_a(x, y, w, h, rgb, radius, alpha);
}
void loam_platform_plat_fill_g(int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                               int64_t c1, int64_t axis, int64_t radius, int64_t alpha) {
    loam_zeus_plat_fill_g(x, y, w, h, c0, c1, axis, radius, alpha);
}
void loam_platform_plat_shadow(int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius,
                               int64_t rgb, int64_t alpha, int64_t blur, int64_t dx,
                               int64_t dy) {
    loam_zeus_plat_shadow(x, y, w, h, radius, rgb, alpha, blur, dx, dy);
}
void loam_platform_plat_stroke(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                               int64_t radius, int64_t width, int64_t alpha) {
    loam_zeus_plat_stroke(x, y, w, h, rgb, radius, width, alpha);
}
void loam_platform_plat_fill4(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                              int64_t alpha, int64_t tl, int64_t tr, int64_t br,
                              int64_t bl) {
    loam_zeus_plat_fill4(x, y, w, h, rgb, alpha, tl, tr, br, bl);
}
void loam_platform_plat_xform(int64_t dx, int64_t dy, int64_t scale, int64_t rot,
                              int64_t ox, int64_t oy) {
    loam_zeus_plat_xform(dx, dy, scale, rot, ox, oy);
}
void loam_platform_plat_text(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font) {
    loam_zeus_plat_text(x, y, s, rgb, font);
}

void loam_platform_plat_text_rot(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font,
                                 int64_t deg) {
    loam_zeus_plat_text_rot(x, y, s, rgb, font, deg);
}
void loam_platform_plat_text_int(int64_t x, int64_t y, int64_t v, int64_t rgb, int64_t font) {
    loam_zeus_plat_text_int(x, y, v, rgb, font);
}
void loam_platform_plat_svg(int64_t x, int64_t y, int64_t w, int64_t h, loam_str markup,
                            int64_t rgb, int64_t alpha) {
    loam_zeus_plat_svg(x, y, w, h, markup, rgb, alpha);
}
void loam_platform_plat_image(int64_t x, int64_t y, int64_t w, int64_t h, loam_str src,
                              int64_t radius, int64_t alpha, int64_t fit) {
    loam_zeus_plat_image(x, y, w, h, src, radius, alpha, fit);
}
loam_str loam_platform_plat_pick_image(int32_t *w, int32_t *h) {
    return loam_zeus_plat_pick_image(w, h);
}
void loam_platform_plat_image_size(loam_str src, int32_t *w, int32_t *h) {
    loam_zeus_plat_image_size(src, w, h);
}
void loam_platform_plat_save(void) { loam_zeus_plat_save(); }
void loam_platform_plat_alpha(int64_t a) { loam_zeus_plat_alpha(a); }

/* Wall clock for host-sleep-safe deadlines (scrollbar hide). */
int64_t loam_platform_plat_now_ms(void) { return loam_async_now_ms(); }

/* Host process memory in KB for the gallery RAM chip: Apple phys_footprint
   (the number Xcode's memory gauge shows — RSS overstates iOS processes),
   Linux/Android resident set, wasm live heap on the web host. */
int64_t loam_platform_plat_mem_kb(void) {
#ifdef __wasm32__
    extern int32_t zeus_heap_used(void);
    return (int64_t)(zeus_heap_used() / 1024);
#elif defined(__APPLE__)
    {
        struct task_vm_info info;
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
            return (int64_t)(info.phys_footprint / 1024);
        return 0;
    }
#elif defined(__linux__)
    {
        long rss_kb = 0;
        FILE *f = fopen("/proc/self/statm", "r");
        if (f) {
            unsigned long size_pages = 0, res_pages = 0;
            if (fscanf(f, "%lu %lu", &size_pages, &res_pages) == 2)
                rss_kb = (long)res_pages * (long)sysconf(_SC_PAGESIZE) / 1024;
            fclose(f);
        }
        return (int64_t)rss_kb;
    }
#else
    return 0;
#endif
}

/* Phase 0 arena-stats gate (`platform.plat_mem_stats`). A build flag turns it on
   for good; the env var turns it on for one run; neither changes allocation. */
int64_t loam_platform_plat_mem_stats(void) {
#if defined(ZEUS_MEM_STATS)
    return 1;
#else
    const char *v = getenv("ZEUS_MEM_STATS");
    if (!v || !v[0] || strcmp(v, "0") == 0) return 0;
    return 1;
#endif
}

void loam_platform_plat_clip(int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius) {
    loam_zeus_plat_clip(x, y, w, h, radius);
}
void loam_platform_plat_restore(void) { loam_zeus_plat_restore(); }

/* Phase 4: the one blit. `buf` is the Loam framebuffer vector handed over by
   value — we pass its first byte and its length straight through, so not one
   pixel is copied here. `gen` is bumped by the Loam side ONLY on reallocation
   (a size or backing-scale change), which is the signal a host needs to rebuild
   its wrapper; between bumps the same bytes stay wrapped.

   The integer types are int32_t to match the prototype codegen emits for the
   bodyless `platform.plat_blit`; `w * h * 4` is widened to int64_t so a large
   surface cannot overflow the length check. */
int32_t loam_platform_plat_blit(loam_vec buf, int32_t w, int32_t h, int32_t gen,
                                int32_t scale) {
    int64_t need = (int64_t)w * (int64_t)h * 4;
    if (!buf.ptr || w <= 0 || h <= 0 || (int64_t)buf.len < need) return 0;
    return (int32_t)loam_zeus_plat_blit((const uint8_t *)buf.ptr, w, h, gen, scale);
}
int64_t loam_platform_plat_key_intern(loam_str name) {
    char *s = dup_ys(name);
    int id = zeus_key_context_id(s);
    free(s);
    return id;
}
int64_t loam_platform_plat_key_intern_action(loam_str action) {
    char *s = dup_ys(action);
    int id = zeus_key_context_from_action(s);
    free(s);
    return id;
}
static void intern_fn_reset(void);

void loam_platform_plat_key_reset(void) {
    zeus_key_reset_handlers();
    intern_fn_reset();
}
void loam_platform_plat_map_key(loam_str spec, loam_str action, int64_t ctx) {
    char *a = dup_ys(spec), *b = dup_ys(action);
    zeus_key_map_ctx(a, b, (int)ctx);
    free(a);
    free(b);
}

int64_t loam_platform_plat_key_ev(int64_t key, int64_t mods) {
    return zeus_handle_key_ev((int)key, (int)mods);
}

#define ZEUS_EDIT_SLOTS 64
#define ZEUS_EDIT_MAX 65536

static char *edit_buf[ZEUS_EDIT_SLOTS];
static int edit_len[ZEUS_EDIT_SLOTS];
static int edit_cap[ZEUS_EDIT_SLOTS];
static int edit_pos[ZEUS_EDIT_SLOTS];
static int edit_anchor[ZEUS_EDIT_SLOTS];
static char *edit_mark[ZEUS_EDIT_SLOTS];
static int edit_mark_len[ZEUS_EDIT_SLOTS];
static char *edit_view[ZEUS_EDIT_SLOTS];
static int edit_view_cap[ZEUS_EDIT_SLOTS];
static int edit_wrap_w[ZEUS_EDIT_SLOTS];
static int edit_font[ZEUS_EDIT_SLOTS];

static int edit_ok(int64_t slot) {
    return slot >= 0 && slot < ZEUS_EDIT_SLOTS;
}

static void edit_clamp(int slot) {
    if (edit_pos[slot] < 0) edit_pos[slot] = 0;
    if (edit_pos[slot] > edit_len[slot]) edit_pos[slot] = edit_len[slot];
    if (edit_anchor[slot] < 0) edit_anchor[slot] = 0;
    if (edit_anchor[slot] > edit_len[slot]) edit_anchor[slot] = edit_len[slot];
}

static int edit_lo(int slot) {
    return edit_pos[slot] < edit_anchor[slot] ? edit_pos[slot] : edit_anchor[slot];
}

static int edit_hi(int slot) {
    return edit_pos[slot] > edit_anchor[slot] ? edit_pos[slot] : edit_anchor[slot];
}

static int edit_has_sel(int slot) { return edit_pos[slot] != edit_anchor[slot]; }

static int utf8_next(const char *s, int n, int i) {
    unsigned char c;
    int adv = 1;
    if (i >= n) return n;
    c = (unsigned char)s[i];
    if ((c & 0x80) == 0) adv = 1;
    else if ((c & 0xE0) == 0xC0) adv = 2;
    else if ((c & 0xF0) == 0xE0) adv = 3;
    else if ((c & 0xF8) == 0xF0) adv = 4;
    i += adv;
    return i > n ? n : i;
}

static int utf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int edit_grow(int slot, int need) {
    int cap;
    char *p;
    if (need < 1) need = 1;
    if (need > ZEUS_EDIT_MAX) return 0;
    if (need <= edit_cap[slot]) return 1;
    cap = edit_cap[slot] ? edit_cap[slot] * 2 : 512;
    while (cap < need) cap *= 2;
    if (cap > ZEUS_EDIT_MAX) cap = ZEUS_EDIT_MAX;
    p = (char *)realloc(edit_buf[slot], (size_t)cap);
    if (!p) return 0;
    edit_buf[slot] = p;
    edit_cap[slot] = cap;
    return 1;
}

static void edit_clear_mark(int slot) {
    if (edit_mark[slot]) edit_mark[slot][0] = '\0';
    edit_mark_len[slot] = 0;
}

static int edit_delete_range(int slot, int lo, int hi) {
    int n = edit_len[slot];
    if (lo < 0) lo = 0;
    if (hi > n) hi = n;
    if (hi <= lo) return 0;
    if (edit_buf[slot]) memmove(edit_buf[slot] + lo, edit_buf[slot] + hi, (size_t)(n - hi + 1));
    edit_len[slot] = n - (hi - lo);
    edit_pos[slot] = lo;
    edit_anchor[slot] = lo;
    return 1;
}

static int edit_insert_bytes(int slot, const char *p, int n) {
    int at;
    if (n < 0) n = 0;
    if (edit_has_sel(slot)) edit_delete_range(slot, edit_lo(slot), edit_hi(slot));
    edit_clear_mark(slot);
    at = edit_pos[slot];
    if (!edit_grow(slot, edit_len[slot] + n + 1)) return 0;
    if (n && p) {
        memmove(edit_buf[slot] + at + n, edit_buf[slot] + at, (size_t)(edit_len[slot] - at + 1));
        memcpy(edit_buf[slot] + at, p, (size_t)n);
        edit_len[slot] += n;
        edit_pos[slot] = at + n;
        edit_anchor[slot] = edit_pos[slot];
    }
    if (edit_buf[slot]) edit_buf[slot][edit_len[slot]] = '\0';
    return 1;
}

static int edit_put(int slot, char ch) {
    char b[1];
    b[0] = ch;
    return edit_insert_bytes(slot, b, 1);
}

int64_t loam_platform_plat_edit_append(int64_t slot, int64_t key) {
    int i;
    if (!edit_ok(slot)) return 0;
    if (key == 13) key = 10;
    if (key == 9) {
        for (i = 0; i < 4; i++) {
            if (!edit_put((int)slot, ' ')) return 1;
        }
        return 1;
    }
    if (key != 10 && (key < 32 || key >= 127)) return 0;
    return edit_put((int)slot, (char)key);
}

int64_t loam_platform_plat_edit_back(int64_t slot) {
    int lo, hi;
    if (!edit_ok(slot)) return 0;
    if (edit_has_sel((int)slot))
        return edit_delete_range((int)slot, edit_lo((int)slot), edit_hi((int)slot));
    if (edit_pos[slot] <= 0) return 0;
    hi = edit_pos[slot];
    lo = utf8_prev(edit_buf[slot] ? edit_buf[slot] : "", hi);
    return edit_delete_range((int)slot, lo, hi);
}

int64_t loam_platform_plat_edit_del(int64_t slot) {
    int lo, hi, n;
    if (!edit_ok(slot)) return 0;
    if (edit_has_sel((int)slot))
        return edit_delete_range((int)slot, edit_lo((int)slot), edit_hi((int)slot));
    n = edit_len[slot];
    if (edit_pos[slot] >= n) return 0;
    lo = edit_pos[slot];
    hi = utf8_next(edit_buf[slot] ? edit_buf[slot] : "", n, lo);
    return edit_delete_range((int)slot, lo, hi);
}

int64_t loam_platform_plat_edit_len(int64_t slot) {
    if (!edit_ok(slot)) return 0;
    return edit_len[slot];
}

loam_str loam_platform_plat_edit_text(int64_t slot) {
    if (!edit_ok(slot) || edit_len[slot] <= 0 || !edit_buf[slot])
        return (loam_str){"", 0};
    return (loam_str){edit_buf[slot], edit_len[slot]};
}

int64_t loam_platform_plat_edit_set(int64_t slot, loam_str text) {
    int n;
    if (!edit_ok(slot)) return 0;
    n = text.len > 0 && text.ptr ? (int)text.len : 0;
    if (n > ZEUS_EDIT_MAX - 1) n = ZEUS_EDIT_MAX - 1;
    if (!edit_grow((int)slot, n + 1)) return 0;
    if (n && text.ptr) memcpy(edit_buf[slot], text.ptr, (size_t)n);
    edit_buf[slot][n] = '\0';
    edit_len[slot] = n;
    edit_pos[slot] = n;
    edit_anchor[slot] = n;
    edit_clear_mark((int)slot);
    return 1;
}

int64_t loam_platform_plat_edit_insert(int64_t slot, loam_str text) {
    int n;
    if (!edit_ok(slot)) return 0;
    n = text.len > 0 && text.ptr ? (int)text.len : 0;
    if (n <= 0) return 0;
    return edit_insert_bytes((int)slot, text.ptr, n);
}

int64_t loam_platform_plat_edit_caret(int64_t slot) {
    if (!edit_ok(slot)) return 0;
    return edit_pos[slot];
}

int64_t loam_platform_plat_edit_anchor(int64_t slot) {
    if (!edit_ok(slot)) return 0;
    return edit_anchor[slot];
}

/* Absolute caret / anchor setters. The Loam side computes grapheme-cluster
   boundaries with `std:unicode` and parks them here, then reuses the existing
   selection delete paths. */
int64_t loam_platform_plat_edit_set_caret(int64_t slot, int64_t pos) {
    if (!edit_ok(slot)) return 0;
    edit_pos[slot] = (int)pos;
    edit_clamp((int)slot);
    return 1;
}

int64_t loam_platform_plat_edit_set_anchor(int64_t slot, int64_t pos) {
    if (!edit_ok(slot)) return 0;
    edit_anchor[slot] = (int)pos;
    edit_clamp((int)slot);
    return 1;
}

int64_t loam_platform_plat_edit_mark(int64_t slot, loam_str text) {
    int n;
    char *p;
    if (!edit_ok(slot)) return 0;
    n = text.len > 0 && text.ptr ? (int)text.len : 0;
    if (n <= 0) {
        edit_clear_mark((int)slot);
        return 1;
    }
    p = (char *)realloc(edit_mark[slot], (size_t)n + 1);
    if (!p) return 0;
    memcpy(p, text.ptr, (size_t)n);
    p[n] = '\0';
    edit_mark[slot] = p;
    edit_mark_len[slot] = n;
    return 1;
}

loam_str loam_platform_plat_edit_shown(int64_t slot) {
    int n, m, at, need;
    char *v;
    if (!edit_ok(slot)) return (loam_str){"", 0};
    n = edit_len[slot];
    m = edit_mark_len[slot];
    if (m <= 0) {
        if (n <= 0 || !edit_buf[slot]) return (loam_str){"", 0};
        return (loam_str){edit_buf[slot], n};
    }
    at = edit_pos[slot];
    if (at < 0) at = 0;
    if (at > n) at = n;
    need = n + m + 1;
    if (need > edit_view_cap[slot]) {
        v = (char *)realloc(edit_view[slot], (size_t)need);
        if (!v) return (loam_str){edit_buf[slot] ? edit_buf[slot] : "", n};
        edit_view[slot] = v;
        edit_view_cap[slot] = need;
    }
    v = edit_view[slot];
    if (at && edit_buf[slot]) memcpy(v, edit_buf[slot], (size_t)at);
    if (m && edit_mark[slot]) memcpy(v + at, edit_mark[slot], (size_t)m);
    if (n - at > 0 && edit_buf[slot]) memcpy(v + at + m, edit_buf[slot] + at, (size_t)(n - at));
    v[n + m] = '\0';
    return (loam_str){v, n + m};
}

void loam_platform_plat_edit_metrics(int64_t slot, int64_t wrap_w, int64_t font) {
    if (!edit_ok(slot)) return;
    edit_wrap_w[slot] = wrap_w > 0 ? (int)wrap_w : 0;
    edit_font[slot] = font > 0 ? (int)font : 14;
}

static int edit_xy_to_pos(int slot, int x, int y, int wrap_w, int font) {
    const char *s = edit_buf[slot] ? edit_buf[slot] : "";
    int n = edit_len[slot], i = 0, best = 0;
    int64_t lh, line_w = 0, yy = 0, sw = 0, sh = 0;
    if (n <= 0) return 0;
    if (wrap_w < 1) wrap_w = 1 << 20;
    if (font < 8) font = 8;
    lh = wrap_line_h(font);
    measure_span(" ", 1, font, &sw, &sh);
    if (y < 0) y = 0;
    while (i < n) {
        int64_t ww = 0, wh = 0;
        int j, adv;
        if (s[i] == '\n') {
            if (y < yy + lh) return i;
            yy += lh;
            line_w = 0;
            i++;
            best = i;
            continue;
        }
        j = i;
        while (j < n && s[j] != ' ' && s[j] != '\t' && s[j] != '\n') j++;
        if (j == i) {
            measure_span(s + i, 1, font, &ww, &wh);
            adv = utf8_next(s, n, i) - i;
            if (adv < 1) adv = 1;
            j = i + adv;
        } else {
            measure_span(s + i, j - i, font, &ww, &wh);
        }
        if (line_w > 0 && line_w + ww > wrap_w) {
            if (y < yy + lh) return i;
            yy += lh;
            line_w = 0;
        }
        if (y < yy + lh) {
            int k = i;
            int64_t cx = line_w;
            while (k < j) {
                int nk = utf8_next(s, n, k);
                int64_t cw = 0, ch = 0;
                measure_span(s + i, nk - i, font, &cw, &ch);
                if (x < line_w + cw) {
                    int64_t mid = (cx + line_w + cw) / 2;
                    return x < mid ? k : nk;
                }
                cx = line_w + cw;
                k = nk;
            }
            return j;
        }
        line_w += ww;
        i = j;
        best = i;
    }
    return best;
}

static void edit_pos_to_xy(int slot, int pos, int wrap_w, int font, int64_t *ox, int64_t *oy) {
    const char *s = edit_buf[slot] ? edit_buf[slot] : "";
    int n = edit_len[slot], i = 0;
    int64_t lh, line_w = 0, yy = 0, sw = 0, sh = 0;
    if (pos < 0) pos = 0;
    if (pos > n) pos = n;
    if (wrap_w < 1) wrap_w = 1 << 20;
    if (font < 8) font = 8;
    lh = wrap_line_h(font);
    measure_span(" ", 1, font, &sw, &sh);
    while (i < pos) {
        int64_t ww = 0, wh = 0;
        int j;
        if (s[i] == '\n') {
            yy += lh;
            line_w = 0;
            i++;
            continue;
        }
        j = i;
        while (j < n && j < pos && s[j] != ' ' && s[j] != '\t' && s[j] != '\n') j++;
        if (j == i) j = utf8_next(s, n, i);
        if (j > pos) j = pos;
        measure_span(s + i, j - i, font, &ww, &wh);
        if (line_w > 0 && line_w + ww > wrap_w) {
            yy += lh;
            line_w = 0;
        }
        line_w += ww;
        i = j;
    }
    if (ox) *ox = line_w;
    if (oy) *oy = yy;
}

int64_t loam_platform_plat_edit_click(int64_t slot, int64_t x, int64_t y, int64_t wrap_w,
                                      int64_t font) {
    int p;
    if (!edit_ok(slot)) return 0;
    p = edit_xy_to_pos((int)slot, (int)x, (int)y, (int)wrap_w, (int)font);
    edit_pos[slot] = p;
    edit_anchor[slot] = p;
    edit_clamp((int)slot);
    return 1;
}

int64_t loam_platform_plat_edit_caret_x(int64_t slot) {
    int64_t x = 0, y = 0;
    if (!edit_ok(slot)) return 0;
    edit_pos_to_xy((int)slot, edit_pos[slot], edit_wrap_w[slot] ? edit_wrap_w[slot] : (1 << 20),
                   edit_font[slot] ? edit_font[slot] : 14, &x, &y);
    return x;
}

int64_t loam_platform_plat_edit_caret_y(int64_t slot) {
    int64_t x = 0, y = 0;
    if (!edit_ok(slot)) return 0;
    edit_pos_to_xy((int)slot, edit_pos[slot], edit_wrap_w[slot] ? edit_wrap_w[slot] : (1 << 20),
                   edit_font[slot] ? edit_font[slot] : 14, &x, &y);
    return y;
}

int64_t loam_platform_plat_edit_line_h(int64_t slot) {
    int font;
    if (!edit_ok(slot)) return 18;
    font = edit_font[slot] ? edit_font[slot] : 14;
    return wrap_line_h(font);
}

int64_t loam_platform_plat_edit_move(int64_t slot, int64_t dir, int64_t extend) {
    int n, wrap_w, font, p;
    int64_t x = 0, y = 0, lh;
    if (!edit_ok(slot)) return 0;
    n = edit_len[slot];
    wrap_w = edit_wrap_w[slot];
    font = edit_font[slot] ? edit_font[slot] : 14;
    p = edit_pos[slot];
    if (dir == -1) p = utf8_prev(edit_buf[slot] ? edit_buf[slot] : "", p);
    else if (dir == 1) p = utf8_next(edit_buf[slot] ? edit_buf[slot] : "", n, p);
    else if (dir == -2) {
        while (p > 0 && edit_buf[slot] && edit_buf[slot][p - 1] != '\n') p--;
    } else if (dir == 2) {
        while (p < n && edit_buf[slot] && edit_buf[slot][p] != '\n') p++;
    } else if (dir == -3 || dir == 3) {
        lh = wrap_line_h(font);
        edit_pos_to_xy((int)slot, p, wrap_w ? wrap_w : (1 << 20), font, &x, &y);
        y += dir == 3 ? lh : -lh;
        if (y < 0) y = 0;
        p = edit_xy_to_pos((int)slot, (int)x, (int)y, wrap_w ? wrap_w : (1 << 20), font);
    } else
        return 0;
    edit_pos[slot] = p;
    if (!extend) edit_anchor[slot] = p;
    edit_clamp((int)slot);
    return 1;
}

/* Drop the text of a slot (keeps the buffer) so a recycled slot starts
   empty. Called when the input node holding the slot is torn down. */
void loam_platform_plat_edit_reset(int64_t slot) {
    if (!edit_ok(slot)) return;
    if (edit_buf[slot]) edit_buf[slot][0] = '\0';
    edit_len[slot] = 0;
    edit_pos[slot] = 0;
    edit_anchor[slot] = 0;
    edit_clear_mark((int)slot);
}

void loam_platform_plat_measure(loam_str s, int32_t px, int32_t *w, int32_t *h) {
    loam_zeus_plat_measure(s, px, w, h);
}
void loam_platform_plat_measure_int(int32_t v, int32_t px, int32_t *w, int32_t *h) {
    loam_zeus_plat_measure_int(v, px, w, h);
}
void loam_platform_plat_measure_wrap(loam_str s, int32_t px, int32_t max_w, int32_t *w,
                                     int32_t *h) {
    loam_zeus_plat_measure_wrap(s, px, max_w, w, h);
}
void loam_platform_plat_text_wrap(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font,
                                  int64_t max_w) {
    loam_zeus_plat_text_wrap(x, y, s, rgb, font, max_w);
}
void loam_platform_plat_set_window(loam_str title, int64_t width, int64_t height) {
    loam_zeus_plat_set_window(title, width, height);
}

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("set_title")))
void zeus_js_set_title(const char *t);
#endif

/* Retitle the open window without reopening it (router `Meta.title`). */
void loam_platform_plat_set_title(loam_str title) {
#ifdef __wasm32__
    static char buf[512];
    size_t n = (title.len > 0 && title.ptr) ? (size_t)title.len : 0;
    if (n >= sizeof buf) n = sizeof buf - 1;
    if (n) memcpy(buf, title.ptr, n);
    buf[n] = '\0';
    zeus_js_set_title(buf);
#else
    free(win_title);
    win_title = dup_ys(title);
    if (plat_title_fn) plat_title_fn(win_title ? win_title : "");
#endif
}
int64_t loam_platform_plat_view_width(void) { return loam_zeus_plat_view_width(); }
int64_t loam_platform_plat_view_height(void) { return loam_zeus_plat_view_height(); }

int64_t loam_platform_plat_overlay_scroll(void) {
    return loam_zeus_plat_overlay_scroll();
}
void loam_platform_plat_set_overlay_scroll(int64_t on) {
    zeus_set_overlay_scroll(on);
}
int64_t loam_platform_plat_inset_top(void) { return loam_zeus_plat_inset_top(); }
int64_t loam_platform_plat_inset_right(void) { return loam_zeus_plat_inset_right(); }
int64_t loam_platform_plat_inset_bottom(void) { return loam_zeus_plat_inset_bottom(); }
int64_t loam_platform_plat_inset_left(void) { return loam_zeus_plat_inset_left(); }
void loam_platform_plat_set_insets(int64_t top, int64_t right, int64_t bottom,
                                  int64_t left) {
    zeus_set_insets(top, right, bottom, left);
}

/* Interned handlers and reactive prop thunks share this table. A page of
   declarative widgets interns one entry per prop, so it grows rather than
   capping — a full table used to return 0 and silently drop every prop past
   the limit.

   Each slot also carries the SIGNATURE it was interned under, which is the
   difference between a defined refusal and undefined behaviour. The four
   `intern_*` entry points all land in this one table and all look like a bare
   `loam_fn` to C, so nothing in the value says which kind it is; the four
   `invoke_*` entry points used to call whatever `fn` they found as their own
   type. A slot holding another kind then returned garbage on native — a silent
   bug that surfaced as a jittering UI, because the garbage was written straight
   back into a signal — and trapped outright on wasm, which type-checks indirect
   calls ("null function or function signature mismatch"). The kind makes that a
   counted, loud refusal instead. */
typedef struct {
    loam_fn h;
    unsigned char kind;
} ZeusInterned;

#define ZEUS_FN_VOID 1
#define ZEUS_FN_INT 2
#define ZEUS_FN_BOOL 3
#define ZEUS_FN_STR 4

static ZeusInterned *click_fns;
static int nclick_fns = 1;
static int click_fns_cap;
/* Refusals to call a slot through the wrong signature. Zero is the contract. */
static int64_t intern_kind_mismatches;
static int intern_kind_reported;

/* Freed interned-handler slots, recycled by plat_intern_fn. A slot is freed
   only when the subtree that interned it is torn down, so recycling never
   aliases a live handler. */
static int *free_fns;
static int nfree_fns;
static int cfree_fns;

static void free_fn_push(int id) {
    if (nfree_fns == cfree_fns) {
        int cap = cfree_fns ? cfree_fns * 2 : 256;
        int *next = (int *)realloc(free_fns, (size_t)cap * sizeof(int));
        if (!next) {
            fprintf(stderr, "zeus: out of memory\n");
            abort();
        }
        free_fns = next;
        cfree_fns = cap;
    }
    free_fns[nfree_fns++] = id;
}

static int free_fn_pop(void) {
    if (nfree_fns <= 0) return 0;
    return free_fns[--nfree_fns];
}

static int intern_fn_grow(void) {
    if (nclick_fns < click_fns_cap) return 1;
    int cap = click_fns_cap ? click_fns_cap * 2 : 2048;
    ZeusInterned *next = (ZeusInterned *)realloc(click_fns, (size_t)cap * sizeof(ZeusInterned));
    if (!next) return 0;
    memset(next + click_fns_cap, 0, (size_t)(cap - click_fns_cap) * sizeof(ZeusInterned));
    click_fns = next;
    click_fns_cap = cap;
    return 1;
}

static void intern_fn_reset(void) {
    int i;
    for (i = 1; i < nclick_fns; i++) {
        free(click_fns[i].h.env);
        click_fns[i].h.fn = NULL;
        click_fns[i].h.env = NULL;
        click_fns[i].h.env_size = 0;
        click_fns[i].kind = 0;
    }
    nclick_fns = 1;
    nfree_fns = 0;
}

/* Refuse a call through a signature the slot was not interned with. Counted,
   and said out loud the first few times: a guard that hides a real aliasing
   bug is worse than the bug, and silence here would be easy to miss. */
static int intern_kind_matches(int id, int want) {
    if ((int)click_fns[id].kind == want) return 1;
    intern_kind_mismatches++;
    if (intern_kind_reported < 8) {
        intern_kind_reported++;
        fprintf(stderr,
                "zeus: interned handler %d invoked as kind %d but interned as kind %d"
                " — refusing the call\n",
                id, want, (int)click_fns[id].kind);
    }
    return 0;
}

/* Free one interned handler; its id becomes recyclable. */
void loam_platform_plat_intern_free(int64_t id) {
    if (id <= 0 || id >= nclick_fns) return;
    if (!click_fns[id].h.fn && !click_fns[id].h.env) return;
    free(click_fns[id].h.env);
    click_fns[id].h.fn = NULL;
    click_fns[id].h.env = NULL;
    click_fns[id].h.env_size = 0;
    /* Clear the kind with the slot: a recycled slot must not answer to the
       signature the previous occupant was interned under. */
    click_fns[id].kind = 0;
    free_fn_push((int)id);
}

int64_t loam_platform_plat_intern_fn(loam_fn handler) {
    loam_fn kept;
    int id = free_fn_pop();
    if (id == 0) {
        if (!intern_fn_grow()) return 0;
        id = nclick_fns++;
    }
    kept = handler;
    /* Snapshot the env. The Loam value is often a field that is dropped when
       the props struct returns, which would free the original and leave the
       interned click/styled handler dangling. */
    if (handler.env && handler.env_size > 0) {
        void *copy = malloc(handler.env_size);
        if (copy) {
            memcpy(copy, handler.env, handler.env_size);
            kept.env = copy;
        }
    }
    click_fns[id].h = kept;
    click_fns[id].kind = ZEUS_FN_VOID;
    return id;
}

/* Reactive prop thunks share the interned-handler table; only the call
   signature differs. Interning yields an int a `||` closure can capture.
   The kind is recorded with the slot, so the matching `invoke_*` can tell its
   own thunks from the other three kinds. */
static int64_t intern_fn_as(loam_fn thunk, int kind) {
    int64_t id = loam_platform_plat_intern_fn(thunk);
    if (id > 0 && id < nclick_fns) click_fns[id].kind = (unsigned char)kind;
    return id;
}

int64_t loam_platform_plat_intern_int_fn(loam_fn thunk) {
    return intern_fn_as(thunk, ZEUS_FN_INT);
}

int64_t loam_platform_plat_invoke_int_fn(int64_t id) {
    if (id <= 0 || id >= nclick_fns) return 0;
    if (!click_fns[id].h.fn) return 0;
    if (!intern_kind_matches((int)id, ZEUS_FN_INT)) return 0;
    /* The thunk is a Loam `fn() -> int`, and Loam's `int` is i32 — so the
       closure RETURNS int32_t. Casting it to an `int64_t (*)(void *)` is the
       bug this comment exists to prevent: it is undefined behaviour that hands
       back the callee's stale high bits on native (a garbage prop value, which
       is how the UI came to jitter while scrolling or dragging) and traps
       outright on wasm, which type-checks indirect calls ("null function or
       function signature mismatch"). Cast to the signature the closure really
       has, and widen the result afterwards. */
    return (int64_t)((int32_t (*)(void *))click_fns[id].h.fn)(click_fns[id].h.env);
}

int64_t loam_platform_plat_intern_bool_fn(loam_fn thunk) {
    return intern_fn_as(thunk, ZEUS_FN_BOOL);
}

int64_t loam_platform_plat_invoke_bool_fn(int64_t id) {
    if (id <= 0 || id >= nclick_fns) return 0;
    if (!click_fns[id].h.fn) return 0;
    if (!intern_kind_matches((int)id, ZEUS_FN_BOOL)) return 0;
    return ((bool (*)(void *))click_fns[id].h.fn)(click_fns[id].h.env) ? 1 : 0;
}

int64_t loam_platform_plat_intern_str_fn(loam_fn thunk) {
    return intern_fn_as(thunk, ZEUS_FN_STR);
}

loam_str loam_platform_plat_invoke_str_fn(int64_t id) {
    if (id <= 0 || id >= nclick_fns) return (loam_str){ "", 0 };
    if (!click_fns[id].h.fn) return (loam_str){ "", 0 };
    if (!intern_kind_matches((int)id, ZEUS_FN_STR)) return (loam_str){ "", 0 };
    return ((loam_str (*)(void *))click_fns[id].h.fn)(click_fns[id].h.env);
}

void loam_platform_plat_invoke_fn(int64_t id) {
    if (id <= 0 || id >= nclick_fns) return;
    if (!click_fns[id].h.fn) return;
    if (!intern_kind_matches((int)id, ZEUS_FN_VOID)) return;
    ((void (*)(void *))click_fns[id].h.fn)(click_fns[id].h.env);
}

/* How many calls were refused because the slot held another signature. Zero is
   the contract: a non-zero count means a handler was interned as one kind and
   invoked as another, which is the bug this guard exists to make visible. */
int32_t loam_platform_plat_intern_kind_mismatch_count(void) {
    return (int32_t)intern_kind_mismatches;
}

/* Live rows in the interned-handler table: a slot is live while it carries a
   kind. A streaming list, or a re-running `scope`, must recycle handlers rather
   than grow this, so a test can hold it flat. */
int32_t loam_platform_plat_intern_live_count(void) {
    int i, n = 0;
    for (i = 1; i < nclick_fns; i++) {
        if (click_fns[i].kind != 0) n++;
    }
    return (int32_t)n;
}

const char *zeus_window_title(void) { return win_title ? win_title : ""; }
int64_t zeus_window_width(void) { return win_w; }
int64_t zeus_window_height(void) { return win_h; }

void zeus_layout(int64_t width, int64_t height) {
    loam_zeus_engine_layout(width, height);
}

int zeus_step(float dt) {
    int32_t ms = (int32_t)(dt * 1000.0f + 0.5f);
    if (ms <= 0) ms = 16;
    return (int)loam_zeus_engine_step_dt(ms);
}

void zeus_bind_draw(ZeusDraw draw) {
    paint_ctx = NULL;
    paint_draw = draw;
    have_draw = 1;
}

void zeus_paint(void *ctx, ZeusDraw draw) {
    paint_ctx = ctx;
    paint_draw = draw;
    have_draw = 1;
    loam_zeus_engine_paint();
    have_draw = 0;
}

int zeus_handle_click(int64_t x, int64_t y) {
    return (int)loam_zeus_engine_click(x, y);
}

int zeus_handle_hover(int64_t x, int64_t y) {
    return (int)loam_zeus_engine_hover(x, y);
}

int zeus_over_button(void) { return (int)loam_zeus_engine_over_button(); }

/* CSS cursor name under the pointer; "" or unknown = default arrow. */
const char *zeus_cursor(void) {
    static char buf[64];
    loam_str s = loam_zeus_engine_cursor();
    size_t n = s.len < 0 ? 0 : (size_t)s.len;
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    if (n && s.ptr) memcpy(buf, s.ptr, n);
    buf[n] = '\0';
    return buf;
}

int zeus_handle_scroll(int64_t x, int64_t y, int64_t dx, int64_t dy) {
    return (int)loam_zeus_engine_scroll(x, y, dx, dy);
}

/* Discrete scroll step (mouse wheel notch, web line/page wheel): applies the
 * delta without velocity, so the scroller never coasts past the click. */
int zeus_handle_scroll_smooth(int64_t x, int64_t y, int64_t dx, int64_t dy) {
    return (int)loam_zeus_engine_scroll_smooth(x, y, dx, dy);
}

int zeus_handle_scroll_step(int64_t x, int64_t y, int64_t dx, int64_t dy) {
    return (int)loam_zeus_engine_scroll_step(x, y, dx, dy);
}

int zeus_handle_drag(int64_t x, int64_t y) {
    return (int)loam_zeus_engine_drag(x, y);
}

void zeus_handle_mouseup(void) { loam_zeus_engine_mouseup(); }

int zeus_handle_key(int key) {
    return (int)loam_zeus_engine_key(key);
}

int zeus_handle_text(const char *utf8, int n) {
    loam_str s;
    if (!utf8) utf8 = "";
    if (n < 0) n = (int)strlen(utf8);
    s.ptr = utf8;
    s.len = n;
    return (int)loam_zeus_engine_insert(s);
}

int zeus_handle_marked(const char *utf8, int n) {
    loam_str s;
    if (!utf8) utf8 = "";
    if (n < 0) n = (int)strlen(utf8);
    s.ptr = utf8;
    s.len = n;
    return (int)loam_zeus_engine_marked(s);
}

int zeus_handle_key_up(int key, int mods) {
    return (int)loam_zeus_engine_key_up(key, mods);
}

int zeus_handle_key_ev(int key, int mods) {
    loam_zeus_engine_set_mods((int64_t)mods);
    if (zeus_key_dispatch(key, mods)) return 1;
    /* Plain Tab / Shift-Tab: the engine decides — step the focus ring, or
       insert a tab when a multiline field is focused (single-line fields
       step focus like the web). Keymaps win via dispatch above. */
    if (key == ZEUS_K_TAB && !(mods & ~ZEUS_MOD_SHIFT))
        return zeus_handle_key(key);
    /* Cmd/Ctrl chords usually belong to the host (menus, reload). A focused
       text field still needs Cmd/Ctrl+A to select all so Backspace can clear. */
    if (mods & ~ZEUS_MOD_SHIFT) {
        if (zeus_focus_captures_text())
            return zeus_handle_key(key);
        return 0;
    }
    return zeus_handle_key(key);
}

int zeus_focus_chain(int *nodes_out, int *ctxs, int max) {
    loam_zeus_engine_fill_focus();
    int n = (int)loam_zeus_engine_focus_depth();
    if (n > max) n = max;
    if (n <= 0) {
        if (nodes_out) nodes_out[0] = 0;
        if (ctxs) ctxs[0] = 0;
        return 1;
    }
    for (int i = 0; i < n; i++) {
        if (nodes_out) nodes_out[i] = (int)loam_zeus_engine_focus_node(i);
        if (ctxs) ctxs[i] = (int)loam_zeus_engine_focus_ctx(i);
    }
    return n;
}

void zeus_key_apply(int sig, int mode, int64_t value, int64_t lo, int64_t hi) {
    loam_zeus_engine_key_apply((int64_t)sig, (int64_t)mode, value, lo, hi);
}

int zeus_focus_captures_text(void) {
    return (int)loam_zeus_engine_focus_captures_text();
}

int zeus_focus_step(int back) {
    return (int)loam_zeus_engine_focus_step(back);
}

Node loam_zeusbase_on_action(Node node, loam_str action, Signal sig, int64_t mode, int64_t value) {
    char *s = dup_ys(action);
    zeus_key_on_action((int)node.id, s, (int)sig.id, (int)mode, value);
    free(s);
    return node;
}

Node loam_zeusbase_on_range(Node node, loam_str action, Signal sig, int64_t delta, int64_t lo,
                       int64_t hi) {
    char *s = dup_ys(action);
    zeus_key_on_range((int)node.id, s, (int)sig.id, delta, lo, hi);
    free(s);
    return node;
}

void loam_zeusbase_on_action_global(loam_str action, Signal sig, int64_t mode, int64_t value) {
    char *s = dup_ys(action);
    zeus_key_on_action(0, s, (int)sig.id, (int)mode, value);
    free(s);
}

void loam_zeusbase_map_key(loam_str spec, loam_str action, loam_str ctx) {
    char *a = dup_ys(spec), *b = dup_ys(action), *c = dup_ys(ctx);
    zeus_key_map(a, b, c);
    free(a);
    free(b);
    free(c);
}

void loam_zeusbase_remap_key(loam_str spec, loam_str action, loam_str ctx) {
    char *a = dup_ys(spec), *b = dup_ys(action), *c = dup_ys(ctx);
    zeus_key_remap(a, b, c);
    free(a);
    free(b);
    free(c);
}

/* --- recoverable traps (zeus.Boundary) ---
   `loam_panic` longjmps here when a boundary is installed. The trap state
   (`loam_jmp_top`, `loam_jmp_msg`) is defined once in the generated program
   and reached through the declarations in loam_rt.h. */

/* Run `build` under a recoverable trap. Returns "" if it completed, else the
   panic message. On wasm there is no setjmp, so the build runs unprotected and
   a trap aborts as it always has. */
loam_str loam_platform_plat_boundary(loam_fn build) {
    loam_str none = { "", 0 };
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
        loam_str msg = { loam_jmp_msg, (int64_t)strlen(loam_jmp_msg) };
        return msg;
    }
#else
    if (build.fn) ((void (*)(void *))build.fn)(build.env);
    return none;
#endif
}

/* Router host entry points. A build that never imports `std:router` has no
   strong definition, so the web host's `zeus_open_url` still links; importing
   the router overrides these weak no-ops. */
__attribute__((weak)) void loam_router_engine_open_url(loam_str url) { (void)url; }
__attribute__((weak)) void loam_router_engine_history_back(void) {}

/* Router history mirror. The Loam-side stack is authoritative; these mirror a
   push / replace / back into the host's own affordance. The web host forwards
   to `history.pushState` / `replaceState` / `back`; other hosts ignore them
   until wired. */
#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("history_push")))
void zeus_js_history_push(const char *path);
__attribute__((import_module("zeus"), import_name("history_replace")))
void zeus_js_history_replace(const char *path);
__attribute__((import_module("zeus"), import_name("history_back")))
void zeus_js_history_back(void);
#endif

static const char *history_path(loam_str path, char *buf, size_t cap) {
    size_t n = (path.len > 0 && path.ptr) ? (size_t)path.len : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(buf, path.ptr, n);
    buf[n] = '\0';
    return buf;
}

void loam_platform_plat_history_push(loam_str path) {
#ifdef __wasm32__
    char buf[1024];
    zeus_js_history_push(history_path(path, buf, sizeof buf));
#else
    (void)path;
#endif
}
void loam_platform_plat_history_replace(loam_str path) {
#ifdef __wasm32__
    char buf[1024];
    zeus_js_history_replace(history_path(path, buf, sizeof buf));
#else
    (void)path;
#endif
}
void loam_platform_plat_history_back(void) {
#ifdef __wasm32__
    zeus_js_history_back();
#endif
}
