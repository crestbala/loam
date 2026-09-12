/* wasm.c — Canvas2D glue for Zeus (zeus/web). Not WebGPU.
 *
 * One paint path: Yuga `scene.paint` (`yuga_zeus_engine_paint`). This file
 * binds Canvas2D as the plat_* backend. The browser owns rAF: `zeus_paint`
 * returns 0 when idle so the loader can stop, like Cocoa `engine_next_ms`.
 */
#include "zeus_rt.h"
#include "zeus_key.h"
#include <stdint.h>
#include <string.h>

int main(void);

__attribute__((import_module("zeus"), import_name("fill")))
void zeus_js_fill(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rgb, int32_t radius);

__attribute__((import_module("zeus"), import_name("fill_a")))
void zeus_js_fill_a(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rgb, int32_t radius,
                    int32_t alpha);

__attribute__((import_module("zeus"), import_name("fill_g")))
void zeus_js_fill_g(int32_t x, int32_t y, int32_t w, int32_t h, int32_t c0, int32_t c1,
                    int32_t axis);

__attribute__((import_module("zeus"), import_name("text")))
void zeus_js_text(int32_t x, int32_t y, const char *s, int32_t rgb, int32_t font);

__attribute__((import_module("zeus"), import_name("text_rot")))
void zeus_js_text_rot(int32_t x, int32_t y, const char *s, int32_t rgb, int32_t font,
                      int32_t deg);

__attribute__((import_module("zeus"), import_name("measure")))
void zeus_js_measure(const char *s, int32_t px, int32_t *w, int32_t *h);

__attribute__((import_module("zeus"), import_name("set_font_family")))
void zeus_js_set_font_family(const char *name);

__attribute__((import_module("zeus"), import_name("load_font")))
int32_t zeus_js_load_font(const char *family, const char *src);

__attribute__((import_module("zeus"), import_name("save")))
void zeus_js_save(void);

__attribute__((import_module("zeus"), import_name("clip")))
void zeus_js_clip(int32_t x, int32_t y, int32_t w, int32_t h);

__attribute__((import_module("zeus"), import_name("restore")))
void zeus_js_restore(void);

__attribute__((import_module("zeus"), import_name("svg")))
void zeus_js_svg(int32_t x, int32_t y, int32_t w, int32_t h, const char *markup, int32_t rgb,
                 int32_t alpha);

__attribute__((import_module("zeus"), import_name("image")))
void zeus_js_image(int32_t x, int32_t y, int32_t w, int32_t h, const char *src, int32_t radius,
                   int32_t alpha, int32_t fit);

__attribute__((import_module("zeus"), import_name("pick_image")))
void zeus_js_pick_image(void);

static void draw_fill(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                      int64_t radius) {
    (void)ctx;
    zeus_js_fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (int32_t)(rgb & 0xFFFFFF),
                 (int32_t)radius);
}

