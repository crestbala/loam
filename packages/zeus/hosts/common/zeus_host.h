/* zeus_host.h — the host contract: what a Zeus host (mac.m, linux.c, ios.m,
 * android.c, wasm.c) registers and calls. The retained tree, layout, paint and
 * hit-test are Loam (packages/zeus/src). Hosts call the engine's Loam exports
 * (`loam_zeus_engine_*`) directly, register their callbacks here, and paint
 * through zeus_paint. Generated C does not include this header: the
 * `host_contract` stanza checks that it agrees with what codegen emits.
 */
#ifndef ZEUS_HOST_H
#define ZEUS_HOST_H

#include "loam_abi.h"

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
    /* Multiply subsequent drawing by `a` (0..255) until the matching restore.
       Text has no alpha argument; present() wraps faded text in this. The
       callback itself saves, so the caller only restores. NULL hosts skip
       it and text stays opaque. */
    void (*alpha)(void *ctx, int64_t a);
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
    /* A vector path: `n` ints of commands, `0` move x y, `1` line x y,
       `2` cubic x1 y1 x2 y2 x y, `3` close, coordinates in 1/64 of a layout
       point. `width` 0 fills it (non-zero winding) in `rgb`; otherwise it is
       stroked `width`/64 wide with `style` = cap + 4 * join (0 butt / miter,
       1 round, 2 square / bevel). alpha is 0..255. Icons, charts and every
       `Svg` arrive this way: the engine parses the markup, never the host. */
    void (*path)(void *ctx, const int32_t *cmds, int64_t n, int64_t rgb, int64_t alpha,
                 int64_t width, int64_t style);
} ZeusDraw;

/* One frame, in the format of packages/zeus/docs/frame-format.md: `n` ops of
   `stride` int32 (`kind x y w h rgb radius alpha font value str`), `str`
   indexing `strs` (-1 for none), path ops indexing the `np` ints of `path`.
   The buffers are the engine's and valid only for the call that receives them. */
#define ZEUS_FRAME_MAGIC 0x5355455A /* "ZEUS" */
#define ZEUS_FRAME_VERSION 1
#define ZEUS_FRAME_STRIDE 11
typedef struct {
    int32_t version, stride;
    const int32_t *ops;
    int64_t n;
    const loam_str *strs;
    int64_t ns;
    const int32_t *path;
    int64_t np;
} ZeusFrame;

/* Take each frame whole instead of as ZeusDraw calls (NULL restores replay).
   For a host that decodes the buffer itself: the web host hands it to JS. */
void zeus_bind_frame(void (*sink)(const ZeusFrame *f));

void zeus_set_platform(void (*run)(void),
                      void (*measure)(const char *s, int64_t px, int64_t *w, int64_t *h),
                      void (*redraw)(void));
void zeus_set_pick_image(void (*pick)(char *out, int cap, int64_t *w, int64_t *h));
void zeus_set_image_size(void (*size)(const char *src, int64_t *w, int64_t *h));
void zeus_bind_draw(ZeusDraw draw);

/* Media playback seam. A native host registers one table; the web routes the
   same calls to JS imports. The null player, the pick queue and fullscreen
   live in `zeuscore/mediastate.loam`. Frames reach the screen through the
   ordinary `image` draw op with a `"media:<slot>"` src, which the host
   recognises. `text` writes at most `cap - 1` bytes plus a NUL into `out`.
   `picked` is the host telling the runtime a picker or a drop produced a file. */
typedef struct {
    int64_t (*open)(int64_t slot, const char *src);
    void (*cmd)(int64_t slot, int64_t cmd, int64_t value);
    int64_t (*get)(int64_t slot, int64_t what);
    void (*text)(int64_t slot, int64_t what, char *out, int cap);
    void (*pick)(void);
    /* Audio files, several at once. NULL falls back to `pick`. */
    void (*pick_audio)(void);
    /* Voice tools, async: 1 maps where a voice is, 2 writes the file minus
       its voice. Returns 1 when the job started; the result is `out`, then
       `out.done` or `out.fail`. NULL (or 0) means this host has neither. */
    int64_t (*voice)(int64_t what, const char *src, const char *out);
    int64_t (*fullscreen)(int64_t on);
} ZeusMedia;
void zeus_set_media(ZeusMedia media);
/* `zeuscore/mediastate.loam` and `imagesize.loam`: bodyless hooks. One bit of
   `media_host_caps` per non-NULL `ZeusMedia` callback, plus 256 on wasm. */
