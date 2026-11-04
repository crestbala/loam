/* wasm.c — Canvas2D glue for Zeus (zeus/web). Not WebGPU.
 *
 * One paint path: Loam `scene.paint` (`loam_zeus_engine_paint`) produces one
 * frame buffer (docs/frame-format.md) a frame, which this file hands to JS
 * whole; loader.js decodes it into Canvas2D. The browser owns rAF: `zeus_paint`
 * returns 0 when idle so the loader can stop, like Cocoa `engine_next_ms`.
 */
#include "zeus_rt.h"
#include "zeus_key.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int main(void);

__attribute__((import_module("zeus"), import_name("measure")))
void zeus_js_measure(const char *s, int32_t px, int32_t *w, int32_t *h);

__attribute__((import_module("zeus"), import_name("set_font_family")))
void zeus_js_set_font_family(const char *name);

__attribute__((import_module("zeus"), import_name("load_font")))
int32_t zeus_js_load_font(const char *family, const char *src);

__attribute__((import_module("zeus"), import_name("font_fetch")))
int32_t zeus_js_font_fetch(const char *src);

__attribute__((import_module("zeus"), import_name("image_size")))
void zeus_js_image_size(const char *src, int32_t *w, int32_t *h);

__attribute__((import_module("zeus"), import_name("pick_image")))
void zeus_js_pick_image(void);

/* Canvas2D caches metrics per size, so the family change has to reach JS. */
static void wasm_set_font_family(const char *family) {
    zeus_js_set_font_family(family ? family : "");
}

static int wasm_load_font(const char *family, const char *src) {
    if (!family || !src) return 0;
    return zeus_js_load_font(family, src) ? 1 : 0;
}

/* In-tree metrics need the font's bytes, which this host has no filesystem to
   read. Ask JS to fetch `src`; it copies the bytes back into `wasm_font_buf`
   and calls `zeus_font_set`, which binds them. Until then `loam_metrics_bind`
   is not called and layout keeps Canvas2D `measureText` — the same "accepted,
   not ready" contract as `load_font`. */
static char *wasm_font_buf;
static int32_t wasm_font_cap;

static loam_str wasm_font_bytes(loam_str src) {
    char buf[512];
    size_t n = src.len > 0 && src.ptr ? (size_t)src.len : 0;
    if (n >= sizeof buf) n = sizeof buf - 1;
    if (n && src.ptr) memcpy(buf, src.ptr, n);
    buf[n] = '\0';
    if (n) zeus_js_font_fetch(buf);
    return (loam_str){"", 0};
}

int32_t loam_metrics_bind(loam_str bytes);

/* JS fetches a font, reserves space here, writes it, then calls `zeus_font_set`.
   Bind copies into the Loam arena so a later fetch's realloc cannot dangle the
   bytes the metrics hold. */
__attribute__((export_name("zeus_font_reserve")))
char *zeus_font_reserve(int32_t n) {
    if (n <= 0) return wasm_font_buf;
    if (n > wasm_font_cap) {
        char *p = (char *)realloc(wasm_font_buf, (size_t)n);
        if (!p) return wasm_font_buf;
        wasm_font_buf = p;
        wasm_font_cap = n;
    }
    return wasm_font_buf;
}

__attribute__((export_name("zeus_font_set")))
int32_t zeus_font_set(int32_t n) {
    char *p;
    if (n <= 0 || n > wasm_font_cap || !wasm_font_buf) return 0;
    p = (char *)loam_new((size_t)n, "font_bytes", 0);
    memcpy(p, wasm_font_buf, (size_t)n);
    return loam_metrics_bind((loam_str){p, n});
}

static void wasm_measure(const char *s, int64_t px, int64_t *w, int64_t *h) {
    int32_t tw = 0, th = 0;
    zeus_js_measure(s ? s : "", (int32_t)px, &tw, &th);
    if (w) *w = tw;
    if (h) *h = th;
}

__attribute__((import_module("zeus"), import_name("request_frame")))
void zeus_js_request_frame(void);

