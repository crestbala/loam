/* maya_plat.c — host seam for Maya. No scene/tracer logic.
 *
 * plat_* match empty fns in packages/maya/std/maya.yuga. Cocoa (maya_mac.m) owns the
 * event loop; this file is getenv / clock / framebuffer blit for CGImage.
 */
#include "maya_rt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static uint8_t *fb;
static int fb_w, fb_h, fb_cap;

int64_t yuga_maya_plat_headless(void) {
    const char *a = getenv("MAYA_HEADLESS");
    const char *b = getenv("ZEUS_HEADLESS");
    const char *c = getenv("YUGA_HEADLESS");
    if ((a && a[0] == '1') || (b && b[0] == '1') || (c && c[0] == '1')) return 1;
#if !defined(__APPLE__)
    return 1;
#else
    return 0;
#endif
}

int64_t yuga_maya_plat_now_ms(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#else
    return (int64_t)clock() * 1000 / (int64_t)CLOCKS_PER_SEC;
#endif
}

const uint8_t *maya_cpu_fb(int *w, int *h) {
    int64_t ww = yuga_maya_engine_fb_w();
    int64_t hh = yuga_maya_engine_fb_h();
    int need, i, n;
    if (ww < 1 || hh < 1) {
        if (w) *w = 0;
        if (h) *h = 0;
        return NULL;
    }
    need = (int)ww * (int)hh * 4;
    if (need > fb_cap) {
        free(fb);
        fb = (uint8_t *)malloc((size_t)need);
        fb_cap = need;
    }
    fb_w = (int)ww;
    fb_h = (int)hh;
    n = fb_w * fb_h;
    for (i = 0; i < n; i++) {
        int64_t rgb = yuga_maya_engine_fb_at(i);
        int p = i * 4;
        fb[p] = (uint8_t)((rgb / 65536) % 256);
        fb[p + 1] = (uint8_t)((rgb / 256) % 256);
        fb[p + 2] = (uint8_t)(rgb % 256);
        fb[p + 3] = 255;
    }
    if (w) *w = fb_w;
    if (h) *h = fb_h;
    return fb;
}

#if !defined(__APPLE__)
void yuga_maya_plat_window(yuga_str title, int64_t w, int64_t h) {
    (void)title;
    (void)w;
    (void)h;
}

void yuga_maya_plat_run(void) {}
#endif
