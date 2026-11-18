/* mac.m — Cocoa window + Core Text paint for Zeus (zeus/desktop). */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <SoundAnalysis/SoundAnalysis.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#include "zeus_apple.h"
/* The key codes and modifier bits Zeus expects (zeuscore/keymap.loam). */
#define ZEUS_MOD_SHIFT 1
#define ZEUS_MOD_CTRL 2
#define ZEUS_MOD_ALT 4
#define ZEUS_MOD_CMD 8
#define ZEUS_K_TAB 9
#include <string.h>
#include <stdlib.h>
#include <malloc/malloc.h>

static NSView *g_view;
static NSWindow *g_win;

CGContextRef za_cg(void) {
    return [[NSGraphicsContext currentContext] CGContext];
}

void za_redraw(void) {
    if (g_view) [g_view setNeedsDisplay:YES];
}

static void mac_mem_debug(void) {
    static int frames;
    malloc_statistics_t ms;
    if (!loam_platform_host_env_flag(2) || (frames++ % 60) != 0) return;
    malloc_zone_statistics(malloc_default_zone(), &ms);
    fprintf(stderr, "[mem] malloc=%uKB blocks=%u phys=%dKB win=%lldx%lld view=%.0fx%.0f\n",
            (unsigned)(ms.size_in_use / 1024), (unsigned)ms.blocks_in_use,
            (int)loam_platform_plat_mem_kb(),
            (long long)zeus_window_width(), (long long)zeus_window_height(),
            g_view ? g_view.bounds.size.width : 0.0, g_view ? g_view.bounds.size.height : 0.0);
}

static void mac_media_pick(void) {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    NSString *path;
    [p setCanChooseFiles:YES];
    [p setCanChooseDirectories:NO];
    [p setAllowsMultipleSelection:NO];
    [p setAllowedFileTypes:@[ @"mp4", @"m4v", @"mov", @"mkv", @"webm", @"avi", @"wmv", @"flv",
                              @"ogv", @"ogg", @"ts", @"mts", @"m2ts", @"3gp", @"mpg", @"mpeg",
                              @"m3u8", @"mp3", @"m4a", @"aac", @"wav", @"flac", @"opus" ]];
    if ([p runModal] != NSModalResponseOK) return;
    path = [[p URL] path];
    if (!path) return;
    loam_zeus_engine_media_picked(za_str([[path lastPathComponent] UTF8String]), za_str([path UTF8String]));
}

/* Mune's recordings panel. Separate from `mac_media_pick`, which stays a
   single video-or-audio file for Vidy. Several audio files publish through
   the same queue a drop uses. */
static void mac_audio_pick(void) {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    NSArray *urls;
    NSUInteger i, n;
    [p setCanChooseFiles:YES];
    [p setCanChooseDirectories:NO];
    [p setAllowsMultipleSelection:YES];
    [p setMessage:@"Select the recordings to build a new piece from."];
    [p setPrompt:@"Add"];
    [p setAllowedFileTypes:@[ @"public.audio",
                              @"mp3", @"m4a", @"aac", @"wav", @"aiff", @"aif", @"aifc",
                              @"flac", @"caf", @"opus", @"ogg", @"alac", @"m4b" ]];
    if ([p runModal] != NSModalResponseOK) return;
    urls = [p URLs];
    n = [urls count];
    for (i = 0; i < n; i++) {
        NSString *path = [[urls objectAtIndex:i] path];
        if (!path) continue;
        loam_zeus_engine_media_picked(za_str([[path lastPathComponent] UTF8String]), za_str([path UTF8String]));
    }
}

/* Voice tools for audio apps (Mune). Both run on one serial background queue
   and never touch the UI. Each writes its result, then `<out>.done`, or the
   reason to `<out>.fail`. The app polls for the marker.

   ZEUS_VOICE_MAP: Apple's sound classifier over 1.5 s windows (hop 0.75 s).
   `out` is text, one line per window: `start_ms end_ms voice` where voice is
   the highest confidence (0..100) of any singing, speech, or other voice
   label.

   ZEUS_VOICE_SPLIT: AUSoundIsolation isolates the voice, and the file minus
   that voice (latency aligned) is written to `out` as 32-bit float WAV. The
   model masks the spectrum, so the subtraction leaves the music. */
#define ZEUS_VOICE_MAP 1
#define ZEUS_VOICE_SPLIT 2

static NSString *const zeus_voice_labels[] = {
    @"speech", @"singing", @"choir_singing", @"humming", @"rapping", @"yodeling",
    @"whispering", @"shout", @"screaming", @"children_shouting", @"battle_cry",
    @"laughter", @"belly_laugh", @"crying_sobbing", @"babble", @"crowd", @"cheering",
};

static dispatch_queue_t zeus_voice_queue(void) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        q = dispatch_queue_create("zeus.voice", DISPATCH_QUEUE_SERIAL);
    });
    return q;
}

static void zeus_voice_mark(NSString *out, NSString *why) {
    if (why) {
        [why writeToFile:[out stringByAppendingString:@".fail"] atomically:YES
                encoding:NSUTF8StringEncoding error:NULL];
        return;
    }
    [@"" writeToFile:[out stringByAppendingString:@".done"] atomically:YES
            encoding:NSUTF8StringEncoding error:NULL];
}

API_AVAILABLE(macos(12.0))
@interface ZeusVoiceObserver : NSObject <SNResultsObserving> {
@public
    NSMutableString *rows;
    NSString *failure;
}
@end

@implementation ZeusVoiceObserver
- (instancetype)init {
    if ((self = [super init])) rows = [[NSMutableString alloc] init];
    return self;
}
- (void)request:(id<SNRequest>)request didProduceResult:(id<SNResult>)result {
    SNClassificationResult *r = (SNClassificationResult *)result;
    double voice = 0;
    size_t i;
    CMTimeRange span;
    (void)request;
    if (![r isKindOfClass:[SNClassificationResult class]]) return;
    for (i = 0; i < sizeof zeus_voice_labels / sizeof zeus_voice_labels[0]; i++) {
        SNClassification *c = [r classificationForIdentifier:zeus_voice_labels[i]];
        if (c && c.confidence > voice) voice = c.confidence;
    }
    span = r.timeRange;
    [rows appendFormat:@"%lld %lld %d\n", (long long)llround(CMTimeGetSeconds(span.start) * 1000.0),
                       (long long)llround(CMTimeGetSeconds(CMTimeRangeGetEnd(span)) * 1000.0),
                       (int)llround(voice * 100.0)];
}
- (void)request:(id<SNRequest>)request didFailWithError:(NSError *)error {
    (void)request;
    [failure release];
    failure = [[error localizedDescription] copy];
}
- (void)requestDidComplete:(id<SNRequest>)request {
    (void)request;
}
- (void)dealloc {
    [rows release];
    [failure release];
    [super dealloc];
}
@end

