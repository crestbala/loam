/* apple_draw.m — the ZeusDraw table for macOS and iOS, on CoreGraphics.
 *
 * One implementation of every paint op, text measure, font lookup and the
 * image cache, shared by desktop/mac.m and ios/ios.m (see zeus_apple.h).
 * Shapes are CGPaths; text is NSString drawing, which AppKit and UIKit both
 * provide, so what `za_measure` reports is exactly what `text` draws.
 */
#import "zeus_apple.h"
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#include <string.h>

#if TARGET_OS_IPHONE
/* UIKit contexts take shadow offsets y-down already. */
#define ZA_SHADOW_DY 1
#else
/* CoreGraphics on macOS takes shadow offsets in y-up base space, whatever the
   CTM: a positive dy (downward on screen) has to be negated. */
#define ZA_SHADOW_DY -1
#endif

static CGFloat za_alpha(int64_t alpha) {
    return alpha < 0 ? 0 : (alpha > 255 ? 1.0 : (CGFloat)alpha / 255.0);
}

ZAColor *za_color(int64_t rgb, CGFloat a) {
    CGFloat r = ((rgb >> 16) & 255) / 255.0;
    CGFloat g = ((rgb >> 8) & 255) / 255.0;
    CGFloat b = (rgb & 255) / 255.0;
#if TARGET_OS_IPHONE
    return [UIColor colorWithRed:r green:g blue:b alpha:a];
#else
    return [NSColor colorWithCalibratedRed:r green:g blue:b alpha:a];
#endif
}

static void za_set_fill(CGContextRef cg, int64_t rgb, CGFloat a) {
    CGContextSetFillColorWithColor(cg, [za_color(rgb, a) CGColor]);
}

/* --- fonts and text ------------------------------------------------------ */

static ZAFont *g_fonts[72];
static NSString *g_font_family;

void za_set_family(const char *fam) {
    for (int k = 0; k < 72; k++) {
        [g_fonts[k] release];
        g_fonts[k] = nil;
    }
    [g_font_family release];
    g_font_family = (fam && fam[0]) ? [[NSString alloc] initWithUTF8String:fam] : nil;
}

ZAFont *za_font(int64_t px) {
    int i = px < 8 ? 8 : (px > 71 ? 71 : (int)px);
    if (!g_fonts[i]) {
        ZAFont *f = g_font_family ? [ZAFont fontWithName:g_font_family size:(CGFloat)i] : nil;
        if (!f) f = [ZAFont systemFontOfSize:(CGFloat)i];
        g_fonts[i] = [f retain];
    }
    return g_fonts[i];
}

/* Register a .ttf / .otf / .ttc with the process so `fontWithName:` finds it. */
int za_load_font(const char *family, const char *src) {
    CFErrorRef err = NULL;
    NSString *path;
    (void)family;
    if (!src || !src[0]) return 0;
    path = [NSString stringWithUTF8String:src];
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) return 0;
    bool ok = CTFontManagerRegisterFontsForURL((CFURLRef)[NSURL fileURLWithPath:path],
                                               kCTFontManagerScopeProcess, &err);
    if (!ok && err) CFRelease(err);
    return ok ? 1 : 0;
}

/* Attributes are a pure function of (colour, size) and are asked for by every
   measure and every text op, so they are cached. The cache is bounded and
   evicts (releasing) once full; never storing past the cap while still
   returning a retained dictionary leaked one per text node per frame. */
