/* raygui_rt.h — declarations for `std/raygui.yuga` (`yuga_raygui_plat_*`).
 *
 * The immediate-mode widget logic is raygui (vendored: packages/raygui/vendor/
 * raygui.h); the module is the Yuga API and raygui_plat.c is the raylib +
 * raygui trampoline. This header only declares what the generated C calls —
 * see docs/boundary.md.
 */
#ifndef RAYGUI_RT_H
#define RAYGUI_RT_H

#ifdef YUGA_RT_H
#else
#include "yuga_rt.h"
#endif

/* Window + frame loop (raylib owns the OS window and event pump). */
void yuga_raygui_plat_init(yuga_str title, int32_t w, int32_t h, int32_t fps);
int32_t yuga_raygui_plat_headless(void);
int32_t yuga_raygui_plat_should_close(void);
void yuga_raygui_plat_begin(int32_t bg);
void yuga_raygui_plat_end(void);
void yuga_raygui_plat_close(void);
int32_t yuga_raygui_plat_ram_kb(void);
int32_t yuga_raygui_plat_fps(void);
int32_t yuga_raygui_plat_frame_ms(void);
int32_t yuga_raygui_plat_screen_w(void);
int32_t yuga_raygui_plat_screen_h(void);
void yuga_raygui_plat_set_style(int32_t control, int32_t prop, int32_t value);
void yuga_raygui_plat_load_style_default(void);

/* Controls. `int` mirrors raygui's `bool` out-params as 1/0. */
int32_t yuga_raygui_plat_label(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text);
int32_t yuga_raygui_plat_panel(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text);
int32_t yuga_raygui_plat_group_box(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text);
int32_t yuga_raygui_plat_line(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text);
int32_t yuga_raygui_plat_button(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text);
int32_t yuga_raygui_plat_toggle(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                                int32_t *active);
int32_t yuga_raygui_plat_checkbox(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                                  int32_t *checked);
int32_t yuga_raygui_plat_slider(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str left,
                                yuga_str right, float *value, float lo, float hi);
int32_t yuga_raygui_plat_progress(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str left,
                                  yuga_str right, float *value, float lo, float hi);
int32_t yuga_raygui_plat_spinner(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                                 int32_t *value, int32_t lo, int32_t hi, int32_t edit);
int32_t yuga_raygui_plat_value_box(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                                   int32_t *value, int32_t lo, int32_t hi, int32_t edit);
int32_t yuga_raygui_plat_combo(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                               int32_t *active);
int32_t yuga_raygui_plat_listview(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                                  int32_t *scroll, int32_t *active);

#endif