static NSString *zeus_voice_map_run(NSString *src, NSString *out) API_AVAILABLE(macos(12.0)) {
    NSError *err = nil;
    SNClassifySoundRequest *req;
    SNAudioFileAnalyzer *an;
    ZeusVoiceObserver *obs;
    NSString *why = nil;
    req = [[SNClassifySoundRequest alloc] initWithClassifierIdentifier:SNClassifierIdentifierVersion1
                                                                 error:&err];
    if (!req) return [err localizedDescription] ?: @"The sound classifier is not available.";
    req.windowDuration = CMTimeMakeWithSeconds(1.5, 48000);
    req.overlapFactor = 0.5;
    an = [[SNAudioFileAnalyzer alloc] initWithURL:[NSURL fileURLWithPath:src] error:&err];
    if (!an) {
        [req release];
        return [err localizedDescription] ?: @"Could not open the recording.";
    }
    obs = [[ZeusVoiceObserver alloc] init];
    if (![an addRequest:req withObserver:obs error:&err]) {
        why = [err localizedDescription] ?: @"Could not start the classifier.";
    } else {
        [an analyze];
        if (obs->failure) why = [[obs->failure copy] autorelease];
        else if (![obs->rows writeToFile:out atomically:YES encoding:NSUTF8StringEncoding error:&err])
            why = [err localizedDescription] ?: @"Could not write the voice map.";
    }
    [obs release];
    [an release];
    [req release];
    return why;
}

/* Pass 1 renders the isolated voice to `<out>.voice.caf`; pass 2 subtracts it
   from the source, `latency` frames later, into `out`. */
