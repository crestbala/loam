/**
 * driver.c — `loam` CLI: check, emit C, or compile+link with cc.
 *
 * Default: write a binary next to the source under build/. Generated C is
 * The frontend is cheap (~50ms for a zeus app); wall time is `cc` on
 * ~800KB of generated C plus Cocoa. Runtime .c/.m files compile once into
 * `runtime/.obj/` and are reused. `ZEUS_HEADLESS=1` skips Cocoa entirely.
 * Set `LOAM_TIME=1` to print check / codegen / cc timings on stderr.
 *
 * zeus links runtime/zeus_plat.c + zeus_key.c and a host:
 *   zeus/desktop/mac.m     Cocoa
 *   zeus/ios/ios.m         UIKit Simulator (paint only)
 *   zeus/android/android.c JNI + Canvas (paint only)
 *   zeus/web/wasm.c        Canvas2D
 */
#include "compile.h"
#include "codegen_c.h"
#include "ext.h"
#include "ir.h"
#include "diagnostics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#ifndef LOAM_RT_PATH
#define LOAM_RT_PATH "runtime/loam_rt.h"
#endif
#ifndef LOAM_RUNTIME_DIR
#define LOAM_RUNTIME_DIR "runtime"
#endif
#ifndef LOAM_ZEUS_DIR
#define LOAM_ZEUS_DIR "zeus"
#endif
#ifndef LOAM_RAYGUI_DIR
#define LOAM_RAYGUI_DIR "raygui"
#endif

/** Directory containing `path`, or ".". */
static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return loam_dup(".");
    if (slash == path) return loam_dup("/");
    return loam_dupn(path, (size_t)(slash - path));
}

/** File stem without directory or source extension. */
static char *stem_of(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return loam_dupn(base, loam_stem_len(base, strlen(base)));
}

/* raylib compile+link flags for the raygui host. `RAYLIB_PREFIX` wins, then
   pkg-config, then the usual Homebrew prefixes. Empty when raylib is missing —
   the link then fails with the compiler's own message. */
static void raygui_raylib_flags(char *out, size_t n) {
    const char *p = getenv("RAYLIB_PREFIX");
    char buf[512];
    FILE *f;
    size_t got;
    out[0] = 0;
    if (p && p[0]) {
        snprintf(out, n, "-I\"%s/include\" -L\"%s/lib\" -lraylib", p, p);
        return;
    }
    f = popen("pkg-config --cflags --libs raylib 2>/dev/null", "r");
    if (f) {
        got = fread(buf, 1, sizeof buf - 1, f);
        buf[got] = 0;
        if (pclose(f) == 0 && got > 0) {
            while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) buf[--got] = 0;
            snprintf(out, n, "%s", buf);
            return;
        }
    }
    if (access("/opt/homebrew/opt/raylib/include/raylib.h", R_OK) == 0)
        snprintf(out, n, "-I/opt/homebrew/opt/raylib/include -L/opt/homebrew/opt/raylib/lib -lraylib");
    else if (access("/usr/local/opt/raylib/include/raylib.h", R_OK) == 0)
        snprintf(out, n, "-I/usr/local/opt/raylib/include -L/usr/local/opt/raylib/lib -lraylib");
}

/** mkdir -p. 0 on success. */
static int mkdir_p(const char *dir) {
    if (!dir || !dir[0] || strcmp(dir, ".") == 0) return 0;
    char *copy = loam_dup(dir);
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return 1;
            }
            *p = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        free(copy);
        return 1;
    }
    free(copy);
    return 0;
}

static int ensure_parent_dir(const char *file) {
    char *d = dir_of(file);
    int rc = mkdir_p(d);
    free(d);
    return rc;
}

/** Non-empty env var other than "0". */
static int env_on(const char *name) {
    const char *v = getenv(name);
    if (!v || !v[0]) return 0;
    if (strcmp(v, "0") == 0) return 0;
    return 1;
}

static int want_headless(void) {
    return env_on("ZEUS_HEADLESS") || env_on("LOAM_HEADLESS");
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/** 1 if `src` is missing or newer than `dst` (or `dst` is missing). */
static int src_newer(const char *src, const char *dst) {
    struct stat ss, ds;
    if (stat(dst, &ds) != 0) return 1;
    if (stat(src, &ss) != 0) return 1;
    return ss.st_mtime > ds.st_mtime;
}

static int any_src_newer(const char *dst, const char **srcs, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (src_newer(srcs[i], dst)) return 1;
    }
    return 0;
}

/** Compile `src` to `obj` if it is stale. `extra` is extra cc flags (may be ""). */
static int ensure_obj(const char *src, const char *obj, const char *extra,
                      const char **deps, int ndeps) {
    if (!any_src_newer(obj, deps, ndeps)) return 0;
    if (ensure_parent_dir(obj) != 0) return 1;
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "cc -std=gnu99 -O1 -c -I\"%s\" %s \"%s\" -o \"%s\"",
             LOAM_RUNTIME_DIR, extra ? extra : "", src, obj);
    if (env_on("LOAM_TIME")) fprintf(stderr, "loam: cc %s\n", src);
    return system(cmd) != 0;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in, *out;
    char buf[4096];
    size_t n;
    if (ensure_parent_dir(dst) != 0) return 1;
    in = fopen(src, "rb");
    if (!in) return 1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return 0;
}

static int wasm_cc_ok(const char *cc) {
    char cmd[1024];
    if (!cc || !cc[0]) return 0;
    snprintf(cmd, sizeof cmd,
             "echo 'void loam_wasm_probe(void){}' | \"%s\" --target=wasm32 -c -x c - "
             "-o /dev/null >/dev/null 2>&1",
             cc);
    return system(cmd) == 0;
}

static const char *find_wasm_cc(char *buf, size_t n) {
    const char *env = getenv("LOAM_WASM_CC");
    const char *cands[] = {
        "clang",
        "/opt/homebrew/opt/llvm/bin/clang",
        "/usr/local/opt/llvm/bin/clang",
        "emcc",
        NULL,
    };
    int i;
    if (env && env[0] && wasm_cc_ok(env)) {
        snprintf(buf, n, "%s", env);
        return buf;
    }
    for (i = 0; cands[i]; i++) {
        if (!wasm_cc_ok(cands[i])) continue;
        snprintf(buf, n, "%s", cands[i]);
        return buf;
    }
    return NULL;
}

