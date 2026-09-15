/* raygui_plat.c — raylib + raygui trampoline for `import "std:raygui"`.
 *
 * This is a host seam, not a runtime: the window and OS event pump belong to
 * raylib, the widget logic belongs to raygui (vendored header), and the API and
 * frame loop belong to `packages/raygui/std/raygui.yuga`. Everything here is a
 * thin conversion (NUL-terminate a `yuga_str`, mirror raygui's `bool *` state
 * as `int *`) plus the memory probe a RAM test reads. See docs/boundary.md.
 *
 * Headless (RAYGUI_HEADLESS / ZEUS_HEADLESS / YUGA_HEADLESS) opens no window:
 * the frame calls are inert and `should_close` ends the loop after
 * RAYGUI_FRAMES frames (default 3) so `make test` exercises the control code.
 */
#define RAYGUI_IMPLEMENTATION
#include "raygui.h" /* pulls in raylib.h */

#include "raygui_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

static int inited;   /* window opened (never in headless) */
static int frames;   /* frames drawn this run */
static int headless; /* computed once in init */

static int env_is(const char *name) {
    const char *v = getenv(name);
    return v && v[0] == '1';
}

static const char *rg_cstr(yuga_str s, char *buf, size_t cap) {
    size_t n = s.len < 0 ? 0 : (size_t)s.len;
    if (n >= cap) n = cap - 1;
    if (n > 0 && s.ptr) memcpy(buf, s.ptr, n);
    buf[n] = 0;
    return buf;
}

/* Headless frame cap: enough for the first frames to lay out state. */
static int frame_cap(void) {
    const char *v = getenv("RAYGUI_FRAMES");
    int n = v && v[0] ? atoi(v) : 0;
    return n > 0 ? n : 3;
}

int32_t yuga_raygui_plat_headless(void) {
    return headless;
}

void yuga_raygui_plat_init(yuga_str title, int32_t w, int32_t h, int32_t fps) {
    char tb[256];
    headless = env_is("RAYGUI_HEADLESS") || env_is("ZEUS_HEADLESS") || env_is("YUGA_HEADLESS");
#if !defined(__APPLE__) && !defined(__linux__)
    headless = 1;
#endif
    frames = 0;
    if (headless) return;
    if (w <= 0) w = 800;
    if (h <= 0) h = 600;
    InitWindow((int)w, (int)h, rg_cstr(title, tb, sizeof tb));
    if (fps > 0) SetTargetFPS((int)fps);
    inited = 1;
}

int32_t yuga_raygui_plat_should_close(void) {
    if (headless) {
        frames++;
        return frames > frame_cap() ? 1 : 0;
    }
    if (!inited) return 1;
    return WindowShouldClose() ? 1 : 0;
}

void yuga_raygui_plat_begin(int32_t bg) {
    if (!inited) return;
    BeginDrawing();
    ClearBackground(GetColor((unsigned int)bg));
}

void yuga_raygui_plat_end(void) {
    if (!inited) return;
    EndDrawing();
}

void yuga_raygui_plat_close(void) {
    if (!inited) return;
    CloseWindow();
    inited = 0;
}

int32_t yuga_raygui_plat_fps(void) {
    return inited ? GetFPS() : 0;
}

int32_t yuga_raygui_plat_frame_ms(void) {
    return inited ? (int32_t)(GetFrameTime() * 1000.0f) : 0;
}

int32_t yuga_raygui_plat_screen_w(void) {
    return inited ? GetScreenWidth() : 0;
}

int32_t yuga_raygui_plat_screen_h(void) {
    return inited ? GetScreenHeight() : 0;
}

void yuga_raygui_plat_set_style(int32_t control, int32_t prop, int32_t value) {
    GuiSetStyle((int)control, (int)prop, (int)value);
}

void yuga_raygui_plat_load_style_default(void) {
    GuiLoadStyleDefault();
}

/* Host process memory in KB: Apple phys_footprint (the number Xcode's memory
   gauge shows; RSS overstates iOS/desktop scope) or the Linux/Android peak
   resident set. This is the value a RAM test watches. */
int32_t yuga_raygui_plat_ram_kb(void) {
#if defined(__APPLE__)
    struct task_vm_info info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return 0;
    return (int32_t)(info.phys_footprint / 1024);
#elif defined(__linux__)
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return (int32_t)ru.ru_maxrss;
#else
    return 0;
#endif
}

