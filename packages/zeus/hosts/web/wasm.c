/* wasm.c — Canvas2D glue for Zeus (zeus/web). Not WebGPU.
 *
 * One paint path: Loam `scene.paint` (`loam_zeus_engine_paint`) produces one
 * frame buffer (docs/frame-format.md) a frame, which this file hands to JS
 * whole; loader.js decodes it into Canvas2D. The browser owns rAF: `zeus_paint`
 * returns 0 when idle so the loader can stop, like Cocoa `engine_next_ms`.
 */
#include "zeus_host.h"

int main(void);
int32_t loam_sys_wasm_heap_used(void);
void loam_zeus_engine_fonts_changed(void);
int32_t loam_zeus_engine_a11y_activate(int32_t id);
void loam_zeus_engine_a11y_focus(int32_t id);
int32_t loam_zeus_engine_focus_id(void);

__attribute__((import_module("zeus"), import_name("measure")))
void zeus_js_measure(const char *s, int32_t px, int32_t *w, int32_t *h);

__attribute__((import_module("zeus"), import_name("frame")))
void zeus_js_frame(const int32_t *head);

static char *vec_ptr(loam_vec b) {
    char *p = b.len > 0 ? (char *)b.ptr : NULL;
    loam_abi_vec_drop(&b);
    return p;
}

__attribute__((export_name("zeus_font_reserve")))
char *zeus_font_reserve(int32_t n) {
    return vec_ptr(loam_metrics_font_reserve(n));
}

__attribute__((export_name("zeus_font_set")))
int32_t zeus_font_set(int32_t n) {
    return loam_metrics_font_commit(n);
}

static void wasm_measure(const char *s, int64_t px, int64_t *w, int64_t *h) {
    int32_t tw = 0, th = 0;
    zeus_js_measure(s ? s : "", (int32_t)px, &tw, &th);
    if (w) *w = tw;
    if (h) *h = th;
}

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

__attribute__((export_name("zeus_start")))
void zeus_start(void) {
    zeus_set_platform(NULL, wasm_measure, NULL);
    zeus_bind_frame(wasm_frame);
    loam_zeus_engine_boot_begin();
    main();
    loam_zeus_engine_boot_end();
}

__attribute__((export_name("zeus_heap_kb")))
int32_t zeus_heap_kb(void) {
    return loam_sys_wasm_heap_used() / 1024;
}

__attribute__((export_name("zeus_resize")))
void zeus_resize(int32_t w, int32_t h) {
    zeus_set_window_size(w, h);
}

__attribute__((export_name("zeus_paint")))
int32_t zeus_wasm_paint(void) {
    return loam_zeus_engine_wasm_paint();
}

__attribute__((export_name("zeus_recover")))
int32_t zeus_recover(void) {
    return loam_zeus_engine_recover();
}

__attribute__((export_name("zeus_trap_seq")))
int32_t zeus_trap_seq(void) {
    return loam_zeus_engine_trap_seq();
}

__attribute__((export_name("zeus_trap_msg")))
const char *zeus_trap_msg(void) {
    return loam_jmp_msg;
}

__attribute__((export_name("zeus_boundary_preset")))
void zeus_boundary_preset(int32_t seq, const char *msg) {
    zeus_boundary_preset_add(seq, msg ? msg : "", msg ? strlen(msg) : 0);
}

__attribute__((export_name("zeus_fonts_changed")))
void zeus_fonts_changed(void) {
    loam_zeus_engine_fonts_changed();
}

__attribute__((export_name("zeus_reduced_motion")))
void zeus_reduced_motion(int32_t on) {
    loam_zeus_engine_set_reduced_motion(on);
}

__attribute__((export_name("zeus_pointer_down")))
void zeus_pointer_down(int32_t x, int32_t y) {
    loam_zeus_engine_click(x, y);
}

__attribute__((export_name("zeus_grab")))
int32_t zeus_grab(int32_t x, int32_t y) {
    return loam_zeus_engine_grab(x, y);
}

__attribute__((export_name("zeus_context_click")))
int32_t zeus_context_click(int32_t x, int32_t y) {
    return loam_zeus_engine_context_click(x, y);
}

