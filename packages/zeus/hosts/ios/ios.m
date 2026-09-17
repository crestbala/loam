/* ios.m — UIKit window + Core Graphics replay of the Zeus draw list (zeus/ios).
 *
 * UIKit is only the host: UIWindow / UIView / touches. Buttons, labels,
 * navigation bars, Dynamic Type, and semantic colors are not used. Theme,
 * layout, and paint stay in Loam — the same fill / text / clip / SVG list
 * as Cocoa and Canvas2D.
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include "zeus_rt.h"
#include "zeus_key.h"
#include <string.h>
#include <stdlib.h>

int loam_app_main(void);

static UIView *g_view;
static UIWindow *g_window;
static id g_del;

static UIFont *font_at(int64_t px) {
    static UIFont *cache[72];
    int i = (int)px;
    if (i < 8) i = 8;
    if (i > 71) i = 71;
    /* Same mapping as zeus/desktop/mac.m (system UI font at Zeus px). Not Dynamic Type. */
    if (!cache[i])
        cache[i] = [[UIFont systemFontOfSize:(CGFloat)i] retain];
    return cache[i];
}

static UIColor *zeus_color(int64_t rgb) {
    CGFloat r = ((rgb >> 16) & 255) / 255.0;
    CGFloat g = ((rgb >> 8) & 255) / 255.0;
    CGFloat b = (rgb & 255) / 255.0;
    return [UIColor colorWithRed:r green:g blue:b alpha:1.0];
}

/* Bounded, evicting cache — same fix as zeus/desktop/mac.m: past the cap the
   old code returned a retained dictionary it never stored, leaking one per text
   node per frame once the app used more colour/size pairs than the cache held. */
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

static void ios_measure(const char *s, int64_t px, int64_t *w, int64_t *h) {
    NSString *str = s ? [NSString stringWithUTF8String:s] : @"";
    NSDictionary *a = zeus_attrs(0x000000, px);
    CGSize sz = [str sizeWithAttributes:a];
    *w = (int64_t)(sz.width + 0.999);
    *h = (int64_t)(sz.height + 0.999);
}

static void ios_fill_a(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                       int64_t rgb, int64_t radius, int64_t alpha) {
    (void)ctx;
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    CGFloat r = ((rgb >> 16) & 255) / 255.0;
    CGFloat g = ((rgb >> 8) & 255) / 255.0;
    CGFloat b = (rgb & 255) / 255.0;
    CGRect rect = CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    [[UIColor colorWithRed:r green:g blue:b alpha:a] setFill];
    if (radius > 0) {
        CGFloat rad = (CGFloat)radius;
        if (rad > rect.size.width / 2) rad = rect.size.width / 2;
        if (rad > rect.size.height / 2) rad = rect.size.height / 2;
        [[UIBezierPath bezierPathWithRoundedRect:rect cornerRadius:rad] fill];
    } else {
        [[UIBezierPath bezierPathWithRect:rect] fill];
    }
}

static void ios_fill(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                     int64_t rgb, int64_t radius) {
    (void)ctx;
    CGRect r = CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    [zeus_color(rgb) setFill];
    if (radius > 0) {
        CGFloat rad = (CGFloat)radius;
        if (rad > r.size.width / 2) rad = r.size.width / 2;
        if (rad > r.size.height / 2) rad = r.size.height / 2;
        [[UIBezierPath bezierPathWithRoundedRect:r cornerRadius:rad] fill];
    } else {
        UIRectFill(r);
    }
}

static void ios_text(void *ctx, int64_t x, int64_t y, const char *s,
                     int64_t rgb, int64_t font) {
    (void)ctx;
    NSString *str = s ? [NSString stringWithUTF8String:s] : @"";
    [str drawAtPoint:CGPointMake((CGFloat)x, (CGFloat)y) withAttributes:zeus_attrs(rgb, font)];
}

/* Rotated text: UIKit is y-down, so positive degrees are clockwise on
   screen; pivot at the top-left corner of the line box. */
static void ios_text_rot(void *ctx, int64_t x, int64_t y, const char *s,
                         int64_t rgb, int64_t font, int64_t deg) {
    (void)ctx;
    NSString *str = s ? [NSString stringWithUTF8String:s] : @"";
    CGContextRef c = UIGraphicsGetCurrentContext();
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, (CGFloat)x, (CGFloat)y);
    CGContextRotateCTM(c, (CGFloat)deg * (CGFloat)0.017453292519943295);
    [str drawAtPoint:CGPointZero withAttributes:zeus_attrs(rgb, font)];
    CGContextRestoreGState(c);
}

