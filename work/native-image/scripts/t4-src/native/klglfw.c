/*
 * klglfw.c -- libklglfw: the native half of Klepton's GLFW stand-in (T4 of the Vivecraft-on-Vision-Pro port).
 *
 * The Java half is org.lwjgl.glfw.GLFW + KlGlfwNative (scripts/t4-src/java). Every native below is a plain JNI
 * downcall; this file never calls back into Java (no FindClass/GetMethodID/upcalls, no LWJGL closures). GLFW
 * callbacks are dispatched in Java from the event records nPollEvents copies out.
 *
 * Dependencies: libc, pthread, dl, mach time, EGL headers (ANGLE's vendor/include, header-only). No AppKit/Cocoa/
 * UIKit/CoreFoundation, so the same file compiles for xros. The EGL and GL entry points are all taken from the GL
 * library's own eglGetProcAddress / OSMesaGetProcAddress (libltw: Pojav's egl_loader.c pattern). That matters: ltw
 * keys its per-context state by EGLContext, so the context MUST be created and made current through ltw's
 * eglCreateContext / eglMakeCurrent; a context made current behind ltw's back makes every ltw override a silent no-op.
 *
 * ---- JNI surface (class org.lwjgl.glfw.KlGlfwNative) -------------------------------------------------------------
 *  nInit(String glLib)          dlopen glLib (= -Dorg.lwjgl.opengl.libname, normally libltw.dylib; env
 *                               KLGLFW_GL_LIBRARY overrides), resolve EGL through its eglGetProcAddress, open the
 *                               ANGLE display (EGL_PLATFORM_ANGLE_ANGLE + METAL; KLGLFW_DEFAULT_DISPLAY=1 ->
 *                               eglGetDisplay(DEFAULT)), eglInitialize, bind ES, choose an ES3 PBUFFER RGBA8 D24 S8
 *                               config; start the clock; parse the test script. false + nLastError() on failure.
 *                               T8: the display/config come from kl_egl (the same objects kl_openxr renders into).
 *  nCreateWindow(w,h,share,..)  "window" = ES 3.0 context (ltw eglCreateContext) + a w x h pbuffer surface.
 *                               KLGLFW_SIZE=WxH overrides the size. Queues focus(1) + cursor-enter(1).
 *                               T8: replace the pbuffer with the kl_egl surface / swapchain-backed FBO mirror.
 *  nDestroyWindow(win)          destroy context + surface.
 *  nGetFramebufferSize(win)     (w << 32) | h.
 *  nMakeContextCurrent(win)     ltw eglMakeCurrent(surface, surface, context); win == 0 releases. Thread-local.
 *  nGetCurrentContext()         window current on the calling thread (0 if none).
 *  nSwapBuffers(win)            frame counter; optional pbuffer screenshot (glReadPixels of framebuffer 0 -> PNG,
 *                               state saved/restored) every KLGLFW_SHOT_EVERY frames, at KLGLFW_SHOT_AT=s1,s2,..
 *                               seconds and on script "shot"; into KLGLFW_SHOT_DIR (default "."); then
 *                               eglSwapBuffers; KLGLFW_FPS=1 logs frames/s from here once a second.
 *                               T8: present = blit to the compositor mirror / nothing in VR (kl_openxr submits).
 *  nSwapInterval(i)             eglSwapInterval (meaningless on a pbuffer; kept for T8).
 *  nGetProcAddress(name)        the GL library's OSMesaGetProcAddress (ltw's lookup: overrides, stubs, ANGLE).
 *  nExtensionSupported(name)    glGetStringi(GL_EXTENSIONS, i) scan on the current context.
 *  nPollEvents(buf, cap)        run due script lines, then copy up to cap queued events into buf (records of
 *                               struct klglfw_event, 48 bytes, native endian); returns the count. ONE JNI call/poll.
 *  nWaitEvents(timeout)         block until an event is queued, a script line is due, nPostEmptyEvent, or the
 *                               timeout (seconds; < 0 = no timeout). Minecraft's frame limiter sits here.
 *  nPostEmptyEvent()            wake nWaitEvents.
 *  nSetCursorPos(win,x,y)       record the cursor position (no event, as GLFW).
 *  nSetInputMode(win,mode,val)  record cursor mode (GLFW_CURSOR_DISABLED = grabbed) for the platform side.
 *  nGetTime / nSetTime          seconds since nInit, mach_absolute_time based.
 *  nGetTimerValue()             nanoseconds since nInit (frequency 1e9).
 *  nGetMonitorSize()            KLGLFW_MONITOR=WxH, default 1920x1080, packed (w << 32) | h.
 *  nGetEGLHandle(win, which)    0 EGLDisplay, 1 EGLContext, 2 EGLSurface, 3 EGLConfig. T8: Vivecraft's
 *                               VLoader.getEGLDisplay/Context/Config must hand kl_openxr exactly these.
 *  nLastError / nDescribe       diagnostics.
 *
 * ---- Two hosts (T8b) ----------------------------------------------------------------------------------------------
 *  default (macOS T4 runs)      ANGLE platform display (METAL), ES3 PBUFFER config, the "window" is a pbuffer.
 *  KLGLFW_VR=1                  (with KLGLFW_HOST=klepton, M2) VR mode: nSwapBuffers does not present the window.
 *  KLGLFW_HOST=klepton          inside the Klepton visionOS app (set by kl_app.c's native door). libltw's EGL is then
 *                               the app's kl_egl gateway (ltw dlopens $LTW_EGL_LIBRARY = Frameworks/libklegl.dylib,
 *                               whose eglGetProcAddress the app primes), so: eglGetDisplay(EGL_DEFAULT_DISPLAY) is
 *                               kl_egl's display, the config is kl_egl's, and the window is eglCreateWindowSurface on
 *                               kl_egl -- which tells kl_present this guest has a FLAT picture (the app's window path)
 *                               and whose eglSwapBuffers hands each frame to the compositor (kl_glfb -> kl_mono).
 *                               The context is still created and made current THROUGH ltw. One share group: kl_glfb
 *                               backs it with its root ANGLE context, the same one kl_openxr makes swapchains in (M2).
 *                               Input arrives through klglfw_push_event from the app's window (kl_mono hooks).
 *
 * ---- C API for the platform side (T8) -----------------------------------------------------------------------------
 *  klglfw_push_event(type, a, b, c, d, x, y)  thread-safe enqueue + wake (KLGLFW_EV_* below). T8 feeds this from
 *                               kl_openxr / hand tracking / the compositor keyboard; the test feeds it from the script.
 *  klglfw_get_cursor_mode(), klglfw_get_cursor_pos(&x, &y)  what Minecraft asked for (grab state, warp).
 *
 * ---- Test input script (KLGLFW_SCRIPT=<file>, and/or KLGLFW_EVENTS="line;line;...") --------------------------------
 *  One command per line, "t=<seconds since glfwInit> <cmd> <args>", '#' starts a comment. Commands:
 *    key <glfwkey> press|release|repeat [mods [scancode]]   tap <glfwkey> [mods]  (press + release in one poll)
 *    char <codepoint> [mods]     type <text...>              button <0-7> press|release [mods]
 *    click <button> [x y]        move <x> <y>                scroll <dx> <dy>
 *    focus 0|1                   close                       shot          log <text...>
 *  e.g. "t=5 key 257 press" presses Enter five seconds after glfwInit.
 *  KLGLFW_LOG=1 logs every queued event.
 */