/* Monotonic milliseconds from the browser (`performance.now`). The delta
   between frames drives the animation tracks, so 60 Hz and 120 Hz hosts move
   at the same speed. */
__attribute__((import_module("zeus"), import_name("now_ms")))
int64_t zeus_js_now_ms(void);

static void wasm_run(void) {
    /* Browser owns the loop. `zeus_paint` layouts; do not block `zeus_start`. */
}

static void wasm_pick_image(char *out, int cap, int64_t *w, int64_t *h) {
    if (out && cap > 0) out[0] = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    zeus_js_pick_image();
}

static void wasm_image_size(const char *src, int64_t *w, int64_t *h) {
    int32_t sw = 0, sh = 0;
    zeus_js_image_size(src, &sw, &sh);
    if (w) *w = sw;
    if (h) *h = sh;
}

static void wasm_redraw(void) {
    zeus_js_request_frame();
}

/* The frame goes to JS whole: `frame` gets one pointer to `wasm_frame_head`,
   `version stride n ops ns strs np path`, the pointers into this memory. The
   strings are `ns` (ptr, len) int32 pairs, so JS decodes UTF-8 straight from
   the engine's bytes. Everything stays valid only during the call. */
__attribute__((import_module("zeus"), import_name("frame")))
void zeus_js_frame(const int32_t *head);

static int32_t wasm_frame_head[8];
static int32_t *wasm_strs;
static int64_t wasm_strs_cap;

static void wasm_frame(const ZeusFrame *f) {
    if (f->ns * 2 > wasm_strs_cap) {
        int64_t cap = f->ns * 2 + 64;
        int32_t *p = (int32_t *)realloc(wasm_strs, (size_t)cap * sizeof *p);
        if (!p) return;
        wasm_strs = p;
        wasm_strs_cap = cap;
    }
    for (int64_t i = 0; i < f->ns; i++) {
        wasm_strs[i * 2] = (int32_t)(intptr_t)f->strs[i].ptr;
        wasm_strs[i * 2 + 1] = (int32_t)f->strs[i].len;
    }
    wasm_frame_head[0] = f->version;
    wasm_frame_head[1] = f->stride;
    wasm_frame_head[2] = (int32_t)f->n;
    wasm_frame_head[3] = (int32_t)(intptr_t)f->ops;
    wasm_frame_head[4] = (int32_t)f->ns;
    wasm_frame_head[5] = (int32_t)(intptr_t)wasm_strs;
    wasm_frame_head[6] = (int32_t)f->np;
    wasm_frame_head[7] = (int32_t)(intptr_t)f->path;
    zeus_js_frame(wasm_frame_head);
}

/* 1 while `main` runs (the initial build). A trap then cannot be recovered in
   place — what was cut short is the whole app — so `zeus_recover` reports 2
   and the loader re-instantiates with the failed boundary pre-seeded. */
static int wasm_booting;

__attribute__((export_name("zeus_start")))
void zeus_start(void) {
    zeus_set_platform(wasm_run, wasm_measure, wasm_redraw);
    zeus_set_pick_image(wasm_pick_image);
    zeus_set_image_size(wasm_image_size);
    zeus_set_font_hooks(wasm_load_font, wasm_set_font_family);
    zeus_set_font_bytes_hook(wasm_font_bytes);
    zeus_bind_frame(wasm_frame);
    wasm_booting = 1;
    main();
    wasm_booting = 0;
}

/* Live wasm heap in KB (allocated minus the free list), for the RAM chip and
   for leak checks from a harness. */
int32_t zeus_heap_used(void);
__attribute__((export_name("zeus_heap_kb")))
int32_t zeus_heap_kb(void) {
    return zeus_heap_used() / 1024;
}

__attribute__((export_name("zeus_resize")))
void zeus_resize(int32_t w, int32_t h) {
    zeus_set_window_size(w, h);
}

