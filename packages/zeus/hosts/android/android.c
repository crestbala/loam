/* android.c — JNI + Android Canvas replay of the Zeus draw list (zeus/android).
 *
 * Android views are only the host: Activity / View / touch. Buttons, Material
 * widgets, and system colors are not used. Theme, layout, and paint stay in
 * Loam — the same fill / text / clip / SVG list as Cocoa, iOS, and Canvas2D.
 */
#include "zeus_rt.h"
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

int loam_app_main(void);

static JavaVM *g_jvm;
static jobject g_view;
static jmethodID m_fill, m_fill_a, m_fill_g, m_shadow, m_stroke, m_fill4, m_xform, m_text, m_text_rot, m_save, m_clip, m_restore, m_svg, m_image, m_image_size, m_pick, m_measure, m_invalidate;
static JNIEnv *g_env;
static jobject g_canvas;
static int g_started;
static volatile int g_ready;
static int g_view_w, g_view_h;

static JNIEnv *env_now(void) {
    JNIEnv *env = NULL;
    if (!g_jvm) return NULL;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == JNI_OK) return env;
    return NULL;
}

static void draw_fill(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                      int64_t radius) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_fill) return;
    (*env)->CallVoidMethod(env, g_view, m_fill, g_canvas, (jint)x, (jint)y, (jint)w, (jint)h,
                           (jint)(rgb & 0xFFFFFF), (jint)radius);
}

static void draw_fill_a(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                        int64_t radius, int64_t alpha) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_fill_a) return;
    (*env)->CallVoidMethod(env, g_view, m_fill_a, g_canvas, (jint)x, (jint)y, (jint)w, (jint)h,
                           (jint)(rgb & 0xFFFFFF), (jint)radius, (jint)alpha);
}

static void draw_fill_g(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t c0,
                        int64_t c1, int64_t axis, int64_t radius, int64_t alpha) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_fill_g) return;
    (*env)->CallVoidMethod(env, g_view, m_fill_g, g_canvas, (jint)x, (jint)y, (jint)w, (jint)h,
                           (jint)(c0 & 0xFFFFFF), (jint)(c1 & 0xFFFFFF), (jint)axis,
                           (jint)radius, (jint)alpha);
}

static void draw_shadow(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius,
                        int64_t rgb, int64_t alpha, int64_t blur, int64_t dx, int64_t dy) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_shadow) return;
    (*env)->CallVoidMethod(env, g_view, m_shadow, g_canvas, (jint)x, (jint)y, (jint)w, (jint)h,
                           (jint)radius, (jint)(rgb & 0xFFFFFF), (jint)alpha, (jint)blur,
                           (jint)dx, (jint)dy);
}

static void draw_stroke(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                        int64_t radius, int64_t width, int64_t alpha) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_stroke) return;
    (*env)->CallVoidMethod(env, g_view, m_stroke, g_canvas, (jint)x, (jint)y, (jint)w, (jint)h,
                           (jint)(rgb & 0xFFFFFF), (jint)radius, (jint)width, (jint)alpha);
}

static void draw_fill4(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t rgb,
                       int64_t alpha, int64_t tl, int64_t tr, int64_t br, int64_t bl) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_fill4) return;
    (*env)->CallVoidMethod(env, g_view, m_fill4, g_canvas, (jint)x, (jint)y, (jint)w, (jint)h,
                           (jint)(rgb & 0xFFFFFF), (jint)alpha, (jint)tl, (jint)tr, (jint)br,
                           (jint)bl);
}

static void draw_xform(void *ctx, int64_t dx, int64_t dy, int64_t scale, int64_t rot,
                       int64_t ox, int64_t oy) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_xform) return;
    (*env)->CallVoidMethod(env, g_view, m_xform, g_canvas, (jint)dx, (jint)dy, (jint)scale,
                           (jint)rot, (jint)ox, (jint)oy);
}

