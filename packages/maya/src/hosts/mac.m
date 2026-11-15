/* mac.m — Cocoa 2D present for Maya (no Metal).
 *
 * Host only: the window, input, vsync and Cocoa drawing. The scene, tracer,
 * map and what each frame draws are Loam (`maya.loam`, `mayacore/`). The
 * boundary is the plain C ABI both ways, with no Loam type in this file:
 * the host calls the int-only `engine_*` exports below, and Loam calls the
 * `maya_mac_*` primitives at the bottom (bound as `#[os("macos")] #[c]` fns,
 * so this file is a `#![c_source]` of `maya.loam`, built on macOS only).
 * Text arrives as a NUL-terminated `const char *` and the frame as RGBA
 * bytes, each valid for the call.
 */
#import <Cocoa/Cocoa.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* The Loam exports this host calls (`maya.loam`). */
int32_t loam_maya_engine_is_map(void);
void loam_maya_engine_set_viewport(int32_t w, int32_t h);
void loam_maya_engine_fixed_update(int32_t dt_ms);
void loam_maya_engine_frame(void);
void loam_maya_engine_present(void);
void loam_maya_engine_input_key(int32_t key);
void loam_maya_engine_cam_orbit(int32_t dx, int32_t dy);
void loam_maya_engine_cam_zoom(int32_t dy100);
void loam_maya_engine_cam_pan(int32_t dx, int32_t dy);
void loam_maya_engine_note_present(int32_t dt_ms);

#define MAYA_DT (1.0 / 60.0)

static NSWindow *g_win;
static NSView *g_view;
static NSTimer *g_timer;
static double g_acc, g_last;
static int g_inited_time;
static NSPoint g_mouse;
static int g_have_mouse;

/* The view being drawn: what the primitives below fill. */
static NSRect g_bounds;

static NSString *ys(const char *s) {
    if (!s || !s[0]) return @"";
    return [NSString stringWithUTF8String:s] ?: @"";
}

static NSColor *rgb_a(uint8_t r, uint8_t g, uint8_t b, CGFloat a) {
    return [NSColor colorWithCalibratedRed:(CGFloat)r / 255.0
                                     green:(CGFloat)g / 255.0
                                      blue:(CGFloat)b / 255.0
                                     alpha:a];
}

static NSDictionary *hud_attrs(void) {
    return @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:13.0
                                                         weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: rgb_a(210, 214, 235, 1.0)
    };
}

static NSDictionary *name_attrs(void) {
    return @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12.0
                                                         weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: rgb_a(230, 232, 242, 1.0)
    };
}

static void fill_oval(CGFloat cx, CGFloat cy, CGFloat rad, NSColor *c) {
    if (rad < 0.5) rad = 0.5;
    [c setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - rad, cy - rad,
                                                       rad * 2.0, rad * 2.0)] fill];
}

static void stroke_oval(CGFloat cx, CGFloat cy, CGFloat rad, NSColor *c, CGFloat w) {
    if (rad < 0.5) return;
    NSBezierPath *p =
        [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - rad, cy - rad,
                                                          rad * 2.0, rad * 2.0)];
    [c setStroke];
    p.lineWidth = w;
    [p stroke];
}

static void draw_stars(NSRect bounds) {
    int i;
    [[NSColor colorWithCalibratedRed:0.012 green:0.008 blue:0.035 alpha:1] setFill];
    NSRectFill(bounds);
    for (i = 0; i < 220; i++) {
        unsigned h = (unsigned)i * 374761393u + 668265263u;
        h ^= h >> 17;
        CGFloat x = (CGFloat)(h % 4096) / 4096.0 * bounds.size.width;
        CGFloat y = (CGFloat)((h >> 12) % 4096) / 4096.0 * bounds.size.height;
        CGFloat s = 0.6 + (CGFloat)((h >> 10) & 7u) * 0.12;
        [[NSColor colorWithCalibratedWhite:0.55 + (CGFloat)((h >> 6) & 7u) * 0.06
                                     alpha:1] setFill];
        NSRectFill(NSMakeRect(x, y, s, s));
    }
}

/* --- primitives Loam draws a frame with (`maya.engine_present`) --------- */

/* The map's background: near-black space and a fixed star field. */
void maya_mac_stars(void) {
    draw_stars(g_bounds);
}

