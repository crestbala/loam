/* maya_mac.m — Cocoa 2D present for Maya (no Metal).
 *
 * Host only: window, input, vsync. Scene/tracer/map are Yuga engine_*.
 * 2D map draws discs and orbit rings. 3D blits the CPU RGBA buffer.
 */
#import <Cocoa/Cocoa.h>
#include "maya_rt.h"
#include <string.h>
#include <stdlib.h>

#define MAYA_DT (1.0 / 60.0)

static NSWindow *g_win;
static NSView *g_view;
static NSTimer *g_timer;
static double g_acc, g_last;
static int g_inited_time;
static NSPoint g_mouse;
static int g_have_mouse;

static NSString *ys(yuga_str s) {
    if (!s.ptr || s.len <= 0) return @"";
    return [[NSString alloc] initWithBytes:s.ptr length:(NSUInteger)s.len
                                  encoding:NSUTF8StringEncoding];
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

static void draw_hud(void) {
    yuga_str hud = yuga_maya_engine_hud();
    if (hud.len > 0 && hud.ptr) {
        NSString *str = ys(hud);
        [str drawAtPoint:NSMakePoint(14.0, 12.0) withAttributes:hud_attrs()];
    }
}

static void draw_map(NSRect bounds) {
    int i, n;
    CGFloat cx = 0, cy = 0;

    draw_stars(bounds);
    n = (int)yuga_maya_engine_sprite_count();
    for (i = 0; i < n; i++) {
        if (yuga_maya_engine_sprite_is_sun(i)) {
            cx = (CGFloat)yuga_maya_engine_sprite_x(i);
            cy = (CGFloat)yuga_maya_engine_sprite_y(i);
            break;
        }
    }
    for (i = 0; i < n; i++) {
        CGFloat orbit = (CGFloat)yuga_maya_engine_sprite_orbit_r(i);
        if (orbit > 1.0)
            stroke_oval(cx, cy, orbit, rgb_a(107, 117, 158, 0.55), 1.15);
    }
    for (i = 0; i < n; i++) {
        int64_t rgb = yuga_maya_engine_sprite_rgb(i);
        uint8_t cr = (uint8_t)((rgb / 65536) % 256);
        uint8_t cg = (uint8_t)((rgb / 256) % 256);
        uint8_t cb = (uint8_t)(rgb % 256);
        CGFloat x = (CGFloat)yuga_maya_engine_sprite_x(i);
        CGFloat y = (CGFloat)yuga_maya_engine_sprite_y(i);
        CGFloat r = (CGFloat)yuga_maya_engine_sprite_r(i);
        NSColor *c = rgb_a(cr, cg, cb, 1.0);
        if (yuga_maya_engine_sprite_is_sun(i))
            fill_oval(x, y, r * 2.4f, rgb_a(cr, cg, cb, 0.20));
        fill_oval(x, y, r, c);
        yuga_str nm = yuga_maya_engine_sprite_name(i);
        if (nm.len > 0 && nm.ptr) {
            NSString *str = ys(nm);
            NSSize sz = [str sizeWithAttributes:name_attrs()];
            [str drawAtPoint:NSMakePoint(x + r + 5.0, y - sz.height * 0.55)
              withAttributes:name_attrs()];
        }
    }
    draw_hud();
}

static void draw_cpu(NSRect bounds) {
    int w = 0, h = 0;
    const uint8_t *fb = maya_cpu_fb(&w, &h);
    CGColorSpaceRef cs;
    CGDataProviderRef prov;
    CGImageRef img;

    if (!fb || w < 1 || h < 1) {
        [[NSColor colorWithCalibratedRed:0.012 green:0.008 blue:0.035 alpha:1] setFill];
        NSRectFill(bounds);
        return;
    }
    cs = CGColorSpaceCreateDeviceRGB();
    prov = CGDataProviderCreateWithData(NULL, (const void *)fb,
                                        (size_t)w * (size_t)h * 4, NULL);
    img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                        kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big,
                        prov, NULL, false, kCGRenderingIntentDefault);
    if (img) {
        CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, 0, bounds.size.height);
        CGContextScaleCTM(ctx, 1, -1);
        CGContextDrawImage(ctx, CGRectMake(0, 0, bounds.size.width, bounds.size.height),
                           img);
        CGContextRestoreGState(ctx);
        CGImageRelease(img);
    }
    CGDataProviderRelease(prov);
    CGColorSpaceRelease(cs);
    draw_hud();
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
    yuga_maya_engine_note_present(dt_ms);
    if (dt > 0.25) dt = 0.25;
    yuga_maya_engine_set_viewport(vw, vh);
    if (yuga_maya_engine_is_map()) {
        g_acc += dt;
        while (g_acc >= MAYA_DT) {
            yuga_maya_engine_fixed_update(16);
            g_acc -= MAYA_DT;
        }
    } else {
        yuga_maya_engine_frame();
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
        if (yuga_maya_engine_is_map())
            draw_map(self.bounds);
        else
            draw_cpu(self.bounds);
    }
}

- (void)keyDown:(NSEvent *)e {
    NSString *c = [e charactersIgnoringModifiers];
    unichar k = c.length ? [c characterAtIndex:0] : 0;
    if (yuga_maya_engine_is_map()) {
        if (k == ' ' || k == '+' || k == '=' || k == '-' || k == '_') {
            yuga_maya_engine_input_key((int64_t)k);
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
            yuga_maya_engine_input_key((int64_t)k);
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
    if (yuga_maya_engine_is_map()) return;
    if (e.modifierFlags & NSEventModifierFlagShift)
        yuga_maya_engine_cam_pan((int64_t)dx, (int64_t)(-dy));
    else
        yuga_maya_engine_cam_orbit((int64_t)dx, (int64_t)(-dy));
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
    if (!yuga_maya_engine_is_map()) {
        yuga_maya_engine_cam_pan((int64_t)dx, (int64_t)(-dy));
        [self setNeedsDisplay:YES];
    }
}

- (void)rightMouseDown:(NSEvent *)e {
    g_mouse = [self convertPoint:e.locationInWindow fromView:nil];
    g_have_mouse = 1;
}

- (void)scrollWheel:(NSEvent *)e {
    double d = [e scrollingDeltaY];
    if (yuga_maya_engine_is_map()) return;
    if (![e hasPreciseScrollingDeltas]) d *= 4.0;
    yuga_maya_engine_cam_zoom((int64_t)(d * 100.0));
    [self setNeedsDisplay:YES];
}

- (void)windowWillClose:(NSNotification *)n {
    (void)n;
    [g_timer invalidate];
    g_timer = nil;
    [NSApp stop:nil];
}
@end

void yuga_maya_plat_window(yuga_str title, int64_t w, int64_t h) {
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
    [g_win setTitle:title.len > 0 ? ys(title) : @"Maya"];
    v = [[MayaView alloc] initWithFrame:rect];
    v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    g_view = v;
    g_win.contentView = v;
    g_win.delegate = v;
    [g_win makeKeyAndOrderFront:nil];
    [g_win makeFirstResponder:v];
    [NSApp activateIgnoringOtherApps:YES];
    yuga_maya_engine_set_viewport(wi, hi);
    g_timer = [NSTimer scheduledTimerWithTimeInterval:MAYA_DT
                                              repeats:YES
                                                block:^(NSTimer *t) {
                                                    (void)t;
                                                    [g_view setNeedsDisplay:YES];
                                                }];
    [[NSRunLoop mainRunLoop] addTimer:g_timer forMode:NSRunLoopCommonModes];
}

void yuga_maya_plat_run(void) {
    [NSApp run];
}
