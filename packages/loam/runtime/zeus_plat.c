/* zeus_plat.c — Cocoa/headless seam for the Loam zeus library.
 *
 * packages/zeus/src/zeus.loam owns the node arena, layout, paint, and hit-test. This file
 * is the other side of that seam: window title/size, text measure, present,
 * and the zeus_* entry points the Cocoa/iOS/web hosts already call. Keyboard chords,
 * the focus chain and Tab are Loam (zeuscore/keymap.loam, zeus.loam).
 */
#include "zeus_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

/* The signal byte store is Loam (`zeuscore/sigcells.loam`, R4.1b). */

/* Allocation accounting (phase 3 proof harness). `loam_rt.h` increments this
   from `loam_new` / array growth when the program was compiled with
   `-DLOAM_ALLOC_TRACE` (all Zeus programs are); `zeus.proof_allocs()` reads it. */
int64_t loam_runtime_alloc_count(void) {
    /* The counter is the runtime's (`loam_rt.h`, `LOAM_RT_DEFINE_ALLOC`); this
       seam only reports it. The copy that used to live here — one line, but a
       Zeus translation unit defining a language-runtime symbol — is gone. */
    return loam_alloc_total();
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
    loam_ewrite_bytes("zeus: out of memory\n", 20);
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
    return loam_str_copy(buf, (int64_t)strlen(buf));
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
#ifdef __wasm32__
    return plat_run ? 0 : 1; /* no environment on wasm */
#else
    if (getenv("ZEUS_HEADLESS") || !plat_run) return 1;
    return 0;
#endif
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

/* Safe-area insets (notch / home indicator / landscape inset).

   Held in one struct rather than four file-scope ints read back through four
   getters: the engine's layout pass already keeps `safe_t/r/b/l` in the arena
   (layout.loam), so the *authority* is Loam and this side is only the store a
   host can write before the engine runs. `plat_set_insets` writes it; a layout
   reads it once per pass through `plat_inset_get`. Four separate accessors made
   it possible for a host to update half the box between two reads. */
typedef struct {
    int64_t t, r, b, l;
} ZeusInsets;

static ZeusInsets g_insets;

void zeus_set_insets(int64_t top, int64_t right, int64_t bottom, int64_t left) {
    g_insets.t = top < 0 ? 0 : top;
    g_insets.r = right < 0 ? 0 : right;
    g_insets.b = bottom < 0 ? 0 : bottom;
    g_insets.l = left < 0 ? 0 : left;
}

/* Read all four at once, into the caller's `[]int` (the ABI passes a
   `loam_vec *`, so the callee writes through `->ptr`).

   The element width is `int32_t`, NOT `int64_t`: `int` in Loam is i32 (the
   Phase 10 width flip), and the generated vec literal pushes with
   `sizeof(int32_t)`. Writing 8-byte values into a 4-byte-per-element buffer
   corrupts it and silently loses the insets — which is exactly what the
   `zeus_anchor_safe` test caught. */
void loam_platform_plat_inset_get(loam_vec *out) {
    int32_t *p;
    if (!out || !out->ptr) return;
    if (out->len < 4) return;
    p = (int32_t *)out->ptr;
    p[0] = (int32_t)g_insets.t;
    p[1] = (int32_t)g_insets.r;
    p[2] = (int32_t)g_insets.b;
    p[3] = (int32_t)g_insets.l;
}

void loam_zeus_plat_inset_get(loam_vec *out) { loam_platform_plat_inset_get(out); }

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

/* The four `inset_*` getters are gone: `plat_inset_get` above returns the whole
   box in one call. Kept as a comment rather than deleted silently because the
   old names are referenced from `zeus_rt.h`'s history and from older hosts. */

/* packages/zeus/src/zeuscore/platform.loam is the GPUI-style platform leaf. Empty Loam stubs
   compile to loam_platform_plat_*; the Cocoa implementations stay as
   loam_zeus_plat_* so existing callers do not change. */
void loam_platform_plat_run(void) { loam_zeus_plat_run(); }
int64_t loam_platform_plat_headless(void) { return loam_zeus_plat_headless(); }

loam_str loam_platform_plat_pick_image(int32_t *w, int32_t *h) {
    return loam_zeus_plat_pick_image(w, h);
}
void loam_platform_plat_image_size(loam_str src, int32_t *w, int32_t *h) {
    loam_zeus_plat_image_size(src, w, h);
}

/* Wall clock for host-sleep-safe deadlines (scrollbar hide). The clock itself is
   the runtime's `loam_now_ms`, so there is exactly one monotonic clock in the
   tree instead of one per library. */
static int64_t plat_clock_skipped;

int64_t loam_platform_plat_now_ms(void) { return loam_now_ms() + plat_clock_skipped; }

/* Tests move this clock instead of sleeping through a timeline. */
void loam_platform_plat_clock_skip(int32_t ms) { plat_clock_skipped += ms; }

/* Host process memory in KB for the gallery RAM chip: Apple phys_footprint
   (the number Xcode's memory gauge shows — RSS overstates iOS processes),
   Linux/Android resident set, live wasm heap on the web host (0 = unknown). */
int64_t loam_platform_plat_mem_kb(void) {
#ifdef __wasm32__
    return loam_heap_used() / 1024;
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
void loam_platform_plat_set_window(loam_str title, int64_t width, int64_t height) {
    loam_zeus_plat_set_window(title, width, height);
}

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("set_title")))
void zeus_js_set_title(const char *t);
#endif

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("set_head")))
void zeus_js_set_head(const char *key, int32_t key_len, const char *value, int32_t value_len);
#endif