static NSString *zeus_voice_split_run(NSString *src, NSString *out) API_AVAILABLE(macos(13.0)) {
    AudioComponentDescription d = {kAudioUnitType_Effect, kAudioUnitSubType_AUSoundIsolation,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    __block AVAudioUnit *unit = nil;
    __block NSString *why = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSString *voice_path = [out stringByAppendingString:@".voice.caf"];
    NSError *err = nil;
    AVAudioFile *in_f = nil, *voice_f = nil, *a = nil, *b = nil, *music_f = nil;
    AVAudioEngine *eng = nil;
    AVAudioPlayerNode *player = nil;
    AVAudioPCMBuffer *buf = nil, *ba = nil, *bb = nil;
    AVAudioFormat *fmt;
    Float64 lat = 0;
    UInt32 sz = sizeof lat;
    AVAudioFramePosition total, lat_frames;

    [AVAudioUnit instantiateWithComponentDescription:d options:0
                                   completionHandler:^(AVAudioUnit *u, NSError *e) {
        if (u) unit = [u retain];
        else why = [[e localizedDescription] copy];
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    dispatch_release(sem);
    if (!unit) return why ? [why autorelease] : @"Voice isolation is not available.";

    in_f = [[AVAudioFile alloc] initForReading:[NSURL fileURLWithPath:src] error:&err];
    if (!in_f) {
        why = [err localizedDescription] ?: @"Could not open the recording.";
        goto done;
    }
    fmt = in_f.processingFormat;
    AudioUnitSetParameter(unit.audioUnit, kAUSoundIsolationParam_WetDryMixPercent,
                          kAudioUnitScope_Global, 0, 100.0f, 0);
    if (@available(macOS 15.0, *))
        AudioUnitSetParameter(unit.audioUnit, kAUSoundIsolationParam_SoundToIsolate,
                              kAudioUnitScope_Global, 0,
                              (AudioUnitParameterValue)kAUSoundIsolationSoundType_HighQualityVoice, 0);
    AudioUnitGetProperty(unit.audioUnit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &lat, &sz);
    lat_frames = (AVAudioFramePosition)llround(lat * fmt.sampleRate);

    eng = [[AVAudioEngine alloc] init];
    player = [[AVAudioPlayerNode alloc] init];
    [eng attachNode:player];
    [eng attachNode:unit];
    [eng connect:player to:unit format:fmt];
    [eng connect:unit to:eng.mainMixerNode format:fmt];
    if (![eng enableManualRenderingMode:AVAudioEngineManualRenderingModeOffline format:fmt
                      maximumFrameCount:4096 error:&err] ||
        ![eng startAndReturnError:&err]) {
        why = [err localizedDescription] ?: @"Could not start voice isolation.";
        goto done;
    }
    [player scheduleFile:in_f atTime:nil completionHandler:nil];
    [player play];
    {
        /* A file is always interleaved; saying otherwise only logs a warning. */
        NSMutableDictionary *vs = [[fmt.settings mutableCopy] autorelease];
        vs[AVLinearPCMIsNonInterleaved] = @NO;
        voice_f = [[AVAudioFile alloc] initForWriting:[NSURL fileURLWithPath:voice_path] settings:vs
                                         commonFormat:fmt.commonFormat interleaved:fmt.isInterleaved
                                                error:&err];
    }
    if (!voice_f) {
        why = [err localizedDescription] ?: @"Could not write the voice.";
        goto done;
    }
    buf = [[AVAudioPCMBuffer alloc] initWithPCMFormat:eng.manualRenderingFormat
                                        frameCapacity:eng.manualRenderingMaximumFrameCount];
    total = in_f.length + lat_frames;
    while (eng.manualRenderingSampleTime < total) {
        AVAudioFrameCount n = (AVAudioFrameCount)MIN((AVAudioFramePosition)buf.frameCapacity,
                                                     total - eng.manualRenderingSampleTime);
        AVAudioEngineManualRenderingStatus st = [eng renderOffline:n toBuffer:buf error:&err];
        if (st == AVAudioEngineManualRenderingStatusError) {
            why = [err localizedDescription] ?: @"Voice isolation stopped.";
            goto done;
        }
        if (st == AVAudioEngineManualRenderingStatusSuccess && ![voice_f writeFromBuffer:buf error:&err]) {
            why = [err localizedDescription] ?: @"Could not write the voice.";
            goto done;
        }
    }
    [player stop];
    [eng stop];
    [voice_f release];
    voice_f = nil;

    {
        NSDictionary *wav = @{
            AVFormatIDKey : @(kAudioFormatLinearPCM),
            AVSampleRateKey : @(fmt.sampleRate),
            AVNumberOfChannelsKey : @(fmt.channelCount),
            AVLinearPCMBitDepthKey : @32,
            AVLinearPCMIsFloatKey : @YES,
            AVLinearPCMIsBigEndianKey : @NO,
            AVLinearPCMIsNonInterleaved : @NO,
        };
        AVAudioFrameCount cap = 16384;
        AVAudioChannelCount ch, c;
        a = [[AVAudioFile alloc] initForReading:[NSURL fileURLWithPath:src] error:&err];
        b = a ? [[AVAudioFile alloc] initForReading:[NSURL fileURLWithPath:voice_path] error:&err] : nil;
        music_f = b ? [[AVAudioFile alloc] initForWriting:[NSURL fileURLWithPath:out] settings:wav
                                             commonFormat:AVAudioPCMFormatFloat32 interleaved:NO error:&err]
                    : nil;
        if (!music_f) {
            why = [err localizedDescription] ?: @"Could not write the music.";
            goto done;
        }
        b.framePosition = lat_frames;
        ba = [[AVAudioPCMBuffer alloc] initWithPCMFormat:a.processingFormat frameCapacity:cap];
        bb = [[AVAudioPCMBuffer alloc] initWithPCMFormat:b.processingFormat frameCapacity:cap];
        ch = a.processingFormat.channelCount;
        for (;;) {
            AVAudioFrameCount k, n;
            if (![a readIntoBuffer:ba error:&err] || ba.frameLength == 0) break;
            bb.frameLength = 0;
            if (b.framePosition < b.length) [b readIntoBuffer:bb frameCount:ba.frameLength error:NULL];
            n = MIN(ba.frameLength, bb.frameLength);
            for (c = 0; c < ch && c < b.processingFormat.channelCount; c++) {
                float *x = ba.floatChannelData[c];
                const float *v = bb.floatChannelData[c];
                for (k = 0; k < n; k++) x[k] -= v[k];
            }
            if (![music_f writeFromBuffer:ba error:&err]) {
                why = [err localizedDescription] ?: @"Could not write the music.";
                goto done;
            }
        }
    }

done:
    [[NSFileManager defaultManager] removeItemAtPath:voice_path error:NULL];
    [ba release];
    [bb release];
    [a release];
    [b release];
    [music_f release];
    [buf release];
    [voice_f release];
    [player release];
    [eng release];
    [in_f release];
    [unit release];
    return why;
}

static int64_t mac_media_voice(int64_t what, const char *src, const char *out) {
    NSString *s, *o;
    if (!src || !out || !*src || !*out) return 0;
    if (what == ZEUS_VOICE_MAP) {
        if (@available(macOS 12.0, *)) {
        } else {
            return 0;
        }
    } else if (what == ZEUS_VOICE_SPLIT) {
        if (@available(macOS 13.0, *)) {
        } else {
            return 0;
        }
    } else {
        return 0;
    }
    s = [[NSString alloc] initWithUTF8String:src];
    o = [[NSString alloc] initWithUTF8String:out];
    if (!s || !o) {
        [s release];
        [o release];
        return 0;
    }
    [[NSFileManager defaultManager] removeItemAtPath:[o stringByAppendingString:@".done"] error:NULL];
    [[NSFileManager defaultManager] removeItemAtPath:[o stringByAppendingString:@".fail"] error:NULL];
    dispatch_async(zeus_voice_queue(), ^{
        @autoreleasepool {
            NSString *why = nil;
            if (what == ZEUS_VOICE_MAP) {
                if (@available(macOS 12.0, *)) why = zeus_voice_map_run(s, o);
            } else {
                if (@available(macOS 13.0, *)) why = zeus_voice_split_run(s, o);
            }
            zeus_voice_mark(o, why);
        }
        [s release];
        [o release];
    });
    return 1;
}

static int64_t mac_fullscreen(int64_t on) {
    int is;
    if (!g_win) return 0;
    is = ([g_win styleMask] & NSWindowStyleMaskFullScreen) ? 1 : 0;
    if (on >= 0 && (on ? 1 : 0) != is) {
        [g_win toggleFullScreen:nil];
        return on ? 1 : 0;
    }
    return is;
}

static NSCursor *mac_cursor_for_id(int id) {
    switch (id) {
    case 1: return [NSCursor pointingHandCursor];
    case 2: return [NSCursor IBeamCursor];
    case 3: return [NSCursor crosshairCursor];
    case 4: return [NSCursor operationNotAllowedCursor];
    case 5: return [NSCursor openHandCursor];
    case 6: return [NSCursor closedHandCursor];
    case 7: return [NSCursor resizeLeftRightCursor];
    case 8: return [NSCursor resizeUpDownCursor];
    default: return [NSCursor arrowCursor];
    }
}

static NSCursor *mac_cursor_now(void) {
    return mac_cursor_for_id(loam_zeus_engine_cursor_id());
}

@interface ZeusView : NSView <NSTextInputClient> {
    NSTrackingArea *track;
    NSString *marked;
    NSRange markedSel;
}
- (void)zeusRender;
@end

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

     default              the owned IOSurface set below (two surfaces by
     (ZEUS_OWN_SURFACE=1) default, `ZEUS_OWN_SURFACES` 2..4): our memory IS the
                          layer's texture, so a finished frame is never copied,
                          and each frame is drawn into a surface the compositor
                          is not reading. No shared mutable buffer, so no
                          shimmer under fast scroll.
     ZEUS_OWN_SURFACE=0   the owned bitmap: one buffer (~18 MB), wrapped in a
                          fresh CGImage per frame. CoreAnimation materialises
                          each frame into a texture of its own, so the frame
                          carries one full copy — and that one buffer is both
                          our draw target and the layer's contents, which a fast
                          scroll can catch mid-draw. Also the fallback when a
                          surface cannot be allocated.
     APP_KIT=1            AppKit's own store, window pinned to sRGB. `drawRect:`
                          paints straight into the surface the compositor reads,
                          so nothing is ever copied and a screen-filling window
                          holds 60 fps — at the cost of ~54 MB of IOSurface.
     ZEUS_WIDE_GAMUT=1    AppKit's store with the window keeping the display's
                          ICC profile, which doubles each buffer's depth to 8
                          bytes per pixel: ~109 MB for identical pixels.

   `ZEUS_OWN_BUFFER=0` is the same choice as `APP_KIT=1`, spelled as a value.

   Measured, handing the layer the IOSurface itself (rather than a CGImage over
   the bitmap) does remove that copy: `other` fell from 32 ms to 9.5 ms, the frame
   held 60 fps and the footprint dropped to ~35 MB. It was held back because
   CoreAnimation then holds the surface as the layer's texture, so writing the
   frame it is reading flickers; that is fixed by rotating over a few surfaces and
   drawing only into one `IOSurfaceIsInUse` says is free (never
   `kIOSurfaceLockAvoidSync`, which skips exactly that check and leaves the lock
   state inconsistent with a plain unlock).

   Cost: one SCREEN-sized buffer per surface in the set, so ~2x21 MB on a Retina
   laptop display whatever the window's size — the set is sized to the screen,
   not the window, so a live resize never swaps surfaces (which is what blinked
   white; see `OWN_SURFACES_MAX`). `ZEUS_OWN_SURFACES=2` trims a buffer; watch
   the frame trace's `surf_skip`, which counts frames skipped because every
   surface was busy (a stutter, told apart from a slow frame).

   Measured on the gallery (Retina, 2026-10-07): two surfaces with the
   in-frame wait (`OWN_SURF_WAIT_MS`) and malloc relief hold 60 fps at ~60 MB,
   of which 43 MB is the pair. Without the wait two surfaces ran at 30 fps —
   the compositor lets go of the last-shown surface 2–10 ms after the refresh,
   just after the link tick checked it. Three surfaces cost ~81 MB.

   No scale knob: the surface is always the display's own resolution. */
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

static int own_buffer_on(void) { return loam_platform_host_display_path() == 1; }
static int own_surface_on(void) { return loam_platform_host_display_path() == 2; }

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

typedef struct {
    double setup_ms, engine_ms, image_ms, set_ms;
    int more;
} FramePhases;
static double frame_now_ms(void) {
    return CFAbsoluteTimeGetCurrent() * 1000.0;
}

static int own_surf_skips;
static double own_surf_wait_t0;
static double own_surf_last_wait;

static void mac_memory_relief(int more) {
    static int frames;
    static double last;
    double now = frame_now_ms();
    frames++;
    if (frames == 120 || (!more && now - last > 5000.0)) {
        malloc_zone_pressure_relief(NULL, 0);
        last = now;
    }
}

static void frame_trace(double t0, FramePhases p) {
    static double prev;
    static int n;
    double interval = (prev > 0.0) ? (t0 - prev) : 0.0;
    double work = p.setup_ms + p.engine_ms + p.image_ms + p.set_ms;
    prev = t0;
    if (!loam_platform_host_env_flag(0) || (n++ % 30) != 0) return;
    fprintf(stderr,
            "[frame] interval=%.1fms setup=%.2f engine=%.1f image=%.2f set=%.2f other=%.1f "
            "more=%d next=%dms surf_skip=%d surf_wait=%.1f\n",
            interval, p.setup_ms, p.engine_ms, p.image_ms, p.set_ms, interval - work, p.more,
            (int)loam_zeus_engine_next_ms(), own_surf_skips, own_surf_last_wait);
}

static void mac_sync_chrome(void) {
    NSWindow *w = g_win;
    NSRect lr;
    CGFloat h, lead = 0, trail = 0;
    if (!w) return;
    if (zeus_window_chrome() != 1) {
        zeus_set_titlebar_box(0, 0, 0);
        return;
    }
    if (!([w styleMask] & NSWindowStyleMaskFullSizeContentView)) {
        [w setStyleMask:[w styleMask] | NSWindowStyleMaskFullSizeContentView];
        [w setTitlebarAppearsTransparent:YES];
        [w setTitleVisibility:NSWindowTitleHidden];
    }
    if ([w styleMask] & NSWindowStyleMaskFullScreen) {
        zeus_set_titlebar_box(0, 0, 0);
        return;
    }
    lr = [w contentLayoutRect];
    h = [[w contentView] bounds].size.height - lr.size.height;
    {
        NSButton *zoom = [w standardWindowButton:NSWindowZoomButton];
        NSButton *close = [w standardWindowButton:NSWindowCloseButton];
        NSView *last = zoom ? zoom : close;
        if (last && ![last isHidden]) {
            NSRect r = [[w contentView] convertRect:[last bounds] fromView:last];
            lead = NSMaxX(r) + 12.0;
        }
    }
    zeus_set_titlebar_box((int64_t)(h + 0.5), (int64_t)(lead + 0.5), (int64_t)(trail + 0.5));
}

static void mac_snapshot(const char *path) {
    @autoreleasepool {
        int32_t w = 0, h = 0;
        CGFloat scale = 2;
        loam_zeus_engine_snapshot_prepare(&w, &h);
        size_t pw = (size_t)(w * scale), ph = (size_t)(h * scale);
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef bc = CGBitmapContextCreate(NULL, pw, ph, 8, 0, cs,
                                                kCGImageAlphaPremultipliedFirst |
                                                    kCGBitmapByteOrder32Host);
        CGColorSpaceRelease(cs);
        if (!bc) exit(2);
        CGContextSetRGBFillColor(bc, 1, 1, 1, 1);
        CGContextFillRect(bc, CGRectMake(0, 0, pw, ph));
        CGContextTranslateCTM(bc, 0, (CGFloat)ph);
        CGContextScaleCTM(bc, scale, -scale);
        [NSGraphicsContext setCurrentContext:
            [NSGraphicsContext graphicsContextWithCGContext:bc flipped:YES]];
        loam_zeus_engine_snapshot_paint(w, h);
        CGImageRef img = CGBitmapContextCreateImage(bc);
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        int ok = [png writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
        CGImageRelease(img);
        CGContextRelease(bc);
        exit(ok ? 0 : 3);
    }
}

static int mac_frame(int64_t vw, int64_t vh) {
    int rm = 0;
    if (@available(macOS 10.12, *))
        rm = [NSWorkspace sharedWorkspace].accessibilityDisplayShouldReduceMotion ? 1 : 0;
    mac_mem_debug();
    mac_sync_chrome();
    return loam_zeus_engine_mac_frame((int32_t)vw, (int32_t)vh, rm, za_media_live());
}

static CADisplayLink *g_link;
static int g_link_want;

static void mac_link_tick(void) {
    static double prev;
    static int n;
    double now = frame_now_ms();
    if (loam_platform_host_env_flag(0) && prev > 0.0 && (n++ % 30) == 0)
        fprintf(stderr, "[tick] period=%.1fms want=%d\n", now - prev, g_link_want);
    prev = now;
    if (!g_link_want || !g_view) return;
    g_link_want = 0;
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

static void mac_schedule_next(int more) {
    NSView *v = g_view;
    int64_t due;
    if (!v) return;
    if (loam_platform_host_env_flag(1)) more = 1;
    if (more) {
        if (g_link) {
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
    if (g_link) g_link.paused = YES;
}

/* --- owned IOSurface display path -------------------------------------------
 *
 * The bitmap path hands CoreAnimation a CGImage, and CA then builds a texture
 * of its own from it: a second full-size buffer, plus a full-frame copy every
 * frame. Backing the bitmap with an IOSurface and handing the layer the
 * SURFACE removes both — the memory we draw into IS the layer's texture, so a
 * finished frame never moves, and the compositor keeps no store of its own for
 * the view. That is the difference between three full-window buffers and one.
 *
 * One surface is not enough. CoreAnimation holds the surface as the layer's
 * texture while compositing, so writing the surface it is reading is what makes
 * a frame flash or tear — and with a single buffer that is every frame. A small
 * ROTATING SET, picked per frame from the ones the compositor is NOT using, is
 * what makes the read and the write different memory with enough slack that a
 * free surface is available: the last-shown surface is usually still held, the
 * one before it may be, and the next one after that is free. Two by default:
 * when both are held, the frame waits a few milliseconds for the compositor to
 * release one (`OWN_SURF_WAIT_MS`) rather than dropping to the next refresh.
 * `ZEUS_OWN_SURFACES` moves it between 2 and 4 (3 and 4 trade ~21 MB each
 * for never waiting).
 *
 * "Not using" is `IOSurfaceIsInUse`, which answers whether another client (the
 * WindowServer) has the surface — the one non-blocking probe the IOSurface API
 * offers. `kIOSurfaceLockAvoidSync` must NOT be used here: it tells the kernel
 * not to wait for in-flight users, which is exactly the protection a presenting
 * surface needs, and it leaves the surface's lock state inconsistent with a
 * plain unlock — which is what showed as a blinking background before this. The
 * lock that remains is only for the CPU write, uses the same options as its
 * unlock, and contends with nobody because the surface was just found free.
 *
 * `ZEUS_OWN_SURFACE=1` selects it: it is the only path with neither a per-frame
 * copy nor a buffer shared with the compositor, and it is the default;
 * `ZEUS_OWN_SURFACE=0` selects the bitmap, `ZEUS_OWN_BUFFER=1` the same. The pixels are identical to the other paths by
 * construction: `own_draw` runs the same engine the same way, in the same
 * format, at the same physical size — only the memory differs.
 *
 * The set is NOT sized to the window. Handing the layer an IOSurface whose pixel
 * size differs from the one it is showing costs a frame in which the compositor
 * draws the layer with no contents at all — the window's background, white —
 * and a live resize does that on every step: a fast drag blinked white every
 * few frames and could stay white after the drag ended (the bitmap and AppKit
 * paths never did, and fresh surfaces at an unchanged size never did either;
 * only the size change does it). So the set is allocated once at the screen's
 * own backing size, the frame is drawn into its top-left `px_w x px_h`, and
 * `layer.contentsRect` shows exactly that region. A resize then changes a
 * rectangle, never the surface, and the same surfaces serve the whole
 * drag. Reallocation is left for the cases a drag cannot reach: the window
 * landing on a bigger screen, or a scale change. Cost: one screen-sized buffer
 * per surface whatever the window's size, so a smaller window pays for the
 * headroom. */
#define OWN_SURFACES_MAX 4
#define OWN_SURFACES_DEFAULT 2
/* How long a frame may wait for a free surface before it gives up until the
   next refresh: half a 60 Hz frame, so a frame that waits still has the other
   half to draw in. */
#define OWN_SURF_WAIT_MS 8.0
/* 'BGRA' — the same 4-byte premultiplied layout `CGBitmapContextCreate` and the
   layer already use, so nothing converts on the way out. */
#define OWN_SURF_BGRA 0x42475241u

static IOSurfaceRef own_surf[OWN_SURFACES_MAX];
static CGContextRef own_surf_ctx[OWN_SURFACES_MAX];
static NSGraphicsContext *own_surf_gc[OWN_SURFACES_MAX];
static int own_surf_cur;
static int own_surf_w, own_surf_h;

static void own_surf_free(void) {
    int i;
    for (i = 0; i < OWN_SURFACES_MAX; i++) {
        [own_surf_gc[i] release];
        own_surf_gc[i] = nil;
        if (own_surf_ctx[i]) {
            CGContextRelease(own_surf_ctx[i]);
            own_surf_ctx[i] = NULL;
        }
        if (own_surf[i]) {
            CFRelease(own_surf[i]);
            own_surf[i] = NULL;
        }
    }
    own_surf_w = 0;
    own_surf_h = 0;
    own_surf_cur = 0;
}

static int own_surf_ensure(int px_w, int px_h) {
    NSDictionary *props;
    int i, n;
    if (own_surf[0] && own_surf_w == px_w && own_surf_h == px_h) return 1;
    own_surf_free();
    n = loam_platform_host_surface_count();
    props = @{ (id)kIOSurfaceWidth : @(px_w),
               (id)kIOSurfaceHeight : @(px_h),
               (id)kIOSurfaceBytesPerElement : @4,
               (id)kIOSurfaceBytesPerRow : @(px_w * 4),
               (id)kIOSurfacePixelFormat : @(OWN_SURF_BGRA) };
    for (i = 0; i < n; i++) {
        void *base;
        size_t bpr;
        own_surf[i] = IOSurfaceCreate((CFDictionaryRef)props);
        if (!own_surf[i] || IOSurfaceLock(own_surf[i], 0, NULL) != 0) {
            own_surf_free();
            return 0;
        }
        base = IOSurfaceGetBaseAddress(own_surf[i]);
        bpr = IOSurfaceGetBytesPerRow(own_surf[i]);
        own_surf_ctx[i] = CGBitmapContextCreate(base, (size_t)px_w, (size_t)px_h, 8,
                                                bpr, own_colorspace(),
                                                kCGImageAlphaPremultipliedFirst |
                                                    kCGBitmapByteOrder32Little);
        if (!own_surf_ctx[i]) {
            IOSurfaceUnlock(own_surf[i], 0, NULL);
            own_surf_free();
            return 0;
        }
        own_surf_gc[i] =
            [[NSGraphicsContext graphicsContextWithCGContext:own_surf_ctx[i]
                                                    flipped:YES] retain];
        IOSurfaceUnlock(own_surf[i], 0, NULL);
    }
    own_surf_w = px_w;
    own_surf_h = px_h;
    own_surf_cur = 0;
    return 1;
}

static void own_surf_alloc_size(NSView *v, int px_w, int px_h, int *aw, int *ah) {
    NSScreen *scr;
    NSRect f;
    CGFloat s;
    if (own_surf[0] && own_surf_w >= px_w && own_surf_h >= px_h) {
        *aw = own_surf_w;
        *ah = own_surf_h;
        return;
    }
    *aw = px_w;
    *ah = px_h;
    scr = v.window ? v.window.screen : nil;
    if (!scr) scr = [NSScreen mainScreen];
    if (!scr) return;
    f = scr.frame;
    s = scr.backingScaleFactor;
    if (s < 1.0) s = 1.0;
    if ((int)(f.size.width * s + 0.5) > *aw) *aw = (int)(f.size.width * s + 0.5);
    if ((int)(f.size.height * s + 0.5) > *ah) *ah = (int)(f.size.height * s + 0.5);
}

static int own_draw(CGContextRef ctx, NSGraphicsContext *gc, int ctx_h, CGFloat scale,
                    int64_t lw, int64_t lh, double *engine_ms) {
    double t0;
    int more;
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, (CGFloat)ctx_h);
    CGContextScaleCTM(ctx, scale, -scale);
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:gc];
    t0 = frame_now_ms();
    more = mac_frame(lw, lh);
    if (engine_ms) *engine_ms = frame_now_ms() - t0;
    [NSGraphicsContext restoreGraphicsState];
    CGContextRestoreGState(ctx);
    return more;
}

@implementation ZeusView
- (BOOL)isFlipped { return YES; }

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if ([self window]) [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSArray *urls = [[sender draggingPasteboard]
        readObjectsForClasses:@[ [NSURL class] ]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    return [urls count] ? NSDragOperationCopy : NSDragOperationNone;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray *urls = [[sender draggingPasteboard]
        readObjectsForClasses:@[ [NSURL class] ]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    BOOL any = NO;
    for (NSURL *u in urls) {
        if (![u isFileURL] || ![u path]) continue;
        loam_zeus_engine_media_picked(za_str([[[u path] lastPathComponent] UTF8String]), za_str([[u path] UTF8String]));
        any = YES;
    }
    return any;
}
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)preservesContentDuringLiveResize { return NO; }

- (BOOL)isAccessibilityElement { return NO; }
- (NSArray *)accessibilityChildren {
    NSAccessibilityRole const roles[] = {
        NSAccessibilityStaticTextRole, NSAccessibilityButtonRole,
        NSAccessibilityTextFieldRole,  NSAccessibilitySliderRole,
        NSAccessibilityCheckBoxRole,   NSAccessibilityListRole,
        NSAccessibilityTableRole,      NSAccessibilityImageRole,
    };
    NSMutableArray *kids = [NSMutableArray array];
    int32_t i, n = loam_zeus_engine_a11y_count();
    for (i = 0; i < n; i++) {
        int32_t x = 0, y = 0, w = 0, h = 0, rid = 0;
        loam_str lbl = loam_zeus_engine_a11y_item(i, &x, &y, &w, &h, &rid);
        NSString *label = lbl.ptr && lbl.len > 0
                              ? [[[NSString alloc] initWithBytes:lbl.ptr
                                                          length:(NSUInteger)lbl.len
                                                        encoding:NSUTF8StringEncoding] autorelease]
                              : @"";
        loam_abi_str_release(&lbl);
        NSRect frame = [self convertRect:NSMakeRect(x, y, w, h) toView:nil];
        if (self.window) frame = [self.window convertRectToScreen:frame];
        NSAccessibilityElement *el =
            [NSAccessibilityElement accessibilityElementWithRole:roles[rid >= 0 && rid < 8 ? rid : 0]
                                                           frame:frame
                                                           label:label
                                                          parent:self];
        if (el) [kids addObject:el];
    }
    return kids;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
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
        FramePhases p = {0};
        double t0 = frame_now_ms();
        p.more = mac_frame((int64_t)b.size.width, (int64_t)b.size.height);
        p.engine_ms = frame_now_ms() - t0;
        frame_trace(t0, p);
        mac_schedule_next(p.more);
    }
}

- (BOOL)wantsUpdateLayer {
    return (own_buffer_on() || own_surface_on()) ? YES : NO;
}

- (void)linkTick:(id)link {
    (void)link;
    mac_link_tick();
}

- (void)zeusRender {
    NSRect b = self.bounds;
    CGImageRef img;
    FramePhases p = {0};
    double t0, tprev, tnow;
    int px_w, px_h;
    t0 = tprev = frame_now_ms();
    if (!self.layer || b.size.width < 1 || b.size.height < 1) return;
    own_ctx_scale = self.window ? self.window.backingScaleFactor : 2.0;
    if (own_ctx_scale < 1.0) own_ctx_scale = 1.0;
    px_w = (int)(b.size.width * own_ctx_scale + 0.5);
    px_h = (int)(b.size.height * own_ctx_scale + 0.5);
    if (px_w < 1 || px_h < 1) return;
    if (own_surface_on()) {
        if ([self zeusRenderSurface:b pxw:px_w pxh:px_h]) return;
    }
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
        if (own_ctx)
            own_gc = [[NSGraphicsContext graphicsContextWithCGContext:own_ctx
                                                             flipped:YES] retain];
        self.layer.contents = nil;
        self.layer.contentsGravity = kCAGravityResize;
        self.layer.contentsScale = own_ctx_scale;
        self.layer.magnificationFilter = kCAFilterLinear;
        self.layer.opaque = YES;
    }
    if (!own_ctx) return;
    tnow = frame_now_ms();
    p.setup_ms = tnow - tprev;
    tprev = tnow;
    p.more = own_draw(own_ctx, own_gc, px_h, own_ctx_scale,
                      (int64_t)b.size.width, (int64_t)b.size.height, &p.engine_ms);
    tprev = frame_now_ms();
    img = own_image();
    tnow = frame_now_ms();
    p.image_ms = tnow - tprev;
    tprev = tnow;
    if (img) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        self.layer.contents = (id)img;
        [CATransaction commit];
        CGImageRelease(img);
    }
    p.set_ms = frame_now_ms() - tprev;
    frame_trace(t0, p);
    mac_memory_relief(p.more);
    mac_schedule_next(p.more);
}

- (int)zeusRenderSurface:(NSRect)b pxw:(int)px_w pxh:(int)px_h {
    static int shown_w, shown_h;
    FramePhases p = {0};
    double t0, tprev;
    int i, more, realloc, aw, ah;
    own_surf_alloc_size(self, px_w, px_h, &aw, &ah);
    realloc = (!own_surf[0] || own_surf_w != aw || own_surf_h != ah) ? 1 : 0;
    if (loam_platform_host_env_flag(0) && realloc)
        fprintf(stderr, "[surf] set %dx%d for a %dx%d frame\n", aw, ah, px_w, px_h);
    if (!own_surf_ensure(aw, ah)) {
        loam_platform_host_set_display_path(1);
        self.layer.contents = nil;
        return 0;
    }
    if (realloc) {
        self.layer.contentsGravity = kCAGravityResize;
        self.layer.contentsScale = own_ctx_scale;
        self.layer.magnificationFilter = kCAFilterLinear;
        self.layer.opaque = YES;
        if ([self.layer respondsToSelector:@selector(setContentsFormat:)])
            self.layer.contentsFormat = kCAContentsFormatRGBA8Uint;
        shown_w = shown_h = 0;
    }
    i = own_surf_cur;
    int tries = 0;
    int ns = loam_platform_host_surface_count();
    while (tries < ns && IOSurfaceIsInUse(own_surf[i])) {
        i = (i + 1) % ns;
        tries++;
    }
    if (tries == ns) {
        double now = frame_now_ms();
        own_surf_skips++;
        if (own_surf_wait_t0 == 0.0) own_surf_wait_t0 = now;
        if (now - own_surf_wait_t0 < OWN_SURF_WAIT_MS) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)NSEC_PER_MSEC),
                           dispatch_get_main_queue(), ^{
                [self zeusRender];
            });
        } else {
            own_surf_wait_t0 = 0.0;
            mac_schedule_next(1);
        }
        return 1;
    }
    own_surf_last_wait = (own_surf_wait_t0 == 0.0) ? 0.0 : frame_now_ms() - own_surf_wait_t0;
    own_surf_wait_t0 = 0.0;
    if (IOSurfaceLock(own_surf[i], 0, NULL) != 0) {
        own_surf_skips++;
        mac_schedule_next(1);
        return 1;
    }
    t0 = tprev = frame_now_ms();
    more = own_draw(own_surf_ctx[i], own_surf_gc[i], ah, own_ctx_scale,
                    (int64_t)b.size.width, (int64_t)b.size.height, &p.engine_ms);
    IOSurfaceUnlock(own_surf[i], 0, NULL);
    tprev = frame_now_ms();
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (shown_w != px_w || shown_h != px_h) {
        self.layer.contentsRect = CGRectMake(0, 0, (CGFloat)px_w / (CGFloat)aw,
                                             (CGFloat)px_h / (CGFloat)ah);
        shown_w = px_w;
        shown_h = px_h;
    }
    self.layer.contents = (id)own_surf[i];
    [CATransaction commit];
    p.more = more;
    p.set_ms = frame_now_ms() - tprev;
    own_surf_cur = (i + 1) % ns;
    frame_trace(t0, p);
    mac_memory_relief(more);
    mac_schedule_next(more);
    return 1;
}

