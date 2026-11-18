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
    CGRect b = self.bounds;
    UIEdgeInsets in = self.safeAreaInsets;
    (void)dirty;
    loam_zeus_engine_ios_frame((int32_t)b.size.width, (int32_t)b.size.height,
                               (int32_t)in.top, (int32_t)in.right,
                               (int32_t)in.bottom, (int32_t)in.left,
                               (int32_t)(link.duration * 1000.0 + 0.5),
                               UIAccessibilityIsReduceMotionEnabled() ? 1 : 0);
}

- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event {
    UITouch *t = [touches anyObject];
    CGPoint p;
    int dirty;
    (void)event;
    if (!t) return;
    p = [t locationInView:self];
    dirty = loam_zeus_engine_touch_began((int32_t)(p.x * 256.0), (int32_t)(p.y * 256.0));
    zeus_sync_keyboard(self);
    if (dirty) [self setNeedsDisplay];
}

- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event {
    UITouch *t = [touches anyObject];
    CGPoint p;
    (void)event;
    if (!t) return;
    p = [t locationInView:self];
    if (loam_zeus_engine_touch_moved((int32_t)(p.x * 256.0), (int32_t)(p.y * 256.0)))
        [self setNeedsDisplay];
}

- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)touches;
    (void)event;
    loam_zeus_engine_touch_ended();
    [self setNeedsDisplay];
}

- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event {
    [self touchesEnded:touches withEvent:event];
}

- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (BOOL)hasText {
    return loam_zeus_engine_focus_captures_text() ? YES : NO;
}

- (void)insertText:(NSString *)text {
    const char *u = [text UTF8String];
    if (u && loam_zeus_engine_ios_insert(za_str(u))) [self setNeedsDisplay];
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
    zeus_bind_draw(za_draw_table());
    zeus_set_platform(ios_run, za_measure, za_redraw);
    zeus_set_pick_image(ios_pick_image);
    zeus_set_image_size(za_image_size);
    zeus_set_font_hooks(za_load_font, za_set_family);
    {
        ZeusMedia media = {0};
        za_media_table(&media);
        media.pick = ios_media_pick;
        media.fullscreen = ios_fullscreen;
        zeus_set_media(media);
    }
}
