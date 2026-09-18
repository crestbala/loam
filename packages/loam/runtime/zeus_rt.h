/* zeus_rt.h — platform seam and handle types for `import "std:zeus"`.
 *
 * Node and Signal are typed handles (id + struct). The retained tree,
 * layout, paint, and hit-test live in packages/zeus/std/zeus.loam. Empty fns in that
 * file (plat_*, keyboard) map to the hooks below. Cocoa talks to the
 * engine through zeus_layout / zeus_paint / zeus_handle_*.
 */
#ifndef ZEUS_RT_H
#define ZEUS_RT_H

#include <stdint.h>

#ifdef LOAM_RT_H
/* loam_str already defined in generated C */
#else
#include "loam_rt.h"
#endif

typedef struct {
    uint32_t id;
} Node;

typedef struct {
    uint32_t id;
} Signal;

/* --- Loam API: import "std:zeus" → loam_zeus_* --- */

void loam_zeus_raw_init(void);
void loam_zeus_open_window(loam_str title, int32_t width, int32_t height);
void loam_zeus_raw_run(void);

Signal loam_zeus_signal(int64_t value);
/* Signal slot allocation with free-list reuse (arena compaction). The int
   form writes the int mirror slot; the zero form reserves a placeholder slot
   for a typed payload the caller binds afterwards. */
int64_t loam_zeus_sig_alloc_int(int64_t value);
int64_t loam_zeus_sig_alloc_zero(void);
void loam_zeus_sig_free(int64_t id);
void loam_zeus_hook_begin(void);
Signal loam_zeus_hook_signal(int32_t value);
int64_t loam_zeus_get(Signal sig);
void loam_zeus_set(Signal sig, int64_t value);
void loam_zeus_inc(Signal sig, int32_t delta);

void loam_zeus_sig_bind(int64_t id, const void *src, int64_t n);
void loam_zeus_sig_load(int64_t id, void *dst, int64_t n);
int64_t loam_zeus_sig_changed(int64_t id, const void *src, int64_t n);
int64_t loam_zeus_sig_gen(int64_t id);
Signal loam_zeus_appearance(void);
void loam_zeus_theme(int32_t slot, int32_t light, int32_t dark);
int32_t loam_zeus_role(int32_t slot);

Node loam_zeus_raw_row(void);
Node loam_zeus_raw_col(void);
Node loam_zeus_raw_box(void);
Node loam_zeus_grid(int32_t columns, int32_t gap);
Node loam_zeus_grid_auto(int32_t min_width, int32_t gap);
Node loam_zeus_raw_label(loam_str text);
Node loam_zeus_raw_button(loam_str text);
Node loam_zeus_raw_spacer(void);
Node loam_zeus_raw_progress(void);
Node loam_zeus_raw_slider(void);
Node loam_zeus_raw_toggle(void);
Node loam_zeus_raw_check(loam_str markup);
Node loam_zeus_raw_overlay(void);
Node loam_zeus_raw_input(loam_str placeholder);
Node loam_zeus_raw_scroll(void);
Node loam_zeus_raw_svg(loam_str markup);

Node loam_zeus_child(Node parent, Node node);
Node loam_zeus_root(Node node);