/* Clamp a corner radius to what the rect can hold, so a RAD_FULL pill on a
   short box is a capsule rather than a degenerate path. Same rule as Cocoa. */
static CGFloat ios_rad(CGFloat rad, CGRect r) {
    if (rad < 0) rad = 0;
    if (rad > r.size.width / 2) rad = r.size.width / 2;
    if (rad > r.size.height / 2) rad = r.size.height / 2;
    return rad;
}

static UIBezierPath *ios_rrect(CGRect r, CGFloat rad) {
    rad = ios_rad(rad, r);
    if (rad <= 0) return [UIBezierPath bezierPathWithRect:r];
    return [UIBezierPath bezierPathWithRoundedRect:r cornerRadius:rad];
}

static void ios_fill_g(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                       int64_t c0, int64_t c1, int64_t axis, int64_t radius,
                       int64_t alpha) {
    CGContextRef c = UIGraphicsGetCurrentContext();
    CGRect r = CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    CGFloat comps[8];
    CGColorSpaceRef sp;
    CGGradientRef g;
    const CGFloat locs[2] = {0.0, 1.0};
    (void)ctx;
    if (w <= 0 || h <= 0) return;
    comps[0] = ((c0 >> 16) & 255) / 255.0; comps[1] = ((c0 >> 8) & 255) / 255.0;
    comps[2] = (c0 & 255) / 255.0;         comps[3] = 1.0;
    comps[4] = ((c1 >> 16) & 255) / 255.0; comps[5] = ((c1 >> 8) & 255) / 255.0;
    comps[6] = (c1 & 255) / 255.0;         comps[7] = 1.0;
    sp = CGColorSpaceCreateDeviceRGB();
    g = CGGradientCreateWithColorComponents(sp, comps, locs, 2);
    CGContextSaveGState(c);
    CGContextSetAlpha(c, a);
    [ios_rrect(r, (CGFloat)radius) addClip];
    CGContextDrawLinearGradient(c, g, r.origin,
        axis ? CGPointMake(r.origin.x + r.size.width, r.origin.y)
             : CGPointMake(r.origin.x, r.origin.y + r.size.height), 0);
    CGContextRestoreGState(c);
    CGGradientRelease(g);
    CGColorSpaceRelease(sp);
}

/* Soft drop shadow. Same construction as the Cocoa host: an even-odd clip
   excludes the rect's own area, so the opaque path that generates the blur
   never paints over the surface the card sits on. UIKit is y-down, so dy
   needs no sign flip (unlike Cocoa). */
