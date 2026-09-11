/* mac.m — Cocoa window + Core Text paint for Zeus (zeus/desktop). */
#import <Cocoa/Cocoa.h>
#include "zeus_rt.h"
#include "zeus_key.h"
#include <string.h>
#include <stdlib.h>

static NSView *g_view;

static NSFont *font_at(int64_t px) {
    static NSFont *cache[72];
    int i = (int)px;
    if (i < 8) i = 8;
    if (i > 71) i = 71;
    if (!cache[i])
        cache[i] = [[NSFont systemFontOfSize:(CGFloat)i] retain];
    return cache[i];
}

static NSColor *zeus_color(int64_t rgb) {
    CGFloat r = ((rgb >> 16) & 255) / 255.0;
    CGFloat g = ((rgb >> 8) & 255) / 255.0;
    CGFloat b = (rgb & 255) / 255.0;
    return [NSColor colorWithCalibratedRed:r green:g blue:b alpha:1.0];
}

static NSDictionary *zeus_attrs(int64_t rgb, int64_t px) {
    static NSDictionary *cache[24];
    static int64_t keys[24];
    static unsigned n;
    int64_t key = (rgb << 8) ^ (px & 255);
    for (unsigned i = 0; i < n; i++)
        if (keys[i] == key) return cache[i];
    NSDictionary *a = [@{
        NSFontAttributeName: font_at(px),
        NSForegroundColorAttributeName: zeus_color(rgb)
    } retain];
    if (n < 24) {
        keys[n] = key;
        cache[n] = a;
        n++;
    }
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

static void mac_clip(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h) {
    (void)ctx;
    NSRectClip(NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h));
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

/* PNG / JPEG / WebP / GIF via ImageIO. Cache by src; http(s) loads off-thread. */
static NSMutableDictionary *g_img;
static NSMutableSet *g_img_load;

static void mac_img_put(NSString *key, NSImage *img) {
    [g_img setObject:(img ? (id)img : (id)[NSNull null]) forKey:key];
    [img release];
}

static NSImage *mac_img_get(const char *src) {
    NSString *key;
    NSImage *img;
    if (!src || !src[0]) return nil;
    if (!g_img) {
        g_img = [[NSMutableDictionary alloc] init];
        g_img_load = [[NSMutableSet alloc] init];
    }
    key = [NSString stringWithUTF8String:src];
    img = [g_img objectForKey:key];
    if (img) return img == (id)[NSNull null] ? nil : img;
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
@end

@implementation ZeusView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)preservesContentDuringLiveResize { return NO; }

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (track) [self removeTrackingArea:track];
    track = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInKeyWindow
               owner:self
            userInfo:nil];
    [self addTrackingArea:track];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    @autoreleasepool {
        static CFAbsoluteTime last;
        CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        float dt = (last > 0) ? (float)(now - last) : (1.f / 60.f);
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        if (dt < 0.001f) dt = 1.f / 60.f;
        NSRect b = self.bounds;
        zeus_layout((int64_t)b.size.width, (int64_t)b.size.height);
        int more = zeus_step(dt);
        ZeusDraw d;
        memset(&d, 0, sizeof d);
        d.fill = mac_fill;
        d.fill_a = mac_fill_a;
        d.text = mac_text;
        d.text_rot = mac_text_rot;
        d.save = mac_save;
        d.clip = mac_clip;
        d.restore = mac_restore;
        d.svg = mac_svg;
        d.image = mac_image;
        zeus_paint(NULL, d);
        /* setNeedsDisplay inside drawRect is dropped by AppKit; queue the next frame. */
        if (more && g_view) {
            NSView *v = g_view;
            dispatch_async(dispatch_get_main_queue(), ^{
                [v setNeedsDisplay:YES];
            });
        } else if (g_view) {
            /* Idle: sleep until the next async deadline instead of drawing
               at 60 fps. -1 = a spawn is waiting, draw a frame soon. */
            int64_t due = yuga_zeus_engine_next_ms();
            if (due) {
                NSView *v = g_view;
                double delay = (due < 0) ? 0.0 : ((double)due / 1000.0);
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                             (int64_t)(delay * NSEC_PER_SEC)),
                               dispatch_get_main_queue(), ^{
                    [v setNeedsDisplay:YES];
                });
            }
        }
    }
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
    int dirty;
    if (precise)
        dirty = zeus_handle_scroll((int64_t)p.x, (int64_t)p.y, (int64_t)(-dx), (int64_t)(-dy));
    else
        dirty = zeus_handle_scroll_step((int64_t)p.x, (int64_t)p.y, (int64_t)(-dx), (int64_t)(-dy));
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
        /* The window clears to white until the engine's first paper fill, so
           scroll edges / the first frame never flash black. */
        [win setBackgroundColor:[NSColor whiteColor]];
        ZeusView *view = [[ZeusView alloc] initWithFrame:frame];
        g_view = view;
        [win setReleasedWhenClosed:YES];
        [win setRestorable:NO];
        [win setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [win setContentView:view];
        /* Native desktop default: the window fills the device screen. An app
           that asks for zeus.window_size() (or never overrides the engine's
           placeholder) gets the full main-screen work area instead of a
           centered 640×480 box. Explicit sizes are honored as-is. */
        if (mac_window_fills_screen()) {
            [win setFrame:mac_screen_visible() display:YES];
        } else {
            [win center];
        }
        zeus_window_opened();
        [win makeFirstResponder:view];
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
}

/* ── screen-size plumbing ────────────────────────────────────────────
   The engine's pre-window window_size() and the native default window both
   come from the main screen's work area (points, like the whole engine). */
static NSRect mac_screen_visible(void) {
    NSRect vis = [[NSScreen mainScreen] visibleFrame];
    if (vis.size.width < 1 || vis.size.height < 1) {
        /* No main screen yet (pre-WindowServer / headless launch): keep a
           sane desktop default instead of a zero-sized frame. */
        vis = NSMakeRect(0, 0, (CGFloat)ZEUS_DEFAULT_WIN_W, (CGFloat)ZEUS_DEFAULT_WIN_H);
    }
    return vis;
}

static int64_t mac_initial_w(void) {
    NSRect vis = mac_screen_visible();
    return (int64_t)(vis.size.width + 0.5);
}

static int64_t mac_initial_h(void) {
    NSRect vis = mac_screen_visible();
    return (int64_t)(vis.size.height + 0.5);
}

/* True when the app asked for the screen-filling default: either the size
   zeus.window_size() reported (the device screen) or the engine's built-in
   placeholder that predates any host knowledge of the device. */
static int mac_window_fills_screen(void) {
    NSRect vis = mac_screen_visible();
    int64_t want_w = zeus_window_width();
    int64_t want_h = zeus_window_height();
    if (vis.size.width < 1 || vis.size.height < 1) return 0;
    if (want_w == ZEUS_DEFAULT_WIN_W && want_h == ZEUS_DEFAULT_WIN_H) return 1;
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
    zeus_set_pick_image(mac_pick_image);
}