Node loam_zeus_grow(Node node, int32_t weight);
Node loam_zeus_shrink(Node node, int32_t weight);
Node loam_zeus_pad(Node node, int32_t all);
Node loam_zeus_padX(Node node, int32_t px);
Node loam_zeus_padY(Node node, int32_t py);
Node loam_zeus_padTop(Node node, int32_t px);
Node loam_zeus_padRight(Node node, int32_t px);
Node loam_zeus_padBottom(Node node, int32_t px);
Node loam_zeus_padLeft(Node node, int32_t px);
Node loam_zeus_margin(Node node, int32_t all);
Node loam_zeus_marginX(Node node, int32_t px);
Node loam_zeus_marginY(Node node, int32_t py);
Node loam_zeus_marginTop(Node node, int32_t px);
Node loam_zeus_marginRight(Node node, int32_t px);
Node loam_zeus_marginBottom(Node node, int32_t px);
Node loam_zeus_marginLeft(Node node, int32_t px);
Node loam_zeus_border(Node node, int32_t width);
Node loam_zeus_border_color(Node node, int32_t rgb);
Node loam_zeus_gap(Node node, int32_t g);
Node loam_zeus_gap_row(Node node, int32_t g);
Node loam_zeus_gap_col(Node node, int32_t g);
Node loam_zeus_bg(Node node, int32_t rgb);
Node loam_zeus_fg(Node node, int32_t rgb);
Node loam_zeus_w(Node node, int32_t px);
Node loam_zeus_h(Node node, int32_t px);
Node loam_zeus_width_pct(Node node, int32_t pct);
Node loam_zeus_height_pct(Node node, int32_t pct);
Node loam_zeus_min_w(Node node, int32_t px);
Node loam_zeus_max_w(Node node, int32_t px);
Node loam_zeus_min_h(Node node, int32_t px);
Node loam_zeus_max_h(Node node, int32_t px);
Node loam_zeus_aspect(Node node, int32_t aw, int32_t ah);
Node loam_zeus_flex_row(Node node);
Node loam_zeus_flex_col(Node node);
Node loam_zeus_flex_row_reverse(Node node);
Node loam_zeus_flex_col_reverse(Node node);
Node loam_zeus_flex_wrap(Node node);
Node loam_zeus_align_self(Node node, int32_t mode);
Node loam_zeus_span(Node node, int32_t columns);
Node loam_zeus_radius(Node node, int32_t px);
Node loam_zeus_justify(Node node, int32_t mode);
Node loam_zeus_align(Node node, int32_t mode);
Node loam_zeus_position(Node node, int32_t mode);
Node loam_zeus_top(Node node, int32_t px);
Node loam_zeus_right(Node node, int32_t px);
Node loam_zeus_bottom(Node node, int32_t px);
Node loam_zeus_left(Node node, int32_t px);
Node loam_zeus_z_index(Node node, int32_t z);
Node loam_zeus_opacity(Node node, int32_t pct);
Node loam_zeus_overflow(Node node, int32_t mode);
Node loam_zeus_font(Node node, int32_t px);

Node loam_zeus_bind(Node label, Signal sig);
Node loam_zeus_bind_n(Node label, int32_t n);
Node loam_zeus_digits(int32_t n);
Node loam_zeus_on_click_inc(Node button, Signal sig);
Node loam_zeus_on_click_toggle(Node node, Signal sig);
Node loam_zeus_on_click_set(Node node, Signal sig, int32_t value);
Node loam_zeus_on_click_add(Node node, Signal sig, int32_t delta);
Node loam_zeus_show(Node node, Signal sig);
Node loam_zeus_show_eq(Node node, Signal sig, int32_t value);
Node loam_zeus_show_ne(Node node, Signal sig, int32_t value);
Node loam_zeus_key_context(Node node, loam_str name);
Node loam_zeus_focusable(Node node);
Node loam_zeus_capture_text(Node node);
Node loam_zeus_on_action(Node node, loam_str action, Signal sig, int64_t mode, int64_t value);
Node loam_zeus_on_key_fn(Node node, loam_str action);
Node loam_zeus_on_range(Node node, loam_str action, Signal sig, int64_t delta, int64_t lo,
                       int64_t hi);
void loam_zeus_on_action_global(loam_str action, Signal sig, int64_t mode, int64_t value);
void loam_zeus_map_key(loam_str spec, loam_str action, loam_str ctx);
void loam_zeus_remap_key(loam_str spec, loam_str action, loam_str ctx);
int zeus_handle_key_ev(int key, int mods);

Node loam_zeus_show_ge(Node node, Signal sig, int32_t value);
Node loam_zeus_show_le(Node node, Signal sig, int32_t value);
Node loam_zeus_raw_hover(Node node, Signal sig);
Node loam_zeus_hover_delay(Node node, int32_t ms);
Node loam_zeus_hover_leave(Node node, int32_t ms);
Node loam_zeus_pulse(Node node);
Node loam_zeus_dismiss(Node node);
Node loam_zeus_keys(Node node, Signal sig, int32_t lo, int32_t hi);
Node loam_zeus_keys_page(Node node, Signal sig);

