/* mac.m — Cocoa window + Core Text paint for Zeus (zeus/desktop). */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include "zeus_rt.h"
#include "zeus_key.h"
#include <string.h>
#include <stdlib.h>
#include <malloc/malloc.h>

static NSView *g_view;
static NSWindow *g_win;

/* Font cache, keyed by size and invalidated when the family changes
   (zeus_font_family() is global — see zeus_plat.c). Falling back to the
   system font keeps text rendering when a face is missing. */
static char font_cache_family[128];

static NSFont *font_at(int64_t px) {
    static NSFont *cache[72];
    const char *fam = zeus_font_family();
    int i = (int)px;
    if (!fam) fam = "";
    if (strcmp(fam, font_cache_family) != 0) {
        for (int k = 0; k < 72; k++) {
            if (cache[k]) [cache[k] release];
            cache[k] = nil;
        }
        strncpy(font_cache_family, fam, sizeof font_cache_family - 1);
        font_cache_family[sizeof font_cache_family - 1] = 0;
    }
    if (i < 8) i = 8;
    if (i > 71) i = 71;
    if (!cache[i]) {
        NSFont *f = nil;
        if (fam[0]) {
            NSString *n = [NSString stringWithUTF8String:fam];
            f = [NSFont fontWithName:n size:(CGFloat)i];
        }
        if (!f) f = [NSFont systemFontOfSize:(CGFloat)i];
        cache[i] = [f retain];
    }
    return cache[i];
}

/* Register a font file with the process so `fontWithName:` can find it.
   `src` is a path to a .ttf / .otf / .ttc. */
static int mac_load_font(const char *family, const char *src) {
    (void)family;
    if (!src || !src[0]) return 0;
    NSString *path = [NSString stringWithUTF8String:src];
    NSURL *url = [NSURL fileURLWithPath:path];
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) return 0;
    CFErrorRef err = NULL;
    bool ok = CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url,
                                               kCTFontManagerScopeProcess, &err);
    if (!ok && err) CFRelease(err);
    return ok ? 1 : 0;
}

static NSColor *zeus_color(int64_t rgb) {
    CGFloat r = ((rgb >> 16) & 255) / 255.0;
    CGFloat g = ((rgb >> 8) & 255) / 255.0;
    CGFloat b = (rgb & 255) / 255.0;
    return [NSColor colorWithCalibratedRed:r green:g blue:b alpha:1.0];
}

/* Attributes are pure functions of (colour, size), so they are cached — this is
   called from both measure and draw, every text node, every frame. The cache is
   bounded and evicts (releasing) once full: the previous version stopped
   *storing* past 24 entries but still returned a freshly retained dictionary,
   so a themed app with more than 24 colour/size pairs leaked one dictionary
   per text node per frame — the steady climb the RAM chip showed. */
static NSDictionary *zeus_attrs(int64_t rgb, int64_t px) {
    enum { ATTR_CACHE = 256 };
    static NSDictionary *cache[ATTR_CACHE];
    static int64_t keys[ATTR_CACHE];
    static unsigned n, next;
    int64_t key = (rgb << 8) ^ (px & 255);
    unsigned i;
    for (i = 0; i < n; i++)
        if (keys[i] == key) return cache[i];
    NSDictionary *a = [@{
        NSFontAttributeName: font_at(px),
        NSForegroundColorAttributeName: zeus_color(rgb)
    } retain];
    if (n < ATTR_CACHE) {
        i = n++;
    } else {
        i = next;
        next = (next + 1) % ATTR_CACHE;
        [cache[i] release];
    }
    keys[i] = key;
    cache[i] = a;
    return a;
}

static void mac_measure(const char *s, int64_t px, int64_t *w, int64_t *h) {
    NSString *str = s ? [NSString stringWithUTF8String:s] : @"";
    NSDictionary *a = zeus_attrs(0x000000, px);
    NSSize sz = [str sizeWithAttributes:a];
    *w = (int64_t)(sz.width + 0.999);
    *h = (int64_t)(sz.height + 0.999);
}

static void mac_fill_a(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                       int64_t rgb, int64_t radius, int64_t alpha) {
    (void)ctx;
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    CGFloat r = ((rgb >> 16) & 255) / 255.0;
    CGFloat g = ((rgb >> 8) & 255) / 255.0;
    CGFloat b = (rgb & 255) / 255.0;
    NSRect rect = NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    [[NSColor colorWithCalibratedRed:r green:g blue:b alpha:a] setFill];
    if (radius > 0) {
        CGFloat rad = (CGFloat)radius;
        if (rad > rect.size.width / 2) rad = rect.size.width / 2;
        if (rad > rect.size.height / 2) rad = rect.size.height / 2;
        [[NSBezierPath bezierPathWithRoundedRect:rect xRadius:rad yRadius:rad] fill];
    } else {
        [[NSBezierPath bezierPathWithRect:rect] fill];
    }
}

/* Clamp a corner radius to what the rect can actually hold, so a `RAD_FULL`
   (999) pill on a short box is a capsule rather than a degenerate path. */
static CGFloat mac_rad(CGFloat rad, NSRect r) {
    if (rad < 0) rad = 0;
    if (rad > r.size.width / 2) rad = r.size.width / 2;
    if (rad > r.size.height / 2) rad = r.size.height / 2;
    return rad;
}

static NSBezierPath *mac_rrect(NSRect r, CGFloat rad) {
    rad = mac_rad(rad, r);
    if (rad <= 0) return [NSBezierPath bezierPathWithRect:r];
    return [NSBezierPath bezierPathWithRoundedRect:r xRadius:rad yRadius:rad];
}

static void mac_fill_g(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                       int64_t c0, int64_t c1, int64_t axis, int64_t radius,
                       int64_t alpha) {
    (void)ctx;
    NSRect r = NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    NSGradient *g = [[NSGradient alloc] initWithStartingColor:zeus_color(c0)
                                                 endingColor:zeus_color(c1)];
    [NSGraphicsContext saveGraphicsState];
    if (a < 1.0) [[NSGraphicsContext currentContext] setCompositingOperation:NSCompositingOperationSourceOver];
    /* The radius used to be dropped here, which is why every gradient
       background painted with square corners. Clipping to the rounded path
       is what NSGradient offers in place of a rounded-rect gradient fill. */
    if (radius > 0) [mac_rrect(r, (CGFloat)radius) addClip];
    if (a < 1.0) {
        CGContextSetAlpha([[NSGraphicsContext currentContext] CGContext], a);
    }
    [g drawInRect:r angle:(axis ? 0.0 : 90.0)];
    [NSGraphicsContext restoreGraphicsState];
    [g release];
}

/* Soft drop shadow under a rounded rect.
 *
 * The shadow is the blur of an opaque path, so that path has to be drawn —
 * but drawing it would paint over whatever sits under the card. The fix is
 * an even-odd clip that excludes the rect's own area: the solid fill lands
 * entirely outside the clip and only its blur survives. Painting the shape
 * and letting the caller's fill cover it would look identical on an opaque
 * surface and wrong on a translucent one, which is exactly the case
 * elevation-on-glass needs. */