static void draw_text(void *ctx, int64_t x, int64_t y, const char *s, int64_t rgb, int64_t font) {
    JNIEnv *env = g_env;
    jstring js;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_text) return;
    js = (*env)->NewStringUTF(env, s ? s : "");
    (*env)->CallVoidMethod(env, g_view, m_text, g_canvas, (jint)x, (jint)y, js,
                           (jint)(rgb & 0xFFFFFF), (jint)font);
    (*env)->DeleteLocalRef(env, js);
}

static void draw_text_rot(void *ctx, int64_t x, int64_t y, const char *s, int64_t rgb,
                          int64_t font, int64_t deg) {
    JNIEnv *env = g_env;
    jstring js;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_text_rot) return;
    js = (*env)->NewStringUTF(env, s ? s : "");
    (*env)->CallVoidMethod(env, g_view, m_text_rot, g_canvas, (jint)x, (jint)y, js,
                           (jint)(rgb & 0xFFFFFF), (jint)font, (jint)deg);
    (*env)->DeleteLocalRef(env, js);
}

static void draw_save(void *ctx) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_save) return;
    (*env)->CallVoidMethod(env, g_view, m_save, g_canvas);
}

static void draw_clip(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_clip) return;
    (*env)->CallVoidMethod(env, g_view, m_clip, g_canvas, (jint)x, (jint)y, (jint)w, (jint)h,
                           (jint)radius);
}

static void draw_restore(void *ctx) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_restore) return;
    (*env)->CallVoidMethod(env, g_view, m_restore, g_canvas);
}

static void draw_svg(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, const char *markup,
                     int64_t rgb, int64_t alpha) {
    JNIEnv *env = g_env;
    jstring js;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_svg) return;
    js = (*env)->NewStringUTF(env, markup ? markup : "");
    (*env)->CallVoidMethod(env, g_view, m_svg, g_canvas, (jint)x, (jint)y, (jint)w, (jint)h, js,
                           (jint)(rgb & 0xFFFFFF), (jint)alpha);
    (*env)->DeleteLocalRef(env, js);
}

static void draw_image(void *ctx, int64_t x, int64_t y, int64_t w, int64_t h, const char *src,
                       int64_t radius, int64_t alpha, int64_t fit) {
    JNIEnv *env = g_env;
    jstring js;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_image) return;
    js = (*env)->NewStringUTF(env, src ? src : "");
    (*env)->CallVoidMethod(env, g_view, m_image, g_canvas, (jint)x, (jint)y, (jint)w, (jint)h, js,
                           (jint)radius, (jint)alpha, (jint)fit);
    (*env)->DeleteLocalRef(env, js);
}

static void android_measure(const char *s, int64_t px, int64_t *w, int64_t *h) {
    JNIEnv *env = g_env ? g_env : env_now();
    jstring js;
    jlong packed;
    if (!env || !g_view || !m_measure) {
        size_t n = s ? strlen(s) : 0;
        if (px < 8) px = 8;
        if (w) *w = (int64_t)n * (px * 6 / 10);
        if (h) *h = px + 4;
        return;
    }
    js = (*env)->NewStringUTF(env, s ? s : "");
    packed = (*env)->CallLongMethod(env, g_view, m_measure, js, (jint)px);
    (*env)->DeleteLocalRef(env, js);
    if (w) *w = (int64_t)(packed >> 32);
    if (h) *h = (int64_t)(packed & 0xffffffffL);
}

static void android_image_size(const char *src, int64_t *w, int64_t *h) {
    JNIEnv *env = g_env ? g_env : env_now();
    jstring js;
    jlong packed;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!env || !g_view || !m_image_size) return;
    js = (*env)->NewStringUTF(env, src ? src : "");
    packed = (*env)->CallLongMethod(env, g_view, m_image_size, js);
    (*env)->DeleteLocalRef(env, js);
    if (w) *w = (int64_t)(packed >> 32);
    if (h) *h = (int64_t)(packed & 0xffffffffL);
}

static void android_run(void) {
    /* Activity already created the view; layout happens on first paint. */
}

