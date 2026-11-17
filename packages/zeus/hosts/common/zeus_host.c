/* zeus_host.c — the host contract's C side, shared by every Zeus host.
 *
 * The engine and every bit of state it keeps are Loam (packages/zeus/src). This
 * file holds only what a C host needs to meet it: the callbacks a host
 * registers (`zeus_set_*`), the bodyless Loam hooks that call them, and the
 * one-line forwarders from the host's C names into Loam's entry points.
 */
#include "zeus_host.h"
#ifndef __wasm32__
#include <time.h>
#endif

/* The signal byte store is Loam (`zeuscore/sigcells.loam`, R4.1b). */

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

/* The window title, size and the rest of what the seam remembers are Loam
   (`zeuscore/platform.loam`). The title reaches a host through its hook, which
   is handed the current title as soon as it is registered. */
static void (*plat_title_fn)(const char *);
void zeus_set_title_hook(void (*fn)(const char *)) {
    plat_title_fn = fn;
    loam_platform_host_retitle();
}

/* Desktop "initial view" providers: the size the host will open its window
   at (device screen work area). Only consulted before the window exists and
   only on real (non-headless) runs, so headless layout stays deterministic. */
static int64_t (*plat_initial_w)(void);
static int64_t (*plat_initial_h)(void);

static void *paint_ctx;
static ZeusDraw paint_draw;
static int have_draw;

/* Decimal text of `v` at `out` (at most 20 bytes, no NUL); its length. No
   printf: wasm has none, and these are the only numbers this file writes. */
static size_t plat_put_int(char *out, int64_t v) {
    char tmp[20];
    size_t n = 0, k = 0;
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0) out[k++] = '-';
    while (n) out[k++] = tmp[--n];
    return k;
}

static void plat_out_of_memory(void) {
    (void)write(2, "zeus: out of memory\n", 20);
    abort();
}

static char *dup_ys(loam_str s) {
    size_t n = s.len < 0 ? 0 : (size_t)s.len;
    char *p = (char *)malloc(n + 1);
    if (!p) plat_out_of_memory();
    if (s.ptr && n) memcpy(p, s.ptr, n);
    p[n] = '\0';
    return p;
}

/* Defined with the frame forwarders. Image and media hooks above them use it. */
static char *cstr_in(loam_str s, char *buf, size_t cap);
static void cstr_done(char *p, char *buf);

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

/* The header parse lives in `zeuscore/imagesize.loam`. This only asks the
   host hook, and leaves both sides 0 when the host has not registered one. */
void loam_zeus_plat_image_size(loam_str src, int32_t *w, int32_t *h) {
    int64_t host_w = 0, host_h = 0;
    if (plat_image_size_fn) {
        char stack_buf[256];
        char *text = cstr_in(src, stack_buf, sizeof stack_buf);
        plat_image_size_fn(text, &host_w, &host_h);
        cstr_done(text, stack_buf);
    }
    if (w) *w = (int32_t)host_w;
    if (h) *h = (int32_t)host_h;
}

loam_str loam_zeus_plat_pick_image(int32_t *w, int32_t *h) {
    char buf[4096];
    int64_t lw = 0, lh = 0;
    buf[0] = 0;
    if (plat_pick_image) plat_pick_image(buf, (int)sizeof buf, &lw, &lh);
    if (w) *w = (int32_t)lw;
    if (h) *h = (int32_t)lh;
    return loam_abi_str_copy(buf, (int64_t)strlen(buf));
}

void loam_zeus_plat_run(void) {
    if (plat_run) plat_run();
}

int32_t loam_platform_native_run_bound(void) { return plat_run ? 1 : 0; }

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