static void mac_shadow(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                       int64_t radius, int64_t rgb, int64_t alpha, int64_t blur,
                       int64_t dx, int64_t dy) {
    (void)ctx;
    NSRect r = NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    CGFloat pad = (CGFloat)(blur * 3 + (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + 8);
    NSShadow *sh = [[NSShadow alloc] init];
    NSBezierPath *hole = [NSBezierPath bezierPathWithRect:NSInsetRect(r, -pad, -pad)];
    [NSGraphicsContext saveGraphicsState];
    [hole appendBezierPath:mac_rrect(r, (CGFloat)radius)];
    [hole setWindingRule:NSWindingRuleEvenOdd];
    [hole addClip];
    /* The view is flipped (y grows down), so a positive dy must read as
       "downward" here too — AppKit shadow offsets are in unflipped space. */
    [sh setShadowOffset:NSMakeSize((CGFloat)dx, (CGFloat)-dy)];
    [sh setShadowBlurRadius:(CGFloat)blur];
    [sh setShadowColor:[zeus_color(rgb) colorWithAlphaComponent:a]];
    [sh set];
    [[NSColor blackColor] setFill];
    [mac_rrect(r, (CGFloat)radius) fill];
    [NSGraphicsContext restoreGraphicsState];
    [sh release];
}

/* Ring stroke, inset by half the width so the line paints inside the rect —
   the same box model CSS `border` uses, and what the engine's layout assumes
   when it insets content by `border_w`. */
static void mac_stroke(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                       int64_t rgb, int64_t radius, int64_t width, int64_t alpha) {
    (void)ctx;
    CGFloat lw = (CGFloat)width;
    NSRect r = NSMakeRect((CGFloat)x + lw / 2, (CGFloat)y + lw / 2,
                          (CGFloat)w - lw, (CGFloat)h - lw);
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    NSBezierPath *p;
    if (r.size.width <= 0 || r.size.height <= 0) return;
    p = mac_rrect(r, (CGFloat)radius - lw / 2);
    [p setLineWidth:lw];
    [[zeus_color(rgb) colorWithAlphaComponent:a] setStroke];
    [p stroke];
}

static void mac_fill4(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                      int64_t rgb, int64_t alpha, int64_t tl, int64_t tr,
                      int64_t br, int64_t bl) {
    (void)ctx;
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    CGFloat X = (CGFloat)x, Y = (CGFloat)y, W = (CGFloat)w, H = (CGFloat)h;
    CGFloat lim = (W < H ? W : H) / 2;
    CGFloat a1 = (CGFloat)tl, a2 = (CGFloat)tr, a3 = (CGFloat)br, a4 = (CGFloat)bl;
    NSBezierPath *p = [NSBezierPath bezierPath];
    if (W <= 0 || H <= 0) return;
    if (a1 > lim) a1 = lim;
    if (a2 > lim) a2 = lim;
    if (a3 > lim) a3 = lim;
    if (a4 > lim) a4 = lim;
    /* Clockwise from the top-left, arcs tangent to each corner. The view is
       flipped, so this traversal is visually clockwise on screen. */
    [p moveToPoint:NSMakePoint(X + a1, Y)];
    [p lineToPoint:NSMakePoint(X + W - a2, Y)];
    if (a2 > 0)
        [p appendBezierPathWithArcFromPoint:NSMakePoint(X + W, Y)
                                    toPoint:NSMakePoint(X + W, Y + a2) radius:a2];
    [p lineToPoint:NSMakePoint(X + W, Y + H - a3)];
    if (a3 > 0)
        [p appendBezierPathWithArcFromPoint:NSMakePoint(X + W, Y + H)
                                    toPoint:NSMakePoint(X + W - a3, Y + H) radius:a3];
    [p lineToPoint:NSMakePoint(X + a4, Y + H)];
    if (a4 > 0)
        [p appendBezierPathWithArcFromPoint:NSMakePoint(X, Y + H)
                                    toPoint:NSMakePoint(X, Y + H - a4) radius:a4];
    [p lineToPoint:NSMakePoint(X, Y + a1)];
    if (a1 > 0)
        [p appendBezierPathWithArcFromPoint:NSMakePoint(X, Y)
                                    toPoint:NSMakePoint(X + a1, Y) radius:a1];
    [p closePath];
    [[zeus_color(rgb) colorWithAlphaComponent:a] setFill];
    [p fill];
}

/* Translate, then scale and rotate about (ox, oy). Concatenated onto the
   current CTM, so it composes with an enclosing clip and unwinds with the
   enclosing `restore`. */
static void mac_xform(void *ctx, int64_t dx, int64_t dy, int64_t scale,
                      int64_t rot, int64_t ox, int64_t oy) {
    (void)ctx;
    NSAffineTransform *t = [NSAffineTransform transform];
    [t translateXBy:(CGFloat)(ox + dx) yBy:(CGFloat)(oy + dy)];
    if (rot) [t rotateByDegrees:(CGFloat)rot];
    if (scale != 100) {
        CGFloat k = (CGFloat)scale / 100.0;
        [t scaleXBy:k yBy:k];
    }
    [t translateXBy:(CGFloat)-ox yBy:(CGFloat)-oy];
    [t concat];
}

static void mac_fill(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                     int64_t rgb, int64_t radius) {
    (void)ctx;
    NSRect r = NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    [zeus_color(rgb) setFill];
    if (radius > 0) {
        CGFloat rad = (CGFloat)radius;
        if (rad > r.size.width / 2) rad = r.size.width / 2;
        if (rad > r.size.height / 2) rad = r.size.height / 2;
        [[NSBezierPath bezierPathWithRoundedRect:r xRadius:rad yRadius:rad] fill];
    } else {
        NSRectFill(r);
    }
}

static void mac_text(void *ctx, int64_t x, int64_t y, const char *s,
                     int64_t rgb, int64_t font) {
    (void)ctx;
    NSString *str = s ? [NSString stringWithUTF8String:s] : @"";
    [str drawAtPoint:NSMakePoint((CGFloat)x, (CGFloat)y) withAttributes:zeus_attrs(rgb, font)];
}

/* Rotated text around the top-left corner of the line box. The view is
   flipped (y grows down), where a positive AppKit rotation appears
   clockwise on screen — exactly the engine's convention (UiNode.text_rot:
   "clockwise-on-screen degrees"). Web (canvas rotate(+deg)) renders the
   same way. */
static void mac_text_rot(void *ctx, int64_t x, int64_t y, const char *s,
                         int64_t rgb, int64_t font, int64_t deg) {
    (void)ctx;
    NSString *str = s ? [NSString stringWithUTF8String:s] : @"";
    [NSGraphicsContext saveGraphicsState];
    NSAffineTransform *t = [NSAffineTransform transform];
    [t translateXBy:(CGFloat)x yBy:(CGFloat)y];
    [t rotateByDegrees:(CGFloat)deg];
    [t concat];
    [str drawAtPoint:NSZeroPoint withAttributes:zeus_attrs(rgb, font)];
    [NSGraphicsContext restoreGraphicsState];
}

static void mac_save(void *ctx) {
    (void)ctx;
    [NSGraphicsContext saveGraphicsState];
}

static void mac_clip(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                     int64_t radius) {
    NSRect r = NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    (void)ctx;
    if (radius > 0) {
        [mac_rrect(r, (CGFloat)radius) addClip];
        return;
    }
    NSRectClip(r);
}

static void mac_restore(void *ctx) {
    (void)ctx;
    [NSGraphicsContext restoreGraphicsState];
}

/* --- SVG (viewBox, g, path, circle; Solar Linear subset) --- */

typedef struct {
    const char *p;
} SvgScan;

typedef struct {
    int has_fill, has_stroke;
    int64_t fill, stroke;
    float sw;
    int cap;  /* 0 butt, 1 round, 2 square */
    int join; /* 0 miter, 1 round, 2 bevel */
} SvgStyle;

static void svg_skip(SvgScan *s) {
    while (*s->p && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' ||
                     *s->p == '\r' || *s->p == ','))
        s->p++;
}

static int svg_num(SvgScan *s, float *out) {
    svg_skip(s);
    const char *p = s->p;
    if (!*p) return 0;
    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p) return 0;
    s->p = end;
    *out = v;
    return 1;
}