#include <jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <unistd.h>

#define KL_EXPORT __attribute__((visibility("default")))

/* ---- event records: keep in sync with GLFW.EV_* in GLFW.java -------------------------------------------------- */
enum {
    KLGLFW_EV_KEY = 1, KLGLFW_EV_CHAR = 2, KLGLFW_EV_MOUSE_BUTTON = 3, KLGLFW_EV_CURSOR_POS = 4, KLGLFW_EV_SCROLL = 5,
    KLGLFW_EV_FRAMEBUFFER_SIZE = 6, KLGLFW_EV_WINDOW_SIZE = 7, KLGLFW_EV_FOCUS = 8, KLGLFW_EV_CURSOR_ENTER = 9,
    KLGLFW_EV_CLOSE = 10, KLGLFW_EV_ICONIFY = 11, KLGLFW_EV_CONTENT_SCALE = 12, KLGLFW_EV_REFRESH = 13,
    KLGLFW_EV_WINDOW_POS = 14,
};
struct klglfw_event {           /* 48 bytes */
    int64_t window;             /* 0 = the first window */
    int32_t type, a, b, c, d, pad;
    double x, y;
};
_Static_assert(sizeof(struct klglfw_event) == 48, "event record size is shared with GLFW.java");

/* ---- GL constants (desktop + ES) ---------------------------------------------------------------------------------- */
#define GL_RGBA_ 0x1908
#define GL_UNSIGNED_BYTE_ 0x1401
#define GL_PACK_ROW_LENGTH_ 0x0D02
#define GL_PACK_SKIP_ROWS_ 0x0D03
#define GL_PACK_SKIP_PIXELS_ 0x0D04
#define GL_PACK_ALIGNMENT_ 0x0D05
#define GL_READ_FRAMEBUFFER_ 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING_ 0x8CAA
#define GL_PIXEL_PACK_BUFFER_ 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING_ 0x88ED
#define GL_EXTENSIONS_ 0x1F03
#define GL_NUM_EXTENSIONS_ 0x821D
#define GL_VERSION_ 0x1F02
#define GL_RENDERER_ 0x1F01

typedef void (*fn_t)(void);
typedef fn_t (*gpa_t)(const char *);