static NSDictionary *za_attrs(int64_t rgb, int64_t px) {
    enum { ATTR_CACHE = 256 };
    static NSDictionary *cache[ATTR_CACHE];
    static int64_t keys[ATTR_CACHE];
    static unsigned n, next;
    int64_t key = (rgb << 8) ^ (px & 255);
    unsigned i;
    for (i = 0; i < n; i++)
        if (keys[i] == key) return cache[i];
    NSDictionary *a = [@{
        NSFontAttributeName : za_font(px),
        NSForegroundColorAttributeName : za_color(rgb, 1.0)
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

void za_measure(const char *s, int64_t px, int64_t *w, int64_t *h) {
    NSString *str = s ? [NSString stringWithUTF8String:s] : @"";
    CGSize sz = [str ?: @"" sizeWithAttributes:za_attrs(0x000000, px)];
    *w = (int64_t)(sz.width + 0.999);
    *h = (int64_t)(sz.height + 0.999);
}

static void za_text(void *ctx, int64_t x, int64_t y, const char *s, int64_t rgb, int64_t font) {
    NSString *str = s ? [NSString stringWithUTF8String:s] : @"";
    (void)ctx;
    [str ?: @"" drawAtPoint:CGPointMake((CGFloat)x, (CGFloat)y) withAttributes:za_attrs(rgb, font)];
}

/* Clockwise on screen about the top-left of the line box: the context is
   y-down, where a positive rotation turns clockwise, as on the web. */
static void za_text_rot(void *ctx, int64_t x, int64_t y, const char *s, int64_t rgb,
                        int64_t font, int64_t deg) {
    CGContextRef cg = za_cg();
    NSString *str = s ? [NSString stringWithUTF8String:s] : @"";
    (void)ctx;
    CGContextSaveGState(cg);
    CGContextTranslateCTM(cg, (CGFloat)x, (CGFloat)y);
    CGContextRotateCTM(cg, (CGFloat)deg * (CGFloat)(M_PI / 180.0));
    [str ?: @"" drawAtPoint:CGPointZero withAttributes:za_attrs(rgb, font)];
    CGContextRestoreGState(cg);
}

/* --- shapes -------------------------------------------------------------- */

/* A corner can be no rounder than half the rect's short side, so a 999 pill on
   a short box is a capsule rather than a degenerate path. */
static CGFloat za_rad(CGFloat rad, CGRect r) {
    if (rad < 0) rad = 0;
    if (rad > r.size.width / 2) rad = r.size.width / 2;
    if (rad > r.size.height / 2) rad = r.size.height / 2;
    return rad;
}

static void za_add_rrect(CGContextRef cg, CGRect r, CGFloat rad) {
    rad = za_rad(rad, r);
#if !TARGET_OS_IPHONE
    /* AppKit's default path flatness, which every NSBezierPath this host used
       to draw with: curves keep the anti-aliasing they always had. */
    CGContextSetFlatness(cg, 0.6);
#endif
    if (rad <= 0) {
        CGContextAddRect(cg, r);
        return;
    }
    CGPathRef p = CGPathCreateWithRoundedRect(r, rad, rad, NULL);
    CGContextAddPath(cg, p);
    CGPathRelease(p);
}

static void za_fill_a(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                      int64_t radius, int64_t alpha) {
    CGContextRef cg = za_cg();
    (void)ctx;
    za_set_fill(cg, rgb, za_alpha(alpha));
    za_add_rrect(cg, CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h), (CGFloat)radius);
    CGContextFillPath(cg);
}

static void za_fill(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                    int64_t radius) {
    za_fill_a(ctx, x, y, w, h, rgb, radius, 255);
}

static void za_fill_g(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                      int64_t c1, int64_t axis, int64_t radius, int64_t alpha) {
    CGContextRef cg = za_cg();
    CGRect r = CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    CGFloat locs[2] = {0.0, 1.0};
    NSArray *cols = @[ (id)[za_color(c0, 1.0) CGColor], (id)[za_color(c1, 1.0) CGColor] ];
    CGGradientRef g;
    (void)ctx;
    if (w <= 0 || h <= 0) return;
    g = CGGradientCreateWithColors(NULL, (CFArrayRef)cols, locs);
    if (!g) return;
    CGContextSaveGState(cg);
    CGContextSetAlpha(cg, za_alpha(alpha));
    za_add_rrect(cg, r, (CGFloat)radius);
    CGContextClip(cg);
    /* c0 at the top (axis 0) or the left (axis 1), as on every host. */
    CGContextDrawLinearGradient(cg, g, r.origin,
                                axis ? CGPointMake(CGRectGetMaxX(r), r.origin.y)
                                     : CGPointMake(r.origin.x, CGRectGetMaxY(r)),
                                0);
    CGContextRestoreGState(cg);
    CGGradientRelease(g);
}

/* Ring stroke, inset by half the width so the line paints inside the rect:
   the box model CSS `border` uses, and what layout assumes when it insets
   content by the border width. */
static void za_stroke(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                      int64_t radius, int64_t width, int64_t alpha) {
    CGContextRef cg = za_cg();
    CGFloat lw = (CGFloat)width;
    CGRect r = CGRectMake((CGFloat)x + lw / 2, (CGFloat)y + lw / 2, (CGFloat)w - lw,
                          (CGFloat)h - lw);
    (void)ctx;
    if (r.size.width <= 0 || r.size.height <= 0) return;
    CGContextSetStrokeColorWithColor(cg, [za_color(rgb, za_alpha(alpha)) CGColor]);
    CGContextSetLineWidth(cg, lw);
    za_add_rrect(cg, r, (CGFloat)radius - lw / 2);
    CGContextStrokePath(cg);
}

/* Per-corner radii: clockwise from the top-left, arcs tangent to each corner. */
static void za_fill4(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                     int64_t alpha, int64_t tl, int64_t tr, int64_t br, int64_t bl) {
    CGContextRef cg = za_cg();
    CGFloat X = (CGFloat)x, Y = (CGFloat)y, W = (CGFloat)w, H = (CGFloat)h;
    CGFloat lim = (W < H ? W : H) / 2;
    CGFloat a1 = (CGFloat)tl, a2 = (CGFloat)tr, a3 = (CGFloat)br, a4 = (CGFloat)bl;
    CGMutablePathRef p;
    (void)ctx;
    if (W <= 0 || H <= 0) return;
    if (a1 > lim) a1 = lim;
    if (a2 > lim) a2 = lim;
    if (a3 > lim) a3 = lim;
    if (a4 > lim) a4 = lim;
    p = CGPathCreateMutable();
    CGPathMoveToPoint(p, NULL, X + a1, Y);
    CGPathAddLineToPoint(p, NULL, X + W - a2, Y);
    if (a2 > 0) CGPathAddArcToPoint(p, NULL, X + W, Y, X + W, Y + a2, a2);
    CGPathAddLineToPoint(p, NULL, X + W, Y + H - a3);
    if (a3 > 0) CGPathAddArcToPoint(p, NULL, X + W, Y + H, X + W - a3, Y + H, a3);
    CGPathAddLineToPoint(p, NULL, X + a4, Y + H);
    if (a4 > 0) CGPathAddArcToPoint(p, NULL, X, Y + H, X, Y + H - a4, a4);
    CGPathAddLineToPoint(p, NULL, X, Y + a1);
    if (a1 > 0) CGPathAddArcToPoint(p, NULL, X, Y, X + a1, Y, a1);
    CGPathCloseSubpath(p);
    za_set_fill(cg, rgb, za_alpha(alpha));
    CGContextAddPath(cg, p);
    CGContextFillPath(cg);
    CGPathRelease(p);
}

/* A vector path the engine parsed (icons, charts, `Svg`): commands 0 move,
   1 line, 2 cubic, 3 close, in 1/64 point. `width` 0 fills (non-zero);
   otherwise a stroke `width`/64 wide with `style` = cap + 4 * join. */
static void za_path(void *ctx, const int32_t *c, int64_t n, int64_t rgb, int64_t alpha,
                    int64_t width, int64_t style) {
    CGContextRef cg = za_cg();
    const CGFloat u = 1.0 / 64.0;
    CGMutablePathRef p = CGPathCreateMutable();
    int64_t i = 0;
    (void)ctx;
    while (i < n) {
        int32_t k = c[i++];
        if (k == 0 && i + 2 <= n) {
            CGPathMoveToPoint(p, NULL, c[i] * u, c[i + 1] * u);
            i += 2;
        } else if (k == 1 && i + 2 <= n) {
            CGPathAddLineToPoint(p, NULL, c[i] * u, c[i + 1] * u);
            i += 2;
        } else if (k == 2 && i + 6 <= n) {
            CGPathAddCurveToPoint(p, NULL, c[i] * u, c[i + 1] * u, c[i + 2] * u, c[i + 3] * u,
                                  c[i + 4] * u, c[i + 5] * u);
            i += 6;
        } else if (k == 3) {
            CGPathCloseSubpath(p);
        } else {
            break;
        }
    }
    if (!CGPathIsEmpty(p)) {
        CGContextAddPath(cg, p);
        if (width == 0) {
            za_set_fill(cg, rgb, za_alpha(alpha));
            CGContextFillPath(cg);
        } else {
            int cap = (int)(style & 3), join = (int)(style >> 2);
            CGContextSaveGState(cg);
            CGContextSetStrokeColorWithColor(cg, [za_color(rgb, za_alpha(alpha)) CGColor]);
            CGContextSetLineWidth(cg, width * u);
            CGContextSetLineCap(cg, cap == 1 ? kCGLineCapRound
                                    : cap == 2 ? kCGLineCapSquare : kCGLineCapButt);
            CGContextSetLineJoin(cg, join == 1 ? kCGLineJoinRound
                                     : join == 2 ? kCGLineJoinBevel : kCGLineJoinMiter);
            CGContextStrokePath(cg);
            CGContextRestoreGState(cg);
        }
    }
    CGPathRelease(p);
}

/* --- state --------------------------------------------------------------- */

static void za_save(void *ctx) {
    (void)ctx;
    CGContextSaveGState(za_cg());
}

static void za_alpha_op(void *ctx, int64_t alpha) {
    CGContextRef cg = za_cg();
    (void)ctx;
    CGContextSaveGState(cg);
    CGContextSetAlpha(cg, za_alpha(alpha));
}

static void za_clip(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius) {
    CGContextRef cg = za_cg();
    CGRect r = CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    (void)ctx;
    if (radius > 0) {
        za_add_rrect(cg, r, (CGFloat)radius);
        CGContextClip(cg);
        return;
    }
    CGContextClipToRect(cg, r);
}

static void za_restore(void *ctx) {
    (void)ctx;
    CGContextRestoreGState(za_cg());
}

/* Translate, then scale and rotate about (ox, oy), onto the current CTM, so it
   composes with an enclosing clip and unwinds with the enclosing restore. */
static void za_xform(void *ctx, int64_t dx, int64_t dy, int64_t scale, int64_t rot, int64_t ox,
                     int64_t oy) {
    CGContextRef cg = za_cg();
    (void)ctx;
    /* `dx`, `dy` are 1/16 dp and `scale` permille. */
    CGContextTranslateCTM(cg, (CGFloat)ox + (CGFloat)dx / 16.0, (CGFloat)oy + (CGFloat)dy / 16.0);
    if (rot) CGContextRotateCTM(cg, (CGFloat)rot * (CGFloat)(M_PI / 180.0));
    if (scale != 1000) CGContextScaleCTM(cg, (CGFloat)scale / 1000.0, (CGFloat)scale / 1000.0);
    CGContextTranslateCTM(cg, (CGFloat)-ox, (CGFloat)-oy);
}

/* --- shadows ------------------------------------------------------------- */

/* A soft drop shadow under a rounded rect. The blur comes from an opaque path,
   but drawing that path would paint over whatever sits under the card, so an
   even-odd clip excludes the rect's own area: the solid fill lands outside the
   clip and only its blur survives. That keeps elevation right on translucent
   surfaces. `blur`, `dx`, `dy` are device pixels: CoreGraphics shadows ignore
   the CTM's scale. */
static void za_shadow_direct(CGContextRef cg, int64_t x, int64_t y, int64_t w, int64_t h,
                             int64_t radius, int64_t rgb, int64_t alpha, CGFloat blur,
                             CGFloat dx, CGFloat dy) {
    CGRect r = CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);
    CGFloat pad = blur * 3 + fabs(dx) + fabs(dy) + 8;
    CGContextSaveGState(cg);
    CGContextAddRect(cg, CGRectInset(r, -pad, -pad));
    za_add_rrect(cg, r, (CGFloat)radius);
    CGContextEOClip(cg);
    CGContextSetShadowWithColor(cg, CGSizeMake(dx, ZA_SHADOW_DY * dy), blur,
                                [za_color(rgb, za_alpha(alpha)) CGColor]);
    CGContextSetRGBFillColor(cg, 0, 0, 0, 1);
    za_add_rrect(cg, r, (CGFloat)radius);
    CGContextFillPath(cg);
    CGContextRestoreGState(cg);
}

#if !TARGET_OS_IPHONE
static NSMutableDictionary *g_shadow_cache;

static CGImageRef za_shadow_template(CGFloat scale, CGFloat ratio, int64_t radius, int64_t rgb,
                                     int64_t alpha, int64_t blur, int64_t dx, int64_t dy,
                                     int32_t m, int32_t side) {
    NSString *key = [NSString stringWithFormat:@"%lld,%lld,%lld,%lld,%lld,%lld,%.2f,%.3f",
                                               (long long)radius, (long long)rgb,
                                               (long long)alpha, (long long)blur, (long long)dx,
                                               (long long)dy, (double)scale, (double)ratio];
    CGImageRef ci;
    if (!g_shadow_cache) g_shadow_cache = [[NSMutableDictionary alloc] init];
    ci = (CGImageRef)[g_shadow_cache objectForKey:key];
    if (ci) return ci;
    {
        size_t pw = (size_t)((CGFloat)side * scale + 0.5);
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef bc = CGBitmapContextCreate(NULL, pw, pw, 8, 0, cs,
                                                kCGImageAlphaPremultipliedFirst |
                                                    kCGBitmapByteOrder32Host);
        CGColorSpaceRelease(cs);
        if (!bc) return NULL;
        CGContextTranslateCTM(bc, 0, (CGFloat)pw);
        CGContextScaleCTM(bc, scale, -scale);
        za_shadow_direct(bc, m, m, side - 2 * m, side - 2 * m, radius, rgb, alpha,
                         (CGFloat)blur * ratio, (CGFloat)dx * ratio, (CGFloat)dy * ratio);
        ci = CGBitmapContextCreateImage(bc);
        CGContextRelease(bc);
        if (!ci) return NULL;
    }
    if ([g_shadow_cache count] >= 256) [g_shadow_cache removeAllObjects];
    [g_shadow_cache setObject:(id)ci forKey:key];
    CGImageRelease(ci);
    return ci;
}

static void za_shadow_slice(CGContextRef cg, CGImageRef ci, CGFloat scale, int smooth, int down,
                            CGRect src, CGRect dst) {
    CGImageRef part;
    if (dst.size.width <= 0 || dst.size.height <= 0) return;
    part = CGImageCreateWithImageInRect(ci, CGRectMake(src.origin.x * scale, src.origin.y * scale,
                                                       src.size.width * scale,
                                                       src.size.height * scale));
    if (!part) return;
    CGContextSaveGState(cg);
    CGContextSetInterpolationQuality(cg, smooth ? kCGInterpolationLow : kCGInterpolationNone);
    if (down) {
        CGContextTranslateCTM(cg, dst.origin.x, dst.origin.y + dst.size.height);
        CGContextScaleCTM(cg, 1, -1);
        CGContextDrawImage(cg, CGRectMake(0, 0, dst.size.width, dst.size.height), part);
    } else {
        CGContextDrawImage(cg, dst, part);
    }
    CGContextRestoreGState(cg);
    CGImageRelease(part);
}

static int za_shadow_cached(CGContextRef cg, int64_t x, int64_t y, int64_t w, int64_t h,
                            int64_t radius, int64_t rgb, int64_t alpha, int64_t blur, int64_t dx,
                            int64_t dy) {
    CGAffineTransform ctm = CGContextGetCTM(cg);
    CGFloat scale = fabs(ctm.a), tscale, K, S, T, X0, Y0, X1, Y1, W, H;
    int32_t k = 0, m = 0, side = 0, ts = 0;
    int smooth, down = ctm.d < 0;
    CGImageRef ci;
    if (scale < 1.0 || ctm.b != 0 || ctm.c != 0 || fabs(fabs(ctm.d) - scale) > 0.001 ||
        fabs(scale - floor(scale + 0.5)) > 0.001)
        return 0;
    if (!loam_replay_shadow_geom((int32_t)w, (int32_t)h, (int32_t)radius, (int32_t)blur,
                                 (int32_t)dx, (int32_t)dy, (int32_t)(scale + 0.5), &k, &m, &side,
                                 &ts))
        return 0;
    tscale = (CGFloat)ts;
    smooth = ts != (int32_t)(scale + 0.5);
    ci = za_shadow_template(tscale, tscale / scale, radius, rgb, alpha, blur, dx, dy, m, side);
    if (!ci) return 0;
    K = (CGFloat)k;
    S = (CGFloat)side;
    T = (CGFloat)(m + radius);
    X0 = (CGFloat)(x - m);
    Y0 = (CGFloat)(y - m);
    W = (CGFloat)(w + 2 * m);
    H = (CGFloat)(h + 2 * m);
    X1 = X0 + W - K;
    Y1 = Y0 + H - K;
    {
        const CGFloat s[12][8] = {
            {0, 0, K, T, X0, Y0, K, T},
            {K, 0, 1, T, X0 + K, Y0, W - 2 * K, T},
            {S - K, 0, K, T, X1, Y0, K, T},
            {0, S - T, K, T, X0, Y0 + H - T, K, T},
            {K, S - T, 1, T, X0 + K, Y0 + H - T, W - 2 * K, T},
            {S - K, S - T, K, T, X1, Y0 + H - T, K, T},
            {0, T, T, K - T, X0, Y0 + T, T, K - T},
            {0, K, T, 1, X0, Y0 + K, T, H - 2 * K},
            {0, S - K, T, K - T, X0, Y1, T, K - T},
            {S - T, T, T, K - T, X0 + W - T, Y0 + T, T, K - T},
            {S - T, K, T, 1, X0 + W - T, Y0 + K, T, H - 2 * K},
            {S - T, S - K, T, K - T, X0 + W - T, Y1, T, K - T},
        };
        for (int i = 0; i < 12; i++)
            za_shadow_slice(cg, ci, tscale, smooth, down,
                            CGRectMake(s[i][0], s[i][1], s[i][2], s[i][3]),
                            CGRectMake(s[i][4], s[i][5], s[i][6], s[i][7]));
    }
    return 1;
}
#endif

static void za_shadow(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius,
                      int64_t rgb, int64_t alpha, int64_t blur, int64_t dx, int64_t dy) {
    CGContextRef cg = za_cg();
    (void)ctx;
    if (!cg) return;
#if !TARGET_OS_IPHONE
    if (blur > 0 && alpha > 0 && za_shadow_cached(cg, x, y, w, h, radius, rgb, alpha, blur, dx, dy))
        return;
#endif
    za_shadow_direct(cg, x, y, w, h, radius, rgb, alpha, (CGFloat)blur, (CGFloat)dx,
                     (CGFloat)dy);
}

/* --- images -------------------------------------------------------------- */

/* PNG / JPEG / WebP / GIF / HEIC via ImageIO, with the file's orientation
   applied. Cached by src; http(s) loads off-thread. A decoded photo is the
   largest thing a page holds (~48 MB for 4000x3000), so the cache is bounded
   and evicts least-recently-used first; an evicted source decodes again when
   next drawn. A failed load stays in as NSNull, a real answer rather than a
   retry every frame, and ages out like any other. */
enum { IMG_CACHE_MAX = 24 };
static NSMutableDictionary *g_img;
static NSMutableSet *g_img_load;
static NSMutableArray *g_img_lru; /* keys, least recently used first */

static CGImageRef za_decode(NSData *data) {
    CGImageSourceRef src;
    CGImageRef img = NULL;
    if (!data) return NULL;
    src = CGImageSourceCreateWithData((CFDataRef)data, NULL);
    if (!src) return NULL;
    if (CGImageSourceGetCount(src) > 0) {
        NSDictionary *props = (NSDictionary *)CGImageSourceCopyPropertiesAtIndex(src, 0, NULL);
        NSNumber *orient = [props objectForKey:(id)kCGImagePropertyOrientation];
        if (orient && [orient intValue] != 1) {
            /* Rotated or mirrored camera images: let ImageIO bake the transform
               in, at full size. */
            NSNumber *pw = [props objectForKey:(id)kCGImagePropertyPixelWidth];
            NSNumber *ph = [props objectForKey:(id)kCGImagePropertyPixelHeight];
            int big = [pw intValue] > [ph intValue] ? [pw intValue] : [ph intValue];
            NSDictionary *opt = @{
                (id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
                (id)kCGImageSourceCreateThumbnailWithTransform : @YES,
                (id)kCGImageSourceThumbnailMaxPixelSize : @(big > 0 ? big : 4096),
            };
            img = CGImageSourceCreateThumbnailAtIndex(src, 0, (CFDictionaryRef)opt);
        }
        if (!img) img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
        [props release];
    }
    CFRelease(src);
    return img;
}

static void za_img_put(NSString *key, CGImageRef img) {
    [g_img setObject:(img ? (id)img : (id)[NSNull null]) forKey:key];
    if (img) CGImageRelease(img);
    [g_img_lru removeObject:key];
    [g_img_lru addObject:key];
    while ([g_img_lru count] > IMG_CACHE_MAX) {
        NSString *old = [g_img_lru objectAtIndex:0];
        [g_img_lru removeObjectAtIndex:0];
        [g_img removeObjectForKey:old];
    }
}

static CGImageRef za_img_get(const char *src) {
    NSString *key;
    id hit;
    CGImageRef img;
    if (!src || !src[0]) return NULL;
    if (!g_img) {
        g_img = [[NSMutableDictionary alloc] init];
        g_img_load = [[NSMutableSet alloc] init];
        g_img_lru = [[NSMutableArray alloc] init];
    }
    key = [NSString stringWithUTF8String:src];
    if (!key) return NULL;
    hit = [g_img objectForKey:key];
    if (hit) {
        [g_img_lru removeObject:key];
        [g_img_lru addObject:key];
        return hit == [NSNull null] ? NULL : (CGImageRef)hit;
    }
    if ([g_img_load containsObject:key]) return NULL;
    if ([key hasPrefix:@"data:"]) {
        NSRange comma = [key rangeOfString:@","];
        NSData *data = nil;
        if (comma.location != NSNotFound)
            data = [[NSData alloc]
                initWithBase64EncodedString:[key substringFromIndex:comma.location + 1]
                                    options:NSDataBase64DecodingIgnoreUnknownCharacters];
        img = za_decode(data);
        [data release];
        za_img_put(key, img);
        return img;
    }
    if ([key hasPrefix:@"http://"] || [key hasPrefix:@"https://"]) {
        NSURL *url = [NSURL URLWithString:key];
        if (!url) {
            za_img_put(key, NULL);
            return NULL;
        }
        [g_img_load addObject:key];
        [[[NSURLSession sharedSession]
              dataTaskWithURL:url
            completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
                (void)resp;
                (void)err;
                CGImageRef im = za_decode(data);
                dispatch_async(dispatch_get_main_queue(), ^{
                    za_img_put(key, im);
                    [g_img_load removeObject:key];
                    za_redraw();
                });
            }] resume];
        return NULL;
    }
    {
        NSString *path = [key hasPrefix:@"file://"] ? [[NSURL URLWithString:key] path] : key;
        NSData *data = path ? [[NSData alloc] initWithContentsOfFile:path] : nil;
        img = za_decode(data);
        [data release];
    }
    za_img_put(key, img);
    return img;
}

void za_draw_cgimage(CGImageRef img, int64_t x, int64_t y, int64_t w, int64_t h,
                     int64_t iw, int64_t ih, int64_t radius, int64_t alpha,
                     int64_t fit, int smooth) {
    CGContextRef cg = za_cg();
    int32_t dx = 0, dy = 0, dw = 0, dh = 0;
    if (!img || !cg) return;
    loam_replay_image_fit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (int32_t)iw, (int32_t)ih,
                          (int32_t)fit, &dx, &dy, &dw, &dh);
    CGContextSaveGState(cg);
    za_add_rrect(cg, CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h), (CGFloat)radius);
    CGContextClip(cg);
    if (alpha >= 0 && alpha < 255) CGContextSetAlpha(cg, (CGFloat)alpha / 255.0);
    if (smooth) CGContextSetInterpolationQuality(cg, kCGInterpolationHigh);
    /* The context is y-down; CGContextDrawImage paints row 0 at the bottom of
       its rect, so flip about the destination. */
    CGContextTranslateCTM(cg, (CGFloat)dx, (CGFloat)(dy + dh));
    CGContextScaleCTM(cg, 1.0, -1.0);
    CGContextDrawImage(cg, CGRectMake(0, 0, (CGFloat)dw, (CGFloat)dh), img);
    CGContextRestoreGState(cg);
}