- (void)updateLayer {
    [self zeusRender];
}

- (void)mouseMoved:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    int dirty = loam_zeus_engine_hover((int32_t)p.x, (int32_t)p.y);
    [mac_cursor_now() set];
    if (dirty) [self setNeedsDisplay:YES];
}

- (void)mouseExited:(NSEvent *)event {
    (void)event;
    if (loam_zeus_engine_hover(-1, -1)) [self setNeedsDisplay:YES];
    [[NSCursor arrowCursor] set];
}

- (void)mouseDown:(NSEvent *)event {
    [[self window] makeFirstResponder:self];
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    int ctrl = ([event modifierFlags] & NSEventModifierFlagControl) ? 1 : 0;
    int r = loam_zeus_engine_mac_mouse_down((int32_t)p.x, (int32_t)p.y, ctrl);
    if (r == 2) {
        if ([event clickCount] == 2) {
            NSString *act = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
            if ([act isEqualToString:@"Minimize"]) [[self window] performMiniaturize:nil];
            else if (![act isEqualToString:@"None"]) [[self window] performZoom:nil];
        } else {
            [[self window] performWindowDragWithEvent:event];
        }
    } else if (r == 1) {
        [self setNeedsDisplay:YES];
    }
}

- (void)rightMouseDown:(NSEvent *)event {
    [[self window] makeFirstResponder:self];
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (loam_zeus_engine_context_click((int32_t)p.x, (int32_t)p.y))
        [self setNeedsDisplay:YES];
    else
        [super rightMouseDown:event];
}