/* EGL, all from the GL library's eglGetProcAddress */
static EGLDisplay (*p_eglGetDisplay)(EGLNativeDisplayType);
static EGLDisplay (*p_eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
static EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*p_eglTerminate)(EGLDisplay);
static EGLBoolean (*p_eglBindAPI)(EGLenum);
static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLSurface (*p_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
static EGLSurface (*p_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
static EGLBoolean (*p_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*p_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*p_eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean (*p_eglSwapInterval)(EGLDisplay, EGLint);
static EGLint (*p_eglGetError)(void);
static const char *(*p_eglQueryString)(EGLDisplay, EGLint);
/* GL, all from the GL library's OSMesaGetProcAddress (what LWJGL itself uses on MACOSX) */
static void (*p_glGetIntegerv)(unsigned, int *);
static void (*p_glBindFramebuffer)(unsigned, unsigned);
static void (*p_glBindBuffer)(unsigned, unsigned);
static void (*p_glPixelStorei)(unsigned, int);
static void (*p_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
static const unsigned char *(*p_glGetString)(unsigned);
static const unsigned char *(*p_glGetStringi)(unsigned, unsigned);

typedef struct klglfw_window {
    uint32_t magic;
    int width, height;
    EGLSurface surface;
    EGLContext context;
    struct klglfw_window *next;
} klglfw_window;
#define WIN_MAGIC 0x6b6c6777u /* 'klgw' */

static struct {
    int inited;
    int klepton;            /* KLGLFW_HOST=klepton: inside the visionOS app (see the header) */
    int vr;                 /* KLGLFW_VR=1 (M2, set by kl_app.c in VR mode): no flat present (see nSwapBuffers) */
    char gl_path[1024];
    void *gl;
    gpa_t egl_gpa, gl_gpa;
    EGLDisplay dpy;
    EGLConfig cfg;
    int egl_major, egl_minor;
    klglfw_window *windows;
    /* clock */
    mach_timebase_info_data_t tb;
    uint64_t t0;
    double offset;          /* glfwSetTime */
    /* event ring */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct klglfw_event ring[4096];
    unsigned head, tail;    /* tail - head = queued */
    unsigned long dropped;
    int woken;
    /* input state seen by the platform side */
    int cursor_mode;
    double cursor_x, cursor_y;
    /* screenshots / fps */
    long shot_every;
    char shot_dir[1024];
    double shot_at[64];
    int n_shot_at, next_shot_at;
    int shot_pending;
    unsigned long frames, shots;
    int fps_log;
    double fps_t, fps_frames_at;
    int log_events;
} S = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static __thread klglfw_window *tls_current;
static char last_error[1024];

static void seterr(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(last_error, sizeof last_error, fmt, ap); va_end(ap);
    fprintf(stderr, "[klglfw] error: %s\n", last_error);
}
#define LOG(...) do { fprintf(stderr, "[klglfw] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ---- time --------------------------------------------------------------------------------------------------------- */
static uint64_t ns_since_init(void) {
    uint64_t t = mach_absolute_time() - S.t0;
    return (uint64_t)((__uint128_t)t * S.tb.numer / S.tb.denom);
}
static double now_s(void) { return (double)ns_since_init() / 1e9; }

/* ---- event ring --------------------------------------------------------------------------------------------------- */
#define RING_N (sizeof S.ring / sizeof S.ring[0])
static void push_locked(int64_t win, int type, int a, int b, int c, int d, double x, double y) {
    if (S.tail - S.head >= RING_N) { S.dropped++; return; }
    struct klglfw_event *e = &S.ring[S.tail++ % RING_N];
    e->window = win; e->type = type; e->a = a; e->b = b; e->c = c; e->d = d; e->pad = 0; e->x = x; e->y = y;
    if (S.log_events) LOG("event t=%.3f type=%d a=%d b=%d c=%d d=%d x=%.1f y=%.1f", now_s(), type, a, b, c, d, x, y);
    pthread_cond_broadcast(&S.cv);
}

KL_EXPORT void klglfw_push_event(int type, int a, int b, int c, int d, double x, double y) {
    pthread_mutex_lock(&S.mu);
    push_locked(0, type, a, b, c, d, x, y);
    pthread_mutex_unlock(&S.mu);
}
KL_EXPORT int klglfw_get_cursor_mode(void) { return S.cursor_mode; }
/* The EGL objects of the first window (0 display, 1 context, 2 surface, 3 config) -- what GLFWNativeEGL answers, for a
 * C caller (the app, kl_openxr's binding check) that has no JNIEnv. */
KL_EXPORT void *klglfw_egl_handle(int which);
KL_EXPORT void klglfw_get_cursor_pos(double *x, double *y) { if (x) *x = S.cursor_x; if (y) *y = S.cursor_y; }

/* ---- test script -------------------------------------------------------------------------------------------------- */
typedef struct { double t; char *line; } script_line;
static script_line *script;
static int n_script, next_script;

static void script_add(const char *src) {
    while (*src == ' ' || *src == '\t') src++;
    if (!*src || *src == '#') return;
    double t = 0;
    if (strncmp(src, "t=", 2) == 0) {
        char *end; t = strtod(src + 2, &end); src = end;
        while (*src == ' ' || *src == '\t') src++;
    }
    char *l = strdup(src), *hash = strchr(l, '#');
    if (hash) *hash = 0;
    size_t n = strlen(l);
    while (n && (l[n - 1] == '\n' || l[n - 1] == '\r' || l[n - 1] == ' ')) l[--n] = 0;
    if (!n) { free(l); return; }
    script = realloc(script, sizeof *script * (n_script + 1));
    int i = n_script++;
    while (i > 0 && script[i - 1].t > t) { script[i] = script[i - 1]; i--; }   /* stable insert by time */
    script[i].t = t; script[i].line = l;
}

static void script_load(void) {
    const char *f = getenv("KLGLFW_SCRIPT");
    if (f && *f) {
        FILE *fp = fopen(f, "r");
        if (!fp) LOG("KLGLFW_SCRIPT %s: %s", f, strerror(errno));
        else { char buf[1024]; while (fgets(buf, sizeof buf, fp)) script_add(buf); fclose(fp); }
    }
    const char *ev = getenv("KLGLFW_EVENTS");
    if (ev && *ev) {
        char *dup = strdup(ev), *save = NULL;
        for (char *tok = strtok_r(dup, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) script_add(tok);
        free(dup);
    }
    if (n_script) LOG("script: %d line(s), first at t=%.2f, last at t=%.2f", n_script, script[0].t, script[n_script - 1].t);
}

static int action_of(const char *s) {
    if (!s) return 1;
    if (!strcmp(s, "release") || !strcmp(s, "0")) return 0;
    if (!strcmp(s, "repeat") || !strcmp(s, "2")) return 2;
    return 1;
}

static void script_exec_locked(const char *line) {
    char cmd[32] = {0}, a1[64] = {0}, a2[64] = {0}, a3[64] = {0}, a4[64] = {0};
    int n = sscanf(line, "%31s %63s %63s %63s %63s", cmd, a1, a2, a3, a4);
    if (n < 1) return;
    LOG("script t=%.2f: %s", now_s(), line);
    if (!strcmp(cmd, "key")) {
        push_locked(0, KLGLFW_EV_KEY, atoi(a1), n >= 5 ? atoi(a4) : 0, action_of(n >= 3 ? a2 : NULL), n >= 4 ? atoi(a3) : 0, 0, 0);
    } else if (!strcmp(cmd, "tap")) {
        int mods = n >= 3 ? atoi(a2) : 0;
        push_locked(0, KLGLFW_EV_KEY, atoi(a1), 0, 1, mods, 0, 0);
        push_locked(0, KLGLFW_EV_KEY, atoi(a1), 0, 0, mods, 0, 0);
    } else if (!strcmp(cmd, "char")) {
        push_locked(0, KLGLFW_EV_CHAR, atoi(a1), 0, 0, n >= 3 ? atoi(a2) : 0, 0, 0);
    } else if (!strcmp(cmd, "type")) {
        const char *p = line + 4; while (*p == ' ') p++;
        for (; *p; p++) push_locked(0, KLGLFW_EV_CHAR, (unsigned char)*p, 0, 0, 0, 0, 0);
    } else if (!strcmp(cmd, "button")) {
        push_locked(0, KLGLFW_EV_MOUSE_BUTTON, atoi(a1), action_of(n >= 3 ? a2 : NULL), n >= 4 ? atoi(a3) : 0, 0, 0, 0);
    } else if (!strcmp(cmd, "click")) {
        if (n >= 4) { S.cursor_x = atof(a2); S.cursor_y = atof(a3); push_locked(0, KLGLFW_EV_CURSOR_POS, 0, 0, 0, 0, S.cursor_x, S.cursor_y); }
        push_locked(0, KLGLFW_EV_MOUSE_BUTTON, atoi(a1), 1, 0, 0, 0, 0);
        push_locked(0, KLGLFW_EV_MOUSE_BUTTON, atoi(a1), 0, 0, 0, 0, 0);
    } else if (!strcmp(cmd, "move")) {
        S.cursor_x = atof(a1); S.cursor_y = atof(a2);
        push_locked(0, KLGLFW_EV_CURSOR_POS, 0, 0, 0, 0, S.cursor_x, S.cursor_y);
    } else if (!strcmp(cmd, "scroll")) {
        push_locked(0, KLGLFW_EV_SCROLL, 0, 0, 0, 0, atof(a1), atof(a2));
    } else if (!strcmp(cmd, "focus")) {
        push_locked(0, KLGLFW_EV_FOCUS, atoi(a1), 0, 0, 0, 0, 0);
    } else if (!strcmp(cmd, "close")) {
        push_locked(0, KLGLFW_EV_CLOSE, 0, 0, 0, 0, 0, 0);
    } else if (!strcmp(cmd, "shot")) {
        S.shot_pending = 1;
    } else if (!strcmp(cmd, "log")) {
        /* the LOG line above is the whole effect */
    } else {
        LOG("script: unknown command '%s'", cmd);
    }
}

static void script_run_due_locked(void) {
    double t = now_s();
    while (next_script < n_script && script[next_script].t <= t) script_exec_locked(script[next_script++].line);
}

/* ---- PNG (stored deflate, no zlib) -------------------------------------------------------------------------------- */
static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc_update(uint32_t c, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len) {
    unsigned char h[8]; be32(h, len); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc_update(0xffffffffu, (const unsigned char *)type, 4);
    c = crc_update(c, data, len) ^ 0xffffffffu;
    unsigned char t[4]; be32(t, c); fwrite(t, 1, 4, f);
}
/* rgba: bottom-up rows (glReadPixels); written top-down RGB. */
static int write_png(const char *path, const unsigned char *rgba, int w, int h) {
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    unsigned char *raw = malloc(raw_len);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        unsigned char *o = raw + (size_t)y * (1 + (size_t)w * 3);
        const unsigned char *s = rgba + (size_t)(h - 1 - y) * w * 4;
        *o++ = 0;
        for (int x = 0; x < w; x++) { *o++ = s[0]; *o++ = s[1]; *o++ = s[2]; s += 4; }
    }
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + nblocks * 5 + 4;
    unsigned char *z = malloc(z_len), *q = z;
    if (!z) { free(raw); return -1; }
    *q++ = 0x78; *q++ = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t off = 0; off < raw_len;) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        *q++ = off + n == raw_len;
        *q++ = n & 0xff; *q++ = n >> 8; *q++ = ~n & 0xff; *q++ = (~n >> 8) & 0xff;
        memcpy(q, raw + off, n);
        for (size_t i = 0; i < n; i++) { s1 = (s1 + raw[off + i]) % 65521; s2 = (s2 + s1) % 65521; }
        q += n; off += n;
    }
    be32(q, (s2 << 16) | s1); q += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return -1; }
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13]; be32(ihdr, w); be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    int rc = ferror(f) ? -1 : 0;
    fclose(f); free(raw); free(z);
    return rc;
}