static int svg_attr(const char *tag, const char *name, char *out, int cap) {
    size_t nlen = strlen(name);
    const char *p = tag;
    while (*p) {
        if ((p == tag || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') &&
            strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            p += nlen + 1;
            char q = *p;
            if (q != '"' && q != '\'') return 0;
            p++;
            int i = 0;
            while (*p && *p != q && i + 1 < cap) out[i++] = *p++;
            out[i] = 0;
            return 1;
        }
        p++;
    }
    return 0;
}

static int svg_color(const char *s, int64_t current, int64_t *out) {
    if (!s || !*s) return 0;
    if (strcmp(s, "none") == 0 || strcmp(s, "transparent") == 0) return 0;
    if (strcmp(s, "currentColor") == 0 || strcmp(s, "currentcolor") == 0) {
        *out = current;
        return 1;
    }
    if (s[0] == '#') {
        unsigned v = 0;
        int n = 0;
        const char *p = s + 1;
        while (*p && n < 8) {
            int d;
            if (*p >= '0' && *p <= '9') d = *p - '0';
            else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
            else break;
            v = (v << 4) | (unsigned)d;
            p++;
            n++;
        }
        if (n == 3)
            *out = (int64_t)(((v >> 8) & 0xF) * 0x110000 + ((v >> 4) & 0xF) * 0x1100 +
                             (v & 0xF) * 0x11);
        else if (n == 6)
            *out = (int64_t)v;
        else
            return 0;
        return 1;
    }
    return 0;
}

static void svg_style_from(const char *tag, SvgStyle *st, int64_t current) {
    char buf[128];
    int64_t c;
    if (svg_attr(tag, "fill", buf, (int)sizeof buf)) {
        if (svg_color(buf, current, &c)) {
            st->has_fill = 1;
            st->fill = c;
        } else {
            st->has_fill = 0;
        }
    }
    if (svg_attr(tag, "stroke", buf, (int)sizeof buf)) {
        if (svg_color(buf, current, &c)) {
            st->has_stroke = 1;
            st->stroke = c;
        } else {
            st->has_stroke = 0;
        }
    }
    if (svg_attr(tag, "stroke-width", buf, (int)sizeof buf)) {
        SvgScan sc = {buf};
        float v;
        if (svg_num(&sc, &v) && v > 0) st->sw = v;
    }
    if (svg_attr(tag, "stroke-linecap", buf, (int)sizeof buf)) {
        if (strcmp(buf, "round") == 0) st->cap = 1;
        else if (strcmp(buf, "square") == 0) st->cap = 2;
        else st->cap = 0;
    }
    if (svg_attr(tag, "stroke-linejoin", buf, (int)sizeof buf)) {
        if (strcmp(buf, "round") == 0) st->join = 1;
        else if (strcmp(buf, "bevel") == 0) st->join = 2;
        else st->join = 0;
    }
}

static NSColor *svg_nscolor(int64_t rgb, int64_t alpha) {
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    return [NSColor colorWithCalibratedRed:((rgb >> 16) & 255) / 255.0
                                     green:((rgb >> 8) & 255) / 255.0
                                      blue:(rgb & 255) / 255.0
                                     alpha:a];
}

static void svg_paint_path(NSBezierPath *path, const SvgStyle *st, float scale,
                           int64_t alpha) {
    if (!path || [path isEmpty]) return;
    [path setLineWidth:(CGFloat)(st->sw * scale)];
    [path setLineCapStyle:st->cap == 1 ? NSLineCapStyleRound
                         : st->cap == 2 ? NSLineCapStyleSquare
                                        : NSLineCapStyleButt];
    [path setLineJoinStyle:st->join == 1 ? NSLineJoinStyleRound
                          : st->join == 2 ? NSLineJoinStyleBevel
                                          : NSLineJoinStyleMiter];
    if (st->has_fill) {
        [svg_nscolor(st->fill, alpha) setFill];
        [path fill];
    }
    if (st->has_stroke) {
        [svg_nscolor(st->stroke, alpha) setStroke];
        [path stroke];
    }
}

static NSPoint svg_xf(float x, float y, float ox, float oy, float s) {
    return NSMakePoint((CGFloat)(ox + x * s), (CGFloat)(oy + y * s));
}

static int svg_cmd_is(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void svg_parse_d(const char *d, float ox, float oy, float s, NSBezierPath *path) {
    SvgScan sc = {d};
    float cx = 0, cy = 0, sx = 0, sy = 0, ox1 = 0, oy1 = 0;
    int prev = 0, started = 0;
    while (1) {
        svg_skip(&sc);
        if (!*sc.p) break;
        int cmd = *sc.p;
        if (svg_cmd_is(cmd)) {
            sc.p++;
        } else {
            if (!prev) break;
            cmd = (prev == 'M') ? 'L' : (prev == 'm') ? 'l' : prev;
        }
        int rel = cmd >= 'a';
        int op = rel ? cmd - 32 : cmd;
        if (op == 'M') {
            float x, y;
            if (!svg_num(&sc, &x) || !svg_num(&sc, &y)) break;
            if (rel) {
                x += cx;
                y += cy;
            }
            cx = x;
            cy = y;
            sx = cx;
            sy = cy;
            [path moveToPoint:svg_xf(cx, cy, ox, oy, s)];
            started = 1;
            prev = rel ? 'm' : 'M';
            continue;
        }
        if (!started) {
            prev = cmd;
            continue;
        }
        if (op == 'L') {
            float x, y;
            if (!svg_num(&sc, &x) || !svg_num(&sc, &y)) break;
            if (rel) {
                x += cx;
                y += cy;
            }
            cx = x;
            cy = y;
            [path lineToPoint:svg_xf(cx, cy, ox, oy, s)];
        } else if (op == 'H') {
            float x;
            if (!svg_num(&sc, &x)) break;
            if (rel) x += cx;
            cx = x;
            [path lineToPoint:svg_xf(cx, cy, ox, oy, s)];
        } else if (op == 'V') {
            float y;
            if (!svg_num(&sc, &y)) break;
            if (rel) y += cy;
            cy = y;
            [path lineToPoint:svg_xf(cx, cy, ox, oy, s)];
        } else if (op == 'C') {
            float x1, y1, x2, y2, x, y;
            if (!svg_num(&sc, &x1) || !svg_num(&sc, &y1) || !svg_num(&sc, &x2) ||
                !svg_num(&sc, &y2) || !svg_num(&sc, &x) || !svg_num(&sc, &y))
                break;
            if (rel) {
                x1 += cx;
                y1 += cy;
                x2 += cx;
                y2 += cy;
                x += cx;
                y += cy;
            }
            [path curveToPoint:svg_xf(x, y, ox, oy, s)
                 controlPoint1:svg_xf(x1, y1, ox, oy, s)
                 controlPoint2:svg_xf(x2, y2, ox, oy, s)];
            ox1 = x2;
            oy1 = y2;
            cx = x;
            cy = y;
        } else if (op == 'S') {
            float x2, y2, x, y;
            if (!svg_num(&sc, &x2) || !svg_num(&sc, &y2) || !svg_num(&sc, &x) ||
                !svg_num(&sc, &y))
                break;
            if (rel) {
                x2 += cx;
                y2 += cy;
                x += cx;
                y += cy;
            }
            float x1 = cx, y1 = cy;
            if (prev == 'C' || prev == 'c' || prev == 'S' || prev == 's') {
                x1 = 2 * cx - ox1;
                y1 = 2 * cy - oy1;
            }
            [path curveToPoint:svg_xf(x, y, ox, oy, s)
                 controlPoint1:svg_xf(x1, y1, ox, oy, s)
                 controlPoint2:svg_xf(x2, y2, ox, oy, s)];
            ox1 = x2;
            oy1 = y2;
            cx = x;
            cy = y;
        } else if (op == 'Q') {
            float qx, qy, x, y;
            if (!svg_num(&sc, &qx) || !svg_num(&sc, &qy) || !svg_num(&sc, &x) ||
                !svg_num(&sc, &y))
                break;
            if (rel) {
                qx += cx;
                qy += cy;
                x += cx;
                y += cy;
            }
            float x1 = cx + 2.f / 3.f * (qx - cx);
            float y1 = cy + 2.f / 3.f * (qy - cy);
            float x2 = x + 2.f / 3.f * (qx - x);
            float y2 = y + 2.f / 3.f * (qy - y);
            [path curveToPoint:svg_xf(x, y, ox, oy, s)
                 controlPoint1:svg_xf(x1, y1, ox, oy, s)
                 controlPoint2:svg_xf(x2, y2, ox, oy, s)];
            ox1 = qx;
            oy1 = qy;
            cx = x;
            cy = y;
        } else if (op == 'T') {
            float x, y;
            if (!svg_num(&sc, &x) || !svg_num(&sc, &y)) break;
            if (rel) {
                x += cx;
                y += cy;
            }
            float qx = cx, qy = cy;
            if (prev == 'Q' || prev == 'q' || prev == 'T' || prev == 't') {
                qx = 2 * cx - ox1;
                qy = 2 * cy - oy1;
            }
            float x1 = cx + 2.f / 3.f * (qx - cx);
            float y1 = cy + 2.f / 3.f * (qy - cy);
            float x2 = x + 2.f / 3.f * (qx - x);
            float y2 = y + 2.f / 3.f * (qy - y);
            [path curveToPoint:svg_xf(x, y, ox, oy, s)
                 controlPoint1:svg_xf(x1, y1, ox, oy, s)
                 controlPoint2:svg_xf(x2, y2, ox, oy, s)];
            ox1 = qx;
            oy1 = qy;
            cx = x;
            cy = y;
        } else if (op == 'Z') {
            [path closePath];
            cx = sx;
            cy = sy;
        } else if (op == 'A') {
            /* Arc: skip the 7 parameters. */
            float dump;
            int i;
            for (i = 0; i < 7; i++)
                if (!svg_num(&sc, &dump)) break;
        } else {
            break;
        }
        prev = cmd;
    }
}

static const char *svg_tag_end(const char *p, int *self_close) {
    *self_close = 0;
    while (*p && *p != '>') {
        if (*p == '/' && p[1] == '>') {
            *self_close = 1;
            return p + 2;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) p++;
            if (*p) p++;
            continue;
        }
        p++;
    }
    if (*p == '>') p++;
    return p;
}

static void svg_draw_tree(const char **pp, SvgStyle st, float ox, float oy, float scale,
                          int64_t current, int64_t alpha, const char *stop) {
    const char *p = *pp;
    while (*p) {
        if (stop && strncmp(p, stop, strlen(stop)) == 0) {
            p += strlen(stop);
            break;
        }
        if (p[0] == '<' && p[1] == '/') {
            while (*p && *p != '>') p++;
            if (*p) p++;
            break;
        }
        if (*p != '<') {
            p++;
            continue;
        }
        if (strncmp(p, "<!--", 4) == 0) {
            p = strstr(p, "-->");
            p = p ? p + 3 : p;
            if (!p) break;
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        const char *name = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '/' && *p != '>') p++;
        int nlen = (int)(p - name);
        int self_close = 0;
        const char *gt = svg_tag_end(p, &self_close);
        int tlen = (int)(gt - name);
        if (tlen > 2047) tlen = 2047;
        char tag[2048];
        memcpy(tag, name, (size_t)tlen);
        tag[tlen] = 0;
        p = gt;
        SvgStyle kid = st;
        svg_style_from(tag, &kid, current);
        if (nlen == 1 && name[0] == 'g') {
            if (!self_close) {
                const char *rest = p;
                svg_draw_tree(&rest, kid, ox, oy, scale, current, alpha, "</g>");
                p = rest;
            }
        } else if (nlen == 3 && strncmp(name, "svg", 3) == 0) {
            if (!self_close) {
                const char *rest = p;
                svg_draw_tree(&rest, kid, ox, oy, scale, current, alpha, "</svg>");
                p = rest;
            }
        } else if (nlen == 4 && strncmp(name, "path", 4) == 0) {
            char d[4096];
            if (svg_attr(tag, "d", d, (int)sizeof d)) {
                NSBezierPath *path = [NSBezierPath bezierPath];
                svg_parse_d(d, ox, oy, scale, path);
                svg_paint_path(path, &kid, scale, alpha);
            }
        } else if (nlen == 6 && strncmp(name, "circle", 6) == 0) {
            char buf[64];
            float cx = 0, cy = 0, r = 0, v;
            SvgScan sc;
            if (svg_attr(tag, "cx", buf, (int)sizeof buf)) {
                sc.p = buf;
                if (svg_num(&sc, &v)) cx = v;
            }
            if (svg_attr(tag, "cy", buf, (int)sizeof buf)) {
                sc.p = buf;
                if (svg_num(&sc, &v)) cy = v;
            }
            if (svg_attr(tag, "r", buf, (int)sizeof buf)) {
                sc.p = buf;
                if (svg_num(&sc, &v)) r = v;
            }
            if (r > 0) {
                NSPoint c = svg_xf(cx, cy, ox, oy, scale);
                CGFloat rr = (CGFloat)(r * scale);
                NSBezierPath *path =
                    [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - rr, c.y - rr,
                                                                      rr * 2, rr * 2)];
                svg_paint_path(path, &kid, scale, alpha);
            }
        }
        (void)self_close;
    }
    *pp = p;
}

static void mac_svg(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                    const char *markup, int64_t rgb, int64_t alpha) {
    (void)ctx;
    if (!markup || w <= 0 || h <= 0) return;
    float vb_x = 0, vb_y = 0, vb_w = 24, vb_h = 24;
    const char *svg = strstr(markup, "<svg");
    if (!svg) svg = markup;
    char tag[2048], vb[128];
    int self_close = 0;
    const char *name = svg + 1;
    while (*name == ' ') name++;
    const char *gt = svg_tag_end(name, &self_close);
    int tlen = (int)(gt - name);
    if (tlen > 2047) tlen = 2047;
    memcpy(tag, name, (size_t)tlen);
    tag[tlen] = 0;
    if (svg_attr(tag, "viewBox", vb, (int)sizeof vb) ||
        svg_attr(tag, "viewbox", vb, (int)sizeof vb)) {
        SvgScan sc = {vb};
        svg_num(&sc, &vb_x);
        svg_num(&sc, &vb_y);
        svg_num(&sc, &vb_w);
        svg_num(&sc, &vb_h);
        if (vb_w < 1) vb_w = 24;
        if (vb_h < 1) vb_h = 24;
    }
    float sx = (float)w / vb_w, sy = (float)h / vb_h;
    float scale = sx < sy ? sx : sy;
    float ox = (float)x + ((float)w - vb_w * scale) * 0.5f - vb_x * scale;
    float oy = (float)y + ((float)h - vb_h * scale) * 0.5f - vb_y * scale;
    SvgStyle st;
    memset(&st, 0, sizeof st);
    st.sw = 1.f;
    st.cap = 1;
    st.join = 1;
    svg_style_from(tag, &st, rgb);
    const char *body = gt;
    svg_draw_tree(&body, st, ox, oy, scale, rgb, alpha, NULL);
}

/* PNG / JPEG / WebP / GIF via ImageIO. Cache by src; http(s) loads off-thread.
   A decoded image is the largest thing a page can hold — one 4000×3000 photo is
   ~48 MB resident — and an unbounded cache pins every source a feed ever drew
   for the life of the process, which is memory no retry would have cost. The
   cache is bounded and evicts least-recently-used first; an evicted source just
   decodes again on the next frame that draws it. A failed load stays in as
   NSNull (a real answer, not a retry every frame) and ages out like any other. */
enum { IMG_CACHE_MAX = 24 };
static NSMutableDictionary *g_img;
static NSMutableSet *g_img_load;
static NSMutableArray *g_img_lru; /* keys, least recently used first */