int32_t loam_imagesize_image_host_bound(void);
int32_t loam_mediastate_native_caps(void);
int32_t loam_mediastate_native_open(int32_t slot, loam_str src);
void loam_mediastate_native_cmd(int32_t slot, int32_t command, int32_t value);
int32_t loam_mediastate_native_get(int32_t slot, int32_t what);
loam_str loam_mediastate_native_text(int32_t slot, int32_t what);
void loam_mediastate_native_pick(void);
void loam_mediastate_native_pick_audio(void);
int32_t loam_mediastate_native_voice(int32_t what, loam_str src, loam_str out);
int32_t loam_mediastate_native_fullscreen(int32_t on);
void loam_mediastate_host_redraw(void);

/* Phase 4: the ONE blit of a frame Zeus rasterized itself — the sole place a
   host may touch our pixels.

   `px` is the framebuffer's first byte: `w * h * 4` bytes of BGRA8
   premultiplied sRGB, owned by the Loam side. A host wraps those bytes with a
   no-copy provider (native CGImage / web typed-array view) and rebuilds the
   wrapper only when `gen` changes — `gen` moves on reallocation (a size or
   backing-scale change), never per frame. No pixel is copied, and no host may
   read past `w * h * 4`.

   Returns 1 when the host presented the frame, 0 when it has nowhere to present.
   The hook is NULL until a host registers one, so every other host — Linux,
   iOS, Android, the web loader, and headless — links and runs unchanged, with
   blitting a no-op. */
typedef int64_t (*ZeusBlit)(void *ctx, const uint8_t *px, int64_t w, int64_t h,
                            int64_t gen, int64_t scale);
void zeus_set_blit(void *ctx, ZeusBlit fn);

/* Empty packages/zeus/src/zeus.loam fns → these C symbols. */
void loam_zeus_plat_run(void);
/* Current global font family ("" = host default) and the hook a host
   registers to load one. See zeus_host.c for why the family is global. */
const char *zeus_font_family(void);
void zeus_set_font_hooks(int (*load)(const char *family, const char *src),
                         void (*set_family)(const char *family));
/* Bytes for in-tree metrics. Desktop/iOS/Android default to reading `src` as a
   file; the web host (no filesystem) installs a hook that fetches it. */
void zeus_set_font_bytes_hook(loam_str (*fetch)(loam_str src));
void loam_platform_plat_set_font_family(loam_str name);
int32_t loam_platform_plat_load_font(loam_str family, loam_str src);

void loam_zeus_plat_measure(loam_str s, int32_t px, int32_t *w, int32_t *h);
void loam_zeus_plat_measure_int(int32_t v, int32_t px, int32_t *w, int32_t *h);
void loam_zeus_plat_image_size(loam_str src, int32_t *w, int32_t *h);
loam_str loam_zeus_plat_pick_image(int32_t *w, int32_t *h);

/* The engine side of the blit: dispatched to the hook `zeus_set_blit`
   registered, or a no-op returning 0 when no host did. */
int64_t loam_zeus_plat_blit(const uint8_t *px, int64_t w, int64_t h, int64_t gen,
                           int64_t scale);

void zeus_set_insets(int64_t top, int64_t right, int64_t bottom, int64_t left);

/* packages/zeus/src/zeuscore/platform.loam FFI (aliases of loam_zeus_plat_*). */
void loam_platform_plat_run(void);
/* The seam state is Loam (`zeuscore/platform.loam`). Hosts reach it through
   `zeus_set_insets`, `zeus_window_width`, ... which call these. */