static void draw_fill_a(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                        int64_t radius, int64_t alpha) {
    (void)ctx;
    zeus_js_fill_a((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (int32_t)(rgb & 0xFFFFFF),
                   (int32_t)radius, (int32_t)alpha);
}

static void draw_fill_g(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                        int64_t c1, int64_t axis) {
    (void)ctx;
    zeus_js_fill_g((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (int32_t)(c0 & 0xFFFFFF),
                   (int32_t)(c1 & 0xFFFFFF), (int32_t)axis);
}

static void draw_text(void *ctx, int64_t x, int64_t y, const char *s, int64_t rgb, int64_t font) {
    (void)ctx;
    zeus_js_text((int32_t)x, (int32_t)y, s ? s : "", (int32_t)(rgb & 0xFFFFFF), (int32_t)font);
}

static void draw_text_rot(void *ctx, int64_t x, int64_t y, const char *s, int64_t rgb,
                          int64_t font, int64_t deg) {
    (void)ctx;
    zeus_js_text_rot((int32_t)x, (int32_t)y, s ? s : "", (int32_t)(rgb & 0xFFFFFF),
                     (int32_t)font, (int32_t)deg);
}

static void draw_save(void *ctx) {
    (void)ctx;
    zeus_js_save();
}

static void draw_clip(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h) {
    (void)ctx;
    zeus_js_clip((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h);
}

static void draw_restore(void *ctx) {
    (void)ctx;
    zeus_js_restore();
}

static void draw_svg(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, const char *markup,
                     int64_t rgb, int64_t alpha) {
    (void)ctx;
    zeus_js_svg((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, markup ? markup : "",
                (int32_t)(rgb & 0xFFFFFF), (int32_t)alpha);
}

static void draw_image(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, const char *src,
                       int64_t radius, int64_t alpha, int64_t fit) {
    (void)ctx;
    zeus_js_image((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, src ? src : "",
                  (int32_t)radius, (int32_t)alpha, (int32_t)fit);
}

/* Canvas2D caches metrics per size, so the family change has to reach JS. */
static void wasm_set_font_family(const char *family) {
    zeus_js_set_font_family(family ? family : "");
}

static int wasm_load_font(const char *family, const char *src) {
    if (!family || !src) return 0;
    return zeus_js_load_font(family, src) ? 1 : 0;
}

static void wasm_measure(const char *s, int64_t px, int64_t *w, int64_t *h) {
    int32_t tw = 0, th = 0;
    zeus_js_measure(s ? s : "", (int32_t)px, &tw, &th);
    if (w) *w = tw;
    if (h) *h = th;
}

__attribute__((import_module("zeus"), import_name("request_frame")))
void zeus_js_request_frame(void);

static void wasm_run(void) {
    /* Browser owns the loop. `zeus_paint` layouts; do not block `zeus_start`. */
}

static void wasm_pick_image(char *out, int cap, int64_t *w, int64_t *h) {
    if (out && cap > 0) out[0] = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    zeus_js_pick_image();
}

static void wasm_redraw(void) {
    zeus_js_request_frame();
}

static void bind_canvas(void) {
    ZeusDraw d;
    memset(&d, 0, sizeof d);
    d.fill = draw_fill;
    d.fill_a = draw_fill_a;
    d.fill_g = draw_fill_g;
    d.text = draw_text;
    d.text_rot = draw_text_rot;
    d.save = draw_save;
    d.clip = draw_clip;
    d.restore = draw_restore;
    d.svg = draw_svg;
    d.image = draw_image;
    zeus_bind_draw(d);
}

__attribute__((export_name("zeus_start")))
void zeus_start(void) {
    zeus_set_platform(wasm_run, wasm_measure, wasm_redraw);
    zeus_set_pick_image(wasm_pick_image);
    zeus_set_font_hooks(wasm_load_font, wasm_set_font_family);
    bind_canvas();
    main();
}

__attribute__((export_name("zeus_resize")))
void zeus_resize(int32_t w, int32_t h) {
    zeus_set_window_size(w, h);
}

/* 0 = idle (stop rAF). 1 = paint next frame. >1 = ms until the next timer. */
__attribute__((export_name("zeus_paint")))
int32_t zeus_wasm_paint(void) {
    int more;
    int64_t due;
    zeus_layout(zeus_window_width(), zeus_window_height());
    more = zeus_step(1.f / 60.f);
    yuga_zeus_engine_paint();
    if (more) return 1;
    due = yuga_zeus_engine_next_ms();
    if (due < 0) return 1;
    if (due > 2147483647) return 2147483647;
    return (int32_t)due;
}

__attribute__((export_name("zeus_pointer_down")))
void zeus_pointer_down(int32_t x, int32_t y) {
    zeus_handle_click(x, y);
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

__attribute__((export_name("zeus_scroll")))
void zeus_scroll(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    zeus_handle_scroll(x, y, dx, dy);
}

__attribute__((export_name("zeus_scroll_step")))
void zeus_scroll_step(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    zeus_handle_scroll_step(x, y, dx, dy);
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
