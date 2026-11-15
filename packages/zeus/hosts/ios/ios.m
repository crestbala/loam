/* ios.m — UIKit window + Core Graphics replay of the Zeus draw list (zeus/ios).
 *
 * UIKit is only the host: UIWindow / UIView / touches. Buttons, labels,
 * navigation bars, Dynamic Type, and semantic colors are not used. Theme,
 * layout, and paint stay in Loam — the same fill / text / clip / path list
 * as Cocoa and Canvas2D.
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#include "zeus_apple.h"
#include <string.h>
#include <stdlib.h>

int loam_app_main(void);

static UIView *g_view;
static UIWindow *g_window;
static id g_del;

/* The shared Apple layer (../apple/zeus_apple.h) paints, measures, decodes
   images and plays media; this file supplies its two hooks and the rest:
   the window, the view, touches, the keyboard and the pickers. */
CGContextRef za_cg(void) {
    return UIGraphicsGetCurrentContext();
}

void za_redraw(void) {
    [g_view setNeedsDisplay];
}

/* The picker: Files (which also reaches iCloud Drive and Downloads). A pick
   is copied into the app's tmp inbox, so the path stays readable. */
@interface ZeusMediaPicker : NSObject <UIDocumentPickerDelegate>
@end
@implementation ZeusMediaPicker
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *u = [urls firstObject];
    (void)c;
    if (!u || ![u path]) return;
    loam_zeus_engine_media_picked(za_str([[[u path] lastPathComponent] UTF8String]),
                                  za_str([[u path] UTF8String]));
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c {
    (void)c;
}
@end
static ZeusMediaPicker *g_media_picker;