static void take_shot(klglfw_window *w, const char *why) {
    if (!p_glReadPixels || !p_glGetIntegerv || !p_glBindFramebuffer || !p_glPixelStorei) { LOG("shot: GL functions missing"); return; }
    size_t n = (size_t)w->width * w->height * 4;
    unsigned char *px = malloc(n);
    if (!px) return;
    int rfb = 0, pbo = 0, al = 4, rl = 0, sr = 0, sp = 0;
    p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_, &rfb);
    p_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING_, &pbo);
    p_glGetIntegerv(GL_PACK_ALIGNMENT_, &al);
    p_glGetIntegerv(GL_PACK_ROW_LENGTH_, &rl);
    p_glGetIntegerv(GL_PACK_SKIP_ROWS_, &sr);
    p_glGetIntegerv(GL_PACK_SKIP_PIXELS_, &sp);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER_, 0);
    if (pbo && p_glBindBuffer) p_glBindBuffer(GL_PIXEL_PACK_BUFFER_, 0);
    p_glPixelStorei(GL_PACK_ALIGNMENT_, 1); p_glPixelStorei(GL_PACK_ROW_LENGTH_, 0);
    p_glPixelStorei(GL_PACK_SKIP_ROWS_, 0); p_glPixelStorei(GL_PACK_SKIP_PIXELS_, 0);
    memset(px, 0, n);
    p_glReadPixels(0, 0, w->width, w->height, GL_RGBA_, GL_UNSIGNED_BYTE_, px);
    p_glPixelStorei(GL_PACK_ALIGNMENT_, al); p_glPixelStorei(GL_PACK_ROW_LENGTH_, rl);
    p_glPixelStorei(GL_PACK_SKIP_ROWS_, sr); p_glPixelStorei(GL_PACK_SKIP_PIXELS_, sp);
    if (pbo && p_glBindBuffer) p_glBindBuffer(GL_PIXEL_PACK_BUFFER_, pbo);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER_, rfb);
    /* evidence summary: mean colour + share of non-black pixels */
    double sum[3] = {0}; size_t lit = 0, npx = (size_t)w->width * w->height;
    for (size_t i = 0; i < npx; i++) {
        unsigned char *p = px + i * 4;
        sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
        if (p[0] > 8 || p[1] > 8 || p[2] > 8) lit++;
    }
    char path[1200];
    snprintf(path, sizeof path, "%s/klglfw-%04lu-f%06lu-t%05.1f.png", S.shot_dir, S.shots++, S.frames, now_s());
    int rc = write_png(path, px, w->width, w->height);
    LOG("shot (%s) frame=%lu t=%.2f %dx%d -> %s%s  mean=(%.0f,%.0f,%.0f) nonblack=%.1f%%", why, S.frames, now_s(),
        w->width, w->height, path, rc ? " WRITE FAILED" : "", sum[0] / npx, sum[1] / npx, sum[2] / npx, 100.0 * lit / npx);
    free(px);
}

