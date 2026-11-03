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
#include "zeus_key.h"
#include <string.h>
#include <stdlib.h>
#include <malloc/malloc.h>

static NSView *g_view;
static NSWindow *g_win;

/* The shared Apple layer (../apple/zeus_apple.h) paints, measures, decodes
   images and plays media; this file supplies its two hooks and the rest:
   window, view, input, display, pickers, voice tools and hot reload. */
CGContextRef za_cg(void) {
    return [[NSGraphicsContext currentContext] CGContext];
}

void za_redraw(void) {
    if (g_view) [g_view setNeedsDisplay:YES];
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
    zeus_media_picked([[path lastPathComponent] UTF8String], [path UTF8String]);
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
        zeus_media_picked([[path lastPathComponent] UTF8String], [path UTF8String]);
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

static int env_truthy(const char *name) {
    const char *e = getenv(name);
    return e && e[0] && e[0] != '0';
}

/* The presentation path, chosen once, from the environment:

     2  owned IOSurface set    DEFAULT — our pixels ARE the layer's texture, so
                              a frame is never copied, and the set is what lets
                              the next frame be drawn while the compositor still
                              reads the last one (`ZEUS_OWN_SURFACE=1`).
     1  owned bitmap           one `CGBitmapContext` wrapped in a fresh CGImage
                              per frame, handed to the layer as its `contents`
                              (`ZEUS_OWN_SURFACE=0`, `ZEUS_OWN_BUFFER=1`).
     0  AppKit's own store     `drawRect:` paints straight into the window's
                              backing store (`APP_KIT=1`, `ZEUS_OWN_BUFFER=0`).

   The IOSurface set is the default; the bitmap and AppKit's store are opt-in,
   and a surface allocation failure falls back to the bitmap. */
static int display_path = -1;

static int display_path_get(void) {
    if (display_path < 0) {
        /* Every explicit spelling keeps the path it always meant; only the
           ABSENCE of all of them moved. Order matters only if more than one is
           set: APP_KIT wins, then ZEUS_OWN_SURFACE, then ZEUS_OWN_BUFFER. */
        if (env_truthy("APP_KIT"))
            display_path = 0;
        else if (getenv("ZEUS_OWN_SURFACE"))
            display_path = env_truthy("ZEUS_OWN_SURFACE") ? 2 : 1;
        else if (getenv("ZEUS_OWN_BUFFER"))
            display_path = env_truthy("ZEUS_OWN_BUFFER") ? 1 : 0;
        else
            display_path = 2;
    }
    return display_path;
}

/* The two owned paths, as predicates. `own_buffer_on()` is the bitmap path,
   `own_surface_on()` the IOSurface pair; `wantsUpdateLayer` asks whether either
   is in use, since both hand the layer memory we own and AppKit must not then
   allocate a store for the view. */
static int own_buffer_on(void) { return display_path_get() == 1; }
static int own_surface_on(void) { return display_path_get() == 2; }

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

/* Frames the surface path skipped because the compositor held every surface.
   Printed by the frame trace, so a stutter can be told from a slow frame. */
static int own_surf_skips;
/* When every surface is busy, the frame is retried shortly instead of at the
   next display refresh (see `zeusRenderSurface:`). `own_surf_wait_t0` is when
   the current wait began (0 = not waiting); `own_surf_last_wait` is how long
   the last drawn frame waited for a free surface, for the frame trace. */
static double own_surf_wait_t0;
static double own_surf_last_wait;

/* Hand freed malloc pages back to the system. Start-up (font atlas, decoded
   images, the first layouts) frees ~15 MB of large blocks that the zone keeps
   dirty and the footprint keeps counting; relief returns them. Run once when
   start-up has settled (frame 120), then whenever the window goes idle, at
   most every 5 s — never per frame, since it walks every zone. */
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
            "more=%d next=%dms surf_skip=%d surf_wait=%.1f\n",
            interval, p.setup_ms, p.engine_ms, p.image_ms, p.set_ms, interval - work, p.more,
            (int)loam_zeus_engine_next_ms(), own_surf_skips, own_surf_last_wait);
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
/* App-drawn chrome (`zeus.window_chrome(CHROME_CUSTOM)`): the content view
   runs under a transparent title bar with the system title hidden, and the
   window buttons stay where they are. The app's `TitleBar` paints the strip;
   this reports its box each frame — the bar's height (0 in full screen, where
   the system bar slides in over the content on its own) and the room the
   buttons take — and a press on the bar, off any control, moves the window
   (`mouseDown`). Checked every frame, so it does not matter whether the app
   asked before or after the window opened. */
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


