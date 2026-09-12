/* zeus_rt.h — platform seam and handle types for `import "std:zeus"`.
 *
 * Node and Signal are typed handles (id + struct). The retained tree,
 * layout, paint, and hit-test live in std/zeus.yuga. Empty fns in that
 * file (plat_*, keyboard) map to the hooks below. Cocoa talks to the
 * engine through zeus_layout / zeus_paint / zeus_handle_*.
 */
#ifndef ZEUS_RT_H
#define ZEUS_RT_H

#include <stdint.h>

#ifdef YUGA_RT_H
/* yuga_str already defined in generated C */
#else
#include "yuga_rt.h"
#endif

typedef struct {
    int64_t id;
} Node;

typedef struct {
    int64_t id;
} Signal;

/* --- Yuga API: import "std:zeus" → yuga_zeus_* --- */

void yuga_zeus_raw_init(void);
void yuga_zeus_open_window(yuga_str title, int64_t width, int64_t height);
void yuga_zeus_raw_run(void);

Signal yuga_zeus_signal(int64_t value);
/* Signal slot allocation with free-list reuse (arena compaction). The int
   form writes the int mirror slot; the zero form reserves a placeholder slot
   for a typed payload the caller binds afterwards. */
int64_t yuga_zeus_sig_alloc_int(int64_t value);
int64_t yuga_zeus_sig_alloc_zero(void);
void yuga_zeus_sig_free(int64_t id);
void yuga_zeus_hook_begin(void);
Signal yuga_zeus_hook_signal(int64_t value);
int64_t yuga_zeus_get(Signal sig);
void yuga_zeus_set(Signal sig, int64_t value);
void yuga_zeus_inc(Signal sig, int64_t delta);

void yuga_zeus_sig_bind(int64_t id, const void *src, int64_t n);
void yuga_zeus_sig_load(int64_t id, void *dst, int64_t n);
int64_t yuga_zeus_sig_changed(int64_t id, const void *src, int64_t n);
int64_t yuga_zeus_sig_gen(int64_t id);
Signal yuga_zeus_appearance(void);
void yuga_zeus_theme(int64_t slot, int64_t light, int64_t dark);
int64_t yuga_zeus_role(int64_t slot);

Node yuga_zeus_raw_row(void);
Node yuga_zeus_raw_col(void);
Node yuga_zeus_raw_box(void);
Node yuga_zeus_grid(int64_t columns, int64_t gap);
Node yuga_zeus_grid_auto(int64_t min_width, int64_t gap);
Node yuga_zeus_raw_label(yuga_str text);
Node yuga_zeus_raw_button(yuga_str text);
Node yuga_zeus_raw_spacer(void);
Node yuga_zeus_raw_progress(void);
Node yuga_zeus_raw_slider(void);
Node yuga_zeus_raw_toggle(void);
Node yuga_zeus_raw_check(yuga_str markup);
Node yuga_zeus_raw_overlay(void);
Node yuga_zeus_raw_input(yuga_str placeholder);
Node yuga_zeus_raw_scroll(void);
Node yuga_zeus_raw_svg(yuga_str markup);

Node yuga_zeus_child(Node parent, Node node);
Node yuga_zeus_root(Node node);