/* 0 = idle (stop rAF). 1 = paint next frame. >1 = ms until the next timer. */
__attribute__((export_name("zeus_paint")))
int32_t zeus_wasm_paint(void) {
    static int64_t last_ms;
    int more;
    int64_t due;
    int64_t now = zeus_js_now_ms();
    float dt = 1.f / 60.f;
    if (last_ms > 0) dt = (float)((double)(now - last_ms) / 1000.0);
    last_ms = now;
    if (dt < 0.001f) dt = 1.f / 60.f;
    if (dt > 0.064f) dt = 0.064f;
    zeus_layout(zeus_window_width(), zeus_window_height());
    more = zeus_step(dt);
    loam_zeus_engine_paint();
    if (more) return 1;
    due = loam_zeus_engine_next_ms();
    if (due < 0) return 1;
    if (due > 2147483647) return 2147483647;
    return (int32_t)due;
}

/* A trap (a Loam panic: bounds, overflow, `panic`) unwound out of an export:
   wasm has no setjmp, so the loader catches the RuntimeError, restores the
   shadow stack pointer, and calls this. The innermost `Boundary` that was at
   work takes the trap and shows its fallback on the next frame. 1 = recovered
   (keep running), 0 = no boundary could take it. */
__attribute__((export_name("zeus_recover")))
int32_t zeus_recover(void) {
    int32_t r = loam_zeus_engine_recover();
    if (r && wasm_booting) return loam_zeus_engine_trap_seq() > 0 ? 2 : 0;
    return r;
}

/* After `zeus_recover` returned 2: the failed boundary's build-order number,
   and the panic message (a C string in static storage). */
__attribute__((export_name("zeus_trap_seq")))
int32_t zeus_trap_seq(void) {
    return loam_zeus_engine_trap_seq();
}

__attribute__((export_name("zeus_trap_msg")))
const char *zeus_trap_msg(void) {
    return loam_jmp_msg;
}

/* On a fresh instance, before `zeus_start`: pre-seed boundary `seq` as failed
   with `msg`, so it builds its fallback without running its body. */
__attribute__((export_name("zeus_boundary_preset")))
void zeus_boundary_preset(int32_t seq, const char *msg) {
    zeus_boundary_preset_add(seq, msg ? msg : "", msg ? strlen(msg) : 0);
}

void loam_zeus_engine_fonts_changed(void);

/* A web font landed after frames were measured with the fallback face. */
__attribute__((export_name("zeus_fonts_changed")))
void zeus_fonts_changed(void) {
    loam_zeus_engine_fonts_changed();
}

/* The loader reports `prefers-reduced-motion` (and re-reports on change). */
__attribute__((export_name("zeus_reduced_motion")))
void zeus_reduced_motion(int32_t on) {
    loam_zeus_engine_set_reduced_motion(on);
}

__attribute__((export_name("zeus_pointer_down")))
void zeus_pointer_down(int32_t x, int32_t y) {
    zeus_handle_click(x, y);
}

/* Touch classification for the loader, without pressing. See `engine_grab`. */
__attribute__((export_name("zeus_grab")))
int32_t zeus_grab(int32_t x, int32_t y) {
    return loam_zeus_engine_grab(x, y);
}

/* Secondary click (`contextmenu`): the nearest `on_context_menu` handler. */
__attribute__((export_name("zeus_context_click")))
int32_t zeus_context_click(int32_t x, int32_t y) {
    return zeus_handle_context_click(x, y);
}

__attribute__((export_name("zeus_pointer_move")))
void zeus_pointer_move(int32_t x, int32_t y) {
    zeus_handle_hover(x, y);
    zeus_handle_drag(x, y);
}

__attribute__((export_name("zeus_pointer_up")))
void zeus_pointer_up(void) {
    zeus_handle_mouseup();
}

static char wasm_cursor[64];

/* Copy the engine's CSS cursor name ("", "pointer", "text", …) into a
   static buffer the loader can read and hand to canvas.style.cursor. */
__attribute__((export_name("zeus_cursor_sync")))
const char *zeus_cursor_sync(void) {
    const char *name = zeus_cursor();
    size_t n = name ? strlen(name) : 0;
    if (n >= sizeof wasm_cursor) n = sizeof wasm_cursor - 1;
    memcpy(wasm_cursor, name ? name : "", n);
    wasm_cursor[n] = '\0';
    return wasm_cursor;
}