/* ---- helpers ------------------------------------------------------------------------------------------------------ */
static int parse_wxh(const char *s, int *w, int *h) {
    return s && sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}
static fn_t need_egl(const char *name, int *ok) {
    fn_t f = S.egl_gpa(name);
    if (!f) { LOG("EGL %s missing", name); *ok = 0; }
    return f;
}
static klglfw_window *W(jlong h) {
    klglfw_window *w = (klglfw_window *)(intptr_t)h;
    return w && w->magic == WIN_MAGIC ? w : NULL;
}
static jstring jstr(JNIEnv *env, const char *s) { return (*env)->NewStringUTF(env, s ? s : ""); }

/* ---- JNI ---------------------------------------------------------------------------------------------------------- */
#define JNI_FN(ret, name) JNIEXPORT ret JNICALL Java_org_lwjgl_glfw_KlGlfwNative_##name

JNI_FN(jboolean, nInit)(JNIEnv *env, jclass cls, jstring jgl) {
    (void)cls;
    if (S.inited) return JNI_TRUE;
    crc_init();
    mach_timebase_info(&S.tb);
    S.t0 = mach_absolute_time();
    const char *env_gl = getenv("KLGLFW_GL_LIBRARY");
    const char *gl = NULL;
    if (env_gl && *env_gl) gl = env_gl;
    else if (jgl) gl = (*env)->GetStringUTFChars(env, jgl, NULL);
    if (!gl || !*gl) {
        seterr("no GL library: set -Dorg.lwjgl.opengl.libname=<libltw.dylib> (or KLGLFW_GL_LIBRARY)");
        return JNI_FALSE;
    }
    snprintf(S.gl_path, sizeof S.gl_path, "%s", gl);
    if (gl != env_gl) (*env)->ReleaseStringUTFChars(env, jgl, gl);
    S.gl = dlopen(S.gl_path, RTLD_NOW | RTLD_GLOBAL);
    if (!S.gl) { seterr("dlopen %s: %s", S.gl_path, dlerror()); return JNI_FALSE; }
    S.egl_gpa = (gpa_t)dlsym(S.gl, "eglGetProcAddress");
    S.gl_gpa = (gpa_t)dlsym(S.gl, "OSMesaGetProcAddress");
    if (!S.gl_gpa) S.gl_gpa = (gpa_t)dlsym(S.gl, "glXGetProcAddress");
    if (!S.gl_gpa) S.gl_gpa = S.egl_gpa;
    if (!S.egl_gpa) { seterr("%s exports no eglGetProcAddress (not libltw/EGL?)", S.gl_path); return JNI_FALSE; }
    int ok = 1;
    p_eglGetDisplay = (void *)need_egl("eglGetDisplay", &ok);
    p_eglGetPlatformDisplayEXT = (void *)S.egl_gpa("eglGetPlatformDisplayEXT");
    p_eglInitialize = (void *)need_egl("eglInitialize", &ok);
    p_eglTerminate = (void *)need_egl("eglTerminate", &ok);
    p_eglBindAPI = (void *)need_egl("eglBindAPI", &ok);
    p_eglChooseConfig = (void *)need_egl("eglChooseConfig", &ok);
    p_eglCreatePbufferSurface = (void *)need_egl("eglCreatePbufferSurface", &ok);
    p_eglCreateWindowSurface = (void *)S.egl_gpa("eglCreateWindowSurface");
    p_eglDestroySurface = (void *)need_egl("eglDestroySurface", &ok);
    p_eglCreateContext = (void *)need_egl("eglCreateContext", &ok);
    p_eglDestroyContext = (void *)need_egl("eglDestroyContext", &ok);
    p_eglMakeCurrent = (void *)need_egl("eglMakeCurrent", &ok);
    p_eglSwapBuffers = (void *)need_egl("eglSwapBuffers", &ok);
    p_eglSwapInterval = (void *)need_egl("eglSwapInterval", &ok);
    p_eglGetError = (void *)need_egl("eglGetError", &ok);
    p_eglQueryString = (void *)need_egl("eglQueryString", &ok);
    if (!ok) { seterr("EGL entry points missing from %s", S.gl_path); return JNI_FALSE; }
    /* the ltw wrappers must be the ones in use (they register the context) */
    void *ltw_cc = dlsym(S.gl, "eglCreateContext"), *ltw_mc = dlsym(S.gl, "eglMakeCurrent");
    LOG("eglCreateContext %s, eglMakeCurrent %s", ltw_cc && (void *)p_eglCreateContext == ltw_cc ? "= GL library's own wrapper" : "NOT the GL library's export",
        ltw_mc && (void *)p_eglMakeCurrent == ltw_mc ? "= GL library's own wrapper" : "NOT the GL library's export");
    p_glGetIntegerv = (void *)S.gl_gpa("glGetIntegerv");
    p_glBindFramebuffer = (void *)S.gl_gpa("glBindFramebuffer");
    p_glBindBuffer = (void *)S.gl_gpa("glBindBuffer");
    p_glPixelStorei = (void *)S.gl_gpa("glPixelStorei");
    p_glReadPixels = (void *)S.gl_gpa("glReadPixels");
    p_glGetString = (void *)S.gl_gpa("glGetString");
    p_glGetStringi = (void *)S.gl_gpa("glGetStringi");

    const char *dd = getenv("KLGLFW_DEFAULT_DISPLAY");
    const char *host = getenv("KLGLFW_HOST");
    S.klepton = host && !strcmp(host, "klepton");
    { const char *vr = getenv("KLGLFW_VR"); S.vr = vr && *vr == '1'; }
    if (S.klepton && !p_eglCreateWindowSurface) { seterr("KLGLFW_HOST=klepton but no eglCreateWindowSurface"); return JNI_FALSE; }
    if (p_eglGetPlatformDisplayEXT && !(dd && *dd == '1') && !S.klepton) {
        const EGLint attr[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
        S.dpy = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, (void *)EGL_DEFAULT_DISPLAY, attr);
    } else {
        S.dpy = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (S.dpy == EGL_NO_DISPLAY) { seterr("no EGL display (0x%x)", p_eglGetError()); return JNI_FALSE; }
    if (!p_eglInitialize(S.dpy, &S.egl_major, &S.egl_minor)) { seterr("eglInitialize 0x%x", p_eglGetError()); return JNI_FALSE; }
    p_eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cattr[] = { EGL_SURFACE_TYPE, S.klepton ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                       EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLint n = 0;
    if (!p_eglChooseConfig(S.dpy, cattr, &S.cfg, 1, &n) || n < 1) {
        cattr[14] = EGL_NONE;   /* retry without stencil */
        if (!p_eglChooseConfig(S.dpy, cattr, &S.cfg, 1, &n) || n < 1) { seterr("no ES3 pbuffer config (0x%x)", p_eglGetError()); return JNI_FALSE; }
        LOG("config without stencil");
    }
    /* settings */
    const char *v;
    S.shot_every = (v = getenv("KLGLFW_SHOT_EVERY")) ? atol(v) : 0;
    snprintf(S.shot_dir, sizeof S.shot_dir, "%s", (v = getenv("KLGLFW_SHOT_DIR")) && *v ? v : ".");
    mkdir(S.shot_dir, 0755);
    if ((v = getenv("KLGLFW_SHOT_AT")) && *v) {
        char *dup = strdup(v), *save = NULL;
        for (char *t = strtok_r(dup, ",", &save); t && S.n_shot_at < 64; t = strtok_r(NULL, ",", &save)) S.shot_at[S.n_shot_at++] = atof(t);
        free(dup);
    }
    S.fps_log = (v = getenv("KLGLFW_FPS")) && *v == '1';
    S.log_events = (v = getenv("KLGLFW_LOG")) && *v == '1';
    script_load();
    S.inited = 1;
    LOG("init: GL library %s, EGL %d.%d (%s, %s)%s", S.gl_path, S.egl_major, S.egl_minor,
        p_eglQueryString(S.dpy, EGL_VENDOR), p_eglQueryString(S.dpy, EGL_VERSION),
        S.klepton ? ", host klepton: kl_egl display/config, window surface -> the app's compositor" : "");
    return JNI_TRUE;
}

JNI_FN(jstring, nLastError)(JNIEnv *env, jclass cls) { (void)cls; return jstr(env, last_error); }

JNI_FN(jstring, nDescribe)(JNIEnv *env, jclass cls) {
    (void)cls;
    char b[2048];
    snprintf(b, sizeof b, "libklglfw: EGL %d.%d via %s; ES 3.0 pbuffer contexts created+made current through its "
             "eglCreateContext/eglMakeCurrent; shots every=%ld at=%d dir=%s; fps log %s; script %d line(s)",
             S.egl_major, S.egl_minor, S.gl_path, S.shot_every, S.n_shot_at, S.shot_dir, S.fps_log ? "on" : "off", n_script);
    return jstr(env, b);
}

/* The host's "the game closed its window" callback (kl_app's quit grace: glfwTerminate is the last act of
 * Minecraft.close(), and main may then hang in the JVM's join of non-daemon threads). */
static void (*volatile terminate_hook)(void);
KL_EXPORT void klglfw_set_terminate_hook(void (*f)(void)) { terminate_hook = f; }

JNI_FN(void, nTerminate)(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    if (!S.inited) return;
    p_eglMakeCurrent(S.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    tls_current = NULL;
    LOG("terminate after %lu frames, %lu shots, %lu dropped events", S.frames, S.shots, S.dropped);
    if (terminate_hook) terminate_hook();
}

JNI_FN(jlong, nGetMonitorSize)(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    int w = 1920, h = 1080;
    parse_wxh(getenv("KLGLFW_MONITOR"), &w, &h);
    return ((jlong)w << 32) | (uint32_t)h;
}

JNI_FN(jlong, nCreateWindow)(JNIEnv *env, jclass cls, jint width, jint height, jlong share, jint maj, jint min, jint samples) {
    (void)env; (void)cls;
    if (!S.inited) { seterr("not initialised"); return 0; }
    int w = width > 0 ? width : 854, h = height > 0 ? height : 480;
    parse_wxh(getenv("KLGLFW_SIZE"), &w, &h);
    const EGLint sattr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    /* In the app the window is kl_egl's window surface: kl_egl sizes it from the activity window kl_app set to the
     * same KLGLFW_SIZE, and creating it is what flips kl_present to the flat (mono) picture. */
    EGLSurface surf = S.klepton ? p_eglCreateWindowSurface(S.dpy, S.cfg, (EGLNativeWindowType)0, NULL)
                                : p_eglCreatePbufferSurface(S.dpy, S.cfg, sattr);
    if (surf == EGL_NO_SURFACE) { seterr("eglCreate%sSurface %dx%d: 0x%x", S.klepton ? "Window" : "Pbuffer", w, h, p_eglGetError()); return 0; }
    klglfw_window *sh = W(share);
    /* Desktop GL version libltw reports (it has no other way to know what the game asked for). Minecraft 26.x
     * asks for 3.3 core and refuses a lower GL ("Driver does not support OpenGL 3.3"); 1.21.x asks for 3.2, which is
     * ltw's own default. So a 3.3+ request sets LTW_REPORT_GL_VERSION=3.3 unless the launcher already set it.
     * Must happen before the first glGetString/glGetIntegerv on the new context (ltw parses it once, lazily). */
    if ((maj > 3 || (maj == 3 && min >= 3)) && !getenv("LTW_REPORT_GL_VERSION")) {
        setenv("LTW_REPORT_GL_VERSION", "3.3", 0);
        LOG("asked GL %d.%d: LTW_REPORT_GL_VERSION=3.3 (ltw reports \"3.3 OpenLTW\", GLSL 3.30)", maj, min);
    }
    const EGLint xattr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLContext ctx = p_eglCreateContext(S.dpy, S.cfg, sh ? sh->context : EGL_NO_CONTEXT, xattr);
    if (ctx == EGL_NO_CONTEXT) { seterr("eglCreateContext ES 3.0: 0x%x", p_eglGetError()); p_eglDestroySurface(S.dpy, surf); return 0; }
    klglfw_window *win = calloc(1, sizeof *win);
    win->magic = WIN_MAGIC; win->width = w; win->height = h; win->surface = surf; win->context = ctx;
    pthread_mutex_lock(&S.mu);
    win->next = S.windows; S.windows = win;
    push_locked((int64_t)(intptr_t)win, KLGLFW_EV_FOCUS, 1, 0, 0, 0, 0, 0);
    push_locked((int64_t)(intptr_t)win, KLGLFW_EV_CURSOR_ENTER, 1, 0, 0, 0, 0, 0);
    pthread_mutex_unlock(&S.mu);
    LOG("window %p: %dx%d %s, ES 3.0 context %p (asked GL %d.%d, %d samples: ignored)%s", (void *)win, w, h,
        S.klepton ? "kl_egl window surface" : "pbuffer", ctx,
        maj, min, samples, sh ? ", shared" : "");
    return (jlong)(intptr_t)win;
}

JNI_FN(void, nDestroyWindow)(JNIEnv *env, jclass cls, jlong h) {
    (void)env; (void)cls;
    klglfw_window *w = W(h);
    if (!w) return;
    if (tls_current == w) { p_eglMakeCurrent(S.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); tls_current = NULL; }
    p_eglDestroyContext(S.dpy, w->context);
    p_eglDestroySurface(S.dpy, w->surface);
    pthread_mutex_lock(&S.mu);
    for (klglfw_window **p = &S.windows; *p; p = &(*p)->next) if (*p == w) { *p = w->next; break; }
    pthread_mutex_unlock(&S.mu);
    w->magic = 0;
    free(w);
}

JNI_FN(jlong, nGetFramebufferSize)(JNIEnv *env, jclass cls, jlong h) {
    (void)env; (void)cls;
    klglfw_window *w = W(h);
    return w ? ((jlong)w->width << 32) | (uint32_t)w->height : 0;
}

JNI_FN(jboolean, nMakeContextCurrent)(JNIEnv *env, jclass cls, jlong h) {
    (void)env; (void)cls;
    klglfw_window *w = W(h);
    EGLBoolean ok = w ? p_eglMakeCurrent(S.dpy, w->surface, w->surface, w->context)
                      : p_eglMakeCurrent(S.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (!ok) { seterr("eglMakeCurrent(%p): 0x%x", (void *)w, p_eglGetError()); return JNI_FALSE; }
    tls_current = w;
    static int once;
    if (w && !once && p_glGetString) {
        once = 1;
        LOG("current: GL_VERSION \"%s\", GL_RENDERER \"%s\"", p_glGetString(GL_VERSION_), p_glGetString(GL_RENDERER_));
    }
    return JNI_TRUE;
}

JNI_FN(jlong, nGetCurrentContext)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return (jlong)(intptr_t)tls_current; }

JNI_FN(void, nSwapBuffers)(JNIEnv *env, jclass cls, jlong h) {
    (void)env; (void)cls;
    klglfw_window *w = W(h);
    if (!w) return;
    S.frames++;
    if (tls_current == w) {
        double t = now_s();
        if (S.shot_every > 0 && S.frames % S.shot_every == 0) take_shot(w, "every");
        else if (S.next_shot_at < S.n_shot_at && t >= S.shot_at[S.next_shot_at]) { S.next_shot_at++; take_shot(w, "at"); }
        else if (S.shot_pending) { S.shot_pending = 0; take_shot(w, "script"); }
    }
    /* VR (KLGLFW_VR=1): the picture leaves through xrEndFrame (kl_openxr), so the flat window is not presented --
       eglSwapBuffers on kl_egl's window surface would read fb 0 back into the mono window every frame for nobody.
       Vivecraft still draws its desktop mirror into fb 0; KLGLFW_SHOT_* screenshots above still see it. */
    if (!S.vr) p_eglSwapBuffers(S.dpy, w->surface);
    else {
        /* ...and nothing else throttles a swap then. Vivecraft skips the XR frame (so xrWaitFrame's pacing) on some
           screens -- Create World, the level-loading screens -- and measured on the sim that ran the render loop at
           370-630 fps for nothing. Cap it (KLGLFW_VR_MAXFPS, default 90): when xrWaitFrame paces, this never sleeps. */
        static double last, min_dt = -1;
        if (min_dt < 0) { const char *m = getenv("KLGLFW_VR_MAXFPS"); double f = m ? atof(m) : 90.0; min_dt = f > 0 ? 1.0 / f : 0; }
        double t = now_s(), wait = last + min_dt - t;
        if (wait > 0 && wait < 1.0) { usleep((useconds_t)(wait * 1e6)); t = now_s(); }
        last = t;
    }
    if (S.fps_log) {
        double t = now_s();
        if (t - S.fps_t >= 1.0) {
            LOG("fps t=%.0fs frames=%lu fps=%.1f", t, S.frames, (S.frames - S.fps_frames_at) / (t - S.fps_t));
            S.fps_t = t; S.fps_frames_at = S.frames;
        }
    }
}

JNI_FN(void, nSwapInterval)(JNIEnv *env, jclass cls, jint i) { (void)env; (void)cls; if (S.inited) p_eglSwapInterval(S.dpy, i); }

JNI_FN(jlong, nGetProcAddress)(JNIEnv *env, jclass cls, jstring jname) {
    (void)cls;
    if (!S.gl_gpa || !jname) return 0;
    const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
    fn_t f = S.gl_gpa(name);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    return (jlong)(intptr_t)f;
}

JNI_FN(jboolean, nExtensionSupported)(JNIEnv *env, jclass cls, jstring jname) {
    (void)cls;
    if (!tls_current || !p_glGetStringi || !p_glGetIntegerv || !jname) return JNI_FALSE;
    const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
    int n = 0, found = 0;
    p_glGetIntegerv(GL_NUM_EXTENSIONS_, &n);
    for (int i = 0; i < n && !found; i++) { const char *e = (const char *)p_glGetStringi(GL_EXTENSIONS_, i); found = e && !strcmp(e, name); }
    (*env)->ReleaseStringUTFChars(env, jname, name);
    return found ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jint, nPollEvents)(JNIEnv *env, jclass cls, jlong buf, jint cap) {
    (void)env; (void)cls;
    struct klglfw_event *out = (struct klglfw_event *)(intptr_t)buf;
    int n = 0;
    pthread_mutex_lock(&S.mu);
    script_run_due_locked();
    while (n < cap && S.head != S.tail) out[n++] = S.ring[S.head++ % RING_N];
    S.woken = 0;
    pthread_mutex_unlock(&S.mu);
    return n;
}

JNI_FN(void, nWaitEvents)(JNIEnv *env, jclass cls, jdouble timeout) {
    (void)env; (void)cls;
    pthread_mutex_lock(&S.mu);
    double t = now_s(), until = timeout >= 0 ? t + timeout : 1e300;
    if (next_script < n_script && script[next_script].t < until) until = script[next_script].t;
    while (S.head == S.tail && !S.woken && (t = now_s()) < until) {
        double left = until - t;
        if (left > 1.0) left = 1.0;     /* re-check the clock at least once a second */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long long ns = ts.tv_nsec + (long long)(left * 1e9);
        ts.tv_sec += ns / 1000000000LL; ts.tv_nsec = ns % 1000000000LL;
        pthread_cond_timedwait(&S.cv, &S.mu, &ts);
    }
    S.woken = 0;
    pthread_mutex_unlock(&S.mu);
}

JNI_FN(void, nPostEmptyEvent)(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    pthread_mutex_lock(&S.mu); S.woken = 1; pthread_cond_broadcast(&S.cv); pthread_mutex_unlock(&S.mu);
}

JNI_FN(void, nSetCursorPos)(JNIEnv *env, jclass cls, jlong h, jdouble x, jdouble y) {
    (void)env; (void)cls; (void)h;
    S.cursor_x = x; S.cursor_y = y;
}

JNI_FN(void, nSetInputMode)(JNIEnv *env, jclass cls, jlong h, jint mode, jint value) {
    (void)env; (void)cls; (void)h;
    if (mode == 0x33001 /* GLFW_CURSOR */) {
        if (S.cursor_mode != value) LOG("cursor mode 0x%x (%s)", value, value == 0x34003 ? "grabbed" : "free");
        S.cursor_mode = value;
    }
}

JNI_FN(jdouble, nGetTime)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return now_s() + S.offset; }
JNI_FN(void, nSetTime)(JNIEnv *env, jclass cls, jdouble t) { (void)env; (void)cls; S.offset = t - now_s(); }
JNI_FN(jlong, nGetTimerValue)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return (jlong)ns_since_init(); }

JNI_FN(jlong, nGetEGLHandle)(JNIEnv *env, jclass cls, jlong h, jint which) {
    (void)env; (void)cls;
    klglfw_window *w = W(h);
    if (!w) w = S.windows;
    switch (which) {
        case 0: return (jlong)(intptr_t)S.dpy;
        case 1: return w ? (jlong)(intptr_t)w->context : 0;
        case 2: return w ? (jlong)(intptr_t)w->surface : 0;
        case 3: return (jlong)(intptr_t)S.cfg;
        default: return 0;
    }
}

KL_EXPORT void *klglfw_egl_handle(int which) {
    klglfw_window *w = S.windows;
    switch (which) {
        case 0: return S.dpy;
        case 1: return w ? w->context : NULL;
        case 2: return w ? w->surface : NULL;
        case 3: return S.cfg;
        default: return NULL;
    }
}