int32_t loam_platform_plat_headless(void);
void loam_platform_plat_set_insets(int32_t top, int32_t right, int32_t bottom, int32_t left);
void loam_platform_plat_set_titlebar_box(int32_t height, int32_t lead, int32_t trail);
int32_t loam_platform_host_window_width(void);
int32_t loam_platform_host_window_height(void);
void loam_platform_host_set_window_size(int32_t width, int32_t height);
int32_t loam_platform_host_window_chrome(void);
void loam_platform_host_window_opened(void);
void loam_platform_host_retitle(void);
/* Its bodyless hooks, answered from what a native host registered. */
int32_t loam_platform_native_run_bound(void);
int32_t loam_platform_native_initial_w(void);
int32_t loam_platform_native_initial_h(void);
int64_t loam_platform_native_clock_ms(void);
void loam_platform_native_title_changed(loam_str title);
void loam_platform_native_clipboard_write(loam_str text);
void loam_platform_plat_measure(loam_str s, int32_t px, int32_t *w, int32_t *h);
loam_str loam_platform_plat_font_bytes(loam_str src);
void loam_platform_plat_measure_int(int32_t v, int32_t px, int32_t *w, int32_t *h);
void loam_platform_plat_measure_span(loam_str s, int32_t from, int32_t to, int32_t px,
                                     int32_t *w, int32_t *h);
void loam_platform_plat_image_size(loam_str src, int32_t *w, int32_t *h);
loam_str loam_platform_plat_pick_image(int32_t *w, int32_t *h);
/* `zeuscore/replay.loam`'s forwarders into the bound ZeusDraw (zeus_host.c). */
int32_t loam_replay_draw_caps(void);
void loam_replay_draw_fill(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rgb, int32_t r);
void loam_replay_draw_fill_a(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rgb, int32_t r,
                             int32_t a);
void loam_replay_draw_text(int32_t x, int32_t y, const loam_vec *strs, int32_t si, int32_t rgb,
                           int32_t font);
void loam_replay_draw_text_rot(int32_t x, int32_t y, const loam_vec *strs, int32_t si,
                               int32_t rgb, int32_t font, int32_t deg);
void loam_replay_draw_save(void);
void loam_replay_draw_clip(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r);
void loam_replay_draw_restore(void);
void loam_replay_draw_alpha(int32_t a);
void loam_replay_draw_image(int32_t x, int32_t y, int32_t w, int32_t h, const loam_vec *strs,
                            int32_t si, int32_t r, int32_t a, int32_t fit);
void loam_replay_draw_fill_g(int32_t x, int32_t y, int32_t w, int32_t h, int32_t c0, int32_t c1,
                             int32_t axis, int32_t r, int32_t a);
void loam_replay_draw_shadow(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t rgb,
                             int32_t a, int32_t blur, int32_t dx, int32_t dy);
void loam_replay_draw_stroke(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rgb, int32_t r,
                             int32_t width, int32_t a);
void loam_replay_draw_fill4(int32_t x, int32_t y, int32_t w, int32_t h, int32_t rgb, int32_t a,
                            int32_t tl, int32_t tr, int32_t br, int32_t bl);
void loam_replay_draw_xform(int32_t dx, int32_t dy, int32_t scale, int32_t rot, int32_t ox,
                            int32_t oy);
void loam_replay_draw_path(const loam_vec *path, int32_t start, int32_t n, int32_t rgb, int32_t a,
                           int32_t width, int32_t style);
/* Phase 4 blit. The Loam `[]u8` arrives as a `loam_vec` (ptr/len/cap) BY VALUE,
   so the pixels are handed over rather than copied. See loam_zeus_plat_blit.
   The parameter types are `int32_t` because that is exactly what codegen emits
   in the forward declaration it writes for this bodyless fn (`int` is i32), and
   C requires the declaration and the definition to agree. */
void loam_platform_plat_frame(loam_vec head, loam_vec ops, loam_vec strs, loam_vec path);
int32_t loam_platform_plat_blit(loam_vec buf, int32_t w, int32_t h, int32_t gen,
                                int32_t scale);
/* Startup preset, filled by the web loader before `main`. */
void loam_runtime_preset_store(int32_t seq, loam_str msg);
void zeus_boundary_preset_add(int32_t seq, const char *msg, size_t n);

/* Loam engine entry points (packages/zeus/src/zeus.loam). Every host calls
   these directly; there is no C layer in between. A `loam_str` argument is
   borrowed for the call (a host may pass `(loam_str){ptr, len}` over its own
   buffer: the engine copies what it keeps). A returned `loam_str` is the
   host's: `loam_abi_str_release` it once read. Points are in view coordinates,
   and calls that answer an `int32_t` answer 1 when the view needs a redraw. */
void loam_zeus_engine_layout(int32_t width, int32_t height);
void loam_zeus_engine_paint(void);
int32_t loam_zeus_engine_step(void);
/* One host frame with the host-supplied delta in ms (<= 0 means 16; capped
   at 64). A host with a float `dt` in seconds passes `dt * 1000 + 0.5`. */