Node yuga_zeus_grow(Node node, int64_t weight);
Node yuga_zeus_shrink(Node node, int64_t weight);
Node yuga_zeus_pad(Node node, int64_t all);
Node yuga_zeus_padX(Node node, int64_t px);
Node yuga_zeus_padY(Node node, int64_t py);
Node yuga_zeus_padTop(Node node, int64_t px);
Node yuga_zeus_padRight(Node node, int64_t px);
Node yuga_zeus_padBottom(Node node, int64_t px);
Node yuga_zeus_padLeft(Node node, int64_t px);
Node yuga_zeus_margin(Node node, int64_t all);
Node yuga_zeus_marginX(Node node, int64_t px);
Node yuga_zeus_marginY(Node node, int64_t py);
Node yuga_zeus_marginTop(Node node, int64_t px);
Node yuga_zeus_marginRight(Node node, int64_t px);
Node yuga_zeus_marginBottom(Node node, int64_t px);
Node yuga_zeus_marginLeft(Node node, int64_t px);
Node yuga_zeus_border(Node node, int64_t width);
Node yuga_zeus_border_color(Node node, int64_t rgb);
Node yuga_zeus_gap(Node node, int64_t g);
Node yuga_zeus_gap_row(Node node, int64_t g);
Node yuga_zeus_gap_col(Node node, int64_t g);
Node yuga_zeus_bg(Node node, int64_t rgb);
Node yuga_zeus_fg(Node node, int64_t rgb);
Node yuga_zeus_w(Node node, int64_t px);
Node yuga_zeus_h(Node node, int64_t px);
Node yuga_zeus_width_pct(Node node, int64_t pct);
Node yuga_zeus_height_pct(Node node, int64_t pct);
Node yuga_zeus_min_w(Node node, int64_t px);
Node yuga_zeus_max_w(Node node, int64_t px);
Node yuga_zeus_min_h(Node node, int64_t px);
Node yuga_zeus_max_h(Node node, int64_t px);
Node yuga_zeus_aspect(Node node, int64_t aw, int64_t ah);
Node yuga_zeus_flex_row(Node node);
Node yuga_zeus_flex_col(Node node);
Node yuga_zeus_flex_row_reverse(Node node);
Node yuga_zeus_flex_col_reverse(Node node);
Node yuga_zeus_flex_wrap(Node node);
Node yuga_zeus_align_self(Node node, int64_t mode);
Node yuga_zeus_span(Node node, int64_t columns);
Node yuga_zeus_radius(Node node, int64_t px);
Node yuga_zeus_justify(Node node, int64_t mode);
Node yuga_zeus_align(Node node, int64_t mode);
Node yuga_zeus_position(Node node, int64_t mode);
Node yuga_zeus_top(Node node, int64_t px);
Node yuga_zeus_right(Node node, int64_t px);
Node yuga_zeus_bottom(Node node, int64_t px);
Node yuga_zeus_left(Node node, int64_t px);
Node yuga_zeus_z_index(Node node, int64_t z);
Node yuga_zeus_opacity(Node node, int64_t pct);
Node yuga_zeus_overflow(Node node, int64_t mode);
Node yuga_zeus_font(Node node, int64_t px);

Node yuga_zeus_bind(Node label, Signal sig);
Node yuga_zeus_bind_n(Node label, int64_t n);
Node yuga_zeus_digits(int64_t n);
Node yuga_zeus_on_click_inc(Node button, Signal sig);
Node yuga_zeus_on_click_toggle(Node node, Signal sig);
Node yuga_zeus_on_click_set(Node node, Signal sig, int64_t value);
Node yuga_zeus_on_click_add(Node node, Signal sig, int64_t delta);
Node yuga_zeus_show(Node node, Signal sig);
Node yuga_zeus_show_eq(Node node, Signal sig, int64_t value);
Node yuga_zeus_show_ne(Node node, Signal sig, int64_t value);
Node yuga_zeus_key_context(Node node, yuga_str name);
Node yuga_zeus_focusable(Node node);
Node yuga_zeus_capture_text(Node node);
Node yuga_zeus_on_action(Node node, yuga_str action, Signal sig, int64_t mode, int64_t value);
Node yuga_zeus_on_key_fn(Node node, yuga_str action);
Node yuga_zeus_on_range(Node node, yuga_str action, Signal sig, int64_t delta, int64_t lo,
                       int64_t hi);
void yuga_zeus_on_action_global(yuga_str action, Signal sig, int64_t mode, int64_t value);
void yuga_zeus_map_key(yuga_str spec, yuga_str action, yuga_str ctx);
void yuga_zeus_remap_key(yuga_str spec, yuga_str action, yuga_str ctx);
int zeus_handle_key_ev(int key, int mods);

Node yuga_zeus_show_ge(Node node, Signal sig, int64_t value);
Node yuga_zeus_show_le(Node node, Signal sig, int64_t value);
Node yuga_zeus_raw_hover(Node node, Signal sig);
Node yuga_zeus_hover_delay(Node node, int64_t ms);
Node yuga_zeus_hover_leave(Node node, int64_t ms);
Node yuga_zeus_pulse(Node node);
Node yuga_zeus_dismiss(Node node);
Node yuga_zeus_keys(Node node, Signal sig, int64_t lo, int64_t hi);
Node yuga_zeus_keys_page(Node node, Signal sig);

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
    void (*clip)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h);
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
       NULL hosts fall back to a solid `c0` fill. */
    void (*fill_g)(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                   int64_t c0, int64_t c1, int64_t axis);
} ZeusDraw;

void zeus_set_platform(void (*run)(void),
                      void (*measure)(const char *s, int64_t px, int64_t *w, int64_t *h),
                      void (*redraw)(void));
void zeus_set_pick_image(void (*pick)(char *out, int cap, int64_t *w, int64_t *h));
void zeus_picked_image(const char *src, int64_t w, int64_t h);
void zeus_bind_draw(ZeusDraw draw);

/* Empty std/zeus.yuga fns → these C symbols. */
void yuga_zeus_plat_run(void);
int64_t yuga_zeus_plat_headless(void);
void yuga_zeus_plat_fill(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                        int64_t radius);
void yuga_zeus_plat_fill_a(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                          int64_t radius, int64_t alpha);
void yuga_zeus_plat_fill_g(int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                          int64_t c1, int64_t axis);
void yuga_zeus_plat_text(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font);
/* Current global font family ("" = host default) and the hook a host
   registers to load one. See zeus_plat.c for why the family is global. */