/* --- Platform hosts: zeus/desktop/mac.m, zeus/ios/ios.m, zeus/web/wasm.c --- */

typedef struct {
    void (*fill)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                 int64_t rgb, int64_t radius);
    void (*fill_a)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                   int64_t rgb, int64_t radius, int64_t alpha);
    void (*text)(void *ctx, int64_t x, int64_t y, const char *s,
                 int64_t rgb, int64_t font);
    /* Rotated text: `deg` clockwise on screen, pivot at the top-left of the
       line box. NULL hosts fall back to horizontal `text`. */
    void (*text_rot)(void *ctx, int64_t x, int64_t y, const char *s,
                     int64_t rgb, int64_t font, int64_t deg);
    void (*save)(void *ctx);
    /* Rounded clip. `radius` 0 is the plain rect clip this used to be; a
       positive radius is what lets an image or a gradient inside a rounded
       card stop having square corners. Hosts that cannot clip to a path
       ignore the radius rather than dropping the clip. */
    void (*clip)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                 int64_t radius);
    void (*restore)(void *ctx);
    /* SVG markup in `markup`; `currentColor` paints as `rgb`. alpha is 0..255. */
    void (*svg)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                const char *markup, int64_t rgb, int64_t alpha);
    /* Raster image. `src` is a path, data URI, or URL. Decode is in the host
       (PNG / JPEG / WebP / GIF); NULL skips. radius clips; alpha is 0..255.
       fit: 0 stretch, 1 contain, 2 cover, 3 none. */
    void (*image)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                  const char *src, int64_t radius, int64_t alpha, int64_t fit);
    /* Linear gradient. axis 0 = top c0 → bottom c1, 1 = left → right.
       `radius` rounds the corners (it used to be dropped, which is why every
       gradient background painted square); alpha is 0..255.
       NULL hosts fall back to a solid `c0` fill. */
    void (*fill_g)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                   int64_t c0, int64_t c1, int64_t axis, int64_t radius,
                   int64_t alpha);
    /* --- depth, stroke, and transform (design-system phase 2) --- */
    /* Soft drop shadow of a rounded rect, painted under the fill it belongs
       to. `blur` is the blur radius in dp, `dx`/`dy` the offset, alpha
       0..255. This is the one primitive elevation cannot be faked without.
       NULL hosts skip it: a card goes flat, never hard-edged black. */
    void (*shadow)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                   int64_t radius, int64_t rgb, int64_t alpha,
                   int64_t blur, int64_t dx, int64_t dy);
    /* Anti-aliased ring stroke, inset by width/2 so it paints inside the
       rect. Replaces the old "draw a bigger filled rect underneath" border,
       which made a translucent bordered surface impossible and forced every
       bordered node to carry a background. NULL hosts fall back to four
       `fill_a` edges (square corners). */
    void (*stroke)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                   int64_t rgb, int64_t radius, int64_t width, int64_t alpha);
    /* Per-corner radius fill, clockwise from the top-left. Grouped button
       runs, top-rounded sheets, and rounded table headers need it.
       NULL hosts fall back to `fill_a` with the largest of the four. */
    void (*fill4)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                  int64_t rgb, int64_t alpha,
                  int64_t tl, int64_t tr, int64_t br, int64_t bl);
    /* Translate by (dx, dy), then scale and rotate about (ox, oy). `scale`
       is a percent (100 = identity), `rot` is clockwise degrees. Applies
       within the current save/restore level. Every transition that moves or
       scales a box rather than re-laying it out goes through this — press
       scale, dialog scale-in, the tabs indicator, toast slide.
       NULL hosts no-op, so the widget lands at its settled position. */
    void (*xform)(void *ctx, int64_t dx, int64_t dy, int64_t scale,
                  int64_t rot, int64_t ox, int64_t oy);
} ZeusDraw;

void zeus_set_platform(void (*run)(void),
                      void (*measure)(const char *s, int64_t px, int64_t *w, int64_t *h),
                      void (*redraw)(void));