static void ios_shadow(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                       int64_t radius, int64_t rgb, int64_t alpha, int64_t blur,
                       int64_t dx, int64_t dy) {
    CGContextRef c = UIGraphicsGetCurrentContext();
    CGRect r = CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    CGFloat pad = (CGFloat)(blur * 3 + (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + 8);
    UIColor *col = [zeus_color(rgb) colorWithAlphaComponent:a];
    UIBezierPath *hole = [UIBezierPath bezierPathWithRect:CGRectInset(r, -pad, -pad)];
    (void)ctx;
    CGContextSaveGState(c);
    [hole appendPath:ios_rrect(r, (CGFloat)radius)];
    [hole setUsesEvenOddFillRule:YES];
    [hole addClip];
    CGContextSetShadowWithColor(c, CGSizeMake((CGFloat)dx, (CGFloat)dy),
                                (CGFloat)blur, col.CGColor);
    [[UIColor blackColor] setFill];
    [ios_rrect(r, (CGFloat)radius) fill];
    CGContextRestoreGState(c);
}

static void ios_stroke(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                       int64_t rgb, int64_t radius, int64_t width, int64_t alpha) {
    CGFloat lw = (CGFloat)width;
    CGRect r = CGRectMake((CGFloat)x + lw / 2, (CGFloat)y + lw / 2,
                          (CGFloat)w - lw, (CGFloat)h - lw);
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    UIBezierPath *p;
    (void)ctx;
    if (r.size.width <= 0 || r.size.height <= 0) return;
    p = ios_rrect(r, (CGFloat)radius - lw / 2);
    [p setLineWidth:lw];
    [[zeus_color(rgb) colorWithAlphaComponent:a] setStroke];
    [p stroke];
}

static void ios_fill4(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                      int64_t rgb, int64_t alpha, int64_t tl, int64_t tr,
                      int64_t br, int64_t bl) {
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    CGFloat X = (CGFloat)x, Y = (CGFloat)y, W = (CGFloat)w, H = (CGFloat)h;
    CGFloat lim = (W < H ? W : H) / 2;
    CGFloat a1 = (CGFloat)tl, a2 = (CGFloat)tr, a3 = (CGFloat)br, a4 = (CGFloat)bl;
    UIBezierPath *p = [UIBezierPath bezierPath];
    (void)ctx;
    if (W <= 0 || H <= 0) return;
    if (a1 > lim) a1 = lim;
    if (a2 > lim) a2 = lim;
    if (a3 > lim) a3 = lim;
    if (a4 > lim) a4 = lim;
    [p moveToPoint:CGPointMake(X + a1, Y)];
    [p addLineToPoint:CGPointMake(X + W - a2, Y)];
    if (a2 > 0)
        [p addArcWithCenter:CGPointMake(X + W - a2, Y + a2) radius:a2
                 startAngle:(CGFloat)-M_PI_2 endAngle:0 clockwise:YES];
    [p addLineToPoint:CGPointMake(X + W, Y + H - a3)];
    if (a3 > 0)
        [p addArcWithCenter:CGPointMake(X + W - a3, Y + H - a3) radius:a3
                 startAngle:0 endAngle:(CGFloat)M_PI_2 clockwise:YES];
    [p addLineToPoint:CGPointMake(X + a4, Y + H)];
    if (a4 > 0)
        [p addArcWithCenter:CGPointMake(X + a4, Y + H - a4) radius:a4
                 startAngle:(CGFloat)M_PI_2 endAngle:(CGFloat)M_PI clockwise:YES];
    [p addLineToPoint:CGPointMake(X, Y + a1)];
    if (a1 > 0)
        [p addArcWithCenter:CGPointMake(X + a1, Y + a1) radius:a1
                 startAngle:(CGFloat)M_PI endAngle:(CGFloat)(3 * M_PI_2) clockwise:YES];
    [p closePath];
    [[zeus_color(rgb) colorWithAlphaComponent:a] setFill];
    [p fill];
}

static void ios_xform(void *ctx, int64_t dx, int64_t dy, int64_t scale,
                      int64_t rot, int64_t ox, int64_t oy) {
    CGContextRef c = UIGraphicsGetCurrentContext();
    (void)ctx;
    CGContextTranslateCTM(c, (CGFloat)(ox + dx), (CGFloat)(oy + dy));
    if (rot) CGContextRotateCTM(c, (CGFloat)rot * (CGFloat)0.017453292519943295);
    if (scale != 100) {
        CGFloat k = (CGFloat)scale / 100.0;
        CGContextScaleCTM(c, k, k);
    }
    CGContextTranslateCTM(c, (CGFloat)-ox, (CGFloat)-oy);
}

static void ios_save(void *ctx) {
    (void)ctx;
    CGContextSaveGState(UIGraphicsGetCurrentContext());
}

static void ios_clip(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                     int64_t radius) {
    CGRect r = CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    (void)ctx;
    if (radius > 0) {
        [ios_rrect(r, (CGFloat)radius) addClip];
        return;
    }
    UIRectClip(r);
}

static void ios_restore(void *ctx) {
    (void)ctx;
    CGContextRestoreGState(UIGraphicsGetCurrentContext());
}

/* --- SVG (same subset as zeus/desktop/mac.m; UIBezierPath instead of NSBezierPath) --- */

typedef struct {
    const char *p;
} SvgScan;

typedef struct {
    int has_fill, has_stroke;
    int64_t fill, stroke;
    float sw;
    int cap;
    int join;
} SvgStyle;

static void svg_skip(SvgScan *s) {
    while (*s->p && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' ||
                     *s->p == '\r' || *s->p == ','))
        s->p++;
}

