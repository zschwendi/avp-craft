/* Darwin shims for libltw (macOS, visionOS, visionOS simulator). No AppKit/UIKit; EGL is ANGLE's.
 *
 *  1. __android_log_* -> stderr (+ os_log so it shows in Console/`log stream` on device).
 *  2. ltw_darwin_dlopen: upstream proc.c's constructor does dlopen("libEGL.so"). Map that onto
 *     ANGLE's libEGL. Search order:
 *        $LTW_EGL_LIBRARY (explicit path, used by the macOS test)
 *        @rpath/libEGL.framework/libEGL   (Klepton app bundles: Frameworks/libEGL.framework)
 *        @rpath/libEGL.dylib              (ANGLE gn output layout, e.g. vendor/out/Debug)
 *        libEGL.dylib                     (DYLD_LIBRARY_PATH / cwd)
 *  3. ltw_darwin_dlsym: dlsym(NULL, ...) means "global scope" on bionic/glibc, but NULL is not a
 *     valid handle on Darwin (RTLD_DEFAULT is (void*)-2). The stub_gl* names live in libltw itself,
 *     so search RTLD_SELF first (cheap, and cannot pick up a foreign stub_), then RTLD_DEFAULT.
 *  4. OSMesaGetProcAddress: LWJGL's GL FunctionProvider (org.lwjgl.opengl.GL$1) looks for
 *     glXGetProcAddress only on LINUX and wglGetProcAddress only on WINDOWS; on MACOSX it falls
 *     through to "OSMesaGetProcAddress" in the loaded GL library, then to plain dlsym. Exporting
 *     this alias makes stock LWJGL on Darwin route EVERY GL lookup through ltw with no Java change
 *     (just -Dorg.lwjgl.opengl.libname=<path>/libltw.dylib).
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/log.h>
#include <android/log.h>

/* ---- 1. logging ------------------------------------------------------------------------------ */
static const char *prio_name(int p) {
    switch (p) {
        case ANDROID_LOG_VERBOSE: return "V"; case ANDROID_LOG_DEBUG: return "D";
        case ANDROID_LOG_INFO: return "I";    case ANDROID_LOG_WARN: return "W";
        case ANDROID_LOG_ERROR: return "E";   case ANDROID_LOG_FATAL: return "F";
        default: return "?";
    }
}

int __android_log_write(int prio, const char *tag, const char *text) {
    if (!tag) tag = "ltw";
    if (!text) text = "";
    fprintf(stderr, "%s/%s: %s\n", prio_name(prio), tag, text);
    os_log_type_t t = prio >= ANDROID_LOG_ERROR ? OS_LOG_TYPE_ERROR
                    : prio == ANDROID_LOG_DEBUG || prio == ANDROID_LOG_VERBOSE ? OS_LOG_TYPE_DEBUG
                    : OS_LOG_TYPE_DEFAULT;
    os_log_with_type(OS_LOG_DEFAULT, t, "%{public}s: %{public}s", tag, text);
    return 1;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    return __android_log_write(prio, tag, buf);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- 2. dlopen("libEGL.so") -> ANGLE ---------------------------------------------------------- */
#undef dlopen
#undef dlsym
__attribute__((visibility("hidden"))) void *ltw_darwin_dlopen(const char *path, int mode) {
    if (path && strcmp(path, "libEGL.so") != 0) return dlopen(path, mode);
    const char *env = getenv("LTW_EGL_LIBRARY");
    const char *cands[] = { env, "@rpath/libEGL.framework/libEGL", "@rpath/libEGL.dylib",
                            "libEGL.dylib" };
    for (size_t i = 0; i < sizeof cands / sizeof *cands; i++) {
        if (!cands[i] || !*cands[i]) continue;
        void *h = dlopen(cands[i], mode);
        if (h) {
            if (getenv("LTW_DARWIN_VERBOSE")) fprintf(stderr, "LTW: EGL from %s\n", cands[i]);
            return h;
        }
    }
    return NULL; /* proc.c reports dlerror() and aborts, as upstream */
}

/* ---- 3. dlsym(NULL, ...) ------------------------------------------------------------------------ */
__attribute__((visibility("hidden"))) void *ltw_darwin_dlsym(void *handle, const char *sym) {
    if (handle != NULL) return dlsym(handle, sym);
    void *p = dlsym(RTLD_SELF, sym);
    return p ? p : dlsym(RTLD_DEFAULT, sym);
}

/* ---- 4. LWJGL-on-MACOSX hook ------------------------------------------------------------------ */
typedef void (*ltw_proc_t)(void);
extern ltw_proc_t eglGetProcAddress(const char *procname); /* ltw's own (proc.c) */
__attribute__((visibility("default"), used)) ltw_proc_t OSMesaGetProcAddress(const char *procname) {
    return eglGetProcAddress(procname);
}