/* `ZEUS_SNAPSHOT=out.png`: paint one frame at 2x through the real draw table
   into an offscreen sRGB bitmap, write it as PNG, and exit, with no window.
   `ZEUS_SNAPSHOT_SIZE=WxH` overrides the window size (a tall one captures a
   whole scrolling page).
   Animations are stepped for one simulated second first, so the frame is the
   settled one. For host-level pixel checks and before / after diffs. */
static void mac_snapshot(const char *path) {
    @autoreleasepool {
        int64_t w = zeus_window_width(), h = zeus_window_height();
        const char *sz = getenv("ZEUS_SNAPSHOT_SIZE");
        long sw = 0, sh = 0;
        CGFloat scale = 2;
        if (sz && sscanf(sz, "%ldx%ld", &sw, &sh) == 2 && sw > 0 && sh > 0) {
            w = sw;
            h = sh;
            zeus_set_window_size(w, h);
        }
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
        zeus_layout(w, h);
        for (int i = 0; i < 60; i++) zeus_step(1.f / 60.f);
        zeus_layout(w, h);
        zeus_paint(NULL, za_draw_table());
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
    mac_sync_chrome();
    zeus_layout(vw, vh);
    more = zeus_step(dt);
    d = za_draw_table();
    zeus_paint(NULL, d);
    if (za_media_live()) more = 1;
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
static int own_surf_n;
static int own_surf_w, own_surf_h;

/* How many surfaces to rotate over. 2 by default; the count is clamped to 2..4
   and read once, so a frame never re-parses the environment.

   `ZEUS_OWN_SURFACE` and `ZEUS_OWN_SURFACES` differ by one letter and mean
   different things, which is a trap worth closing: the singular selects the
   DISPLAY PATH (0 = bitmap, 1 = this one) and the plural sets the COUNT. To make
   the singular forgiving, a value of 2 or more there is taken as the count too,
   so `ZEUS_OWN_SURFACE=2` means what it reads as — the surface path, two
   buffers — rather than silently meaning `=1` with three. The plural wins when
   both are set. */
static int own_surf_count(void) {
    if (own_surf_n <= 0) {
        const char *e = getenv("ZEUS_OWN_SURFACES");
        int n = OWN_SURFACES_DEFAULT;
        if (!e || !e[0]) e = getenv("ZEUS_OWN_SURFACE");
        if (e && e[0]) {
            int v = atoi(e);
            /* A bare "1" (or a non-numeric truthy value) is the path switch, not
               a count; anything 2+ is a count. */
            if (v >= 2) n = v;
        }
        if (n < 2) n = 2;
        if (n > OWN_SURFACES_MAX) n = OWN_SURFACES_MAX;
        own_surf_n = n;
    }
    return own_surf_n;
}

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

/* (Re)create the set at physical size: 1 on success. On failure everything is
   released and the caller falls back to the bitmap path — a display path that
   cannot be allocated must cost memory, not the window. */
static int own_surf_ensure(int px_w, int px_h) {
    NSDictionary *props;
    int i, n;
    if (own_surf[0] && own_surf_w == px_w && own_surf_h == px_h) return 1;
    own_surf_free();
    n = own_surf_count();
    props = @{ (id)kIOSurfaceWidth : @(px_w),
               (id)kIOSurfaceHeight : @(px_h),
               (id)kIOSurfaceBytesPerElement : @4,
               (id)kIOSurfaceBytesPerRow : @(px_w * 4),
               (id)kIOSurfacePixelFormat : @(OWN_SURF_BGRA) };
    for (i = 0; i < n; i++) {
        void *base;
        size_t bpr;
        own_surf[i] = IOSurfaceCreate((CFDictionaryRef)props);
        if (!own_surf[i]) {
            own_surf_free();
            return 0;
        }
        /* The stride is the surface's, not necessarily `px_w * 4`: IOSurface is
           free to pad rows. Read it rather than assume it, and build the
           context over what it really is. */
        if (IOSurfaceLock(own_surf[i], 0, NULL) != 0) {
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

/* The size the surface set is allocated at for a frame of `px_w x px_h`: the
   current set if it already fits, else the window's screen at its backing
   scale (the largest a drag can make the window), or the frame itself when
   that is somehow bigger. Growth only — see the note above `OWN_SURFACES_MAX`. */
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

/* Draw one frame into `ctx`: physical pixels, `scale` pixels per point, y down
   (the view is flipped). `ctx_h` is the CONTEXT's height in pixels, which is
   what puts logical (0,0) at the first row of the buffer; the frame may be
   shorter than the context (the surface path draws into the top-left of a
   larger surface). BOTH display paths go through here, which is what keeps
   their pixels from drifting apart — the memory being drawn into is the only
   thing that differs between them. */
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

/* A file dropped on the window goes where the media picker's result goes
   (`zeus_media_picked`); the app decides what to do with it. */
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
        zeus_media_picked([[[u path] lastPathComponent] UTF8String], [[u path] UTF8String]);
        any = YES;
    }
    return any;
}
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
    /* Both owned paths paint into memory they hand the layer, so AppKit must
       not allocate a store for the view. */
    return (own_buffer_on() || own_surface_on()) ? YES : NO;
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
    /* The owned-IOSurface path, when asked for: one buffer and no copy, instead
       of a bitmap plus CoreAnimation's own texture. Falling through (0) means
       the surfaces could not be allocated, and the bitmap path below runs. */
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
    mac_memory_relief(p.more);
    mac_schedule_next(p.more);
}

/* The owned-IOSurface frame. Returns 1 when it drew (or deliberately skipped)
   the frame and 0 when the path is unavailable, in which case the caller draws
   the bitmap path instead. */
- (int)zeusRenderSurface:(NSRect)b pxw:(int)px_w pxh:(int)px_h {
    /* The region of the surface the layer shows, in pixels. A resize moves this
       and nothing else — see the note above `OWN_SURFACES_MAX`. */
    static int shown_w, shown_h;
    FramePhases p;
    double t0, tprev;
    int i, more, realloc, aw, ah;
    own_surf_alloc_size(self, px_w, px_h, &aw, &ah);
    realloc = (!own_surf[0] || own_surf_w != aw || own_surf_h != ah) ? 1 : 0;
    if (frame_debug_on() && realloc)
        fprintf(stderr, "[surf] set %dx%d for a %dx%d frame\n", aw, ah, px_w, px_h);
    if (!own_surf_ensure(aw, ah)) {
        /* No surface: fall back to the bitmap, not to AppKit's store — the view
           is already layer-backed, so AppKit's drawn path is not available. */
        display_path = 1;
        self.layer.contents = nil;
        return 0;
    }
    if (realloc) {
        /* Static layer properties: set them when the backing changes, never per
           frame. Writing `contentsScale` every frame marks the layer dirty and
           can force a texture reallocation. The old surface stays the layer's
           contents until the new one is drawn below — never nil in between. */
        self.layer.contentsGravity = kCAGravityResize;
        self.layer.contentsScale = own_ctx_scale;
        self.layer.magnificationFilter = kCAFilterLinear;
        self.layer.opaque = YES;
        /* Say the surface is 8-bit RGBA — the same thing the bitmap path's
           CGImage says. Without it CoreAnimation has to guess the format. */
        if ([self.layer respondsToSelector:@selector(setContentsFormat:)])
            self.layer.contentsFormat = kCAContentsFormatRGBA8Uint;
        shown_w = shown_h = 0;
    }
    /* Rotate to a surface the compositor is NOT using, then draw into it. This is
       the whole point of having more than one: the surface in `layer.contents`
       is being read, and writing it mid-scanout is what shows as a flash or a
       tear. `IOSurfaceIsInUse` is the only non-blocking probe for that. If none
       is free (the compositor is holding all of them), never write over one it
       is reading — a late frame is one frame of animation, a torn one is a
       visible flash. */
    i = own_surf_cur;
    int tries = 0;
    int ns = own_surf_count();
    while (tries < ns && IOSurfaceIsInUse(own_surf[i])) {
        i = (i + 1) % ns;
        tries++;
    }
    if (tries == ns) {
        /* The compositor lets go of the surface it showed last a little after
           the refresh that replaced it — usually just after the link tick that
           brought us here. Waiting for the next tick would drop a whole frame,
           so poll again in 1 ms, for up to `OWN_SURF_WAIT_MS`, and only then
           fall back to the next refresh. */
        double now = frame_now_ms();
        own_surf_skips++;
        if (own_surf_wait_t0 == 0.0) own_surf_wait_t0 = now;
        if (now - own_surf_wait_t0 < OWN_SURF_WAIT_MS) {
            /* Render directly: `setNeedsDisplay:` would be coalesced to the
               next refresh, which is exactly the frame this wait saves. */
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
    /* Only for the CPU write now, and it contends with nobody: the surface was
       just found unused. A blocking lock is correct here (and its options must
       match the unlock's, which `kIOSurfaceLockAvoidSync` did not). */
    if (IOSurfaceLock(own_surf[i], 0, NULL) != 0) {
        own_surf_skips++;
        mac_schedule_next(1);
        return 1;
    }
    memset(&p, 0, sizeof p);
    t0 = tprev = frame_now_ms();
    more = own_draw(own_surf_ctx[i], own_surf_gc[i], ah, own_ctx_scale,
                    (int64_t)b.size.width, (int64_t)b.size.height, &p.engine_ms);
    IOSurfaceUnlock(own_surf[i], 0, NULL);
    tprev = frame_now_ms();
    p.setup_ms = 0.0;
    p.image_ms = 0.0; /* there is no image step on this path by definition */
    /* Assigning `contents` is not a plain write: outside a transaction with
       actions disabled, CoreAnimation cross-fades between frames. */
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (shown_w != px_w || shown_h != px_h) {
        /* The frame lives in the surface's top-left; `contentsRect` is in unit
           coordinates of the contents, origin at its top-left, so this is the
           one rectangle the compositor stretches to the layer — 1:1, since the
           region is `bounds x scale` pixels and `contentsScale` is `scale`.
           Moved in the same commit as the frame drawn for it, so a skipped
           frame keeps the old frame with the old rectangle. */
        self.layer.contentsRect = CGRectMake(0, 0, (CGFloat)px_w / (CGFloat)aw,
                                             (CGFloat)px_h / (CGFloat)ah);
        shown_w = px_w;
        shown_h = px_h;
    }
    self.layer.contents = (id)own_surf[i];
    [CATransaction commit];
    p.more = more;
    p.set_ms = frame_now_ms() - tprev;
    own_surf_cur = (i + 1) % own_surf_count(); /* next frame starts past this one */
    frame_trace(t0, p);
    mac_memory_relief(more);
    mac_schedule_next(more);
    return 1;
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
    /* Control-click is the Mac's secondary click. */
    if (([event modifierFlags] & NSEventModifierFlagControl) &&
        zeus_handle_context_click((int64_t)p.x, (int64_t)p.y)) {
        [self setNeedsDisplay:YES];
        return;
    }
    /* An app-drawn title bar moves the window like the system's: drag it
       anywhere off its controls, double-click to zoom (or whatever the user
       chose for title-bar double-clicks in System Settings). */
    if (zeus_window_drag_at((int64_t)p.x, (int64_t)p.y)) {
        if ([event clickCount] == 2) {
            NSString *act = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
            if ([act isEqualToString:@"Minimize"]) [[self window] performMiniaturize:nil];
            else if (![act isEqualToString:@"None"]) [[self window] performZoom:nil];
        } else {
            [[self window] performWindowDragWithEvent:event];
        }
        return;
    }
    if (zeus_handle_click((int64_t)p.x, (int64_t)p.y))
        [self setNeedsDisplay:YES];
}

- (void)rightMouseDown:(NSEvent *)event {
    [[self window] makeFirstResponder:self];
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (zeus_handle_context_click((int64_t)p.x, (int64_t)p.y))
        [self setNeedsDisplay:YES];
    else
        [super rightMouseDown:event];
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
    /* The engine scrolls in whole points, but a trackpad reports fractions —
       a slow drag or the tail of a momentum glide is a stream of 0.3-0.9 pt
       deltas. Truncating each one dropped them all (the content froze, then
       jumped when a delta finally crossed 1 pt) and shaved distance off every
       fast one: the stagger. Carry the remainder into the next event instead,
       and start fresh when a new gesture begins or the direction flips. */
    static CGFloat rem_x, rem_y;
    if (event.phase == NSEventPhaseBegan || event.phase == NSEventPhaseMayBegin ||
        event.momentumPhase == NSEventPhaseBegan) {
        rem_x = 0;
        rem_y = 0;
    }
    if ((rem_x > 0 && dx < 0) || (rem_x < 0 && dx > 0)) rem_x = 0;
    if ((rem_y > 0 && dy < 0) || (rem_y < 0 && dy > 0)) rem_y = 0;
    dx += rem_x;
    dy += rem_y;
    int64_t ix = (int64_t)dx, iy = (int64_t)dy;
    rem_x = dx - (CGFloat)ix;
    rem_y = dy - (CGFloat)iy;
    if (ix == 0 && iy == 0) return;
    /* No engine coast: a trackpad's inertia already arrives from AppKit as a
       stream of momentum-phase events, so precise deltas step exactly where
       they land. A mouse notch eases to its target instead, like a browser's
       smooth wheel scroll — bounded, and retargeted by the next notch. */
    int dirty;
    if (precise)
        dirty = zeus_handle_scroll_step((int64_t)p.x, (int64_t)p.y, -ix, -iy);
    else
        dirty = zeus_handle_scroll_smooth((int64_t)p.x, (int64_t)p.y, -ix, -iy);
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
/* Finder's Open / Open With and a drop on the Dock icon, for a bundled app
   that declares document types: each file goes where a picked or dropped one
   does (`zeus_media_picked`), in order. */
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls {
    (void)app;
    for (NSURL *u in urls) {
        if ([u isFileURL])
            zeus_media_picked([[[u path] lastPathComponent] UTF8String], [[u path] UTF8String]);
        else
            zeus_media_picked([[[u path] lastPathComponent] UTF8String], [[u absoluteString] UTF8String]);
    }
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

/* `plat_clipboard_write` (`CopyButton`): the general pasteboard. */
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
        const char *t = zeus_window_title();
        [win setTitle:t ? [NSString stringWithUTF8String:t] : @"Zeus"];
        g_win = win;
        zeus_set_title_hook(mac_apply_title);
        zeus_set_clipboard_hook(mac_clipboard_write);
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
        /* The owned paths supply the layer's content themselves, so the view
           must be layer-backed; with `APP_KIT=true` AppKit manages the store as
           usual. */
        if (own_buffer_on() || own_surface_on()) [view setWantsLayer:YES];
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
    ZEUS_BIND(scroll_smooth, "loam_zeus_engine_scroll_smooth");
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
    ZEUS_BIND(context_click, "loam_zeus_engine_context_click");
    ZEUS_BIND(window_drag_at, "loam_zeus_engine_window_drag_at");
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
            zeus_set_clipboard_hook(mac_clipboard_write);
        zeus_set_clipboard_hook(mac_clipboard_write);
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
            if (own_buffer_on() || own_surface_on()) [view setWantsLayer:YES];
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
    NSArray *dirs;
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
    dirs = NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES);
    if ([dirs count] > 0) {
        [p setDirectoryURL:[NSURL fileURLWithPath:[dirs objectAtIndex:0]]];
    }
    if ([p runModal] != NSModalResponseOK) return;
    path = [[p URL] path];
    utf = path ? [path UTF8String] : NULL;
    if (!utf) return;
    strncpy(out, utf, (size_t)cap - 1);
    out[cap - 1] = 0;
    za_image_size(utf, w, h);
}

/* Phase 4: the ONE blit of a frame Zeus rasterized itself.

   The Loam rasterizer has already produced every pixel, in its own BGRA8
   premultiplied sRGB buffer. This wraps those bytes and hands the image to the
   layer: nothing is redrawn here and no pixel is copied in our code — the data
   provider reads the engine's buffer directly.

   Two details that are easy to get wrong and expensive to get wrong:

     * The CGImage WRAPPER is recreated per frame. Assigning the same CGImage
       object back to `contents` is a pointer store that CoreAnimation treats as
       "no change", so a reused wrapper would simply never repaint. The wrapper
       is a header over the same bytes, so this costs an allocation, not a
       20 MB copy.
     * The data PROVIDER and the buffer are reused across frames, keyed by
       `gen`, which the Loam side bumps only on reallocation (a size or
       backing-scale change). That is the contract that keeps a steady frame
       from churning a second full-size buffer. */
static CGDataProviderRef blit_prov = NULL;
static int64_t blit_gen = -1;

static CGColorSpaceRef blit_srgb(void) {
    static CGColorSpaceRef cs;
    if (!cs) cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    /* A missing sRGB profile is not a reason to skip the frame. */
    if (!cs) cs = own_colorspace();
    return cs;
}

static int64_t mac_blit(void *ctx, const uint8_t *px, int64_t w, int64_t h,
                       int64_t gen, int64_t scale) {
    (void)ctx;
    if (!px || w <= 0 || h <= 0) return 0;
    /* Headless, or no window yet: say so rather than pretend we presented. */
    if (loam_zeus_plat_headless() || !g_view) return 0;
    if (!g_view.layer) return 0;
    if (!blit_prov || blit_gen != gen) {
        if (blit_prov) { CGDataProviderRelease(blit_prov); blit_prov = NULL; }
        blit_prov = CGDataProviderCreateWithData(NULL, (void *)px,
                                                (size_t)(w * h * 4), NULL);
        if (!blit_prov) return 0;
        blit_gen = gen;
    }
    /* BGRA8 premultiplied, dimensions exactly the buffer's, scale exactly the
       buffer's — so the compositor neither converts nor resamples (rules 1-3). */
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
    zeus_set_platform(mac_run, za_measure, mac_redraw);
#ifdef LOAM_HOST_BUILD
    /* A dev host loads the app as an image and owns the loop; the shim's
       `zeus.host_run` reaches the loader through this hook. */
    zeus_set_host_hooks(loam_mac_host_run);
#endif
    zeus_set_pick_image(mac_pick_image);
    zeus_set_image_size(za_image_size);
    {
        ZeusMedia media;
        memset(&media, 0, sizeof media);
        za_media_table(&media);
        media.pick = mac_media_pick;
        media.pick_audio = mac_audio_pick;
        media.voice = mac_media_voice;
        media.fullscreen = mac_fullscreen;
        zeus_set_media(media);
    }
    zeus_set_font_hooks(za_load_font, NULL);
    /* The one blit of a frame: the engine hands its own BGRA8 buffer over, and
       this wraps it in a CGImage over a reused data provider. */
    zeus_set_blit(NULL, mac_blit);
    /* `zeus.App` reads `window_size()` before any other host entry point runs,
       so the lazy work-area getters must be known by load time or the engine
       answers with the 640×480 placeholder and a default app opens as a small
       centered window. This stores two pointers only; the AppKit read happens
       on first call, and only on a real (non-headless) run. */
    zeus_set_initial_view(mac_initial_w, mac_initial_h);
}