/* The 3D view's background before the first traced frame. */
void maya_mac_clear(void) {
    [[NSColor colorWithCalibratedRed:0.012 green:0.008 blue:0.035 alpha:1] setFill];
    NSRectFill(g_bounds);
}

/* An orbit ring of radius `r` around (cx, cy). */
void maya_mac_ring(int32_t cx, int32_t cy, int32_t r) {
    stroke_oval((CGFloat)cx, (CGFloat)cy, (CGFloat)r, rgb_a(107, 117, 158, 0.55), 1.15);
}

/* A body's disc in 0xRRGGBB; `glow` adds the sun's halo under it. */
void maya_mac_disc(int32_t x, int32_t y, int32_t r, int32_t rgb, int32_t glow) {
    uint8_t cr = (uint8_t)((rgb >> 16) & 255);
    uint8_t cg = (uint8_t)((rgb >> 8) & 255);
    uint8_t cb = (uint8_t)(rgb & 255);
    if (glow) fill_oval((CGFloat)x, (CGFloat)y, (CGFloat)r * 2.4f, rgb_a(cr, cg, cb, 0.20));
    fill_oval((CGFloat)x, (CGFloat)y, (CGFloat)r, rgb_a(cr, cg, cb, 1.0));
}

/* A body's name, right of its disc of radius `r` at (x, y). */
void maya_mac_label(const char *text, int32_t x, int32_t y, int32_t r) {
    NSString *str = ys(text);
    NSSize sz;
    if (str.length == 0) return;
    sz = [str sizeWithAttributes:name_attrs()];
    [str drawAtPoint:NSMakePoint((CGFloat)x + (CGFloat)r + 5.0, (CGFloat)y - sz.height * 0.55)
      withAttributes:name_attrs()];
}

/* The HUD line, top left. */
void maya_mac_hud(const char *text) {
    NSString *str = ys(text);
    if (str.length == 0) return;
    [str drawAtPoint:NSMakePoint(14.0, 12.0) withAttributes:hud_attrs()];
}

/* The traced frame, `w` x `h` RGBA bytes, scaled to the view. */
void maya_mac_blit(const uint8_t *rgba, int32_t w, int32_t h) {
    CGColorSpaceRef cs;
    CGDataProviderRef prov;
    CGImageRef img;
    NSRect bounds = g_bounds;
    if (!rgba || w < 1 || h < 1) return;
    cs = CGColorSpaceCreateDeviceRGB();
    prov = CGDataProviderCreateWithData(NULL, (const void *)rgba, (size_t)w * (size_t)h * 4, NULL);
    img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                        kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big,
                        prov, NULL, false, kCGRenderingIntentDefault);
    if (img) {
        CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, 0, bounds.size.height);
        CGContextScaleCTM(ctx, 1, -1);
        CGContextDrawImage(ctx, CGRectMake(0, 0, bounds.size.width, bounds.size.height), img);
        CGContextRestoreGState(ctx);
        CGImageRelease(img);
    }
    CGDataProviderRelease(prov);
    CGColorSpaceRelease(cs);
}

static void tick_view(NSView *view) {
    NSRect b = view.bounds;
    int vw, vh;
    double now, dt;
    int64_t dt_ms;

    if (b.size.width < 1 || b.size.height < 1) return;
    vw = (int)b.size.width;
    vh = (int)b.size.height;
    now = CFAbsoluteTimeGetCurrent();
    if (!g_inited_time) {
        g_last = now;
        g_inited_time = 1;
    }
    dt = now - g_last;
    g_last = now;
    if (dt < 0) dt = 0;
    dt_ms = (int64_t)(dt * 1000.0);
    if (dt_ms < 1 && dt > 0) dt_ms = 1;
    loam_maya_engine_note_present(dt_ms);
    if (dt > 0.25) dt = 0.25;
    loam_maya_engine_set_viewport(vw, vh);
    if (loam_maya_engine_is_map()) {
        g_acc += dt;
        while (g_acc >= MAYA_DT) {
            loam_maya_engine_fixed_update(16);
            g_acc -= MAYA_DT;
        }
    } else {
        loam_maya_engine_frame();
    }
}

@interface MayaView : NSView <NSWindowDelegate>
@end
@implementation MayaView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)preservesContentDuringLiveResize { return NO; }

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    @autoreleasepool {
        tick_view(self);
        g_bounds = self.bounds;
        loam_maya_engine_present();
    }
}