static void za_image(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, const char *src,
                     int64_t radius, int64_t alpha, int64_t fit) {
    CGImageRef img;
    int32_t slot = loam_imagesize_media_slot(za_str(src));
    (void)ctx;
    if (w <= 0 || h <= 0) return;
    if (slot >= 0) {
        za_media_draw(x, y, w, h, slot, radius, alpha, fit);
        return;
    }
    img = za_img_get(src);
    if (!img) return;
    za_draw_cgimage(img, x, y, w, h, (int64_t)CGImageGetWidth(img),
                    (int64_t)CGImageGetHeight(img), radius, alpha, fit, 0);
}

void za_image_size(const char *src, int64_t *w, int64_t *h) {
    CGImageRef img;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!src) return;
    img = za_img_get(src);
    if (!img) return;
    if (w) *w = (int64_t)CGImageGetWidth(img);
    if (h) *h = (int64_t)CGImageGetHeight(img);
}

ZeusDraw za_draw_table(void) {
    ZeusDraw d;
    memset(&d, 0, sizeof d);
    d.fill = za_fill;
    d.fill_a = za_fill_a;
    d.fill_g = za_fill_g;
    d.shadow = za_shadow;
    d.stroke = za_stroke;
    d.fill4 = za_fill4;
    d.xform = za_xform;
    d.text = za_text;
    d.text_rot = za_text_rot;
    d.save = za_save;
    d.alpha = za_alpha_op;
    d.clip = za_clip;
    d.restore = za_restore;
    d.path = za_path;
    d.image = za_image;
    return d;
}