static int ios_sdk_path(char *buf, size_t n) {
    FILE *f = popen("xcrun --sdk iphonesimulator --show-sdk-path 2>/dev/null", "r");
    size_t len;
    if (!f) return 1;
    if (!fgets(buf, (int)n, f)) {
        pclose(f);
        return 1;
    }
    pclose(f);
    len = strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
    return len ? 0 : 1;
}

static int write_ios_plist(const char *path, const char *exe, const char *bid) {
    FILE *f;
    if (ensure_parent_dir(path) != 0) return 1;
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "  <key>CFBundleExecutable</key><string>%s</string>\n"
            "  <key>CFBundleIdentifier</key><string>%s</string>\n"
            "  <key>CFBundleName</key><string>%s</string>\n"
            "  <key>CFBundlePackageType</key><string>APPL</string>\n"
            "  <key>CFBundleVersion</key><string>1</string>\n"
            "  <key>CFBundleShortVersionString</key><string>1.0</string>\n"
            "  <key>MinimumOSVersion</key><string>16.0</string>\n"
            "  <key>LSRequiresIPhoneOS</key><true/>\n"
            "  <key>UIDeviceFamily</key><array><integer>1</integer></array>\n"
            "  <key>CFBundleSupportedPlatforms</key>"
            "<array><string>iPhoneSimulator</string></array>\n"
            "  <key>UILaunchScreen</key><dict/>\n"
            "  <key>UIStatusBarHidden</key><true/>\n"
            "  <key>UIViewControllerBasedStatusBarAppearance</key><true/>\n"
            "  <key>NSAppTransportSecurity</key>\n"
            "  <dict>\n"
            "    <key>NSAllowsLocalNetworking</key><true/>\n"
            "  </dict>\n"
            "</dict>\n"
            "</plist>\n",
            exe, bid, exe);
    fclose(f);
    return 0;
}

static int ios_sim_run(const char *app, const char *bid) {
    char cmd[4096];
    (void)system("open -a Simulator >/dev/null 2>&1");
    snprintf(cmd, sizeof cmd,
             "udid=$(xcrun simctl list devices available 2>/dev/null | "
             "awk -F '[()]' '/iPhone/{gsub(/^ +| +$/,\"\",$2); print $2; exit}'); "
             "if [ -z \"$udid\" ]; then echo 'loam: no iPhone Simulator found' >&2; exit 1; fi; "
             "xcrun simctl boot \"$udid\" >/dev/null 2>&1 || true; "
             "xcrun simctl bootstatus \"$udid\" -b >/dev/null; "
             "xcrun simctl install booted \"%s\" && xcrun simctl launch booted %s",
             app, bid);
    return system(cmd) != 0;
}

static int path_is_dir(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int find_android_sdk(char *buf, size_t n) {
    const char *e = getenv("ANDROID_HOME");
    const char *home;
    if (!e || !e[0]) e = getenv("ANDROID_SDK_ROOT");
    if (e && e[0] && path_is_dir(e)) {
        snprintf(buf, n, "%s", e);
        return 0;
    }
    home = getenv("HOME");
    if (home && home[0]) {
        snprintf(buf, n, "%s/Library/Android/sdk", home);
        if (path_is_dir(buf)) return 0;
        snprintf(buf, n, "%s/Android/Sdk", home);
        if (path_is_dir(buf)) return 0;
    }
    /* Homebrew `android-commandlinetools` cask (Apple Silicon / Intel). */
    if (path_is_dir("/opt/homebrew/share/android-commandlinetools")) {
        snprintf(buf, n, "%s", "/opt/homebrew/share/android-commandlinetools");
        return 0;
    }
    if (path_is_dir("/usr/local/share/android-commandlinetools")) {
        snprintf(buf, n, "%s", "/usr/local/share/android-commandlinetools");
        return 0;
    }
    if (n) buf[0] = 0;
    return 1;
}

static int find_gradle(char *buf, size_t n) {
    FILE *f;
    size_t len;
    struct stat st;
    const char *cands[] = {
        "/opt/homebrew/bin/gradle",
        "/usr/local/bin/gradle",
        NULL,
    };
    int i;
    f = popen("command -v gradle 2>/dev/null", "r");
    if (f) {
        if (fgets(buf, (int)n, f)) {
            len = strlen(buf);
            while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = 0;
            pclose(f);
            if (len && stat(buf, &st) == 0) return 0;
        } else {
            pclose(f);
        }
    }
    for (i = 0; cands[i]; i++) {
        if (stat(cands[i], &st) == 0) {
            snprintf(buf, n, "%s", cands[i]);
            return 0;
        }
    }
    if (n) buf[0] = 0;
    return 1;
}

static void android_app_id(const char *stem, char *out, size_t n) {
    char suf[128];
    size_t j = 0;
    const char *s = stem && stem[0] ? stem : "app";
    if (s[0] >= '0' && s[0] <= '9') suf[j++] = 'a';
    for (; *s && j + 1 < sizeof suf; s++) {
        char c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_')
            suf[j++] = c;
        else
            suf[j++] = '_';
    }
    suf[j] = 0;
    snprintf(out, n, "com.loam.%s", suf);
}

static int android_write_cmake(const char *path, int uses_http) {
    FILE *f;
    if (ensure_parent_dir(path) != 0) return 1;
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f,
            "cmake_minimum_required(VERSION 3.22.1)\n"
            "project(zeus C)\n"
            "add_library(zeus SHARED\n"
            "  app.c\n"
            "  \"%s/hosts/android/android.c\"\n"
            "  \"%s/zeus_plat.c\"\n"
            "  \"%s/zeus_key.c\"\n",
            LOAM_ZEUS_DIR, LOAM_RUNTIME_DIR, LOAM_RUNTIME_DIR);
            if (uses_http)
        fprintf(f, "  \"%s/net.c\"\n", LOAM_RUNTIME_DIR);
    fprintf(f,
            ")\n"
            "target_include_directories(zeus PRIVATE \"%s\")\n"
            "target_compile_definitions(zeus PRIVATE LOAM_ANDROID)\n"
            "target_compile_options(zeus PRIVATE -std=gnu99 -O1 -ffp-contract=off "
            "-fno-asynchronous-unwind-tables)\n"
            "target_link_libraries(zeus android log)\n",
            LOAM_RUNTIME_DIR);
    fclose(f);
    return 0;
}