static int svg_num(SvgScan *s, float *out) {
    svg_skip(s);
    const char *p = s->p;
    char *end = NULL;
    float v;
    if (!*p) return 0;
    v = strtof(p, &end);
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
            int i = 0;
            if (q != '"' && q != '\'') return 0;
            p++;
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

static UIColor *svg_uicolor(int64_t rgb, int64_t alpha) {
    CGFloat a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    return [UIColor colorWithRed:((rgb >> 16) & 255) / 255.0
                           green:((rgb >> 8) & 255) / 255.0
                            blue:(rgb & 255) / 255.0
                           alpha:a];
}

static void svg_paint_path(UIBezierPath *path, const SvgStyle *st, float scale,
                           int64_t alpha) {
    if (!path || [path isEmpty]) return;
    path.lineWidth = (CGFloat)(st->sw * scale);
    path.lineCapStyle = st->cap == 1 ? kCGLineCapRound
                      : st->cap == 2 ? kCGLineCapSquare
                                     : kCGLineCapButt;
    path.lineJoinStyle = st->join == 1 ? kCGLineJoinRound
                       : st->join == 2 ? kCGLineJoinBevel
                                       : kCGLineJoinMiter;
    if (st->has_fill) {
        [svg_uicolor(st->fill, alpha) setFill];
        [path fill];
    }
    if (st->has_stroke) {
        [svg_uicolor(st->stroke, alpha) setStroke];
        [path stroke];
    }
}

static CGPoint svg_xf(float x, float y, float ox, float oy, float s) {
    return CGPointMake((CGFloat)(ox + x * s), (CGFloat)(oy + y * s));
}

static int svg_cmd_is(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void svg_parse_d(const char *d, float ox, float oy, float s, UIBezierPath *path) {
    SvgScan sc = {d};
    float cx = 0, cy = 0, sx = 0, sy = 0, ox1 = 0, oy1 = 0;
    int prev = 0, started = 0;
    while (1) {
        int cmd, rel, op;
        svg_skip(&sc);
        if (!*sc.p) break;
        cmd = *sc.p;
        if (svg_cmd_is(cmd)) {
            sc.p++;
        } else {
            if (!prev) break;
            cmd = (prev == 'M') ? 'L' : (prev == 'm') ? 'l' : prev;
        }
        rel = cmd >= 'a';
        op = rel ? cmd - 32 : cmd;
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
            [path addLineToPoint:svg_xf(cx, cy, ox, oy, s)];
        } else if (op == 'H') {
            float x;
            if (!svg_num(&sc, &x)) break;
            if (rel) x += cx;
            cx = x;
            [path addLineToPoint:svg_xf(cx, cy, ox, oy, s)];
        } else if (op == 'V') {
            float y;
            if (!svg_num(&sc, &y)) break;
            if (rel) y += cy;
            cy = y;
            [path addLineToPoint:svg_xf(cx, cy, ox, oy, s)];
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
            [path addCurveToPoint:svg_xf(x, y, ox, oy, s)
                    controlPoint1:svg_xf(x1, y1, ox, oy, s)
                    controlPoint2:svg_xf(x2, y2, ox, oy, s)];
            ox1 = x2;
            oy1 = y2;
            cx = x;
            cy = y;
        } else if (op == 'S') {
            float x2, y2, x, y, x1, y1;
            if (!svg_num(&sc, &x2) || !svg_num(&sc, &y2) || !svg_num(&sc, &x) ||
                !svg_num(&sc, &y))
                break;
            if (rel) {
                x2 += cx;
                y2 += cy;
                x += cx;
                y += cy;
            }
            x1 = cx;
            y1 = cy;
            if (prev == 'C' || prev == 'c' || prev == 'S' || prev == 's') {
                x1 = 2 * cx - ox1;
                y1 = 2 * cy - oy1;
            }
            [path addCurveToPoint:svg_xf(x, y, ox, oy, s)
                    controlPoint1:svg_xf(x1, y1, ox, oy, s)
                    controlPoint2:svg_xf(x2, y2, ox, oy, s)];
            ox1 = x2;
            oy1 = y2;
            cx = x;
            cy = y;
        } else if (op == 'Q') {
            float qx, qy, x, y, x1, y1, x2, y2;
            if (!svg_num(&sc, &qx) || !svg_num(&sc, &qy) || !svg_num(&sc, &x) ||
                !svg_num(&sc, &y))
                break;
            if (rel) {
                qx += cx;
                qy += cy;
                x += cx;
                y += cy;
            }
            x1 = cx + 2.f / 3.f * (qx - cx);
            y1 = cy + 2.f / 3.f * (qy - cy);
            x2 = x + 2.f / 3.f * (qx - x);
            y2 = y + 2.f / 3.f * (qy - y);
            [path addCurveToPoint:svg_xf(x, y, ox, oy, s)
                    controlPoint1:svg_xf(x1, y1, ox, oy, s)
                    controlPoint2:svg_xf(x2, y2, ox, oy, s)];
            ox1 = qx;
            oy1 = qy;
            cx = x;
            cy = y;
        } else if (op == 'T') {
            float x, y, qx, qy, x1, y1, x2, y2;
            if (!svg_num(&sc, &x) || !svg_num(&sc, &y)) break;
            if (rel) {
                x += cx;
                y += cy;
            }
            qx = cx;
            qy = cy;
            if (prev == 'Q' || prev == 'q' || prev == 'T' || prev == 't') {
                qx = 2 * cx - ox1;
                qy = 2 * cy - oy1;
            }
            x1 = cx + 2.f / 3.f * (qx - cx);
            y1 = cy + 2.f / 3.f * (qy - cy);
            x2 = x + 2.f / 3.f * (qx - x);
            y2 = y + 2.f / 3.f * (qy - y);
            [path addCurveToPoint:svg_xf(x, y, ox, oy, s)
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
        int nlen, self_close, tlen;
        const char *name, *gt;
        char tag[2048];
        SvgStyle kid;
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
        name = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '/' && *p != '>') p++;
        nlen = (int)(p - name);
        self_close = 0;
        gt = svg_tag_end(p, &self_close);
        tlen = (int)(gt - name);
        if (tlen > 2047) tlen = 2047;
        memcpy(tag, name, (size_t)tlen);
        tag[tlen] = 0;
        p = gt;
        kid = st;
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
                UIBezierPath *path = [UIBezierPath bezierPath];
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
                CGPoint c = svg_xf(cx, cy, ox, oy, scale);
                CGFloat rr = (CGFloat)(r * scale);
                UIBezierPath *path =
                    [UIBezierPath bezierPathWithOvalInRect:CGRectMake(c.x - rr, c.y - rr,
                                                                      rr * 2, rr * 2)];
                svg_paint_path(path, &kid, scale, alpha);
            }
        }
        (void)self_close;
    }
    *pp = p;
}