void zeus_set_pick_image(void (*pick)(char *out, int cap, int64_t *w, int64_t *h));
void zeus_set_image_size(void (*size)(const char *src, int64_t *w, int64_t *h));
void zeus_picked_image(const char *src, int64_t w, int64_t h);
void zeus_bind_draw(ZeusDraw draw);

/* Empty packages/zeus/std/zeus.loam fns → these C symbols. */
void loam_zeus_plat_run(void);
int64_t loam_zeus_plat_headless(void);
void loam_zeus_plat_fill(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                        int64_t radius);
void loam_zeus_plat_fill_a(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                          int64_t radius, int64_t alpha);
void loam_zeus_plat_fill_g(int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                          int64_t c1, int64_t axis, int64_t radius, int64_t alpha);
void loam_zeus_plat_shadow(int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius,
                          int64_t rgb, int64_t alpha, int64_t blur, int64_t dx, int64_t dy);
void loam_zeus_plat_stroke(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                          int64_t radius, int64_t width, int64_t alpha);
void loam_zeus_plat_fill4(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                         int64_t alpha, int64_t tl, int64_t tr, int64_t br, int64_t bl);
void loam_zeus_plat_xform(int64_t dx, int64_t dy, int64_t scale, int64_t rot,
                         int64_t ox, int64_t oy);
void loam_zeus_plat_text(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font);
/* Current global font family ("" = host default) and the hook a host
   registers to load one. See zeus_plat.c for why the family is global. */
const char *zeus_font_family(void);
void zeus_set_font_hooks(int (*load)(const char *family, const char *src),
                         void (*set_family)(const char *family));
/* Bytes for in-tree metrics. Desktop/iOS/Android default to reading `src` as a
   file; the web host (no filesystem) installs a hook that fetches it. */
void zeus_set_font_bytes_hook(loam_str (*fetch)(loam_str src));
void loam_platform_plat_set_font_family(loam_str name);
int64_t loam_platform_plat_load_font(loam_str family, loam_str src);

void loam_zeus_plat_text_rot(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font,
                             int64_t deg);
void loam_zeus_plat_text_int(int64_t x, int64_t y, int64_t v, int64_t rgb, int64_t font);
void loam_zeus_plat_measure(loam_str s, int32_t px, int32_t *w, int32_t *h);
void loam_zeus_plat_measure_int(int32_t v, int32_t px, int32_t *w, int32_t *h);
void loam_zeus_plat_measure_wrap(loam_str s, int32_t px, int32_t max_w, int32_t *w, int32_t *h);
void loam_zeus_plat_text_wrap(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font,
                             int64_t max_w);
void loam_zeus_plat_set_window(loam_str title, int64_t width, int64_t height);
int64_t loam_zeus_plat_view_width(void);
int64_t loam_zeus_plat_view_height(void);
void loam_zeus_plat_svg(int64_t x, int64_t y, int64_t w, int64_t h, loam_str markup,
                       int64_t rgb, int64_t alpha);
void loam_zeus_plat_image(int64_t x, int64_t y, int64_t w, int64_t h, loam_str src,
                         int64_t radius, int64_t alpha, int64_t fit);
void loam_zeus_plat_image_size(loam_str src, int32_t *w, int32_t *h);
loam_str loam_zeus_plat_pick_image(int32_t *w, int32_t *h);
void loam_zeus_plat_save(void);
void loam_zeus_plat_clip(int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius);
void loam_zeus_plat_restore(void);

void zeus_set_insets(int64_t top, int64_t right, int64_t bottom, int64_t left);
int64_t loam_zeus_plat_overlay_scroll(void);
int64_t loam_zeus_plat_inset_top(void);
int64_t loam_zeus_plat_inset_right(void);
int64_t loam_zeus_plat_inset_bottom(void);
int64_t loam_zeus_plat_inset_left(void);

