/* linux.c — X11 present for Zeus. Fill + text only; no native widgets.
   Linked on Linux GUI builds (`loam --target=native` without ZEUS_HEADLESS). */
#if defined(__linux__)
#include "zeus_host.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>

static Display *g_dpy;
static Window g_win;
static GC g_gc;
static int g_scr;

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

static void linux_text(void *ctx, int64_t x, int64_t y, const char *s,
                       int64_t rgb, int64_t font) {
    (void)ctx;
    (void)font;
    if (!g_dpy || !s) return;
    XSetForeground(g_dpy, g_gc, rgb_px(rgb));
    XDrawString(g_dpy, g_win, g_gc, (int)x, (int)y + 12, s, (int)strlen(s));
}

static void linux_redraw(void) {
    if (g_dpy) XClearArea(g_dpy, g_win, 0, 0, 0, 0, True);
}

static void linux_paint(void) {
    loam_zeus_engine_host_paint();
    XFlush(g_dpy);
}

static void linux_apply_title(const char *t) {
    if (g_dpy && t && t[0]) XStoreName(g_dpy, g_win, t);
}

static void linux_run(void) {
    XEvent ev;
    Atom wm_del;
    if (g_dpy) return;
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "loam: no X11 display (set DISPLAY or use ZEUS_HEADLESS=1)\n");
        return;
    }
    g_scr = DefaultScreen(g_dpy);
    g_win = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, g_scr), 0, 0,
                                (unsigned)zeus_window_width(), (unsigned)zeus_window_height(), 0,
                                BlackPixel(g_dpy, g_scr), WhitePixel(g_dpy, g_scr));
    g_gc = XCreateGC(g_dpy, g_win, 0, NULL);
    zeus_set_title_hook(linux_apply_title);
    wm_del = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &wm_del, 1);
    XSelectInput(g_dpy, g_win, ExposureMask | ButtonPressMask | ButtonReleaseMask |
                                   PointerMotionMask | KeyPressMask | StructureNotifyMask |
                                   Button4Mask | Button5Mask);
    XMapWindow(g_dpy, g_win);
    zeus_window_opened();

    int xfd = ConnectionNumber(g_dpy);
    for (;;) {
        int timeout_ms = loam_zeus_engine_poll_timeout_ms();
        while (XPending(g_dpy) > 0) {
            XNextEvent(g_dpy, &ev);
            if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wm_del) {
                goto done;
            } else if (ev.type == Expose && ev.xexpose.count == 0) {
                linux_paint();
            } else if (ev.type == ConfigureNotify) {
                zeus_set_window_size(ev.xconfigure.width, ev.xconfigure.height);
                linux_paint();
            } else if (ev.type == ButtonPress) {
                loam_zeus_engine_x11_button((int32_t)ev.xbutton.button, ev.xbutton.x, ev.xbutton.y);
                XFlush(g_dpy);
            } else if (ev.type == MotionNotify) {
                loam_zeus_engine_hover(ev.xmotion.x, ev.xmotion.y);
            } else if (ev.type == KeyPress) {
                loam_zeus_engine_x11_key((int32_t)XLookupKeysym(&ev.xkey, 0));
                XFlush(g_dpy);
            }
        }
        if (timeout_ms != 0) {
            struct pollfd pfd = {xfd, POLLIN, 0};
            int r = poll(&pfd, 1, timeout_ms);
            if (r < 0) break;
            if (r == 0) {
                loam_zeus_engine_step_dt(17);
                linux_paint();
            }
            continue;
        }
        if (loam_zeus_engine_step_dt(17)) linux_paint();
    }
done:
    XFreeGC(g_dpy, g_gc);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);
    g_dpy = NULL;
}

__attribute__((constructor))
static void zeus_linux_register(void) {
    ZeusDraw d = {0};
    d.fill = linux_fill;
    d.text = linux_text;
    zeus_bind_draw(d);
    zeus_set_platform(linux_run, NULL, linux_redraw);
}

#endif /* __linux__ */