static int android_write_app_gradle(const char *path, const char *app_id) {
    FILE *f;
    if (ensure_parent_dir(path) != 0) return 1;
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f,
            "apply plugin: 'com.android.application'\n"
            "android {\n"
            "    namespace 'com.loam.zeus'\n"
            "    compileSdk 34\n"
            "    defaultConfig {\n"
            "        applicationId \"%s\"\n"
            "        minSdk 26\n"
            "        targetSdk 34\n"
            "        ndk { abiFilters 'arm64-v8a', 'x86_64' }\n"
            "    }\n"
            "    compileOptions {\n"
            "        sourceCompatibility JavaVersion.VERSION_1_8\n"
            "        targetCompatibility JavaVersion.VERSION_1_8\n"
            "    }\n"
            "    externalNativeBuild {\n"
            "        cmake { path file('src/main/cpp/CMakeLists.txt') }\n"
            "    }\n"
            "}\n",
            app_id);
    fclose(f);
    return 0;
}

static int android_write_local_properties(const char *path, const char *sdk) {
    FILE *f;
    if (!sdk || !sdk[0]) return 0;
    if (ensure_parent_dir(path) != 0) return 1;
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f, "sdk.dir=%s\n", sdk);
    fclose(f);
    return 0;
}

static int android_copy(const char *src, const char *dst) {
    if (copy_file(src, dst) != 0) {
        fprintf(stderr, "loam: cannot copy %s -> %s\n", src, dst);
        return 1;
    }
    return 0;
}

static int android_emit_project(const char *proj, const char *cpath, const char *app_id,
                                int uses_http) {
    char src[1536], dst[1536], sdk[1024];
    if (mkdir_p(proj) != 0) return 1;
    snprintf(src, sizeof src, "%s/hosts/android/java/com/loam/zeus/ZeusActivity.java", LOAM_ZEUS_DIR);
    snprintf(dst, sizeof dst, "%s/app/src/main/java/com/loam/zeus/ZeusActivity.java", proj);
    if (android_copy(src, dst) != 0) return 1;
    snprintf(src, sizeof src, "%s/hosts/android/java/com/loam/zeus/ZeusView.java", LOAM_ZEUS_DIR);
    snprintf(dst, sizeof dst, "%s/app/src/main/java/com/loam/zeus/ZeusView.java", proj);
    if (android_copy(src, dst) != 0) return 1;
    snprintf(src, sizeof src, "%s/hosts/android/AndroidManifest.xml", LOAM_ZEUS_DIR);
    snprintf(dst, sizeof dst, "%s/app/src/main/AndroidManifest.xml", proj);
    if (android_copy(src, dst) != 0) return 1;
    snprintf(src, sizeof src, "%s/hosts/android/network_security_config.xml", LOAM_ZEUS_DIR);
    snprintf(dst, sizeof dst, "%s/app/src/main/res/xml/network_security_config.xml", proj);
    if (android_copy(src, dst) != 0) return 1;
    snprintf(src, sizeof src, "%s/hosts/android/root-build.gradle", LOAM_ZEUS_DIR);
    snprintf(dst, sizeof dst, "%s/build.gradle", proj);
    if (android_copy(src, dst) != 0) return 1;
    snprintf(src, sizeof src, "%s/hosts/android/settings.gradle", LOAM_ZEUS_DIR);
    snprintf(dst, sizeof dst, "%s/settings.gradle", proj);
    if (android_copy(src, dst) != 0) return 1;
    snprintf(src, sizeof src, "%s/hosts/android/gradle.properties", LOAM_ZEUS_DIR);
    snprintf(dst, sizeof dst, "%s/gradle.properties", proj);
    if (android_copy(src, dst) != 0) return 1;
    snprintf(dst, sizeof dst, "%s/app/src/main/cpp/app.c", proj);
    if (android_copy(cpath, dst) != 0) return 1;
    snprintf(dst, sizeof dst, "%s/app/src/main/cpp/CMakeLists.txt", proj);
    if (android_write_cmake(dst, uses_http) != 0) return 1;
    snprintf(dst, sizeof dst, "%s/app/build.gradle", proj);
    if (android_write_app_gradle(dst, app_id) != 0) return 1;
    if (find_android_sdk(sdk, sizeof sdk) == 0) {
        snprintf(dst, sizeof dst, "%s/local.properties", proj);
        if (android_write_local_properties(dst, sdk) != 0) return 1;
    }
    return 0;
}

static void android_howto(const char *proj, const char *pkg) {
    char sdk[1024];
    fprintf(stderr,
            "  Gradle project: %s\n"
            "  applicationId:  %s\n"
            "  Open in Android Studio, or with ANDROID_HOME, NDK, and Gradle 8.2+:\n"
            "    cd \"%s\" && gradle installDebug\n"
            "    adb shell am start -n %s/com.loam.zeus.ZeusActivity\n"
            "  Emulator RPC host is 10.0.2.2 (not 127.0.0.1). A physical device\n"
            "  needs the Mac LAN IP instead, and the backend must bind that path.\n",
            proj, pkg, proj, pkg);
    if (find_android_sdk(sdk, sizeof sdk) == 0)
        fprintf(stderr, "  SDK: %s\n", sdk);
    else
        fprintf(stderr,
                "  No Android SDK found. For the counter example:\n"
                "    ./zeus/examples/counter/android/install.sh\n"
                "  That installs Temurin, command-line tools, Gradle, platform 34, NDK,\n"
                "  and an AVD named yuga (macOS + Homebrew). Then:\n"
                "    emulator -avd yuga\n"
                "    ./zeus/examples/counter/android/run.sh\n"
                "  Or install Android Studio and set ANDROID_HOME=~/Library/Android/sdk\n");
}