- (void)mouseDragged:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (loam_zeus_engine_pointer_move((int32_t)p.x, (int32_t)p.y, 0, 0))
        [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)event {
    (void)event;
    loam_zeus_engine_mouseup();
}

- (void)scrollWheel:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    int reset = (event.phase == NSEventPhaseBegan || event.phase == NSEventPhaseMayBegin ||
                 event.momentumPhase == NSEventPhaseBegan) ? 1 : 0;
    int shift = ([event modifierFlags] & NSEventModifierFlagShift) ? 1 : 0;
    if (loam_zeus_engine_wheel((int32_t)p.x, (int32_t)p.y,
                               (int32_t)([event scrollingDeltaX] * 256.0),
                               (int32_t)([event scrollingDeltaY] * 256.0),
                               [event hasPreciseScrollingDeltas] ? 1 : 0, shift, reset))
        [self setNeedsDisplay:YES];
}

- (int)zeusMods:(NSEvent *)event {
    NSEventModifierFlags f = [event modifierFlags];
    return ((f & NSEventModifierFlagShift) ? ZEUS_MOD_SHIFT : 0) |
           ((f & NSEventModifierFlagControl) ? ZEUS_MOD_CTRL : 0) |
           ((f & NSEventModifierFlagOption) ? ZEUS_MOD_ALT : 0) |
           ((f & NSEventModifierFlagCommand) ? ZEUS_MOD_CMD : 0);
}