int32_t loam_platform_plat_load_font(loam_str family, loam_str src) {
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

/* In-tree metrics (`packages/zeus/src/zeuscore/metrics.loam`) parse the font's tables, so
   they need its bytes. The web host has no filesystem and installs a hook
   that fetches the URL and pushes the bytes back (packages/zeus/hosts/web).
   Without a hook the answer is empty and `zeus.use_font` reads `src` as a
   path in Loam (`sys.read_file`, R2.2) — desktop, iOS, and Android. */
static loam_str (*plat_font_bytes_fn)(loam_str src);

void zeus_set_font_bytes_hook(loam_str (*fetch)(loam_str src)) {
    plat_font_bytes_fn = fetch;
}

loam_str loam_platform_plat_font_bytes(loam_str src) {
    if (plat_font_bytes_fn) return plat_font_bytes_fn(src);
    return (loam_str){"", 0};
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
    buf[plat_put_int(buf, v)] = '\0';
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

/* `s[from..to)` as the host measures it: no allocation, `measure_span`
   copies the bytes to a terminated stack buffer. The wrap and the text-edit
   caret are Loam (`metrics.host_wrap`, `edit.loam`) and measure words and
   prefixes through this. */
void loam_platform_plat_measure_span(loam_str s, int32_t from, int32_t to, int32_t px,
                                     int32_t *w, int32_t *h) {
    int64_t tw = 0, th = 0;
    if (from < 0) from = 0;
    if (to > s.len) to = (int32_t)s.len;
    if (to < from) to = from;
    measure_span(s.ptr ? s.ptr + from : "", to - from, px, &tw, &th);
    if (w) *w = (int32_t)tw;
    if (h) *h = (int32_t)th;
}

void zeus_set_initial_view(int64_t (*width)(void), int64_t (*height)(void)) {
    plat_initial_w = width;
    plat_initial_h = height;
}

/* Before the window exists, the size a desktop host will open it at. */
int32_t loam_platform_native_initial_w(void) {
    return plat_initial_w ? (int32_t)plat_initial_w() : 0;
}
int32_t loam_platform_native_initial_h(void) {
    return plat_initial_h ? (int32_t)plat_initial_h() : 0;
}

/* Host writes and reads of the window state, forwarded to Loam. */
void zeus_window_opened(void) { loam_platform_host_window_opened(); }
int64_t zeus_window_width(void) { return loam_platform_host_window_width(); }
int64_t zeus_window_height(void) { return loam_platform_host_window_height(); }
void zeus_set_window_size(int64_t width, int64_t height) {
    loam_platform_host_set_window_size((int32_t)width, (int32_t)height);
}
int64_t zeus_window_chrome(void) { return loam_platform_host_window_chrome(); }

void zeus_set_insets(int64_t top, int64_t right, int64_t bottom, int64_t left) {
    loam_platform_plat_set_insets((int32_t)top, (int32_t)right, (int32_t)bottom, (int32_t)left);
}

void zeus_set_titlebar_box(int64_t height, int64_t lead, int64_t trail) {
    loam_platform_plat_set_titlebar_box((int32_t)height, (int32_t)lead, (int32_t)trail);
}

/* packages/zeus/src/zeuscore/platform.loam is the GPUI-style platform leaf. Empty Loam stubs
   compile to loam_platform_plat_*; the Cocoa implementations stay as
   loam_zeus_plat_* so existing callers do not change. */
void loam_platform_plat_run(void) { loam_zeus_plat_run(); }

loam_str loam_platform_plat_pick_image(int32_t *w, int32_t *h) {
    return loam_zeus_plat_pick_image(w, h);
}
void loam_platform_plat_image_size(loam_str src, int32_t *w, int32_t *h) {
    loam_zeus_plat_image_size(src, w, h);
}

/* The clock `platform.plat_now_ms` runs on (it adds a test's skip itself):
   the one `std:async` reads, CLOCK_MONOTONIC (the JS clock on wasm), without
   the allocation `async.now_ms` makes for its `timespec`. */
#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("now_ms")))
int64_t zeus_js_now_ms(void);
int64_t loam_platform_native_clock_ms(void) { return zeus_js_now_ms(); }
#else
int64_t loam_platform_native_clock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}
#endif

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

void loam_platform_plat_measure(loam_str s, int32_t px, int32_t *w, int32_t *h) {
    loam_zeus_plat_measure(s, px, w, h);
}
void loam_platform_plat_measure_int(int32_t v, int32_t px, int32_t *w, int32_t *h) {
    loam_zeus_plat_measure_int(v, px, w, h);
}

/* The title and the clipboard text are kept in Loam. These hand a native host
   each new one, NUL-terminated for the moment of the call. */
void loam_platform_native_title_changed(loam_str title) {
    char stack_buf[256];
    char *text;
    if (!plat_title_fn) return;
    text = cstr_in(title, stack_buf, sizeof stack_buf);
    plat_title_fn(text);
    cstr_done(text, stack_buf);
}

static void (*plat_clip_fn)(const char *);
void zeus_set_clipboard_hook(void (*fn)(const char *)) { plat_clip_fn = fn; }

void loam_platform_native_clipboard_write(loam_str text) {
    char stack_buf[256];
    char *copy;
    if (!plat_clip_fn) return;
    copy = cstr_in(text, stack_buf, sizeof stack_buf);
    plat_clip_fn(copy);
    cstr_done(copy, stack_buf);
}

/* Media state (the null player, the pick queue, fullscreen) lives in
   `zeuscore/mediastate.loam`. This file only stores the host table and
   forwards each callback. A host reports a pick to the Loam export
   `engine_media_picked`, which decides whether to redraw. */
static ZeusMedia media_host;

void zeus_set_media(ZeusMedia media) { media_host = media; }

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

void zeus_boundary_preset_add(int32_t seq, const char *msg, size_t n) {
    loam_str owned;
    if (!msg) {
        msg = "";
        n = 0;
    }
    /* The parameter is borrowed: the store retains what it keeps, and drops
       nothing. Release the temporary here, including the one past the cap. */
    owned = loam_abi_str_copy(msg, (int64_t)n);
    loam_runtime_preset_store(seq, owned);
    loam_abi_str_release(&owned);
}

/* Router host entry points. A build that never imports `std:router` has no
   strong definition, so the web host's `zeus_open_url` still links; importing
   the router overrides these weak no-ops. */
__attribute__((weak)) void loam_router_engine_open_url(loam_str url) { (void)url; }
__attribute__((weak)) void loam_router_engine_history_back(void) {}
__attribute__((weak)) void loam_router_engine_chunks_done(int32_t index) { (void)index; }
__attribute__((weak)) void loam_router_engine_chunks_slow(int32_t index) { (void)index; }

/* Router history mirror. The Loam-side stack is authoritative; these mirror a
   push / replace / back into the host's own affordance. The web host forwards
   to `history.pushState` / `replaceState` / `back`; other hosts ignore them
   until wired. */

/* --- the frame (`platform.plat_frame`, `replay.loam`) ----------------------
   A frame is one buffer in the format of packages/zeus/docs/frame-format.md.
   `zeuscore/replay.loam` decodes it into the bound `ZeusDraw`, with the
   fallbacks for the callbacks a host left NULL (R4.2), through the one-line
   forwarders below. A host that takes frames whole (`zeus_bind_frame`, as the
   web host does to decode them in JS) gets the buffer here instead. */
static void (*frame_sink)(const ZeusFrame *f);

void zeus_bind_frame(void (*sink)(const ZeusFrame *f)) {
    frame_sink = sink;
}

/* The Loam vectors arrive by value (int32 elements, loam_str elements). The
   header's counts are clamped to what the vectors hold, so a short buffer
   hands over less instead of reading past its end. */
void loam_platform_plat_frame(loam_vec head, loam_vec ops, loam_vec strs, loam_vec path) {
    const int32_t *hd = (const int32_t *)head.ptr;
    ZeusFrame f;
    if (!frame_sink || !hd || head.len < 6 || hd[0] != ZEUS_FRAME_MAGIC) return;
    f.version = hd[1];
    f.stride = hd[2];
    f.n = hd[3];
    f.ns = hd[4];
    f.np = hd[5];
    if (f.stride <= 0 || !ops.ptr) return;
    if (f.n * f.stride > ops.len) f.n = ops.len / f.stride;
    if (!strs.ptr || f.ns > strs.len) f.ns = strs.ptr ? strs.len : 0;
    if (!path.ptr || f.np > path.len) f.np = path.ptr ? path.len : 0;
    f.ops = (const int32_t *)ops.ptr;
    f.strs = (const loam_str *)strs.ptr;
    f.path = (const int32_t *)path.ptr;
    if (f.n <= 0) return;
    frame_sink(&f);
}

/* Which callbacks the bound table has (`replay.CAP_*`). */
int32_t loam_replay_draw_caps(void) {
    const ZeusDraw *d = &paint_draw;
    int32_t c = frame_sink ? 65536 : 0;
    if (!have_draw) return c;
    c |= 1 | (d->fill ? 2 : 0) | (d->fill_a ? 4 : 0) | (d->text ? 8 : 0) | (d->text_rot ? 16 : 0);
    c |= (d->save ? 32 : 0) | (d->clip ? 64 : 0) | (d->restore ? 128 : 0) | (d->alpha ? 256 : 0);
    c |= (d->image ? 512 : 0) | (d->fill_g ? 1024 : 0) | (d->shadow ? 2048 : 0);
    c |= (d->stroke ? 4096 : 0) | (d->fill4 ? 8192 : 0) | (d->xform ? 16384 : 0);
    c |= d->path ? 32768 : 0;
    return c;
}

/* A Loam string as the NUL-terminated text a host takes: in `buf` when it
   fits, else on the heap (`cstr_done` frees that). */
static char *cstr_in(loam_str s, char *buf, size_t cap) {
    size_t n = s.len < 0 ? 0 : (size_t)s.len;
    char *p = n < cap ? buf : dup_ys(s);
    if (p == buf) {
        if (n) memcpy(buf, s.ptr, n);
        buf[n] = '\0';
    }
    return p;
}
static void cstr_done(char *p, char *buf) {
    if (p != buf) free(p);
}

/* One forwarder per callback; `replay.loam` calls only those `draw_caps`
   reported, and each re-checks its own, so a NULL is never called. */
#define ZD (paint_draw)
void loam_replay_draw_fill(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rgb, int32_t r) {
    if (ZD.fill) ZD.fill(paint_ctx, x, y, w, h, rgb, r);
}
void loam_replay_draw_fill_a(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rgb, int32_t r,
                             int32_t a) {
    if (ZD.fill_a) ZD.fill_a(paint_ctx, x, y, w, h, rgb, r, a);
}
/* String `si` of a frame's table, or "" (`si` -1 or out of range). */
static loam_str frame_str_at(const loam_vec *strs, int32_t si) {
    static const loam_str none = {"", 0, 0};
    if (!strs || !strs->ptr || si < 0 || si >= strs->len) return none;
    return ((const loam_str *)strs->ptr)[si];
}
void loam_replay_draw_text(int32_t x, int32_t y, const loam_vec *strs, int32_t si, int32_t rgb,
                           int32_t font) {
    char buf[256], *p;
    if (!ZD.text) return;
    p = cstr_in(frame_str_at(strs, si), buf, sizeof buf);
    ZD.text(paint_ctx, x, y, p, rgb, font);
    cstr_done(p, buf);
}
void loam_replay_draw_text_rot(int32_t x, int32_t y, const loam_vec *strs, int32_t si,
                               int32_t rgb, int32_t font, int32_t deg) {
    char buf[256], *p;
    if (!ZD.text_rot) return;
    p = cstr_in(frame_str_at(strs, si), buf, sizeof buf);
    ZD.text_rot(paint_ctx, x, y, p, rgb, font, deg);
    cstr_done(p, buf);
}
void loam_replay_draw_save(void) {
    if (ZD.save) ZD.save(paint_ctx);
}
void loam_replay_draw_clip(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r) {
    if (ZD.clip) ZD.clip(paint_ctx, x, y, w, h, r);
}
void loam_replay_draw_restore(void) {
    if (ZD.restore) ZD.restore(paint_ctx);
}
void loam_replay_draw_alpha(int32_t a) {
    if (ZD.alpha) ZD.alpha(paint_ctx, a);
}
void loam_replay_draw_image(int32_t x, int32_t y, int32_t w, int32_t h, const loam_vec *strs,
                            int32_t si, int32_t r, int32_t a, int32_t fit) {
    char buf[256], *p;
    if (!ZD.image) return;
    p = cstr_in(frame_str_at(strs, si), buf, sizeof buf);
    ZD.image(paint_ctx, x, y, w, h, p, r, a, fit);
    cstr_done(p, buf);
}
void loam_replay_draw_fill_g(int32_t x, int32_t y, int32_t w, int32_t h, int32_t c0, int32_t c1,
                             int32_t axis, int32_t r, int32_t a) {
    if (ZD.fill_g) ZD.fill_g(paint_ctx, x, y, w, h, c0, c1, axis, r, a);
}
void loam_replay_draw_shadow(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t rgb,
                             int32_t a, int32_t blur, int32_t dx, int32_t dy) {
    if (ZD.shadow) ZD.shadow(paint_ctx, x, y, w, h, r, rgb, a, blur, dx, dy);
}
void loam_replay_draw_stroke(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rgb, int32_t r,
                             int32_t width, int32_t a) {
    if (ZD.stroke) ZD.stroke(paint_ctx, x, y, w, h, rgb, r, width, a);
}
void loam_replay_draw_fill4(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rgb, int32_t a,
                            int32_t tl, int32_t tr, int32_t br, int32_t bl) {
    if (ZD.fill4) ZD.fill4(paint_ctx, x, y, w, h, rgb, a, tl, tr, br, bl);
}
void loam_replay_draw_xform(int32_t dx, int32_t dy, int32_t scale, int32_t rot, int32_t ox,
                            int32_t oy) {
    if (ZD.xform) ZD.xform(paint_ctx, dx, dy, scale, rot, ox, oy);
}
/* `start` / `n` are checked against `path` here too: a pointer offset is the
   one thing this file may not take on trust. */
void loam_replay_draw_path(const loam_vec *path, int32_t start, int32_t n, int32_t rgb, int32_t a,
                           int32_t width, int32_t style) {
    if (!ZD.path || !path || !path->ptr || start < 0 || n <= 0 || (int64_t)start + n > path->len)
        return;
    ZD.path(paint_ctx, (const int32_t *)path->ptr + start, n, rgb, a, width, style);
}
#undef ZD

/* --- media host forwarders (`zeuscore/mediastate.loam`) -------------------
   One call each, no player state, native hosts only: the web host's player
   is Loam over its imports (`mediastate.host_*`). Strings are NUL-terminated
   in a stack buffer when they fit (`cstr_in`). */
int32_t loam_mediastate_native_caps(void) {
    int32_t bits = 0;
    if (media_host.open) bits |= 1;
    if (media_host.cmd) bits |= 2;
    if (media_host.get) bits |= 4;
    if (media_host.text) bits |= 8;
    if (media_host.pick) bits |= 16;
    if (media_host.pick_audio) bits |= 32;
    if (media_host.voice) bits |= 64;
    if (media_host.fullscreen) bits |= 128;
    return bits;
}

int32_t loam_imagesize_image_host_bound(void) {
    return plat_image_size_fn ? 1 : 0;
}

int32_t loam_mediastate_native_open(int32_t slot, loam_str src) {
    char stack_buf[256];
    char *text = cstr_in(src, stack_buf, sizeof stack_buf);
    int32_t opened = 0;
    if (media_host.open) opened = (int32_t)media_host.open((int64_t)slot, text);
    cstr_done(text, stack_buf);
    return opened;
}

void loam_mediastate_native_cmd(int32_t slot, int32_t command, int32_t value) {
    if (media_host.cmd) media_host.cmd((int64_t)slot, (int64_t)command, (int64_t)value);
}

int32_t loam_mediastate_native_get(int32_t slot, int32_t what) {
    if (!media_host.get) return 0;
    return (int32_t)media_host.get((int64_t)slot, (int64_t)what);
}

loam_str loam_mediastate_native_text(int32_t slot, int32_t what) {
    char buffer[4096];
    buffer[0] = 0;
    if (media_host.text) {
        media_host.text((int64_t)slot, (int64_t)what, buffer, (int)sizeof buffer);
        buffer[sizeof buffer - 1] = 0;
    }
    return loam_abi_str_copy(buffer, (int64_t)strlen(buffer));
}

void loam_mediastate_native_pick(void) {
    if (media_host.pick) media_host.pick();
}

void loam_mediastate_native_pick_audio(void) {
    if (media_host.pick_audio) media_host.pick_audio();
}

int32_t loam_mediastate_native_voice(int32_t what, loam_str src, loam_str out_path) {
    char src_buf[256], out_buf[256];
    char *src_text, *out_text;
    int32_t started = 0;
    if (!media_host.voice) return 0;
    src_text = cstr_in(src, src_buf, sizeof src_buf);
    out_text = cstr_in(out_path, out_buf, sizeof out_buf);
    started = (int32_t)media_host.voice((int64_t)what, src_text, out_text);
    cstr_done(src_text, src_buf);
    cstr_done(out_text, out_buf);
    return started;
}

int32_t loam_mediastate_native_fullscreen(int32_t on) {
    if (!media_host.fullscreen) return 0;
    return (int32_t)media_host.fullscreen((int64_t)on);
}

void loam_mediastate_host_redraw(void) {
    if (plat_redraw) plat_redraw();
}