static void img_lru_touch(NSString *key) {
    [g_img_lru removeObject:key];
    [g_img_lru addObject:key];
}

static void mac_img_put(NSString *key, NSImage *img) {
    [g_img setObject:(img ? (id)img : (id)[NSNull null]) forKey:key];
    [img release];
    img_lru_touch(key);
    while ([g_img_lru count] > IMG_CACHE_MAX) {
        NSString *old = [g_img_lru objectAtIndex:0];
        [g_img_lru removeObjectAtIndex:0];
        [g_img removeObjectForKey:old];
    }
}

static NSImage *mac_img_get(const char *src) {
    NSString *key;
    NSImage *img;
    if (!src || !src[0]) return nil;
    if (!g_img) {
        g_img = [[NSMutableDictionary alloc] init];
        g_img_load = [[NSMutableSet alloc] init];
        g_img_lru = [[NSMutableArray alloc] init];
    }
    key = [NSString stringWithUTF8String:src];
    img = [g_img objectForKey:key];
    if (img) {
        img_lru_touch(key);
        return img == (id)[NSNull null] ? nil : img;
    }
    if ([g_img_load containsObject:key]) return nil;
    if ([key hasPrefix:@"data:"]) {
        NSRange comma = [key rangeOfString:@","];
        NSData *data = nil;
        if (comma.location != NSNotFound)
            data = [[NSData alloc] initWithBase64EncodedString:[key substringFromIndex:comma.location + 1]
                                                       options:NSDataBase64DecodingIgnoreUnknownCharacters];
        img = data ? [[NSImage alloc] initWithData:data] : nil;
        [data release];
        mac_img_put(key, img);
        return img;
    }
    if ([key hasPrefix:@"http://"] || [key hasPrefix:@"https://"]) {
        NSURL *url = [NSURL URLWithString:key];
        if (!url) {
            mac_img_put(key, nil);
            return nil;
        }
        [g_img_load addObject:key];
        [[[NSURLSession sharedSession] dataTaskWithURL:url
                                     completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
            (void)resp;
            (void)err;
            NSImage *im = data ? [[NSImage alloc] initWithData:data] : nil;
            dispatch_async(dispatch_get_main_queue(), ^{
                mac_img_put(key, im);
                [g_img_load removeObject:key];
                if (g_view) [g_view setNeedsDisplay:YES];
            });
        }] resume];
        return nil;
    }
    if ([key hasPrefix:@"file://"])
        img = [[NSImage alloc] initWithContentsOfURL:[NSURL URLWithString:key]];
    else
        img = [[NSImage alloc] initWithContentsOfFile:key];
    mac_img_put(key, img);
    return img;
}

static NSRect mac_image_dest(int64_t x, int64_t y, int64_t w, int64_t h,
                             int64_t iw, int64_t ih, int64_t fit) {
    int64_t dw, dh;
    if (fit != 1 && fit != 2 && fit != 3) return NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    if (iw <= 0 || ih <= 0) return NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    if (fit == 3) return NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)iw, (CGFloat)ih);
    if ((fit == 1 && iw * h <= ih * w) || (fit == 2 && iw * h >= ih * w)) {
        dh = h;
        dw = iw * h / ih;
    } else {
        dw = w;
        dh = ih * w / iw;
    }
    return NSMakeRect((CGFloat)(x + (w - dw) / 2), (CGFloat)(y + (h - dh) / 2),
                      (CGFloat)dw, (CGFloat)dh);
}

static void mac_image(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                      const char *src, int64_t radius, int64_t alpha, int64_t fit) {
    NSImage *img;
    NSRect box, dest;
    NSSize sz;
    CGFloat a, rad;
    (void)ctx;
    if (w <= 0 || h <= 0) return;
    img = mac_img_get(src);
    if (!img) return;
    a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    sz = [img size];
    box = NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    dest = mac_image_dest(x, y, w, h, (int64_t)(sz.width + 0.5), (int64_t)(sz.height + 0.5), fit);
    [NSGraphicsContext saveGraphicsState];
    if (radius > 0) {
        rad = (CGFloat)radius;
        if (rad > box.size.width / 2) rad = box.size.width / 2;
        if (rad > box.size.height / 2) rad = box.size.height / 2;
        [[NSBezierPath bezierPathWithRoundedRect:box xRadius:rad yRadius:rad] addClip];
    } else {
        [[NSBezierPath bezierPathWithRect:box] addClip];
    }
    [img drawInRect:dest
           fromRect:NSZeroRect
          operation:NSCompositingOperationSourceOver
           fraction:a
     respectFlipped:YES
              hints:nil];
    [NSGraphicsContext restoreGraphicsState];
}

static void mac_redraw(void) {
    [g_view setNeedsDisplay:YES];
}

/* `ZEUS_MEM_DEBUG=1`: once a second, log the process footprint next to the
   malloc heap Loam actually owns. The two tell different stories — a spike in
   `phys` with `malloc` flat is the graphics stack (window drawable, CoreText
   caches), not the engine — which is the first thing to check when chasing
   app memory. */
static void mac_mem_debug(void) {
    static int frames;
    malloc_statistics_t ms;
    if (!getenv("ZEUS_MEM_DEBUG")) return;
    if ((frames++ % 60) != 0) return;
    malloc_zone_statistics(malloc_default_zone(), &ms);
    fprintf(stderr, "[mem] malloc=%uKB blocks=%u phys=%lldKB win=%lldx%lld view=%.0fx%.0f\n",
            (unsigned)(ms.size_in_use / 1024), (unsigned)ms.blocks_in_use,
            (long long)loam_platform_plat_mem_kb(),
            (long long)zeus_window_width(), (long long)zeus_window_height(),
            g_view ? g_view.bounds.size.width : 0.0, g_view ? g_view.bounds.size.height : 0.0);
}

/* Intrinsic size for layout (`platform.plat_image_size`): decodes from the
 * same cache `mac_image` draws from, so a widthless `Image` keeps the source
 * aspect. Remote loads return 0 here and answer on a later frame — the
 * completion handler already calls `setNeedsDisplay`. */
static void mac_image_size(const char *src, int64_t *w, int64_t *h) {
    NSImage *img;
    NSSize sz;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!src) return;
    img = mac_img_get(src);
    if (!img) return;
    sz = [img size];
    if (w) *w = (int64_t)(sz.width + 0.5);
    if (h) *h = (int64_t)(sz.height + 0.5);
}

/* CSS cursor name (from `cursor = "pointer"` props) → AppKit cursor.
 * Names are the CSS values; shapes AppKit cannot draw fall back to the
 * arrow. Web hosts can set `style.cursor` directly with the same string. */
static NSCursor *zeus_cursor_for(const char *name) {
    if (!name || !*name || strcmp(name, "default") == 0)
        return [NSCursor arrowCursor];
    if (strcmp(name, "pointer") == 0) return [NSCursor pointingHandCursor];
    if (strcmp(name, "text") == 0) return [NSCursor IBeamCursor];
    if (strcmp(name, "crosshair") == 0) return [NSCursor crosshairCursor];
    if (strcmp(name, "not-allowed") == 0 || strcmp(name, "no-drop") == 0)
        return [NSCursor operationNotAllowedCursor];
    if (strcmp(name, "grab") == 0 || strcmp(name, "move") == 0)
        return [NSCursor openHandCursor];
    if (strcmp(name, "grabbing") == 0) return [NSCursor closedHandCursor];
    if (strcmp(name, "col-resize") == 0 || strcmp(name, "e-resize") == 0 ||
        strcmp(name, "w-resize") == 0 || strcmp(name, "ew-resize") == 0)
        return [NSCursor resizeLeftRightCursor];
    if (strcmp(name, "row-resize") == 0 || strcmp(name, "n-resize") == 0 ||
        strcmp(name, "s-resize") == 0 || strcmp(name, "ns-resize") == 0)
        return [NSCursor resizeUpDownCursor];
    /* wait, help, cell, copy, alias, context-menu, progress, vertical-text,
       zoom-in/out, diagonal resizes, all-scroll: no AppKit shape → arrow. */
    return [NSCursor arrowCursor];
}

@interface ZeusView : NSView <NSTextInputClient> {
    NSTrackingArea *track;
    NSString *marked;
    NSRange markedSel;
}
- (void)zeusRender;
@end

/* Semantic role string from `engine_a11y_dump` → NSAccessibility role. */
static NSAccessibilityRole mac_a11y_role(NSString *r) {
    if ([r isEqualToString:@"button"]) return NSAccessibilityButtonRole;
    if ([r isEqualToString:@"textbox"]) return NSAccessibilityTextFieldRole;
    if ([r isEqualToString:@"slider"]) return NSAccessibilitySliderRole;
    if ([r isEqualToString:@"switch"]) return NSAccessibilityCheckBoxRole;
    if ([r isEqualToString:@"checkbox"]) return NSAccessibilityCheckBoxRole;
    if ([r isEqualToString:@"list"]) return NSAccessibilityListRole;
    if ([r isEqualToString:@"table"]) return NSAccessibilityTableRole;
    if ([r isEqualToString:@"img"]) return NSAccessibilityImageRole;
    return NSAccessibilityStaticTextRole;
}

/* A CG-drawn window's drawable is IOSurface memory the compositor allocates at
   the display's scale, and it does *not* follow `layer.contentsScale` — a window
   store cannot be talked into rendering fewer pixels. Measured on a
   screen-filling window: 2–3 full-window buffers plus a few 16 KB bookkeeping
   regions, against an 85–182 MB footprint depending on the window's colour space
   and how much is resident when you sample (see `mac_run`). The supported way to
   supply a layer's content without AppKit allocating a store for the view is
   `wantsUpdateLayer` / `updateLayer` (AppKit calls those instead of `drawRect:`),
   so this path hands the layer one bitmap *we* own.

   Display paths:

     default              AppKit's store, window pinned to sRGB. `drawRect:`
                          paints straight into the surface the compositor reads,
                          so nothing is ever copied and a screen-filling window
                          holds 60 fps — at the cost of ~54 MB of IOSurface.
     ZEUS_WIDE_GAMUT=1    the same, but the window keeps the display's ICC
                          profile, which doubles each buffer's depth to 8 bytes
                          per pixel: ~109 MB for identical pixels.
     ZEUS_OWN_BUFFER=1    the bitmap below: one buffer (~18 MB) instead of three,
                          but CoreAnimation must materialise every frame into a
                          texture of its own — 16.6 ms against the sRGB window,
                          25.4 ms against a half-float one, since the copy also
                          converts. That copy is the whole difference between the
                          paths.

   Measured, handing the layer the IOSurface itself (rather than a CGImage over
   the bitmap) does remove that copy: `other` fell from 32 ms to 9.5 ms, the frame
   held 60 fps and the footprint dropped to ~35 MB. It is not shipped because
   CoreAnimation then holds the surface as the layer's texture, so locking it to
   write the next frame deadlocks the app after a few frames. The untried fix is a
   two-buffer swap with a non-blocking `kIOSurfaceLockAvoidSync` lock.

   No scale knob: the bitmap is always the display's own resolution. */