/* One `Meta` field for the page's head. Passed by length: JSON-LD has no limit. */
void loam_platform_plat_set_head(loam_str key, loam_str value) {
#ifdef __wasm32__
    zeus_js_set_head(key.ptr, (int32_t)key.len, value.ptr, (int32_t)value.len);
#else
    (void)key;
    (void)value;
#endif
}

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
/* Window chrome (`zeus.window_chrome`). 0: the host's own title bar. 1: the
   app draws it (`TitleBar`): a desktop host runs the content under a
   transparent bar, keeps the window buttons, and reports the bar's box here —
   height, the room the buttons take on the left, and on the right — every
   frame. Hosts without a title bar (web, mobile, headless) leave it zero, so a
   `TitleBar` there takes no space. Tests write the box through
   `zeus.engine_set_titlebar`. */
static int64_t chrome_mode;
static int64_t titlebar_box[3];

void loam_platform_plat_set_chrome(int64_t mode) { chrome_mode = mode; }
int64_t zeus_window_chrome(void) { return chrome_mode; }

void zeus_set_titlebar_box(int64_t height, int64_t lead, int64_t trail) {
    titlebar_box[0] = height < 0 ? 0 : height;
    titlebar_box[1] = lead < 0 ? 0 : lead;
    titlebar_box[2] = trail < 0 ? 0 : trail;
}
void loam_platform_plat_set_titlebar_box(int64_t height, int64_t lead, int64_t trail) {
    zeus_set_titlebar_box(height, lead, trail);
}
void loam_platform_plat_titlebar_get(loam_vec *out) {
    int32_t *p;
    if (!out || !out->ptr || out->len < 3) return;
    p = (int32_t *)out->ptr;
    p[0] = (int32_t)titlebar_box[0];
    p[1] = (int32_t)titlebar_box[1];
    p[2] = (int32_t)titlebar_box[2];
}

/* Clipboard write (`CopyButton`). The runtime keeps the last string so a
   headless run can read it back (`plat_clipboard_last`); a desktop host
   registers a hook that hands it to the system pasteboard, and the web asks
   `navigator.clipboard`. */
static char *clip_last;
static void (*plat_clip_fn)(const char *);
void zeus_set_clipboard_hook(void (*fn)(const char *)) { plat_clip_fn = fn; }

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("clipboard_write")))
void zeus_js_clipboard_write(const char *t);
__attribute__((import_module("zeus"), import_name("apple_keys")))
int32_t zeus_js_apple_keys(void);
#endif