static int android_run(const char *proj, const char *pkg) {
    char sdk[1024], gradle[1024], cmd[8192];
    char java_export[512] = "";
    if (find_gradle(gradle, sizeof gradle) != 0) {
        fprintf(stderr,
                "loam: no gradle on PATH. Install Gradle 8.2+ or open the project "
                "in Android Studio.\n");
        android_howto(proj, pkg);
        return 1;
    }
    if (find_android_sdk(sdk, sizeof sdk) != 0) {
        fprintf(stderr, "loam: no Android SDK (set ANDROID_HOME).\n");
        android_howto(proj, pkg);
        return 1;
    }
    if (!getenv("JAVA_HOME") || !getenv("JAVA_HOME")[0]) {
        if (path_is_dir("/Applications/Android Studio.app/Contents/jbr/Contents/Home"))
            snprintf(java_export, sizeof java_export,
                     "export JAVA_HOME=\"/Applications/Android Studio.app/Contents/jbr/Contents/Home\"; ");
    }
    snprintf(cmd, sizeof cmd,
             "%s"
             "export ANDROID_HOME=\"%s\"; export ANDROID_SDK_ROOT=\"%s\"; "
             "export PATH=\"%s/platform-tools:$PATH\"; "
             "cd \"%s\" && \"%s\" installDebug && "
             "adb shell am start -n %s/com.loam.zeus.ZeusActivity",
             java_export, sdk, sdk, sdk, proj, gradle, pkg);
    return system(cmd) != 0;
}

/** Print session diagnostics to stderr. */
static void print_diags(LoamSession *s) {
    for (int i = 0; i < s->ndiag; i++) {
        LoamDiag *d = &s->diags[i];
        fprintf(stderr, "%s:%d:%d: error: %s\n",
                d->file && d->file[0] ? d->file : "<unknown>",
                d->line, d->col, d->msg);
    }
}

/** CLI help on stderr. */
static void usage(void) {
    fprintf(stderr,
            "usage: loam [build] [options] <file.loam>\n"
            "  build       compile (optional; same as omitting it)\n"
            "  test        compile a runner for every `#[test]` fn and run it\n"
            "  check       typecheck only: no codegen, no cc, no output file\n"
            "  -o PATH     output binary (or .c/.ir with --emit-c/--emit-ir)\n"
            "  --emit-c    emit C99 (gnu99) instead of a binary\n"
            "  --emit-ir   emit backend-neutral IR instead of a binary\n"
            "  --target native  Cocoa desktop (default)\n"
            "  --target wasm32  Canvas2D .wasm (alias: wasm; needs clang wasm32)\n"
            "  --target ios     iOS Simulator .app (same Zeus paint as Cocoa; needs Xcode)\n"
            "  --target android Gradle + JNI Canvas host (needs Android SDK/NDK to build APK)\n"
            "  --run       compile and run (Simulator for --target=ios; gradle+adb for android)\n"
            "  --int64-compat  `int` = i64 and `float` = f64 (pre-Phase-10 behavior)\n"
            "Default output: <source-dir>/build/<name> (.app on ios; Gradle tree on android)\n");
}