static int own_buffer = -1;
static CGContextRef own_ctx;
static NSGraphicsContext *own_gc; /* AppKit wrapper around `own_ctx`, built once */
static int own_ctx_w, own_ctx_h;
static CGFloat own_ctx_scale = 1.0;

/* One device-RGB space for both the bitmap and the image over it. A fresh space
   per frame is pointless churn, and sharing it keeps the image's space identical
   to the layer's so CoreAnimation has nothing to convert. */
static CGColorSpaceRef own_colorspace(void) {
    static CGColorSpaceRef cs;
    if (!cs) cs = CGColorSpaceCreateDeviceRGB();
    return cs;
}

static int env_truthy(const char *name) {
    const char *e = getenv(name);
    return e && e[0] && e[0] != '0';
}

static int own_buffer_on(void) {
    if (own_buffer < 0) {
        /* AppKit's store is the default; `ZEUS_OWN_BUFFER=1` asks for the owned
           bitmap. `APP_KIT` is still accepted as an explicit spelling of the
           default, so an older command line keeps its meaning. */
        const char *e = getenv("ZEUS_OWN_BUFFER");
        if (env_truthy("APP_KIT")) {
            own_buffer = 0;
        } else {
            own_buffer = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
    }
    return own_buffer;
}

/* A `CGImage` over the bitmap's own pixels, without copying them.
   `CGBitmapContextCreateImage` allocates a fresh buffer and memcpy's the whole
   frame on every call — tens of MB of allocation and copy per frame at Retina,
   which is the scroll and paint latency this path introduced. The image is
   read-only in principle; CoreAnimation reads it during the commit that follows
   this call, before the next frame's draw starts. */
static CGImageRef own_image(void) {
    CGDataProviderRef prov;
    CGImageRef img;
    void *base;
    size_t bpr;
    if (!own_ctx) return NULL;
    base = CGBitmapContextGetData(own_ctx);
    bpr = CGBitmapContextGetBytesPerRow(own_ctx);
    if (!base || bpr == 0) return NULL;
    prov = CGDataProviderCreateWithData(NULL, base, bpr * (size_t)own_ctx_h, NULL);
    if (!prov) return NULL;
    img = CGImageCreate((size_t)own_ctx_w, (size_t)own_ctx_h, 8, 32, bpr,
                        own_colorspace(),
                        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,
                        prov, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(prov);
    return img;
}

/* `ZEUS_FRAME_DEBUG=1`: what a frame actually costs, split so the parts add up.
   `setup` is the graphics-context plumbing, `engine` is layout + step + paint,
   `image` is the bitmap→CGImage hand-off, `set` is the `contents` assignment and
   commit, and `other` is whatever is left of `interval` — CoreAnimation's own
   work plus the wait for the next vsync, which no code of ours can time. */
typedef struct {
    double setup_ms, engine_ms, image_ms, set_ms;
    int more;
} FramePhases;
static double frame_now_ms(void) {
    return CFAbsoluteTimeGetCurrent() * 1000.0;
}

static int frame_debug_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("ZEUS_FRAME_DEBUG");
        v = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return v;
}

static void frame_trace(double t0, FramePhases p) {
    static double prev;
    static int n;
    double interval = (prev > 0.0) ? (t0 - prev) : 0.0;
    double work = p.setup_ms + p.engine_ms + p.image_ms + p.set_ms;
    prev = t0;
    if (!frame_debug_on()) return;
    if ((n++ % 30) != 0) return;
    fprintf(stderr,
            "[frame] interval=%.1fms setup=%.2f engine=%.1f image=%.2f set=%.2f other=%.1f "
            "more=%d next=%dms\n",
            interval, p.setup_ms, p.engine_ms, p.image_ms, p.set_ms, interval - work, p.more,
            (int)loam_zeus_engine_next_ms());
}

/* Reduced-motion preference (§3.2): reported once to the engine, which makes
   animations jump to their final value. */
static void mac_sync_reduced_motion(void) {
    static int done = 0;
    int on = 0;
    if (done) return;
    done = 1;
    if (@available(macOS 10.12, *)) {
        on = [NSWorkspace sharedWorkspace].accessibilityDisplayShouldReduceMotion ? 1 : 0;
    }
#ifdef LOAM_HOST_BUILD
    if (zeus_app_api.set_reduced_motion) zeus_app_api.set_reduced_motion(on);
#else
    loam_zeus_engine_set_reduced_motion(on);
#endif
}

/* One engine frame: layout, step, and paint through the host's draw callbacks.
   Both display paths share it so they stay frame-for-frame identical. Returns 1
   when the engine wants another frame (animation running, async work pending). */
static int mac_frame(int64_t vw, int64_t vh) {
    static CFAbsoluteTime last;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    float dt = (last > 0) ? (float)(now - last) : (1.f / 60.f);
    ZeusDraw d;
    int more;
    last = now;
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0.001f) dt = 1.f / 60.f;
    mac_sync_reduced_motion();
    mac_mem_debug();
    zeus_layout(vw, vh);
    more = zeus_step(dt);
    memset(&d, 0, sizeof d);
    d.fill = mac_fill;
    d.fill_a = mac_fill_a;
    d.fill_g = mac_fill_g;
    d.shadow = mac_shadow;
    d.stroke = mac_stroke;
    d.fill4 = mac_fill4;
    d.xform = mac_xform;
    d.text = mac_text;
    d.text_rot = mac_text_rot;
    d.save = mac_save;
    d.clip = mac_clip;
    d.restore = mac_restore;
    d.svg = mac_svg;
    d.image = mac_image;
    zeus_paint(NULL, d);
    return more;
}

/* Frame clock. Asking for the next frame from inside the display callback
   (`dispatch_async` → `setNeedsDisplay`) lands one runloop hop late, and the
   display then falls two vsyncs out: with `ZEUS_FRAME_DEBUG=1` the interval
   stayed pinned at 33.3 ms while `engine` ranged from 4.8 to 17.5 ms — a spare
   vsync we had the headroom for and never used. A display link asks at the
   display's own cadence instead, which is what puts a frame back inside one
   vsync. Paused whenever no frame is wanted, so an idle app still costs
   nothing. */
static CADisplayLink *g_link;
static int g_link_want;

static void mac_link_tick(void) {
    static double prev;
    static int n;
    double now = frame_now_ms();
    /* The clock's own cadence, next to the frame cost: if this is ~16.7ms and
       frames still take longer, the gap is after our render, not in it. */
    if (frame_debug_on() && prev > 0.0 && (n++ % 30) == 0)
        fprintf(stderr, "[tick] period=%.1fms want=%d\n", now - prev, g_link_want);
    prev = now;
    if (!g_link_want || !g_view) return;
    g_link_want = 0;
    /* Mark only. Rendering inside the tick puts CoreAnimation's flush straight
       onto the clock and measured *worse* (39 ms vs 24 ms) — the display pass
       is where that work belongs. */
    [g_view setNeedsDisplay:YES];
}

static void mac_link_arm(void) {
    if (g_link || !g_view) return;
    if (@available(macOS 14.0, *)) {
        g_link = [[g_view displayLinkWithTarget:g_view
                                       selector:@selector(linkTick:)] retain];
        [g_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }
}

/* `setNeedsDisplay` from inside a display callback is dropped by AppKit; queue
   the next frame instead — or sleep until the next async deadline when idle.
   -1 = a spawn is waiting, draw a frame soon. */
/* A benchmark hook. The engine stops asking for frames when nothing is pending
   (the tail of `mac_schedule_next`), so frame cost can only be sampled while
   something is animating — which would make `bench/own_buffer.sh` depend on a
   human moving the mouse, and let the two display paths be measured at different
   workloads. `ZEUS_FRAME_BENCH=1` keeps frames coming. Off by default; it exists
   so the numbers in examples/zeus/myapp/readme.md can be re-derived. */
static int frame_bench_on(void) {
    static int v = -1;
    if (v < 0) v = env_truthy("ZEUS_FRAME_BENCH");
    return v;
}

static void mac_schedule_next(int more) {
    NSView *v = g_view;
    int64_t due;
    if (!v) return;
    if (frame_bench_on()) more = 1;
    if (more) {
        if (g_link) {
            /* The link asks at the next display refresh (see `mac_link_tick`). */
            g_link_want = 1;
            g_link.paused = NO;
            return;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            [v setNeedsDisplay:YES];
        });
        return;
    }
    due = loam_zeus_engine_next_ms();
    if (due) {
        double delay = (due < 0) ? 0.0 : ((double)due / 1000.0);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            [v setNeedsDisplay:YES];
        });
        return;
    }
    /* Nothing pending: stop the clock so an idle window costs nothing. */
    if (g_link) g_link.paused = YES;
}

@implementation ZeusView
- (BOOL)isFlipped { return YES; }
/* The engine paints every pixel of the viewport — the root scroller carries the
   page background, so scroll edges and short pages stay paper. Saying so lets
   the compositor copy the window instead of blending it, which is one fewer
   surface for a full-window canvas. */
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)preservesContentDuringLiveResize { return NO; }

/* Accessibility bridge (§2.3): the UI is a canvas, so VoiceOver would see one
   opaque view. This returns the semantic tree as accessibility children,
   built from the roles/labels the widgets already set. It is not a render
   target and never affects layout. */
