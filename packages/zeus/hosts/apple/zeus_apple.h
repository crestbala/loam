/* zeus_apple.h — what the macOS and iOS hosts share.
 *
 * Both platforms paint with CoreGraphics, measure and draw text through
 * NSString, decode images with ImageIO and play media with AVFoundation, so
 * that code lives once, in this directory:
 *
 *   apple_draw.m   the ZeusDraw table, text measure, fonts, the image cache
 *   apple_media.m  the AVFoundation player behind `plat_media_*`
 *
 * A platform file (desktop/mac.m, ios/ios.m) keeps only what differs: the
 * window or view, input, the display path and the file pickers. It provides
 * the two hooks below and calls `za_register` once at load time.
 *
 * Coordinates are layout points in a y-down context, as the engine draws.
 */
#ifndef ZEUS_APPLE_H
#define ZEUS_APPLE_H

#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
typedef UIFont ZAFont;
typedef UIColor ZAColor;
#else
#import <Cocoa/Cocoa.h>
typedef NSFont ZAFont;
typedef NSColor ZAColor;
#endif
#include "zeus_rt.h"

/* --- provided by the platform file -------------------------------------- */

/* The context the engine is painting into right now (the view's, an owned
   buffer's, or a snapshot's). */
CGContextRef za_cg(void);
/* Ask for another frame (an image finished loading, a seek landed). */
void za_redraw(void);

/* --- shared: apple_draw.m ------------------------------------------------ */

/* The platform's colour for 0xRRGGBB at `a` (0..1). macOS keeps calibrated
   RGB, as AppKit always drew it; iOS uses UIKit's default. */
ZAColor *za_color(int64_t rgb, CGFloat a);
/* The system font, or the family `zeus_font_family()` names, at `px`. */
ZAFont *za_font(int64_t px);
void za_measure(const char *s, int64_t px, int64_t *w, int64_t *h);
/* Every paint callback, ready for `zeus_paint` / `zeus_bind_draw`. */
ZeusDraw za_draw_table(void);
void za_image_size(const char *src, int64_t *w, int64_t *h);
int za_load_font(const char *family, const char *src);
/* The rect an image of `iw` x `ih` fills inside (x, y, w, h) for `fit`
   (0 stretch, 1 contain, 2 cover, 3 none). */
CGRect za_image_dest(int64_t x, int64_t y, int64_t w, int64_t h, int64_t iw, int64_t ih,
                     int64_t fit);
/* Draw `img` into `dest`, clipped to `box` with `radius`, at `alpha` 0..255. */
void za_draw_cgimage(CGImageRef img, CGRect box, CGRect dest, int64_t radius, int64_t alpha,
                     int smooth);

/* --- shared: apple_media.m ----------------------------------------------- */

/* Fills the player half of a ZeusMedia table (open, cmd, get, text). The
   platform adds its pickers, voice tools and full screen, then binds it. */
void za_media_table(ZeusMedia *m);
/* 1 while any player wants frames (playing, buffering, seeking). */
int za_media_live(void);
/* The slot a `media:<n>` src names, or -1. */
int64_t za_media_slot_of(const char *src);
int64_t za_media_get(int64_t slot, int64_t what);
void za_media_draw(int64_t x, int64_t y, int64_t w, int64_t h, int64_t slot, int64_t radius,
                   int64_t alpha, int64_t fit);

#endif
