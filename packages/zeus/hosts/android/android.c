/* android.c — JNI + Android Canvas replay of the Zeus draw list (zeus/android).
 *
 * Android views are only the host: Activity / View / touch. Buttons, Material
 * widgets, and system colors are not used. Theme, layout, and paint stay in
 * Loam — the same fill / text / clip / SVG list as Cocoa, iOS, and Canvas2D.
 */
#include "zeus_rt.h"
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int loam_app_main(void);

static JavaVM *g_jvm;
static int tls_ready(JNIEnv *env);
static jobject g_view;
static jmethodID m_fill, m_fill_a, m_fill_g, m_shadow, m_stroke, m_fill4, m_xform, m_text, m_text_rot, m_save, m_alpha, m_clip, m_restore, m_path, m_image, m_image_size, m_pick, m_measure, m_invalidate;
static JNIEnv *g_env;
static jobject g_canvas;
static int g_started;
static volatile int g_ready;
static int g_view_w, g_view_h;

/* Android drops stdout, and `std:log` and `fmt.println` write there. The write
   end of this pipe becomes stdout, and each line reaches logcat as tag `zeus`,
   where `zeli serve` follows it. */
static void *stdout_pump(void *arg) {
    int fd = (int)(intptr_t)arg;
    char line[1024];
    size_t n = 0;
    for (;;) {
        char c;
        if (read(fd, &c, 1) <= 0) return NULL;
        if (c != '\n') line[n++] = c;
        if (c != '\n' && n < sizeof line - 1) continue;
        line[n] = 0;
        __android_log_write(ANDROID_LOG_INFO, "zeus", line);
        n = 0;
    }
}