static void ios_svg(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                    const char *markup, int64_t rgb, int64_t alpha) {
    float vb_x = 0, vb_y = 0, vb_w = 24, vb_h = 24;
    const char *svg, *name, *gt, *body;
    char tag[2048], vb[128];
    int self_close = 0, tlen;
    float sx, sy, scale, ox, oy;
    SvgStyle st;
    (void)ctx;
    if (!markup || w <= 0 || h <= 0) return;
    svg = strstr(markup, "<svg");
    if (!svg) svg = markup;
    name = svg + 1;
    while (*name == ' ') name++;
    gt = svg_tag_end(name, &self_close);
    tlen = (int)(gt - name);
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
    sx = (float)w / vb_w;
    sy = (float)h / vb_h;
    scale = sx < sy ? sx : sy;
    ox = (float)x + ((float)w - vb_w * scale) * 0.5f - vb_x * scale;
    oy = (float)y + ((float)h - vb_h * scale) * 0.5f - vb_y * scale;
    memset(&st, 0, sizeof st);
    st.sw = 1.f;
    st.cap = 1;
    st.join = 1;
    svg_style_from(tag, &st, rgb);
    body = gt;
    svg_draw_tree(&body, st, ox, oy, scale, rgb, alpha, NULL);
}

/* PNG / JPEG / WebP / GIF via ImageIO. Cache by src; http(s) loads off-thread.
   A decoded image is the largest thing a page can hold — one 4000×3000 photo is
   ~48 MB resident — and an unbounded cache pins every source a feed ever drew
   for the life of the process. iPhone memory limits make that worse than on the
   desktop. The cache is bounded and evicts least-recently-used first; an evicted
   source just decodes again on the next frame that draws it. */
enum { IMG_CACHE_MAX = 24 };
static NSMutableDictionary *g_img;
static NSMutableSet *g_img_load;
static NSMutableArray *g_img_lru; /* keys, least recently used first */

static void img_lru_touch(NSString *key) {
    [g_img_lru removeObject:key];
    [g_img_lru addObject:key];
}

