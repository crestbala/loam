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

void loam_maya_engine_set_viewport(int32_t w, int32_t h);
void loam_maya_engine_draw(int32_t vw, int32_t vh);
int32_t loam_maya_engine_key_down(int32_t key);
void loam_maya_engine_mouse_down(int32_t x, int32_t y);
int32_t loam_maya_engine_mouse_drag(int32_t x, int32_t y, int32_t pan);
int32_t loam_maya_engine_wheel(int32_t dy100, int32_t precise);

#define MAYA_DT (1.0 / 60.0)

static NSWindow *g_win;
static NSView *g_view;
static NSTimer *g_timer;
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

static NSDictionary *text_attrs(CGFloat sz, uint8_t r, uint8_t g, uint8_t b) {
    return @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:sz weight:NSFontWeightMedium],
        NSForegroundColorAttributeName: rgb_a(r, g, b, 1.0)
    };
}

static void fill_oval(CGFloat cx, CGFloat cy, CGFloat rad, NSColor *c) {
    if (rad < 0.5) rad = 0.5;
    [c setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - rad, cy - rad,
                                                       rad * 2.0, rad * 2.0)] fill];
}

void maya_mac_star(int32_t x16, int32_t y16, int32_t s100, int32_t w100) {
    CGFloat s = (CGFloat)s100 / 100.0;
    [[NSColor colorWithCalibratedWhite:(CGFloat)w100 / 100.0 alpha:1.0] setFill];
    NSRectFill(NSMakeRect((CGFloat)x16 / 16.0, (CGFloat)y16 / 16.0, s, s));
}

void maya_mac_clear(void) {
    [[NSColor colorWithCalibratedRed:0.012 green:0.008 blue:0.035 alpha:1] setFill];
    NSRectFill(g_bounds);
}

void maya_mac_ring(int32_t cx, int32_t cy, int32_t r) {
    CGFloat rad = (CGFloat)r;
    if (rad < 0.5) return;
    NSBezierPath *p =
        [NSBezierPath bezierPathWithOvalInRect:NSMakeRect((CGFloat)cx - rad, (CGFloat)cy - rad,
                                                          rad * 2.0, rad * 2.0)];
    [rgb_a(107, 117, 158, 0.55) setStroke];
    p.lineWidth = 1.15;
    [p stroke];
}

void maya_mac_disc(int32_t x, int32_t y, int32_t r, int32_t rgb, int32_t glow) {
    uint8_t cr = (uint8_t)((rgb >> 16) & 255);
    uint8_t cg = (uint8_t)((rgb >> 8) & 255);
    uint8_t cb = (uint8_t)(rgb & 255);
    if (glow) fill_oval((CGFloat)x, (CGFloat)y, (CGFloat)r * 2.4f, rgb_a(cr, cg, cb, 0.20));
    fill_oval((CGFloat)x, (CGFloat)y, (CGFloat)r, rgb_a(cr, cg, cb, 1.0));
}

void maya_mac_label(const char *text, int32_t x, int32_t y, int32_t r) {
    NSString *str = ys(text);
    NSDictionary *a;
    NSSize sz;
    if (str.length == 0) return;
    a = text_attrs(12.0, 230, 232, 242);
    sz = [str sizeWithAttributes:a];
    [str drawAtPoint:NSMakePoint((CGFloat)x + (CGFloat)r + 5.0, (CGFloat)y - sz.height * 0.55)
      withAttributes:a];
}

void maya_mac_hud(const char *text) {
    NSString *str = ys(text);
    if (str.length == 0) return;
    [str drawAtPoint:NSMakePoint(14.0, 12.0) withAttributes:text_attrs(13.0, 210, 214, 235)];
}

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
        g_bounds = self.bounds;
        loam_maya_engine_draw((int32_t)g_bounds.size.width, (int32_t)g_bounds.size.height);
    }
}

- (void)keyDown:(NSEvent *)e {
    NSString *c = [e charactersIgnoringModifiers];
    unichar k = c.length ? [c characterAtIndex:0] : 0;
    if (loam_maya_engine_key_down((int32_t)k)) [self setNeedsDisplay:YES];
    else [super keyDown:e];
}

- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    loam_maya_engine_mouse_down((int32_t)p.x, (int32_t)p.y);
}

- (void)rightMouseDown:(NSEvent *)e {
    [self mouseDown:e];
}

- (void)mouseDragged:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int pan = (e.modifierFlags & NSEventModifierFlagShift) ? 1 : 0;
    if (loam_maya_engine_mouse_drag((int32_t)p.x, (int32_t)p.y, pan)) [self setNeedsDisplay:YES];
}

- (void)rightMouseDragged:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (loam_maya_engine_mouse_drag((int32_t)p.x, (int32_t)p.y, 1)) [self setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent *)e {
    if (loam_maya_engine_wheel((int32_t)([e scrollingDeltaY] * 100.0),
                               [e hasPreciseScrollingDeltas] ? 1 : 0))
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
    int wi = w < 64 ? 64 : (int)w, hi = h < 64 ? 64 : (int)h;
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