int32_t loam_zeus_engine_step_dt(int32_t dt_ms);
void loam_zeus_engine_set_reduced_motion(int32_t on);
/* ms until the next async timer is due (0 = none; -1 = a spawn waits). */
int32_t loam_zeus_engine_next_ms(void);
/* wasm: a trap unwound out of an export; hand it to the innermost Boundary.
   1 = recovered, 0 = nothing could take it. */
int32_t loam_zeus_engine_recover(void);
int32_t loam_zeus_engine_trap_seq(void);
int32_t loam_zeus_engine_click(int32_t x, int32_t y);
/* What a finger grabs without pressing: 1 slider/thumb, 2 on_drag, 0 else. */
int32_t loam_zeus_engine_grab(int32_t x, int32_t y);
/* Secondary click (right button, ctrl-click, long press): the nearest
   `on_context_menu` handler under the point runs. */
int32_t loam_zeus_engine_context_click(int32_t x, int32_t y);
/* 1 when a press at (x, y) lands on an app-drawn `TitleBar` and on no
   control inside it, so the host moves the window. */
int32_t loam_zeus_engine_window_drag_at(int32_t x, int32_t y);
int32_t loam_zeus_engine_scroll(int32_t x, int32_t y, int32_t dx, int32_t dy);
void loam_zeus_engine_scroll_latch(int32_t x, int32_t y);
/* Discrete scroll (mouse notch, web line wheel): the delta, no momentum. */
int32_t loam_zeus_engine_scroll_step(int32_t x, int32_t y, int32_t dx, int32_t dy);
/* A mouse-wheel notch eased to its target (browser feel). */
int32_t loam_zeus_engine_scroll_smooth(int32_t x, int32_t y, int32_t dx, int32_t dy);
int32_t loam_zeus_engine_drag(int32_t x, int32_t y);
int32_t loam_zeus_engine_hover(int32_t x, int32_t y);
void loam_zeus_engine_mouseup(void);
int32_t loam_zeus_engine_over_button(void);
/* CSS cursor name under the pointer; "" or an unknown name = the arrow. */
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
void loam_router_engine_chunks_done(int32_t index);
void loam_router_engine_chunks_slow(int32_t index);
/* 1 while the focused node takes typed text, so a host routes keys to it. */
int32_t loam_zeus_engine_focus_captures_text(void);
int32_t loam_zeus_engine_key(int32_t key);
/* A key with its modifiers: the keymap first, then the focused node. */
int32_t loam_zeus_engine_key_ev(int32_t key, int32_t mods);
/* Key release, from hosts that deliver it: `on_key_up` fires. */
int32_t loam_zeus_engine_key_up(int32_t key, int32_t mods);
void loam_zeus_engine_set_mods(int32_t mods);
/* UTF-8 into the focused input (IME commit, paste). */
int32_t loam_zeus_engine_insert(loam_str text);
/* IME composition overlay; "" clears the mark. */
int32_t loam_zeus_engine_marked(loam_str text);
/* The image a `pick_image` dialog chose (its path or URL, and its size);
   the engine redraws. */
void loam_zeus_engine_picked_image(loam_str src, int32_t w, int32_t h);
/* A file a media pick chose, or one dropped on the window. */
void loam_zeus_engine_media_picked(loam_str name, loam_str src);

/* Paint one frame into `draw` (with `ctx` passed to every callback). */
void zeus_paint(void *ctx, ZeusDraw draw);

/** The window title: a host registers a callback, which is handed the current
 *  title at once (open the window with it) and every later one (router
 *  `Meta.title` on navigation). */
void zeus_set_title_hook(void (*fn)(const char *));
/** Window chrome: 0 the host's title bar, 1 app-drawn (`zeus.window_chrome`).
 *  A desktop host reads it when it opens the window. */
int64_t zeus_window_chrome(void);
/** The host's title-bar box in points, written every frame under app-drawn
 *  chrome: bar height, the window buttons' room on the left, and on the right. */
void zeus_set_titlebar_box(int64_t height, int64_t lead, int64_t trail);
/** Clipboard writes (`plat_clipboard_write`): a desktop host registers the
 *  system pasteboard here. Unset, the text is only remembered. */
void zeus_set_clipboard_hook(void (*fn)(const char *));
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