- (void)keyDown:(NSEvent *)event {
    NSString *chars = [event characters];
    NSString *raw = [event charactersIgnoringModifiers];
    int ch = [chars length] > 0 ? (int)[chars characterAtIndex:0] : 0;
    int rch = [raw length] > 0 ? (int)[raw characterAtIndex:0] : 0;
    int r = loam_zeus_engine_mac_key_down([event keyCode], ch, rch, [self zeusMods:event]);
    if (r == 3) [self paste:nil];
    else if (r == 2) [self interpretKeyEvents:@[ event ]];
    else if (r == 1) [self setNeedsDisplay:YES];
    else [super keyDown:event];
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
    if (u && loam_zeus_engine_insert(za_str(u))) {
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
    loam_zeus_engine_marked(za_str([s UTF8String]));
    [self setNeedsDisplay:YES];
}

- (void)unmarkText {
    [marked release];
    marked = nil;
    loam_zeus_engine_marked(za_str(""));
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
    else if (sel == @selector(moveLeftAndModifySelection:)) { key = 1000; mods = ZEUS_MOD_SHIFT; }
    else if (sel == @selector(moveRightAndModifySelection:)) { key = 1001; mods = ZEUS_MOD_SHIFT; }
    else if (sel == @selector(moveUpAndModifySelection:)) { key = 1002; mods = ZEUS_MOD_SHIFT; }
    else if (sel == @selector(moveDownAndModifySelection:)) { key = 1003; mods = ZEUS_MOD_SHIFT; }
    else if (sel == @selector(insertTab:)) key = ZEUS_K_TAB;
    else if (sel == @selector(cancelOperation:)) key = 27;
    if (key && loam_zeus_engine_key_ev(key, mods)) [self setNeedsDisplay:YES];
}

- (void)paste:(id)sender {
    (void)sender;
    NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    const char *u = s ? [s UTF8String] : NULL;
    if (u && loam_zeus_engine_insert(za_str(u))) [self setNeedsDisplay:YES];
}

- (void)keyUp:(NSEvent *)event {
    NSString *chars = [event charactersIgnoringModifiers];
    int rch = [chars length] > 0 ? (int)[chars characterAtIndex:0] : 0;
    if (loam_zeus_engine_mac_key_up([event keyCode], rch, [self zeusMods:event]))
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
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls {
    (void)app;
    for (NSURL *u in urls) {
        NSString *src = [u isFileURL] ? [u path] : [u absoluteString];
        loam_zeus_engine_media_picked(za_str([[[u path] lastPathComponent] UTF8String]),
                                      za_str([src UTF8String]));
    }
}
@end

static NSRect mac_screen_visible(void);

static void mac_apply_title(const char *t) {
    if (!g_win) return;
    [g_win setTitle:(t && t[0]) ? [NSString stringWithUTF8String:t] : @"Zeus"];
}

static void mac_clipboard_write(const char *t) {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:[NSString stringWithUTF8String:t ? t : ""] forType:NSPasteboardTypeString];
}

static void mac_run(void) {
    const char *snap = getenv("ZEUS_SNAPSHOT");
    if (snap && *snap) mac_snapshot(snap);
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
        g_win = win;
        zeus_set_title_hook(mac_apply_title);
        zeus_set_clipboard_hook(mac_clipboard_write);
        [win setBackgroundColor:[NSColor whiteColor]];
        ZeusView *view = [[ZeusView alloc] initWithFrame:frame];
        g_view = view;
        [win setReleasedWhenClosed:YES];
        [win setRestorable:NO];
        [win setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        if (!loam_platform_host_env_flag(3))
            [win setColorSpace:[NSColorSpace sRGBColorSpace]];
        [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [win setContentView:view];
        if (loam_platform_host_window_fills_screen())
            [win setFrame:mac_screen_visible() display:YES];
        else
            [win center];
        if (own_buffer_on() || own_surface_on()) [view setWantsLayer:YES];
        mac_link_arm();
        zeus_window_opened();
        [win makeFirstResponder:view];
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
}

static NSRect mac_screen_visible(void) {
    @autoreleasepool {
        NSRect vis;
        [NSApplication sharedApplication];
        vis = [[NSScreen mainScreen] visibleFrame];
        if (vis.size.width < 1 || vis.size.height < 1)
            vis = NSMakeRect(0, 0, (CGFloat)ZEUS_DEFAULT_WIN_W, (CGFloat)ZEUS_DEFAULT_WIN_H);
        return vis;
    }
}

static int64_t mac_initial_w(void) { return (int64_t)(mac_screen_visible().size.width + 0.5); }
static int64_t mac_initial_h(void) { return (int64_t)(mac_screen_visible().size.height + 0.5); }

static void mac_pick_image(char *out, int cap, int64_t *w, int64_t *h) {
    NSOpenPanel *p;
    NSString *path;
    const char *utf;
    NSArray *dirs;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!out || cap <= 0) return;
    out[0] = 0;
    p = [NSOpenPanel openPanel];
    [p setCanChooseFiles:YES];
    [p setCanChooseDirectories:NO];
    [p setAllowsMultipleSelection:NO];
    [p setAllowedFileTypes:@[ @"png", @"jpg", @"jpeg", @"gif", @"webp" ]];
    dirs = NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES);
    if ([dirs count] > 0)
        [p setDirectoryURL:[NSURL fileURLWithPath:[dirs objectAtIndex:0]]];
    if ([p runModal] != NSModalResponseOK) return;
    path = [[p URL] path];
    utf = path ? [path UTF8String] : NULL;
    if (!utf) return;
    strncpy(out, utf, (size_t)cap - 1);
    out[cap - 1] = 0;
    za_image_size(utf, w, h);
}

static CGDataProviderRef blit_prov = NULL;
static int64_t blit_gen = -1;

static CGColorSpaceRef blit_srgb(void) {
    static CGColorSpaceRef cs;
    if (!cs) cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!cs) cs = own_colorspace();
    return cs;
}