/* packages/zeus/std/zeuscore/platform.loam FFI (aliases of loam_zeus_plat_*). */
void loam_platform_plat_run(void);
int64_t loam_platform_plat_headless(void);
void loam_platform_plat_fill(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                             int64_t radius);
void loam_platform_plat_fill_a(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                               int64_t radius, int64_t alpha);
void loam_platform_plat_fill_g(int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                               int64_t c1, int64_t axis, int64_t radius, int64_t alpha);
void loam_platform_plat_shadow(int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius,
                               int64_t rgb, int64_t alpha, int64_t blur,
                               int64_t dx, int64_t dy);
void loam_platform_plat_stroke(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                               int64_t radius, int64_t width, int64_t alpha);
void loam_platform_plat_fill4(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                              int64_t alpha, int64_t tl, int64_t tr, int64_t br, int64_t bl);
void loam_platform_plat_xform(int64_t dx, int64_t dy, int64_t scale, int64_t rot,
                              int64_t ox, int64_t oy);
void loam_platform_plat_text(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font);
void loam_platform_plat_text_rot(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font,
                                 int64_t deg);
void loam_platform_plat_text_int(int64_t x, int64_t y, int64_t v, int64_t rgb, int64_t font);
void loam_platform_plat_measure(loam_str s, int32_t px, int32_t *w, int32_t *h);
loam_str loam_platform_plat_font_bytes(loam_str src);
void loam_platform_plat_measure_int(int32_t v, int32_t px, int32_t *w, int32_t *h);
void loam_platform_plat_measure_wrap(loam_str s, int32_t px, int32_t max_w, int32_t *w,
                                     int32_t *h);
void loam_platform_plat_text_wrap(int64_t x, int64_t y, loam_str s, int64_t rgb, int64_t font,
                                  int64_t max_w);
void loam_platform_plat_set_window(loam_str title, int64_t width, int64_t height);
void loam_platform_plat_set_title(loam_str title);
int64_t loam_platform_plat_view_width(void);
int64_t loam_platform_plat_view_height(void);
void loam_platform_plat_svg(int64_t x, int64_t y, int64_t w, int64_t h, loam_str markup,
                            int64_t rgb, int64_t alpha);
void loam_platform_plat_image(int64_t x, int64_t y, int64_t w, int64_t h, loam_str src,
                              int64_t radius, int64_t alpha, int64_t fit);
void loam_platform_plat_image_size(loam_str src, int32_t *w, int32_t *h);
loam_str loam_platform_plat_pick_image(int32_t *w, int32_t *h);
void loam_platform_plat_save(void);
int64_t loam_platform_plat_mem_kb(void);
int64_t loam_platform_plat_now_ms(void);
void loam_platform_plat_clip(int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius);
void loam_platform_plat_restore(void);
int64_t loam_platform_plat_key_intern(loam_str name);
int64_t loam_platform_plat_key_intern_action(loam_str action);
void loam_platform_plat_key_reset(void);
void loam_platform_plat_map_key(loam_str spec, loam_str action, int64_t ctx);
int64_t loam_platform_plat_key_ev(int64_t key, int64_t mods);
int64_t loam_platform_plat_edit_append(int64_t slot, int64_t key);
int64_t loam_platform_plat_edit_back(int64_t slot);
int64_t loam_platform_plat_edit_del(int64_t slot);
int64_t loam_platform_plat_edit_len(int64_t slot);
loam_str loam_platform_plat_edit_text(int64_t slot);
loam_str loam_platform_plat_edit_shown(int64_t slot);
int64_t loam_platform_plat_edit_set(int64_t slot, loam_str text);
int64_t loam_platform_plat_edit_insert(int64_t slot, loam_str text);
int64_t loam_platform_plat_edit_caret(int64_t slot);
int64_t loam_platform_plat_edit_anchor(int64_t slot);
int64_t loam_platform_plat_edit_set_caret(int64_t slot, int64_t pos);
int64_t loam_platform_plat_edit_set_anchor(int64_t slot, int64_t pos);
int64_t loam_platform_plat_edit_mark(int64_t slot, loam_str text);
void loam_platform_plat_edit_metrics(int64_t slot, int64_t wrap_w, int64_t font);
int64_t loam_platform_plat_edit_click(int64_t slot, int64_t x, int64_t y, int64_t wrap_w,
                                      int64_t font);