- (BOOL)isAccessibilityElement { return NO; }
- (NSArray *)accessibilityChildren {
    NSMutableArray *kids = [NSMutableArray array];
    loam_str dump = loam_zeus_engine_a11y_dump();
    if (!dump.ptr || dump.len <= 0) return kids;
    NSString *s = [[[NSString alloc] initWithBytes:dump.ptr
                                           length:(NSUInteger)dump.len
                                         encoding:NSUTF8StringEncoding] autorelease];
    if (!s) return kids;
    for (NSString *line in [s componentsSeparatedByString:@"\n"]) {
        if (line.length == 0) continue;
        NSArray *parts = [line componentsSeparatedByString:@"\t"];
        if (parts.count < 3) continue;
        NSString *role = parts[1];
        NSString *label = parts[2];
        NSRect frame = NSZeroRect;
        if (parts.count >= 7) {
            frame = NSMakeRect([parts[3] doubleValue], [parts[4] doubleValue],
                               [parts[5] doubleValue], [parts[6] doubleValue]);
            frame = [self convertRect:frame toView:nil];
            if (self.window) frame = [self.window convertRectToScreen:frame];
        }
        NSAccessibilityElement *el = [NSAccessibilityElement accessibilityElementWithRole:
                                                                                         mac_a11y_role(role)
                                                                                             frame:frame
                                                                                             label:label
                                                                                            parent:self];
        if (el) [kids addObject:el];
    }
    return kids;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    /* The old area is owned by us (+1 from alloc) *and* retained by the view.
       `removeTrackingArea:` drops the view's retain only, so the alloc must be
       balanced here or every resize leaks an NSTrackingArea. */
    if (track) {
        [self removeTrackingArea:track];
        [track release];
        track = nil;
    }
    track = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInKeyWindow
               owner:self
            userInfo:nil];
    [self addTrackingArea:track];
}

- (void)dealloc {
    if (track) [self removeTrackingArea:track];
    [track release];
    [marked release];
    [super dealloc];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    @autoreleasepool {
        NSRect b = self.bounds;
        FramePhases p;
        double t0 = frame_now_ms();
        memset(&p, 0, sizeof p);
        p.more = mac_frame((int64_t)b.size.width, (int64_t)b.size.height);
        p.engine_ms = frame_now_ms() - t0;
        frame_trace(t0, p);
        mac_schedule_next(p.more);
    }
}

/* Default display path: AppKit calls these instead of `drawRect:` and allocates
   no store for the view, so the layer's pixels are the bitmap we hand it. */
- (BOOL)wantsUpdateLayer {
    return own_buffer_on() ? YES : NO;
}

- (void)linkTick:(id)link {
    (void)link;
    mac_link_tick();
}

/* The frame. AppKit reaches it through `updateLayer` (input, resize, a forced
   display); the frame clock calls it directly, which keeps the runloop out of
   the per-frame path. */
- (void)zeusRender {
    NSRect b = self.bounds;
    CGImageRef img;
    FramePhases p;
    double t0, tprev, tnow;
    int px_w, px_h;
    memset(&p, 0, sizeof p);
    t0 = tprev = frame_now_ms();
    if (!self.layer || b.size.width < 1 || b.size.height < 1) return;
    /* The display's own scale: the bitmap is always Retina resolution. */
    own_ctx_scale = self.window ? self.window.backingScaleFactor : 2.0;
    if (own_ctx_scale < 1.0) own_ctx_scale = 1.0;
    px_w = (int)(b.size.width * own_ctx_scale + 0.5);
    px_h = (int)(b.size.height * own_ctx_scale + 0.5);
    if (px_w < 1 || px_h < 1) return;
    if (!own_ctx || own_ctx_w != px_w || own_ctx_h != px_h) {
        if (own_ctx) {
            CGContextRelease(own_ctx);
            own_ctx = NULL;
        }
        [own_gc release];
        own_gc = nil;
        own_ctx = CGBitmapContextCreate(NULL, (size_t)px_w, (size_t)px_h, 8,
                                        (size_t)px_w * 4, own_colorspace(),
                                        kCGImageAlphaPremultipliedFirst |
                                            kCGBitmapByteOrder32Little);
        own_ctx_w = px_w;
        own_ctx_h = px_h;
        /* The AppKit wrapper is a property of the context, not of a frame:
           building one per frame was pure churn on the hot path. */
        if (own_ctx)
            own_gc = [[NSGraphicsContext graphicsContextWithCGContext:own_ctx
                                                             flipped:YES] retain];
        self.layer.contents = nil; /* the old image belongs to the old context */
        /* Static layer properties: set them when the backing changes, never per
           frame. Writing `contentsScale` every frame marks the layer dirty and
           can force a texture reallocation — the exact cost this path exists to
           avoid. */
        self.layer.contentsGravity = kCAGravityResize;
        self.layer.contentsScale = own_ctx_scale;
        self.layer.magnificationFilter = kCAFilterLinear;
        self.layer.opaque = YES;
    }
    if (!own_ctx) return;
    /* Draw in layout points, y down (the view is flipped), with `own_ctx_scale`
       pixels per point. */
    CGContextSaveGState(own_ctx);
    CGContextTranslateCTM(own_ctx, 0, (CGFloat)px_h);
    CGContextScaleCTM(own_ctx, own_ctx_scale, -own_ctx_scale);
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:own_gc];
    tnow = frame_now_ms();
    p.setup_ms = tnow - tprev;
    tprev = tnow;
    p.more = mac_frame((int64_t)b.size.width, (int64_t)b.size.height);
    tnow = frame_now_ms();
    p.engine_ms = tnow - tprev;
    tprev = tnow;
    [NSGraphicsContext restoreGraphicsState];
    CGContextRestoreGState(own_ctx);
    img = own_image();
    tnow = frame_now_ms();
    p.image_ms = tnow - tprev;
    tprev = tnow;
    if (img) {
        /* Assigning `contents` is not a plain write. Outside a transaction with
           actions disabled, CoreAnimation treats the change as an implicit
           animation, so each frame cross-fades into the next — work per frame,
           and precisely the "the content isn't ready yet" feel this path
           showed. */
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        self.layer.contents = (id)img;
        [CATransaction commit];
        CGImageRelease(img);
    }
    p.set_ms = frame_now_ms() - tprev;
    frame_trace(t0, p);
    mac_schedule_next(p.more);
}

/* AppKit's entry: input, resize, or a forced display. */
- (void)updateLayer {
    [self zeusRender];
}

- (void)mouseMoved:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    int dirty = zeus_handle_hover((int64_t)p.x, (int64_t)p.y);
    [zeus_cursor_for(zeus_cursor()) set];
    if (dirty) [self setNeedsDisplay:YES];
}

- (void)mouseExited:(NSEvent *)event {
    (void)event;
    if (zeus_handle_hover(-1, -1)) [self setNeedsDisplay:YES];
    [zeus_cursor_for(0) set];
}

- (void)mouseDown:(NSEvent *)event {
    [[self window] makeFirstResponder:self];
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (zeus_handle_click((int64_t)p.x, (int64_t)p.y))
        [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    int dirty = zeus_handle_drag((int64_t)p.x, (int64_t)p.y);
    dirty |= zeus_handle_hover((int64_t)p.x, (int64_t)p.y);
    if (dirty) [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)event {
    (void)event;
    zeus_handle_mouseup();
}

- (void)scrollWheel:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    CGFloat dx = [event scrollingDeltaX];
    CGFloat dy = [event scrollingDeltaY];
    BOOL precise = [event hasPreciseScrollingDeltas];
    if (!precise) {
        /* Mouse notches are discrete: scale lines to points and stop exactly
           there — no coast, no rubber-band, like the platform scroll view. */
        dx *= 16.0;
        dy *= 16.0;
    }
    if (([event modifierFlags] & NSEventModifierFlagShift) && dx == 0.0 && dy != 0.0) {
        dx = dy;
        dy = 0.0;
    }
    /* Step for both: a trackpad's inertia already arrives from AppKit as a
       stream of momentum-phase events, so an engine coast on top of it ran
       the scroll twice and kept painting after the finger lifted. */
    int dirty = zeus_handle_scroll_step((int64_t)p.x, (int64_t)p.y, (int64_t)(-dx), (int64_t)(-dy));
    if (dirty) [self setNeedsDisplay:YES];
}

- (int)zeusMods:(NSEvent *)event {
    NSEventModifierFlags f = [event modifierFlags];
    int mods = 0;
    if (f & NSEventModifierFlagShift) mods |= ZEUS_MOD_SHIFT;
    if (f & NSEventModifierFlagControl) mods |= ZEUS_MOD_CTRL;
    if (f & NSEventModifierFlagOption) mods |= ZEUS_MOD_ALT;
    if (f & NSEventModifierFlagCommand) mods |= ZEUS_MOD_CMD;
    return mods;
}

- (int)zeusKeyFromEvent:(NSEvent *)event {
    int key = 0;
    unsigned short code = [event keyCode];
    if (code == 53) key = 27;
    else if (code == 51) key = 8;
    else if (code == 36) key = 13;
    else if (code == 123) key = 1000;
    else if (code == 124) key = 1001;
    else if (code == 126) key = 1002;
    else if (code == 125) key = 1003;
    else if (code == 116) key = 1004;
    else if (code == 121) key = 1005;
    else if (code == 115) key = 1006;
    else if (code == 119) key = 1007;
    else {
        NSString *chars = [event characters];
        if ([chars length] > 0) {
            unichar c = [chars characterAtIndex:0];
            if (c >= 32 && c < 127) key = (int)c;
        }
    }
    int mods = [self zeusMods:event];
    if (code == 48) key = ZEUS_K_TAB;
    if (!key || (mods & ~ZEUS_MOD_SHIFT)) {
        NSString *raw = [event charactersIgnoringModifiers];
        if ([raw length] > 0) {
            unichar c = [raw characterAtIndex:0];
            if (c >= 'A' && c <= 'Z') c = c + 32;
            if (c >= 32 && c < 127) key = (int)c;
        }
    }
    return key;
}

- (void)keyDown:(NSEvent *)event {
    int mods = [self zeusMods:event];
    unsigned short code = [event keyCode];
    if ((mods & ZEUS_MOD_CMD) && code == 9) {
        /* Cmd+V: the bare Zeus view has no Edit menu, so route the
           key equivalent straight to the pasteboard handler. */
        [self paste:nil];
        return;
    }
    if (code == 48 || code == 53 || (mods & (ZEUS_MOD_CMD | ZEUS_MOD_CTRL))) {
        int key = [self zeusKeyFromEvent:event];
        if (key && zeus_handle_key_ev(key, mods))
            [self setNeedsDisplay:YES];
        else
            [super keyDown:event];
        return;
    }
    if (zeus_focus_captures_text()) {
        [self interpretKeyEvents:@[ event ]];
        return;
    }
    int key = [self zeusKeyFromEvent:event];
    if (key && zeus_handle_key_ev(key, mods))
        [self setNeedsDisplay:YES];
    else
        [super keyDown:event];
}

- (void)insertText:(id)string {
    [self insertText:string replacementRange:NSMakeRange(NSNotFound, 0)];
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    NSString *s = [string isKindOfClass:[NSAttributedString class]]
                      ? [(NSAttributedString *)string string]
                      : (NSString *)string;
    const char *u = [s UTF8String];
    if (u && zeus_handle_text(u, (int)strlen(u))) {
        [marked release];
        marked = nil;
        [self setNeedsDisplay:YES];
    }
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange
       replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    NSString *s = [string isKindOfClass:[NSAttributedString class]]
                      ? [(NSAttributedString *)string string]
                      : (NSString *)string;
    [marked release];
    marked = [s copy];
    markedSel = selectedRange;
    const char *u = [s UTF8String];
    zeus_handle_marked(u ? u : "", u ? (int)strlen(u) : 0);
    [self setNeedsDisplay:YES];
}

- (void)unmarkText {
    [marked release];
    marked = nil;
    zeus_handle_marked("", 0);
    [self setNeedsDisplay:YES];
}

- (BOOL)hasMarkedText { return marked != nil && [marked length] > 0; }
- (NSRange)markedRange {
    return marked ? NSMakeRange(0, [marked length]) : NSMakeRange(NSNotFound, 0);
}
- (NSRange)selectedRange { return markedSel; }
- (NSArray *)validAttributesForMarkedText { return @[]; }
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return 0;
}
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range;
    if (actualRange) *actualRange = range;
    NSRect r = [self convertRect:self.bounds toView:nil];
    r = [[self window] convertRectToScreen:r];
    r.size.width = 1;
    r.size.height = 16;
    return r;
}
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range
                                                actualRange:(NSRangePointer)actualRange {
    (void)range;
    if (actualRange) *actualRange = NSMakeRange(NSNotFound, 0);
    return nil;
}