static void ios_img_put(NSString *key, UIImage *img) {
    [g_img setObject:(img ? (id)img : (id)[NSNull null]) forKey:key];
    [img release];
    img_lru_touch(key);
    while ([g_img_lru count] > IMG_CACHE_MAX) {
        NSString *old = [g_img_lru objectAtIndex:0];
        [g_img_lru removeObjectAtIndex:0];
        [g_img removeObjectForKey:old];
    }
}

static UIImage *ios_img_get(const char *src) {
    NSString *key;
    UIImage *img;
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
        img = data ? [[UIImage alloc] initWithData:data] : nil;
        [data release];
        ios_img_put(key, img);
        return img;
    }
    if ([key hasPrefix:@"http://"] || [key hasPrefix:@"https://"]) {
        NSURL *url = [NSURL URLWithString:key];
        if (!url) {
            ios_img_put(key, nil);
            return nil;
        }
        [g_img_load addObject:key];
        [[[NSURLSession sharedSession] dataTaskWithURL:url
                                     completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
            (void)resp;
            (void)err;
            UIImage *im = data ? [[UIImage alloc] initWithData:data] : nil;
            dispatch_async(dispatch_get_main_queue(), ^{
                ios_img_put(key, im);
                [g_img_load removeObject:key];
                if (g_view) [g_view setNeedsDisplay];
            });
        }] resume];
        return nil;
    }
    if ([key hasPrefix:@"file://"])
        img = [[UIImage alloc] initWithContentsOfFile:[[NSURL URLWithString:key] path]];
    else
        img = [[UIImage alloc] initWithContentsOfFile:key];
    ios_img_put(key, img);
    return img;
}

static void ios_image_size(const char *src, int64_t *w, int64_t *h) {
    UIImage *img;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!src) return;
    img = ios_img_get(src);
    if (!img) return;
    if (w) *w = (int64_t)(img.size.width + 0.5);
    if (h) *h = (int64_t)(img.size.height + 0.5);
}

static CGRect ios_image_dest(int64_t x, int64_t y, int64_t w, int64_t h,
                             int64_t iw, int64_t ih, int64_t fit) {
    int64_t dw, dh;
    if (fit != 1 && fit != 2 && fit != 3) return CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    if (iw <= 0 || ih <= 0) return CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    if (fit == 3) return CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)iw, (CGFloat)ih);
    if ((fit == 1 && iw * h <= ih * w) || (fit == 2 && iw * h >= ih * w)) {
        dh = h;
        dw = iw * h / ih;
    } else {
        dw = w;
        dh = ih * w / iw;
    }
    return CGRectMake((CGFloat)(x + (w - dw) / 2), (CGFloat)(y + (h - dh) / 2),
                      (CGFloat)dw, (CGFloat)dh);
}

static void ios_image(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h,
                      const char *src, int64_t radius, int64_t alpha, int64_t fit) {
    UIImage *img;
    CGRect box, dest;
    CGFloat a, rad;
    (void)ctx;
    if (w <= 0 || h <= 0) return;
    img = ios_img_get(src);
    if (!img) return;
    a = alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
    box = CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    dest = ios_image_dest(x, y, w, h, (int64_t)(img.size.width + 0.5),
                          (int64_t)(img.size.height + 0.5), fit);
    CGContextSaveGState(UIGraphicsGetCurrentContext());
    if (radius > 0) {
        rad = (CGFloat)radius;
        if (rad > box.size.width / 2) rad = box.size.width / 2;
        if (rad > box.size.height / 2) rad = box.size.height / 2;
        [[UIBezierPath bezierPathWithRoundedRect:box cornerRadius:rad] addClip];
    } else {
        [[UIBezierPath bezierPathWithRect:box] addClip];
    }
    [img drawInRect:dest blendMode:kCGBlendModeNormal alpha:a];
    CGContextRestoreGState(UIGraphicsGetCurrentContext());
}

static void ios_redraw(void) {
    [g_view setNeedsDisplay];
}

static CGRect zeus_content_rect(UIView *v) {
    /* Full view: Zeus theme paints every pixel. No UIKit chrome insets. */
    return v.bounds;
}

static CGPoint zeus_content_point(UIView *v, CGPoint p) {
    (void)v;
    return p;
}

/* Keep the software keyboard in step with the engine's focus.
  
   The engine owns the field and its contents; the host supplies only the
   keyboard. The browser host does the same with a hidden <textarea> (`syncIme`
   in hosts/web/loader.js): focus it while `zeus_focus_captures_text()` and blur
   it otherwise. Call after anything that can move focus — today, a touch. */