static void android_pick_image(char *out, int cap, int64_t *w, int64_t *h) {
    JNIEnv *env = g_env ? g_env : env_now();
    if (w) *w = 0;
    if (h) *h = 0;
    if (out && cap > 0) out[0] = 0;
    if (!env || !g_view || !m_pick) return;
    (*env)->CallVoidMethod(env, g_view, m_pick);
}

static void android_redraw(void) {
    JNIEnv *env = g_env ? g_env : env_now();
    if (!env || !g_view || !m_invalidate) return;
    (*env)->CallVoidMethod(env, g_view, m_invalidate);
}

static void bind_canvas(void) {
    ZeusDraw d;
    memset(&d, 0, sizeof d);
    d.fill = draw_fill;
    d.fill_a = draw_fill_a;
    d.fill_g = draw_fill_g;
    d.shadow = draw_shadow;
    d.stroke = draw_stroke;
    d.fill4 = draw_fill4;
    d.xform = draw_xform;
    d.text = draw_text;
    d.save = draw_save;
    d.clip = draw_clip;
    d.restore = draw_restore;
    d.svg = draw_svg;
    d.image = draw_image;
    zeus_bind_draw(d);
}

static void cache_methods(JNIEnv *env, jobject thiz) {
    jclass cls = (*env)->GetObjectClass(env, thiz);
    m_fill = (*env)->GetMethodID(env, cls, "jniFill", "(Landroid/graphics/Canvas;IIIIII)V");
    m_fill_a = (*env)->GetMethodID(env, cls, "jniFillA", "(Landroid/graphics/Canvas;IIIIIII)V");
    m_text = (*env)->GetMethodID(env, cls, "jniText",
                                 "(Landroid/graphics/Canvas;IILjava/lang/String;II)V");
    m_text_rot = (*env)->GetMethodID(env, cls, "jniTextRot",
                                     "(Landroid/graphics/Canvas;IILjava/lang/String;III)V");
    m_save = (*env)->GetMethodID(env, cls, "jniSave", "(Landroid/graphics/Canvas;)V");
    m_fill_g = (*env)->GetMethodID(env, cls, "jniFillG", "(Landroid/graphics/Canvas;IIIIIIIII)V");
    m_shadow = (*env)->GetMethodID(env, cls, "jniShadow", "(Landroid/graphics/Canvas;IIIIIIIIII)V");
    m_stroke = (*env)->GetMethodID(env, cls, "jniStroke", "(Landroid/graphics/Canvas;IIIIIIII)V");
    m_fill4 = (*env)->GetMethodID(env, cls, "jniFill4", "(Landroid/graphics/Canvas;IIIIIIIIII)V");
    m_xform = (*env)->GetMethodID(env, cls, "jniXform", "(Landroid/graphics/Canvas;IIIIII)V");
    m_clip = (*env)->GetMethodID(env, cls, "jniClip", "(Landroid/graphics/Canvas;IIIII)V");
    m_restore = (*env)->GetMethodID(env, cls, "jniRestore", "(Landroid/graphics/Canvas;)V");
    m_svg = (*env)->GetMethodID(env, cls, "jniSvg",
                                "(Landroid/graphics/Canvas;IIIILjava/lang/String;II)V");
    m_image = (*env)->GetMethodID(env, cls, "jniImage",
                                  "(Landroid/graphics/Canvas;IIIILjava/lang/String;III)V");
    m_pick = (*env)->GetMethodID(env, cls, "jniPick", "()V");
    m_measure = (*env)->GetMethodID(env, cls, "jniMeasure", "(Ljava/lang/String;I)J");
    m_image_size = (*env)->GetMethodID(env, cls, "jniImageSize", "(Ljava/lang/String;)J");
    m_invalidate = (*env)->GetMethodID(env, cls, "jniInvalidate", "()V");
    (*env)->DeleteLocalRef(env, cls);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativeStart(JNIEnv *env, jobject thiz) {
    if (g_started) return;
    g_started = 1;
    g_env = env;
    if (g_view) (*env)->DeleteGlobalRef(env, g_view);
    g_view = (*env)->NewGlobalRef(env, thiz);
    cache_methods(env, thiz);
    zeus_set_platform(android_run, android_measure, android_redraw);
    zeus_set_pick_image(android_pick_image);
    zeus_set_image_size(android_image_size);
    bind_canvas();
    /* HTTP in loam_app_main blocks. Java calls this off the UI thread. */
    loam_app_main();
    g_env = NULL;
    g_ready = 1;
    android_redraw();
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativeResize(JNIEnv *env, jobject thiz, jint w,
                                                                jint h, jint inset_t, jint inset_r,
                                                                jint inset_b, jint inset_l) {
    (void)thiz;
    g_env = env;
    if (w > 0) g_view_w = w;
    if (h > 0) g_view_h = h;
    zeus_set_window_size(w, h);
    zeus_set_insets(inset_t, inset_r, inset_b, inset_l);
    if (w > 0 && h > 0) zeus_layout(w, h);
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativeReducedMotion(JNIEnv *env, jobject thiz,
                                                                       jint on) {
    (void)env;
    (void)thiz;
    loam_zeus_engine_set_reduced_motion(on);
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativePaint(JNIEnv *env, jobject thiz,
                                                               jobject canvas) {
    ZeusDraw d;
    int w, h;
    static double last_ms;
    double now_ms;
    float dt;
    struct timespec ts;
    (void)thiz;
    if (!g_ready) return;
    g_env = env;
    g_canvas = canvas;
    /* Same as iOS: layout to the view, not theme.Page's 560×520. */
    w = g_view_w > 0 ? g_view_w : (int)zeus_window_width();
    h = g_view_h > 0 ? g_view_h : (int)zeus_window_height();
    /* Drive animations by elapsed time, not frame count, so a 60 Hz and a
       120 Hz device look the same. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    dt = 1.f / 60.f;
    if (last_ms > 0.0) dt = (float)((now_ms - last_ms) / 1000.0);
    last_ms = now_ms;
    if (dt < 0.001f) dt = 1.f / 60.f;
    if (dt > 0.064f) dt = 0.064f;
    zeus_layout(w, h);
    (void)zeus_step(dt);
    memset(&d, 0, sizeof d);
    d.fill = draw_fill;
    d.fill_a = draw_fill_a;
    d.fill_g = draw_fill_g;
    d.shadow = draw_shadow;
    d.stroke = draw_stroke;
    d.fill4 = draw_fill4;
    d.xform = draw_xform;
    d.text = draw_text;
    d.text_rot = draw_text_rot;
    d.save = draw_save;
    d.clip = draw_clip;
    d.restore = draw_restore;
    d.svg = draw_svg;
    d.image = draw_image;
    zeus_paint(NULL, d);
    g_canvas = NULL;
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativePointerDown(JNIEnv *env, jobject thiz,
                                                                     jint x, jint y) {
    (void)env;
    (void)thiz;
    (void)zeus_handle_click(x, y);
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativePointerMove(JNIEnv *env, jobject thiz,
                                                                     jint x, jint y, jint dx,
                                                                     jint dy) {
    int dirty;
    (void)env;
    (void)thiz;
    dirty = zeus_handle_drag(x, y);
    dirty |= zeus_handle_hover(x, y);
    if (dx || dy) dirty |= zeus_handle_scroll(x, y, dx, dy);
    (void)dirty;
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativePointerUp(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    zeus_handle_mouseup();
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativeKey(JNIEnv *env, jobject thiz, jint key,
                                                            jint mods) {
    (void)env;
    (void)thiz;
    (void)zeus_handle_key_ev((int)key, (int)mods);
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativePicked(JNIEnv *env, jobject thiz,
                                                               jstring src, jint w, jint h) {
    const char *s;
    (void)thiz;
    g_env = env;
    s = src ? (*env)->GetStringUTFChars(env, src, NULL) : NULL;
    zeus_picked_image(s ? s : "", w, h);
    if (s) (*env)->ReleaseStringUTFChars(env, src, s);
}