- (void)keyDown:(NSEvent *)e {
    NSString *c = [e charactersIgnoringModifiers];
    unichar k = c.length ? [c characterAtIndex:0] : 0;
    if (loam_maya_engine_is_map()) {
        if (k == ' ' || k == '+' || k == '=' || k == '-' || k == '_') {
            loam_maya_engine_input_key((int64_t)k);
            [self setNeedsDisplay:YES];
            return;
        }
    } else {
        if (k == NSLeftArrowFunctionKey) k = 'a';
        if (k == NSRightArrowFunctionKey) k = 'd';
        if (k == NSUpArrowFunctionKey) k = 'w';
        if (k == NSDownArrowFunctionKey) k = 's';
        if (k == 'a' || k == 'A' || k == 'd' || k == 'D' || k == 'w' || k == 'W' ||
            k == 's' || k == 'S' || k == 'q' || k == 'Q' || k == 'e' || k == 'E' ||
            k == 'r' || k == 'R' || k == '+' || k == '=' || k == '-' || k == '_') {
            loam_maya_engine_input_key((int64_t)k);
            [self setNeedsDisplay:YES];
            return;
        }
    }
    [super keyDown:e];
}

- (void)mouseDown:(NSEvent *)e {
    g_mouse = [self convertPoint:e.locationInWindow fromView:nil];
    g_have_mouse = 1;
}

- (void)mouseDragged:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    double dx, dy;
    if (!g_have_mouse) {
        g_mouse = p;
        g_have_mouse = 1;
        return;
    }
    dx = p.x - g_mouse.x;
    dy = p.y - g_mouse.y;
    g_mouse = p;
    if (loam_maya_engine_is_map()) return;
    if (e.modifierFlags & NSEventModifierFlagShift)
        loam_maya_engine_cam_pan((int64_t)dx, (int64_t)(-dy));
    else
        loam_maya_engine_cam_orbit((int64_t)dx, (int64_t)(-dy));
    [self setNeedsDisplay:YES];
}

- (void)rightMouseDragged:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    double dx, dy;
    if (!g_have_mouse) {
        g_mouse = p;
        g_have_mouse = 1;
        return;
    }
    dx = p.x - g_mouse.x;
    dy = p.y - g_mouse.y;
    g_mouse = p;
    if (!loam_maya_engine_is_map()) {
        loam_maya_engine_cam_pan((int64_t)dx, (int64_t)(-dy));
        [self setNeedsDisplay:YES];
    }
}

- (void)rightMouseDown:(NSEvent *)e {
    g_mouse = [self convertPoint:e.locationInWindow fromView:nil];
    g_have_mouse = 1;
}

- (void)scrollWheel:(NSEvent *)e {
    double d = [e scrollingDeltaY];
    if (loam_maya_engine_is_map()) return;
    if (![e hasPreciseScrollingDeltas]) d *= 4.0;
    loam_maya_engine_cam_zoom((int64_t)(d * 100.0));
    [self setNeedsDisplay:YES];
}

- (void)windowWillClose:(NSNotification *)n {
    (void)n;
    [g_timer invalidate];
    g_timer = nil;
    [NSApp stop:nil];
}
@end

void maya_mac_window(const char *title, int32_t w, int32_t h) {
    NSRect rect;
    MayaView *v;
    int wi = (int)w, hi = (int)h;
    if (wi < 64) wi = 64;
    if (hi < 64) hi = 64;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    rect = NSMakeRect(60, 60, (CGFloat)wi, (CGFloat)hi);
    g_win = [[NSWindow alloc]
        initWithContentRect:rect
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [g_win setTitle:title && title[0] ? [NSString stringWithUTF8String:title] : @"Maya"];
    v = [[MayaView alloc] initWithFrame:rect];
    v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    g_view = v;
    g_win.contentView = v;
    g_win.delegate = v;
    [g_win makeKeyAndOrderFront:nil];
    [g_win makeFirstResponder:v];
    [NSApp activateIgnoringOtherApps:YES];
    loam_maya_engine_set_viewport(wi, hi);
    g_timer = [NSTimer scheduledTimerWithTimeInterval:MAYA_DT
                                              repeats:YES
                                                block:^(NSTimer *t) {
                                                    (void)t;
                                                    [g_view setNeedsDisplay:YES];
                                                }];
    [[NSRunLoop mainRunLoop] addTimer:g_timer forMode:NSRunLoopCommonModes];
}

void maya_mac_run(void) {
    [NSApp run];
}