static int64_t mac_blit(void *ctx, const uint8_t *px, int64_t w, int64_t h,
                       int64_t gen, int64_t scale) {
    (void)ctx;
    if (!px || w <= 0 || h <= 0 || loam_platform_plat_headless() || !g_view || !g_view.layer)
        return 0;
    if (!blit_prov || blit_gen != gen) {
        if (blit_prov) { CGDataProviderRelease(blit_prov); blit_prov = NULL; }
        blit_prov = CGDataProviderCreateWithData(NULL, (void *)px,
                                                (size_t)(w * h * 4), NULL);
        if (!blit_prov) return 0;
        blit_gen = gen;
    }
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4,
                                   blit_srgb(),
                                   kCGImageAlphaPremultipliedFirst |
                                       kCGBitmapByteOrder32Little,
                                   blit_prov, NULL, false, kCGRenderingIntentDefault);
    if (!img) return 0;
    g_view.layer.contentsGravity = kCAGravityResize;
    g_view.layer.contentsScale = (CGFloat)scale;
    g_view.layer.opaque = YES;
    g_view.layer.magnificationFilter = kCAFilterNearest;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    g_view.layer.contents = (id)img;
    [CATransaction commit];
    CGImageRelease(img);
    return 1;
}

__attribute__((constructor))
static void zeus_mac_register(void) {
    zeus_bind_draw(za_draw_table());
    zeus_set_platform(mac_run, za_measure, za_redraw);
    zeus_set_pick_image(mac_pick_image);
    zeus_set_image_size(za_image_size);
    {
        ZeusMedia media = {0};
        za_media_table(&media);
        media.pick = mac_media_pick;
        media.pick_audio = mac_audio_pick;
        media.voice = mac_media_voice;
        media.fullscreen = mac_fullscreen;
        zeus_set_media(media);
    }
    zeus_set_font_hooks(za_load_font, za_set_family);
    zeus_set_blit(NULL, mac_blit);
    zeus_set_initial_view(mac_initial_w, mac_initial_h);
}