- (void)doCommandBySelector:(SEL)sel {
    int key = 0, mods = 0;
    if (sel == @selector(deleteBackward:)) key = 8;
    else if (sel == @selector(deleteForward:)) key = 127;
    else if (sel == @selector(insertNewline:)) key = 13;
    else if (sel == @selector(moveLeft:)) key = 1000;
    else if (sel == @selector(moveRight:)) key = 1001;
    else if (sel == @selector(moveUp:)) key = 1002;
    else if (sel == @selector(moveDown:)) key = 1003;
    else if (sel == @selector(moveToBeginningOfLine:) || sel == @selector(moveToLeftEndOfLine:))
        key = 1006;
    else if (sel == @selector(moveToEndOfLine:) || sel == @selector(moveToRightEndOfLine:))
        key = 1007;
    else if (sel == @selector(moveLeftAndModifySelection:)) {
        key = 1000;
        mods = ZEUS_MOD_SHIFT;
    } else if (sel == @selector(moveRightAndModifySelection:)) {
        key = 1001;
        mods = ZEUS_MOD_SHIFT;
    } else if (sel == @selector(moveUpAndModifySelection:)) {
        key = 1002;
        mods = ZEUS_MOD_SHIFT;
    } else if (sel == @selector(moveDownAndModifySelection:)) {
        key = 1003;
        mods = ZEUS_MOD_SHIFT;
    } else if (sel == @selector(insertTab:))
        key = ZEUS_K_TAB;
    else if (sel == @selector(cancelOperation:))
        key = 27;
    if (key && zeus_handle_key_ev(key, mods)) [self setNeedsDisplay:YES];
}

- (void)paste:(id)sender {
    (void)sender;
    NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    if (!s) return;
    const char *u = [s UTF8String];
    if (u && zeus_handle_text(u, (int)strlen(u))) [self setNeedsDisplay:YES];
}

- (void)keyUp:(NSEvent *)event {
    int key = 0;
    unsigned short code = [event keyCode];
    if (code == 53) key = 27;
    else if (code == 51) key = 8;
    else if (code == 36) key = 13;
    else if (code == 123) key = 1000;
    else if (code == 124) key = 1001;
    else if (code == 126) key = 1002;
    else if (code == 125) key = 1003;
    else if (code == 116) key = 1004;
    else if (code == 121) key = 1005;
    else {
        NSString *chars = [event charactersIgnoringModifiers];
        if ([chars length] > 0) {
            unichar c = [chars characterAtIndex:0];
            if (c >= 'A' && c <= 'Z') c = c + 32;
            if (c >= 32 && c < 127) key = (int)c;
        }
    }
    NSEventModifierFlags f = [event modifierFlags];
    int mods = 0;
    if (f & NSEventModifierFlagShift) mods |= ZEUS_MOD_SHIFT;
    if (f & NSEventModifierFlagControl) mods |= ZEUS_MOD_CTRL;
    if (f & NSEventModifierFlagOption) mods |= ZEUS_MOD_ALT;
    if (f & NSEventModifierFlagCommand) mods |= ZEUS_MOD_CMD;
    if (code == 48) key = ZEUS_K_TAB;
    if (key && zeus_handle_key_up(key, mods))
        [self setNeedsDisplay:YES];
    else
        [super keyUp:event];
}
@end

@interface ZeusAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation ZeusAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    (void)app;
    return YES;
}
@end

static NSRect mac_screen_visible(void);
static int mac_window_fills_screen(void);

/* Live window title (`zeus_set_title_hook`), driven by the router's
   `Meta.title` on navigation. */
static void mac_apply_title(const char *t) {
    if (!g_win) return;
    [g_win setTitle:(t && t[0]) ? [NSString stringWithUTF8String:t] : @"Zeus"];
}