/* Copy the visible semantic tree (one line per node, see `engine_a11y_dump`)
   into a buffer the loader reads to keep its visually-hidden DOM mirror in
   sync (§2.3). The buffer grows to fit: a fixed one truncated large pages, so
   a screen reader only ever heard their top part.

   Re-dumped when a layout ran since the last call, or when the loader forces
   it (`force`, a few times a second) — a checkbox flip or a focus move
   changes the semantics without a layout. NULL = unchanged. */
static char *wasm_a11y_buf;
static size_t wasm_a11y_cap;
int32_t loam_zeus_engine_layout_gen(void);
loam_vec loam_zeus_engine_a11y_bytes(void);
int32_t loam_zeus_engine_a11y_activate(int32_t id);
void loam_zeus_engine_a11y_focus(int32_t id);
int32_t loam_zeus_engine_focus_id(void);

__attribute__((export_name("zeus_a11y_sync")))
const char *zeus_a11y_sync(int32_t force) {
    static int32_t a11y_gen = -1;
    int32_t gen = loam_zeus_engine_layout_gen();
    loam_vec b;
    const int32_t *el;
    size_t n, i;
    if (gen == a11y_gen && !force) return NULL;
    a11y_gen = gen;
    /* Bytes, not a string: the vec is refcounted and dropped here, whereas a
       string from `engine_a11y_dump` would live forever (a per-frame leak). */
    b = loam_zeus_engine_a11y_bytes();
    el = (const int32_t *)b.ptr;
    n = b.len > 0 ? (size_t)b.len : 0;
    if (n + 1 > wasm_a11y_cap) {
        size_t cap = wasm_a11y_cap ? wasm_a11y_cap : 16384;
        char *p;
        while (cap < n + 1) cap *= 2;
        p = (char *)realloc(wasm_a11y_buf, cap);
        if (!p) {
            loam_vec_drop(&b);
            return NULL;
        }
        wasm_a11y_buf = p;
        wasm_a11y_cap = cap;
    }
    for (i = 0; i < n; i++) wasm_a11y_buf[i] = (char)(el ? (el[i] & 255) : 0);
    wasm_a11y_buf[n] = '\0';
    loam_vec_drop(&b);
    return wasm_a11y_buf;
}

/* A screen reader pressed / focused mirror element `id` (the node id from the
   dump): run it through the engine as Enter / a focus move would. */
__attribute__((export_name("zeus_a11y_activate")))
int32_t zeus_a11y_activate(int32_t id) {
    return loam_zeus_engine_a11y_activate(id);
}

__attribute__((export_name("zeus_a11y_focus")))
void zeus_a11y_focus(int32_t id) {
    loam_zeus_engine_a11y_focus(id);
}

/* The engine's focused node (0 = none), so the mirror can move DOM focus with
   it and a screen reader announces Tab / arrow moves made on the canvas. */
__attribute__((export_name("zeus_focus_id")))
int32_t zeus_focus_id(void) {
    return loam_zeus_engine_focus_id();
}

/* Deep link / popstate: route the app to `path`. The loader passes a C string
   in `wasm_text_buf`; copy it into the arena so the router can store it. */
__attribute__((export_name("zeus_open_url")))
void zeus_open_url(const char *path) {
    size_t n = path ? strlen(path) : 0;
    char *p = (char *)loam_new(n + 1, "open_url", 0);
    if (!p) return;
    if (n) memcpy(p, path, n);
    p[n] = '\0';
    loam_router_engine_open_url((loam_str){p, n});
}

/* A route's chunks have landed: mount the page that was waiting for them. */
__attribute__((export_name("zeus_chunks_done")))
void zeus_chunks_done(int32_t index) {
    loam_router_engine_chunks_done(index);
}

/* That route's chunks are slow: time to say so on screen. */
__attribute__((export_name("zeus_chunks_slow")))
void zeus_chunks_slow(int32_t index) {
    loam_router_engine_chunks_slow(index);
}