__attribute__((export_name("zeus_pointer_move")))
void zeus_pointer_move(int32_t x, int32_t y) {
    loam_zeus_engine_pointer_move(x, y, 0, 0);
}

__attribute__((export_name("zeus_pointer_up")))
void zeus_pointer_up(void) {
    loam_zeus_engine_mouseup();
}

__attribute__((export_name("zeus_cursor_sync")))
const char *zeus_cursor_sync(void) {
    return vec_ptr(loam_zeus_engine_cursor_sync());
}

__attribute__((export_name("zeus_a11y_sync")))
const char *zeus_a11y_sync(int32_t force) {
    return vec_ptr(loam_zeus_engine_a11y_sync(force));
}

__attribute__((export_name("zeus_a11y_activate")))
int32_t zeus_a11y_activate(int32_t id) {
    return loam_zeus_engine_a11y_activate(id);
}

__attribute__((export_name("zeus_a11y_focus")))
void zeus_a11y_focus(int32_t id) {
    loam_zeus_engine_a11y_focus(id);
}

__attribute__((export_name("zeus_focus_id")))
int32_t zeus_focus_id(void) {
    return loam_zeus_engine_focus_id();
}

__attribute__((export_name("zeus_open_url")))
void zeus_open_url(const char *path) {
    loam_router_engine_open_url((loam_str){path ? path : "", path ? (int64_t)strlen(path) : 0, 0});
}

__attribute__((export_name("zeus_chunks_done")))
void zeus_chunks_done(int32_t index) {
    loam_router_engine_chunks_done(index);
}

__attribute__((export_name("zeus_chunks_slow")))
void zeus_chunks_slow(int32_t index) {
    loam_router_engine_chunks_slow(index);
}

#ifndef LOAM_WEB_RELEASE
__attribute__((export_name("zeus_state_snapshot")))
const char *zeus_state_snapshot(void) {
    return vec_ptr(loam_zeus_engine_state_snapshot());
}

__attribute__((export_name("zeus_state_restore")))
int32_t zeus_state_restore(const char *s) {
    return s && *s ? loam_zeus_engine_state_load((loam_str){s, (int64_t)strlen(s), 0}) : 0;
}
#endif

__attribute__((export_name("zeus_scroll")))
void zeus_scroll(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    loam_zeus_engine_scroll(x, y, dx, dy);
}

__attribute__((export_name("zeus_scroll_step")))
int32_t zeus_scroll_step(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    return loam_zeus_engine_scroll_step(x, y, dx, dy);
}

__attribute__((export_name("zeus_scroll_latch")))
void zeus_scroll_latch(int32_t x, int32_t y) {
    loam_zeus_engine_scroll_latch(x, y);
}

__attribute__((export_name("zeus_scroll_smooth")))
void zeus_scroll_smooth(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    loam_zeus_engine_scroll_smooth(x, y, dx, dy);
}

__attribute__((export_name("zeus_key_up")))
void zeus_key_up(int32_t key, int32_t mods) {
    loam_zeus_engine_key_up(key, mods);
}

__attribute__((export_name("zeus_key")))
void zeus_key(int32_t key, int32_t mods) {
    loam_zeus_engine_key_ev(key, mods);
}

__attribute__((export_name("zeus_text_buf")))
char *zeus_text_buf(void) {
    return vec_ptr(loam_zeus_engine_text_buf());
}

__attribute__((export_name("zeus_text_buf_cap")))
int32_t zeus_text_buf_cap(void) {
    return loam_zeus_engine_text_buf_cap();
}

__attribute__((export_name("zeus_text")))
void zeus_text(int32_t n) {
    loam_zeus_engine_text_commit(n);
}

__attribute__((export_name("zeus_marked")))
void zeus_marked(int32_t n) {
    loam_zeus_engine_marked_commit(n);
}

__attribute__((export_name("zeus_captures_text")))
int32_t zeus_captures_text(void) {
    return loam_zeus_engine_focus_captures_text() ? 1 : 0;
}

__attribute__((export_name("zeus_picked")))
void zeus_picked(int32_t n, int32_t w, int32_t h) {
    loam_zeus_engine_picked_commit(n, w, h);
}