static void mac_run(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        ZeusAppDelegate *del = [ZeusAppDelegate new];
        [NSApp setDelegate:del];

        NSRect frame = NSMakeRect(0, 0, (CGFloat)zeus_window_width(),
                                  (CGFloat)zeus_window_height());
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        NSWindow *win = [[NSWindow alloc] initWithContentRect:frame
                                                    styleMask:style
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        const char *t = zeus_window_title();
        [win setTitle:t ? [NSString stringWithUTF8String:t] : @"Zeus"];
        g_win = win;
        zeus_set_title_hook(mac_apply_title);
        /* The window clears to white until the engine's first paper fill, so
           scroll edges / the first frame never flash black. */
        [win setBackgroundColor:[NSColor whiteColor]];
        ZeusView *view = [[ZeusView alloc] initWithFrame:frame];
        g_view = view;
        [win setReleasedWhenClosed:YES];
        [win setRestorable:NO];
        [win setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        /* The window's colour space decides the *depth* of the store AppKit
           gives it. Left alone, a window inherits the display's ICC profile
           ("Color LCD"), which on a wide-gamut Mac is a P3/EDR space — and
           CoreAnimation then backs the window with an RGBA **half-float**
           surface: `vmmap` shows 'CA Whippet Drawable', 2940x1612 (RGhA),
           36.2 MB per buffer, three of them. That is 8 bytes per pixel where the
           pixels need 4, and it is the whole of the AppKit path's IOSurface
           cost. Pinning the window to sRGB makes the store 32-bpp BGRA and
           halves it — to ~54 MB — and costs no fidelity here, because every
           colour this host can name is already a 24-bit packed value (see
           `zeus_color`): 8 bits per channel holds all of them. `ZEUS_WIDE_GAMUT=1`
           keeps the display profile, and so the half-float store, for anyone who
           wants extended range. */
        if (!env_truthy("ZEUS_WIDE_GAMUT"))
            [win setColorSpace:[NSColorSpace sRGBColorSpace]];
        [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [win setContentView:view];
        /* The default size is the work area, so this fills the screen; any
           other size is an explicit request and is centered as asked. */
        if (mac_window_fills_screen()) {
            [win setFrame:mac_screen_visible() display:YES];
        } else {
            [win center];
        }
        /* The owned-bitmap path supplies the layer's content itself, so the view
           must be layer-backed; with `APP_KIT=true` AppKit manages the store as
           usual. */
        if (own_buffer_on()) [view setWantsLayer:YES];
        mac_link_arm();
        zeus_window_opened();
        [win makeFirstResponder:view];
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
}

#ifdef LOAM_HOST_BUILD
/* ── the development host ───────────────────────────────────────────────

   `zeli serve --target macos` builds a host (this file), the app as a shared
   image, and a shim whose `main` calls `zeus.host_run(<image>)`. The host owns
   the window and the loop, so a rebuild only swaps the image: the window stays
   up, and the app is re-registered from the new code by calling its `main`
   again. The app's `main` returns instead of starting a loop because
   `loam_zeus_plat_run` yields once the host mode is on.

   The engine entry points are re-read on every load through `zeus_app_api`
   (zeus_rt.h). A direct call would stay bound to the first image and go on
   painting a tree that no longer exists — the window would look fine and be
   stale. Images are never unloaded, so pointers the host holds into the old one
   stay valid; a dev session grows by one image per edit. */

#include <dlfcn.h>
#include <sys/stat.h>

@interface ZeusReloader : NSObject
- (void)tick:(NSTimer *)t;
@end

static char *g_image_path;
static struct timespec g_image_stamp;
static NSString *g_arg0;

/* Bind one entry point out of the image just loaded. */
#define ZEUS_BIND(field, name) \
    (*(void **)&zeus_app_api.field = dlsym(handle, name))

static int host_load_image(void) {
    struct stat st;
    void *handle;
    int (*enter)(int, char **);
    char *argv0 = (char *)[g_arg0 UTF8String];
    char *argv[2];

    handle = dlopen(g_image_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        fprintf(stderr, "zeli: host: %s\n", dlerror());
        return 0;
    }
    ZEUS_BIND(layout, "loam_zeus_engine_layout");
    ZEUS_BIND(paint, "loam_zeus_engine_paint");
    ZEUS_BIND(step, "loam_zeus_engine_step");
    ZEUS_BIND(step_dt, "loam_zeus_engine_step_dt");
    ZEUS_BIND(set_reduced_motion, "loam_zeus_engine_set_reduced_motion");
    ZEUS_BIND(next_ms, "loam_zeus_engine_next_ms");
    ZEUS_BIND(click, "loam_zeus_engine_click");
    ZEUS_BIND(scroll, "loam_zeus_engine_scroll");
    ZEUS_BIND(scroll_step, "loam_zeus_engine_scroll_step");
    ZEUS_BIND(drag, "loam_zeus_engine_drag");
    ZEUS_BIND(hover, "loam_zeus_engine_hover");
    ZEUS_BIND(mouseup, "loam_zeus_engine_mouseup");
    ZEUS_BIND(over_button, "loam_zeus_engine_over_button");
    ZEUS_BIND(cursor, "loam_zeus_engine_cursor");
    ZEUS_BIND(a11y_dump, "loam_zeus_engine_a11y_dump");
    ZEUS_BIND(tree_dump, "loam_zeus_engine_tree_dump");
    ZEUS_BIND(signals_dump, "loam_zeus_engine_signals_dump");
    ZEUS_BIND(state_load, "loam_zeus_engine_state_load");
    ZEUS_BIND(key_apply, "loam_zeus_engine_key_apply");
    ZEUS_BIND(fill_focus, "loam_zeus_engine_fill_focus");
    ZEUS_BIND(focus_depth, "loam_zeus_engine_focus_depth");
    ZEUS_BIND(focus_node, "loam_zeus_engine_focus_node");
    ZEUS_BIND(focus_ctx, "loam_zeus_engine_focus_ctx");
    ZEUS_BIND(focus_step, "loam_zeus_engine_focus_step");
    ZEUS_BIND(focus_captures_text, "loam_zeus_engine_focus_captures_text");
    ZEUS_BIND(key, "loam_zeus_engine_key");
    ZEUS_BIND(key_up, "loam_zeus_engine_key_up");
    ZEUS_BIND(set_mods, "loam_zeus_engine_set_mods");
    ZEUS_BIND(insert, "loam_zeus_engine_insert");
    ZEUS_BIND(marked, "loam_zeus_engine_marked");
    ZEUS_BIND(picked_image, "loam_zeus_engine_picked_image");
    if (!zeus_app_api.layout || !zeus_app_api.paint) {
        fprintf(stderr, "zeli: host: %s is not a Loam app image\n", g_image_path);
        return 0;
    }
    enter = (int (*)(int, char **))dlsym(handle, "main");
    if (!enter) {
        fprintf(stderr, "zeli: host: %s has no main\n", g_image_path);
        return 0;
    }
    if (stat(g_image_path, &st) == 0) g_image_stamp = st.st_mtimespec;
    argv[0] = argv0;
    argv[1] = NULL;
    enter(1, argv);
    return 1;
}

@implementation ZeusReloader
- (void)tick:(NSTimer *)t {
    struct stat st;
    loam_str snap;
    char *keep = NULL;
    (void)t;
    if (stat(g_image_path, &st) != 0) return;
    if (st.st_mtime == g_image_stamp.tv_sec &&
        st.st_mtimespec.tv_nsec == g_image_stamp.tv_nsec)
        return;
    /* Carry what the app had. The signal arena is the part worth keeping, and
       the image knows how to dump and restore it — the same pair the web
       reload uses. Read it through the *old* table, load, then hand it to the
       new one. */
    snap = zeus_app_api.signals_dump ? zeus_app_api.signals_dump() : (loam_str){"", 0};
    if (snap.ptr && snap.len > 0) {
        keep = (char *)malloc((size_t)snap.len + 1);
        if (keep) {
            memcpy(keep, snap.ptr, (size_t)snap.len);
            keep[snap.len] = 0;
        }
    }
    if (!host_load_image()) {
        free(keep);
        return;
    }
    if (keep && zeus_app_api.state_load)
        zeus_app_api.state_load((loam_str){keep, (int64_t)strlen(keep)});
    free(keep);
    fprintf(stderr, "zeli: host: reloaded %s\n", g_image_path);
    mac_redraw();
}
@end

/* `zeus.host_run(path)`: take the process over. Called from the shim's `main`. */
void loam_mac_host_run(const char *path) {
    @autoreleasepool {
        ZeusReloader *rel;
        g_image_path = strdup(path);
        g_arg0 = [[NSString alloc] initWithUTF8String:path];
        zeus_set_host_mode();
        if (!host_load_image()) exit(1);
        /* Headless: no window, but the same loop and the same reload poll, so the
           mechanism can be exercised (and gated) without a screen. */
        if (loam_zeus_plat_headless()) {
            [NSApplication sharedApplication];
            zeus_window_opened();
            rel = [ZeusReloader new];
            [NSTimer scheduledTimerWithTimeInterval:0.25
                                             target:rel
                                           selector:@selector(tick:)
                                           userInfo:nil
                                            repeats:YES];
            [NSApp run];
            return;
        }
        /* The window, exactly as `mac_run` opens it: the app has registered its
           size and title by now. */
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        ZeusAppDelegate *del = [ZeusAppDelegate new];
        [NSApp setDelegate:del];
        {
            NSRect frame = NSMakeRect(0, 0, (CGFloat)zeus_window_width(),
                                      (CGFloat)zeus_window_height());
            NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                               NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
            NSWindow *win = [[NSWindow alloc] initWithContentRect:frame
                                                        styleMask:style
                                                          backing:NSBackingStoreBuffered
                                                            defer:NO];
            const char *t = zeus_window_title();
            [win setTitle:t ? [NSString stringWithUTF8String:t] : @"Zeus"];
            g_win = win;
            zeus_set_title_hook(mac_apply_title);
            [win setBackgroundColor:[NSColor whiteColor]];
            ZeusView *view = [[ZeusView alloc] initWithFrame:frame];
            g_view = view;
            [win setReleasedWhenClosed:YES];
            [win setRestorable:NO];
            if (!env_truthy("ZEUS_WIDE_GAMUT"))
                [win setColorSpace:[NSColorSpace sRGBColorSpace]];
            [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
            [win setContentView:view];
            if (mac_window_fills_screen()) {
                [win setFrame:mac_screen_visible() display:YES];
            } else {
                [win center];
            }
            if (own_buffer_on()) [view setWantsLayer:YES];
            mac_link_arm();
            zeus_window_opened();
            [win makeFirstResponder:view];
            [win makeKeyAndOrderFront:nil];
        }
        /* Watch the image. A quarter second is under a human's impatience and
           well over a rebuild. */
        rel = [ZeusReloader new];
        [NSTimer scheduledTimerWithTimeInterval:0.25
                                         target:rel
                                       selector:@selector(tick:)
                                       userInfo:nil
                                        repeats:YES];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
}
#endif /* LOAM_HOST_BUILD */

/* ── screen-size plumbing ────────────────────────────────────────────
   The engine's pre-window window_size() and the native default window both
   come from the main screen's work area (points, like the whole engine). */
static NSRect mac_screen_visible(void) {
    @autoreleasepool {
        /* NSScreen only answers once AppKit has a window-server connection, and
           `window_size()` is read from `zeus.App` — before `mac_run` sets one
           up and outside its pool. Asking for the shared app here (idempotent)
           is what makes the pre-window size the real work area rather than a
           zero frame. */
        NSRect vis;
        [NSApplication sharedApplication];
        vis = [[NSScreen mainScreen] visibleFrame];
        if (vis.size.width < 1 || vis.size.height < 1) {
            /* No main screen yet (pre-WindowServer launch): keep a sane desktop
               default instead of a zero-sized frame. */
            vis = NSMakeRect(0, 0, (CGFloat)ZEUS_DEFAULT_WIN_W, (CGFloat)ZEUS_DEFAULT_WIN_H);
        }
        return vis;
    }
}

static int64_t mac_initial_w(void) {
    NSRect vis = mac_screen_visible();
    return (int64_t)(vis.size.width + 0.5);
}

static int64_t mac_initial_h(void) {
    NSRect vis = mac_screen_visible();
    return (int64_t)(vis.size.height + 0.5);
}

/* `zeus.App`'s default size is the host's `window_size()` — the work area — so
   a default app lands here with the whole work area and opens screen-filling.
   Any other size is an explicit request, and gets a centered window instead. */
static int mac_window_fills_screen(void) {
    NSRect vis = mac_screen_visible();
    int64_t want_w = zeus_window_width();
    int64_t want_h = zeus_window_height();
    if (vis.size.width < 1 || vis.size.height < 1) return 0;
    return (int64_t)(vis.size.width + 0.5) == want_w &&
           (int64_t)(vis.size.height + 0.5) == want_h;
}

static void mac_pick_image(char *out, int cap, int64_t *w, int64_t *h) {
    NSOpenPanel *p;
    NSString *path;
    const char *utf;
    NSImage *img;
    NSSize sz;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!out || cap <= 0) return;
    out[0] = 0;
    p = [NSOpenPanel openPanel];
    [p setCanChooseFiles:YES];
    [p setCanChooseDirectories:NO];
    [p setAllowsMultipleSelection:NO];
    [p setAllowedFileTypes:[NSArray arrayWithObjects:@"png", @"jpg", @"jpeg", @"gif",
                                                     @"webp", nil]];
    if ([p runModal] != NSModalResponseOK) return;
    path = [[p URL] path];
    utf = path ? [path UTF8String] : NULL;
    if (!utf) return;
    strncpy(out, utf, (size_t)cap - 1);
    out[cap - 1] = 0;
    img = mac_img_get(utf);
    if (!img) return;
    sz = [img size];
    if (w) *w = (int64_t)(sz.width + 0.5);
    if (h) *h = (int64_t)(sz.height + 0.5);
}

__attribute__((constructor))
static void zeus_mac_register(void) {
    zeus_set_platform(mac_run, mac_measure, mac_redraw);
#ifdef LOAM_HOST_BUILD
    /* A dev host loads the app as an image and owns the loop; the shim's
       `zeus.host_run` reaches the loader through this hook. */
    zeus_set_host_hooks(loam_mac_host_run);
#endif
    zeus_set_pick_image(mac_pick_image);
    zeus_set_image_size(mac_image_size);
    zeus_set_font_hooks(mac_load_font, NULL);
    /* `zeus.App` reads `window_size()` before any other host entry point runs,
       so the lazy work-area getters must be known by load time or the engine
       answers with the 640×480 placeholder and a default app opens as a small
       centered window. This stores two pointers only; the AppKit read happens
       on first call, and only on a real (non-headless) run. */
    zeus_set_initial_view(mac_initial_w, mac_initial_h);
}