/** Parse flags, run the frontend, emit C, optionally invoke cc and run. */
int main(int argc, char **argv) {
    const char *in_path = NULL;
    const char *out_path = NULL;
    int emit_c = 0;
    int emit_ir = 0;
    int run = 0;
    int test_mode = 0;
    int check_only = 0;
    int target_wasm = 0;
    int target_ios = 0;
    int target_android = 0;
    int int64_compat = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--emit-c") == 0 || strcmp(argv[i], "-S") == 0) {
            emit_c = 1;
        } else if (strcmp(argv[i], "--emit-ir") == 0) {
            emit_ir = 1;
        } else if (strncmp(argv[i], "--target=", 9) == 0 ||
                   (strcmp(argv[i], "--target") == 0 && i + 1 < argc)) {
            const char *t = strncmp(argv[i], "--target=", 9) == 0
                ? argv[i] + 9
                : argv[++i];
            if (strcmp(t, "native") == 0) {
                target_wasm = 0;
                target_ios = 0;
                target_android = 0;
            } else if (strcmp(t, "wasm") == 0 || strcmp(t, "wasm32") == 0) {
                target_wasm = 1;
                target_ios = 0;
                target_android = 0;
            } else if (strcmp(t, "ios") == 0) {
                target_ios = 1;
                target_wasm = 0;
                target_android = 0;
            } else if (strcmp(t, "android") == 0) {
                target_android = 1;
                target_wasm = 0;
                target_ios = 0;
            } else {
                fprintf(stderr, "unknown target %s (want native, wasm32, ios, or android)\n", t);
                return 1;
            }
        } else if (strcmp(argv[i], "--run") == 0) {
            run = 1;
        } else if (strcmp(argv[i], "test") == 0) {
            /* `loam test app.loam` — build a runner that executes every
               `#[test]` fn and run it. */
            test_mode = 1;
        } else if (strcmp(argv[i], "check") == 0) {
            /* `loam check app.loam` — run the frontend and stop. Used by
               run.sh and the zeus CLI to gate every run on a clean check. */
            check_only = 1;
        } else if (strcmp(argv[i], "--int64-compat") == 0) {
            int64_compat = 1;
        } else if (strcmp(argv[i], "build") == 0) {
            /* `loam build --target=native app.loam` — same as omitting `build`. */
            continue;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            usage();
            return 1;
        } else {
            in_path = argv[i];
        }
    }
    if (!in_path) {
        usage();
        return 1;
    }
    if (test_mode) run = 1;

    type_set_int64_compat(int64_compat);

    int show_time = env_on("LOAM_TIME");
    double t0 = now_sec();
    LoamSession sess;
    loam_session_init(&sess);
    if (loam_session_check(&sess, in_path, NULL) != 0) {
        print_diags(&sess);
        loam_session_free(&sess);
        return 1;
    }
    if (show_time) fprintf(stderr, "loam: check %.3fs\n", now_sec() - t0);

    if (check_only) {
        /* The frontend already ran above; a clean session is the whole result. */
        printf("loam: %s ok\n", in_path);
        loam_session_free(&sess);
        return 0;
    }

    if (test_mode) {
        /* The generated runner calls test.begin/ok/summary. */
        int has_test_mod = 0;
        for (int i = 0; i < sess.nmods; i++)
            if (sess.mods[i].name && strcmp(sess.mods[i].name, "test") == 0)
                has_test_mod = 1;
        if (!has_test_mod) {
            fprintf(stderr, "loam test: %s must import \"std:test\"\n", in_path);
            loam_session_free(&sess);
            return 1;
        }
    }

    if (emit_ir) {
        IrModule *ir = ir_lower(sess.mods, sess.nmods);
        int bad = ir_verify(ir);
        FILE *ir_out = stdout;
        if (out_path) {
            if (ensure_parent_dir(out_path) != 0) {
                fprintf(stderr, "error: cannot create directory for output\n");
                ir_free(ir);
                loam_session_free(&sess);
                return 1;
            }
            ir_out = fopen(out_path, "w");
            if (!ir_out) {
                fprintf(stderr, "error: cannot write '%s'\n", out_path);
                ir_free(ir);
                loam_session_free(&sess);
                return 1;
            }
        }
        ir_print(ir_out, ir);
        if (out_path) fclose(ir_out);
        ir_free(ir);
        loam_session_free(&sess);
        return bad ? 1 : 0;
    }

    char *stem = stem_of(in_path);
    char cpath[1024];
    char binpath[1024];
    int cpath_is_temp = 0;
    char *srcdir = dir_of(in_path);
    if (emit_c) {
        if (out_path)
            snprintf(cpath, sizeof cpath, "%s", out_path);
        else
            snprintf(cpath, sizeof cpath, "%s/build/%s.c", srcdir, stem);
        snprintf(binpath, sizeof binpath, "%s", "");
    } else {
        if (out_path)
            snprintf(binpath, sizeof binpath, "%s", out_path);
        else if (target_wasm)
            snprintf(binpath, sizeof binpath, "%s/build/%s.wasm", srcdir, stem);
        else if (target_ios)
            snprintf(binpath, sizeof binpath, "%s/build/%s.app", srcdir, stem);
        else if (target_android)
            snprintf(binpath, sizeof binpath, "%s/build/%s", srcdir, stem);
        else
            snprintf(binpath, sizeof binpath, "%s/build/%s", srcdir, stem);
        snprintf(cpath, sizeof cpath, "/tmp/loam_%s_XXXXXX", stem);
        const char *tmpdir = getenv("TMPDIR");
        if (tmpdir && tmpdir[0])
            snprintf(cpath, sizeof cpath, "%s/loam_%s_XXXXXX", tmpdir, stem);
        int fd = mkstemp(cpath);
        if (fd < 0) {
            fprintf(stderr, "error: cannot create temp C file\n");
            free(srcdir);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        close(fd);
        cpath_is_temp = 1;
        if (test_mode) {
            /* The runner binary is a throwaway; never write it into the app's
               build/ tree. `cc` overwrites the mkstemp'd empty file. */
            char tb[1024];
            snprintf(tb, sizeof tb, "%s/loam_test_XXXXXX",
                     (tmpdir && tmpdir[0]) ? tmpdir : "/tmp");
            int tfd = mkstemp(tb);
            if (tfd < 0) {
                fprintf(stderr, "error: cannot create temp test binary\n");
                unlink(cpath);
                free(srcdir);
                free(stem);
                loam_session_free(&sess);
                return 1;
            }
            close(tfd);
            snprintf(binpath, sizeof binpath, "%s", tb);
        }
    }
    /* App-owned C seam: `runtime/<stem>_runtime.c` next to the entry file is
       compiled and linked for native targets (used by e.g.
       examples/zeus/greeninfer). wasm / ios / android targets ignore it. */
    char app_rt[1024] = "";
    if (!emit_c && !target_wasm && !target_ios && !target_android)
        snprintf(app_rt, sizeof app_rt, "%s/runtime/%s_runtime.c", srcdir, stem);
    if (app_rt[0] && access(app_rt, R_OK) != 0) app_rt[0] = '\0';
    free(srcdir);
    if (ensure_parent_dir(emit_c ? cpath : binpath) != 0) {
        fprintf(stderr, "error: cannot create directory for output\n");
        if (cpath_is_temp) unlink(cpath);
        free(stem);
        loam_session_free(&sess);
        return 1;
    }

    FILE *out = fopen(cpath, "w");
    if (!out) {
        fprintf(stderr, "error: cannot write '%s'\n", cpath);
        if (cpath_is_temp) unlink(cpath);
        free(stem);
        loam_session_free(&sess);
        return 1;
    }
    t0 = now_sec();
    codegen_set_test_mode(test_mode);
    /* `#[server]` bodies are excluded from client targets (wasm here; the other
       client hosts join when their targets are split out). `LOAM_SERVER_SPLIT`
       forces it on so the exclusion can be checked from a generated-C build. */
    codegen_set_server_split(target_wasm || getenv("LOAM_SERVER_SPLIT") != NULL);
    codegen_emit_c(out, sess.mods, sess.nmods, LOAM_RT_PATH);
    fclose(out);
    if (show_time) fprintf(stderr, "loam: codegen %.3fs\n", now_sec() - t0);

    if (emit_c) {
        printf("loam: %s -> %s\n", in_path, cpath);
        free(stem);
        loam_session_free(&sess);
        return 0;
    }

    int uses_zeus = 0, uses_http = 0, uses_maya = 0, uses_net = 0, uses_raygui = 0;
    for (int i = 0; i < sess.nmods; i++) {
        if (!sess.mods[i].name) continue;
        if (strcmp(sess.mods[i].name, "zeus") == 0) uses_zeus = 1;
        if (strcmp(sess.mods[i].name, "http") == 0) uses_http = 1;
        if (strcmp(sess.mods[i].name, "maya") == 0) uses_maya = 1;
        if (strcmp(sess.mods[i].name, "net") == 0) uses_net = 1;
        if (strcmp(sess.mods[i].name, "raygui") == 0) uses_raygui = 1;
    }

    char http_link[768] = "";
    if (uses_http) {
        snprintf(http_link, sizeof http_link, " \"%s/net.c\"", LOAM_RUNTIME_DIR);
    } else if (uses_net) {
        snprintf(http_link, sizeof http_link, " \"%s/net.c\"", LOAM_RUNTIME_DIR);
    }

    /* App C seam (above): compile it inside the native link command. */
    char extra_link[1400] = "";
    if (app_rt[0])
        snprintf(extra_link, sizeof extra_link,
                 " -I\"%s\" -x c \"%s\"", LOAM_RUNTIME_DIR, app_rt);
    /* LOAM_LINK_EXTRA: extra .c/.o inputs appended to the native link, for
       entries without a sibling runtime file (e.g. CLI tools sharing an
       app's C seam). Space-separated paths. */
    const char *le = getenv("LOAM_LINK_EXTRA");
    if (le && le[0]) {
        size_t used = strlen(extra_link);
        size_t left = sizeof extra_link - used - 1;
        if (strlen(le) + 64 <= left) {
            snprintf(extra_link + used, left + 1, " -I\"%s\" %s", LOAM_RUNTIME_DIR, le);
        } else {
            fprintf(stderr, "loam: warning: LOAM_LINK_EXTRA too long, ignored\n");
        }
    }

    if (target_wasm) {
        char wasmcc[512];
        char loader_src[768], loader_dst[768];
        char *outdir = dir_of(binpath);
        char cmdw[8192];
        const char *cc;
        snprintf(loader_src, sizeof loader_src, "%s/hosts/web/loader.js", LOAM_ZEUS_DIR);
        snprintf(loader_dst, sizeof loader_dst, "%s/loader.js", outdir);
        if (copy_file(loader_src, loader_dst) != 0)
            fprintf(stderr, "loam: warning: could not copy %s\n", loader_src);
        cc = find_wasm_cc(wasmcc, sizeof wasmcc);
        if (!cc) {
            char keep[1024];
            snprintf(keep, sizeof keep, "%s.c", binpath);
            copy_file(cpath, keep);
            fprintf(stderr,
                    "loam: no clang with wasm32 (Apple /usr/bin/clang cannot).\n"
                    "  ./install.sh          # Homebrew LLVM, puts clang on PATH\n"
                    "  or: brew install llvm\n"
                    "  LOAM_WASM_CC=/opt/homebrew/opt/llvm/bin/clang ./bin/loam --target=wasm32 "
                    "%s -o %s\n"
                    "  generated C kept at %s ; Canvas2D loader at %s\n",
                    in_path, binpath, keep, loader_dst);
            if (cpath_is_temp) unlink(cpath);
            free(outdir);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        /* --fatal-warnings: wasm-ld downgrades a C prototype that disagrees with
           the generated definition to a warning and links a trapping stub, so
           the program builds and then dies with a bare "unreachable" at the
           first call. Fail the build instead. */
        {
            char http_w[512] = "";
            if (uses_http || uses_net)
                snprintf(http_w, sizeof http_w, " \"%s/net.c\"", LOAM_RUNTIME_DIR);
            if (uses_zeus) {
                snprintf(cmdw, sizeof cmdw,
                         "\"%s\" --target=wasm32 -nostdlib -ffreestanding "
                         "-fno-stack-protector -O2 -ffp-contract=off -I\"%s/wasm_inc\" -I\"%s\" "
                         "-Wl,--no-entry -Wl,--export-dynamic -Wl,--fatal-warnings "
                         "-x c \"%s\" -x none "
                         "\"%s/zeus_wasm_libc.c\" \"%s/hosts/web/wasm.c\" "
                         "\"%s/zeus_plat.c\" \"%s/zeus_key.c\"%s -o \"%s\"",
                         cc, LOAM_RUNTIME_DIR, LOAM_RUNTIME_DIR, cpath, LOAM_RUNTIME_DIR,
                         LOAM_ZEUS_DIR, LOAM_RUNTIME_DIR, LOAM_RUNTIME_DIR, http_w, binpath);
            } else {
                snprintf(cmdw, sizeof cmdw,
                         "\"%s\" --target=wasm32 -nostdlib -ffreestanding "
                         "-fno-stack-protector -O2 -ffp-contract=off -I\"%s/wasm_inc\" -I\"%s\" "
                         "-Wl,--no-entry -Wl,--export-dynamic -Wl,--export=main -Wl,--fatal-warnings "
                         "-x c \"%s\" -x none \"%s/zeus_wasm_libc.c\"%s -o \"%s\"",
                         cc, LOAM_RUNTIME_DIR, LOAM_RUNTIME_DIR, cpath, LOAM_RUNTIME_DIR,
                         http_w, binpath);
            }
        }
        t0 = now_sec();
        if (env_on("LOAM_TIME")) fprintf(stderr, "loam: cc %s\n", cc);
        if (system(cmdw) != 0) {
            fprintf(stderr, "loam: wasm compile failed (C: %s)\n", cpath);
            free(outdir);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        if (show_time) fprintf(stderr, "loam: cc %.3fs\n", now_sec() - t0);
        if (cpath_is_temp) unlink(cpath);
        printf("loam: %s -> %s (Canvas2D wasm)\n", in_path, binpath);
        free(outdir);
        free(stem);
        loam_session_free(&sess);
        return 0;
    }

    if (target_ios) {
        char sdk[1024];
        char appdir[1024];
        char exe[1024];
        char plist[1024];
        char bid[256];
        char cmdios[8192];
        char http_ios[768] = "";
        const char *arch =
#if defined(__aarch64__) || defined(__arm64__)
            "arm64";
#else
            "x86_64";
#endif
        if (!uses_zeus) {
            fprintf(stderr, "loam: --target=ios requires import \"std:zeus\"\n");
            if (cpath_is_temp) unlink(cpath);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        if (ios_sdk_path(sdk, sizeof sdk) != 0) {
            fprintf(stderr,
                    "loam: no iPhone Simulator SDK. Install Xcode (App Store), then:\n"
                    "  xcodebuild -downloadPlatform iOS\n"
                    "  or open Xcode → Settings → Platforms\n");
            if (cpath_is_temp) unlink(cpath);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        snprintf(appdir, sizeof appdir, "%s", binpath);
        {
            size_t n = strlen(appdir);
            if (n < 4 || strcmp(appdir + n - 4, ".app") != 0)
                snprintf(appdir, sizeof appdir, "%s.app", binpath);
        }
        if (mkdir_p(appdir) != 0) {
            fprintf(stderr, "loam: cannot create %s\n", appdir);
            if (cpath_is_temp) unlink(cpath);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        snprintf(exe, sizeof exe, "%s/%s", appdir, stem);
        snprintf(plist, sizeof plist, "%s/Info.plist", appdir);
        snprintf(bid, sizeof bid, "com.loam.%s", stem);
        if (write_ios_plist(plist, stem, bid) != 0) {
            fprintf(stderr, "loam: cannot write Info.plist\n");
            if (cpath_is_temp) unlink(cpath);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        if (uses_http)
            snprintf(http_ios, sizeof http_ios, " \"%s/net.c\"", LOAM_RUNTIME_DIR);
        snprintf(cmdios, sizeof cmdios,
                 "xcrun clang -isysroot \"%s\" -target %s-apple-ios16.0-simulator "
                 "-O1 -ffp-contract=off -fno-asynchronous-unwind-tables -DLOAM_IOS -I\"%s\" "
                 "-x c -std=gnu99 \"%s\" \"%s/zeus_plat.c\" \"%s/zeus_key.c\"%s "
                 "-x objective-c -fno-objc-arc \"%s/hosts/ios/ios.m\" "
                 "-framework UIKit -framework Foundation -framework CoreGraphics "
                 "-framework CoreText -framework QuartzCore "
                 "-framework Security -framework CoreFoundation -o \"%s\"",
                 sdk, arch, LOAM_RUNTIME_DIR, cpath, LOAM_RUNTIME_DIR, LOAM_RUNTIME_DIR,
                 http_ios, LOAM_ZEUS_DIR, exe);
        t0 = now_sec();
        if (env_on("LOAM_TIME")) fprintf(stderr, "loam: cc ios\n");
        if (system(cmdios) != 0) {
            fprintf(stderr, "loam: iOS compile failed (C: %s)\n", cpath);
            if (cpath_is_temp) unlink(cpath);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        {
            char sign[1536];
            snprintf(sign, sizeof sign,
                     "codesign --sign - --force --timestamp=none \"%s\"", appdir);
            (void)system(sign);
        }
        if (show_time) fprintf(stderr, "loam: cc %.3fs\n", now_sec() - t0);
        if (cpath_is_temp) unlink(cpath);
        printf("loam: %s -> %s (iOS Simulator)\n", in_path, appdir);
        {
            int run_rc = 0;
            if (run && ios_sim_run(appdir, bid) != 0) run_rc = 1;
            free(stem);
            loam_session_free(&sess);
            return run_rc;
        }
    }

    if (target_android) {
        char pkg[256];
        if (!uses_zeus) {
            fprintf(stderr, "loam: --target=android requires import \"std:zeus\"\n");
            if (cpath_is_temp) unlink(cpath);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        android_app_id(stem, pkg, sizeof pkg);
        if (mkdir_p(binpath) != 0) {
            fprintf(stderr, "loam: cannot create %s\n", binpath);
            if (cpath_is_temp) unlink(cpath);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        if (android_emit_project(binpath, cpath, pkg, uses_http) != 0) {
            fprintf(stderr, "loam: cannot write Android project to %s\n", binpath);
            if (cpath_is_temp) unlink(cpath);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
        if (cpath_is_temp) unlink(cpath);
        printf("loam: %s -> %s (Android Gradle)\n", in_path, binpath);
        fflush(stdout);
        {
            int run_rc = 0;
            if (run) {
                if (android_run(binpath, pkg) != 0) run_rc = 1;
            } else {
                android_howto(binpath, pkg);
            }
            free(stem);
            loam_session_free(&sess);
            return run_rc;
        }
    }

    /* Generated C is large. -O2 + function-sections on the whole TU dominated
       wall time. Headless tests skip the optimizer and Cocoa; GUI uses -O1.
       Runtime .c/.m compile once into runtime/.obj/. */
    int headless = want_headless();
    /* Generated C carries `#line` directives pointing at `.loam` sources, so
       -g makes lldb/gdb, profilers, and sanitizers report Loam lines. Off by
       default: it inflates binaries and the published size benchmark. */
    static char copt_buf[256];
    snprintf(copt_buf, sizeof copt_buf, "%s%s",
             headless ? "-std=gnu99 -O0 -fno-asynchronous-unwind-tables -ffp-contract=off"
                      : "-std=gnu99 -O1 -fno-asynchronous-unwind-tables "
                        "-fomit-frame-pointer -ffp-contract=off",
             env_on("LOAM_DEBUG") ? " -g" : "");
    const char *copt = copt_buf;
#if defined(__APPLE__)
    const char *ld = headless ? "" : "-Wl,-dead_strip";
#else
    const char *ld = headless ? "" : "-Wl,--gc-sections";
#endif

    char cmd[4096];
    char plat_c[512], key_c[512], mac_m[512], rt_h[512], key_h[512];
    char plat_o[512], key_o[512], mac_o[512];
    snprintf(plat_c, sizeof plat_c, "%s/zeus_plat.c", LOAM_RUNTIME_DIR);
    snprintf(key_c, sizeof key_c, "%s/zeus_key.c", LOAM_RUNTIME_DIR);
    snprintf(mac_m, sizeof mac_m, "%s/hosts/desktop/mac.m", LOAM_ZEUS_DIR);
    snprintf(rt_h, sizeof rt_h, "%s/zeus_rt.h", LOAM_RUNTIME_DIR);
    snprintf(key_h, sizeof key_h, "%s/zeus_key.h", LOAM_RUNTIME_DIR);
    snprintf(plat_o, sizeof plat_o, "%s/.obj/zeus_plat.o", LOAM_RUNTIME_DIR);
    snprintf(key_o, sizeof key_o, "%s/.obj/zeus_key.o", LOAM_RUNTIME_DIR);
    snprintf(mac_o, sizeof mac_o, "%s/.obj/zeus_mac.o", LOAM_RUNTIME_DIR);

    t0 = now_sec();
    if (uses_zeus) {
        const char *plat_deps[] = {plat_c, rt_h, key_h};
        const char *key_deps[] = {key_c, key_h};
        if (ensure_obj(plat_c, plat_o, "", plat_deps, 3) ||
            ensure_obj(key_c, key_o, "", key_deps, 2)) {
            fprintf(stderr, "loam: failed to compile zeus runtime\n");
            if (cpath_is_temp) unlink(cpath);
            free(stem);
            loam_session_free(&sess);
            return 1;
        }
#if defined(__APPLE__)
        if (!headless) {
            const char *mac_deps[] = {mac_m, rt_h, key_h};
            if (ensure_obj(mac_m, mac_o, "-x objective-c", mac_deps, 3)) {
                fprintf(stderr, "loam: failed to compile %s\n", mac_m);
                if (cpath_is_temp) unlink(cpath);
                free(stem);
                loam_session_free(&sess);
                return 1;
            }
            snprintf(cmd, sizeof cmd,
                     "cc %s %s -o \"%s\" -I\"%s\" -x c \"%s\" -x none \"%s\" \"%s\" \"%s\"%s%s "
                     "-framework Cocoa -framework QuartzCore -framework Security "
                     "-framework CoreFoundation -lm",
                     copt, ld, binpath, LOAM_RUNTIME_DIR, cpath, plat_o, key_o, mac_o, http_link,
                     extra_link);
        } else {
            snprintf(cmd, sizeof cmd,
                     "cc %s -o \"%s\" -I\"%s\" -x c \"%s\" -x none \"%s\" \"%s\"%s%s "
                     "-framework Security -framework CoreFoundation -lm",
                     copt, binpath, LOAM_RUNTIME_DIR, cpath, plat_o, key_o, http_link, extra_link);
        }
#else
        if (!headless) {
            char linux_c[512], linux_o[512];
            snprintf(linux_c, sizeof linux_c, "%s/hosts/desktop/linux.c", LOAM_ZEUS_DIR);
            snprintf(linux_o, sizeof linux_o, "%s/.obj/zeus_linux.o", LOAM_RUNTIME_DIR);
            {
                const char *linux_deps[] = {linux_c, rt_h, key_h};
                if (ensure_obj(linux_c, linux_o, "", linux_deps, 3)) {
                    fprintf(stderr, "loam: failed to compile %s (need libx11)\n", linux_c);
                    if (cpath_is_temp) unlink(cpath);
                    free(stem);
                    loam_session_free(&sess);
                    return 1;
                }
            }
            snprintf(cmd, sizeof cmd,
                     "cc %s -o \"%s\" -I\"%s\" -x c \"%s\" -x none \"%s\" \"%s\" \"%s\"%s%s "
                     "-lX11 -lm",
                     copt, binpath, LOAM_RUNTIME_DIR, cpath, plat_o, key_o, linux_o, http_link,
                     extra_link);
        } else {
            snprintf(cmd, sizeof cmd,
                     "cc %s -o \"%s\" -I\"%s\" -x c \"%s\" -x none \"%s\" \"%s\"%s%s -lm",
                     copt, binpath, LOAM_RUNTIME_DIR, cpath, plat_o, key_o, http_link, extra_link);
        }
        (void)mac_m;
        (void)mac_o;
#endif
    } else if (uses_maya) {
#if defined(__APPLE__)
        snprintf(cmd, sizeof cmd,
                 "cc %s %s -o \"%s\" -I\"%s\" -x c \"%s\" \"%s/maya_plat.c\" "
                 "-x objective-c -fobjc-arc \"%s/maya_mac.m\" "
                 "-framework Cocoa -lm%s",
                 copt, ld, binpath, LOAM_RUNTIME_DIR, cpath, LOAM_RUNTIME_DIR, LOAM_RUNTIME_DIR,
                 extra_link);
#else
        snprintf(cmd, sizeof cmd,
                 "cc %s %s -o \"%s\" -I\"%s\" -x c \"%s\" \"%s/maya_plat.c\" -lm%s",
                 copt, ld, binpath, LOAM_RUNTIME_DIR, cpath, LOAM_RUNTIME_DIR, extra_link);
#endif
    } else if (uses_http) {
#if defined(__APPLE__)
        snprintf(cmd, sizeof cmd,
                 "cc %s %s -o \"%s\" -I\"%s\" -x c \"%s\" \"%s/net.c\" "
                 "-framework Security -framework CoreFoundation%s",
                 copt, ld, binpath, LOAM_RUNTIME_DIR, cpath, LOAM_RUNTIME_DIR, extra_link);
#else
        snprintf(cmd, sizeof cmd,
                 "cc %s %s -o \"%s\" -I\"%s\" -x c \"%s\" \"%s/net.c\"%s",
                 copt, ld, binpath, LOAM_RUNTIME_DIR, cpath, LOAM_RUNTIME_DIR, extra_link);
#endif
    } else if (uses_net) {
#if defined(__APPLE__)
        snprintf(cmd, sizeof cmd,
                 "cc %s %s -o \"%s\" -I\"%s\" -x c \"%s\" \"%s/net.c\" "
                 "-framework Security -framework CoreFoundation%s",
                 copt, ld, binpath, LOAM_RUNTIME_DIR, cpath, LOAM_RUNTIME_DIR, extra_link);
#else
        snprintf(cmd, sizeof cmd,
                 "cc %s %s -o \"%s\" -I\"%s\" -x c \"%s\" \"%s/net.c\"%s",
                 copt, ld, binpath, LOAM_RUNTIME_DIR, cpath, LOAM_RUNTIME_DIR, extra_link);
#endif
    } else if (uses_raygui) {
        /* raygui.h is vendored in the raygui package; raylib comes from the
           system (Homebrew / pkg-config / RAYLIB_PREFIX). The host shim owns
           the window + raygui implementation; Loam owns the frame loop. */
        char rlib[512];
        raygui_raylib_flags(rlib, sizeof rlib);
        snprintf(cmd, sizeof cmd,
                 "cc %s %s -o \"%s\" -I\"%s\" -I\"%s/vendor\" %s "
                 "-x c \"%s\" -x none \"%s/raygui_plat.c\" -lm%s",
                 copt, ld, binpath, LOAM_RUNTIME_DIR, LOAM_RAYGUI_DIR, rlib, cpath,
                 LOAM_RUNTIME_DIR, extra_link);
    } else {
        snprintf(cmd, sizeof cmd, "cc %s %s -x c \"%s\" -o \"%s\"%s", copt, ld, cpath,
                 binpath, extra_link);
    }
    int rc = system(cmd);
    if (show_time) fprintf(stderr, "loam: cc %.3fs\n", now_sec() - t0);
    if (rc != 0) {
        fprintf(stderr, "loam: C compile failed (temp source: %s)\n", cpath);
        free(stem);
        loam_session_free(&sess);
        return 1;
    }
    unlink(cpath);
    if (!test_mode) printf("loam: %s -> %s\n", in_path, binpath);

    int run_rc = 0;
    if (run) {
        char rcmd[1024];
        if (binpath[0] == '/' || (binpath[0] == '.' && binpath[1] == '/'))
            snprintf(rcmd, sizeof rcmd, "\"%s\"", binpath);
        else
            snprintf(rcmd, sizeof rcmd, "\"./%s\"", binpath);
        run_rc = system(rcmd);
        if (run_rc != 0) run_rc = 1;
    }
    if (test_mode) unlink(binpath);

    free(stem);
    loam_session_free(&sess);
    return run_rc;
}