void loam_platform_plat_clipboard_write(loam_str text) {
    free(clip_last);
    clip_last = dup_ys(text);
#ifdef __wasm32__
    zeus_js_clipboard_write(clip_last);
#else
    if (plat_clip_fn) plat_clip_fn(clip_last);
#endif
}

loam_str loam_platform_plat_clipboard_last(void) {
    if (!clip_last) return (loam_str){"", 0};
    /* A copy: the next clipboard write frees `clip_last`. */
    return loam_str_copy(clip_last, (int64_t)strlen(clip_last));
}

/* Media state (the null player, the pick queue, fullscreen) lives in
   `zeuscore/mediastate.loam`. This file only stores the host table and
   forwards each callback. A host reports a pick to the Loam export
   `engine_media_picked`, which decides whether to redraw. */
static ZeusMedia media_host;

void zeus_set_media(ZeusMedia media) { media_host = media; }

/* 1 when shortcuts follow Apple conventions (⌘ rather than Ctrl): macOS and
   iOS builds, and a browser on an Apple platform. A headless run (snapshots,
   draw goldens) answers 1 on every OS, so its output is host-independent. */
int64_t loam_platform_plat_apple_keys(void) {
#ifdef __wasm32__
    return zeus_js_apple_keys();
#elif defined(__APPLE__)
    return 1;
#else
    return loam_zeus_plat_headless() ? 1 : 0;
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
/* Forward to the one insets read. The four per-edge forms are gone. */
void loam_platform_plat_set_insets(int64_t top, int64_t right, int64_t bottom,
                                  int64_t left) {
    zeus_set_insets(top, right, bottom, left);
}

const char *zeus_window_title(void) { return win_title ? win_title : ""; }
int64_t zeus_window_width(void) { return win_w; }
int64_t zeus_window_height(void) { return win_h; }

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

/* --- recoverable traps (zeus.Boundary) ---

   The policy and the interned-handler table are `zeuscore/runtime.loam`.
   This is the arm (`loam_trap_guard`, loam_rt.h) and the panic buffer. On
   wasm the arm runs the body unprotected. */

loam_str loam_runtime_trap_guard(loam_fn build) {
    return loam_trap_guard(build);
}

loam_str loam_runtime_panic_text(void) {
    loam_str msg;
    msg.ptr = loam_jmp_msg;
    msg.len = (int64_t)strlen(loam_jmp_msg);
    msg.own = 0;
    return msg;
}

void zeus_boundary_preset_add(int32_t seq, const char *msg, size_t n) {
    loam_str owned;
    if (!msg) {
        msg = "";
        n = 0;
    }
    /* The parameter is borrowed: the store retains what it keeps, and drops
       nothing. Release the temporary here, including the one past the cap. */
    owned = loam_str_copy(msg, (int64_t)n);
    loam_runtime_preset_store(seq, owned);
    loam_str_release(&owned);
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
#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("history_push")))
void zeus_js_history_push(const char *path);
__attribute__((import_module("zeus"), import_name("history_replace")))
void zeus_js_history_replace(const char *path);
__attribute__((import_module("zeus"), import_name("history_back")))
void zeus_js_history_back(void);
#endif

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("boot_path")))
int32_t zeus_js_boot_path(char *buf, int32_t cap);
#endif

loam_str loam_platform_plat_boot_path(void) {
#ifdef __wasm32__
    char buf[2048];
    int32_t n = zeus_js_boot_path(buf, (int32_t)sizeof buf);
    if (n > 0) {
        char *p = (char *)loam_new((size_t)n + 1, "boot_path", 0);
        if (p) {
            memcpy(p, buf, (size_t)n);
            p[n] = '\0';
            return (loam_str){p, n};
        }
    }
#endif
    return (loam_str){"", 0};
}

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("chunks_ready")))
int32_t zeus_js_chunks_ready(int32_t index);
__attribute__((import_module("zeus"), import_name("chunks_load")))
void zeus_js_chunks_load(int32_t index);
__attribute__((import_module("zeus"), import_name("chunks_preload")))
void zeus_js_chunks_preload(int32_t index);
#endif

int64_t loam_platform_plat_chunks_ready(int64_t index) {
#ifdef __wasm32__
    return zeus_js_chunks_ready((int32_t)index);
#else
    (void)index;
    return 1;
#endif
}

