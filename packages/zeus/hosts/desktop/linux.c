/* linux.c — X11 present for Zeus. Fill + text only; no native widgets.
   Linked on Linux GUI builds (`yugac --target=native` without ZEUS_HEADLESS). */
#if defined(__linux__)
#include "zeus_rt.h"
#include "zeus_key.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static Display *g_dpy;
static Window g_win;
static GC g_gc;
static int g_scr;
static int g_w = 640, g_h = 480;
static char g_title[256];
static int g_open;

static unsigned long rgb_px(int64_t rgb) {
    XColor c;
    Colormap cm = DefaultColormap(g_dpy, g_scr);
    c.red = (unsigned short)(((rgb >> 16) & 255) * 257);
    c.green = (unsigned short)(((rgb >> 8) & 255) * 257);
    c.blue = (unsigned short)((rgb & 255) * 257);
    c.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(g_dpy, cm, &c)) return BlackPixel(g_dpy, g_scr);
    return c.pixel;
}

static void linux_fill(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                       int64_t rgb, int64_t radius) {
    (void)ctx;
    (void)radius;
    if (!g_dpy) return;
    XSetForeground(g_dpy, g_gc, rgb_px(rgb));
    XFillRectangle(g_dpy, g_win, g_gc, (int)x, (int)y, (unsigned)w, (unsigned)h);
}

static void linux_fill_a(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                         int64_t rgb, int64_t radius, int64_t alpha) {
    (void)alpha;
    linux_fill(ctx, x, y, w, h, rgb, radius);
}

static void linux_fill_g(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                         int64_t c0, int64_t c1, int64_t axis) {
    (void)c1;
    (void)axis;
    linux_fill(ctx, x, y, w, h, c0, 0);
}

static void linux_text(void *ctx, int64_t x, int64_t y, const char *s,
                       int64_t rgb, int64_t font) {
    (void)ctx;
    (void)font;
    if (!g_dpy || !s) return;
    XSetForeground(g_dpy, g_gc, rgb_px(rgb));
    XDrawString(g_dpy, g_win, g_gc, (int)x, (int)y + 12, s, (int)strlen(s));
}

static void linux_measure(const char *s, int64_t px, int64_t *w, int64_t *h) {
    int n = s ? (int)strlen(s) : 0;
    if (w) *w = n * (px > 0 ? px * 6 / 10 : 8);
    if (h) *h = px > 0 ? px + 4 : 16;
}

static void linux_redraw(void) {
    if (g_dpy && g_open) XClearArea(g_dpy, g_win, 0, 0, 0, 0, True);
}

static void linux_paint(void) {
    ZeusDraw d;
    memset(&d, 0, sizeof d);
    d.fill = linux_fill;
    d.fill_a = linux_fill_a;
    d.fill_g = linux_fill_g;
    d.text = linux_text;
    zeus_layout((int64_t)g_w, (int64_t)g_h);
    zeus_paint(NULL, d);
    XFlush(g_dpy);
}

static void linux_run(void) {
    XEvent ev;
    Atom wm_del;
    if (g_open) return;
    {
        const char *t = zeus_window_title();
        int64_t ww = zeus_window_width();
        int64_t hh = zeus_window_height();
        if (t && t[0]) {
            strncpy(g_title, t, sizeof g_title - 1);
            g_title[sizeof g_title - 1] = 0;
        }
        if (ww > 0) g_w = (int)ww;
        if (hh > 0) g_h = (int)hh;
    }
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "yuga: no X11 display (set DISPLAY or use ZEUS_HEADLESS=1)\n");
        return;
    }
    g_scr = DefaultScreen(g_dpy);
    g_win = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, g_scr), 0, 0,
                                (unsigned)g_w, (unsigned)g_h, 0,
                                BlackPixel(g_dpy, g_scr), WhitePixel(g_dpy, g_scr));
    g_gc = XCreateGC(g_dpy, g_win, 0, NULL);
    XStoreName(g_dpy, g_win, g_title[0] ? g_title : "Zeus");
    wm_del = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &wm_del, 1);
    XSelectInput(g_dpy, g_win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                                   PointerMotionMask | KeyPressMask | StructureNotifyMask |
                                   Button4Mask | Button5Mask);
    XMapWindow(g_dpy, g_win);
    g_open = 1;
    zeus_window_opened();
    for (;;) {
        XNextEvent(g_dpy, &ev);
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wm_del) break;
        if (ev.type == Expose && ev.xexpose.count == 0) linux_paint();
        if (ev.type == ConfigureNotify) {
            g_w = ev.xconfigure.width;
            g_h = ev.xconfigure.height;
            linux_paint();
        }
        if (ev.type == ButtonPress) {
            int x = ev.xbutton.x, y = ev.xbutton.y;
            if (ev.xbutton.button == Button4) zeus_handle_scroll_step(x, y, 0, -40);
            else if (ev.xbutton.button == Button5) zeus_handle_scroll_step(x, y, 0, 40);
            else zeus_handle_click(x, y);
            linux_paint();
        }
        if (ev.type == MotionNotify)
            zeus_handle_hover(ev.xmotion.x, ev.xmotion.y);
        if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int key = 0;
            if (ks == XK_Return) key = 13;
            else if (ks == XK_Tab) key = 9;
            else if (ks == XK_BackSpace) key = 8;
            else if (ks == XK_Escape) key = 27;
            else if (ks >= 32 && ks < 127) key = (int)ks;
            if (key) zeus_handle_key(key, 0);
            linux_paint();
        }
    }
    XFreeGC(g_dpy, g_gc);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
    g_dpy = NULL;
    g_open = 0;
}

__attribute__((constructor))
static void zeus_linux_register(void) {
    zeus_set_platform(linux_run, linux_measure, linux_redraw);
}

#endif /* __linux__ */