const char *zeus_font_family(void);
void zeus_set_font_hooks(int (*load)(const char *family, const char *src),
                         void (*set_family)(const char *family));
void yuga_platform_plat_set_font_family(yuga_str name);
int64_t yuga_platform_plat_load_font(yuga_str family, yuga_str src);

void yuga_zeus_plat_text_rot(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font,
                             int64_t deg);
void yuga_zeus_plat_text_int(int64_t x, int64_t y, int64_t v, int64_t rgb, int64_t font);
void yuga_zeus_plat_measure(yuga_str s, int64_t px, int64_t *w, int64_t *h);
void yuga_zeus_plat_measure_int(int64_t v, int64_t px, int64_t *w, int64_t *h);
void yuga_zeus_plat_measure_wrap(yuga_str s, int64_t px, int64_t max_w, int64_t *w, int64_t *h);
void yuga_zeus_plat_text_wrap(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font,
                             int64_t max_w);
void yuga_zeus_plat_set_window(yuga_str title, int64_t width, int64_t height);
int64_t yuga_zeus_plat_view_width(void);
int64_t yuga_zeus_plat_view_height(void);
void yuga_zeus_plat_svg(int64_t x, int64_t y, int64_t w, int64_t h, yuga_str markup,
                       int64_t rgb, int64_t alpha);
void yuga_zeus_plat_image(int64_t x, int64_t y, int64_t w, int64_t h, yuga_str src,
                         int64_t radius, int64_t alpha, int64_t fit);
yuga_str yuga_zeus_plat_pick_image(int64_t *w, int64_t *h);
void yuga_zeus_plat_save(void);
void yuga_zeus_plat_clip(int64_t x, int64_t y, int64_t w, int64_t h);
void yuga_zeus_plat_restore(void);

void zeus_set_insets(int64_t top, int64_t right, int64_t bottom, int64_t left);
int64_t yuga_zeus_plat_overlay_scroll(void);
int64_t yuga_zeus_plat_inset_top(void);
int64_t yuga_zeus_plat_inset_right(void);
int64_t yuga_zeus_plat_inset_bottom(void);
int64_t yuga_zeus_plat_inset_left(void);

/* std/zeus/platform.yuga FFI (aliases of yuga_zeus_plat_*). */
void yuga_platform_plat_run(void);
int64_t yuga_platform_plat_headless(void);
void yuga_platform_plat_fill(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                             int64_t radius);
void yuga_platform_plat_fill_a(int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                               int64_t radius, int64_t alpha);
void yuga_platform_plat_fill_g(int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                               int64_t c1, int64_t axis);
void yuga_platform_plat_text(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font);
void yuga_platform_plat_text_rot(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font,
                                 int64_t deg);
void yuga_platform_plat_text_int(int64_t x, int64_t y, int64_t v, int64_t rgb, int64_t font);
void yuga_platform_plat_measure(yuga_str s, int64_t px, int64_t *w, int64_t *h);
void yuga_platform_plat_measure_int(int64_t v, int64_t px, int64_t *w, int64_t *h);
void yuga_platform_plat_measure_wrap(yuga_str s, int64_t px, int64_t max_w, int64_t *w,
                                     int64_t *h);
void yuga_platform_plat_text_wrap(int64_t x, int64_t y, yuga_str s, int64_t rgb, int64_t font,
                                  int64_t max_w);
void yuga_platform_plat_set_window(yuga_str title, int64_t width, int64_t height);
int64_t yuga_platform_plat_view_width(void);
int64_t yuga_platform_plat_view_height(void);
void yuga_platform_plat_svg(int64_t x, int64_t y, int64_t w, int64_t h, yuga_str markup,
                            int64_t rgb, int64_t alpha);
void yuga_platform_plat_image(int64_t x, int64_t y, int64_t w, int64_t h, yuga_str src,
                              int64_t radius, int64_t alpha, int64_t fit);