static void zeus_sync_keyboard(UIView *v) {
    if (zeus_focus_captures_text()) {
        if (![v isFirstResponder]) [v becomeFirstResponder];
    } else if ([v isFirstResponder]) {
        [v resignFirstResponder];
    }
}

/* `UIKeyInput` is what raises the software keyboard on a device, and what routes
   a hardware keyboard (Simulator, or a paired one) to `insertText:` /
   `deleteBackward`. Without it the host has no text path at all: taps move
   focus, and typing goes nowhere. */
@interface ZeusView : UIView <UIKeyInput> {
    CADisplayLink *link;
    CGPoint last;
    int tracking;
    int scrolling;
}
@end

@implementation ZeusView
- (id)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.opaque = YES;
        self.multipleTouchEnabled = NO;
        self.contentMode = UIViewContentModeRedraw;
        self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        link = [[CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)] retain];
        [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    }
    return self;
}

- (void)dealloc {
    [link invalidate];
    [link release];
    [super dealloc];
}

- (void)tick:(CADisplayLink *)l {
    (void)l;
    [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)dirty {
    CGRect b;
    float dt;
    ZeusDraw d;
    (void)dirty;
    b = zeus_content_rect(self);
    {
        UIEdgeInsets in = self.safeAreaInsets;
        zeus_set_insets((int64_t)in.top, (int64_t)in.right,
                        (int64_t)in.bottom, (int64_t)in.left);
    }
    dt = (float)link.duration;
    if (dt <= 0.f || dt > 0.05f) dt = 1.f / 60.f;
    zeus_layout((int64_t)b.size.width, (int64_t)b.size.height);
    (void)zeus_step(dt);
    memset(&d, 0, sizeof d);
    d.fill = ios_fill;
    d.fill_a = ios_fill_a;
    d.text = ios_text;
    d.text_rot = ios_text_rot;
    d.fill_g = ios_fill_g;
    d.shadow = ios_shadow;
    d.stroke = ios_stroke;
    d.fill4 = ios_fill4;
    d.xform = ios_xform;
    d.save = ios_save;
    d.clip = ios_clip;
    d.restore = ios_restore;
    d.svg = ios_svg;
    d.image = ios_image;
    zeus_paint(NULL, d);
}

- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event {
    UITouch *t = [touches anyObject];
    CGPoint p;
    int dirty;
    (void)event;
    if (!t) return;
    p = zeus_content_point(self, [t locationInView:self]);
    last = p;
    tracking = 1;
    scrolling = 0;
    /* A finger has no hover, so the touch *is* the pointer: report the position
       before the press. Without this a tap only ever produces a click, and
       hover-driven widgets (chart tooltips, hover states) never appear on iOS —
       while the same tap works on Cocoa and Canvas2D, where a mouse move has
       already hovered that point. Left in place on `touchesEnded`, so a tap
       keeps its tooltip rather than clearing it when the finger lifts. */
    dirty = zeus_handle_hover((int64_t)p.x, (int64_t)p.y);
    if (zeus_handle_click((int64_t)p.x, (int64_t)p.y)) dirty = 1;
    /* The press may have moved focus into or out of a text field. */
    zeus_sync_keyboard(self);
    if (dirty) [self setNeedsDisplay];
}

- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event {
    UITouch *t = [touches anyObject];
    CGPoint p;
    int64_t dx, dy;
    int dirty;
    (void)event;
    if (!t || !tracking) return;
    p = zeus_content_point(self, [t locationInView:self]);
    dx = (int64_t)(last.x - p.x);
    dy = (int64_t)(last.y - p.y);
    dirty = zeus_handle_drag((int64_t)p.x, (int64_t)p.y);
    dirty |= zeus_handle_hover((int64_t)p.x, (int64_t)p.y);
    if (!scrolling && (dx * dx + dy * dy > 64)) scrolling = 1;
    if (scrolling && (dx || dy))
        dirty |= zeus_handle_scroll((int64_t)p.x, (int64_t)p.y, dx, dy);
    last = p;
    if (dirty) [self setNeedsDisplay];
}

- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)touches;
    (void)event;
    tracking = 0;
    scrolling = 0;
    zeus_handle_mouseup();
    [self setNeedsDisplay];
}

- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event {
    [self touchesEnded:touches withEvent:event];
}