static void stdout_to_logcat(void) {
    int fds[2];
    pthread_t t;
    if (pipe(fds) != 0) return;
    setvbuf(stdout, NULL, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    if (pthread_create(&t, NULL, stdout_pump, (void *)(intptr_t)fds[0]) == 0) pthread_detach(t);
}

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

static void draw_alpha(void *ctx, int64_t a) {
    JNIEnv *env = g_env;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_alpha) return;
    (*env)->CallVoidMethod(env, g_view, m_alpha, g_canvas, (jint)a);
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

static void draw_path(void *ctx, const int32_t *cmds, int64_t n, int64_t rgb, int64_t alpha,
                      int64_t width, int64_t style) {
    JNIEnv *env = g_env;
    jintArray arr;
    (void)ctx;
    if (!env || !g_view || !g_canvas || !m_path || n <= 0) return;
    arr = (*env)->NewIntArray(env, (jsize)n);
    if (!arr) return;
    (*env)->SetIntArrayRegion(env, arr, 0, (jsize)n, (const jint *)cmds);
    (*env)->CallVoidMethod(env, g_view, m_path, g_canvas, arr, (jint)(rgb & 0xFFFFFF),
                           (jint)alpha, (jint)width, (jint)style);
    (*env)->DeleteLocalRef(env, arr);
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
    d.alpha = draw_alpha;
    d.clip = draw_clip;
    d.restore = draw_restore;
    d.path = draw_path;
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
    m_alpha = (*env)->GetMethodID(env, cls, "jniAlpha", "(Landroid/graphics/Canvas;I)V");
    m_fill_g = (*env)->GetMethodID(env, cls, "jniFillG", "(Landroid/graphics/Canvas;IIIIIIIII)V");
    m_shadow = (*env)->GetMethodID(env, cls, "jniShadow", "(Landroid/graphics/Canvas;IIIIIIIIII)V");
    m_stroke = (*env)->GetMethodID(env, cls, "jniStroke", "(Landroid/graphics/Canvas;IIIIIIII)V");
    m_fill4 = (*env)->GetMethodID(env, cls, "jniFill4", "(Landroid/graphics/Canvas;IIIIIIIIII)V");
    m_xform = (*env)->GetMethodID(env, cls, "jniXform", "(Landroid/graphics/Canvas;IIIIII)V");
    m_clip = (*env)->GetMethodID(env, cls, "jniClip", "(Landroid/graphics/Canvas;IIIII)V");
    m_restore = (*env)->GetMethodID(env, cls, "jniRestore", "(Landroid/graphics/Canvas;)V");
    m_path = (*env)->GetMethodID(env, cls, "jniPath", "(Landroid/graphics/Canvas;[IIIII)V");
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
    stdout_to_logcat();
    g_env = env;
    /* The app classloader can see LoamTls. A later native thread reuses the global ref. */
    tls_ready(env);
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
    if (w > 0 && h > 0) loam_zeus_engine_layout(w, h);
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
    double dt_ms;
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
    dt_ms = 1000.0 / 60.0;
    if (last_ms > 0.0) dt_ms = now_ms - last_ms;
    last_ms = now_ms;
    if (dt_ms < 1.0) dt_ms = 1000.0 / 60.0;
    if (dt_ms > 64.0) dt_ms = 64.0;
    loam_zeus_engine_layout(w, h);
    (void)loam_zeus_engine_step_dt((int32_t)(dt_ms + 0.5));
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
    d.alpha = draw_alpha;
    d.clip = draw_clip;
    d.restore = draw_restore;
    d.path = draw_path;
    d.image = draw_image;
    zeus_paint(NULL, d);
    g_canvas = NULL;
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativePointerDown(JNIEnv *env, jobject thiz,
                                                                     jint x, jint y) {
    (void)env;
    (void)thiz;
    (void)loam_zeus_engine_click(x, y);
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativePointerMove(JNIEnv *env, jobject thiz,
                                                                     jint x, jint y, jint dx,
                                                                     jint dy) {
    int dirty;
    (void)env;
    (void)thiz;
    dirty = loam_zeus_engine_drag(x, y);
    dirty |= loam_zeus_engine_hover(x, y);
    if (dx || dy) dirty |= loam_zeus_engine_scroll(x, y, dx, dy);
    (void)dirty;
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativePointerUp(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    loam_zeus_engine_mouseup();
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativeKey(JNIEnv *env, jobject thiz, jint key,
                                                            jint mods) {
    (void)env;
    (void)thiz;
    (void)loam_zeus_engine_key_ev(key, mods);
}

JNIEXPORT void JNICALL Java_com_loam_zeus_ZeusView_nativePicked(JNIEnv *env, jobject thiz,
                                                               jstring src, jint w, jint h) {
    const char *s;
    (void)thiz;
    g_env = env;
    s = src ? (*env)->GetStringUTFChars(env, src, NULL) : NULL;
    loam_zeus_engine_picked_image((loam_str){s ? s : "", s ? (int64_t)strlen(s) : 0, 0}, w, h);
    if (s) (*env)->ReleaseStringUTFChars(env, src, s);
}

/* TLS for std:net. Loam cannot call through the JNI function table, so these
   copy between caller-owned buffers and LoamTls. The symbols are not JNI
   exports; app.c in this library calls them. Handles are 1..8. */
static jclass g_tls_class;
static jmethodID m_tls_open, m_tls_status, m_tls_wrap, m_tls_unwrap, m_tls_pending, m_tls_close, m_tls_free;
static jmethodID m_tls_consumed, m_tls_produced, m_tls_staged;
static int g_tls_ready;
static int g_tls_logged;

static void tls_log_once(const char *msg) {
    if (g_tls_logged) return;
    g_tls_logged = 1;
    __android_log_write(ANDROID_LOG_ERROR, "zeus", msg);
}

static int tls_bind(JNIEnv *env, jclass cls) {
    m_tls_open = (*env)->GetStaticMethodID(env, cls, "open", "(Ljava/lang/String;I)I");
    m_tls_status = (*env)->GetStaticMethodID(env, cls, "status", "(I)I");
    m_tls_wrap = (*env)->GetStaticMethodID(env, cls, "wrap", "(I[BI[B)I");
    m_tls_unwrap = (*env)->GetStaticMethodID(env, cls, "unwrap", "(I[BI[B)I");
    m_tls_pending = (*env)->GetStaticMethodID(env, cls, "pending", "(I)I");
    m_tls_close = (*env)->GetStaticMethodID(env, cls, "close", "(I)I");
    m_tls_free = (*env)->GetStaticMethodID(env, cls, "free", "(I)V");
    m_tls_consumed = (*env)->GetStaticMethodID(env, cls, "takeConsumed", "()I");
    m_tls_produced = (*env)->GetStaticMethodID(env, cls, "takeProduced", "()I");
    m_tls_staged = (*env)->GetStaticMethodID(env, cls, "takeStaged", "()I");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    return m_tls_open && m_tls_status && m_tls_wrap && m_tls_unwrap && m_tls_pending &&
           m_tls_close && m_tls_free && m_tls_consumed && m_tls_produced && m_tls_staged;
}

static int tls_ready(JNIEnv *env) {
    jclass local;
    if (g_tls_ready) return 1;
    if (!env) return 0;
    local = (*env)->FindClass(env, "com/loam/zeus/LoamTls");
    if ((*env)->ExceptionCheck(env) || !local) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        tls_log_once("loam tls: LoamTls class not found");
        return 0;
    }
    g_tls_class = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (!g_tls_class || !tls_bind(env, g_tls_class)) {
        if (g_tls_class) (*env)->DeleteGlobalRef(env, g_tls_class);
        g_tls_class = NULL;
        tls_log_once("loam tls: LoamTls methods missing");
        return 0;
    }
    g_tls_ready = 1;
    return 1;
}

static void tls_drop(JNIEnv *env, jbyteArray a, jbyteArray b) {
    if (a) (*env)->DeleteLocalRef(env, a);
    if (b) (*env)->DeleteLocalRef(env, b);
}

/* counts[0] consumed, counts[1] produced, counts[2] still staged. */
static int32_t tls_io(JNIEnv *env, jmethodID method, int32_t handle, void *in, int32_t in_len,
                      void *out, int32_t out_cap, void *counts) {
    jbyteArray jin, jout;
    int32_t rc, consumed, produced, staged;
    int32_t *got;
    if (!env || !g_tls_ready || in_len < 0 || out_cap < 0) return -1;
    if ((in_len > 0 && !in) || (out_cap > 0 && !out)) return -1;
    jin = (*env)->NewByteArray(env, (jsize)in_len);
    jout = (*env)->NewByteArray(env, (jsize)out_cap);
    if ((*env)->ExceptionCheck(env) || !jin || !jout) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        tls_drop(env, jin, jout);
        return -1;
    }
    if (in_len > 0) {
        (*env)->SetByteArrayRegion(env, jin, 0, (jsize)in_len, (const jbyte *)in);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            tls_drop(env, jin, jout);
            return -1;
        }
    }
    rc = (*env)->CallStaticIntMethod(env, g_tls_class, method, (jint)handle, jin, (jint)in_len, jout);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        rc = -1;
    }
    consumed = 0;
    produced = 0;
    staged = 0;
    if (rc >= 0) {
        consumed = (*env)->CallStaticIntMethod(env, g_tls_class, m_tls_consumed);
        produced = (*env)->CallStaticIntMethod(env, g_tls_class, m_tls_produced);
        staged = (*env)->CallStaticIntMethod(env, g_tls_class, m_tls_staged);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            rc = -1;
        }
    }
    if (rc >= 0 && (consumed < 0 || produced < 0 || staged < 0 || consumed > in_len || produced > out_cap)) {
        rc = -1;
    }
    if (rc >= 0 && produced > 0) {
        (*env)->GetByteArrayRegion(env, jout, 0, (jsize)produced, (jbyte *)out);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            rc = -1;
        }
    }
    if (rc >= 0 && counts) {
        got = counts;
        got[0] = consumed;
        got[1] = produced;
        got[2] = staged;
    }
    tls_drop(env, jin, jout);
    return rc;
}