static void ios_media_pick(void) {
    UIViewController *vc = g_window ? g_window.rootViewController : nil;
    UIDocumentPickerViewController *p;
    if (!vc) return;
    if (!g_media_picker) g_media_picker = [[ZeusMediaPicker alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    p = [[UIDocumentPickerViewController alloc]
        initWithDocumentTypes:@[ @"public.movie", @"public.audio" ]
                       inMode:UIDocumentPickerModeImport];
#pragma clang diagnostic pop
    p.delegate = g_media_picker;
    [vc presentViewController:p animated:YES completion:nil];
    [p release];
}

/* An iOS app is always full screen. */
static int64_t ios_fullscreen(int64_t on) {
    (void)on;
    return 1;
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
   in hosts/web/loader.js): focus it while `loam_zeus_engine_focus_captures_text()` and blur
   it otherwise. Call after anything that can move focus — today, a touch. */
static void zeus_sync_keyboard(UIView *v) {
    if (loam_zeus_engine_focus_captures_text()) {
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
    CGPoint start;
    CGFloat rem_x, rem_y;
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
    double dt;
    ZeusDraw d;
    (void)dirty;
    b = zeus_content_rect(self);
    {
        UIEdgeInsets in = self.safeAreaInsets;
        zeus_set_insets((int64_t)in.top, (int64_t)in.right,
                        (int64_t)in.bottom, (int64_t)in.left);
    }
    {
        /* Report the OS reduced-motion preference once (§3.2). */
        static int rm_done = 0;
        if (!rm_done) {
            rm_done = 1;
            loam_zeus_engine_set_reduced_motion(UIAccessibilityIsReduceMotionEnabled() ? 1 : 0);
        }
    }
    dt = link.duration;
    if (dt <= 0.0 || dt > 0.05) dt = 1.0 / 60.0;
    loam_zeus_engine_layout((int32_t)b.size.width, (int32_t)b.size.height);
    (void)loam_zeus_engine_step_dt((int32_t)(dt * 1000.0 + 0.5));
    d = za_draw_table();
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
    start = p;
    rem_x = 0;
    rem_y = 0;
    tracking = 1;
    scrolling = 0;
    /* A finger has no hover, so the touch *is* the pointer: report the position
       before the press. Without this a tap only ever produces a click, and
       hover-driven widgets (chart tooltips, hover states) never appear on iOS —
       while the same tap works on Cocoa and Canvas2D, where a mouse move has
       already hovered that point. Left in place on `touchesEnded`, so a tap
       keeps its tooltip rather than clearing it when the finger lifts. */
    dirty = loam_zeus_engine_hover((int32_t)p.x, (int32_t)p.y);
    if (loam_zeus_engine_click((int32_t)p.x, (int32_t)p.y)) dirty = 1;
    /* The press may have moved focus into or out of a text field. */
    zeus_sync_keyboard(self);
    if (dirty) [self setNeedsDisplay];
}

- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event {
    UITouch *t = [touches anyObject];
    CGPoint p;
    CGFloat fx, fy;
    int64_t dx, dy;
    int dirty;
    (void)event;
    if (!t || !tracking) return;
    p = zeus_content_point(self, [t locationInView:self]);
    /* The engine takes whole points. Carry the fraction, or a slow drag loses
       a piece of every move and the content falls behind the finger. */
    fx = last.x - p.x + rem_x;
    fy = last.y - p.y + rem_y;
    dx = (int64_t)fx;
    dy = (int64_t)fy;
    rem_x = fx - (CGFloat)dx;
    rem_y = fy - (CGFloat)dy;
    dirty = loam_zeus_engine_drag((int32_t)p.x, (int32_t)p.y);
    dirty |= loam_zeus_engine_hover((int32_t)p.x, (int32_t)p.y);
    /* Measured from the press: moves are small, so a slow start judged one
       move at a time would never become a scroll. */
    if (!scrolling) {
        CGFloat sx = start.x - p.x, sy = start.y - p.y;
        if (sx * sx + sy * sy > 64) scrolling = 1;
    }
    if (scrolling && (dx || dy))
        dirty |= loam_zeus_engine_scroll((int32_t)p.x, (int32_t)p.y, (int32_t)dx, (int32_t)dy);
    last = p;
    if (dirty) [self setNeedsDisplay];
}

- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)touches;
    (void)event;
    tracking = 0;
    scrolling = 0;
    loam_zeus_engine_mouseup();
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
    return loam_zeus_engine_focus_captures_text() ? YES : NO;
}

- (void)insertText:(NSString *)text {
    const char *u;
    if ([text isEqualToString:@"\n"]) {
        if (loam_zeus_engine_key_ev(13, 0)) [self setNeedsDisplay];
        return;
    }
    u = [text UTF8String];
    if (u && loam_zeus_engine_insert(za_str(u))) [self setNeedsDisplay];
}

- (void)deleteBackward {
    if (loam_zeus_engine_key_ev(8, 0)) [self setNeedsDisplay];
}

/* IME composition, kept separate from committed text exactly as mac.m does with
   setMarkedText / unmarkText. Implementing these without adopting the whole
   `UITextInput` protocol is enough for UIKit to use them when present. */
- (void)setMarkedText:(NSString *)markedText selectedRange:(NSRange)selectedRange {
    const char *u;
    (void)selectedRange;
    u = [markedText UTF8String];
    if (u && loam_zeus_engine_marked(za_str(u))) [self setNeedsDisplay];
}

- (void)unmarkText {
    if (loam_zeus_engine_marked(za_str(""))) [self setNeedsDisplay];
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
    loam_zeus_engine_picked_image(za_str([path UTF8String]), (int32_t)(img.size.width + 0.5),
                                  (int32_t)(img.size.height + 0.5));
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
    zeus_set_platform(ios_run, za_measure, ios_redraw);
    zeus_set_pick_image(ios_pick_image);
    zeus_set_image_size(za_image_size);
    zeus_set_font_hooks(za_load_font, NULL);
    {
        ZeusMedia media;
        memset(&media, 0, sizeof media);
        za_media_table(&media);
        media.pick = ios_media_pick;
        media.fullscreen = ios_fullscreen;
        zeus_set_media(media);
    }
}
