/* maya_rt.h — handles + host/engine seam for `import "std:maya"`.
 *
 * Scene, tracer, and 2D map are Yuga (`std/maya.yuga`, `std/mayacore/`).
 * This header is Vec3/Mesh/Body (codegen skips those) plus plat_* (host)
 * and engine_* (generated C) that Cocoa calls.
 */
#ifndef MAYA_RT_H
#define MAYA_RT_H

#include <stdint.h>

#ifdef YUGA_RT_H
#else
#include "yuga_rt.h"
#endif

typedef struct {
    int64_t x, y, z;
} Vec3;

typedef struct {
    int64_t id;
} Mesh;

typedef struct {
    int64_t id;
} Body;

/* Host (maya_plat.c / maya_mac.m). Empty `fn plat_*` in std/maya.yuga. */
void yuga_maya_plat_run(void);
int64_t yuga_maya_plat_headless(void);
void yuga_maya_plat_window(yuga_str title, int64_t w, int64_t h);
int64_t yuga_maya_plat_now_ms(void);

/* Generated Yuga (`yuga_maya_engine_*`). */
int32_t yuga_maya_engine_is_map(void);
void yuga_maya_engine_set_viewport(int32_t w, int32_t h);
void yuga_maya_engine_fixed_update(int32_t dt_ms);
void yuga_maya_engine_frame(void);
void yuga_maya_engine_input_key(int32_t key);
void yuga_maya_engine_cam_orbit(int32_t dx, int32_t dy);
void yuga_maya_engine_cam_zoom(int32_t dy100);
void yuga_maya_engine_cam_pan(int32_t dx, int32_t dy);
void yuga_maya_engine_cam_reset(void);
void yuga_maya_engine_note_present(int32_t dt_ms);
int32_t yuga_maya_engine_sprite_count(void);
int32_t yuga_maya_engine_sprite_x(int32_t i);
int32_t yuga_maya_engine_sprite_y(int32_t i);
int32_t yuga_maya_engine_sprite_r(int32_t i);
int32_t yuga_maya_engine_sprite_orbit_r(int32_t i);
int32_t yuga_maya_engine_sprite_rgb(int32_t i);
int32_t yuga_maya_engine_sprite_is_sun(int32_t i);
yuga_str yuga_maya_engine_sprite_name(int32_t i);
yuga_str yuga_maya_engine_hud(void);
int32_t yuga_maya_engine_fb_w(void);
int32_t yuga_maya_engine_fb_h(void);
int32_t yuga_maya_engine_fb_at(int32_t i);

const uint8_t *maya_cpu_fb(int *w, int *h);

#endif