int64_t loam_platform_plat_edit_move(int64_t slot, int64_t dir, int64_t extend);
int64_t loam_platform_plat_edit_caret_x(int64_t slot);
int64_t loam_platform_plat_edit_caret_y(int64_t slot);
int64_t loam_platform_plat_edit_line_h(int64_t slot);
int64_t loam_platform_plat_intern_fn(loam_fn handler);
void loam_platform_plat_invoke_fn(int64_t id);
/* Reactive prop thunks: interned like handlers, called for their result. */
int64_t loam_platform_plat_intern_int_fn(loam_fn thunk);
int64_t loam_platform_plat_invoke_int_fn(int64_t id);
int64_t loam_platform_plat_intern_bool_fn(loam_fn thunk);
int64_t loam_platform_plat_invoke_bool_fn(int64_t id);
int64_t loam_platform_plat_intern_str_fn(loam_fn thunk);
loam_str loam_platform_plat_invoke_str_fn(int64_t id);
void loam_platform_plat_sig_bind_int(int64_t id, int64_t value);
int64_t loam_platform_plat_sig_gen(int64_t id);
void loam_platform_plat_sig_free(int64_t id);
void loam_platform_plat_edit_reset(int64_t slot);
void loam_platform_plat_intern_free(int64_t id);
/* Recoverable trap (`zeus.Boundary`): "" on success, else the panic message. */
loam_str loam_platform_plat_boundary(loam_fn build);
/* Router history mirror (host back stack). */
void loam_platform_plat_history_push(loam_str path);
void loam_platform_plat_history_replace(loam_str path);
void loam_platform_plat_history_back(void);
int64_t loam_platform_plat_overlay_scroll(void);
int64_t loam_platform_plat_inset_top(void);
int64_t loam_platform_plat_inset_right(void);
int64_t loam_platform_plat_inset_bottom(void);
int64_t loam_platform_plat_alloc_count(void);
int64_t loam_platform_plat_inset_left(void);

/* Loam engine entry points (packages/zeus/std/zeus.loam). Cocoa trampolines through these. */
void loam_zeus_engine_layout(int32_t width, int32_t height);
void loam_zeus_engine_paint(void);
int32_t loam_zeus_engine_step(void);
/* One host frame with the host-supplied delta (ms). `zeus_step(dt)` calls this. */
int32_t loam_zeus_engine_step_dt(int32_t dt_ms);
void loam_zeus_engine_set_reduced_motion(int32_t on);
/* ms until the next async timer is due (0 = none; -1 = a spawn waits). */
int32_t loam_zeus_engine_next_ms(void);
int32_t loam_zeus_engine_click(int32_t x, int32_t y);
int32_t loam_zeus_engine_scroll(int32_t x, int32_t y, int32_t dx, int32_t dy);
int32_t loam_zeus_engine_scroll_step(int32_t x, int32_t y, int32_t dx, int32_t dy);
int32_t loam_zeus_engine_drag(int32_t x, int32_t y);
int32_t loam_zeus_engine_hover(int32_t x, int32_t y);
void loam_zeus_engine_mouseup(void);
int32_t loam_zeus_engine_over_button(void);
loam_str loam_zeus_engine_cursor(void);
/* Visible semantic tree, one `depth\trole\tlabel` line per node (§2.3). */
loam_str loam_zeus_engine_a11y_dump(void);
/* Inspector dumps (devtools §2.4). */
loam_str loam_zeus_engine_tree_dump(void);
loam_str loam_zeus_engine_signals_dump(void);
/* Hot reload (§2.5): restore signal values from a signals dump. Returns the
   number of slots applied. */
int32_t loam_zeus_engine_state_load(loam_str snapshot);

/* Router host entry points (std:router). A host calls `open_url` for a deep
   link and `history_back` for an OS back button. */
void loam_router_engine_open_url(loam_str url);
void loam_router_engine_history_back(void);
void loam_zeus_engine_key_apply(int32_t sig, int32_t mode, int32_t value, int32_t lo,
                               int32_t hi);
