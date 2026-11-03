/* apple_media.m — media playback for macOS and iOS (`plat_media_*`).
 *
 * One AVPlayer per slot. Frames come out of an AVPlayerItemVideoOutput and
 * become a CGImage that the `image` op draws for a `media:<slot>` src, so a
 * video is painted like any other image (see zeus_apple.h). The platform file
 * adds its own pickers, voice tools and full screen to the table.
 */
#import "zeus_apple.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>
#import <VideoToolbox/VideoToolbox.h>
#include <stdlib.h>
#include <string.h>

enum { MEDIA_SLOTS_MAX = 8 };
typedef struct {
    AVPlayer *player;
    AVPlayerItem *item;
    AVPlayerItemVideoOutput *out;
    id end_obs;
    CGImageRef frame;
    int64_t fw, fh;
    float rate;
    int loop, ended, seeking;
    int64_t pending_seek; /* -1 = none; a scrub coalesces to the latest point */
    char err[256];
} ZAMedia;
static ZAMedia g_media[MEDIA_SLOTS_MAX];
static int g_media_init;

static ZAMedia *za_media_at(int64_t slot) {
    int i;
    if (!g_media_init) {
        for (i = 0; i < MEDIA_SLOTS_MAX; i++) {
            g_media[i].rate = 1.0f;
            g_media[i].pending_seek = -1;
        }
        g_media_init = 1;
#if TARGET_OS_IPHONE
        /* Play sound with the ring / silent switch off, like any video app. */
        [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback error:nil];
        [[AVAudioSession sharedInstance] setActive:YES error:nil];
#endif
    }
    if (slot <= 0 || slot >= MEDIA_SLOTS_MAX) return NULL;
    return &g_media[slot];
}

static void za_media_release(ZAMedia *m) {
    if (m->end_obs) {
        [[NSNotificationCenter defaultCenter] removeObserver:m->end_obs];
        [m->end_obs release];
        m->end_obs = nil;
    }
    if (m->player) {
        [m->player pause];
        [m->player replaceCurrentItemWithPlayerItem:nil];
        [m->player release];
        m->player = nil;
    }
    if (m->item && m->out) [m->item removeOutput:m->out];
    [m->out release];
    m->out = nil;
    [m->item release];
    m->item = nil;
    if (m->frame) CGImageRelease(m->frame);
    m->frame = NULL;
    m->fw = m->fh = 0;
    m->ended = 0;
    m->seeking = 0;
    m->pending_seek = -1;
    m->err[0] = 0;
}

static void za_media_seek_now(ZAMedia *m, int64_t ms);

static void za_media_seek_done(ZAMedia *m) {
    int64_t next = m->pending_seek;
    m->seeking = 0;
    if (next >= 0) {
        m->pending_seek = -1;
        za_media_seek_now(m, next);
    }
    za_redraw();
}

static void za_media_seek_now(ZAMedia *m, int64_t ms) {
    CMTime t = CMTimeMake(ms, 1000);
    if (!m->player) return;
    m->seeking = 1;
    m->ended = 0;
    [m->player seekToTime:t
          toleranceBefore:kCMTimeZero
           toleranceAfter:kCMTimeZero
        completionHandler:^(BOOL finished) {
        (void)finished;
        dispatch_async(dispatch_get_main_queue(), ^{
            za_media_seek_done(m);
        });
    }];
}

static NSURL *za_media_url(const char *src) {
    NSString *s = [NSString stringWithUTF8String:src];
    if (!s) return nil;
    if ([s hasPrefix:@"http://"] || [s hasPrefix:@"https://"] || [s hasPrefix:@"file://"])
        return [NSURL URLWithString:s];
    return [NSURL fileURLWithPath:[s stringByExpandingTildeInPath]];
}

static int64_t za_media_open(int64_t slot, const char *src) {
    ZAMedia *m = za_media_at(slot);
    NSURL *url;
    NSDictionary *attrs;
    if (!m) return 0;
    za_media_release(m);
    if (!src || !src[0]) return 0;
    url = za_media_url(src);
    if (!url) {
        snprintf(m->err, sizeof m->err, "Not a valid path or URL.");
        return 0;
    }
    m->item = [[AVPlayerItem alloc] initWithURL:url];
    attrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    m->out = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attrs];
    [m->item addOutput:m->out];
    m->player = [[AVPlayer alloc] initWithPlayerItem:m->item];
    m->player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
    m->end_obs = [[[NSNotificationCenter defaultCenter]
        addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                    object:m->item
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *n) {
        (void)n;
        if (m->loop && m->player) {
            [m->player seekToTime:kCMTimeZero];
            m->player.rate = m->rate;
        } else {
            m->ended = 1;
        }
        za_redraw();
    }] retain];
    return 1;
}

static void za_media_cmd(int64_t slot, int64_t cmd, int64_t value) {
    ZAMedia *m = za_media_at(slot);
    if (!m || !m->player) return;
    switch (cmd) {
    case 1: /* play: from the top again once it has ended */
        if (m->ended) {
            m->ended = 0;
            [m->player seekToTime:kCMTimeZero];
        }
        m->player.rate = m->rate;
        break;
    case 2:
        [m->player pause];
        break;
    case 3:
        if (value < 0) value = 0;
        if (m->seeking) m->pending_seek = value;
        else za_media_seek_now(m, value);
        break;
    case 4:
        m->rate = (float)value / 100.0f;
        if (m->rate < 0.0625f) m->rate = 0.0625f;
        if (m->rate > 16.0f) m->rate = 16.0f;
        if (@available(macOS 13.0, iOS 16.0, *)) m->player.defaultRate = m->rate;
        if (m->player.rate != 0.0f) m->player.rate = m->rate;
        break;
    case 5:
        m->player.volume = (float)(value < 0 ? 0 : (value > 100 ? 100 : value)) / 100.0f;
        break;
    case 6:
        m->player.muted = value ? YES : NO;
        break;
    case 7:
        m->loop = value ? 1 : 0;
        break;
    case 8:
        za_media_release(m);
        break;
    }
    za_redraw();
}