#ifndef LOAM_WEB_RELEASE
/* Hot reload (§2.5): expose the signal arena as text, and restore it. The dev
   loader stashes the snapshot in sessionStorage before reloading. */
static char wasm_state_buf[65536];

__attribute__((export_name("zeus_state_snapshot")))
const char *zeus_state_snapshot(void) {
    loam_str s = loam_zeus_engine_signals_dump();
    size_t n = (s.len > 0 && s.ptr) ? (size_t)s.len : 0;
    if (n >= sizeof wasm_state_buf) n = sizeof wasm_state_buf - 1;
    if (n) memcpy(wasm_state_buf, s.ptr, n);
    wasm_state_buf[n] = '\0';
    return wasm_state_buf;
}

__attribute__((export_name("zeus_state_restore")))
int32_t zeus_state_restore(const char *snapshot) {
    size_t n = snapshot ? strlen(snapshot) : 0;
    if (n == 0) return 0;
    char *p = (char *)loam_new(n + 1, "state_restore", 0);
    if (!p) return 0;
    memcpy(p, snapshot, n);
    p[n] = '\0';
    return loam_zeus_engine_state_load((loam_str){p, n});
}
#endif

__attribute__((export_name("zeus_scroll")))
void zeus_scroll(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    zeus_handle_scroll(x, y, dx, dy);
}

/* 1 if anything moved: a fling stops when it runs out of content. */
__attribute__((export_name("zeus_scroll_step")))
int32_t zeus_scroll_step(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    return zeus_handle_scroll_step(x, y, dx, dy);
}

/* Hold the scrollers under a touch for its whole gesture. See `scroll_latch`. */
__attribute__((export_name("zeus_scroll_latch")))
void zeus_scroll_latch(int32_t x, int32_t y) {
    loam_zeus_engine_scroll_latch(x, y);
}

__attribute__((export_name("zeus_scroll_smooth")))
void zeus_scroll_smooth(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    zeus_handle_scroll_smooth(x, y, dx, dy);
}

__attribute__((export_name("zeus_key_up")))
void zeus_key_up(int32_t key, int32_t mods) {
    zeus_handle_key_up((int)key, (int)mods);
}

__attribute__((export_name("zeus_key")))
void zeus_key(int32_t key, int32_t mods) {
    zeus_handle_key_ev((int)key, (int)mods);
}

static char wasm_text_buf[4096];

__attribute__((export_name("zeus_text_buf")))
char *zeus_text_buf(void) {
    return wasm_text_buf;
}

__attribute__((export_name("zeus_text_buf_cap")))
int32_t zeus_text_buf_cap(void) {
    return (int32_t)sizeof wasm_text_buf;
}

__attribute__((export_name("zeus_text")))
void zeus_text(int32_t n) {
    if (n < 0) n = 0;
    if (n >= (int32_t)sizeof wasm_text_buf) n = (int32_t)sizeof wasm_text_buf - 1;
    wasm_text_buf[n] = '\0';
    zeus_handle_text(wasm_text_buf, (int)n);
}

__attribute__((export_name("zeus_marked")))
void zeus_marked(int32_t n) {
    if (n < 0) n = 0;
    if (n >= (int32_t)sizeof wasm_text_buf) n = (int32_t)sizeof wasm_text_buf - 1;
    wasm_text_buf[n] = '\0';
    zeus_handle_marked(wasm_text_buf, (int)n);
}

__attribute__((export_name("zeus_captures_text")))
int32_t zeus_captures_text(void) {
    return zeus_focus_captures_text() ? 1 : 0;
}

__attribute__((export_name("zeus_picked")))
void zeus_picked(int32_t n, int32_t w, int32_t h) {
    if (n < 0) n = 0;
    if (n >= (int32_t)sizeof wasm_text_buf) n = (int32_t)sizeof wasm_text_buf - 1;
    wasm_text_buf[n] = '\0';
    zeus_picked_image(wasm_text_buf, w, h);
}