void loam_zeus_engine_fill_focus(void);
int32_t loam_zeus_engine_focus_depth(void);
int32_t loam_zeus_engine_focus_node(int32_t i);
int32_t loam_zeus_engine_focus_ctx(int32_t i);
int32_t loam_zeus_engine_focus_step(int32_t back);
int32_t loam_zeus_engine_focus_captures_text(void);
int32_t loam_zeus_engine_key(int32_t key);
int32_t loam_zeus_engine_key_up(int32_t key, int32_t mods);
void loam_zeus_engine_set_mods(int32_t mods);
int32_t loam_zeus_engine_insert(loam_str text);
int32_t loam_zeus_engine_marked(loam_str text);
void loam_zeus_engine_picked_image(loam_str src, int32_t w, int32_t h);

void zeus_layout(int64_t width, int64_t height);
int zeus_step(float dt);
void zeus_paint(void *ctx, ZeusDraw draw);
int zeus_handle_click(int64_t x, int64_t y);
int zeus_handle_hover(int64_t x, int64_t y);
int zeus_over_button(void);
/** CSS cursor name under the pointer; "" or unknown = default arrow. */
const char *zeus_cursor(void);
int zeus_handle_key(int key);
/** Key release. Hosts that deliver key-up call this; `on_key_up` fires. */
int zeus_handle_key_up(int key, int mods);
/** Insert UTF-8 into the focused input (IME commit, paste). */
int zeus_handle_text(const char *utf8, int n);
/** IME composition overlay; n = 0 clears the mark. */
int zeus_handle_marked(const char *utf8, int n);
int zeus_handle_scroll(int64_t x, int64_t y, int64_t dx, int64_t dy);
/** Discrete scroll (mouse notch / web line wheel): delta only, no momentum. */
int zeus_handle_scroll_step(int64_t x, int64_t y, int64_t dx, int64_t dy);
int zeus_handle_drag(int64_t x, int64_t y);
void zeus_handle_mouseup(void);
const char *zeus_window_title(void);

/** Live title updates: a host registers a callback so `plat_set_title` can
 *  retitle the open window (router `Meta.title` on navigation). */
void zeus_set_title_hook(void (*fn)(const char *));
int64_t zeus_window_width(void);
int64_t zeus_window_height(void);
void zeus_set_window_size(int64_t width, int64_t height);
/**
 * Pre-window "initial view" size on desktop hosts. Until the native window
 * is created (zeus_window_opened), zeus.window_size() reports this instead of
 * the placeholder default so desktop apps default to a screen-filling
 * window. Hosts register lazy getters (no AppKit calls at registration).
 */
void zeus_set_initial_view(int64_t (*width)(void), int64_t (*height)(void));
void zeus_window_opened(void);

/* Built-in placeholder window size used before a host reports a real view.
 * Desktop hosts treat a request for exactly this as "no preference" and open
 * the window at the device screen size instead. */
#define ZEUS_DEFAULT_WIN_W 640
#define ZEUS_DEFAULT_WIN_H 480

/* ── the app image, as a host sees it ────────────────────────────────────────

   A normal build is one program: the app *defines* the engine entry points above
   and `zeus_plat.c` / `mac.m` call them directly. A `zeli serve` host instead
   keeps the platform layer and the window and loads the app as a shared image,
   so those entry points belong to whatever image is loaded *now*.

   That is why the calls go through a table in a host build. A plain dlopen is not
   enough: a direct call binds once (lazy binding caches the address), so after an
   edit the host would keep painting the tree the *first* image built, with the
   window still up and nothing to say it had gone stale. The loader re-reads these
   from the new image on every load.

   Without LOAM_HOST_BUILD this whole section is absent, the calls are the direct
   ones they have always been, and what ships is unchanged. */