static int64_t za_media_ms(CMTime t) {
    if (!CMTIME_IS_NUMERIC(t)) return 0;
    return (int64_t)(CMTimeGetSeconds(t) * 1000.0 + 0.5);
}

static int64_t za_media_state(ZAMedia *m) {
    if (!m->player || !m->item) return m->err[0] ? 5 : 0;
    if (m->item.status == AVPlayerItemStatusFailed) {
        if (!m->err[0]) {
            NSError *e = m->item.error;
            NSString *why = e ? (e.localizedFailureReason ?: e.localizedDescription) : nil;
            snprintf(m->err, sizeof m->err, "%s",
                     why ? [why UTF8String] : "This file could not be played.");
        }
        return 5;
    }
    if (m->item.status != AVPlayerItemStatusReadyToPlay) return 1;
    if (m->ended) return 4;
    if (m->player.timeControlStatus == AVPlayerTimeControlStatusPlaying) return m->seeking ? 6 : 3;
    if (m->player.timeControlStatus == AVPlayerTimeControlStatusWaitingToPlayAtSpecifiedRate) return 6;
    return 2;
}

int64_t za_media_get(int64_t slot, int64_t what) {
    ZAMedia *m = za_media_at(slot);
    if (!m) return 0;
    if (what == 3) return za_media_state(m);
    if (!m->player || !m->item) return 0;
    switch (what) {
    case 1:
        return za_media_ms([m->player currentTime]);
    case 2:
        return m->item.status == AVPlayerItemStatusReadyToPlay ? za_media_ms(m->item.duration) : 0;
    case 4:
        return (int64_t)(m->item.presentationSize.width + 0.5);
    case 5:
        return (int64_t)(m->item.presentationSize.height + 0.5);
    case 6: {
        CMTime now = [m->player currentTime];
        for (NSValue *v in m->item.loadedTimeRanges) {
            CMTimeRange r = [v CMTimeRangeValue];
            if (CMTimeRangeContainsTime(r, now)) return za_media_ms(CMTimeRangeGetEnd(r));
        }
        return za_media_ms(now);
    }
    }
    return 0;
}

static void za_media_text(int64_t slot, int64_t what, char *out, int cap) {
    ZAMedia *m = za_media_at(slot);
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!m || what != 0) return;
    (void)za_media_state(m); /* fills `err` from a failed item */
    snprintf(out, (size_t)cap, "%s", m->err);
}

/* True while any player wants frames: playing, waiting for data, or seeking. */
int za_media_live(void) {
    int i;
    if (!g_media_init) return 0;
    for (i = 1; i < MEDIA_SLOTS_MAX; i++) {
        ZAMedia *m = &g_media[i];
        if (!m->player) continue;
        if (m->seeking || m->player.rate != 0.0f) return 1;
    }
    return 0;
}

/* The newest frame for `slot`, or the last one while no newer is due. */
static CGImageRef za_media_frame(int64_t slot) {
    ZAMedia *m = za_media_at(slot);
    CMTime t;
    CVPixelBufferRef pb;
    CGImageRef img = NULL;
    if (!m || !m->out) return NULL;
    t = [m->out itemTimeForHostTime:CACurrentMediaTime()];
    if ([m->out hasNewPixelBufferForItemTime:t]) {
        pb = [m->out copyPixelBufferForItemTime:t itemTimeForDisplay:NULL];
        if (pb) {
            if (VTCreateCGImageFromCVPixelBuffer(pb, NULL, &img) == noErr && img) {
                if (m->frame) CGImageRelease(m->frame);
                m->frame = img;
                m->fw = (int64_t)CGImageGetWidth(img);
                m->fh = (int64_t)CGImageGetHeight(img);
            }
            CVPixelBufferRelease(pb);
        }
    }
    return m->frame;
}

void za_media_draw(int64_t x, int64_t y, int64_t w, int64_t h, int64_t slot, int64_t radius,
                   int64_t alpha, int64_t fit) {
    CGImageRef img = za_media_frame(slot);
    ZAMedia *m = za_media_at(slot);
    int64_t iw, ih;
    if (!img || !m) return;
    /* Fit by the display size (anamorphic sources), not the coded size. */
    iw = (int64_t)(m->item.presentationSize.width + 0.5);
    ih = (int64_t)(m->item.presentationSize.height + 0.5);
    if (iw <= 0 || ih <= 0) {
        iw = m->fw;
        ih = m->fh;
    }
    za_draw_cgimage(img, CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h),
                    za_image_dest(x, y, w, h, iw, ih, fit), radius, alpha, 1);
}

int64_t za_media_slot_of(const char *src) {
    if (!src || strncmp(src, "media:", 6) != 0) return -1;
    return (int64_t)atoi(src + 6);
}


void za_media_table(ZeusMedia *t) {
    t->open = za_media_open;
    t->cmd = za_media_cmd;
    t->get = za_media_get;
    t->text = za_media_text;
}
