/* zeus_plat.c — Cocoa/headless seam for the Yuga zeus library.
 *
 * std/zeus.yuga owns the node arena, layout, paint, and hit-test. This file
 * is the other side of that seam: window title/size, text measure, present,
 * and the zeus_* entry points the Cocoa/iOS/web hosts already call. Keyboard chords live
 * in zeus_key.c; focus chain and Tab walk the Yuga node tree.
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

extern yuga_vec yuga_arena_sigs;
/* Rebuild-scope ownership (arena.yuga): while `scope_node` is non-zero, a
   signal allocation is recorded against it so teardown can recycle the id. */
extern int64_t yuga_arena_scope_node;
extern yuga_vec yuga_arena_rec_sid;
extern yuga_vec yuga_arena_rec_owner;
void yuga_arena_ensure(void);
void yuga_arena_store_sig(int64_t id, int64_t value);
void yuga_track_note_read(int64_t sid);
void yuga_track_notify(int64_t sid);

typedef struct {
    void *p;
    size_t n;
    int64_t gen;
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

void yuga_zeus_sig_bind(int64_t id, const void *src, int64_t n) {
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
    c->gen++;
}

void yuga_zeus_sig_load(int64_t id, void *dst, int64_t n) {
    size_t nn, k;
    if (!dst || n <= 0) return;
    nn = (size_t)n;
    memset(dst, 0, nn);
    if (id < 0 || (size_t)id >= g_ncells) return;
    if (!g_cells[id].p) return;
    k = g_cells[id].n < nn ? g_cells[id].n : nn;
    memcpy(dst, g_cells[id].p, k);
}

int64_t yuga_zeus_sig_changed(int64_t id, const void *src, int64_t n) {
    if (id < 0 || !src || n <= 0) return 1;
    if ((size_t)id >= g_ncells || !g_cells[id].p || g_cells[id].n != (size_t)n)
        return 1;
    return memcmp(g_cells[id].p, src, (size_t)n) != 0;
}

int64_t yuga_zeus_sig_gen(int64_t id) {
    if (id < 0 || (size_t)id >= g_ncells) return 0;
    return g_cells[id].gen;
}

static int64_t sig_slot_zero(void);

/* Captured-`let mut` (state) signals: `signal(0)`-style decls inside a
   rebuild scope must be recycled and freed like every other signal, or a
   chart refit leaks a slot per captured mutable (zone strips, bar heights,
   ...). sig_slot_zero recycles freed ids and records the scope owner, so
   arena teardown (release_sigs) frees them with the rest of the subtree. */
Signal yuga_zeus_signal(int64_t value) {
    Signal s;
    yuga_arena_ensure();
    s.id = sig_slot_zero();
    ((int64_t *)yuga_arena_sigs.ptr)[s.id] = value;
    yuga_zeus_sig_bind(s.id, &value, (int64_t)sizeof(value));
    return s;
}

int64_t yuga_zeus_get(Signal sig) {
    int64_t v = 0;
    yuga_arena_ensure();
    yuga_track_note_read(sig.id);
    yuga_zeus_sig_load(sig.id, &v, (int64_t)sizeof(v));
    return v;
}

void yuga_zeus_set(Signal sig, int64_t value) {
    yuga_arena_ensure();
    yuga_arena_store_sig(sig.id, value);
}

void yuga_platform_plat_sig_bind_int(int64_t id, int64_t value) {
    yuga_zeus_sig_bind(id, &value, (int64_t)sizeof(value));
}

int64_t yuga_platform_plat_sig_gen(int64_t id) {
    return yuga_zeus_sig_gen(id);
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
   scope node so the yuga teardown can free them. */
static int64_t sig_slot_zero(void) {
    int64_t id, zero = 0;
    yuga_arena_ensure();
    id = free_sig_pop();
    if (id >= 0) {
        ((int64_t *)yuga_arena_sigs.ptr)[id] = 0;
    } else {
        yuga_vec_push(&yuga_arena_sigs, &zero, sizeof(int64_t), __FILE__, __LINE__);
        id = yuga_arena_sigs.len - 1;
    }
    if (yuga_arena_scope_node > 0) {
        yuga_vec_push(&yuga_arena_rec_sid, &id, sizeof(int64_t), __FILE__, __LINE__);
        yuga_vec_push(&yuga_arena_rec_owner, &yuga_arena_scope_node, sizeof(int64_t),
                      __FILE__, __LINE__);
    }
    return id;
}

int64_t yuga_zeus_sig_alloc_int(int64_t value) {
    int64_t id = sig_slot_zero();
    ((int64_t *)yuga_arena_sigs.ptr)[id] = value;
    yuga_zeus_sig_bind(id, &value, sizeof(value));
    return id;
}

int64_t yuga_zeus_sig_alloc_zero(void) {
    return sig_slot_zero();
}

void yuga_zeus_sig_free(int64_t id) {
    if (id <= 0 || (size_t)id >= g_ncells) return;
    if (!g_cells[id].p && g_cells[id].n == 0 && g_cells[id].gen == 0) return;
    if (id < yuga_arena_sigs.len) ((int64_t *)yuga_arena_sigs.ptr)[id] = 0;
    free(g_cells[id].p);
    g_cells[id].p = NULL;
    g_cells[id].n = 0;
    g_cells[id].gen = 0;
    free_sig_push(id);
}

void yuga_platform_plat_sig_free(int64_t id) {
    yuga_zeus_sig_free(id);
}

static void (*plat_run)(void);
static void (*plat_measure)(const char *s, int64_t px, int64_t *w, int64_t *h);
static void (*plat_redraw)(void);
static void (*plat_pick_image)(char *out, int cap, int64_t *w, int64_t *h);

static char *win_title;
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

static char *dup_ys(yuga_str s) {
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

static yuga_str pick_dup(const char *src) {
    size_t n = src ? strlen(src) : 0;
    char *p;
    if (n == 0) return (yuga_str){"", 0};
    p = (char *)yuga_new(n + 1, "pick_image", 0);
    memcpy(p, src, n);
    p[n] = 0;
    return (yuga_str){p, (int64_t)n};
}

yuga_str yuga_zeus_plat_pick_image(int64_t *w, int64_t *h) {
    char buf[4096];
    buf[0] = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    if (plat_pick_image) plat_pick_image(buf, (int)sizeof buf, w, h);
    return pick_dup(buf);
}

void zeus_picked_image(const char *src, int64_t w, int64_t h) {
    yuga_zeus_engine_picked_image(pick_dup(src), w, h);
    if (plat_redraw) plat_redraw();
}

void yuga_zeus_plat_run(void) {
    if (plat_run) plat_run();
}

int64_t yuga_zeus_plat_headless(void) {
    if (getenv("ZEUS_HEADLESS") || !plat_run) return 1;
    return 0;
}

void yuga_zeus_plat_fill(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                        int64_t radius) {
    if (!have_draw || !paint_draw.fill) return;
    paint_draw.fill(paint_ctx, x, y, w, h, rgb & 0xFFFFFF, radius);
}

void yuga_zeus_plat_fill_a(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                          int64_t radius, int64_t alpha) {
    if (!have_draw) return;
    if (paint_draw.fill_a)
        paint_draw.fill_a(paint_ctx, x, y, w, h, rgb & 0xFFFFFF, radius, alpha);
    else if (paint_draw.fill)
        paint_draw.fill(paint_ctx, x, y, w, h, rgb & 0xFFFFFF, radius);
}

void yuga_zeus_plat_fill_g(int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                          int64_t c1, int64_t axis) {
    if (!have_draw) return;
    if (paint_draw.fill_g)
        paint_draw.fill_g(paint_ctx, x, y, w, h, c0 & 0xFFFFFF, c1 & 0xFFFFFF, axis);
    else if (paint_draw.fill)
        paint_draw.fill(paint_ctx, x, y, w, h, c0 & 0xFFFFFF, 0);
}

void yuga_zeus_plat_text(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font) {
    char *p;
    if (!have_draw || !paint_draw.text) return;
    p = dup_ys(s);
    paint_draw.text(paint_ctx, x, y, p, rgb & 0xFFFFFF, font);
    free(p);
}

void yuga_zeus_plat_text_rot(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font,
                             int64_t deg) {
    char *p;
    if (!have_draw) return;
    if (!paint_draw.text_rot) {
        if (paint_draw.text) yuga_zeus_plat_text(x, y, s, rgb, font);
        return;
    }
    p = dup_ys(s);
    paint_draw.text_rot(paint_ctx, x, y, p, rgb & 0xFFFFFF, font, deg);
    free(p);
}

void yuga_zeus_plat_text_int(int64_t x, int64_t y, int64_t v, int64_t rgb, int64_t font) {
    char buf[32];
    if (!have_draw || !paint_draw.text) return;
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    paint_draw.text(paint_ctx, x, y, buf, rgb & 0xFFFFFF, font);
}

void yuga_zeus_plat_svg(int64_t x, int64_t y, int64_t w, int64_t h, yuga_str markup,
                       int64_t rgb, int64_t alpha) {
    char *p;
    if (!have_draw || !paint_draw.svg) return;
    p = dup_ys(markup);
    paint_draw.svg(paint_ctx, x, y, w, h, p, rgb & 0xFFFFFF, alpha);
    free(p);
}

void yuga_zeus_plat_image(int64_t x, int64_t y, int64_t w, int64_t h, yuga_str src,
                         int64_t radius, int64_t alpha, int64_t fit) {
    char *p;
    if (!have_draw || !paint_draw.image) return;
    p = dup_ys(src);
    paint_draw.image(paint_ctx, x, y, w, h, p, radius, alpha, fit);
    free(p);
}

void yuga_zeus_plat_save(void) {
    if (have_draw && paint_draw.save) paint_draw.save(paint_ctx);
}

void yuga_zeus_plat_clip(int64_t x, int64_t y, int64_t w, int64_t h) {
    if (have_draw && paint_draw.clip) paint_draw.clip(paint_ctx, x, y, w, h);
}

void yuga_zeus_plat_restore(void) {
    if (have_draw && paint_draw.restore) paint_draw.restore(paint_ctx);
}

void yuga_zeus_plat_measure(yuga_str s, int64_t px, int64_t *w, int64_t *h) {
    char *p = dup_ys(s);
    int64_t tw = 0, th = 0;
    if (plat_measure) plat_measure(p, px, &tw, &th);
    else measure_default(p, px, &tw, &th);
    if (w) *w = tw;
    if (h) *h = th;
    free(p);
}

void yuga_zeus_plat_measure_int(int64_t v, int64_t px, int64_t *w, int64_t *h) {
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    if (plat_measure) plat_measure(buf, px, w, h);
    else measure_default(buf, px, w, h);
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

void yuga_zeus_plat_measure_wrap(yuga_str s, int64_t px, int64_t max_w, int64_t *w, int64_t *h) {
    wrap_text(s.ptr, s.len, px, max_w, 0, 0, 0, 0, w, h);
}

void yuga_zeus_plat_text_wrap(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font,
                             int64_t max_w) {
    wrap_text(s.ptr, s.len, font, max_w, 1, x, y, rgb, NULL, NULL);
}

void yuga_zeus_plat_set_window(yuga_str title, int64_t width, int64_t height) {
    free(win_title);
    win_title = dup_ys(title);
#if defined(YUGA_IOS) || defined(YUGA_ANDROID)
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

int64_t yuga_zeus_plat_view_width(void) {
#ifdef __wasm32__
    int32_t w = zeus_js_view_w();
    if (w > 0) return (int64_t)w;
#endif
    /* Desktop: before the window exists, report the size the host will open
       it at (full device screen) instead of the 640×480 placeholder, so an
       app that passes zeus.window_size() to zeus.App opens screen-filling. */
    if (!opened_window && !yuga_zeus_plat_headless() && plat_initial_w) {
        int64_t w = plat_initial_w();
        if (w > 0) return w;
    }
    return win_w;
}

int64_t yuga_zeus_plat_view_height(void) {
#ifdef __wasm32__
    int32_t h = zeus_js_view_h();
    if (h > 0) return (int64_t)h;
#endif
    if (!opened_window && !yuga_zeus_plat_headless() && plat_initial_h) {
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

int64_t yuga_zeus_plat_overlay_scroll(void) {
#if defined(YUGA_IOS) || defined(YUGA_ANDROID)
    return 1;
#else
    return 0;
#endif
}

int64_t yuga_zeus_plat_inset_top(void) { return g_inset_t; }
int64_t yuga_zeus_plat_inset_right(void) { return g_inset_r; }
int64_t yuga_zeus_plat_inset_bottom(void) { return g_inset_b; }
int64_t yuga_zeus_plat_inset_left(void) { return g_inset_l; }

/* std/zeus/platform.yuga is the GPUI-style platform leaf. Empty Yuga stubs
   compile to yuga_platform_plat_*; the Cocoa implementations stay as
   yuga_zeus_plat_* so existing callers do not change. */
void yuga_platform_plat_run(void) { yuga_zeus_plat_run(); }
int64_t yuga_platform_plat_headless(void) { return yuga_zeus_plat_headless(); }
void yuga_platform_plat_fill(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                             int64_t radius) {
    yuga_zeus_plat_fill(x, y, w, h, rgb, radius);
}
void yuga_platform_plat_fill_a(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                               int64_t radius, int64_t alpha) {
    yuga_zeus_plat_fill_a(x, y, w, h, rgb, radius, alpha);
}
void yuga_platform_plat_fill_g(int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                               int64_t c1, int64_t axis) {
    yuga_zeus_plat_fill_g(x, y, w, h, c0, c1, axis);
}
void yuga_platform_plat_text(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font) {
    yuga_zeus_plat_text(x, y, s, rgb, font);
}

void yuga_platform_plat_text_rot(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font,
                                 int64_t deg) {
    yuga_zeus_plat_text_rot(x, y, s, rgb, font, deg);
}
void yuga_platform_plat_text_int(int64_t x, int64_t y, int64_t v, int64_t rgb, int64_t font) {
    yuga_zeus_plat_text_int(x, y, v, rgb, font);
}
void yuga_platform_plat_svg(int64_t x, int64_t y, int64_t w, int64_t h, yuga_str markup,
                            int64_t rgb, int64_t alpha) {
    yuga_zeus_plat_svg(x, y, w, h, markup, rgb, alpha);
}
void yuga_platform_plat_image(int64_t x, int64_t y, int64_t w, int64_t h, yuga_str src,
                              int64_t radius, int64_t alpha, int64_t fit) {
    yuga_zeus_plat_image(x, y, w, h, src, radius, alpha, fit);
}
yuga_str yuga_platform_plat_pick_image(int64_t *w, int64_t *h) {
    return yuga_zeus_plat_pick_image(w, h);
}
void yuga_platform_plat_save(void) { yuga_zeus_plat_save(); }

/* Host process memory in KB for the gallery RAM chip: Apple phys_footprint
   (the number Xcode's memory gauge shows — RSS overstates iOS processes),
   Linux/Android resident set, wasm live heap on the web host. */
int64_t yuga_platform_plat_mem_kb(void) {
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
void yuga_platform_plat_clip(int64_t x, int64_t y, int64_t w, int64_t h) {
    yuga_zeus_plat_clip(x, y, w, h);
}
void yuga_platform_plat_restore(void) { yuga_zeus_plat_restore(); }
int64_t yuga_platform_plat_key_intern(yuga_str name) {
    char *s = dup_ys(name);
    int id = zeus_key_context_id(s);
    free(s);
    return id;
}
int64_t yuga_platform_plat_key_intern_action(yuga_str action) {
    char *s = dup_ys(action);
    int id = zeus_key_context_from_action(s);
    free(s);
    return id;
}
static void intern_fn_reset(void);

void yuga_platform_plat_key_reset(void) {
    zeus_key_reset_handlers();
    intern_fn_reset();
}
void yuga_platform_plat_map_key(yuga_str spec, yuga_str action, int64_t ctx) {
    char *a = dup_ys(spec), *b = dup_ys(action);
    zeus_key_map_ctx(a, b, (int)ctx);
    free(a);
    free(b);
}

int64_t yuga_platform_plat_key_ev(int64_t key, int64_t mods) {
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

int64_t yuga_platform_plat_edit_append(int64_t slot, int64_t key) {
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

int64_t yuga_platform_plat_edit_back(int64_t slot) {
    int lo, hi;
    if (!edit_ok(slot)) return 0;
    if (edit_has_sel((int)slot))
        return edit_delete_range((int)slot, edit_lo((int)slot), edit_hi((int)slot));
    if (edit_pos[slot] <= 0) return 0;
    hi = edit_pos[slot];
    lo = utf8_prev(edit_buf[slot] ? edit_buf[slot] : "", hi);
    return edit_delete_range((int)slot, lo, hi);
}

int64_t yuga_platform_plat_edit_del(int64_t slot) {
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

int64_t yuga_platform_plat_edit_len(int64_t slot) {
    if (!edit_ok(slot)) return 0;
    return edit_len[slot];
}

yuga_str yuga_platform_plat_edit_text(int64_t slot) {
    if (!edit_ok(slot) || edit_len[slot] <= 0 || !edit_buf[slot])
        return (yuga_str){"", 0};
    return (yuga_str){edit_buf[slot], edit_len[slot]};
}

int64_t yuga_platform_plat_edit_set(int64_t slot, yuga_str text) {
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

int64_t yuga_platform_plat_edit_insert(int64_t slot, yuga_str text) {
    int n;
    if (!edit_ok(slot)) return 0;
    n = text.len > 0 && text.ptr ? (int)text.len : 0;
    if (n <= 0) return 0;
    return edit_insert_bytes((int)slot, text.ptr, n);
}

int64_t yuga_platform_plat_edit_caret(int64_t slot) {
    if (!edit_ok(slot)) return 0;
    return edit_pos[slot];
}

int64_t yuga_platform_plat_edit_anchor(int64_t slot) {
    if (!edit_ok(slot)) return 0;
    return edit_anchor[slot];
}

int64_t yuga_platform_plat_edit_mark(int64_t slot, yuga_str text) {
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

yuga_str yuga_platform_plat_edit_shown(int64_t slot) {
    int n, m, at, need;
    char *v;
    if (!edit_ok(slot)) return (yuga_str){"", 0};
    n = edit_len[slot];
    m = edit_mark_len[slot];
    if (m <= 0) {
        if (n <= 0 || !edit_buf[slot]) return (yuga_str){"", 0};
        return (yuga_str){edit_buf[slot], n};
    }
    at = edit_pos[slot];
    if (at < 0) at = 0;
    if (at > n) at = n;
    need = n + m + 1;
    if (need > edit_view_cap[slot]) {
        v = (char *)realloc(edit_view[slot], (size_t)need);
        if (!v) return (yuga_str){edit_buf[slot] ? edit_buf[slot] : "", n};
        edit_view[slot] = v;
        edit_view_cap[slot] = need;
    }
    v = edit_view[slot];
    if (at && edit_buf[slot]) memcpy(v, edit_buf[slot], (size_t)at);
    if (m && edit_mark[slot]) memcpy(v + at, edit_mark[slot], (size_t)m);
    if (n - at > 0 && edit_buf[slot]) memcpy(v + at + m, edit_buf[slot] + at, (size_t)(n - at));
    v[n + m] = '\0';
    return (yuga_str){v, n + m};
}

void yuga_platform_plat_edit_metrics(int64_t slot, int64_t wrap_w, int64_t font) {
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

int64_t yuga_platform_plat_edit_click(int64_t slot, int64_t x, int64_t y, int64_t wrap_w,
                                      int64_t font) {
    int p;
    if (!edit_ok(slot)) return 0;
    p = edit_xy_to_pos((int)slot, (int)x, (int)y, (int)wrap_w, (int)font);
    edit_pos[slot] = p;
    edit_anchor[slot] = p;
    edit_clamp((int)slot);
    return 1;
}

int64_t yuga_platform_plat_edit_caret_x(int64_t slot) {
    int64_t x = 0, y = 0;
    if (!edit_ok(slot)) return 0;
    edit_pos_to_xy((int)slot, edit_pos[slot], edit_wrap_w[slot] ? edit_wrap_w[slot] : (1 << 20),
                   edit_font[slot] ? edit_font[slot] : 14, &x, &y);
    return x;
}

int64_t yuga_platform_plat_edit_caret_y(int64_t slot) {
    int64_t x = 0, y = 0;
    if (!edit_ok(slot)) return 0;
    edit_pos_to_xy((int)slot, edit_pos[slot], edit_wrap_w[slot] ? edit_wrap_w[slot] : (1 << 20),
                   edit_font[slot] ? edit_font[slot] : 14, &x, &y);
    return y;
}

int64_t yuga_platform_plat_edit_line_h(int64_t slot) {
    int font;
    if (!edit_ok(slot)) return 18;
    font = edit_font[slot] ? edit_font[slot] : 14;
    return wrap_line_h(font);
}

int64_t yuga_platform_plat_edit_move(int64_t slot, int64_t dir, int64_t extend) {
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
void yuga_platform_plat_edit_reset(int64_t slot) {
    if (!edit_ok(slot)) return;
    if (edit_buf[slot]) edit_buf[slot][0] = '\0';
    edit_len[slot] = 0;
    edit_pos[slot] = 0;
    edit_anchor[slot] = 0;
    edit_clear_mark((int)slot);
}

void yuga_platform_plat_measure(yuga_str s, int64_t px, int64_t *w, int64_t *h) {
    yuga_zeus_plat_measure(s, px, w, h);
}
void yuga_platform_plat_measure_int(int64_t v, int64_t px, int64_t *w, int64_t *h) {
    yuga_zeus_plat_measure_int(v, px, w, h);
}
void yuga_platform_plat_measure_wrap(yuga_str s, int64_t px, int64_t max_w, int64_t *w,
                                     int64_t *h) {
    yuga_zeus_plat_measure_wrap(s, px, max_w, w, h);
}
void yuga_platform_plat_text_wrap(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font,
                                  int64_t max_w) {
    yuga_zeus_plat_text_wrap(x, y, s, rgb, font, max_w);
}
void yuga_platform_plat_set_window(yuga_str title, int64_t width, int64_t height) {
    yuga_zeus_plat_set_window(title, width, height);
}
int64_t yuga_platform_plat_view_width(void) { return yuga_zeus_plat_view_width(); }
int64_t yuga_platform_plat_view_height(void) { return yuga_zeus_plat_view_height(); }

int64_t yuga_platform_plat_overlay_scroll(void) {
    return yuga_zeus_plat_overlay_scroll();
}
int64_t yuga_platform_plat_inset_top(void) { return yuga_zeus_plat_inset_top(); }
int64_t yuga_platform_plat_inset_right(void) { return yuga_zeus_plat_inset_right(); }
int64_t yuga_platform_plat_inset_bottom(void) { return yuga_zeus_plat_inset_bottom(); }
int64_t yuga_platform_plat_inset_left(void) { return yuga_zeus_plat_inset_left(); }

/* Interned handlers and reactive prop thunks share this table. A page of
   declarative widgets interns one entry per prop, so it grows rather than
   capping — a full table used to return 0 and silently drop every prop past
   the limit. */
static yuga_fn *click_fns;
static int nclick_fns = 1;
static int click_fns_cap;

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
    yuga_fn *next = (yuga_fn *)realloc(click_fns, (size_t)cap * sizeof(yuga_fn));
    if (!next) return 0;
    memset(next + click_fns_cap, 0, (size_t)(cap - click_fns_cap) * sizeof(yuga_fn));
    click_fns = next;
    click_fns_cap = cap;
    return 1;
}

static void intern_fn_reset(void) {
    int i;
    for (i = 1; i < nclick_fns; i++) {
        free(click_fns[i].env);
        click_fns[i].fn = NULL;
        click_fns[i].env = NULL;
        click_fns[i].env_size = 0;
    }
    nclick_fns = 1;
    nfree_fns = 0;
}

/* Free one interned handler; its id becomes recyclable. */
void yuga_platform_plat_intern_free(int64_t id) {
    if (id <= 0 || id >= nclick_fns) return;
    if (!click_fns[id].fn && !click_fns[id].env) return;
    free(click_fns[id].env);
    click_fns[id].fn = NULL;
    click_fns[id].env = NULL;
    click_fns[id].env_size = 0;
    free_fn_push((int)id);
}

int64_t yuga_platform_plat_intern_fn(yuga_fn handler) {
    yuga_fn kept;
    int id = free_fn_pop();
    if (id == 0) {
        if (!intern_fn_grow()) return 0;
        id = nclick_fns++;
    }
    kept = handler;
    /* Snapshot the env. The Yuga value is often a field that is dropped when
       the props struct returns, which would free the original and leave the
       interned click/styled handler dangling. */
    if (handler.env && handler.env_size > 0) {
        void *copy = malloc(handler.env_size);
        if (copy) {
            memcpy(copy, handler.env, handler.env_size);
            kept.env = copy;
        }
    }
    click_fns[id] = kept;
    return id;
}

/* Reactive prop thunks share the interned-handler table; only the call
   signature differs. Interning yields an int a `||` closure can capture. */
int64_t yuga_platform_plat_intern_int_fn(yuga_fn thunk) {
    return yuga_platform_plat_intern_fn(thunk);
}

int64_t yuga_platform_plat_invoke_int_fn(int64_t id) {
    if (id <= 0 || id >= nclick_fns) return 0;
    yuga_fn h = click_fns[id];
    if (!h.fn) return 0;
    return ((int64_t (*)(void *))h.fn)(h.env);
}

int64_t yuga_platform_plat_intern_bool_fn(yuga_fn thunk) {
    return yuga_platform_plat_intern_fn(thunk);
}

int64_t yuga_platform_plat_invoke_bool_fn(int64_t id) {
    if (id <= 0 || id >= nclick_fns) return 0;
    yuga_fn h = click_fns[id];
    if (!h.fn) return 0;
    return ((bool (*)(void *))h.fn)(h.env) ? 1 : 0;
}

int64_t yuga_platform_plat_intern_str_fn(yuga_fn thunk) {
    return yuga_platform_plat_intern_fn(thunk);
}

yuga_str yuga_platform_plat_invoke_str_fn(int64_t id) {
    if (id <= 0 || id >= nclick_fns) return (yuga_str){ "", 0 };
    yuga_fn h = click_fns[id];
    if (!h.fn) return (yuga_str){ "", 0 };
    return ((yuga_str (*)(void *))h.fn)(h.env);
}

void yuga_platform_plat_invoke_fn(int64_t id) {
    if (id <= 0 || id >= nclick_fns) return;
    yuga_fn h = click_fns[id];
    if (h.fn) ((void (*)(void *))h.fn)(h.env);
}

const char *zeus_window_title(void) { return win_title ? win_title : ""; }
int64_t zeus_window_width(void) { return win_w; }
int64_t zeus_window_height(void) { return win_h; }

void zeus_layout(int64_t width, int64_t height) {
    yuga_zeus_engine_layout(width, height);
}

int zeus_step(float dt) {
    (void)dt;
    return (int)yuga_zeus_engine_step();
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
    yuga_zeus_engine_paint();
    have_draw = 0;
}

int zeus_handle_click(int64_t x, int64_t y) {
    return (int)yuga_zeus_engine_click(x, y);
}

int zeus_handle_hover(int64_t x, int64_t y) {
    return (int)yuga_zeus_engine_hover(x, y);
}

int zeus_over_button(void) { return (int)yuga_zeus_engine_over_button(); }

/* CSS cursor name under the pointer; "" or unknown = default arrow. */
const char *zeus_cursor(void) {
    static char buf[64];
    yuga_str s = yuga_zeus_engine_cursor();
    size_t n = s.len < 0 ? 0 : (size_t)s.len;
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    if (n && s.ptr) memcpy(buf, s.ptr, n);
    buf[n] = '\0';
    return buf;
}

int zeus_handle_scroll(int64_t x, int64_t y, int64_t dx, int64_t dy) {
    return (int)yuga_zeus_engine_scroll(x, y, dx, dy);
}

/* Discrete scroll step (mouse wheel notch, web line/page wheel): applies the
 * delta without velocity, so the scroller never coasts past the click. */
int zeus_handle_scroll_step(int64_t x, int64_t y, int64_t dx, int64_t dy) {
    return (int)yuga_zeus_engine_scroll_step(x, y, dx, dy);
}

int zeus_handle_drag(int64_t x, int64_t y) {
    return (int)yuga_zeus_engine_drag(x, y);
}

void zeus_handle_mouseup(void) { yuga_zeus_engine_mouseup(); }

int zeus_handle_key(int key) {
    return (int)yuga_zeus_engine_key(key);
}

int zeus_handle_text(const char *utf8, int n) {
    yuga_str s;
    if (!utf8) utf8 = "";
    if (n < 0) n = (int)strlen(utf8);
    s.ptr = utf8;
    s.len = n;
    return (int)yuga_zeus_engine_insert(s);
}

int zeus_handle_marked(const char *utf8, int n) {
    yuga_str s;
    if (!utf8) utf8 = "";
    if (n < 0) n = (int)strlen(utf8);
    s.ptr = utf8;
    s.len = n;
    return (int)yuga_zeus_engine_marked(s);
}

int zeus_handle_key_up(int key, int mods) {
    return (int)yuga_zeus_engine_key_up(key, mods);
}

int zeus_handle_key_ev(int key, int mods) {
    yuga_zeus_engine_set_mods((int64_t)mods);
    if (zeus_key_dispatch(key, mods)) return 1;
    /* Plain Tab / Shift-Tab: the engine decides — step the focus ring, or
       insert a tab when a multiline field is focused (single-line fields
       step focus like the web). Keymaps win via dispatch above. */
    if (key == ZEUS_K_TAB && !(mods & ~ZEUS_MOD_SHIFT))
        return zeus_handle_key(key);
    if (mods & ~ZEUS_MOD_SHIFT) return 0;
    return zeus_handle_key(key);
}

int zeus_focus_chain(int *nodes_out, int *ctxs, int max) {
    yuga_zeus_engine_fill_focus();
    int n = (int)yuga_zeus_engine_focus_depth();
    if (n > max) n = max;
    if (n <= 0) {
        if (nodes_out) nodes_out[0] = 0;
        if (ctxs) ctxs[0] = 0;
        return 1;
    }
    for (int i = 0; i < n; i++) {
        if (nodes_out) nodes_out[i] = (int)yuga_zeus_engine_focus_node(i);
        if (ctxs) ctxs[i] = (int)yuga_zeus_engine_focus_ctx(i);
    }
    return n;
}

void zeus_key_apply(int sig, int mode, int64_t value, int64_t lo, int64_t hi) {
    yuga_zeus_engine_key_apply((int64_t)sig, (int64_t)mode, value, lo, hi);
}

int zeus_focus_captures_text(void) {
    return (int)yuga_zeus_engine_focus_captures_text();
}

int zeus_focus_step(int back) {
    return (int)yuga_zeus_engine_focus_step(back);
}

Node yuga_zeus_on_action(Node node, yuga_str action, Signal sig, int64_t mode, int64_t value) {
    char *s = dup_ys(action);
    zeus_key_on_action((int)node.id, s, (int)sig.id, (int)mode, value);
    free(s);
    return node;
}

Node yuga_zeus_on_range(Node node, yuga_str action, Signal sig, int64_t delta, int64_t lo,
                       int64_t hi) {
    char *s = dup_ys(action);
    zeus_key_on_range((int)node.id, s, (int)sig.id, delta, lo, hi);
    free(s);
    return node;
}

void yuga_zeus_on_action_global(yuga_str action, Signal sig, int64_t mode, int64_t value) {
    char *s = dup_ys(action);
    zeus_key_on_action(0, s, (int)sig.id, (int)mode, value);
    free(s);
}

void yuga_zeus_map_key(yuga_str spec, yuga_str action, yuga_str ctx) {
    char *a = dup_ys(spec), *b = dup_ys(action), *c = dup_ys(ctx);
    zeus_key_map(a, b, c);
    free(a);
    free(b);
    free(c);
}

void yuga_zeus_remap_key(yuga_str spec, yuga_str action, yuga_str ctx) {
    char *a = dup_ys(spec), *b = dup_ys(action), *c = dup_ys(ctx);
    zeus_key_remap(a, b, c);
    free(a);
    free(b);
    free(c);
}