#ifdef LOAM_HOST_BUILD
typedef struct {
    void (*layout)(int32_t width, int32_t height);
    void (*paint)(void);
    int32_t (*step)(void);
    int32_t (*step_dt)(int32_t dt_ms);
    void (*set_reduced_motion)(int32_t on);
    int32_t (*next_ms)(void);
    int32_t (*click)(int32_t x, int32_t y);
    int32_t (*scroll)(int32_t x, int32_t y, int32_t dx, int32_t dy);
    int32_t (*scroll_step)(int32_t x, int32_t y, int32_t dx, int32_t dy);
    int32_t (*drag)(int32_t x, int32_t y);
    int32_t (*hover)(int32_t x, int32_t y);
    void (*mouseup)(void);
    int32_t (*over_button)(void);
    loam_str (*cursor)(void);
    loam_str (*a11y_dump)(void);
    loam_str (*tree_dump)(void);
    loam_str (*signals_dump)(void);
    int32_t (*state_load)(loam_str snapshot);
    void (*key_apply)(int32_t sig, int32_t mode, int32_t value, int32_t lo, int32_t hi);
    void (*fill_focus)(void);
    int32_t (*focus_depth)(void);
    int32_t (*focus_node)(int32_t i);
    int32_t (*focus_ctx)(int32_t i);
    int32_t (*focus_step)(int32_t back);
    int32_t (*focus_captures_text)(void);
    int32_t (*key)(int32_t key);
    int32_t (*key_up)(int32_t key, int32_t mods);
    void (*set_mods)(int32_t mods);
    int32_t (*insert)(loam_str text);
    int32_t (*marked)(loam_str text);
    void (*picked_image)(loam_str src, int32_t w, int32_t h);
} ZeusAppApi;

extern ZeusAppApi zeus_app_api;

/* A host: it owns the loop, and loads app images on its own. `zeus_set_host_mode`
   makes `loam_zeus_plat_run` yield (the app registers and returns);
   `zeus_set_host_hooks` hands the platform layer the loader that
   `zeus.host_run(path)` calls. */
void zeus_set_host_mode(void);
void zeus_set_host_hooks(void (*run)(const char *path));

/* One line each, so the call sites read as the engine they call. */
#define loam_zeus_engine_layout zeus_app_api.layout
#define loam_zeus_engine_paint zeus_app_api.paint
#define loam_zeus_engine_step zeus_app_api.step
#define loam_zeus_engine_step_dt zeus_app_api.step_dt
#define loam_zeus_engine_set_reduced_motion zeus_app_api.set_reduced_motion
#define loam_zeus_engine_next_ms zeus_app_api.next_ms
#define loam_zeus_engine_click zeus_app_api.click
#define loam_zeus_engine_scroll zeus_app_api.scroll
#define loam_zeus_engine_scroll_step zeus_app_api.scroll_step
#define loam_zeus_engine_drag zeus_app_api.drag
#define loam_zeus_engine_hover zeus_app_api.hover
#define loam_zeus_engine_mouseup zeus_app_api.mouseup
#define loam_zeus_engine_over_button zeus_app_api.over_button
#define loam_zeus_engine_cursor zeus_app_api.cursor
#define loam_zeus_engine_a11y_dump zeus_app_api.a11y_dump
#define loam_zeus_engine_tree_dump zeus_app_api.tree_dump
#define loam_zeus_engine_signals_dump zeus_app_api.signals_dump
#define loam_zeus_engine_state_load zeus_app_api.state_load
#define loam_zeus_engine_key_apply zeus_app_api.key_apply
#define loam_zeus_engine_fill_focus zeus_app_api.fill_focus
#define loam_zeus_engine_focus_depth zeus_app_api.focus_depth
#define loam_zeus_engine_focus_node zeus_app_api.focus_node
#define loam_zeus_engine_focus_ctx zeus_app_api.focus_ctx
#define loam_zeus_engine_focus_step zeus_app_api.focus_step
#define loam_zeus_engine_focus_captures_text zeus_app_api.focus_captures_text
#define loam_zeus_engine_key zeus_app_api.key
#define loam_zeus_engine_key_up zeus_app_api.key_up
#define loam_zeus_engine_set_mods zeus_app_api.set_mods
#define loam_zeus_engine_insert zeus_app_api.insert
#define loam_zeus_engine_marked zeus_app_api.marked
#define loam_zeus_engine_picked_image zeus_app_api.picked_image
#endif /* LOAM_HOST_BUILD */

#endif