/* ---- controls -----------------------------------------------------------
   raygui is pure immediate mode: bounds in, state pointer in/out, and the
   return is "was this activated". Headless runs skip the draw (no context);
   state is left as the caller passed it. */

static Rectangle rg_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    Rectangle r;
    r.x = (float)x;
    r.y = (float)y;
    r.width = (float)w;
    r.height = (float)h;
    return r;
}

int32_t yuga_raygui_plat_label(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text) {
    char b[512];
    if (!inited) return 0;
    return GuiLabel(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b));
}

int32_t yuga_raygui_plat_panel(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text) {
    char b[512];
    if (!inited) return 0;
    return GuiPanel(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b));
}

int32_t yuga_raygui_plat_group_box(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text) {
    char b[512];
    if (!inited) return 0;
    return GuiGroupBox(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b));
}

int32_t yuga_raygui_plat_line(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text) {
    char b[512];
    if (!inited) return 0;
    return GuiLine(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b));
}

int32_t yuga_raygui_plat_button(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text) {
    char b[512];
    if (!inited) return 0;
    return GuiButton(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b));
}

int32_t yuga_raygui_plat_toggle(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                                int32_t *active) {
    char b[512];
    bool on;
    int r;
    if (!inited) return 0;
    on = active && *active != 0;
    r = GuiToggle(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b), &on);
    if (active) *active = on ? 1 : 0;
    return r;
}

int32_t yuga_raygui_plat_checkbox(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                                  int32_t *checked) {
    char b[512];
    bool on;
    int r;
    if (!inited) return 0;
    on = checked && *checked != 0;
    r = GuiCheckBox(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b), &on);
    if (checked) *checked = on ? 1 : 0;
    return r;
}

int32_t yuga_raygui_plat_slider(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str left,
                                yuga_str right, float *value, float lo, float hi) {
    char lb[128], rb[128];
    float v;
    int r;
    if (!inited || !value) return 0;
    v = *value;
    r = GuiSlider(rg_rect(x, y, w, h), rg_cstr(left, lb, sizeof lb),
                  rg_cstr(right, rb, sizeof rb), &v, lo, hi);
    *value = v;
    return r;
}

int32_t yuga_raygui_plat_progress(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str left,
                                  yuga_str right, float *value, float lo, float hi) {
    char lb[128], rb[128];
    float v;
    int r;
    if (!inited || !value) return 0;
    v = *value;
    r = GuiProgressBar(rg_rect(x, y, w, h), rg_cstr(left, lb, sizeof lb),
                       rg_cstr(right, rb, sizeof rb), &v, lo, hi);
    *value = v;
    return r;
}

int32_t yuga_raygui_plat_spinner(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                                 int32_t *value, int32_t lo, int32_t hi, int32_t edit) {
    char b[256];
    int v;
    int r;
    if (!inited || !value) return 0;
    v = *value;
    r = GuiSpinner(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b), &v, (int)lo, (int)hi, edit != 0);
    *value = v;
    return r;
}

int32_t yuga_raygui_plat_value_box(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                                   int32_t *value, int32_t lo, int32_t hi, int32_t edit) {
    char b[256];
    int v;
    int r;
    if (!inited || !value) return 0;
    v = *value;
    r = GuiValueBox(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b), &v, (int)lo, (int)hi,
                    edit != 0);
    *value = v;
    return r;
}

int32_t yuga_raygui_plat_combo(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                               int32_t *active) {
    char b[512];
    int a;
    int r;
    if (!inited || !active) return 0;
    a = *active;
    r = GuiComboBox(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b), &a);
    *active = a;
    return r;
}

int32_t yuga_raygui_plat_listview(int32_t x, int32_t y, int32_t w, int32_t h, yuga_str text,
                                  int32_t *scroll, int32_t *active) {
    char b[1024];
    int s = scroll ? *scroll : 0;
    int a = active ? *active : -1;
    int r;
    if (!inited) return 0;
    r = GuiListView(rg_rect(x, y, w, h), rg_cstr(text, b, sizeof b), &s, &a);
    if (scroll) *scroll = s;
    if (active) *active = a;
    return r;
}