void loam_platform_plat_chunks_load(int64_t index) {
#ifdef __wasm32__
    zeus_js_chunks_load((int32_t)index);
#else
    (void)index;
#endif
}

void loam_platform_plat_chunks_preload(int64_t index) {
#ifdef __wasm32__
    zeus_js_chunks_preload((int32_t)index);
#else
    (void)index;
#endif
}

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("appearance_load")))
int32_t zeus_js_appearance_load(void);
__attribute__((import_module("zeus"), import_name("appearance_save")))
void zeus_js_appearance_save(int32_t dark);
#endif

int64_t loam_platform_plat_appearance_load(void) {
#ifdef __wasm32__
    return zeus_js_appearance_load();
#else
    return -1;
#endif
}

void loam_platform_plat_appearance_save(int64_t dark) {
#ifdef __wasm32__
    zeus_js_appearance_save((int32_t)dark);
#else
    (void)dark;
#endif
}

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("track_start")))
void zeus_js_track_start(const char *endpoint, int32_t len, int32_t local);
__attribute__((import_module("zeus"), import_name("track_send")))
void zeus_js_track_send(int32_t kind, const char *name, int32_t name_len, const char *data, int32_t data_len);
__attribute__((import_module("zeus"), import_name("track_opt_out")))
void zeus_js_track_opt_out(int32_t off);
__attribute__((import_module("zeus"), import_name("track_opted_out")))
int32_t zeus_js_track_opted_out(void);
#endif

void loam_platform_plat_track_start(loam_str endpoint, int64_t local) {
#ifdef __wasm32__
    zeus_js_track_start(endpoint.ptr, (int32_t)endpoint.len, (int32_t)local);
#else
    (void)endpoint;
    (void)local;
#endif
}

void loam_platform_plat_track_send(int64_t kind, loam_str name, loam_str data) {
#ifdef __wasm32__
    zeus_js_track_send((int32_t)kind, name.ptr, (int32_t)name.len, data.ptr, (int32_t)data.len);
#else
    (void)kind;
    (void)name;
    (void)data;
#endif
}

void loam_platform_plat_track_opt_out(int64_t off) {
#ifdef __wasm32__
    zeus_js_track_opt_out((int32_t)off);
#else
    (void)off;
#endif
}

int64_t loam_platform_plat_track_opted_out(void) {
#ifdef __wasm32__
    return zeus_js_track_opted_out();
#else
    return 0;
#endif
}

#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("open_external")))
void zeus_js_open_external(const char *url, int32_t len);
#endif

int64_t loam_platform_plat_layout_verify(void) {
#if defined(LOAM_WEB_RELEASE) || defined(__wasm32__)
    return 0; /* no environment on wasm */
#else
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("ZEUS_LAYOUT_VERIFY");
        on = v && strcmp(v, "1") == 0;
    }
    return on;
#endif
}

void loam_platform_plat_open_external(loam_str url) {
#ifdef __wasm32__
    zeus_js_open_external(url.ptr, (int32_t)url.len);
#else
    (void)url;
#endif
}

#ifdef __wasm32__
static const char *history_path(loam_str path, char *buf, size_t cap) {
    size_t n = (path.len > 0 && path.ptr) ? (size_t)path.len : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(buf, path.ptr, n);
    buf[n] = '\0';
    return buf;
}
#endif

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
   One call each, no player state. Strings are NUL-terminated in a stack
   buffer when they fit (`cstr_in`). Wasm uses the same JS imports the old
   `plat_media_*` functions did. */
#ifdef __wasm32__
__attribute__((import_module("zeus"), import_name("media_open")))
int32_t zeus_js_media_open(int32_t slot, const char *src);
__attribute__((import_module("zeus"), import_name("media_cmd")))
void zeus_js_media_cmd(int32_t slot, int32_t cmd, int32_t value);
__attribute__((import_module("zeus"), import_name("media_get")))
int32_t zeus_js_media_get(int32_t slot, int32_t what);
__attribute__((import_module("zeus"), import_name("media_text")))
int32_t zeus_js_media_text(int32_t slot, int32_t what, char *out, int32_t cap);
__attribute__((import_module("zeus"), import_name("media_pick")))
void zeus_js_media_pick(void);
__attribute__((import_module("zeus"), import_name("fullscreen")))
int32_t zeus_js_fullscreen(int32_t on);
#endif