/* ── text input ──────────────────────────────────────────────────────────
   The engine owns the field and its contents, so there is nothing to store
   here: each call forwards to the engine and repaints if it says the tree
   changed. Key codes match mac.m and the browser host — backspace 8, Return 13
   (`insertNewline:` there, `Enter` in loader.js) — so a single-line field
   submits instead of growing a newline. */
- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (BOOL)hasText {
    return zeus_focus_captures_text() ? YES : NO;
}

- (void)insertText:(NSString *)text {
    const char *u;
    if ([text isEqualToString:@"\n"]) {
        if (zeus_handle_key_ev(13, 0)) [self setNeedsDisplay];
        return;
    }
    u = [text UTF8String];
    if (u && zeus_handle_text(u, (int)strlen(u))) [self setNeedsDisplay];
}

- (void)deleteBackward {
    if (zeus_handle_key_ev(8, 0)) [self setNeedsDisplay];
}

/* IME composition, kept separate from committed text exactly as mac.m does with
   setMarkedText / unmarkText. Implementing these without adopting the whole
   `UITextInput` protocol is enough for UIKit to use them when present. */
- (void)setMarkedText:(NSString *)markedText selectedRange:(NSRange)selectedRange {
    const char *u;
    (void)selectedRange;
    u = [markedText UTF8String];
    if (u && zeus_handle_marked(u, (int)strlen(u))) [self setNeedsDisplay];
}

- (void)unmarkText {
    if (zeus_handle_marked("", 0)) [self setNeedsDisplay];
}
@end

@interface ZeusViewController : UIViewController <UINavigationControllerDelegate,
                                                  UIImagePickerControllerDelegate>
@end

@implementation ZeusViewController
- (BOOL)prefersStatusBarHidden {
    return YES;
}
- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}
- (void)imagePickerController:(UIImagePickerController *)p
    didFinishPickingMediaWithInfo:(NSDictionary *)info {
    UIImage *img = [info objectForKey:UIImagePickerControllerOriginalImage];
    NSData *data;
    NSString *path;
    [p dismissViewControllerAnimated:YES completion:nil];
    if (!img) return;
    data = UIImageJPEGRepresentation(img, 0.9f);
    path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"zeus-pick.jpg"];
    [data writeToFile:path atomically:YES];
    zeus_picked_image([path UTF8String], (int64_t)(img.size.width + 0.5),
                      (int64_t)(img.size.height + 0.5));
}
- (void)imagePickerControllerDidCancel:(UIImagePickerController *)p {
    [p dismissViewControllerAnimated:YES completion:nil];
}
@end

@interface ZeusAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, retain) UIWindow *window;
@end

@implementation ZeusAppDelegate
@synthesize window;
- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    (void)application;
    (void)launchOptions;
    g_del = self;
    loam_app_main();
    return YES;
}
- (void)dealloc {
    [window release];
    [super dealloc];
}
@end

static void ios_run(void) {
    CGRect screen = [[UIScreen mainScreen] bounds];
    ZeusViewController *vc = [[ZeusViewController alloc] init];
    ZeusView *view = [[ZeusView alloc] initWithFrame:screen];
    UIWindow *win = [[UIWindow alloc] initWithFrame:screen];
    vc.view = view;
    g_view = view;
    g_window = win;
    win.rootViewController = vc;
    [win makeKeyAndVisible];
    if (g_del)
        ((ZeusAppDelegate *)g_del).window = win;
    [view release];
    [vc release];
}

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil,
                                 NSStringFromClass([ZeusAppDelegate class]));
    }
}

static void ios_pick_image(char *out, int cap, int64_t *w, int64_t *h) {
    UIViewController *vc;
    UIImagePickerController *p;
    if (w) *w = 0;
    if (h) *h = 0;
    if (out && cap > 0) out[0] = 0;
    vc = g_window ? g_window.rootViewController : nil;
    if (!vc) return;
    p = [[UIImagePickerController alloc] init];
    p.sourceType = UIImagePickerControllerSourceTypePhotoLibrary;
    p.delegate = (id)vc;
    [vc presentViewController:p animated:YES completion:nil];
    [p release];
}

__attribute__((constructor))
static void zeus_ios_register(void) {
    zeus_set_platform(ios_run, ios_measure, ios_redraw);
    zeus_set_pick_image(ios_pick_image);
    zeus_set_image_size(ios_image_size);
}