int32_t loam_android_tls_open(const char *host, int32_t port) {
    JNIEnv *env = env_now();
    jstring jhost;
    int32_t handle;
    if (!env || !tls_ready(env) || !host || !host[0]) return -1;
    jhost = (*env)->NewStringUTF(env, host);
    if ((*env)->ExceptionCheck(env) || !jhost) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        if (jhost) (*env)->DeleteLocalRef(env, jhost);
        return -1;
    }
    handle = (*env)->CallStaticIntMethod(env, g_tls_class, m_tls_open, jhost, (jint)port);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        handle = -1;
    }
    (*env)->DeleteLocalRef(env, jhost);
    return handle;
}

int32_t loam_android_tls_status(int32_t handle) {
    JNIEnv *env = env_now();
    int32_t rc;
    if (!env || !tls_ready(env)) return -1;
    rc = (*env)->CallStaticIntMethod(env, g_tls_class, m_tls_status, (jint)handle);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return -1;
    }
    return rc;
}

int32_t loam_android_tls_wrap(int32_t handle, void *in, int32_t in_len, void *out, int32_t out_cap,
                              void *counts) {
    JNIEnv *env = env_now();
    if (!env || !tls_ready(env)) return -1;
    return tls_io(env, m_tls_wrap, handle, in, in_len, out, out_cap, counts);
}

int32_t loam_android_tls_unwrap(int32_t handle, void *in, int32_t in_len, void *out, int32_t out_cap,
                                void *counts) {
    JNIEnv *env = env_now();
    if (!env || !tls_ready(env)) return -1;
    return tls_io(env, m_tls_unwrap, handle, in, in_len, out, out_cap, counts);
}

int32_t loam_android_tls_pending(int32_t handle) {
    JNIEnv *env = env_now();
    int32_t rc;
    if (!env || !tls_ready(env)) return 0;
    rc = (*env)->CallStaticIntMethod(env, g_tls_class, m_tls_pending, (jint)handle);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    return rc;
}

int32_t loam_android_tls_close(int32_t handle) {
    JNIEnv *env = env_now();
    int32_t rc;
    if (!env || !tls_ready(env)) return -1;
    rc = (*env)->CallStaticIntMethod(env, g_tls_class, m_tls_close, (jint)handle);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return -1;
    }
    return rc;
}

void loam_android_tls_free(int32_t handle) {
    JNIEnv *env = env_now();
    if (!env || !tls_ready(env)) return;
    (*env)->CallStaticVoidMethod(env, g_tls_class, m_tls_free, (jint)handle);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}