yuga_str yuga_platform_plat_pick_image(int64_t *w, int64_t *h);
void yuga_platform_plat_save(void);
int64_t yuga_platform_plat_mem_kb(void);
void yuga_platform_plat_clip(int64_t x, int64_t y, int64_t w, int64_t h);
void yuga_platform_plat_restore(void);
int64_t yuga_platform_plat_key_intern(yuga_str name);
int64_t yuga_platform_plat_key_intern_action(yuga_str action);
void yuga_platform_plat_key_reset(void);
void yuga_platform_plat_map_key(yuga_str spec, yuga_str action, int64_t ctx);
int64_t yuga_platform_plat_key_ev(int64_t key, int64_t mods);
int64_t yuga_platform_plat_edit_append(int64_t slot, int64_t key);
int64_t yuga_platform_plat_edit_back(int64_t slot);
int64_t yuga_platform_plat_edit_del(int64_t slot);
int64_t yuga_platform_plat_edit_len(int64_t slot);
yuga_str yuga_platform_plat_edit_text(int64_t slot);
yuga_str yuga_platform_plat_edit_shown(int64_t slot);
int64_t yuga_platform_plat_edit_set(int64_t slot, yuga_str text);
int64_t yuga_platform_plat_edit_insert(int64_t slot, yuga_str text);
int64_t yuga_platform_plat_edit_caret(int64_t slot);
int64_t yuga_platform_plat_edit_anchor(int64_t slot);
int64_t yuga_platform_plat_edit_mark(int64_t slot, yuga_str text);
void yuga_platform_plat_edit_metrics(int64_t slot, int64_t wrap_w, int64_t font);
int64_t yuga_platform_plat_edit_click(int64_t slot, int64_t x, int64_t y, int64_t wrap_w,
                                      int64_t font);
int64_t yuga_platform_plat_edit_move(int64_t slot, int64_t dir, int64_t extend);
int64_t yuga_platform_plat_edit_caret_x(int64_t slot);
int64_t yuga_platform_plat_edit_caret_y(int64_t slot);
int64_t yuga_platform_plat_edit_line_h(int64_t slot);
int64_t yuga_platform_plat_intern_fn(yuga_fn handler);
void yuga_platform_plat_invoke_fn(int64_t id);
/* Reactive prop thunks: interned like handlers, called for their result. */
int64_t yuga_platform_plat_intern_int_fn(yuga_fn thunk);
int64_t yuga_platform_plat_invoke_int_fn(int64_t id);
int64_t yuga_platform_plat_intern_bool_fn(yuga_fn thunk);
int64_t yuga_platform_plat_invoke_bool_fn(int64_t id);
int64_t yuga_platform_plat_intern_str_fn(yuga_fn thunk);
yuga_str yuga_platform_plat_invoke_str_fn(int64_t id);
void yuga_platform_plat_sig_bind_int(int64_t id, int64_t value);
int64_t yuga_platform_plat_sig_gen(int64_t id);
void yuga_platform_plat_sig_free(int64_t id);
void yuga_platform_plat_edit_reset(int64_t slot);
void yuga_platform_plat_intern_free(int64_t id);
int64_t yuga_platform_plat_overlay_scroll(void);
int64_t yuga_platform_plat_inset_top(void);
int64_t yuga_platform_plat_inset_right(void);
int64_t yuga_platform_plat_inset_bottom(void);
int64_t yuga_platform_plat_inset_left(void);

/* Yuga engine entry points (std/zeus.yuga). Cocoa trampolines through these. */
void yuga_zeus_engine_layout(int64_t width, int64_t height);
void yuga_zeus_engine_paint(void);
int64_t yuga_zeus_engine_step(void);
/* ms until the next async timer is due (0 = none; -1 = a spawn waits). */
int64_t yuga_zeus_engine_next_ms(void);
int64_t yuga_zeus_engine_click(int64_t x, int64_t y);
int64_t yuga_zeus_engine_scroll(int64_t x, int64_t y, int64_t dx, int64_t dy);
int64_t yuga_zeus_engine_scroll_step(int64_t x, int64_t y, int64_t dx, int64_t dy);
int64_t yuga_zeus_engine_drag(int64_t x, int64_t y);
int64_t yuga_zeus_engine_hover(int64_t x, int64_t y);
void yuga_zeus_engine_mouseup(void);
int64_t yuga_zeus_engine_over_button(void);
yuga_str yuga_zeus_engine_cursor(void);
void yuga_zeus_engine_key_apply(int64_t sig, int64_t mode, int64_t value, int64_t lo,
                               int64_t hi);
void yuga_zeus_engine_fill_focus(void);
int64_t yuga_zeus_engine_focus_depth(void);
int64_t yuga_zeus_engine_focus_node(int64_t i);
int64_t yuga_zeus_engine_focus_ctx(int64_t i);
int64_t yuga_zeus_engine_focus_step(int64_t back);
int64_t yuga_zeus_engine_focus_captures_text(void);
int64_t yuga_zeus_engine_key(int64_t key);
int64_t yuga_zeus_engine_key_up(int64_t key, int64_t mods);
void yuga_zeus_engine_set_mods(int64_t mods);
int64_t yuga_zeus_engine_insert(yuga_str text);
int64_t yuga_zeus_engine_marked(yuga_str text);
void yuga_zeus_engine_picked_image(yuga_str src, int64_t w, int64_t h);

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

#endif