int32_t loam_mediastate_media_host_caps(void) {
#ifdef __wasm32__
    return 256;
#else
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
#endif
}

int32_t loam_imagesize_image_host_bound(void) {
    return plat_image_size_fn ? 1 : 0;
}

loam_vec loam_imagesize_read_head(loam_str path, int32_t cap) {
#ifdef __wasm32__
    (void)path;
    (void)cap;
    return loam_vec_new();
#else
    loam_vec out = loam_vec_new();
    char stack_buf[4096];
    char *text;
    FILE *file;
    size_t got;
    if (cap <= 0) return out;
    text = cstr_in(path, stack_buf, sizeof stack_buf);
    file = fopen(text, "rb");
    cstr_done(text, stack_buf);
    if (!file) return out;
    loam_vec_reserve(&out, (int64_t)cap, 1, "imagesize.read_head", 0);
    got = fread(out.ptr, 1, (size_t)cap, file);
    fclose(file);
    out.len = (int64_t)got;
    return out;
#endif
}

int32_t loam_mediastate_host_open(int32_t slot, loam_str src) {
    char stack_buf[256];
    char *text = cstr_in(src, stack_buf, sizeof stack_buf);
    int32_t opened = 0;
#ifdef __wasm32__
    opened = zeus_js_media_open(slot, text);
#else
    if (media_host.open) opened = (int32_t)media_host.open((int64_t)slot, text);
#endif
    cstr_done(text, stack_buf);
    return opened;
}

void loam_mediastate_host_cmd(int32_t slot, int32_t command, int32_t value) {
#ifdef __wasm32__
    zeus_js_media_cmd(slot, command, value);
#else
    if (media_host.cmd) media_host.cmd((int64_t)slot, (int64_t)command, (int64_t)value);
#endif
}

int32_t loam_mediastate_host_get(int32_t slot, int32_t what) {
#ifdef __wasm32__
    return zeus_js_media_get(slot, what);
#else
    if (!media_host.get) return 0;
    return (int32_t)media_host.get((int64_t)slot, (int64_t)what);
#endif
}

loam_str loam_mediastate_host_text(int32_t slot, int32_t what) {
    char buffer[4096];
    buffer[0] = 0;
#ifdef __wasm32__
    {
        int32_t got = zeus_js_media_text(slot, what, buffer, (int32_t)sizeof buffer);
        if (got < 0) got = 0;
        if (got >= (int32_t)sizeof buffer) got = (int32_t)sizeof buffer - 1;
        buffer[got] = 0;
    }
#else
    if (media_host.text) {
        media_host.text((int64_t)slot, (int64_t)what, buffer, (int)sizeof buffer);
        buffer[sizeof buffer - 1] = 0;
    }
#endif
    return loam_str_copy(buffer, (int64_t)strlen(buffer));
}

void loam_mediastate_host_pick(void) {
#ifdef __wasm32__
    zeus_js_media_pick();
#else
    if (media_host.pick) media_host.pick();
#endif
}

void loam_mediastate_host_pick_audio(void) {
#ifdef __wasm32__
    zeus_js_media_pick();
#else
    if (media_host.pick_audio) media_host.pick_audio();
#endif
}

int32_t loam_mediastate_host_voice(int32_t what, loam_str src, loam_str out_path) {
#ifdef __wasm32__
    (void)what;
    (void)src;
    (void)out_path;
    return 0;
#else
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
#endif
}

int32_t loam_mediastate_host_fullscreen(int32_t on) {
#ifdef __wasm32__
    return zeus_js_fullscreen(on);
#else
    if (!media_host.fullscreen) return 0;
    return (int32_t)media_host.fullscreen((int64_t)on);
#endif
}

void loam_mediastate_host_redraw(void) {
    if (plat_redraw) plat_redraw();
}
