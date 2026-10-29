// The one-eye reference renderer, on ANGLE's Metal backend. It produces the
// known-good frame a real backend is diffed against; it is not the shipping
// backend. Opt in with KL_GLFB=1 — the null driver in kl_egl.c stays the
// default, so `make check` is unaffected by anything here.
//
// The Metal backend must be asked for by name. eglGetDisplay(EGL_DEFAULT_DISPLAY)
// selects ANGLE's OpenGL backend on macOS, which inherits desktop GL's limits
// and rejects ETC2 0x9279 — a format mandatory in the GLES 3.0 the guest speaks
// (`make angle`). EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE accepts it with shaders
// unmodified, so there is no shader rewrite, no format substitution, and no
// framebuffer impersonation: the guest's default framebuffer is a genuine EGL
// pbuffer, binding 0 means what it says, and GL_BACK is a valid buffer name.
//
// ABI hazard, about Darwin rather than about GL: the guest is AAPCS64 and ANGLE
// is a Darwin arm64 dylib. They agree on x0-x7 and disagree on the stack —
// AAPCS64 gives every stack argument an 8-byte slot, Darwin packs each to its
// natural size — so the first stack argument lands in the same place and
// everything after it can diverge. glBlitFramebuffer(8 ints, mask, filter) is
// the live case: the guest writes filter at sp+8, a Darwin callee reads sp+4.
// The wrappers below declare those parameters 64-bit so Darwin reproduces the
// AAPCS64 layout, then truncate. The int64_t parameters holding obviously
// 32-bit values are load-bearing; removing the widening silently corrupts the
// argument after it.
#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>          // powf — the debug tone map in the capture path
#include <time.h>          // clock_gettime — the link-in-flight clock
#include <stdatomic.h>     // the link-in-flight record, read off-thread
#include <zlib.h>
// The decoded-video image path — an AHardwareBuffer here is a
// CVPixelBuffer, and what ANGLE can take is the IOSurface behind it.
#include <CoreVideo/CoreVideo.h>
// The flat door's zero-readback storage. IOSurfaceRef.h by name: the umbrella
// IOSurface.h is macOS-only, and visionOS ships only the Ref/Types split.
#include <IOSurface/IOSurfaceRef.h>
#include "kl_glfb.h"
#include "kl_cvmtl.h"      // native-format video frames as MTLTextures
#include "kl_present.h"
#include "kl_egl.h"        // kl_gl_cap_* — the capability tables
#include "kl_reproject.h"  // kl_reproject_set_srgb_decode — see klfb_srgb_settle
#include "kl_fault.h"      // kl_fault_print_frames — who issued a GL call
#include "klepton.h"       // kl_trace_stub, for the per-name GL call trace
#include "kl_perf.h"       // KL_PERF=1: the GPU clock on ANGLE's MTLDevice too

// ---- the EGL/GLES constants used here, so there is nothing to include ----
#define EGL_DEFAULT_DISPLAY  ((void *)0)
#define EGL_NO_CONTEXT       ((void *)0)
#define EGL_NONE             0x3038
#define EGL_WIDTH            0x3057
#define EGL_HEIGHT           0x3056
#define EGL_SURFACE_TYPE     0x3033
#define EGL_PBUFFER_BIT      0x0001
#define EGL_RENDERABLE_TYPE  0x3040
#define EGL_OPENGL_ES3_BIT   0x0040
#define EGL_RED_SIZE         0x3024
#define EGL_GREEN_SIZE       0x3023
#define EGL_BLUE_SIZE        0x3022
#define EGL_ALPHA_SIZE       0x3021
#define EGL_DEPTH_SIZE       0x3025
#define EGL_STENCIL_SIZE     0x3026
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_ANGLE_ANGLE             0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE        0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE  0x3489

#define GL_NO_ERROR      0
#define GL_RGBA          0x1908
#define GL_TEXTURE_2D    0x0DE1
#define GL_SRGB8_ALPHA8  0x8C43
#define GL_RGBA8         0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT         0x1406
#define GL_RENDERER      0x1F01
#define GL_VERSION       0x1F02
#define GL_EXTENSIONS    0x1F03

// We use the VENDORED ANGLE, always — `make angle-debug` (which pulls and
// patches the checkout first). It is not interchangeable with a stock build:
// angle-patches/klepton.patch raises the Metal backend's kMaxShaderSamplers
// from 16 to 32 because Unity's HLSLCC-baked sampler bindings reach unit 35 on
// the post-processing passes, and a stock ANGLE fails GL-side validation there.
//
// So there is deliberately NO silent fallback to some other ANGLE on the
// machine. Loading Chrome's prebuilt because ours was missing would trade a
// clear "you have not built ANGLE yet" for a rendering bug hunted somewhere
// else entirely. KL_ANGLE_DIR still overrides, for pointing at a different
// build ON PURPOSE.
#define ANGLE_VENDORED_DIR "vendor/out/Debug"

static const char *kl_angle_dir(void) {
    const char *dir = kl_env_str("KL_ANGLE_DIR", NULL);
    if (dir) return dir;
    return ANGLE_VENDORED_DIR;
}

// ANGLE arrives in two shapes and the port needs both: a bare dylib on macOS,
// and a .framework bundle on visionOS (ANGLE emits ios_framework_bundle for
// iOS-family targets, and an app may only load code from inside its bundle
// anyway). Try both rather than making the caller know which.
//
// Note libEGL then finds libGLESv2 *itself* on iOS-family platforms — it dlopens
// <exec dir>/Frameworks/libGLESv2.framework/libGLESv2 (ANGLE's
// system_utils_posix.cpp) — so on the device both frameworks must be embedded,
// and this loader's own libGLESv2 open resolves to that same image.
static void *angle_dlopen(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.dylib", dir, name);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h) return h;
    snprintf(path, sizeof path, "%s/%s.framework/%s", dir, name, name);
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static void *g_egl_lib, *g_gles_lib;
static void *g_dpy, *g_surf, *g_ctx, *g_cfg;
// Kept so per-thread contexts can be created later, after init has chosen a config.
static void *(*a_eglCreateContext)(void *, void *, void *, const int32_t *);
static void *(*a_eglCreatePbufferSurface)(void *, void *, const int32_t *);
static int   g_on = -1, g_ready;
static int   g_w = 4000, g_h = 3200;      // one eye, Quest 2 per-eye default

// The flat door's zero-readback storage: when requested (before init), the
// root surface is a pbuffer over an IOSurface WE allocate, so the guest's
// default framebuffer IS shareable storage — the viewer (and one day the app)
// wraps the same IOSurface in an MTLTexture and samples it, and the display
// path stops calling glReadPixels entirely. Single-buffered by construction
// (an EGL surface has one storage), so a compositor sampling mid-frame can see
// a partial 2D UI — accepted for the pairing shell, where the alternative was
// a full-frame readback and a pipeline stall every swap.
static int          g_flat_request;
static IOSurfaceRef g_flat_ios;
static unsigned g_presented;

int kl_glfb_enabled(void) {
    // Value-aware, not presence-aware. Default off, which keeps every host loop
    // and `make check` exactly as they were — but the visionOS app now turns
    // this on for itself (kl_app_configure), so `KL_GLFB=0` has to be a real way
    // back to the null driver rather than a second way to say "on".
    if (g_on < 0) g_on = kl_env_on("KL_GLFB", 0);
    return g_on;
}

void kl_glfb_request_flat_surface(void) {
    if (g_ready) {
        fprintf(stderr, "  [glfb] flat-surface request arrived after ANGLE was "
                        "up — too late, the root surface exists\n");
        return;
    }
    g_flat_request = 1;
}

void *kl_glfb_flat_surface(int *w, int *h) {
    if (!g_flat_ios) return NULL;
    if (w) *w = g_w;
    if (h) *h = g_h;
    return (void *)g_flat_ios;
}

void kl_glfb_set_size(int w, int h) {
    // KL_GLFB_SIZE=WxH overrides the eye size the guest asked for. Every spike that
    // failed to reproduce the AGX abort used a 256x256 pbuffer while the guest uses
    // 1832x1920, and each thread gets its own with depth and stencil — so surface
    // size is the one resource axis none of them varied. Shrinking it here tests
    // that without touching anything else.
    const char *env = kl_env_str("KL_GLFB_SIZE", NULL);
    if (env) {
        int ew = 0, eh = 0;
        if (sscanf(env, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) { w = ew; h = eh; }
    }
    if (w > 0 && h > 0 && !g_ready) { g_w = w; g_h = h; }
}

static void *asym(const char *n) {
    void *p = g_gles_lib ? dlsym(g_gles_lib, n) : NULL;
    if (!p && g_egl_lib) p = dlsym(g_egl_lib, n);
    return p;
}

// The few ANGLE entry points this file drives itself, as opposed to the ones it
// hands straight to the guest.
static uint32_t (*a_glGetError)(void);
static void     (*a_glReadPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t,
                                  uint32_t, void *);
static void     (*a_glFinish)(void);
static void     (*a_glGetIntegerv)(uint32_t, int32_t *);
// ERRSCAN's framebuffer report (below): what GL itself thinks of the target a
// draw just failed against.
static uint32_t (*a_glCheckFramebufferStatus)(uint32_t);
static void     (*a_glGetFramebufferAttachmentParameteriv)(uint32_t, uint32_t,
                                                           uint32_t, int32_t *);
static const uint8_t *(*a_glGetString)(uint32_t);
static unsigned (*a_eglMakeCurrent)(void *, void *, void *, void *);

static void klfb_selftest(void);

// KL_GLFB_DEBUG_CB=1 registers this with glDebugMessageCallback and prints
// every message. The vendored debug ANGLE speaks KHR_debug fluently, so a
// guest call failing silently (the "0 lit" frame was one) comes with its
// reason here instead of needing an error-code probe per call site.
static void klfb_debug_cb(uint32_t source, uint32_t type, uint32_t id,
                          uint32_t severity, int32_t length, const char *msg,
                          const void *user) {
    (void)source; (void)type; (void)id; (void)severity; (void)user;
    fprintf(stderr, "  [glcb] %.*s\n", length, msg ? msg : "");
}

// ---------------------------------------------------------- synchronous links
//
// r10, on device: the render thread wedged for 33 s inside
//   gl::Program::MainLinkLoadEvent::wait -> angle::AsyncWaitableEvent::wait
// reached from our own glLinkProgram's GL_LINK_STATUS query. The guest mutex
// that thread held then stopped the whole Unreal run — 0 presented frames, one
// eglSwapBuffers, a compositor iterating 4339 times with nothing to show, and
// the immersive space open over an empty world. The same binary on the host
// (same ANGLE git hash, different OS build and GPU) ran to the menu, so this is
// a timing race in ANGLE's parallel link worker, not a shader the Metal backend
// cannot compile.
//
// KHR_parallel_shader_compile's glMaxShaderCompilerThreadsKHR(0) removes the
// async event entirely: ANGLE's pool goes single-threaded and the link task runs
// inline on the calling thread. The total work is unchanged — the caller was
// blocking on that task anyway — so the only thing given up is overlap the guest
// was never collecting.
//
// This is per-CONTEXT state, exactly like the KHR_debug callback below it, so
// the root-context call in kl_glfb_init does not cover the contexts UE4 makes
// for its own threads — and context 0x2, one of those, is where the wedge was.
// kl_glfb_make_current re-applies it whenever a thread's context changes.
//
// KL_GL_LINK_THREADS=N asks for N worker threads instead; N<0 leaves ANGLE's
// default untouched, which is the way back to the r10 behaviour.
static void klfb_set_link_threads(int announce) {
    int n = kl_env_int("KL_GL_LINK_THREADS", 0);
    if (n < 0) {
        if (announce)
            fprintf(stderr, "  [glfb] shader compiler threads: ANGLE's default "
                            "(KL_GL_LINK_THREADS<0)\n");
        return;
    }
    void (*maxthreads)(uint32_t) = asym("glMaxShaderCompilerThreadsKHR");
    if (!maxthreads) maxthreads = asym("glMaxShaderCompilerThreadsANGLE");
    if (!maxthreads) {
        if (announce)
            fprintf(stderr, "  [glfb] no glMaxShaderCompilerThreadsKHR in this "
                            "ANGLE — links stay asynchronous\n");
        return;
    }
    maxthreads((uint32_t)n);
    if (announce) {
        const char *pext = a_glGetString
                             ? (const char *)a_glGetString(GL_EXTENSIONS) : NULL;
        fprintf(stderr, "  [glfb] shader compiler threads -> %d%s "
                        "(KHR_parallel_shader_compile advertised: %s)\n", n,
                n == 0 ? " — links resolve inline, which is the r10 device wedge"
                         " removed" : "",
                pext && strstr(pext, "GL_KHR_parallel_shader_compile")
                    ? "yes" : "no");
    }
}

int kl_glfb_init(void) {
    if (!kl_glfb_enabled() || g_ready) return g_ready;

    // Applied here rather than only in kl_glfb_set_size, which this path never
    // calls — the eye size stays at its 1832x1920 default unless OVRPlugin says
    // otherwise, so a setter-only override silently does nothing.
    const char *size_env = kl_env_str("KL_GLFB_SIZE", NULL);
    if (size_env) {
        int ew = 0, eh = 0;
        if (sscanf(size_env, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) {
            g_w = ew; g_h = eh;
        }
    }

    const char *dir = kl_angle_dir();
    g_egl_lib  = angle_dlopen(dir, "libEGL");
    g_gles_lib = angle_dlopen(dir, "libGLESv2");
    if (!g_egl_lib || !g_gles_lib) {
        fprintf(stderr, "  [glfb] cannot load ANGLE from %s\n         (%s)\n"
                        "         build it with 'make angle-debug' — that pulls and "
                        "patches the checkout too.\n"
                        "         (KL_ANGLE_DIR overrides, but a stock ANGLE is not "
                        "equivalent: see angle-patches/.)\n"
                        "         staying on the null driver\n", dir, dlerror());
        return 0;
    }

    void *(*eglGetPlatformDisplayEXT)(uint32_t, void *, const int32_t *) =
        asym("eglGetPlatformDisplayEXT");
    unsigned (*eglInitialize)(void *, int32_t *, int32_t *) = asym("eglInitialize");
    unsigned (*eglChooseConfig)(void *, const int32_t *, void **, int32_t, int32_t *) =
        asym("eglChooseConfig");
    a_eglCreatePbufferSurface = asym("eglCreatePbufferSurface");
    a_eglCreateContext        = asym("eglCreateContext");
    a_eglMakeCurrent = asym("eglMakeCurrent");
    if (!eglGetPlatformDisplayEXT || !eglInitialize || !eglChooseConfig ||
        !a_eglCreatePbufferSurface || !a_eglCreateContext || !a_eglMakeCurrent) {
        fprintf(stderr, "  [glfb] ANGLE is missing core EGL entry points\n");
        return 0;
    }

    // Metal by name. The default display selects ANGLE's OpenGL backend, and with
    // it every limitation this move exists to escape.
    const int32_t dpy_attrs[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
        EGL_NONE,
    };
    g_dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY,
                                     dpy_attrs);
    int32_t major = 0, minor = 0;
    if (!g_dpy || !eglInitialize(g_dpy, &major, &minor)) {
        fprintf(stderr, "  [glfb] eglInitialize failed\n");
        return 0;
    }

    // The eye. A real pbuffer, so the guest's framebuffer 0 is a real default
    // framebuffer — no FBO impersonation, and GL_BACK means what it says.
    const int32_t cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE,
    };
    void *cfg = NULL; int32_t ncfg = 0;
    if (!eglChooseConfig(g_dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "  [glfb] no ES3 pbuffer config\n");
        return 0;
    }
    g_cfg = cfg;
    const int32_t surf_attrs[] = { EGL_WIDTH, g_w, EGL_HEIGHT, g_h, EGL_NONE };
    // The flat door's surface first, when asked for: a pbuffer over an
    // IOSurface we allocate. Every failure falls through to the plain pbuffer
    // — the accessor answering NULL is how the caller knows to keep the
    // readback path — and MakeCurrent gets its own retry below because a
    // surface that CREATES but cannot go current would otherwise take the
    // whole renderer down for what is only a display optimisation.
    if (g_flat_request) {
        void *(*mkclient)(void *, uint32_t, void *, void *, const int32_t *) =
            asym("eglCreatePbufferFromClientBuffer");
        if (mkclient) {
            // Zeroed at birth (lock + memset) so the first composited frame is
            // black rather than whatever the allocator held.
            CFMutableDictionaryRef props = CFDictionaryCreateMutable(
                kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
            int32_t iw = g_w, ih = g_h, bpe = 4, pfmt = 'BGRA';
            CFNumberRef n;
            n = CFNumberCreate(NULL, kCFNumberSInt32Type, &iw);
            CFDictionarySetValue(props, kIOSurfaceWidth, n); CFRelease(n);
            n = CFNumberCreate(NULL, kCFNumberSInt32Type, &ih);
            CFDictionarySetValue(props, kIOSurfaceHeight, n); CFRelease(n);
            n = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe);
            CFDictionarySetValue(props, kIOSurfaceBytesPerElement, n); CFRelease(n);
            n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pfmt);
            CFDictionarySetValue(props, kIOSurfacePixelFormat, n); CFRelease(n);
            g_flat_ios = IOSurfaceCreate(props);
            CFRelease(props);
            if (g_flat_ios) {
                IOSurfaceLock(g_flat_ios, 0, NULL);
                memset(IOSurfaceGetBaseAddress(g_flat_ios), 0,
                       IOSurfaceGetAllocSize(g_flat_ios));
                IOSurfaceUnlock(g_flat_ios, 0, NULL);
                // The same six-attribute contract the video image states, with
                // the width and height this surface will be rendered at.
                const int32_t io_attrs[] = {
                    EGL_WIDTH,  g_w,
                    EGL_HEIGHT, g_h,
                    0x345A /* EGL_IOSURFACE_PLANE_ANGLE */,           0,
                    0x3081 /* EGL_TEXTURE_TARGET */,                  0x305F /* EGL_TEXTURE_2D */,
                    0x3080 /* EGL_TEXTURE_FORMAT */,                  0x305E /* EGL_TEXTURE_RGBA */,
                    0x345C /* EGL_TEXTURE_TYPE_ANGLE */,              0x1401 /* GL_UNSIGNED_BYTE */,
                    0x345D /* EGL_TEXTURE_INTERNAL_FORMAT_ANGLE */,   0x80E1 /* GL_BGRA_EXT */,
                    EGL_NONE,
                };
                g_surf = mkclient(g_dpy, 0x3454 /* EGL_IOSURFACE_ANGLE */,
                                  (void *)g_flat_ios, cfg, io_attrs);
                if (!g_surf) {
                    fprintf(stderr, "  [glfb] IOSurface pbuffer refused — the flat "
                                    "door keeps the readback path\n");
                    CFRelease(g_flat_ios);
                    g_flat_ios = NULL;
                }
            }
        }
        if (g_surf)
            fprintf(stderr, "  [glfb] flat surface is a %dx%d IOSurface pbuffer — "
                            "the picture is shareable storage, nothing reads back\n",
                    g_w, g_h);
    }
    if (!g_surf) g_surf = a_eglCreatePbufferSurface(g_dpy, cfg, surf_attrs);
    const int32_t ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g_ctx  = a_eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g_surf && g_ctx && g_flat_ios && !a_eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        // The IOSurface surface created but cannot go current: retire it and
        // retry on an ordinary pbuffer before declaring the renderer down.
        fprintf(stderr, "  [glfb] the IOSurface pbuffer cannot go current — "
                        "falling back to a plain pbuffer (readback path)\n");
        unsigned (*dsurf)(void *, void *) = asym("eglDestroySurface");
        if (dsurf) dsurf(g_dpy, g_surf);
        CFRelease(g_flat_ios);
        g_flat_ios = NULL;
        g_surf = a_eglCreatePbufferSurface(g_dpy, cfg, surf_attrs);
    }
    if (!g_surf || !g_ctx || !a_eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        fprintf(stderr, "  [glfb] could not make an ES3 context current\n");
        return 0;
    }

    a_glGetError   = asym("glGetError");
    a_glReadPixels = asym("glReadPixels");
    a_glFinish     = asym("glFinish");
    a_glGetIntegerv = asym("glGetIntegerv");
    a_glCheckFramebufferStatus = asym("glCheckFramebufferStatus");
    a_glGetFramebufferAttachmentParameteriv =
        asym("glGetFramebufferAttachmentParameteriv");
    a_glGetString  = asym("glGetString");
    g_ready = 1;

    // KL_GLFB_DEBUG_CB=1: register a KHR_debug callback and print every
    // message. The vendored debug ANGLE speaks KHR_debug fluently, so a guest
    // call failing silently (the "0 lit" frame was one) comes with its reason
    // here instead of needing an error-code probe per call site.
    if (kl_env_on("KL_GLFB_DEBUG_CB", 0)) {
        void (*glDebugMessageCallback)(const void *, const void *) =
            asym("glDebugMessageCallback");
        void (*glEnable)(uint32_t) = asym("glEnable");
        if (glDebugMessageCallback) {
            glDebugMessageCallback((const void *)klfb_debug_cb, NULL);
            // GL_DEBUG_OUTPUT 0x92E0 — synchronous delivery.
            if (glEnable) glEnable(0x92E0);
            fprintf(stderr, "  [glfb] KHR_debug callback registered\n");
        } else {
            fprintf(stderr, "  [glfb] no glDebugMessageCallback in this ANGLE\n");
        }
    }
    // ---------------------------------------------------- synchronous linking
    //
    // r10, on device: the render thread wedged for 33 s inside
    //   gl::Program::MainLinkLoadEvent::wait -> angle::AsyncWaitableEvent::wait
    // reached from our own glLinkProgram's GL_LINK_STATUS query, after ~409
    // programs had linked cleanly. The guest mutex that thread held then
    // stopped the whole Unreal run: 0 presented frames, one eglSwapBuffers, a
    // compositor iterating 4339 times with nothing to show. The same binary on
    // the host (same ANGLE git hash, different OS build and GPU) ran 56k
    // frames, so this is a timing race in ANGLE's parallel link worker, not a
    // shader the backend cannot compile.
    //
    // KHR_parallel_shader_compile's glMaxShaderCompilerThreadsKHR(0) removes
    // the async event entirely: ANGLE's pool goes single-threaded and the link
    // task runs inline on the calling thread. The total work is unchanged --
    // the caller was blocking on that task anyway -- so the only thing given up
    // is overlap the guest was never collecting.
    //
    // KL_GL_LINK_THREADS=N asks for N worker threads instead; N<0 leaves
    // ANGLE's default untouched, which is the way back to the r10 behaviour.
    klfb_set_link_threads(1);
    fprintf(stderr, "  [glfb] ANGLE %dx%d — %s\n", g_w, g_h,
            a_glGetString ? (const char *)a_glGetString(GL_RENDERER) : "?");
    fprintf(stderr, "  [glfb] %s\n",
            a_glGetString ? (const char *)a_glGetString(GL_VERSION) : "?");
    // Whether a half-float eye texture can be RENDERED to, which on an ES 3.0
    // context is an extension question and not a version one: RGBA16F is
    // texture-filterable in core ES 3.0 but only colour-renderable with
    // EXT_color_buffer_(half_)float. Printed because the failure mode is a
    // single engine-side line ("RenderTexture.Create failed: format
    // unsupported - RGBA16 SFloat") a long way from anything GL, and the
    // engine's own answer depends on which of the two it looks for -- Unity
    // 2019.4 infers it from the version it is told, 2018.4 asks for the string.
    if (a_glGetString) {
        const char *ext = (const char *)a_glGetString(GL_EXTENSIONS);
        if (!ext) ext = "";
        fprintf(stderr, "  [glfb] renderable float: EXT_color_buffer_float=%s "
                        "EXT_color_buffer_half_float=%s\n",
                strstr(ext, "GL_EXT_color_buffer_float") ? "yes" : "NO",
                strstr(ext, "GL_EXT_color_buffer_half_float") ? "yes" : "NO");
        // ...and the compressed formats, for the same reason and with the same
        // failure shape a long way from GL: a UE4 title cooked for ASTC that
        // does not find the ASTC extension in the string puts up its
        // "Unsupported Texture Format — Cooked Flavors: ASTC / Supported: ETC2"
        // message box and then goes looking for an ETC2 pak that was never
        // built. What the guest is TOLD is kl_egl's list, not this one; this
        // line is the control that says what the backend can actually do.
        fprintf(stderr, "  [glfb] compressed: KHR_texture_compression_astc_ldr=%s "
                        "OES_compressed_ETC1_RGB8_texture=%s\n",
                strstr(ext, "GL_KHR_texture_compression_astc_ldr") ? "yes" : "NO",
                strstr(ext, "GL_OES_compressed_ETC1_RGB8_texture") ? "yes" : "NO");

        // ...and the same question for the extensions the description is
        // allowed to REPEAT rather than assert. Everything adopted here was
        // named by the line above's own string; the print is the record of what
        // this device answered, because the whole texture budget of the srceng
        // port turns on the s3tc half of it (kl_egl.c) and neither half is
        // visible anywhere else in a Home-View run's log.
        kl_gl_cap_adopt_backend_extensions(ext);
        int adopted = kl_gl_cap_adopted_extension_count();
        fprintf(stderr, "  [glfb] backend extensions adopted: %d%s", adopted,
                adopted ? " —" : " (none of the candidates are present)");
        for (int i = 0; i < adopted; i++)
            fprintf(stderr, " %s", kl_gl_cap_adopted_extension(i));
        fprintf(stderr, "\n");
        fprintf(stderr, "  [glfb] DXT/BC texture upload: %s\n",
                kl_gl_cap_backend_has_dxt()
                    ? "AVAILABLE — the guest may upload compressed blocks"
                    : "NOT available — a DXT guest will decompress on the CPU "
                      "and upload RGBA8888 (4-8x the bytes)");
    }

    // Release it. EGL binds a context to one thread at a time and *refuses* to
    // migrate it — unlike CGL, which silently allows it. Leaving it current on
    // whichever thread happened to resolve the first GL symbol meant the guest's
    // render thread could never claim it (EGL_BAD_ACCESS), so every guest GL call
    // ran with no context: silently, since that is not an error, just nothing.
    // Whoever asks next through kl_glfb_make_current gets it.
    a_eglMakeCurrent(g_dpy, NULL, NULL, NULL);
    klfb_selftest();
    return 1;
}

// A GL context is current per *thread*, and Unity drives GL from its own render
// thread rather than the one that created the context. Calling in from a thread
// with no current context dereferences a null dispatch table — which presented as
// a SIGSEGV at 0x2d8 inside glFlush when this was first wired up. eglMakeCurrent
// is the guest telling us which thread now owns the context, so it is the hook.
//
// Default mode (since r6): ONE real ANGLE context per GUEST EGLContext, all in
// one share group. This is what EGL actually says, and it is the only model
// that expresses BOTH guests we have:
//
//   * Unity makes exactly ONE of its EGLContexts current, and migrates it
//     between the setup thread and the render thread by releasing it on one
//     (eglMakeCurrent(NULL)) before taking it on the next. Under this model it
//     maps to ONE real context that migrates the same way — bit for bit the old
//     "migration mode" behaviour, including the FBO container semantics it
//     depends on (measured by KL_GLFB_TRACE_FBO: Unity creates its framebuffer
//     objects during setup and draws with them on the render thread; FBOs are
//     container objects and are never shared, so any model that hands the
//     render thread a DIFFERENT real context sees them as empty objects,
//     "incomplete: no attachments", every draw -> 0x506).
//
//   * UE4 creates three EGLContexts on the game thread and then makes context 1
//     current on the RHIThread WHILE the game thread still holds one. That is
//     legal EGL — separate contexts in one share group, one per thread — and it
//     is exactly what the old migration mode could not express: it refused the
//     second thread, which then issued GL with no current context, ANGLE
//     answered a map with NULL, and UE4 memcpy'd into it (SIGSEGV in
//     _platform_memmove on the RHIThread, r5b). Under this model each guest
//     context has its own real context, so both threads are served and neither
//     sees the other's container objects — which is what UE4 expects.
//
// Two guest threads asking for the SAME guest context without a release in
// between is still refused, because that is genuinely illegal EGL
// (EGL_BAD_ACCESS) and no model can serve it.
//
// KL_GLFB_SHARED=1 survives as an INSTRUMENT, not a mode anyone should ship: a
// context per THREAD regardless of which guest context it asked for. It is
// subsumed for correctness by the default above, and it stays only because the
// tools built on it (the draw census, the lock trampolines, KL_GLFB_NOSHARE_OBJ)
// still answer questions about ANGLE's own threading.
//
// Every non-root context gets its own eye-sized pbuffer, so whichever thread
// draws has a real default framebuffer of the right size — and the capture,
// which runs on that same thread, reads exactly what that thread drew.
static pthread_key_t g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

typedef struct { void *guest, *ctx, *surf; int probed; int debug_cb;
                 void *link_ctx; } klfb_thread_gl;

static void klfb_tls_free(void *p) { free(p); }
static void klfb_tls_init(void) { pthread_key_create(&g_tls_key, klfb_tls_free); }

// The guest-EGLContext -> real-ANGLE-context table. `owner` is the thread id
// that currently has this context current and has not released it, 0 when free.
// The FIRST guest context to be made current is backed by the root context that
// kl_glfb_init already created, so the single-context guest (Unity, every GL
// title before tetris) takes the identical code path it always did.
#define KLFB_MAX_GUEST_CTX 8
static struct { void *guest, *ctx, *surf; uint64_t owner; } g_gctx[KLFB_MAX_GUEST_CTX];
static unsigned g_ngctx;
static pthread_mutex_t g_gctx_lock = PTHREAD_MUTEX_INITIALIZER;

// Caller holds g_gctx_lock. Returns the row for `guest`, creating its real
// context on first sight, or NULL if the table is full / creation failed.
static int klfb_gctx_slot(void *guest) {
    for (unsigned i = 0; i < g_ngctx; i++)
        if (g_gctx[i].guest == guest) return (int)i;
    if (g_ngctx >= KLFB_MAX_GUEST_CTX) {
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "  [glfb] more than %d guest EGLContexts — the extras "
                            "share the root context\n", KLFB_MAX_GUEST_CTX); }
        return -1;
    }
    unsigned i = g_ngctx;
    g_gctx[i].guest = guest;
    if (i == 0) {
        g_gctx[i].ctx = g_ctx; g_gctx[i].surf = g_surf;
        fprintf(stderr, "  [glfb] guest context %p -> the root ANGLE context\n", guest);
    } else {
        const int32_t surf_attrs[] = { EGL_WIDTH, g_w, EGL_HEIGHT, g_h, EGL_NONE };
        const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        g_gctx[i].surf = a_eglCreatePbufferSurface(g_dpy, g_cfg, surf_attrs);
        g_gctx[i].ctx  = a_eglCreateContext(g_dpy, g_cfg, g_ctx, ctx_attrs);
        fprintf(stderr, "  [glfb] guest context %p -> a new ANGLE context %p in the "
                        "root share group (%s)\n", guest, g_gctx[i].ctx,
                g_gctx[i].ctx && g_gctx[i].surf ? "ok" : "FAILED");
        if (!g_gctx[i].ctx || !g_gctx[i].surf) { g_gctx[i].guest = NULL; return -1; }
    }
    g_ngctx = i + 1;
    return (int)i;
}

void kl_glfb_make_current(void *guest_ctx) {
    if (!g_ready || !a_eglMakeCurrent) return;
    pthread_once(&g_tls_once, klfb_tls_init);

    klfb_thread_gl *t = pthread_getspecific(g_tls_key);
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
    if (!t) {
        t = calloc(1, sizeof *t);
        if (!t) return;
        pthread_setspecific(g_tls_key, t);
    }

    if (kl_env_on("KL_GLFB_SHARED", 0)) {
        // The instrument: a context per THREAD, whatever the guest asked for.
        if (!t->ctx) {
            const int32_t surf_attrs[] = { EGL_WIDTH, g_w, EGL_HEIGHT, g_h, EGL_NONE };
            const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            t->surf = a_eglCreatePbufferSurface(g_dpy, g_cfg, surf_attrs);
            // KL_GLFB_NOSHARE_OBJ=1 gives the thread its own context that shares
            // *nothing*. The guest then renders with objects it cannot see, so the
            // picture is meaningless — but it answers one question cleanly: whether
            // the AGX abort needs object sharing at all, or merely a second thread
            // issuing GL. Rendering correctness is not the point of this knob.
            void *share = kl_env_on("KL_GLFB_NOSHARE_OBJ", 0) ? EGL_NO_CONTEXT : g_ctx;
            t->ctx  = a_eglCreateContext(g_dpy, g_cfg, share, ctx_attrs);
            fprintf(stderr, "  [glfb] thread %llu gets %s (KL_GLFB_SHARED)\n",
                    (unsigned long long)tid, t->ctx ? "its own context" : "NOTHING");
        }
    } else {
        pthread_mutex_lock(&g_gctx_lock);
        int s = klfb_gctx_slot(guest_ctx);
        if (s < 0) s = g_ngctx ? 0 : -1;          // fall back to the root row
        if (s < 0) { pthread_mutex_unlock(&g_gctx_lock); return; }

        // Switching contexts on this thread implicitly releases the previous
        // one, exactly as eglMakeCurrent does.
        if (t->guest && t->guest != g_gctx[s].guest)
            for (unsigned i = 0; i < g_ngctx; i++)
                if (g_gctx[i].guest == t->guest && g_gctx[i].owner == tid)
                    g_gctx[i].owner = 0;

        if (g_gctx[s].owner && g_gctx[s].owner != tid) {
            // The SAME guest context on two threads at once. Illegal EGL —
            // eglMakeCurrent would answer EGL_BAD_ACCESS — so refuse, and say
            // what a refusal costs: a thread with no current context does not
            // merely draw nothing, every GL call that returns a pointer returns
            // NULL and the guest faults memcpy'ing into it.
            t->guest = NULL; t->ctx = NULL; t->surf = NULL;
            static int warned;
            if (!warned) { warned = 1;
                fprintf(stderr, "  [glfb] thread %llu wants guest context %p while "
                                "thread %llu still holds it — REFUSED (that is "
                                "EGL_BAD_ACCESS); this thread now has NO context, "
                                "so any GL call returning a pointer returns NULL\n",
                        (unsigned long long)tid, guest_ctx,
                        (unsigned long long)g_gctx[s].owner);
            }
            pthread_mutex_unlock(&g_gctx_lock);
            return;
        }
        g_gctx[s].owner = tid;
        t->guest = g_gctx[s].guest;
        t->ctx   = g_gctx[s].ctx;
        t->surf  = g_gctx[s].surf;
        pthread_mutex_unlock(&g_gctx_lock);
    }
    if (!t->ctx || !t->surf) return;
    // KL_GLFB_PROBE=1 runs the self-test's trivial clear+readback on each GUEST
    // thread as it takes a context, once. The host-thread self-test already passes
    // in this same process, so this is the discriminator that remains: if the same
    // three calls fail here, the guest's *thread environment* is at fault (its TLS
    // slot, its x18 veneers, its stack) and the elaborate GL sequence is a red
    // herring; if they pass, the environment is fine and it really is the sequence.
    // It clears the guest's framebuffer, so it is a probe and not a mode.
    if (kl_env_on("KL_GLFB_PROBE", 0) && !t->probed) {
        t->probed = 1;
        if (a_eglMakeCurrent(g_dpy, t->surf, t->surf, t->ctx)) {
            uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
            void (*cc)(float, float, float, float) = asym("glClearColor");
            void (*cl)(uint32_t) = asym("glClear");
            fprintf(stderr, "  [glfb] probe: guest thread %llu about to glClear\n",
                    (unsigned long long)tid);
            if (cc) cc(0.25f, 0.5f, 0.75f, 1.0f);
            if (cl) cl(0x4000);
            if (a_glFinish) a_glFinish();
            unsigned char px[16] = {0};
            if (a_glReadPixels) a_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            fprintf(stderr, "  [glfb] probe: guest thread %llu readback %u,%u,%u,%u"
                            " — trivial GL works on a guest thread\n",
                    (unsigned long long)tid, px[0], px[1], px[2], px[3]);
        }
    }
    if (!a_eglMakeCurrent(g_dpy, t->surf, t->surf, t->ctx)) {
        uint32_t (*eglGetError)(void) = asym("eglGetError");
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [glfb] eglMakeCurrent failed even with a per-thread "
                            "context (egl error 0x%x)\n", eglGetError ? eglGetError() : 0);
        }
    } else {
        if (t->link_ctx != t->ctx) {
            // Per-context state, and the context that wedged in r10 was one of
            // UE4's own — see the block above klfb_set_link_threads.
            t->link_ctx = t->ctx;
            klfb_set_link_threads(0);
        }
        if (kl_env_on("KL_GLFB_DEBUG_CB", 0) && !t->debug_cb) {
            // The debug callback is per-context state, so the root-context
            // registration in kl_glfb_init does not cover this thread's context.
            t->debug_cb = 1;
            void (*glDebugMessageCallback)(const void *, const void *) =
                asym("glDebugMessageCallback");
            void (*glEnable)(uint32_t) = asym("glEnable");
            if (glDebugMessageCallback) {
                glDebugMessageCallback((const void *)klfb_debug_cb, NULL);
                if (glEnable) glEnable(0x92E0);
            }
        }
    }
}

// The guest's eglMakeCurrent(NULL) — the release half. Frees this thread's
// guest context for whichever thread takes it next and detaches the real
// context here. Called from klegl_MakeCurrent when the guest makes no context
// current, and from the visionOS handoff.
void kl_glfb_release_current(void) {
    if (!g_ready || !a_eglMakeCurrent) return;
    pthread_once(&g_tls_once, klfb_tls_init);
    klfb_thread_gl *t = pthread_getspecific(g_tls_key);
    if (!t || !t->ctx) return;
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
    if (kl_env_on("KL_GLFB_SHARED", 0)) return;   // per-thread contexts never migrate
    pthread_mutex_lock(&g_gctx_lock);
    for (unsigned i = 0; i < g_ngctx; i++)
        if (g_gctx[i].guest == t->guest && g_gctx[i].owner == tid)
            g_gctx[i].owner = 0;
    pthread_mutex_unlock(&g_gctx_lock);
    a_eglMakeCurrent(g_dpy, NULL, NULL, NULL);
    t->guest = NULL;
    t->ctx = NULL;
    t->surf = NULL;
}

// ------------------------------------------- ANGLE's per-thread TLS, on the HOST
//
// A DEBUG-ANGLE requirement, not a translation decision. Every generated GLES
// entry point in vendor/out/Debug opens with
//
//     ASSERT(egl::Display::GetCurrentThreadUnlockedTailCall()->empty());
//
// and `GetCurrentThreadUnlockedTailCall` is `&GetDisplayTLS()->unlockedTailCall`
// with no null check (vendor/src/libANGLE/Display.cpp). GetDisplayTLS() answers
// NULL until `AllocateCurrentThread()` has run on the thread, and that only runs
// out of `egl::GetCurrentThread()` — i.e. out of an EGL entry point. So a guest
// thread that issues its first GL call without ever having touched EGL
// dereferences NULL inside libGLESv2 before the entry point has even looked at
// the context.
//
// SHAR does exactly that: the thread SDL runs the game on calls glUniform4f
// while loading art\L1_TERRA.p3d, having never called eglMakeCurrent (the
// context was made current on the thread that created it), and the host run died
// at the level load every time — signal 11 in
// `std::vector<std::function<void(void*)>>::empty()`, lr in
// `angle::UnlockedTailCall::any()`.
//
// The device never sees it: vendor/out/xros is a RELEASE build whose GL_Uniform4f
// has no such prologue. This is host-only breakage that was hiding the host from
// the only bug we could reproduce there.
//
// The fix is one EGL call per guest thread. eglGetCurrentContext is the cheapest
// entry point that still runs AllocateCurrentThread (and therefore
// Display::InitTLS): it makes nothing current, takes no lock the guest cares
// about and answers EGL_NO_CONTEXT on a thread that holds none. KL_GLFB_THREAD_TLS=0
// backs it out.
void kl_glfb_thread_tls_touch(void) {
    if (!g_ready) return;                       // no ANGLE yet: nothing to seed
    if (!kl_env_on("KL_GLFB_THREAD_TLS", 1)) return;
    static void *(*a_eglGetCurrentContext)(void);
    if (!a_eglGetCurrentContext)
        a_eglGetCurrentContext = (void *(*)(void))asym("eglGetCurrentContext");
    if (a_eglGetCurrentContext) (void)a_eglGetCurrentContext();
}

// ------------------------------------------------- the shared-context compile lock
//
// A MITIGATION, not a fix. This build of ANGLE is not thread-safe across shared
// contexts; spikes/s10_shared.c reproduces it with no guest at all — three host
// threads, shared ANGLE contexts, nothing of Klepton linked:
//
//   no lock                        10/30 runs crash
//   lock around compile + link      2/60      <- ~10x better, not a cure
//   lock around the draws only      1/30
//   serialised entirely             0/40
//   one thread only                 0/40
//   concurrency without sharing     1/20      <- not concurrency as such
//
// Neither sharing nor concurrency alone: the combination. The race presents two
// ways from one code path — SIGSEGV inside libGLESv2, and a libmalloc "pointer
// being freed was not allocated" abort (heap corruption in ANGLE's own state).
// Both arrive via GL_DrawArrays and share interior frames.
//
// Compile and link carry most of the risk, ANGLE doing its heavy shared-object
// work at link time, and the improvement survives making the draws overlap
// afterwards (30 draw iterations per thread: 7/15 crashes without, 0/15 with).
// The only configurations clean at 40 runs are the ones that never use shared
// contexts concurrently, which is what the renderer must do — so the lock
// guards a real latent race without making the sharing path safe. Cheap,
// compilation not being hot.
//
// A workaround for the host driver, not a translation decision: nothing about
// the guest needs it, and a real backend does not inherit it.
static pthread_mutex_t g_compile_lock = PTHREAD_MUTEX_INITIALIZER;

static void (*g_real_CompileShader)(uint32_t);
static void (*g_real_LinkProgram)(uint32_t);
static void (*g_real_ShaderSource)(uint32_t, int32_t, const char *const *,
                                   const int32_t *);
static void (*g_real_GetShaderiv)(uint32_t, uint32_t, int32_t *);
static void (*g_real_GetShaderInfoLog)(uint32_t, int32_t, int32_t *, char *);

// The shader capture, and the version rewrite it forced.
//
// Every glShaderSource is kept, keyed by shader name; on a failed compile the
// info log and the source that produced it are printed, and written to
// KL_DUMP_SHADERS/<dir> if one is set.
//
// The gap it exposed: kl_egl describes the device as GLES 3.2 / GLSL ES 3.20 —
// a deliberate group answer, since Unity cross-checks the description against
// itself and gates B10G11R11 renderability on the version number, so describing
// the 3.0 the driver actually is kills post-processing anyway. Unity therefore
// emits "#version 320 es" variants, and the ANGLE context here is ES 3.0 (its
// Metal backend caps there), which accepts only #version 100/300 es. Every 320
// es source failed: 7016 compiles in one run over two distinct texts, retried
// forever.
//
// The rewrite repairs the text at the boundary between what is described and
// what ANGLE implements. Three mechanical moves, verified against the vendored
// ANGLE via build/s09_angle:
//
//   1. "#version 3xx es" with xx > 00 -> "#version 300 es". Nothing in the
//      failing corpus uses a real 3.1/3.2 feature; a later shader that does
//      fails here with the true error, which the failure reporter prints.
//   2. drop "binding = <tok>" from layout qualifiers. layout(binding=N) on
//      uniform blocks is an ES 3.1 feature ("only valid when used with
//      pixel local storage" on this ANGLE). Dropping it is safe because
//      Unity associates UBOs with glUniformBlockBinding/glBindBufferBase
//      explicitly — it must, the same HLSLCC output also targets devices
//      whose GLSL has no binding qualifier at all. <tok> is not always a
//      digit: Unity's own macro is `layout(binding = x, std140)`.
//   3. drop explicit uniform locations ("layout(location = N) uniform" and
//      the UNITY_LOCATION(N) spelling) — likewise ES 3.1-only on a uniform,
//      and a semantic no-op for Unity, which resolves sampler locations
//      through glGetUniformLocation either way. in/out declarations keep
//      their location qualifiers: those are program inputs/outputs, where
//      the qualifier is legal ES 3.0.
//
// Both rewrite the captured copy, so the failure reporter and the
// KL_DUMP_SHADERS output show what ANGLE actually compiled.
// 4096, not 1024 (r26): this title sources more than fourteen hundred shaders
// on a cold cache, so the old cap dropped the whole tail of the library — and
// the tail is where the material shaders are. Everything that turns a program
// number back into text (KL_GLFB_DUMP_PROGRAM, the link-failure reporter,
// FILLHIST's culprit dump) reads this table, and all three answered "(source
// not captured)" for exactly the shaders worth reading. The cost is the sources
// themselves, tens of MB against a 4.5 GB margin.
#define KLFB_MAX_SHADERS 4096
static struct { uint32_t name; char *src; } g_fb_shaders[KLFB_MAX_SHADERS];
static unsigned g_fb_nshaders;

// A GLSL identifier character — the test that keeps a rename inside whole words.
#define KLFB_IDENT(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || \
                       ((c) >= '0' && (c) <= '9') || (c) == '_')

// ---- explicit uniform locations: remembered, not merely thrown away --------
//
// The strip below is required — layout(location=N) on a *uniform* is ES 3.1
// syntax and the ANGLE context behind this renderer is ES 3.0 — and for Unity
// it is a semantic no-op, because Unity resolves every uniform by name through
// glGetUniformLocation. Steam Link does not. Measured: it does not call
// glGetUniformLocation ONCE in a whole run, on either driver. It writes to the
// numbers it pinned in the shader text.
//
// So stripping the pins silently re-points every uniform the guest sets. Its
// pointer shader pins `length` at 1 and the linker's own order puts a mat4
// there, which at least raises GL_INVALID_OPERATION (~12 a frame, glutils.cpp:
// 205, and that error is what led here). The video shader is the one that
// matters and it fails without a sound: it pins
// `uniform samplerExternalOES tex0` at 2, a sampler written with glUniform1i
// takes any integer without complaint, and a sampler that never receives its
// texture unit reads unit 0 — so the quad that should show the decoded stream
// samples something else entirely, and every eye texture reads back black.
//
// The pin is therefore recorded here, resolved to the linker's own location at
// glLinkProgram, and applied to every glUniform* call. The guest keeps its own
// numbering throughout and never learns ours.
#define KLFB_PIN_SHADERS 64
#define KLFB_PINS        32
static struct { uint32_t shader; unsigned n;
                struct { int32_t at; char name[40]; } p[KLFB_PINS]; }
    g_shader_pins[KLFB_PIN_SHADERS];
static unsigned g_shader_pins_n;

static int klfb_pin_slot(uint32_t shader) {
    for (unsigned i = 0; i < g_shader_pins_n; i++)
        if (g_shader_pins[i].shader == shader) return (int)i;
    if (g_shader_pins_n >= KLFB_PIN_SHADERS) return -1;
    int i = (int)g_shader_pins_n++;
    g_shader_pins[i].shader = shader;
    g_shader_pins[i].n = 0;
    return i;
}

// A shader name can be re-sourced, and the rewrite runs once per
// glShaderSource — so the second source must replace the first's pins, not
// append to them.
static void klfb_pin_reset(uint32_t shader) {
    for (unsigned i = 0; i < g_shader_pins_n; i++)
        if (g_shader_pins[i].shader == shader) g_shader_pins[i].n = 0;
}

static void klfb_pin_note(uint32_t shader, int32_t at, const char *name) {
    int i = klfb_pin_slot(shader);
    if (i < 0 || g_shader_pins[i].n >= KLFB_PINS) return;
    unsigned k = g_shader_pins[i].n++;
    g_shader_pins[i].p[k].at = at;
    snprintf(g_shader_pins[i].p[k].name, sizeof g_shader_pins[i].p[k].name,
             "%s", name);
}

// If the layout qualifier starting at `p` ("layout(...)") directly precedes a
// `uniform` declaration, record the pin, then remove the qualifier (and the
// spaces between) in place and return the new scan position; otherwise return
// NULL. in/out declarations keep their qualifiers — those ARE program
// inputs/outputs and ES 3.0 wants them, so they never reach the strip.
static char *klfb_strip_uniform_layout(char *p, uint32_t shader) {
    char *close = strchr(p, ')');
    if (!close) return NULL;
    char *q = close + 1;
    while (*q == ' ' || *q == '\t') q++;
    if (strncmp(q, "uniform", 7) != 0) return NULL;

    // The number, before it goes: "layout(location = N)" and Unity's
    // "UNITY_LOCATION(N)" both put it between the parens.
    int32_t at = -1;
    {
        const char *loc = NULL;
        for (const char *d = p; d + 8 <= close; d++)
            if (strncmp(d, "location", 8) == 0) { loc = d + 8; break; }
        if (!loc && strncmp(p, "UNITY_LOCATION(", 15) == 0) loc = p + 14;
        while (loc && loc < close && (*loc == ' ' || *loc == '=' || *loc == '('))
            loc++;
        if (loc && loc < close && *loc >= '0' && *loc <= '9')
            at = (int32_t)strtol(loc, NULL, 10);
    }
    // ...and the name it pins: the identifier that ends the declaration, before
    // any array subscript. Read backwards from the ';' rather than counting
    // type tokens forwards, because the type may carry a precision qualifier
    // ("uniform mediump sampler2D _MainTex;").
    const char *semi = at >= 0 && shader ? strchr(q, ';') : NULL;
    if (semi) {
        const char *e = semi;
        while (e > q && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if (e > q && e[-1] == ']') {                  // an array: back over "[n]"
            const char *b = e;
            while (b > q && b[-1] != '[') b--;
            if (b > q) {
                e = b - 1;
                while (e > q && (e[-1] == ' ' || e[-1] == '\t')) e--;
            }
        }
        const char *s = e;
        while (s > q && KLFB_IDENT(s[-1])) s--;
        if (e > s && (size_t)(e - s) < 40) {
            char name[40];
            memcpy(name, s, (size_t)(e - s));
            name[e - s] = 0;
            klfb_pin_note(shader, at, name);
        }
    }
    memmove(p, q, strlen(q) + 1);
    return p;
}

// GLSL built-in function names that this guest also declares as variables, with
// the replacement each gets. The replacements are the same width as the names
// they replace, so the rename stays an in-place edit like every other rule in
// klfb_rewrite_glsl. Shared with klfb_GetUniformLocation, which is what keeps a
// renamed uniform findable under the name the guest wrote.
static const struct { const char *from, *to; } g_glsl_builtin_vars[] = {
    {"length", "kl_len"},
};

// ...and the other end of the pin: what the linker actually chose.
//
// Built at glLinkProgram, because that is the first moment a name has a
// location at all, and read by every glUniform* thunk. A program the guest
// pinned nothing in gets no entry, and g_prog_pins_n == 0 is the fast out for
// a guest that pins nothing anywhere.
//
// `byname` is the half this shipped without, and it cost Beat Saber's menu UI.
// The remap is only ever correct for a guest that WRITES TO THE NUMBER IT
// PINNED; a guest that asks the driver where a uniform went is being told our
// linker's answer, and putting that answer back through the pin table is the
// same bug in the mirror. Unity does both — it emits UNITY_LOCATION(n) pins AND
// resolves every uniform by name — so on Beat Saber the remap re-pointed the
// locations the driver itself had just handed out. The reason it was not caught:
// the by-name door was assumed to be glGetUniformLocation, and UNITY 2019.4 DOES
// NOT USE IT. It goes through the ES 3.1 program-interface family
// (glGetProgramResourceLocation), which is emulated further down this file and
// resolves to exactly the same driver call. Both doors set the flag now.
#define KLFB_PIN_PROGS 64
static struct { uint32_t prog; unsigned n; int byname;
                struct { int32_t pinned, actual; } m[KLFB_PINS * 2]; }
    g_prog_pins[KLFB_PIN_PROGS];
static unsigned g_prog_pins_n;

// Counted so the choice is visible in the end-of-run report rather than
// inferred from a picture: a pin table that never fires and one that re-points
// half the guest's uniforms look identical from outside.
static unsigned g_pin_byname_progs, g_pin_remap_hits, g_pin_remap_changed;

// The guest asked the driver where `program`'s uniforms are. Its pins are a
// statement about the shader text, not about what it is going to write to.
static void klfb_pins_byname(uint32_t program) {
    for (unsigned i = 0; i < g_prog_pins_n; i++)
        if (g_prog_pins[i].prog == program) {
            if (!g_prog_pins[i].byname) {
                g_prog_pins[i].byname = 1;
                g_pin_byname_progs++;
            }
            return;
        }
}

static int32_t (*g_real_GetUniformLocation)(uint32_t, const char *);

// The location ANGLE gave `name`, allowing for the built-in rename above — a
// uniform called `length` is in the program as `kl_len`, and the pin still
// names it the way the shader author wrote it.
static int32_t klfb_actual_loc(uint32_t prog, const char *name) {
    if (!g_real_GetUniformLocation) return -1;
    int32_t loc = g_real_GetUniformLocation(prog, name);
    if (loc >= 0) return loc;
    for (size_t i = 0; i < sizeof g_glsl_builtin_vars / sizeof g_glsl_builtin_vars[0]; i++)
        if (strcmp(name, g_glsl_builtin_vars[i].from) == 0)
            return g_real_GetUniformLocation(prog, g_glsl_builtin_vars[i].to);
    return -1;
}

static void klfb_pins_link(uint32_t program) {
    if (!g_shader_pins_n) return;
    static void (*r_GetAttachedShaders)(uint32_t, int32_t, int32_t *, uint32_t *);
    if (!r_GetAttachedShaders) r_GetAttachedShaders = asym("glGetAttachedShaders");
    // The guest may never resolve glGetUniformLocation — this one does not —
    // so the thunk's real slot can still be empty here. Resolve it ourselves.
    if (!g_real_GetUniformLocation)
        g_real_GetUniformLocation = (void *)asym("glGetUniformLocation");
    if (!r_GetAttachedShaders || !g_real_GetUniformLocation) return;

    uint32_t sh[8]; int32_t ns = 0;
    r_GetAttachedShaders(program, 8, &ns, sh);
    if (ns <= 0) return;

    unsigned slot = 0;
    for (; slot < g_prog_pins_n; slot++)
        if (g_prog_pins[slot].prog == program) break;
    if (slot == g_prog_pins_n) {
        if (g_prog_pins_n >= KLFB_PIN_PROGS) return;
        g_prog_pins_n++;
    }
    g_prog_pins[slot].prog = program;
    g_prog_pins[slot].n = 0;

    unsigned lost = 0;
    for (int32_t i = 0; i < ns && i < 8; i++)
        for (unsigned s = 0; s < g_shader_pins_n; s++) {
            if (g_shader_pins[s].shader != sh[i]) continue;
            for (unsigned k = 0; k < g_shader_pins[s].n; k++) {
                const char *name = g_shader_pins[s].p[k].name;
                int32_t at = g_shader_pins[s].p[k].at;
                int32_t actual = klfb_actual_loc(program, name);
                if (actual < 0) { lost++; continue; }   // optimised away
                if (g_prog_pins[slot].n < KLFB_PINS * 2) {
                    g_prog_pins[slot].m[g_prog_pins[slot].n].pinned = at;
                    g_prog_pins[slot].m[g_prog_pins[slot].n].actual = actual;
                    g_prog_pins[slot].n++;
                }
                // An array pinned at N occupies N, N+1, ... and the guest may
                // write any of them; the linker's own elements are found the
                // same way the spec says the guest would.
                for (int e = 1; e < 16; e++) {
                    char el[48];
                    snprintf(el, sizeof el, "%s[%d]", name, e);
                    int32_t a = klfb_actual_loc(program, el);
                    if (a < 0) break;
                    if (g_prog_pins[slot].n >= KLFB_PINS * 2) break;
                    g_prog_pins[slot].m[g_prog_pins[slot].n].pinned = at + e;
                    g_prog_pins[slot].m[g_prog_pins[slot].n].actual = a;
                    g_prog_pins[slot].n++;
                }
            }
        }
    if (g_prog_pins[slot].n || lost)
        fprintf(stderr, "  [glfb] program %u pins %u uniform location%s "
                        "explicitly%s — honoured\n", program,
                g_prog_pins[slot].n, g_prog_pins[slot].n == 1 ? "" : "s",
                lost ? " (plus some the linker optimised away)" : "");
}

// Which program is current, per thread — glUniform* writes into it, and the
// remap is program-scoped. Maintained by klfb_UseProgram rather than queried,
// because a glGetIntegerv under every uniform call is not free and this is on
// the guest's per-draw path.
static __thread uint32_t g_cur_prog;

// The guest's location -> the linker's. Unpinned locations pass through: a
// guest that pins some uniforms and looks up others is asking about ours in
// the second case, and rewriting those would be the same bug in the mirror.
static int32_t klfb_remap_loc(int32_t loc) {
    if (!g_prog_pins_n || loc < 0 || !g_cur_prog) return loc;
    for (unsigned i = 0; i < g_prog_pins_n; i++) {
        if (g_prog_pins[i].prog != g_cur_prog) continue;
        if (g_prog_pins[i].byname) return loc;   // it is quoting our own answer
        for (unsigned j = 0; j < g_prog_pins[i].n; j++)
            if (g_prog_pins[i].m[j].pinned == loc) {
                g_pin_remap_hits++;
                if (g_prog_pins[i].m[j].actual != loc) g_pin_remap_changed++;
                return g_prog_pins[i].m[j].actual;
            }
        return loc;
    }
    return loc;
}

// How much room past the source's own length klfb_ShaderSource allocates, so a
// rule can GROW the text a little. Every rule but one shrinks or preserves
// length and edits in place; the ESSL 3.10 varying rule below needs one byte to
// turn `0` into `0u`, and a rule that silently ran off the end would corrupt
// the heap rather than fail a compile.
#define KLFB_GLSL_SLACK 256

// ---- texture buffers, which ANGLE's Metal backend does not have -------------
//
// UE4 cooks `Buffer<float2>` as a `samplerBuffer` read with a flat integer
// index. GL_EXT_texture_buffer is CORE in ES 3.2, which is the level kl_egl
// describes, so the guest is entitled to it — and the vendored ANGLE has no
// texture-buffer support anywhere in the Metal backend, so the declaration is a
// reserved word and the shader does not compile. One failed link is a Fatal in
// UE4 and a Fatal is a ForceQuit, so a single such permutation ends the run.
//
// A buffer texture is a 1-D array of texels with no filtering and no wrap, and
// that is expressible as a 2-D texture of fixed width read with texelFetch and
// no sampling state — the index just splits into (i & (W-1), i >> log2 W). The
// GL side (klfb_TexBuffer, below) does the other half: it copies the buffer's
// bytes into that 2-D texture through GL_PIXEL_UNPACK_BUFFER, which is an
// ES 3.0 core server-side copy and needs no readback.
#define KLFB_TEXBUF_W      2048
#define KLFB_TEXBUF_SHIFT  11
#define KLFB_TEXBUF_MASK   2047

// Length-preserving: the replacement is shorter and the remainder is padded
// with spaces, so no offset in the buffer moves.
static void klfb_texbuf_retype(char *buf) {
    char *p = buf;
    while ((p = strstr(p, "samplerBuffer"))) {
        int pre = (p > buf && (p[-1] == 'i' || p[-1] == 'u')) ? 1 : 0;
        char *start = p - pre;
        const char *repl = pre ? (p[-1] == 'i' ? "isampler2D" : "usampler2D")
                               : "sampler2D";
        size_t oldlen = strlen("samplerBuffer") + (size_t)pre;
        size_t newlen = strlen(repl);
        memcpy(start, repl, newlen);
        memset(start + newlen, ' ', oldlen - newlen);
        p = start + oldlen;
    }
}

// texelFetch(NAME, EXPR) -> texelFetch(NAME, ivec2(int(EXPR)&M, int(EXPR)>>S), 0)
// for each NAME the shader declared as a buffer sampler. EXPR is a pure
// expression in every shader HLSLcc emits, so evaluating it twice is free of
// consequence. Returns 0 and leaves the buffer untouched if the result would
// not fit — a compile failure is recoverable, a heap overrun is not.
static int klfb_texbuf_fetches(char *buf, size_t cap, char names[][64], int nn) {
    char *out = malloc(cap);
    if (!out) return 0;
    size_t o = 0;
    const char *r = buf;
    int did = 0;
    while (*r) {
        const char *hit = strstr(r, "texelFetch(");
        if (!hit) break;
        const char *arg = hit + strlen("texelFetch(");
        while (*arg == ' ') arg++;
        int which = -1;
        for (int i = 0; i < nn; i++) {
            size_t l = strlen(names[i]);
            if (strncmp(arg, names[i], l) == 0) {
                const char *a = arg + l;
                while (*a == ' ') a++;
                if (*a == ',') { which = i; break; }
            }
        }
        if (which < 0) {                       // not one of ours — copy through
            size_t n = (size_t)(arg - r);
            if (o + n >= cap) { free(out); return 0; }
            memcpy(out + o, r, n); o += n;
            r = arg;
            continue;
        }
        const char *a = arg + strlen(names[which]);
        while (*a == ' ') a++;
        a++;                                    // the comma
        const char *estart = a;
        int depth = 0;
        while (*a && !(depth == 0 && (*a == ')' || *a == ','))) {
            if (*a == '(') depth++;
            else if (*a == ')') depth--;
            a++;
        }
        if (*a != ')') { free(out); return 0; } // a 3-arg fetch is not a buffer fetch
        size_t elen = (size_t)(a - estart);
        size_t head = (size_t)(estart - r);     // through "texelFetch(NAME,"
        if (o + head >= cap) { free(out); return 0; }
        memcpy(out + o, r, head); o += head;
        int wrote = snprintf(out + o, cap - o,
                             "ivec2(int(%.*s)&%d,int(%.*s)>>%d),0",
                             (int)elen, estart, KLFB_TEXBUF_MASK,
                             (int)elen, estart, KLFB_TEXBUF_SHIFT);
        if (wrote < 0 || (size_t)wrote >= cap - o) { free(out); return 0; }
        o += (size_t)wrote;
        r = a;                                  // the closing paren, copied next
        did = 1;
    }
    size_t rest = strlen(r);
    if (o + rest + 1 > cap) { free(out); return 0; }
    memcpy(out + o, r, rest + 1);
    if (did) memcpy(buf, out, o + rest + 1);
    free(out);
    return did;
}

static int klfb_rewrite_texbuffer(char *buf, size_t cap) {
    // The declarations, so only the right texelFetch calls are rewritten: a
    // sampler2D fetch in the same shader already takes an ivec2 and must be
    // left exactly as it is.
    char names[16][64];
    int nn = 0;
    char *p = buf;
    while (nn < 16 && (p = strstr(p, "samplerBuffer "))) {
        char *q = p + strlen("samplerBuffer ");
        while (*q == ' ') q++;
        size_t l = 0;
        while (q[l] && (isalnum((unsigned char)q[l]) || q[l] == '_') && l < 63) l++;
        if (l) { memcpy(names[nn], q, l); names[nn][l] = 0; nn++; }
        p = q + (l ? l : 1);
    }
    if (!nn) return 0;
    int did = klfb_texbuf_fetches(buf, cap, names, nn);
    klfb_texbuf_retype(buf);
    fprintf(stderr, "  [glfb] texture buffer: %d sampler%s (%s%s) served as a "
                    "%d-wide 2D texture — ANGLE's Metal backend has no "
                    "GL_EXT_texture_buffer, and a buffer texture is a flat "
                    "texelFetch either way%s\n",
            nn, nn == 1 ? "" : "s", names[0], nn > 1 ? ", ..." : "",
            KLFB_TEXBUF_W, did ? "" : " (NO fetch was rewritten — see above)");
    return 1;
}

// ---- Scene depth, which this backend cannot fetch from the framebuffer -----
//
// r25's finding, and it is the whole of the blue fill. UE4's cooked mobile
// soft-particle / DepthFade materials reach scene depth through exactly one
// door, and it is not a texture:
//
//     #extension GL_ARM_shader_framebuffer_fetch_depth_stencil : enable
//     ...
//     #ifdef GL_ARM_shader_framebuffer_fetch_depth_stencil
//     float DepthbufferFetchES2() { return gl_LastFragDepthARM; }
//     #else
//     float DepthbufferFetchES2() { return 0.0; }
//     #endif
//
// The `#ifdef` is resolved by the DRIVER'S compiler, and ANGLE's Metal backend
// never sets `shaderFramebufferFetchDepthStencilARM` (only the Vulkan backend
// does — vk_caps_utils.cpp is the single assignment in the tree). So the macro
// is undefined, the `#else` is taken, and every DepthFade material in the title
// reads a CONSTANT scene depth of 0.0.
//
// 0.0 is the FAR plane under UE4's reversed-Z, so ConvertFromDeviceZ turns it
// into ~1e8 world units and the fade term
//
//     clamp((SceneDepth - min(ParticleW, FadeMax)) / FadeDistance, 0.0, 1.0)
//
// saturates to 1.0 for every fragment of every soft particle. The fade is stuck
// fully OPEN: a dense volumetric particle field that is supposed to dissolve
// against nearby geometry instead draws every sprite at full alpha and
// accumulates to a flat, scale-invariant constant bounded by its own geometry.
// That is r20's measurement — sRGB(0,24,46), pixel-identical at three zooms,
// bounded by the well's perspective trapezoid — with a mechanism attached.
//
// It is also silent: the shader compiles, links and draws, and nothing anywhere
// says a capability was missing. UE4 itself has no runtime knob here; the
// `#else` is the cook's own fallback and it is simply wrong on a backend that
// cannot fetch depth.
//
// What we do about it. The depth buffer IS a texture on this path (the r24
// census: `depth tex 2472 12288x4928 fmt=0x88f0`), it is attached to the FBO
// the particles draw into, and translucency draws with depth writes off — so it
// can be SAMPLED instead of fetched. The rewrite swaps the whole `#ifdef` block
// for a texelFetch at the fragment's own integer coordinate:
//
//     float DepthbufferFetchES2() { return texelFetch(kl_SceneDepth, ivec2(gl_FragCoord.xy), 0).x; }
//
// texelFetch and not a UV, deliberately. Under an MTLRasterizationRateMap the
// fragment shader runs in PHYSICAL space and so does the depth attachment it
// was rasterized against, so integer gl_FragCoord indexes the depth texture
// exactly — there is no buffer size to get wrong, no ViewRect to offset by, and
// no DoubleWide half to pick. Every screen-UV mismatch this investigation spent
// two rounds looking for is bypassed rather than solved.
//
// Both edits are LENGTH-PRESERVING (the block is 169 characters and the
// replacement is 94), so this rule runs in place alongside the shrinking ones.
//
// KL_GLFB_DEPTH_FETCH picks the arm:
//   sample  (default) — the texelFetch above.
//   far / 0 / off     — leave the shader alone: the pre-r25 behaviour, fade
//                       stuck open, blue fill present. The A/B control.
//   near              — `return 1.0`, the NEAR plane, which drives the fade
//                       term to zero and makes every soft particle vanish. Not
//                       correct, but it is the cheap way to prove the term is
//                       what paints the fill, and a usable picture if the
//                       sampling arm turns out not to work on this backend.
#define KLFB_DEPTH_UNIFORM "kl_SceneDepth"
static unsigned g_df_rewritten;          // shaders this rule fired on
static int klfb_depth_fetch_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *s = kl_env_str("KL_GLFB_DEPTH_FETCH", "sample");
        if (!s || !*s || !strcmp(s, "0") || !strcmp(s, "off") ||
            !strcmp(s, "far"))            mode = 0;
        else if (!strcmp(s, "near"))      mode = 2;
        else                              mode = 1;
    }
    return mode;
}

static int klfb_rewrite_depth_fetch(char *buf, uint32_t shader) {
    int mode = klfb_depth_fetch_mode();
    if (!mode) return 0;
    char *blk = strstr(buf, "#ifdef GL_ARM_shader_framebuffer_fetch_depth_stencil");
    if (!blk || !strstr(buf, "DepthbufferFetchES2")) return 0;
    char *end = strstr(blk, "#endif");
    if (!end) return 0;
    end += strlen("#endif");

    static const char body_sample[] =
        "float DepthbufferFetchES2() { return texelFetch("
        KLFB_DEPTH_UNIFORM ", ivec2(gl_FragCoord.xy), 0).x; }";
    static const char body_near[] = "float DepthbufferFetchES2() { return 1.0; }";
    const char *body = mode == 1 ? body_sample : body_near;
    size_t bl = strlen(body);
    if ((size_t)(end - blk) < bl) return 0;      // never grow: the slack is the
                                                 // texture-buffer rule's
    if (mode == 1) {
        // The declaration takes the `#extension` line's place. That directive is
        // a no-op here — ANGLE warns and moves on — and it is the one line in
        // the file guaranteed to be long enough to hold the uniform.
        static const char decl[] = "uniform highp sampler2D " KLFB_DEPTH_UNIFORM ";";
        char *ext = strstr(buf, "#extension GL_ARM_shader_framebuffer_fetch_depth_stencil");
        char *nl  = ext ? strchr(ext, '\n') : NULL;
        if (!ext || !nl || (size_t)(nl - ext) < sizeof decl - 1) return 0;
        memset(ext, ' ', (size_t)(nl - ext));
        memcpy(ext, decl, sizeof decl - 1);
    }
    memset(blk, ' ', (size_t)(end - blk));
    memcpy(blk, body, bl);
    if (g_df_rewritten++ == 0)
        fprintf(stderr, "  [glfb] scene depth: this backend has no "
                        "GL_ARM_shader_framebuffer_fetch_depth_stencil, so UE4's "
                        "DepthbufferFetchES2 was compiling to a CONSTANT 0.0 — the "
                        "far plane, which pins every DepthFade to 1.0. Shader %u "
                        "and every one after it now %s (KL_GLFB_DEPTH_FETCH=far "
                        "restores the constant, =near forces the opposite one).\n",
                shader, mode == 1 ? "samples the attached depth texture at "
                                    "ivec2(gl_FragCoord.xy)"
                                  : "returns the NEAR plane (soft particles off)");
    return 1;
}

// ---------------------------------------------------------------------------
// r27 — UE4's VOLUMETRIC FOG lookup, and why an empty volume paints a wall.
//
// FILLHIST named the draw (r27: pass 4, draw 25, program 334, six verts, the
// full eye viewport) and its GLSL is on the container. The fragment shader's
// last statement before the output is UE4's fog composite, unchanged since
// UE4.18:
//
//     out = surface.rgb * in_TEXCOORD7.w + in_TEXCOORD7.xyz
//
// `in_TEXCOORD7` is fog, computed PER VERTEX: `.w` is transmittance, `.xyz` is
// inscattering. A transmittance of zero therefore does not dim the surface, it
// DELETES it and leaves the inscattering colour behind — one flat constant over
// the whole primitive, unchanged by zoom, which is exactly what Austin has been
// looking at since r20.
//
// And the vertex shader says where a zero transmittance comes from. Its height
// fog is fine; what rides on top is the volumetric one:
//
//     v94 = textureLod(vs0, v93, 0.0);          // sampler3D — the fog volume
//     v81.w = 1.0 - (f82 * (1.0 - v94.w));      // f82 -> 1 with distance
//     v79.w = v75.w * v81.w;                    // ...multiplies the height fog
//
// `vs0` is UE4's IntegratedLightScattering volume. `f82` is a pure distance
// ramp — zero inside the fog's start distance, one beyond it — so if the volume
// reads back as ZEROS the arithmetic is: near the camera v81.w = 1 and the
// surface renders (Austin's speckled band), farther out it falls (his black
// band), and past the ramp v81.w = 0 and the surface is replaced wholesale by
// the inscattering colour (the navy fill). The band boundaries are the fog
// evaluation's own vertex seams, which is why they are straight lines in screen
// space and why they slide with pitch and shrink as he zooms in: they are a
// CAMERA-DISTANCE threshold, which is also Austin's own "gone past 80%".
//
// A volume that reads zero is what an ANGLE-on-Metal guest gets whenever the
// pass that fills it did not run, and UE4 has a defined answer for that case:
// no volumetric fog is `float4(0, 0, 0, 1)` — no inscattering, full
// transmittance — which collapses the two lines above to `v79 = v75`, i.e. the
// ordinary height fog, untouched. So the rewrite is not a hack around the
// symptom; it is the value UE4 itself uses when the feature is off.
//
//   KL_GLFB_VOLFOG=neutral   every sampler3D textureLod() in a VERTEX shader
//                            becomes vec4(0.,0.,0.,1.). Height fog survives.
//   KL_GLFB_VOLFOG=pass      (default) leave it alone — the A/B control, and
//                            the right setting for any guest whose volume is
//                            actually being filled.
//
// Scope is deliberately narrow: VERTEX shaders only (UE4 evaluates fog per
// vertex on mobile; a fragment-stage sampler3D is somebody's actual volume
// texture and must not be touched), and only `textureLod`, which is the only
// form the fog lookup uses. The replacement is shorter than what it replaces,
// so this rule never grows the buffer.
static unsigned g_vf_rewritten, g_vf_calls;
static int klfb_volfog_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *s = kl_env_str("KL_GLFB_VOLFOG", "pass");
        on = (s && (!strcmp(s, "neutral") || !strcmp(s, "1") ||
                    !strcmp(s, "on") || !strcmp(s, "off_volume")));
    }
    return on;
}

static int klfb_rewrite_volfog(char *buf, uint32_t shader) {
    if (!klfb_volfog_on()) return 0;
    // Vertex stage only. HLSLCC's vertex output always declares gl_Position;
    // a fragment shader never does. Cheaper and more certain than plumbing the
    // shader type down here, and wrong in neither direction.
    if (!strstr(buf, "gl_Position")) return 0;
    if (!strstr(buf, "sampler3D")) return 0;
    static const char neutral[] = "vec4(0.,0.,0.,1.)";
    const size_t nl = sizeof neutral - 1;
    int fired = 0;
    // Collect the sampler3D names this shader declares, so a textureLod on a
    // 2D or array sampler in the same text is left alone.
    char names[8][64]; unsigned nn = 0;
    for (char *d = buf; nn < 8 && (d = strstr(d, "sampler3D ")); ) {
        d += strlen("sampler3D ");
        unsigned k = 0;
        while (KLFB_IDENT(d[k]) && k + 1 < sizeof names[0]) { names[nn][k] = d[k]; k++; }
        names[nn][k] = 0;
        if (k) nn++;
    }
    for (char *p = buf; (p = strstr(p, "textureLod(")); ) {
        char *arg = p + strlen("textureLod(");
        unsigned k = 0; char id[64];
        while (KLFB_IDENT(arg[k]) && k + 1 < sizeof id) { id[k] = arg[k]; k++; }
        id[k] = 0;
        int is3d = 0;
        for (unsigned i = 0; i < nn; i++) if (!strcmp(id, names[i])) is3d = 1;
        if (!is3d) { p = arg; continue; }
        // Matching close paren; the fog lookup's arguments are plain
        // identifiers and literals, but count depth rather than assume it.
        char *q = arg; int depth = 1;
        while (*q && depth) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            if (depth) q++;
        }
        if (depth || (size_t)(q + 1 - p) < nl) { p = arg; continue; }
        size_t span = (size_t)(q + 1 - p);
        memset(p, ' ', span);
        memcpy(p, neutral, nl);
        g_vf_calls++; fired = 1;
        p += span;
    }
    if (!fired) return 0;
    if (g_vf_rewritten++ == 0)
        fprintf(stderr, "  [glfb] volumetric fog: UE4 multiplies its height-fog "
                        "TRANSMITTANCE by the alpha of a sampler3D lookup, and a "
                        "volume that reads ZERO therefore deletes every surface "
                        "past the fog's start distance and leaves the "
                        "inscattering colour — one flat constant, the blue fill. "
                        "Shader %u and every vertex shader after it uses UE4's "
                        "own no-volumetric-fog value vec4(0,0,0,1) instead "
                        "(KL_GLFB_VOLFOG=pass restores the lookup).\n", shader);
    return 1;
}

// ---- the fog composite itself: the MECHANISM test, not another source guess -
//
// r27 read the vertex shader, decided the zero transmittance came from the
// volumetric term, and neutralised it. r28's worn log says the rewrite fired on
// 246 vertex shaders, that the two shaders FILLHIST named this run both carry
// `vec4(0.,0.,0.,1.)` in their dumped post-rewrite text — and that the fill is
// still there. So the volumetric lookup was NOT the zero.
//
// Everything after that is a guess about WHERE the zero comes from — the height
// fog's own `exp2(-density*distance)`, a uniform we answer wrong, the surface
// term being flat all by itself — and r27 already spent a round on a guess. So
// this round tests the MECHANISM instead: is the fog composite what paints the
// constant AT ALL?
//
// Every one of these fragment shaders ends the same way:
//
//     out.xyz = surface.xyz * in_TEXCOORD7.www + in_TEXCOORD7.xyz
//
// UE4's value for "no fog" is `float4(0, 0, 0, 1)` — no inscattering, full
// transmittance — which collapses that line to `out = surface`. Shadowing
// `in_TEXCOORD7` with a local of that value inside main() therefore removes the
// fog composite from the picture entirely, whatever the vertex shader computed
// and whatever uniform fed it.
//
//   fill GONE  -> the fog composite IS the mechanism, and the next round is
//                 about WHICH term of the vertex fog goes to zero (the uniform
//                 dump below prints the numbers to read it from).
//   fill STAYS -> the composite is not the mechanism at all: `surface` is
//                 already the constant, and the next round is about program
//                 359's three sampler2Ds and what they hold.
//
// Either answer closes a family. That is worth more than a fourth nomination.
//
// SHADOWING, not renaming the declaration: a fragment input with no matching
// vertex output is a link error, so the varying stays declared exactly as it
// was and a local of the same name inside main() takes over every use. The
// insert is ~40 bytes into KLFB_GLSL_SLACK's 256 and is refused if it does not
// fit.
//
//   KL_GLFB_FOG=off    shadow in_TEXCOORD7 with vec4(0,0,0,1) in every
//                      fragment shader that reads it.
//   KL_GLFB_FOG=pass   (default) leave it alone — the A/B control.
static unsigned g_fc_rewritten;
static int klfb_fog_off(void) {
    static int on = -1;
    if (on < 0) {
        const char *s = kl_env_str("KL_GLFB_FOG", "pass");
        on = (s && (!strcmp(s, "off") || !strcmp(s, "neutral") ||
                    !strcmp(s, "1") || !strcmp(s, "on")));
    }
    return on;
}

static int klfb_rewrite_fogcomposite(char *buf, size_t cap, uint32_t shader) {
    if (!klfb_fog_off()) return 0;
    // Fragment stage only: the vertex shader COMPUTES in_TEXCOORD7 and writing
    // over it there would be the same experiment done blind.
    if (strstr(buf, "gl_Position")) return 0;
    if (!strstr(buf, "in_TEXCOORD7;")) return 0;   // the declaration
    if (!strstr(buf, "in_TEXCOORD7."))  return 0;  // ...and at least one use
    // The first `main()` whose body opens is the entry point; the helper
    // functions HLSLCC emits above it are named compiler_internal_*.
    char *m = strstr(buf, "void main(");
    if (!m) return 0;
    char *br = strchr(m, '{');
    if (!br) return 0;
    static const char ins[] = "\n\tvec4 in_TEXCOORD7 = vec4(0.,0.,0.,1.);";
    size_t add = sizeof ins - 1;
    size_t len = strlen(buf);
    if (len + add + 1 > cap) return 0;
    char *at = br + 1;
    memmove(at + add, at, len - (size_t)(at - buf) + 1);
    memcpy(at, ins, add);
    if (g_fc_rewritten++ == 0)
        fprintf(stderr, "  [glfb] fog composite: UE4's mobile fragment shaders "
                        "end in `surface * in_TEXCOORD7.www + in_TEXCOORD7.xyz`, "
                        "and a zero transmittance there does not dim the surface "
                        "— it REPLACES it with one flat colour. Shader %u and "
                        "every fragment shader after it shadows in_TEXCOORD7 "
                        "with UE4's own no-fog value vec4(0,0,0,1), so the "
                        "composite cannot paint anything. If the fill survives "
                        "THIS, the fog composite is not the mechanism and the "
                        "surface term is already constant "
                        "(KL_GLFB_FOG=pass restores it).\n", shader);
    return 1;
}

// ---------------------------------------------------------------------------
// Multiview, de-sugared into the MultiPass the guest is actually running.
//
// The Quest build of this title ships ONLY the Single Pass Instanced shader
// variants — Unity strips the others at build time against the project's stereo
// mode — and on GLES "Single Pass Instanced" means `GL_OVR_multiview2`. We tell
// the guest multiview is unsupported (kl_ovrp_multiview(), and it must: ANGLE's
// Metal backend has no such extension), so Unity configures MultiPass and then
// hands ANGLE the only variants it has:
//
//     ERROR: 0:2:  'GL_OVR_multiview2' : extension is not supported
//     ERROR: 0:41: '?' : Error during layout qualifier parsing.
//     ERROR: 0:59: 'gl_ViewID_OVR' : undeclared identifier
//
// 219 shaders in one 2792-frame boot, i.e. most of the material set. A program
// linked from a shader that did not compile is a program the guest keeps using,
// which is why the picture arrived in a corner of the view, sheared with the
// head, and drew text as solid blocks with 12383 "Invalid texture unit!" beside
// it: those are the symptoms of drawing through a broken program, not of a
// geometry bug.
//
// What multiview does for these shaders is exactly one thing — supply a view
// index that selects a slice of unity_StereoMatrixVP[8]:
//
//     u_xlati1 = int(int(gl_ViewID_OVR) << 2);
//     gl_Position = hlslcc_mtx4x4unity_StereoMatrixVP[u_xlati1] * ... ;
//
// and in MultiPass Unity fills that array for both eyes anyway (it is the same
// constant buffer its non-instanced stereo variants index with
// unity_StereoEyeIndex). So the index is all that is missing, and we can supply
// it: `gl_ViewID_OVR` becomes an ordinary uniform this file writes, once per
// (program, eye), from the pass the guest is in.
//
// Which eye that is comes from the guest's own frame shape. Unity's MultiPass
// resolves each eye with one draw into that eye's swapchain texture (measured:
// 931 draws per eye framebuffer over 931 frames per stage), so a draw whose
// target is eye E's framebuffer ENDS eye E's pass — and the pass after it is
// the other eye's. That is self-correcting: a missed toggle costs one pass, not
// the run.
static int g_mv_view_id;                  // the eye the current pass renders
static unsigned long g_mv_shaders;        // shaders de-sugared
static unsigned long g_mv_sets;           // uniform writes
static unsigned long g_mv_eye_pass[2];    // passes observed ending on each eye
static int g_mv_no_room;                  // a directive too short to rewrite in

#define KLFB_MV_UNIFORM "kl_view_id"

// Returns buf rewritten in place (rules shrink, preserve, or grow into the
// caller's KLFB_GLSL_SLACK), or NULL if no rule applied. `cap` is the total
// allocated size of buf including the slack and the NUL.

// ESSL's default INT precision is per-STAGE, and a uniform declared in both
// stages without one is therefore two different types.
//
// The spec: a vertex shader defaults `int` to highp, a fragment shader defaults
// it to mediump. A uniform (or a struct member) that states no precision picks
// up its stage's default, and a strict linker then refuses the program because
// the two declarations disagree. ANGLE is strict. Qualcomm is not, which is why
// a Quest port ships shaders that cannot link here.
//
// Simpsons Hit & Run is the case that found it, and the failure was six
// programs and a NULL dereference two seconds into the first level load:
//
//   Program linking error: Precisions of uniform 'lights' member
//   'lights.enabled' differ between VERTEX and FRAGMENT shaders.
//
// ...against `struct LightParams { int enabled; vec4 position; vec4 colour;
// vec3 attenuation; }`, and then, when the world's first shadow pass reached
// for one of the six:
//
//   fault: signal 11 at 0x14, pc libmain.so+0x6cc6a8 [pglProgram::UseProgram+0x10]
//   lr libmain.so+0x6c3434 [pglContext::EndSunShadowMap+0x1fc]
//
// `UseProgram` is `ldr w8, [x0, #0x14]` with x0 = 0: the engine looked up a
// program that never linked, got null, and used it without a test. The menu
// never touched a lit program, which is why launch #3 got all the way to a
// level load before it died.
//
// The repair states the vertex stage's default OUT LOUD in both stages, which
// is a no-op in the vertex shader (it already defaults to highp int) and a
// PROMOTION in the fragment shader (mediump -> highp). Nothing is narrowed
// anywhere, so no value that fitted before stops fitting; the alternative
// direction — pinning both to mediump — would demote vertex-stage loop counters
// and index arithmetic, which is a real behaviour change.
//
// Placement is after the source's FIRST precision statement, which is the one
// position guaranteed to be legal: precision statements follow #version and
// #extension, so anything already sitting before one is a directive this must
// not jump ahead of.
//
// Two guards keep it off shaders that cannot have the defect: a source that
// states an int precision already has no mismatch to fix, and a source with no
// precision statement at all is not the ESSL dialect this is about.
// KL_GLFB_NO_INT_PRECISION=1 is the A/B.
static int klfb_rewrite_int_precision(char *buf, size_t cap) {
    static int off = -1;
    if (off < 0) off = kl_env_on("KL_GLFB_NO_INT_PRECISION", 0);
    if (off) return 0;

    // Already stated? Any `precision <qualifier> int;` counts.
    const char *q[] = { "precision highp int;", "precision mediump int;",
                        "precision lowp int;" };
    for (unsigned i = 0; i < sizeof q / sizeof q[0]; i++)
        if (strstr(buf, q[i])) return 0;

    char *p = strstr(buf, "precision ");
    if (!p) return 0;
    char *semi = strchr(p, ';');
    if (!semi) return 0;
    char *at = semi + 1;                       // insert directly after it

    static const char ins[] = "\nprecision highp int;";
    size_t n = sizeof ins - 1;
    size_t len = strlen(buf);
    if (len + n + 1 > cap) return 0;           // no slack: leave the text alone
    memmove(at + n, at, strlen(at) + 1);
    memcpy(at, ins, n);
    return 1;
}

// ---------------------------------------------------------------------------
// KL_VRR for a guest whose scene target is SINGLE-sampled (Vivecraft).
//
// The size rule (kl_glfb_set_eye_rate_map) foveates render targets of the eye
// size with at least KL_RATE_MIN_SAMPLES samples, and 2 is right for every
// Unity guest: their scene is a multisampled renderbuffer resolved into the eye
// texture by a blit ANGLE turns into a physical-to-physical copy. Vivecraft has
// no MSAA. It draws the world into its own eye-sized single-sampled "3D Render"
// texture (VRRenderer.java, framebufferVrRender) and reaches the swapchain
// through ONE full-screen draw, ShaderHelper.doVrPostProcess with
// core/postprocessing_vr.fsh. Under minSamples=2 only that copy is foveated:
// the periphery softened, the world pass at full cost.
//
// KL_VRR_MIN_SAMPLES=1 lets the rule take single-sampled targets too, so the
// 3D Render target and the swapchain share one map. That alone is NOT enough,
// and the reason is the whole design:
//
//   Under an MTLRasterizationRateMap a fragment runs at a PHYSICAL position
//   (gl_FragCoord is physical), but VARYINGS are interpolated at the SCREEN
//   position that physical pixel stands for. postprocessing_vr.fsh samples
//   `texture(Sampler0, texCoordinates)` with texCoordinates the vertex-
//   interpolated 0..1 UV of a full-screen quad (passthrough_vr.vsh) — a SCREEN
//   UV. Reading a WARPED source at a screen UV fetches the texel whose
//   physical position equals that screen position: a second warp, the
//   magnified-centre picture. (ANGLE's own eye resolve hit the same thing and
//   needed the `physicalPassthrough` blit; kl_fove needs an expand pass.)
//
// The post pass is a 1:1 copy of an identically-warped, equally-sized source,
// so the exact read is the texel at the fragment's own physical position:
// UV = gl_FragCoord.xy / textureSize(Sampler0, 0). So this rule shadows
// `texCoordinates` in main() with exactly that, and no unwarp/expand pass is
// needed — the compositor unwarps the swapchain as for any foveated eye.
// Unfoveated (no map: the simulator, KL_VRR=0 after the fact) the same
// expression is the plain screen UV, because gl_FragCoord is then the screen
// position and the two textures are the same size.
//
// What that costs: the radial effects in the same shader (FOV-reduction
// vignette, pumpkin overlay, the portal/water wobble offsets) are evaluated in
// physical rather than screen space — a vignette circle that is slightly
// wider in the periphery after the unwarp. Cosmetic, and only while moving
// with FOV reduction on or wearing a pumpkin.
//
// What it REQUIRES: the 3D Render target is the swapchain's size, i.e.
// Vivecraft's renderScaleFactor 1.0 and useFsaa false (both staged). A size
// mismatch is caught at the first post draw and named
// (klfb_vrr_post_check) rather than trusted.
//
// Gated: KL_VRR on, KL_VRR_MIN_SAMPLES=1, and KL_VRR_POST not 0 (the A/B that
// leaves the shader alone — expect a magnified centre with it).
static unsigned klfb_rate_min_samples(void) {
    static int v = -1;
    if (v < 0) {
        v = kl_env_int("KL_VRR_MIN_SAMPLES", 2);
        if (v < 1) v = 1;
        if (v > 64) v = 64;
    }
    return (unsigned)v;
}

#define KLFB_VRPOST_MAX 16
static uint32_t g_vrpost_sh[KLFB_VRPOST_MAX];     // shaders the rule rewrote
static unsigned g_vrpost_nsh;
static uint32_t g_vrpost_prog[KLFB_VRPOST_MAX];   // programs that linked one
static unsigned g_vrpost_nprog;

static int klfb_vrr_post_wanted(void) {
    static int on = -1;
    if (on < 0)
        on = kl_env_on("KL_VRR", 1) && klfb_rate_min_samples() == 1 &&
             kl_env_on("KL_VRR_POST", 1);
    return on;
}

static int klfb_rewrite_vr_post(char *buf, size_t cap, uint32_t shader) {
    if (!klfb_vrr_post_wanted()) return 0;
    if (strstr(buf, "gl_Position")) return 0;          // fragment stage only
    // Vivecraft's post shader and nothing else: circle_radius is its
    // FOV-reduction uniform (no other shader in the pack declares it), and the
    // varying and sampler are the two names the rewrite works on. glsl-
    // optimizer (ltw) keeps uniform and varying names, which is what the
    // guest's glGetUniformLocation relies on as well.
    if (!strstr(buf, "circle_radius") || !strstr(buf, "texCoordinates") ||
        !strstr(buf, "Sampler0"))
        return 0;
    char *m = strstr(buf, "void main");
    if (!m) return 0;
    char *br = strchr(m, '{');
    if (!br) return 0;
    // highp: a 4608-wide position does not survive mediump (exact only up to
    // 2048), and gl_FragCoord is highp in ESSL 3.00 anyway.
    static const char ins[] =
        "\n  highp vec2 texCoordinates = gl_FragCoord.xy / "
        "vec2(textureSize(Sampler0, 0));";
    size_t add = sizeof ins - 1;
    size_t len = strlen(buf);
    if (len + add + 1 > cap) {
        fprintf(stderr, "  [glfb] VR post shader %u: no room for the "
                        "physical-UV line — left alone, the foveated post copy "
                        "will be WRONG (magnified centre)\n", shader);
        return 0;
    }
    char *at = br + 1;
    memmove(at + add, at, len - (size_t)(at - buf) + 1);
    memcpy(at, ins, add);
    int known = 0;
    for (unsigned i = 0; i < g_vrpost_nsh; i++)
        if (g_vrpost_sh[i] == shader) known = 1;
    if (!known && g_vrpost_nsh < KLFB_VRPOST_MAX) g_vrpost_sh[g_vrpost_nsh++] = shader;
    fprintf(stderr, "  [glfb] VR post shader %u rewritten: texCoordinates is "
                    "gl_FragCoord.xy / textureSize(Sampler0) — the foveated "
                    "3D Render target is read at the fragment's own PHYSICAL "
                    "position, so the copy into the identically-warped "
                    "swapchain is texel-for-texel (KL_VRR_POST=0 is the A/B)\n",
            shader);
    return 1;
}

static char *klfb_rewrite_glsl(char *buf, size_t cap, uint32_t shader) {
    int changed = 0;
    klfb_pin_reset(shader);
    // First, because it inserts a line and every rule below either shrinks or
    // preserves length: doing it last would have to re-find offsets the others
    // moved.
    if (klfb_rewrite_int_precision(buf, cap)) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] `precision highp int;` stated in shaders that "
                            "left it to the stage default — ESSL defaults int to "
                            "highp in a vertex shader and mediump in a fragment "
                            "one, so a uniform declared in both is two types and "
                            "the link fails (KL_GLFB_NO_INT_PRECISION=1 is the A/B)\n");
        changed = 1;
    }
    if (strncmp(buf, "#version 3", 10) == 0 &&
        buf[10] >= '0' && buf[10] <= '9' && buf[11] >= '0' && buf[11] <= '9' &&
        (buf[10] > '0' || buf[11] > '0') && strncmp(buf + 12, " es", 3) == 0) {
        memcpy(buf + 10, "00", 2);          // "#version 3xx es" -> 300 es
        changed = 1;
    }
    // "binding = <tok>" inside a layout list, in either comma position:
    // "layout(binding = x, std140)" or "layout(std140, binding = 0)".
    // Token ends at the first ',' or ')'.
    char *p = buf;
    while ((p = strstr(p, "binding = "))) {
        char *tok = p + strlen("binding = ");
        char *end = strpbrk(tok, ",)");
        if (!end) break;
        if (*end == ',') {                       // leading entry: drop "b = t, "
            char *q = end + 1;
            while (*q == ' ') q++;
            memmove(p, q, strlen(q) + 1);
        } else {                                 // trailing: drop ", b = t"
            char *q = p;
            while (q > buf && q[-1] == ' ') q--;
            if (q > buf && q[-1] == ',') q--;
            memmove(q, end, strlen(end) + 1);
        }
        changed = 1;
    }
    // Explicit uniform locations, both spellings Unity emits: the
    // UNITY_LOCATION(x) macro use ("UNITY_LOCATION(0) uniform mediump
    // sampler2D _MainTex;") and the direct layout form. HLSLCC spells every
    // in/out qualifier as a literal "layout(location = ...)", so the macro
    // form is unambiguous; the literal form goes through the uniform check.
    p = buf;
    while ((p = strstr(p, "UNITY_LOCATION("))) {
        char *q = klfb_strip_uniform_layout(p, shader);
        if (q) { p = q; changed = 1; } else p += 5;
    }
    p = buf;
    while ((p = strstr(p, "layout(location = "))) {
        char *q = klfb_strip_uniform_layout(p, shader);
        if (q) { p = q; changed = 1; } else p += 8;
    }

    // External images, which ANGLE's Metal backend does not have.
    //
    // Steam Link's video shader is the ordinary Android one — it declares
    // `#extension GL_OES_EGL_image_external_essl3` and a `samplerExternalOES`,
    // because on Android the decoded frame arrives as an external image and the
    // sampler is what performs the YUV->RGB conversion. ANGLE hard-disables
    // both extensions on Metal (DisplayMtl.mm sets EGLImageExternalOES and
    // EGLImageExternalEssl3OES to false, with an upstream "NOTE(hqle): Support
    // ..." beside them), so the shader does not compile at all and the app
    // carries on with a broken program — measured, and it is the second half of
    // why there is no picture.
    //
    // An external sampler is a 2D sampler plus three promises: the conversion
    // has happened, there are no mipmaps, and the texture cannot be redefined
    // by glTexImage2D. We keep all three. kl_vtdec asks VideoToolbox for BGRA
    // rather than NV12 precisely so the conversion is already done (see
    // kl_vtdec.h), and the other two are restrictions, so a 2D texture is a
    // strict superset. The retarget is therefore exact HERE — it is not a
    // general claim that external images are 2D images.
    //
    // The bind target has to move with it (klfb_BindTexture below): a shader
    // that samples `sampler2D` while the guest binds GL_TEXTURE_EXTERNAL_OES
    // reads nothing, and would be a worse failure than the compile error
    // because it is silent.
    //
    // Both edits are length-preserving, which is what lets them run in place
    // alongside the shrinking rules above.
    p = buf;
    while ((p = strstr(p, "#extension GL_OES_EGL_image_external"))) {
        char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        memset(p, ' ', (size_t)(e - p));   // blank the directive, keep the line
        changed = 1;
    }
    p = buf;
    while ((p = strstr(p, "samplerExternalOES"))) {
        memcpy(p, "sampler2D", 9);
        memset(p + 9, ' ', strlen("samplerExternalOES") - 9);
        p += strlen("samplerExternalOES");
        changed = 1;
    }

    // Multiview, which ANGLE's Metal backend does not have either. See the
    // block above klfb_rewrite_glsl for why this is a rewrite and not a
    // capability. Three length-preserving-or-shrinking edits, in place:
    //
    //   1. "#extension GL_OVR_multiview2 : require" (38 characters) becomes
    //      "uniform highp uint kl_view_id;" (30) and trailing spaces. The
    //      declaration has to land HERE, before every use, and this is the one
    //      line in the file guaranteed to be long enough and to come first.
    //   2. "layout(num_views = 2) in;" is blanked — with one view per pass
    //      there is nothing left for it to declare.
    //   3. gl_ViewID_OVR (13) -> kl_view_id (10), whole words only.
    //
    // If the directive is somehow too short to hold the declaration, NOTHING is
    // rewritten: a half-de-sugared shader fails to compile with a confusing
    // error instead of the honest "extension is not supported", and the count
    // below says so.
    static int mv_rewrite = -1;
    if (mv_rewrite < 0) mv_rewrite = !kl_env_on("KL_GLFB_NO_MULTIVIEW", 0);
    p = mv_rewrite ? strstr(buf, "#extension GL_OVR_multiview") : NULL;
    if (p) {
        static const char decl[] = "uniform highp uint " KLFB_MV_UNIFORM ";";
        char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        size_t line = (size_t)(e - p);
        if (line < sizeof decl - 1) {
            g_mv_no_room++;
        } else {
            memcpy(p, decl, sizeof decl - 1);
            memset(p + sizeof decl - 1, ' ', line - (sizeof decl - 1));
            char *q = buf;
            while ((q = strstr(q, "layout(num_views"))) {
                char *semi = strchr(q, ';');
                if (!semi) break;
                memset(q, ' ', (size_t)(semi + 1 - q));
                q = semi + 1;
            }
            q = buf;
            while ((q = strstr(q, "gl_ViewID_OVR"))) {
                size_t n = strlen("gl_ViewID_OVR");
                if (q > buf && KLFB_IDENT(q[-1])) { q += n; continue; }
                if (KLFB_IDENT(q[n]))             { q += n; continue; }
                memcpy(q, KLFB_MV_UNIFORM, sizeof KLFB_MV_UNIFORM - 1);
                memset(q + sizeof KLFB_MV_UNIFORM - 1, ' ',
                       n - (sizeof KLFB_MV_UNIFORM - 1));
                q += n;
            }
            g_mv_shaders++;
            changed = 1;
        }
    }

    // A variable named for a built-in function.
    //
    // Steam Link's pointer shader declares `uniform float length;` and also
    // calls the built-in `length(v)` three times. ESSL keeps variables and
    // functions in ONE namespace, so the declaration hides the function and
    // every later call is "'length' : function name expected". The vendor GLES
    // driver this shader shipped against evidently keeps two namespaces, C-style,
    // and accepts it. Three compile errors, then the program fails to LINK — and
    // that program belongs to XRConstruct, which is what draws the decoded video,
    // so the whole stream scene comes up with "[SceneStream] Failed to initialize
    // construct" and there is no surface for a frame to land on.
    //
    // The split is exact rather than a heuristic, because it is the compiler's
    // own: an occurrence followed by `(` IS the function call it is complaining
    // about, and every other occurrence is the variable. Renaming the second
    // group and leaving the first restores exactly the reading the shader was
    // written for. klfb_GetUniformLocation maps the guest's name back, so a
    // uniform this renames is still findable by the name it was declared with.
    for (size_t i = 0; i < sizeof g_glsl_builtin_vars / sizeof g_glsl_builtin_vars[0]; i++) {
        const char *from = g_glsl_builtin_vars[i].from, *to = g_glsl_builtin_vars[i].to;
        size_t n = strlen(from);
        if (strlen(to) != n || strstr(buf, to)) continue;   // never shadow a name already here
        int renamed = 0;
        for (p = buf; (p = strstr(p, from)); p += n) {
            char before = p == buf ? 0 : p[-1];
            if (KLFB_IDENT(before) || KLFB_IDENT(p[n])) continue;   // part of a longer name
            const char *q = p + n;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '(') continue;                                // the function call
            memcpy(p, to, n);
            renamed++;
        }
        if (renamed) {
            fprintf(stderr, "  [glfb] shader declares `%s`, which is a built-in "
                            "function name — renamed %d use%s to `%s`\n",
                    from, renamed, renamed == 1 ? "" : "s", to);
            changed = 1;
        }
    }
    // ---- Unity's instancing UBO against ANGLE's spec-minimum block limit ----
    //
    // SUPERHOT fails to LINK 84 programs in one run, every one of them:
    //
    //   Size of uniform block UnityInstancing_PerDraw0 in VERTEX shader
    //   exceeds GL_MAX_UNIFORM_BLOCK_SIZE (16384)
    //
    // Unity patches its own `UNITY_RUNTIME_INSTANCING_ARRAY_SIZE` token in the
    // GLSL text at load time, space-padding the replacement so the source keeps
    // its length. On this driver it writes 160, and the element is two mat4s
    // spelled as `vec4 m[4]` pairs — 128 bytes — so the block asks for 20480
    // against a limit of 16384. The limit is not a hardware one: ANGLE's Metal
    // backend sets `kMaxUBOSize = 16384` and says so in its own comment,
    // "Default according to GLES 3.0 spec", while its internal ceiling
    // (IMPLEMENTATION_MAX_UNIFORM_BLOCK_SIZE) is 65536.
    //
    // A program that does not link draws nothing at all, so every instanced
    // material in the scene is invisible — which is what a black SUPERHOT
    // environment with a few non-instanced shards left in it looks like.
    //
    // **The right fix has landed**: angle-patches/klepton.patch raises
    // kMaxUBOSize to 65536, ANGLE's own ceiling, and takes this title from 120
    // link failures to zero with no truncation anywhere. This clamp stays only
    // for a tree built against a STOCK ANGLE, and stays off by default, because
    // it is lossy: Unity still batches up to the count it chose, so instances
    // past the clamp index the array out of bounds. If you are reaching for it
    // on a Klepton-built ANGLE, something else is wrong.
    // KL_GLFB_CLAMP_INSTANCING=1.
    if (kl_env_on("KL_GLFB_CLAMP_INSTANCING", 0)) {
        char *ub = strstr(buf, "uniform UnityInstancing_");
        char *brace = ub ? strchr(ub, '{') : NULL;
        char *semi  = brace ? strchr(brace, ';') : NULL;
        char *open  = semi ? memchr(brace, '[', (size_t)(semi - brace)) : NULL;
        if (open) {
            // The element type is the first token after '{' — "Type name[N];".
            char *t = brace + 1;
            while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
            char type[96];
            size_t tn = 0;
            while (KLFB_IDENT(t[tn]) && tn + 1 < sizeof type) { type[tn] = t[tn]; tn++; }
            type[tn] = 0;

            // std140 size of that struct, from its declaration above. Unity's
            // HLSLcc output spells matrices as vec4 arrays, so vec4 and mat4
            // are the whole vocabulary here; anything else and the clamp
            // declines rather than guesses a stride.
            long stride = 0;
            char decl[128];
            snprintf(decl, sizeof decl, "struct %s {", type);
            char *sd = tn ? strstr(buf, decl) : NULL;
            if (sd) {
                char *end = strstr(sd, "};");
                for (char *m = sd; end && m < end; ) {
                    char *nl = strchr(m, '\n');
                    if (!nl || nl > end) break;
                    long unit = strstr(m, "vec4") && strstr(m, "vec4") < nl ? 16
                              : strstr(m, "mat4") && strstr(m, "mat4") < nl ? 64 : 0;
                    if (unit) {
                        long k = 1;
                        char *br = memchr(m, '[', (size_t)(nl - m));
                        if (br) k = strtol(br + 1, NULL, 10);
                        if (k < 1) k = 1;
                        stride += unit * k;
                    }
                    m = nl + 1;
                }
            }
            long want = strtol(open + 1, NULL, 10);
            int32_t limit = 0;
            if (a_glGetIntegerv)
                a_glGetIntegerv(0x8A30 /* GL_MAX_UNIFORM_BLOCK_SIZE */, &limit);
            if (limit <= 0) limit = 16384;
            long maxn = stride > 0 ? limit / stride : 0;
            if (stride > 0 && want > maxn && maxn > 0) {
                // Written in place over the digits Unity left, keeping the
                // space padding it wrote — the declaration must not change
                // length, because everything after it is byte offsets Unity
                // still believes in.
                char *d = open + 1;
                size_t room = 0;
                while (d[room] && d[room] != ']') room++;
                char rep[32];
                int rn = snprintf(rep, sizeof rep, "%ld", maxn);
                if (rn > 0 && (size_t)rn <= room) {
                    memcpy(d, rep, (size_t)rn);
                    memset(d + rn, ' ', room - (size_t)rn);
                    static int said;
                    if (said++ < 4)
                        fprintf(stderr, "  [glfb] instancing block %.*s asks for %ld "
                                "x %ld bytes = %ld, over this driver's %d — clamped "
                                "to %ld (instances past it index out of bounds; the "
                                "real fix is a larger GL_MAX_UNIFORM_BLOCK_SIZE)\n",
                                (int)tn, type, want, stride, want * stride, limit,
                                maxn);
                    changed = 1;
                }
            }
        }
    }

    // ---- UE4's cooked shaders are ESSL 3.10, and this driver is ES 3.0 ----
    //
    // Tetris Effect (UE 4.26, cooked for GLSL_ES3_1_ANDROID) declares its
    // VARYINGS with explicit locations:
    //
    //     layout(location=2) out vec4 var_TEXCOORD7;      (vertex)
    //     layout(location=2) in  vec4 in_TEXCOORD7;       (fragment)
    //
    // `layout(location=)` on a varying is an ESSL **3.10** feature. In ESSL
    // 3.00 it is legal only on vertex inputs and fragment outputs, so ANGLE
    // answers each one with "'location' : invalid layout qualifier: only valid
    // on program inputs and outputs", the shader does not compile, the program
    // does not link, and UE4 turns the Fatal into a ForceQuit.
    //
    // This driver cannot simply grow: ANGLE's Metal backend caps at ES 3.0
    // (mtl::kMaxSupportedGLVersion = 3.0, and getMaxConformantESVersion clamps
    // to 3.0 as well), and UE4 only emits `#version 310 es` when the RHI has
    // decided the device is ES 3.1 — which it is not.
    //
    // What the locations are FOR here is matching a vertex output to a fragment
    // input, and ESSL 3.00 already does that BY NAME. HLSLcc's names are
    // systematic — vertex outputs are `var_<semantic>`, the fragment inputs
    // that receive them are `in_<semantic>`, vertex attributes are
    // `in_ATTRIBUTE<n>` and fragment outputs are `out_<n>` — and the two sides
    // of a varying always agree on the semantic. So dropping the qualifier and
    // renaming `var_` to `in_` reconstructs exactly the pairing the locations
    // expressed, with no information lost:
    //
    //   * a location on `in_ATTRIBUTE<n>` (vertex input) is KEPT — legal, and
    //     load-bearing: the guest binds attributes by index.
    //   * a location on `out_<n>` (fragment output) is KEPT — legal, and it
    //     names the draw buffer.
    //   * every other location is blanked, and `var_` becomes `in_`.
    //
    // The rename shrinks by one byte per occurrence and runs in place; the
    // `gl_ViewID_OVR` fix below is the one byte of growth the slack exists for.
    // Nothing here fires on a shader that does not spell a location UE4's way,
    // so no Unity guest sees any of it.
    //
    // WHICH STAGE THIS IS matters and cannot be guessed from the qualifier
    // alone: `layout(location=N) in ...` is the legal, load-bearing form in a
    // VERTEX shader (the guest binds attributes by index) and the illegal one
    // in a fragment shader, and the mirror image holds for `out`. A rule that
    // stripped by qualifier alone would delete Unity's attribute locations —
    // Unity names its vertex inputs `in_POSITION0`, `in_TEXCOORD0` and so on,
    // which no name test distinguishes from a varying. `gl_Position` is the
    // discriminator: a vertex shader must write it and a fragment shader
    // cannot, so it is not a heuristic.
    if (kl_env_on("KL_GLFB_ESSL310_VARYINGS", 1) && strstr(buf, "layout(location=")) {
        const int is_vertex = strstr(buf, "gl_Position") != NULL;
        int stripped = 0, renamed = 0;
        for (p = buf; (p = strstr(p, "layout(location=")); ) {
            char *close = strchr(p, ')');
            char *semi  = close ? strchr(close, ';') : NULL;
            if (!close || !semi) break;
            // The storage class is the `in`/`out` word after the qualifier.
            int is_in = 0, is_out = 0;
            for (char *w = close + 1; w < semi; ) {
                while (w < semi && !KLFB_IDENT(*w)) w++;
                char *w0 = w;
                while (w < semi && KLFB_IDENT(*w)) w++;
                size_t n = (size_t)(w - w0);
                if (!n) break;
                if (n == 2 && strncmp(w0, "in", 2) == 0)  is_in = 1;
                if (n == 3 && strncmp(w0, "out", 3) == 0) is_out = 1;
            }
            // Legal in ESSL 3.00: a location on a VERTEX input or on a
            // FRAGMENT output. Everything else with `in`/`out` storage is a
            // varying and the qualifier is the 3.10 feature this driver lacks.
            int keep = (is_in  && is_vertex)
                    || (is_out && !is_vertex)
                    || (!is_in && !is_out);        // uniforms are someone else's rule
            if (!keep) {
                memset(p, ' ', (size_t)(close + 1 - p));
                stripped++;
                changed = 1;
            }
            p = close + 1;
        }
        // `var_` -> `in_`, whole identifiers only. One byte shorter each time,
        // so the tail slides left and the buffer only shrinks. Vertex stage
        // only, and only when this shader actually lost a varying location —
        // renaming on a shader whose locations were legal would break a pairing
        // that already worked.
        for (p = (is_vertex && stripped) ? buf : NULL; p && (p = strstr(p, "var_")); ) {
            if (p != buf && KLFB_IDENT(p[-1])) { p += 4; continue; }
            memmove(p, p + 1, strlen(p + 1) + 1);   // drop the 'v'
            memcpy(p, "in_", 3);                    // "ar_..." -> "in_..."
            renamed++;
            changed = 1;
            p += 3;
        }
        if (stripped || renamed) {
            static int said;
            if (!said) {
                said = 1;
                fprintf(stderr, "  [glfb] ESSL 3.10 varyings in a 300 es shader — "
                                "explicit varying locations dropped and `var_` "
                                "renamed to `in_` so the stages still pair by name "
                                "(KL_GLFB_ESSL310_VARYINGS=0 leaves them alone)\n");
            }
        }
    }
    // UE4's own non-multiview fallback defines `gl_ViewID_OVR` as `0`, a signed
    // int, and then the cooked shader uses it in unsigned arithmetic
    // (`vc1_h[int((8u + gl_ViewID_OVR))]`). ESSL has no implicit int->uint
    // conversion, so every such expression is "no operation '+' exists that
    // takes ... 'const uint' and ... 'const int'". Whatever Adreno does with
    // this, ANGLE is right and UE4's fallback is a byte short: `0u` is what it
    // means. This is the one growing edit, and the only one — hence the slack.
    {
        static const char def[] = "#define gl_ViewID_OVR 0";
        char *d = strstr(buf, def);
        if (d && d[sizeof def - 1] == '\n') {
            size_t len = strlen(buf);
            if (len + 2 < cap) {
                char *at = d + sizeof def - 1;             // the '\n'
                memmove(at + 1, at, len - (size_t)(at - buf) + 1);
                *at = 'u';
                changed = 1;
                static int said;
                if (!said) {
                    said = 1;
                    fprintf(stderr, "  [glfb] `#define gl_ViewID_OVR 0` -> `0u` — "
                                    "UE4's non-multiview fallback defines it signed "
                                    "and the shader does unsigned arithmetic with "
                                    "it, which ESSL will not convert\n");
                }
            }
        }
    }
    // Scene depth, before the texture-buffer rule: this one INTRODUCES a
    // texelFetch, and klfb_rewrite_texbuffer rewrites texelFetch argument
    // lists. Ours names a sampler2D, which that rule ignores — but the ordering
    // is stated rather than relied on by accident.
    if (klfb_rewrite_depth_fetch(buf, shader))
        changed = 1;
    // Volumetric fog, in the vertex stage: independent of the depth rule above
    // (that one is a fragment idiom) and of the texture-buffer rule below (this
    // one rewrites textureLod on a sampler3D, that one texelFetch on a
    // samplerBuffer), so the ordering here is documentation, not a dependency.
    if (klfb_rewrite_volfog(buf, shader))
        changed = 1;
    // The fog composite, in the fragment stage: the other half of the same
    // question, and deliberately independent of the rule above — this one does
    // not care what the vertex shader computed, which is the whole point of it.
    if (klfb_rewrite_fogcomposite(buf, cap, shader))
        changed = 1;
    // Vivecraft's VR post copy, under KL_VRR_MIN_SAMPLES=1 — an insert, and
    // independent of every rule above (it touches main()'s first line only).
    if (klfb_rewrite_vr_post(buf, cap, shader))
        changed = 1;
    // Texture buffers last: it rewrites texelFetch argument lists, and every
    // rule above works on declarations and qualifiers it must not have moved.
    if (strstr(buf, "samplerBuffer") && klfb_rewrite_texbuffer(buf, cap))
        changed = 1;

    return changed ? buf : NULL;
}

// ---------------------------------------------------------------------------
// Shader-source instruments (r24). Two of them, and they answer two different
// questions about the SAME text — the GLSL the guest hands us after our own
// rewrite, i.e. exactly what ANGLE compiles.
//
//   KL_GLFB_DUMP_GLSL=<dir>  writes every source to <dir>/sh_<name>.glsl. Host
//                            only in practice: it is the offline copy to grep.
//   KL_GLFB_DEPTH_SCAN=1     (default ON) a census of the idioms UE4 uses to
//                            reach SCENE DEPTH, printed to the log so a DEVICE
//                            run names the depth path without a filesystem.
//
// Why scene depth: the well backdrop is a particle field, and UE4 particle
// materials reach for DepthFade (soft particles) — SceneDepth sampled at the
// fragment's own screen UV, alpha faded by (SceneDepth - PixelDepth). If the
// screen UV math assumes a buffer size or view rect we do not serve (we present
// DoubleWide with per-eye viewports), every particle fades against depth from
// ANOTHER screen location, which is a screen-FIXED artefact over a world-locked
// well. The census is what says whether that idiom is even on this path.
static const char *const g_depth_idioms[] = {
    "SceneDepthTexture",  "MobileSceneDepth",   "SceneDepthAux",
    "SceneTextureLookup", "CalcSceneDepth",     "ConvertFromDeviceZ",
    "LookupDeviceZ",      "DepthFade",          "gl_LastFragDepthARM",
    "FramebufferFetch",   "gl_LastFragData",    "inout highp float gl_LastFrag",
    "BufferSizeAndInvSize", "ViewRectMin",      "ScreenPositionScaleBias",
    "SvPositionToBufferUV", "ResolvedView",
    // r25: the name that actually carries this title's scene-depth read, and
    // the one that SURVIVES our own rewrite — klfb_rewrite_depth_fetch deletes
    // the `gl_LastFragDepthARM` above it, so without this entry the census
    // would go quiet the moment the fix landed and look like a falsification.
    "DepthbufferFetchES2",
};
#define KLFB_NDEPTH_IDIOMS (sizeof g_depth_idioms / sizeof g_depth_idioms[0])
static unsigned g_depth_hits[KLFB_NDEPTH_IDIOMS];
static unsigned g_depth_shaders, g_depth_excerpted;

// Print the lines around a hit so the log carries the ACTUAL expression, not
// just "this word appeared" — the size/offset bug, if it exists, is in the
// arithmetic on that line.
static void klfb_depth_excerpt(uint32_t shader, const char *src, const char *idiom) {
    const char *hit = strstr(src, idiom);
    if (!hit) return;
    const char *ls = hit;
    while (ls > src && ls[-1] != '\n') ls--;
    const char *le = strchr(hit, '\n');
    int len = le ? (int)(le - ls) : (int)strlen(ls);
    if (len > 300) len = 300;
    fprintf(stderr, "  [glfb] depth idiom '%s' in shader %u: %.*s\n",
            idiom, shader, len, ls);
}

static int klfb_depth_scan(uint32_t shader, const char *src) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_DEPTH_SCAN", 1);
    if (!on || !src) return 0;
    int any = 0;
    for (unsigned i = 0; i < KLFB_NDEPTH_IDIOMS; i++) {
        if (!strstr(src, g_depth_idioms[i])) continue;
        g_depth_hits[i]++;
        any = 1;
        // The first few hits per idiom get their line printed; after that the
        // counter alone carries it, or 3000 shaders bury the run.
        if (g_depth_hits[i] <= 2 && g_depth_excerpted < 48) {
            klfb_depth_excerpt(shader, src, g_depth_idioms[i]);
            g_depth_excerpted++;
        }
    }
    if (!any) return 0;
    g_depth_shaders++;
    // The first few in FULL, to stderr. A UE4 material fragment shader is a few
    // hundred lines and the device log is the only channel a worn run has — two
    // of them is a bounded cost and it is the difference between "SceneDepth
    // appeared" and being able to READ the screen-UV arithmetic that goes with
    // it, which is where a size mismatch would live.
    static int budget = -1;
    if (budget < 0) budget = kl_env_int("KL_GLFB_DEPTH_FULL", 2);
    if (budget > 0) {
        budget--;
        fprintf(stderr, "  [glfb] ---- FULL source of depth-idiom shader %u ----\n"
                        "%s\n  [glfb] ---- end shader %u ----\n", shader, src, shader);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// r27 — the 3D-texture census, which is the other half of the volumetric-fog
// reading. The rewrite above assumes UE4's fog volume reads back as zeros; this
// says whether it was ever WRITTEN. A volume with storage, sampler binds in the
// thousands and zero writes of any kind is a texture nobody filled, and the
// shader that multiplies by its alpha is then multiplying by zero all run.
#define KLFB_MAX_TEX3D 32
static struct {
    uint32_t name, fmt;
    int32_t  w, h, d;
    unsigned writes, attaches, binds;
} g_tex3d[KLFB_MAX_TEX3D];
static unsigned g_ntex3d, g_tex3d_dropped;

static int klfb_tex3d_slot(uint32_t name) {
    if (!name) return -1;
    for (unsigned i = 0; i < g_ntex3d; i++) if (g_tex3d[i].name == name) return (int)i;
    return -1;
}
static void klfb_note_tex3d(uint32_t name, uint32_t fmt, int32_t w, int32_t h,
                            int32_t d) {
    if (!name) return;
    int s = klfb_tex3d_slot(name);
    if (s < 0) {
        if (g_ntex3d >= KLFB_MAX_TEX3D) { g_tex3d_dropped++; return; }
        s = (int)g_ntex3d++;
        g_tex3d[s].name = name;
    }
    g_tex3d[s].fmt = fmt; g_tex3d[s].w = w; g_tex3d[s].h = h; g_tex3d[s].d = d;
}
static void klfb_note_tex3d_event(uint32_t name, int write, int attach, int bind) {
    int s = klfb_tex3d_slot(name);
    if (s < 0) return;
    g_tex3d[s].writes += (unsigned)write;
    g_tex3d[s].attaches += (unsigned)attach;
    g_tex3d[s].binds += (unsigned)bind;
}
static void klfb_tex3d_report(void) {
    if (!g_ntex3d) return;
    fprintf(stderr, "  [glfb] 3D textures: %u\n", g_ntex3d);
    for (unsigned i = 0; i < g_ntex3d; i++)
        fprintf(stderr, "  [glfb]   tex %u %dx%dx%d fmt=0x%04x — %u write(s), "
                        "%u fbo-layer attach(es), %u sampler bind(s)%s\n",
                g_tex3d[i].name, g_tex3d[i].w, g_tex3d[i].h, g_tex3d[i].d,
                g_tex3d[i].fmt, g_tex3d[i].writes, g_tex3d[i].attaches,
                g_tex3d[i].binds,
                (!g_tex3d[i].writes && !g_tex3d[i].attaches && g_tex3d[i].binds)
                    ? "   <<< SAMPLED BUT NEVER WRITTEN" : "");
    if (g_tex3d_dropped)
        fprintf(stderr, "  [glfb]   (%u more 3D texture(s) past the table's %d)\n",
                g_tex3d_dropped, KLFB_MAX_TEX3D);
}

void klfb_depth_scan_report(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_DEPTH_SCAN", 1);
    if (!on) return;
    fprintf(stderr, "  [glfb] depth-idiom census: %u shaders carried at least one\n",
            g_depth_shaders);
    for (unsigned i = 0; i < KLFB_NDEPTH_IDIOMS; i++)
        if (g_depth_hits[i])
            fprintf(stderr, "  [glfb]   %-24s %u\n", g_depth_idioms[i], g_depth_hits[i]);
    // The volumetric-fog rule's own two numbers, printed whichever arm is
    // running: "0 shaders" on the neutral arm means the idiom is not in this
    // title at all and the fill is somebody else's, which is a finding.
    fprintf(stderr, "  [glfb] volumetric fog (%s): %u vertex shader(s) rewritten, "
                    "%u sampler3D lookup(s) neutralised\n",
            klfb_volfog_on() ? "neutral" : "pass", g_vf_rewritten, g_vf_calls);
    // r28's mechanism test. A number in the hundreds here and a fill that is
    // STILL on Austin's screen means the fog composite cannot be what paints
    // it, because the composite has been removed from every shader that has
    // one — and that closes the whole fog family in one run.
    fprintf(stderr, "  [glfb] fog composite (%s): %u fragment shader(s) shadow "
                    "in_TEXCOORD7 with vec4(0,0,0,1)\n",
            klfb_fog_off() ? "off" : "pass", g_fc_rewritten);
    klfb_tex3d_report();
}

// Two directories, deliberately separate. KL_GLFB_DUMP_GLSL takes EVERYTHING —
// a host knob, thousands of files. KL_GLFB_DUMP_GLSL_DEPTH takes only the
// shaders that carry a scene-texture idiom, capped, which is small enough to
// bake on DEVICE and pull off the container afterwards.
#define KLFB_DEPTH_DUMP_CAP 64
static void klfb_glsl_dump(uint32_t shader, const char *src, int depth_hit) {
    static const char *all = (const char *)-1, *deep = (const char *)-1;
    if (all == (const char *)-1)  all  = kl_env_str("KL_GLFB_DUMP_GLSL", NULL);
    if (deep == (const char *)-1) deep = kl_env_str("KL_GLFB_DUMP_GLSL_DEPTH", NULL);
    if (!src) return;
    char path[600];
    if (all) {
        snprintf(path, sizeof path, "%s/sh_%06u.glsl", all, shader);
        FILE *f = fopen(path, "w");
        if (f) { fputs(src, f); fclose(f); }
    }
    if (deep && depth_hit) {
        static unsigned n;
        if (n < KLFB_DEPTH_DUMP_CAP) {
            snprintf(path, sizeof path, "%s/depth_%02u_sh%06u.glsl", deep, n, shader);
            FILE *f = fopen(path, "w");
            if (f) { fputs(src, f); fclose(f); n++; }
        }
    }
}

static void klfb_ShaderSource(uint32_t shader, int32_t count,
                              const char *const *strings, const int32_t *lengths) {
    if (strings && count > 0) {
        // The buffer is built for the rewrite, NOT tied to the capture: the
        // capture table holds 1024 entries and a long run sources thousands,
        // so a shared buffer stops rewriting the moment it fills and the 320
        // es failures return with nothing reporting it.
        size_t total = 0;
        for (int32_t i = 0; i < count; i++)
            total += lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                                : (strings[i] ? strlen(strings[i]) : 0);
        // The texture-buffer rule is the one that grows the text by more than a
        // byte — every rewritten texelFetch adds about thirty — so a source
        // that declares a buffer sampler gets a bigger slack. Sized off the
        // source's own length so a long shader cannot outgrow it.
        size_t cap = total + 1 + KLFB_GLSL_SLACK;
        char *buf = malloc(cap);
        if (buf) {
            size_t probe = 0;
            for (int32_t i = 0; i < count; i++) {
                size_t len = lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                                        : (strings[i] ? strlen(strings[i]) : 0);
                if (strings[i] && len && probe + len < cap) {
                    memcpy(buf + probe, strings[i], len); probe += len;
                }
            }
            buf[probe < cap ? probe : cap - 1] = 0;
            if (strstr(buf, "samplerBuffer")) {
                size_t grown = total + 1 + KLFB_GLSL_SLACK + total / 2 + 4096;
                char *big = realloc(buf, grown);
                if (big) { buf = big; cap = grown; }
            }
        }
        if (buf) {
            size_t off = 0;
            for (int32_t i = 0; i < count; i++) {
                size_t len = lengths && lengths[i] >= 0 ? (size_t)lengths[i]
                                                        : (strings[i] ? strlen(strings[i]) : 0);
                if (strings[i] && len) { memcpy(buf + off, strings[i], len); off += len; }
            }
            buf[off] = 0;
            char *rewritten = klfb_rewrite_glsl(buf, cap, shader);
            klfb_glsl_dump(shader, buf, klfb_depth_scan(shader, buf));
            int stored = 0;
            if (g_fb_nshaders < KLFB_MAX_SHADERS) {
                pthread_mutex_lock(&g_compile_lock);
                g_fb_shaders[g_fb_nshaders].name = shader;
                g_fb_shaders[g_fb_nshaders].src  = buf;
                g_fb_nshaders++;
                pthread_mutex_unlock(&g_compile_lock);
                stored = 1;
            }
            if (rewritten) {
                if (g_real_ShaderSource) {
                    const char *s = buf;
                    g_real_ShaderSource(shader, 1, &s, NULL);
                }
                if (!stored) free(buf);
                return;
            }
            if (!stored) free(buf);
        }
    }
    if (g_real_ShaderSource) g_real_ShaderSource(shader, count, strings, lengths);
}

static const char *klfb_shader_src(uint32_t shader) {
    // Latest wins: a name can be re-sourced, and shared contexts share the
    // name space, so the most recent source for a name is the text the next
    // compile actually saw.
    for (unsigned i = g_fb_nshaders; i-- > 0; )
        if (g_fb_shaders[i].name == shader) return g_fb_shaders[i].src;
    return NULL;
}

#define KLFB_GL_COMPILE_STATUS 0x8B81

static void klfb_CompileShader(uint32_t shader) {
    if (!g_real_CompileShader) return;
    // The status/info-log readers are not thunks (the guest must get ANGLE's
    // real answers), so they resolve lazily here rather than through g_thunks,
    // whose entries hand their thunk pointer to the guest.
    if (!g_real_GetShaderiv) g_real_GetShaderiv = (void *)asym("glGetShaderiv");
    if (!g_real_GetShaderInfoLog)
        g_real_GetShaderInfoLog = (void *)asym("glGetShaderInfoLog");
    pthread_mutex_lock(&g_compile_lock);
    g_real_CompileShader(shader);
    int32_t ok = 1;
    if (g_real_GetShaderiv)
        g_real_GetShaderiv(shader, KLFB_GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        log[0] = 0;
        if (g_real_GetShaderInfoLog)
            g_real_GetShaderInfoLog(shader, sizeof log, NULL, log);
        fprintf(stderr, "  [glfb] glCompileShader(%u) FAILED — info log:\n%s\n",
                shader, log);
        const char *src = klfb_shader_src(shader);
        if (src)
            fprintf(stderr, "  [glfb] ---- source of failed shader %u ----\n%s\n"
                            "  [glfb] ---- end source ----\n", shader, src);
        const char *dir = kl_env_str("KL_DUMP_SHADERS", NULL);
        if (dir && src) {
            char path[600];
            snprintf(path, sizeof path, "%s/failed_shader_%u.glsl", dir, shader);
            FILE *f = fopen(path, "w");
            if (f) { fputs(src, f); fclose(f); }
        }
    }
    pthread_mutex_unlock(&g_compile_lock);
}
// A compile failure prints itself above; a LINK failure printed nothing, and a
// program that compiles both stages and then fails to link is the more
// interesting case — it is where the two halves disagree, or where a limit the
// compiler does not enforce finally bites. SUPERHOT hits 84 of them in one run
// ("Size of uniform block UnityInstancing_PerDraw0 in VERTEX shader exceeds
// GL_MAX_UNIFORM_BLOCK_SIZE (16384)"), and the only reason anyone knew is that
// Unity itself logs the info log — so the sources that produced it were never
// recoverable, which is what a rewrite in this file needs to fix one.
//
// Reported once per distinct info-log text: the same variant is retried, and 84
// copies of one message with 84 shader dumps buries the run it is in.
#define KLFB_LINKFAILS 16
static char g_linkfail[KLFB_LINKFAILS][160];
static unsigned g_nlinkfail;

static void klfb_report_link_failure(uint32_t program) {
    static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
    static void (*r_GetProgramInfoLog)(uint32_t, int32_t, int32_t *, char *);
    static void (*r_GetAttachedShaders)(uint32_t, int32_t, int32_t *, uint32_t *);
    if (!r_GetProgramiv) {
        r_GetProgramiv       = asym("glGetProgramiv");
        r_GetProgramInfoLog  = asym("glGetProgramInfoLog");
        r_GetAttachedShaders = asym("glGetAttachedShaders");
    }
    if (!r_GetProgramiv) return;
    int32_t ok = 1;
    r_GetProgramiv(program, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (ok) return;

    char log[2048];
    log[0] = 0;
    if (r_GetProgramInfoLog) r_GetProgramInfoLog(program, sizeof log, NULL, log);

    // First line only for the dedupe key: drivers append per-program detail.
    char key[160];
    snprintf(key, sizeof key, "%s", log);
    for (char *p = key; *p; p++) if (*p == '\n') { *p = 0; break; }
    for (unsigned i = 0; i < g_nlinkfail; i++)
        if (!strcmp(g_linkfail[i], key)) return;
    if (g_nlinkfail < KLFB_LINKFAILS)
        snprintf(g_linkfail[g_nlinkfail++], sizeof g_linkfail[0], "%s", key);

    fprintf(stderr, "  [glfb] glLinkProgram(%u) FAILED — info log:\n%s\n",
            program, log);
    const char *dir = kl_env_str("KL_DUMP_SHADERS", NULL);
    if (!r_GetAttachedShaders) return;
    uint32_t sh[8];
    int32_t n = 0;
    r_GetAttachedShaders(program, 8, &n, sh);
    for (int32_t i = 0; i < n; i++) {
        const char *src = klfb_shader_src(sh[i]);
        if (!src) continue;
        if (dir) {
            char path[600];
            snprintf(path, sizeof path, "%s/linkfail_prog%u_shader%u.glsl",
                     dir, program, sh[i]);
            FILE *f = fopen(path, "w");
            if (f) { fputs(src, f); fclose(f); }
        }
        fprintf(stderr, "  [glfb] ---- program %u shader %u (linked, then failed) "
                        "----\n%s\n  [glfb] ---- end ----\n", program, sh[i], src);
    }
}

// ------------------------------------------------------------- link in flight
//
// r10 spent 33 s inside one glLinkProgram on device and the log could not name
// which one: links are silent unless they fail, so the tail of a wedged run
// showed the ANGLE backtrace and nothing about the program it was linking. This
// records the link that is currently running -- and how many finished before it
// -- so kl_glfb_link_census can print "program N, the 410th, still linking after
// 33.0 s" straight into the exit report. Costs two stores per link.
static _Atomic uint32_t  g_link_prog;       // 0 when no link is running
static _Atomic unsigned  g_links_done;
static _Atomic double    g_link_started;    // CLOCK_MONOTONIC seconds
static _Atomic double    g_link_worst;      // slowest completed link, seconds
static _Atomic uint32_t  g_link_worst_prog;
// Cumulative link time, and the interval the census reports it over.
//
// r15 needed this line and did not have it. The worn log said "610 program(s)
// linked; slowest 0.333 s" at every census, and the count stopped growing at
// exactly the moment the guest went from 13 fps to 120 — which IS the whole
// diagnosis of the menu judder, but it had to be recovered by differencing the
// count across censuses against a frame-rate line printed somewhere else.
//
// The cost of linking is a duration, so measure the duration. "spent 10.4 s of
// the last 15.0 inside glLinkProgram" is one line that says the render thread
// had a tenth of a frame budget left, and it is the number that a QoS change,
// a program cache, or a return to parallel linking has to move.
static _Atomic double    g_link_total;      // seconds inside glLinkProgram
static _Atomic double    g_link_seen;       // g_link_total at the last census
static _Atomic double    g_census_at;       // when the last census ran

static double klfb_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void kl_glfb_link_census(FILE *f) {
    unsigned done = g_links_done;
    uint32_t live = g_link_prog;
    double worst = g_link_worst;
    if (!done && !live) return;
    fprintf(f, "  [links] %u program(s) linked; slowest %.3f s (program %u)\n",
            done, worst, (unsigned)g_link_worst_prog);
    {
        double now = klfb_now(), total = g_link_total;
        double since = g_census_at ? now - g_census_at : 0.0;
        double spent = total - g_link_seen;
        g_census_at = now;
        g_link_seen = total;
        // Only when there was linking in the window — a settled run should not
        // carry a line that says "0.0 s of the last 15.0" forever.
        // `since` is 0 on the first census — there is no window to be a share
        // of yet, and "0.0 s of the last 0.0" reads as a broken instrument.
        if (spent > 0.0005 && since > 0.0)
            fprintf(f, "  [links] %.1f s of the last %.1f spent INSIDE "
                       "glLinkProgram (%.0f%% of the render thread) — links "
                       "resolve inline, so this is time the guest could not "
                       "draw in\n",
                    spent, since, since > 0 ? 100.0 * spent / since : 0.0);
    }
    if (live) {
        // A live entry in the exit report means the link never returned: this
        // is the line that names the wedge.
        fprintf(f, "  [links] STILL LINKING: program %u (the %uth) after "
                   "%.1f s — the run stopped here\n",
                (unsigned)live, done + 1, klfb_now() - g_link_started);
    }
}

// ----------------------------------------------------- the OTHER way to link
//
// r22. `glProgramBinary` was passed straight through, and that was the one hole
// in the link instrument: klfb_report_link_failure only ever runs from
// glLinkProgram, so a program loaded from a cached binary that FAILS to link
// leaves no line anywhere. The guest then uses it, every glUseProgram answers
// GL_INVALID_OPERATION ("program has not been successfully linked" — the same
// error as "not a program object", which is why it reads like a name bug), and
// UE4 turns its RHI's Fatal into 611 ForceQuit calls and a segfault.
//
// That is exactly what a stale ProgramBinaryCache does the first launch after
// an OS update; kl_app.c's shader_cache_guard is the cure, and this is the
// alarm that would have named it in one line instead of a round.
//
// KL_GLFB_PROGRAM_BINARY=0 refuses the binary path outright: the program is
// left unlinked, which is precisely what the spec says a rejected binary looks
// like, so a guest that checks GL_LINK_STATUS falls back to its shader source.
static void (*g_real_ProgramBinary)(uint32_t, uint32_t, const void *, int32_t);
static _Atomic unsigned g_pb_loads, g_pb_failed, g_pb_refused;
static void klfb_ProgramBinary(uint32_t program, uint32_t format,
                               const void *binary, int32_t length) {
    static int allow = -1;
    if (allow < 0) allow = kl_env_on("KL_GLFB_PROGRAM_BINARY", 1);
    g_pb_loads++;
    if (!allow) {
        if (g_pb_refused++ == 0)
            fprintf(stderr, "  [glfb] glProgramBinary REFUSED "
                            "(KL_GLFB_PROGRAM_BINARY=0) — every cached program "
                            "is left unlinked so the guest recompiles from "
                            "source\n");
        return;
    }
    if (!g_real_ProgramBinary) return;
    g_real_ProgramBinary(program, format, binary, length);

    static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
    if (!r_GetProgramiv) r_GetProgramiv = asym("glGetProgramiv");
    if (!r_GetProgramiv) return;
    int32_t ok = 1;
    r_GetProgramiv(program, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (ok) return;

    unsigned nth = ++g_pb_failed;
    if (nth <= 4)
        fprintf(stderr, "  [glfb] glProgramBinary(%u) REJECTED — format 0x%x, "
                        "%d bytes, program left UNLINKED (failure #%u). A "
                        "compiled-program cache does not survive a driver "
                        "change; see kl_app.c shader_cache_guard.\n",
                program, format, length, nth);
    else if (nth == 5)
        fprintf(stderr, "  [glfb] glProgramBinary: further rejections counted, "
                        "not printed\n");
    // The info log is the driver's own reason and is usually empty for a
    // format mismatch, but it costs nothing to ask on the first one.
    if (nth == 1) klfb_report_link_failure(program);
}

void kl_glfb_program_binary_census(FILE *f) {
    unsigned n = g_pb_loads, bad = g_pb_failed, ref = g_pb_refused;
    if (!n) return;
    fprintf(f, "  [glfb] glProgramBinary: %u load(s), %u REJECTED, %u refused "
               "by KL_GLFB_PROGRAM_BINARY=0\n", n, bad, ref);
    if (bad)
        fprintf(f, "  [glfb]   a rejected binary is an UNLINKED program — every "
                   "glUseProgram on it is GL_INVALID_OPERATION, and UE4 turns "
                   "that into Fatal -> ForceQuit\n");
}

static void klfb_LinkProgram(uint32_t program) {
    if (!g_real_LinkProgram) return;
    pthread_mutex_lock(&g_compile_lock);
    g_link_started = klfb_now();
    g_link_prog = program;
    g_real_LinkProgram(program);
    klfb_report_link_failure(program);
    {
        double took = klfb_now() - g_link_started;
        g_link_prog = 0;
        g_links_done = g_links_done + 1;
        g_link_total = g_link_total + took;
        if (took > g_link_worst) { g_link_worst = took; g_link_worst_prog = program; }
        // Anything this slow is worth a line on its own: a link that takes
        // seconds is the shape the r10 wedge grew out of, and seeing one that
        // still completes is the early warning the wedge never gave.
        if (took >= (double)kl_env_int("KL_GL_LINK_WARN_MS", 500) / 1000.0)
            fprintf(stderr, "  [glfb] glLinkProgram(%u) took %.3f s\n",
                    program, took);
    }
    // The pins the rewrite took out of the shader text become a translation
    // table here — this is the first moment a name has a location to translate
    // to. See the block above klfb_strip_uniform_layout.
    klfb_pins_link(program);
    // KL_VRR_MIN_SAMPLES=1: remember which programs carry the rewritten VR
    // post shader, so the first draws can check the sizes it assumes.
    if (g_vrpost_nsh) {
        static void (*r_GetAttached)(uint32_t, int32_t, int32_t *, uint32_t *);
        if (!r_GetAttached) r_GetAttached = asym("glGetAttachedShaders");
        if (r_GetAttached) {
            uint32_t sh[8]; int32_t n = 0;
            r_GetAttached(program, 8, &n, sh);
            int hit = 0;
            for (int32_t i = 0; i < n && !hit; i++)
                for (unsigned j = 0; j < g_vrpost_nsh; j++)
                    if (sh[i] == g_vrpost_sh[j]) hit = 1;
            int known = 0;
            for (unsigned j = 0; j < g_vrpost_nprog; j++)
                if (g_vrpost_prog[j] == program) known = 1;
            if (hit && !known && g_vrpost_nprog < KLFB_VRPOST_MAX) {
                g_vrpost_prog[g_vrpost_nprog++] = program;
                fprintf(stderr, "  [glfb] VR post program %u linked with the "
                                "physical-UV shader\n", program);
            }
        }
    }
    // KL_GLFB_DUMP_PROGRAM=N: print the sources that were linked into program
    // N. The timeline names programs by number ("the frame's last draw is
    // program 7"); this turns the number into the shader text.
    static int dump_prog = -2;
    if (dump_prog == -2) {
        const char *d = kl_env_str("KL_GLFB_DUMP_PROGRAM", NULL);
        dump_prog = d ? atoi(d) : -1;
    }
    if (dump_prog >= 0 && (int)program == dump_prog) {
        static void (*r_GetAttachedShaders)(uint32_t, int32_t, int32_t *,
                                            uint32_t *);
        if (!r_GetAttachedShaders)
            r_GetAttachedShaders = asym("glGetAttachedShaders");
        if (r_GetAttachedShaders) {
            uint32_t sh[8]; int32_t n = 0;
            r_GetAttachedShaders(program, 8, &n, sh);
            fprintf(stderr, "  [glfb] program %u: %d attached shaders\n",
                    program, (int)n);
            for (int32_t i = 0; i < n; i++) {
                const char *src = klfb_shader_src(sh[i]);
                fprintf(stderr, "  [glfb] ---- program %u shader %u ----\n%s\n"
                                "  [glfb] ---- end ----\n", program, sh[i],
                        src ? src : "(source not captured)\n");
            }
        }
    }
    pthread_mutex_unlock(&g_compile_lock);
}

// ------------------------------------------------------ texture storage census
//
// KL_GLFB_SKIP bisected the AGX abort down to this pair: dropping BOTH
// glTexStorage2D and glTexStorage3D avoids it, and so does dropping both
// glTexSubImage2D and glTexSubImage3D, while dropping any one of the four alone
// does not. That is the "allocate immutable storage, then upload into it" pattern,
// and it says the trigger is a *format*, which is what this prints.
//
// Both calls fit in registers (5 and 6 arguments), so the natural signature is
// safe here — unlike glTexSubImage3D above, whose 11 arguments spill to the stack
// where Darwin's packing and AAPCS64's 8-byte slots disagree.
static void (*g_real_TexStorage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
static void (*g_real_TexStorage3D)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t);

#define KLFB_MAX_FMTS 32
static struct { uint32_t fmt; unsigned n; } g_fmts[KLFB_MAX_FMTS];
static unsigned g_nfmts;

static void klfb_note_format(uint32_t fmt, int32_t w, int32_t h, int32_t d,
                             const char *who) {
    for (unsigned i = 0; i < g_nfmts; i++)
        if (g_fmts[i].fmt == fmt) { g_fmts[i].n++; return; }
    if (g_nfmts < KLFB_MAX_FMTS) {
        g_fmts[g_nfmts].fmt = fmt;
        g_fmts[g_nfmts].n = 1;
        g_nfmts++;
        // Only the first sighting of each format is printed, so the tail of a
        // crashing run names the format that was new when it died.
        fprintf(stderr, "  [glfb] %s: new internalformat 0x%04x (%dx%dx%d)\n",
                who, fmt, w, h, d);
    }
}

// ------------------------------------------------------ the GL object census
//
// KL_GL_CENSUS=<swaps> prints, every N eglSwapBuffers, how many GL objects the
// guest has created and how many it has deleted, per class, plus the bytes of
// texture and renderbuffer storage still live.
//
// Why this exists: every gen/delete goes straight to ANGLE (kl_glfb_sym hands
// the guest ANGLE's own pointers), so nothing at this seam knows whether the
// guest's objects are being reclaimed. That is fine until something grows
// across scene loads, at which point the first question — "is the leak the
// guest's, ANGLE's, or ours?" — has no instrument to answer it. A live count
// that climbs by the same amount at each loading transition names the class;
// a flat one exonerates the whole GL path in one run.
//
// The counts are exact. The bytes are an *estimate over immutable allocations
// only*: glTexStorage2D/3D and glRenderbufferStorage(Multisample), which is
// where the large targets in this title come from. Storage taken through
// glTexImage2D is counted as a call, not as bytes, and printed separately so
// the number is never quietly incomplete.
#define KLFB_CENSUS_KINDS 7
enum { KLC_TEX, KLC_FBO, KLC_RBO, KLC_BUF, KLC_VAO, KLC_SHADER, KLC_PROG };
static const char *const g_census_kind[KLFB_CENSUS_KINDS] = {
    "textures", "framebuffers", "renderbuffers", "buffers",
    "vertex arrays", "shaders", "programs",
};
static struct { unsigned long made, killed; } g_census[KLFB_CENSUS_KINDS];
static unsigned long g_census_teximage;      // allocations whose bytes we do not model

// name -> bytes, for the two immutable-storage families. Kept in one table
// because a texture name and a renderbuffer name live in different spaces and
// the kind disambiguates. Grows; entries are removed on delete, so this table
// is itself a leak detector — if it never shrinks, neither does the VRAM.
typedef struct { uint8_t kind; uint32_t name; uint64_t bytes; } klfb_vram;
static klfb_vram *g_vram;
static unsigned   g_vram_n, g_vram_cap;
static uint64_t   g_vram_bytes[KLFB_CENSUS_KINDS];
static pthread_mutex_t g_census_lock = PTHREAD_MUTEX_INITIALIZER;

// Bits per pixel for the formats this guest allocates. Anything unlisted is
// counted at zero and tallied, rather than guessed at: a wrong multiplier
// would make the total look authoritative while being fiction.
static unsigned long g_census_unknown_fmt;
static unsigned klfb_fmt_bits(uint32_t fmt) {
    // The ASTC LDR block formats are two contiguous runs (linear, then sRGB) in
    // the same block-size order, and every block is 128 bits — so the bits per
    // pixel is 128/(w*h) and the table is the block dimensions, not a case each.
    static const uint8_t astc[14][2] = {
        {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
        {10,5},{10,6},{10,8},{10,10},{12,10},{12,12},
    };
    if (fmt >= 0x93B0 && fmt <= 0x93BD) {
        const uint8_t *b = astc[fmt - 0x93B0];
        return 128 / (unsigned)(b[0] * b[1]);
    }
    if (fmt >= 0x93D0 && fmt <= 0x93DD) {
        const uint8_t *b = astc[fmt - 0x93D0];
        return 128 / (unsigned)(b[0] * b[1]);
    }
    switch (fmt) {
    case 0x8229 /* R8 */:      case 0x8F94 /* R8_SNORM */:
    case 0x8D48 /* STENCIL_INDEX8 */:                           return 8;
    case 0x822B /* RG8 */:     case 0x822D /* R16F */:
    case 0x8D62 /* RGB565 */:  case 0x8056 /* RGBA4 */:
    case 0x8057 /* RGB5_A1 */: case 0x81A5 /* DEPTH_COMPONENT16 */: return 16;
    case 0x8051 /* RGB8 */:    case 0x8C41 /* SRGB8 */:         return 24;
    case 0x8058 /* RGBA8 */:   case 0x8C43 /* SRGB8_ALPHA8 */:
    case 0x822E /* R32F */:    case 0x822F /* RG16F */:
    case 0x8C3A /* R11F_G11F_B10F */: case 0x8C3D /* RGB9_E5 */:
    case 0x81A6 /* DEPTH_COMPONENT24 */: case 0x81A7 /* DEPTH_COMPONENT32 */:
    case 0x88F0 /* DEPTH24_STENCIL8 */:  case 0x8CAC /* DEPTH_COMPONENT32F */:
                                                                return 32;
    case 0x881B /* RGB16F */:                                   return 48;
    case 0x881A /* RGBA16F */: case 0x8230 /* RG32F */:
    case 0x8CAD /* DEPTH32F_STENCIL8 */:                        return 64;
    case 0x8815 /* RGB32F */:                                   return 96;
    case 0x8814 /* RGBA32F */:                                  return 128;
    case 0x9274 /* COMPRESSED_RGB8_ETC2 */:
    case 0x9275 /* COMPRESSED_SRGB8_ETC2 */:
    case 0x9270 /* COMPRESSED_R11_EAC */:                       return 4;
    case 0x9278 /* COMPRESSED_RGBA8_ETC2_EAC */:
    case 0x9279 /* COMPRESSED_SRGB8_ALPHA8_ETC2_EAC */:
    case 0x9272 /* COMPRESSED_RG11_EAC */:                      return 8;
    }
    g_census_unknown_fmt++;
    return 0;
}

// Bytes an immutable allocation reserves. A mip chain is the base level times
// 4/3 in the limit; levels>1 uses that bound rather than summing, which is
// within a few per cent and cannot be wrong in the direction that hides a leak.
static uint64_t klfb_storage_bytes(uint32_t fmt, int32_t w, int32_t h,
                                   int32_t d, int32_t levels) {
    if (w <= 0 || h <= 0 || d <= 0) return 0;
    uint64_t px = (uint64_t)w * (uint64_t)h * (uint64_t)d;
    uint64_t bytes = px * klfb_fmt_bits(fmt) / 8;
    if (levels > 1) bytes = bytes * 4 / 3;
    return bytes;
}

static int klfb_census_every(void) {
    static int n = -1;
    if (n < 0) {
        n = kl_env_int("KL_GL_CENSUS", 0);
        if (n < 0) n = 0;
    }
    return n;
}

// Caller must not hold g_census_lock.
static void klfb_vram_note(int kind, uint32_t name, uint64_t bytes) {
    if (!klfb_census_every() || !name) return;
    pthread_mutex_lock(&g_census_lock);
    for (unsigned i = 0; i < g_vram_n; i++)
        if (g_vram[i].kind == kind && g_vram[i].name == name) {
            // Re-specified storage replaces, it does not add.
            g_vram_bytes[kind] -= g_vram[i].bytes;
            g_vram[i].bytes = bytes;
            g_vram_bytes[kind] += bytes;
            pthread_mutex_unlock(&g_census_lock);
            return;
        }
    if (g_vram_n == g_vram_cap) {
        unsigned cap = g_vram_cap ? g_vram_cap * 2 : 256;
        klfb_vram *p = realloc(g_vram, cap * sizeof *p);
        if (!p) { pthread_mutex_unlock(&g_census_lock); return; }
        g_vram = p; g_vram_cap = cap;
    }
    g_vram[g_vram_n++] = (klfb_vram){(uint8_t)kind, name, bytes};
    g_vram_bytes[kind] += bytes;
    pthread_mutex_unlock(&g_census_lock);
}

static void klfb_vram_forget(int kind, uint32_t name) {
    uint64_t freed = 0;
    pthread_mutex_lock(&g_census_lock);
    for (unsigned i = 0; i < g_vram_n; i++)
        if (g_vram[i].kind == kind && g_vram[i].name == name) {
            freed = g_vram[i].bytes;
            g_vram_bytes[kind] -= freed;
            g_vram[i] = g_vram[--g_vram_n];
            break;
        }
    pthread_mutex_unlock(&g_census_lock);
    // The large allocations are the ones a leak is made of, and "was it ever
    // released" is not answerable from a running total. Naming each big release
    // makes the absence of one a visible fact rather than an inference.
    if (freed >= (16u << 20))
        fprintf(stderr, "  [census] released %s %u: %llu MiB\n",
                g_census_kind[kind], name, (unsigned long long)(freed >> 20));
}

static void klfb_census_made(int kind, int32_t n) {
    if (n > 0) __atomic_fetch_add(&g_census[kind].made, (unsigned long)n,
                                  __ATOMIC_RELAXED);
}
static void klfb_census_killed(int kind, int32_t n) {
    if (n > 0) __atomic_fetch_add(&g_census[kind].killed, (unsigned long)n,
                                  __ATOMIC_RELAXED);
}

void kl_glfb_gl_census(FILE *f) {
    // The made/killed counters are two relaxed atomics on the gen/delete path
    // and are therefore maintained ALWAYS, whether or not KL_GL_CENSUS asked
    // for them — so there is no reason to withhold them from a crash report or
    // from the memory tripwire, which are exactly the moments that want to know
    // whether GL objects are being reclaimed. Only the per-object storage
    // ledger below is gated, because only that one costs anything to keep.
    fprintf(f, "  [census] GL objects live (made - deleted):");
    for (int k = 0; k < KLFB_CENSUS_KINDS; k++)
        fprintf(f, " %s %lu/%lu", g_census_kind[k],
                g_census[k].made - g_census[k].killed, g_census[k].made);
    if (!klfb_census_every()) {
        fprintf(f, "\n  [census] immutable storage not tracked (set KL_GL_CENSUS=1 "
                   "for the MiB ledger)\n");
        return;
    }
    // Read without the lock, deliberately. This also runs from kl_egl_report,
    // which every fatal path goes through, and a diagnostic that can block on
    // the way to reporting a crash costs the whole report — the lock is held
    // across a realloc in the allocation path, so that is not hypothetical.
    // The cost of not taking it is three scalars that may be one allocation
    // stale, which changes no conclusion this number is used for.
    uint64_t tex = g_vram_bytes[KLC_TEX], rbo = g_vram_bytes[KLC_RBO];
    unsigned tracked = g_vram_n;
    fprintf(f, "\n  [census] immutable storage live: textures %llu MiB, "
               "renderbuffers %llu MiB (%u allocations tracked)",
            (unsigned long long)(tex >> 20), (unsigned long long)(rbo >> 20),
            tracked);
    if (g_census_teximage)
        fprintf(f, ", %lu glTexImage* allocations unmeasured", g_census_teximage);
    if (g_census_unknown_fmt)
        fprintf(f, ", %lu allocations of unknown format", g_census_unknown_fmt);
    fprintf(f, "\n");
}

// The gen/delete pairs, as a macro: the only thing that varies is which counter
// moves, and a typo in one hand-written copy is a census that lies about exactly
// one class.
// r32: a deleted texture leaves the supersample ledger with its storage. GL
// recycles names, and an entry kept past the delete scales the viewport of
// whatever ordinary texture inherits the name next.
static void klfb_rt_forget_scaled(uint32_t name);
// ...and the SHAR fourth wall drops its match with the buffer it mirrors.
static void klfb_fw_forget_buffer(uint32_t name);
// KL_GLFB_LAZY_UNBIND's other half: the attachments each framebuffer
// currently carries, as the guest made them through glFramebufferTexture2D.
// Minecraft 26.2 (emulated DSA) re-attaches the SAME texture to the same
// framebuffer every time it uses it; for ANGLE any attach dirties the
// framebuffer and ends the render pass, so the clears could never share one.
// A re-attach of exactly what is attached is a GL no-op and is not forwarded.
// Purged on a texture or framebuffer delete (names are recycled) and on any
// other kind of attach to that point.
#define KLFB_ATT_N 128
static struct { uint32_t fbo, att, textarget, tex; int32_t level; } g_att[KLFB_ATT_N];
static int klfb_att_is_ds(uint32_t a) { return a == 0x8D00 || a == 0x8D20 || a == 0x821A; }
static void klfb_att_forget(uint32_t fbo, uint32_t att) {
    for (int i = 0; i < KLFB_ATT_N; i++)
        if (g_att[i].fbo == fbo &&
            (g_att[i].att == att || (klfb_att_is_ds(att) && klfb_att_is_ds(g_att[i].att))))
            g_att[i].fbo = 0;
}
static int klfb_att_same(uint32_t fbo, uint32_t att, uint32_t textarget, uint32_t tex, int32_t level) {
    if (!fbo) return 0;
    for (int i = 0; i < KLFB_ATT_N; i++)
        if (g_att[i].fbo == fbo && g_att[i].att == att)
            return g_att[i].tex == tex && g_att[i].textarget == textarget && g_att[i].level == level;
    return 0;
}
static void klfb_att_note(uint32_t fbo, uint32_t att, uint32_t textarget, uint32_t tex, int32_t level) {
    if (!fbo) return;
    klfb_att_forget(fbo, att);
    if (!tex) return;
    for (int i = 0; i < KLFB_ATT_N; i++)
        if (!g_att[i].fbo) {
            g_att[i].fbo = fbo; g_att[i].att = att; g_att[i].textarget = textarget;
            g_att[i].tex = tex; g_att[i].level = level;
            return;
        }
}
static void klfb_att_forget_tex(uint32_t tex) {
    if (!tex) return;
    for (int i = 0; i < KLFB_ATT_N; i++) if (g_att[i].fbo && g_att[i].tex == tex) g_att[i].fbo = 0;
}
static void klfb_att_forget_fbo(uint32_t fbo) {
    for (int i = 0; i < KLFB_ATT_N; i++) if (g_att[i].fbo == fbo) g_att[i].fbo = 0;
}

#define KLFB_GENDEL(Name, KIND)                                               \
    static void (*g_real_Gen##Name)(int32_t, uint32_t *);                     \
    static void (*g_real_Delete##Name)(int32_t, const uint32_t *);            \
    static void klfb_Gen##Name(int32_t n, uint32_t *ids) {                    \
        if (g_real_Gen##Name) g_real_Gen##Name(n, ids);                       \
        klfb_census_made(KIND, n);                                            \
    }                                                                         \
    static void klfb_Delete##Name(int32_t n, const uint32_t *ids) {           \
        klfb_census_killed(KIND, n);                                          \
        if (klfb_census_every() && ids &&                                     \
            ((KIND) == KLC_TEX || (KIND) == KLC_RBO))                         \
            for (int32_t i = 0; i < n; i++) klfb_vram_forget(KIND, ids[i]);   \
        if ((KIND) == KLC_TEX && ids)                                         \
            for (int32_t i = 0; i < n; i++) klfb_rt_forget_scaled(ids[i]);    \
        if ((KIND) == KLC_TEX && ids)                                         \
            for (int32_t i = 0; i < n; i++) klfb_att_forget_tex(ids[i]);      \
        if ((KIND) == KLC_BUF && ids)                                         \
            for (int32_t i = 0; i < n; i++) klfb_fw_forget_buffer(ids[i]);    \
        if (g_real_Delete##Name) g_real_Delete##Name(n, ids);                 \
    }

KLFB_GENDEL(Textures,     KLC_TEX)
KLFB_GENDEL(Renderbuffers, KLC_RBO)
KLFB_GENDEL(Buffers,      KLC_BUF)
KLFB_GENDEL(VertexArrays, KLC_VAO)
// glGenFramebuffers already has a thunk (the FBO trace); only the delete half
// is new, and the gen half calls klfb_census_made itself.
static void klfb_forget_fbo(uint32_t fbo);           // with the stage map below
static void (*g_real_DeleteFramebuffers)(int32_t, const uint32_t *);
// KL_GLFB_LAZY_UNBIND (see klfb_BindFramebuffer): a draw-framebuffer unbind
// (bind 0) the guest made is held back on this thread until something needs
// the binding — and dropped outright when the guest's next bind puts back the
// framebuffer that was bound. t_lazy_target is the deferred bind's target.
static __thread uint32_t t_lazy_target, t_lazy_real;
static void klfb_lazy_flush_from(const char *who);
#define klfb_lazy_flush() klfb_lazy_flush_from(__func__)
#define KLFB_LAZY_FLUSH() do { if (t_lazy_target) klfb_lazy_flush_from(__func__); } while (0)

static void klfb_DeleteFramebuffers(int32_t n, const uint32_t *ids) {
    KLFB_LAZY_FLUSH();
    if (ids) for (int32_t i = 0; i < n; i++) klfb_att_forget_fbo(ids[i]);
    klfb_census_killed(KLC_FBO, n);
    // Drop the stage attribution with the object. GL recycles names, so an
    // entry kept past the delete answers for whatever inherits the name next.
    for (int32_t i = 0; ids && i < n; i++) klfb_forget_fbo(ids[i]);
    if (g_real_DeleteFramebuffers) g_real_DeleteFramebuffers(n, ids);
}

// ------------------------------------------------- render-target supersample
//
// r32. Tetris Effect's HUD (SPEED LV / LINES / ZONE / TIME / AREA SCORE, and
// the menus) is drawn by UE4 into small offscreen render targets and then
// textured onto world-space quads. The r26 framebuffer census measured the two
// of them: `fb 32 510x688 fmt 0x8c43` (44 draws a frame) and `fb 28 510x306`
// (31 draws a frame). Those are Quest-sized. Composited into a 6144-wide eye
// across a 105 degree field each of their texels covers several device pixels,
// and the text reads soft no matter what the sampler does — which is exactly
// the defect that survived NOMIP, the foveation A/Bs and the whole LOD family
// (font atlases are single-level and immune to all of it).
//
// The fix is to allocate those targets N times larger in each dimension and let
// the guest draw into the larger surface. Magnification at composite time falls
// by N; the sampling side needs nothing at all, because every UV that reads a
// render target is normalised.
//
// THE KEEPING-THE-PICTURE-CORRECT HALF IS THE VIEWPORT. glViewport and
// glScissor had NO thunk in this file before this round — the guest's calls
// went straight to the driver. Scaling a target without scaling them gives you
// a large texture with the guest drawing into its bottom-left corner: a
// smaller, worse HUD, not a sharper one. So this block owns four things:
//
//   1. WHICH allocations to grow — an explicit `WxH` allowlist, never a blanket
//      rule. A blanket scale would multiply the eye stages and every
//      intermediate the engine owns, on a device already carrying a 3780 MiB
//      footprint. Nothing >= 1024 in either dimension is scaled even if listed.
//   2. A ledger of the texture names that came back larger, so that
//   3. glViewport / glScissor / glBlitFramebuffer / glReadPixels /
//      glCopyTexSubImage2D / glInvalidateSubFramebuffer can ask "is the
//      framebuffer I am about to affect one of mine?" and scale their
//      rectangles by the same factor when the answer is yes, and
//   4. glGetIntegerv(GL_VIEWPORT / GL_SCISSOR_BOX) can divide the driver's
//      answer back down, so a guest that saves and restores its viewport never
//      sees — or re-scales — a number it did not write.
//
// INERT WHEN UNSET, BY CONSTRUCTION. `KL_GLFB_RT_SCALE` is empty by default;
// with an empty list `klfb_rt_match()` returns 1 for every allocation, the
// scaled ledger stays empty, `g_rt_draw_scale`/`g_rt_read_scale` stay 1, and
// every one of the new thunks is a forwarding call with one integer compare in
// front of it. That is the guarantee the runtime-wide thunks rest on: every
// other target on `driver` gets the pass-through it has always had. The knob is
// baked for `tetris` only, in kl_app.c, beside NOMIP/VOLFOG/FOG.
//
//   KL_GLFB_RT_SCALE="510x688,510x306"   the sizes to grow (empty = OFF)
//   KL_GLFB_RT_SCALE_FACTOR=3            the factor (KL_GLFB_RT_SUPERSAMPLE
//                                        is accepted as an alias)
#define KLFB_RT_SIZES 32
// r34: THE GUARD WAS ON THE WRONG NUMBER, AND IT WAS REFUSING THE MENU.
//
// It used to be "never scale a target 1024 or wider" — a bound on the size the
// guest ASKED for. The r34 worn log shows why that is the wrong number: the
// main menu is drawn into 1305x183 and 1305x734 targets, LIVE ones redrawn
// every frame (fb 27/28/29/32 climb across all three censuses), and 1305 is
// over the old bound — so the one thing the whole mechanism exists for was the
// one thing it could not touch. Meanwhile the single panel Austin reported as
// SHARP is 326x102, which was on the list and was grown to 978x306.
//
// What the guard is FOR is "do not allocate something absurd", and that is a
// property of the size that comes OUT, not the size that goes in. So it is now
// a bound on the scaled result. It still refuses everything it was meant to
// refuse — the 12288x4928 eye targets and the 3840x2160 / 3840x540 / 3820x2160
// scene targets the same log names all blow past 8192 when tripled — and it
// stops refusing a 1305-wide widget that becomes 3915 wide.
//
// Nothing is scaled by size CLASS in the first place: `klfb_rt_match()` is an
// exact compare against a list a target bakes by hand, so an unlisted target
// can never be grown by accident. This guard is the second fence, and it is now
// placed where an accident would actually land.
#define KLFB_RT_MAX_OUT 8192        // never ALLOCATE anything this wide or taller
static struct { int32_t w, h; } g_rt_size[KLFB_RT_SIZES];
static int g_rt_nsize = -1;         // -1 = env not parsed yet
static int g_rt_factor = 1;

static void klfb_rt_parse(void) {
    if (g_rt_nsize >= 0) return;
    g_rt_nsize = 0;
    const char *list = kl_env_str("KL_GLFB_RT_SCALE", "");
    int f = kl_env_int("KL_GLFB_RT_SCALE_FACTOR", 0);
    if (f <= 0) f = kl_env_int("KL_GLFB_RT_SUPERSAMPLE", 0);
    if (f <= 0) f = 2;
    if (f > 8) f = 8;                    // a typo should not eat the device
    g_rt_factor = f;
    if (!list || !*list) return;
    const char *p = list;
    while (*p && g_rt_nsize < KLFB_RT_SIZES) {
        char *end = NULL;
        long w = strtol(p, &end, 10);
        if (end == p) break;
        if (*end != 'x' && *end != 'X') break;
        p = end + 1;
        long h = strtol(p, &end, 10);
        if (end == p) break;
        p = end;
        if (w > 0 && h > 0) {
            // r33: a listed size the guard refuses used to be accepted here and
            // then silently refused by klfb_rt_match(), so the list read "we
            // scale that" while the device did not. It is still refused — the
            // guard is what keeps an accident bounded — but it SAYS so. r34:
            // the guard moved onto the scaled result, so the test moved with it.
            if (w * (long)f >= KLFB_RT_MAX_OUT || h * (long)f >= KLFB_RT_MAX_OUT)
                fprintf(stderr, "  [glfb] RT supersample: listed size %ldx%ld would "
                                "become %ldx%ld, at or above the %d allocation "
                                "guard — it will NOT be scaled\n",
                        w, h, w * (long)f, h * (long)f, KLFB_RT_MAX_OUT);
            g_rt_size[g_rt_nsize].w = (int32_t)w;
            g_rt_size[g_rt_nsize].h = (int32_t)h;
            g_rt_nsize++;
        }
        while (*p == ',' || *p == ' ') p++;
    }
    if (*p && g_rt_nsize >= KLFB_RT_SIZES)
        fprintf(stderr, "  [glfb] RT supersample: KL_GLFB_RT_SCALE holds more "
                        "than %d sizes; the tail (\"%s\") is IGNORED\n",
                KLFB_RT_SIZES, p);
    if (g_rt_nsize) {
        fprintf(stderr, "  [glfb] RT supersample x%d for", g_rt_factor);
        for (int i = 0; i < g_rt_nsize; i++)
            fprintf(stderr, " %dx%d", g_rt_size[i].w, g_rt_size[i].h);
        fprintf(stderr, " (KL_GLFB_RT_SCALE)\n");
    }
}

// The factor this allocation should be grown by, or 1 for "leave it alone".
static int klfb_rt_match(int32_t w, int32_t h) {
    klfb_rt_parse();
    if (!g_rt_nsize || g_rt_factor <= 1) return 1;
    if (w * (long)g_rt_factor >= KLFB_RT_MAX_OUT ||
        h * (long)g_rt_factor >= KLFB_RT_MAX_OUT) return 1;
    for (int i = 0; i < g_rt_nsize; i++)
        if (g_rt_size[i].w == w && g_rt_size[i].h == h) return g_rt_factor;
    return 1;
}
static int klfb_rt_on(void) {
    klfb_rt_parse();
    return g_rt_nsize > 0 && g_rt_factor > 1;
}

// A DEPTH/STENCIL renderbuffer allocated at a listed size grows with the colour
// target it will be attached beside, or the framebuffer goes INCOMPLETE the
// moment the two disagree. Matching is by size AND by format: glFramebuffer-
// Renderbuffer has no thunk in this file, so a renderbuffer cannot be paired
// with its FBO after the fact — but a COLOUR renderbuffer must therefore never
// be scaled either, because nothing downstream could then find its viewport.
// Colour render targets are textures in this engine (the census's `colour0 tex`
// lines), so that exclusion costs nothing and closes the hole by construction.
static int klfb_rt_is_depth_fmt(uint32_t fmt) {
    switch (fmt) {
    case 0x1902: case 0x81A5: case 0x81A6: case 0x81A7:  // DEPTH_COMPONENT*
    case 0x8CAC:                                          // DEPTH_COMPONENT32F
    case 0x84F9: case 0x88F0: case 0x8CAD:                // DEPTH_STENCIL*
    case 0x8D48:                                          // STENCIL_INDEX8
        return 1;
    }
    return 0;
}
static int klfb_rt_scale_rb(uint32_t fmt, int32_t *w, int32_t *h) {
    if (!klfb_rt_is_depth_fmt(fmt)) return 1;
    int f = klfb_rt_match(*w, *h);
    if (f > 1) { *w *= f; *h *= f; }
    return f;
}

// The texture names that came back larger than the guest asked for. Small and
// fixed: the census counted two such targets, and the ceiling is named rather
// than silently dropped so a title with more of them says so instead of
// half-scaling.
#define KLFB_RT_SCALED 64
static struct { uint32_t name; int factor; } g_rt_scaled[KLFB_RT_SCALED];
static unsigned g_rt_nscaled;
static pthread_mutex_t g_rt_lock = PTHREAD_MUTEX_INITIALIZER;

static void klfb_rt_note_scaled(uint32_t name, int factor) {
    pthread_mutex_lock(&g_rt_lock);
    for (unsigned i = 0; i < g_rt_nscaled; i++)
        if (g_rt_scaled[i].name == name) {
            g_rt_scaled[i].factor = factor;
            pthread_mutex_unlock(&g_rt_lock); return;
        }
    if (g_rt_nscaled < KLFB_RT_SCALED) {
        g_rt_scaled[g_rt_nscaled].name = name;
        g_rt_scaled[g_rt_nscaled].factor = factor;
        g_rt_nscaled++;
    } else {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] RT supersample: the scaled-target table is "
                            "full at %d (tex %u is the first dropped) — its "
                            "viewport will NOT be scaled\n",
                    KLFB_RT_SCALED, name);
    }
    pthread_mutex_unlock(&g_rt_lock);
}
// A texture re-specified at a size we do NOT scale has to leave the ledger, or
// its viewport keeps being multiplied against storage that shrank.
static void klfb_rt_forget_scaled(uint32_t name) {
    if (!g_rt_nscaled) return;
    pthread_mutex_lock(&g_rt_lock);
    for (unsigned i = 0; i < g_rt_nscaled; i++)
        if (g_rt_scaled[i].name == name) {
            g_rt_scaled[i] = g_rt_scaled[--g_rt_nscaled];
            break;
        }
    pthread_mutex_unlock(&g_rt_lock);
}
static int klfb_rt_scale_of_tex(uint32_t name) {
    if (!name || !g_rt_nscaled) return 1;
    int f = 1;
    pthread_mutex_lock(&g_rt_lock);
    for (unsigned i = 0; i < g_rt_nscaled; i++)
        if (g_rt_scaled[i].name == name) { f = g_rt_scaled[i].factor; break; }
    pthread_mutex_unlock(&g_rt_lock);
    return f;
}

// The factor in force for the CURRENT draw and read framebuffers. Cached at the
// two moments it can change — a framebuffer bind, and a colour-0 attach —
// rather than recomputed per glViewport, so the hot path is one load.
static int g_rt_draw_scale = 1, g_rt_read_scale = 1;
static uint32_t klfb_color_of_fbo(uint32_t fbo);      // the FBO colour ledger
static void klfb_rt_refresh(uint32_t draw_fb, uint32_t read_fb) {
    if (!klfb_rt_on()) { g_rt_draw_scale = g_rt_read_scale = 1; return; }
    g_rt_draw_scale = klfb_rt_scale_of_tex(klfb_color_of_fbo(draw_fb));
    g_rt_read_scale = klfb_rt_scale_of_tex(klfb_color_of_fbo(read_fb));
}

// THE SIZES WE DID NOT SCALE, named once each. If the HUD comes back no
// sharper, the first question is whether the allowlist even matched anything on
// this device — the two sizes came from one r26 census and a menu at a different
// layout would allocate different ones. So every texture that becomes the
// colour-0 attachment of a NON-eye framebuffer and was not scaled gets its size
// printed once, capped at 24 distinct sizes. Bounded, only while the knob is on,
// and it turns "extend the list to what?" from a guess into a grep.
#define KLFB_RT_SEEN 48   // r33: 24 was not hit (13 sizes), but oversize targets
                          // are named now too, so leave room rather than truncate
static struct { int32_t w, h; } g_rt_seen[KLFB_RT_SEEN];
static unsigned g_rt_nseen;
static void klfb_rt_note_candidate(int32_t w, int32_t h) {
    if (!klfb_rt_on() || w <= 0 || h <= 0) return;
    // r33: a target at or above the guard used to be dropped from the census
    // entirely, which made it INVISIBLE — the r33 log's job was "name the target
    // the menu is drawn into", and a 3840-wide one would not have appeared in
    // the answer at all. It is still never scaled; it is now merely named, with
    // the reason attached, so the next round reads a complete list.
    // r34: the guard is on the scaled RESULT now, so what makes a target
    // unreachable is what it would BECOME, not what it is.
    int oversize = (w * (long)g_rt_factor >= KLFB_RT_MAX_OUT ||
                    h * (long)g_rt_factor >= KLFB_RT_MAX_OUT);
    pthread_mutex_lock(&g_rt_lock);
    for (unsigned i = 0; i < g_rt_nseen; i++)
        if (g_rt_seen[i].w == w && g_rt_seen[i].h == h) {
            pthread_mutex_unlock(&g_rt_lock); return;
        }
    if (g_rt_nseen < KLFB_RT_SEEN) {
        g_rt_seen[g_rt_nseen].w = w; g_rt_seen[g_rt_nseen].h = h;
        g_rt_nseen++;
        pthread_mutex_unlock(&g_rt_lock);
        fprintf(stderr, "  [glfb] RT supersample: offscreen colour target %dx%d "
                        "is NOT on the KL_GLFB_RT_SCALE list%s\n", w, h,
                oversize ? " (and could not be scaled anyway — it would exceed "
                           "the allocation guard)" : "");
        return;
    }
    pthread_mutex_unlock(&g_rt_lock);
}

// How many draws landed in a scaled target, reported every 15 s so a worn run
// says whether the HUD passes actually went there. Free when the knob is off.
static unsigned long g_rt_draws;
static double klfb_now(void);
static void klfb_rt_note_draw(void) {
    if (g_rt_draw_scale <= 1) return;
    g_rt_draws++;
    static double next;
    double t = klfb_now();
    if (next == 0.0) { next = t + 15.0; return; }
    if (t < next) return;
    next = t + 15.0;
    fprintf(stderr, "  [glfb] RT supersample: %lu draws into scaled targets "
                    "(%u scaled)\n", g_rt_draws, g_rt_nscaled);
}

// Counted, not modelled. Mutable storage is per-level and re-specifiable, so a
// byte figure would need a level table; the count is here so that a leak taking
// this route cannot hide behind a census that only knows about glTexStorage*.
// Nine arguments, all in registers bar the last pointer, so the natural
// signature is safe (unlike glTexSubImage3D above).
static void (*g_real_TexImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                 int32_t, uint32_t, uint32_t, const void *);
// Defined in the ABI-thunks block below; the texture-allocation thunks feed it.
static void klfb_note_tex_storage(uint32_t name, uint32_t fmt, int32_t w,
                                  int32_t h);
// Defined just below; the texture-call trace shared by every wrapper here.
static int klfb_trace_tex(void);
static unsigned g_texcalls;

// ---------------------------------------------------------------------------
// KL_GLFB_ANISO=<n> — anisotropic filtering the guest never asks for.
//
// Minecraft/Sodium set mipmapped MIN_FILTERs on the block atlas (non-trilinear)
// and never write TEXTURE_MAX_ANISOTROPY (`[lod] … MAX_ANISOTROPY 0`), so
// oblique ground and walls blur toward the higher mip. ANGLE's Metal backend
// has EXT_texture_filter_anisotropic, so this sets MAX_ANISOTROPY = n (clamped
// to GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT) on the bound TEXTURE_2D whenever the
// guest gives it a MIPMAPPED min filter — the one call that identifies the
// textures that matter (the atlas) and not the GUI's level-0-only ones. The
// guest's own filters are left exactly as set. Once per texture object: a
// bitmap by name, cleared when the name is re-specified at level 0 (a reused
// name is a new object). Default 0 = off; the guest's own anisotropy writes
// are still dropped (klfb_TexParameteri).
#define KLFB_ANISO_NAMES 65536
static uint8_t  g_aniso_done[KLFB_ANISO_NAMES / 8];
static unsigned g_aniso_textures;
static float    g_aniso_level;
static double   g_aniso_first;

static float klfb_aniso_wanted(void) {
    static int v = -1;
    if (v < 0) {
        v = kl_env_int("KL_GLFB_ANISO", 0);
        if (v < 0) v = 0;
        if (v > 16) v = 16;
        if (v > 1) {
            float mx = 0;
            void (*gf)(uint32_t, float *) = asym("glGetFloatv");
            if (gf) gf(0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &mx);
            if (mx < 1.0f) {
                fprintf(stderr, "  [glfb] KL_GLFB_ANISO=%d: the driver reports no "
                                "anisotropic filtering — off\n", v);
                v = 0;
            } else {
                g_aniso_level = (float)v < mx ? (float)v : mx;
            }
        }
    }
    return v > 1 ? g_aniso_level : 0.0f;
}

static void klfb_aniso_forget(uint32_t target, int32_t level) {
    if (level != 0 || target != 0x0DE1 || !g_aniso_textures || !a_glGetIntegerv) return;
    int32_t bound = 0;
    a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
    if (bound > 0 && bound < KLFB_ANISO_NAMES)
        g_aniso_done[bound >> 3] &= (uint8_t)~(1u << (bound & 7));
}

static void klfb_aniso_inject(uint32_t target, int32_t bound, int32_t minfilter) {
    if (target != 0x0DE1 || bound <= 0 || bound >= KLFB_ANISO_NAMES) return;
    if (minfilter < 0x2700 || minfilter > 0x2703) return;   // the *_MIPMAP_* four
    float n = klfb_aniso_wanted();
    if (n <= 1.0f) return;
    uint8_t bit = (uint8_t)(1u << (bound & 7));
    if (g_aniso_done[bound >> 3] & bit) return;
    g_aniso_done[bound >> 3] |= bit;
    static void (*tpf)(uint32_t, uint32_t, float);
    if (!tpf) tpf = asym("glTexParameterf");
    if (!tpf) return;
    tpf(0x0DE1, 0x84FE /* TEXTURE_MAX_ANISOTROPY_EXT */, n);
    g_aniso_textures++;
    double now = klfb_now();
    if (g_aniso_first == 0) {
        g_aniso_first = now;
        fprintf(stderr, "  [glfb] anisotropic filtering: %.0fx on texture %d (first "
                        "mipmapped min filter 0x%x) — KL_GLFB_ANISO\n",
                (double)n, bound, minfilter);
    }
    static int said_minute;
    if (!said_minute && now - g_aniso_first >= 60.0) {
        said_minute = 1;
        fprintf(stderr, "  [glfb] anisotropic filtering: %.0f x injected on %u "
                        "mipmapped texture(s) (KL_GLFB_ANISO)\n",
                (double)n, g_aniso_textures);
    }
}

static void klfb_TexImage2D(uint32_t target, int32_t level, int32_t ifmt,
                            int32_t w, int32_t h, int32_t border, uint32_t fmt,
                            uint32_t type, const void *pixels) {
    if (level == 0) g_census_teximage++;
    klfb_aniso_forget(target, level);   // KL_GLFB_ANISO: a re-specified name
    // GL_RGB8 with a GL_RGBA upload is a spec violation ES 3.0 rejects
    // (INVALID_OPERATION) and desktop GL always tolerated, which is why
    // togl ships it: six textures, all of them lost. Promote the
    // internalformat to RGBA8 — the upload is four-channel either way, and
    // the alpha byte simply rides along unused for anything that samples
    // .rgb. The NOSRGB rewrite beside this is the same class of repair.
    if (ifmt == 0x8051 /* GL_RGB8 */ && fmt == 0x1908 /* GL_RGBA */)
        ifmt = 0x8058 /* GL_RGBA8 */;
    // Traced under the same knob as glTexParameteri: HL2VR's togl is the first
    // guest whose uploads ANGLE rejects (0x502 on the format triple), and a
    // black-everything run names neither the call nor the format without this.
    if (klfb_trace_tex())
        fprintf(stderr, "  [glfb] #%u glTexImage2D ifmt=0x%04x %dx%d fmt=0x%04x "
                        "type=0x%04x\n", g_texcalls++, ifmt, w, h, fmt, type);
    // A MUTABLE allocation is an allocation: recording only glTexStorage2D/3D
    // leaves a render target created this way invisible to every readback path
    // — the same blindness as a full table, by a different door.
    // r32: the mutable half of the render-target supersample. UE4's OpenGL RHI
    // creates some render targets with glTexImage2D(NULL) rather than
    // glTexStorage2D, so the grow has to happen on BOTH doors or the fix
    // silently applies to neither. Only a NULL-pixels level-0 2D allocation is
    // eligible: an allocation carrying pixels is an IMAGE, and growing it would
    // read past the end of the guest's buffer.
    int rtf = 1;
    if (level == 0 && target == 0x0DE1 /* TEXTURE_2D */ && !pixels)
        rtf = klfb_rt_match(w, h);
    if (level == 0 && w > 0 && h > 0 && a_glGetIntegerv) {
        int32_t bound = -1;
        a_glGetIntegerv(target == 0x8C1A /* TEXTURE_2D_ARRAY */ ? 0x8C1D
                                                                : 0x8069,
                        &bound);
        if (bound > 0) {
            if (rtf > 1) {
                w *= rtf; h *= rtf;
                klfb_rt_note_scaled((uint32_t)bound, rtf);
                fprintf(stderr, "  [glfb] RT supersample: tex %d glTexImage2D "
                                "%dx%d -> %dx%d (ifmt=0x%x)\n",
                        bound, w / rtf, h / rtf, w, h, ifmt);
            } else if (target == 0x0DE1) {
                klfb_rt_forget_scaled((uint32_t)bound);
            }
            klfb_note_tex_storage((uint32_t)bound, (uint32_t)ifmt, w, h);
        }
    }
    if (g_real_TexImage2D)
        g_real_TexImage2D(target, level, ifmt, w, h, border, fmt, type, pixels);
    // Self-attributing error report: the ERRSCAN bracket names a probe point,
    // not the call that raised, and togl's texture stream made the two differ
    // by hundreds of calls. glGetError clears, so only the raiser prints.
    if (a_glGetError) {
        uint32_t e = a_glGetError();
        if (e) fprintf(stderr, "  [glfb] glTexImage2D(ifmt=0x%04x, %dx%d, fmt=0x%04x, "
                               "type=0x%04x) -> err 0x%x\n", ifmt, w, h, fmt, type, e);
    }
}

static uint32_t (*g_real_CreateShader)(uint32_t);
static uint32_t klfb_CreateShader(uint32_t type) {
    klfb_census_made(KLC_SHADER, 1);
    return g_real_CreateShader ? g_real_CreateShader(type) : 0;
}
static void (*g_real_DeleteShader)(uint32_t);
static void klfb_DeleteShader(uint32_t s) {
    klfb_census_killed(KLC_SHADER, 1);
    if (g_real_DeleteShader) g_real_DeleteShader(s);
}
static uint32_t (*g_real_CreateProgram)(void);
static uint32_t klfb_CreateProgram(void) {
    klfb_census_made(KLC_PROG, 1);
    return g_real_CreateProgram ? g_real_CreateProgram() : 0;
}
static void (*g_real_DeleteProgram)(uint32_t);
static void klfb_DeleteProgram(uint32_t p) {
    klfb_census_killed(KLC_PROG, 1);
    if (g_real_DeleteProgram) g_real_DeleteProgram(p);
}

// KL_GLFB_NOSRGB=1 substitutes GL_RGBA8 for GL_SRGB8_ALPHA8 at allocation. The
// census found the guest's eye-sized render target is SRGB8_ALPHA8, and sRGB is a
// case Metal's fixed-function blit cannot always service — it falls back to a
// compute blit, which is the AGX family that aborts. Swapping the format is the
// one-line way to test that without touching anything else; the two are upload- and
// attachment-compatible, so only the colour transfer function changes. A frame
// captured with this set is wrong (un-decoded sRGB), which is fine for a probe.

static uint32_t klfb_maybe_unsrgb(uint32_t fmt) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_NOSRGB", 0);
    return (on && fmt == GL_SRGB8_ALPHA8) ? GL_RGBA8 : fmt;
}

// Defined in the ABI-thunks block below; the glTexStorage2D thunk feeds it.
static void klfb_note_tex_storage(uint32_t name, uint32_t fmt, int32_t w,
                                  int32_t h);
// ...and the mip-chain half of the same record (r30), fed by the same thunk
// and by every upload path.
static void klfb_note_tex_alloc_levels(uint32_t name, int32_t levels);
static void klfb_note_tex_level(uint32_t name, int32_t level, long bytes);

// The 2-D texture bound on this thread, tracked rather than queried: every
// upload names its destination by binding first, so this IS the destination,
// and reading it costs nothing where a glGetIntegerv would cost a round trip
// into ANGLE on a path that runs thousands of times during load.
static _Thread_local uint32_t g_bound2d;

// KL_GLFB_TRACE_TEX=1 logs every texture call rather than only first sightings, so
// the tail of a crashing run names the exact call it died on. The abort happens on
// a Metal compiler thread, so the last line is the trigger's neighbourhood rather
// than provably the trigger itself — but it bounds the search to one call.
static int klfb_trace_tex(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_TRACE_TEX", 0);
    return on;
}

static void (*g_real_TexSubImage2D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                    int32_t, uint32_t, uint32_t, const void *);

// The parameters set on a texture before it is allocated matter as much as the
// format: swizzle in particular (GL_TEXTURE_SWIZZLE_R..A, 0x8E42-0x8E45) is a
// state a Metal backend may have to emulate rather than set natively, and Unity
// uses it to make an R8 texture behave like the GL_LUMINANCE/GL_ALPHA formats
// GLES 3 dropped. The trace shows 156 of these calls, a burst of them immediately
// before the allocation the abort follows, so they are logged with their arguments.
static void (*g_real_TexParameteri)(uint32_t, uint32_t, int32_t);

// KL_GLFB_TRACE_FBO=1 logs the framebuffer-object lifecycle with thread ids.
// FBOs (like VAOs) are NOT shared between contexts, and under KL_GLFB_SHARED
// the guest runs two of them: an FBO generated on the setup thread's context
// and drawn with on the render thread's is a different, empty object there —
// "incomplete: no attachments and default size is zero", which is exactly
// what the blank frame's 0x506s report (caught via ErrorSet::validationError).
// This trace shows who made each FBO and who uses it.
static void (*g_real_GenFramebuffers)(int32_t, uint32_t *);
static void (*g_real_BindFramebuffer)(uint32_t, uint32_t);
static void (*g_real_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t,
                                           int32_t);
// ...and its array-slice sibling. glFramebufferTexture2D cannot name a layer, so
// a guest whose eye swapchain is one 2-slice array texture — every OpenXR guest
// in single-pass-instanced layout — attaches its eyes exclusively through this
// entry point. It was untraced, which is why "which FBO does the guest draw the
// eye into?" had no answer at all on that path while having a complete one on
// Beat Saber's.
static void (*g_real_FramebufferTextureLayer)(uint32_t, uint32_t, uint32_t, int32_t,
                                              int32_t);

static int klfb_tex_info(uint32_t name, uint32_t *fmt, int32_t *w, int32_t *h);

// Every FBO's current colour-0 attachment, for the draw census below. This is a
// different table from g_fbo_stage, which deliberately keeps its sixteen slots
// for EYE framebuffers only: the census's whole job is to say which of the
// guest's OTHER framebuffers the picture is landing in, so it has to know about
// exactly the ones that map refuses.
#define KLFB_FBO_COLOR 48
static struct { uint32_t fbo, tex; int layer; } g_fbo_color[KLFB_FBO_COLOR];
static unsigned g_nfbo_color;

static void klfb_note_fbo_color(uint32_t fbo, uint32_t tex, int layer) {
    if (!fbo) return;
    for (unsigned i = 0; i < g_nfbo_color; i++)
        if (g_fbo_color[i].fbo == fbo) {
            g_fbo_color[i].tex = tex; g_fbo_color[i].layer = layer; return;
        }
    if (g_nfbo_color < KLFB_FBO_COLOR) {
        g_fbo_color[g_nfbo_color].fbo = fbo;
        g_fbo_color[g_nfbo_color].tex = tex;
        g_fbo_color[g_nfbo_color].layer = layer;
        g_nfbo_color++;
    }
}

// The colour-0 attachment an FBO currently carries, or 0 if nothing recorded
// one. The depth census pairs a depth attachment with it so the two sizes can
// be compared at the moment they were attached together.
static uint32_t klfb_color_of_fbo(uint32_t fbo) {
    for (unsigned i = 0; i < g_nfbo_color; i++)
        if (g_fbo_color[i].fbo == fbo) return g_fbo_color[i].tex;
    return 0;
}

// Highest FBO name handed out so far — the census scans 1..g_fbomax. Binding a
// name glGenFramebuffers never returned is INVALID_OPERATION in ES 3.0, so the
// scan needs a real upper bound, not a guess.
static uint32_t g_fbomax;

static int klfb_trace_fbo(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_TRACE_FBO", 0);
    return on;
}
static uint64_t klfb_tid(void) { uint64_t t = 0; pthread_threadid_np(NULL, &t); return t; }

static void klfb_GenFramebuffers(int32_t n, uint32_t *ids) {
    if (g_real_GenFramebuffers) g_real_GenFramebuffers(n, ids);
    klfb_census_made(KLC_FBO, n);
    if (ids)
        for (int32_t i = 0; i < n; i++)
            if (ids[i] > g_fbomax) g_fbomax = ids[i];
    // The context is part of a framebuffer's identity, not decoration: FBOs are
    // container objects and are NOT shared between GL contexts, so a guest that
    // records the context it created one under (Unity does) will refuse to bind
    // it under any other. Reported unconditionally — a handful of lines a run,
    // and without them "which context was this name made under?" is unanswerable
    // after the fact.
    if (ids)
        for (int32_t i = 0; i < n; i++)
            fprintf(stderr, "  [glfb] t%llu glGenFramebuffers -> %u (ctx %p)\n",
                    (unsigned long long)klfb_tid(), ids[i],
                    kl_egl_current_context());
}
// The stage observation, defined with the eye-texture table further down.
static int  klfb_stage_of_tex(uint32_t tex);
static int  klfb_stage_of_fbo(uint32_t fbo);
static void klfb_map_fbo(uint32_t fbo, uint32_t tex);
static void klfb_note_render_stage(int stage);
static void klfb_note_eye_fbo(uint32_t fbo, uint32_t touched);
// r25: re-point the injected scene-depth sampler at whatever the DRAW
// framebuffer's depth attachment now is. Defined with the rest of that machinery
// below the depth census; called from here and from glUseProgram.
static void klfb_depth_fetch_apply(void);
static uint32_t g_draw_fb;
// ...and the framebuffer the last draw CALL targeted, which is a different
// question from the last binding — see klfb_note_draw.
static uint32_t g_last_draw_fb;
// How many blits the guest issued, and how many of them read the DEFAULT
// framebuffer. The second number is the one worth having: a blit whose source
// is 0 is legal, error-free and silent, and it is how a whole frame goes
// missing between the texture the guest drew into and the one it presents.
static unsigned long g_blits, g_blits_read0;

// ...and the READ binding, which had no tracker at all. An attachment call
// names a TARGET, not a framebuffer, so attributing every attach to g_draw_fb
// is a guess — and a guest that configures a blit SOURCE attaches through
// GL_READ_FRAMEBUFFER, where the guess is wrong every time and the trace says
// so confidently.
static uint32_t g_read_fb;

// Is this target a DRAW binding? GL_FRAMEBUFFER binds both.
static int klfb_is_draw_target(uint32_t target) {
    return target == 0x8D40 /* FRAMEBUFFER */ || target == 0x8CA9 /* DRAW_FRAMEBUFFER */;
}
static int klfb_is_read_target(uint32_t target) {
    return target == 0x8D40 /* FRAMEBUFFER */ || target == 0x8CA8 /* READ_FRAMEBUFFER */;
}
// Which framebuffer an attachment call addresses. DRAW wins for GL_FRAMEBUFFER
// only because the two are then the same object.
static uint32_t klfb_fb_for_target(uint32_t target) {
    return klfb_is_draw_target(target) ? g_draw_fb : g_read_fb;
}

// The guest's own call site for a GL entry point, as "<image>+0x<off>". Two
// binds that look identical in a trace are a completely different reading
// depending on whether they come from one helper or two, and a GL trace has no
// other way to say so — the arguments are all the state there is.
static const char *klfb_caller(void *ret) {
    static __thread char buf[64];
    size_t off = 0;
    const char *img = kl_addr_image(ret, &off);
    if (img) snprintf(buf, sizeof buf, "%s+0x%zx", img, off);
    else     snprintf(buf, sizeof buf, "%p", ret);
    return buf;
}

// The last few binds, kept unconditionally and cheaply, because the question
// "which name did the guest ASK for?" can only be answered after the fact — by
// the time GL reports an incomplete framebuffer the bind is thousands of calls
// back, and turning on the full FBO trace to catch it buries the answer in a
// firehose. Deliberately lock-free and racy: a torn entry is still a name, and
// this is read only by a diagnostic that is already reporting a broken state.
#define KLFB_BINDLOG 12
static struct { uint32_t target, fb; uint64_t tid; const char *site; void *ctx; } g_bindlog[KLFB_BINDLOG];
static unsigned g_bindlog_n;
// Whether the REAL glBindFramebuffer produced an error, i.e. whether GL took the
// name. This is the whole of candidate (b): a refused bind leaves the previous
// binding in place and says nothing anywhere.
static uint32_t g_bindlog_err[KLFB_BINDLOG];
static __thread unsigned g_bindlog_slot;
static int glfb_errscan(void);

// ---------------------------------------------------------------------------
// ROUND 26 -- the interior arm, and the ordered per-frame clear/draw trace.
//
// Launch #25 closed culling for the Kwik-E-Mart void: the census counted ~287k
// triangles over ~1330 draws a frame landing in fb1 -- the eye framebuffer,
// colour0 tex 1/3 layer 0/1, 4128x4416, colourmask 1111, correct viewport --
// while the room read as a flat pale sky-blue. The world is SUBMITTED and
// LOST, and the two readings left are:
//
//   (a) the pixels never survive in fb1: a late full-frame clear, an overpaint
//       by a fullscreen draw, or a depth state that rejects everything after
//       something wrote the whole depth buffer near;
//   (b) fb1 is correct at end of frame and the loss is downstream -- the
//       present/compositor path hands the compositor the wrong image.
//
// One PICTURE of fb1 taken indoors splits those outright, and one ORDERED
// trace of the frame that produced it names the wiper if it is (a). Both are
// read-side: no GL call is added to the guest's stream and no pixel is read
// back in the trace path.
//
// Two problems had to be solved to take either measurement indoors.
//
// The capture already arms on a signal from the guest (KL_GLFB_OUT_ARM_CSM,
// the first depth-only framebuffer) because a device build is launched from
// the Home View by a person who then has to walk a menu and load a level. That
// signal fires on the FRONT END here -- SHAR runs three CSM cascades from the
// first world frame -- so it is useless for "the moment Austin walks into the
// Kwik-E-Mart", which is minutes later and is a guest event nothing in this
// file can see. So the arm is EXPORTED: kl_glfb_arm_capture() is called by
// whoever owns that knowledge, which for this guest is kl_sdl.c's
// InteriorManager::Enter interposer (the player pressing the button on a
// doorway). KL_GLFB_OUT_ARM_INTERIOR=1 makes the arm REQUIRED, and it is
// sufficient by itself -- ARM_CSM is a separate gate and either one arms when
// both are set, so an interior build does not silently also wait on a shadow
// pass.
//
// And the frame the picture is OF has to be readable as a program, which is
// what the trace is. The draw-state census samples (first two draws per
// framebuffer, then one in 4096, at most eight lines) -- deliberately, because
// ten pieces of GL state on six million draws is a second frame budget. A
// sample cannot answer "what happened LAST", which is the entire question when
// something paints over a finished world. So KL_GLFB_FRAMETRACE=N traces the
// next N frames COMPLETELY and IN ORDER, and pays for it by being three frames
// long and by coalescing: a run of consecutive draws into the same framebuffer
// with identical state is one line with a count, not one line per draw.
//
// Read it like this, against the PNG the same arming produced:
//
//   fb1 looks right in the PNG          -> (b). The eye texture the guest drew
//                                          is correct and the loss is in the
//                                          hand-off; the trace is the control.
//   fb1 is the flat clear colour, and the trace's LAST event on fb1 is a clear
//   or a full-viewport draw               -> (a), named. That line is the wiper.
//   fb1 is flat with a healthy trace (world draws last, colourmask 1111,
//   sensible depth)                       -> (a) by depth: look at the depth
//                                          func/mask of the runs before it and
//                                          at whatever wrote depth first.
//
// KL_GLFB_FRAMETRACE=0 and KL_GLFB_OUT_ARM_INTERIOR=0 are the backouts.
static int g_capture_armed;   // plain int: __atomic_* refuse an _Atomic operand

void kl_glfb_arm_capture(void) {
    if (__atomic_exchange_n(&g_capture_armed, 1, __ATOMIC_RELAXED)) return;
    fprintf(stderr, "  [glfb] capture/trace ARMED by the guest — the delay, "
                    "spacing and budget start counting from this swap\n");
    fflush(stderr);
}

int kl_glfb_frametrace(void) {
    static int n = -1;
    if (n < 0) { n = kl_env_int("KL_GLFB_FRAMETRACE", 0); if (n < 0) n = 0; }
    return n;
}

// Tracing RIGHT NOW. Read on every draw call of every frame of every run, so
// it is one relaxed atomic load and nothing else when the trace is off.
static int         g_ftrace_on;
static unsigned    g_ftrace_frame;   // 0 .. N-1
static unsigned    g_ftrace_seq;     // event index within the frame
static unsigned    g_ftrace_lines;   // spent this frame
static int         g_ftrace_done;

// A frame whose draws refuse to coalesce (state churning per draw) would
// otherwise write one line per draw for three frames. The cap makes the
// instrument's worst case a known number instead of the guest's draw count.
#define KLFB_FTRACE_MAX_LINES 800

// The pending run of same-state draws. Flushed by the next event of any other
// kind, by a state change, and by the frame boundary.
static struct {
    int      live;
    int32_t  fb, prog;
    unsigned draws, seq;
    int      dt, dm, bl, sc, cm[4];
    int32_t  df, sfac, dfac, box[4], vp[4];
} g_ftrun;

static int klfb_ftrace_budget(void) {
    if (g_ftrace_lines >= KLFB_FTRACE_MAX_LINES) {
        if (g_ftrace_lines == KLFB_FTRACE_MAX_LINES) {
            g_ftrace_lines++;
            fprintf(stderr, "  [ftrace] f%u — capped at %d lines; the rest of "
                            "this frame is NOT traced\n",
                    g_ftrace_frame, KLFB_FTRACE_MAX_LINES);
        }
        return 0;
    }
    g_ftrace_lines++;
    return 1;
}

static void klfb_ftrace_flush(void) {
    if (!g_ftrun.live) return;
    g_ftrun.live = 0;
    if (!klfb_ftrace_budget()) return;
    fprintf(stderr,
            "  [ftrace] f%u s%-4u DRAWS x%-5u fb%-3d prog %-4d vp %d,%d %dx%d "
            "depthtest %d func 0x%x depthmask %d blend %d(0x%x,0x%x) "
            "colourmask %d%d%d%d scissor %d box %d,%d %dx%d\n",
            g_ftrace_frame, g_ftrun.seq, g_ftrun.draws, g_ftrun.fb, g_ftrun.prog,
            g_ftrun.vp[0], g_ftrun.vp[1], g_ftrun.vp[2], g_ftrun.vp[3],
            g_ftrun.dt, (unsigned)g_ftrun.df, g_ftrun.dm, g_ftrun.bl,
            (unsigned)g_ftrun.sfac, (unsigned)g_ftrun.dfac,
            g_ftrun.cm[0], g_ftrun.cm[1], g_ftrun.cm[2], g_ftrun.cm[3],
            g_ftrun.sc, g_ftrun.box[0], g_ftrun.box[1], g_ftrun.box[2],
            g_ftrun.box[3]);
}

// The GL entry points the trace reads. Resolved once; all of them are pure
// queries, and none of them is issued when the trace is off.
static uint8_t (*a_ft_IsEnabled)(uint32_t);
static void    (*a_ft_GetBooleanv)(uint32_t, uint8_t *);
static void    (*a_ft_GetFloatv)(uint32_t, float *);

static int klfb_ftrace_live(void) {
    if (!__atomic_load_n(&g_ftrace_on, __ATOMIC_RELAXED)) return 0;
    if (!a_glGetIntegerv) return 0;
    static int looked;
    if (!looked) {
        looked = 1;
        a_ft_IsEnabled   = asym("glIsEnabled");
        a_ft_GetBooleanv = asym("glGetBooleanv");
        a_ft_GetFloatv   = asym("glGetFloatv");
    }
    return a_ft_IsEnabled && a_ft_GetBooleanv;
}

static void klfb_ftrace_draw(int32_t fb) {
    if (!klfb_ftrace_live()) return;
    int32_t prog = -1, vp[4] = {0,0,0,0}, box[4] = {0,0,0,0};
    int32_t df = 0, sfac = 0, dfac = 0;
    uint8_t cm[4] = {1,1,1,1}, dm = 1;
    a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
    a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    a_glGetIntegerv(0x0C10 /* SCISSOR_BOX */, box);
    a_glGetIntegerv(0x0B74 /* DEPTH_FUNC */, &df);
    a_glGetIntegerv(0x80C9 /* BLEND_SRC_RGB */, &sfac);
    a_glGetIntegerv(0x80C8 /* BLEND_DST_RGB */, &dfac);
    a_ft_GetBooleanv(0x0C23 /* COLOR_WRITEMASK */, cm);
    a_ft_GetBooleanv(0x0B72 /* DEPTH_WRITEMASK */, &dm);
    int dt = a_ft_IsEnabled(0x0B71 /* DEPTH_TEST */)   ? 1 : 0;
    int bl = a_ft_IsEnabled(0x0BE2 /* BLEND */)        ? 1 : 0;
    int sc = a_ft_IsEnabled(0x0C11 /* SCISSOR_TEST */) ? 1 : 0;

    if (g_ftrun.live && g_ftrun.fb == fb && g_ftrun.prog == prog &&
        g_ftrun.dt == dt && g_ftrun.dm == (dm ? 1 : 0) && g_ftrun.bl == bl &&
        g_ftrun.sc == sc && g_ftrun.df == df && g_ftrun.sfac == sfac &&
        g_ftrun.dfac == dfac &&
        g_ftrun.cm[0] == (cm[0] ? 1 : 0) && g_ftrun.cm[1] == (cm[1] ? 1 : 0) &&
        g_ftrun.cm[2] == (cm[2] ? 1 : 0) && g_ftrun.cm[3] == (cm[3] ? 1 : 0) &&
        !memcmp(g_ftrun.vp, vp, sizeof vp) &&
        !memcmp(g_ftrun.box, box, sizeof box)) {
        g_ftrun.draws++;
        return;
    }
    klfb_ftrace_flush();
    g_ftrun.live = 1;
    g_ftrun.fb = fb; g_ftrun.prog = prog;
    g_ftrun.draws = 1;
    g_ftrun.seq = g_ftrace_seq++;
    g_ftrun.dt = dt; g_ftrun.dm = dm ? 1 : 0; g_ftrun.bl = bl; g_ftrun.sc = sc;
    g_ftrun.df = df; g_ftrun.sfac = sfac; g_ftrun.dfac = dfac;
    for (int i = 0; i < 4; i++) g_ftrun.cm[i] = cm[i] ? 1 : 0;
    memcpy(g_ftrun.vp, vp, sizeof vp);
    memcpy(g_ftrun.box, box, sizeof box);
}

// glClear only. glClearBufferfv is not in this guest's stream (the Pure3D
// path clears through glClear), and a hook that guessed at one would be a
// second timeline nobody had checked against the first.
static void klfb_ftrace_clear(uint32_t mask) {
    if (!klfb_ftrace_live()) return;
    klfb_ftrace_flush();
    if (!klfb_ftrace_budget()) return;
    int32_t fb = -1, box[4] = {0,0,0,0}, vp[4] = {0,0,0,0};
    uint8_t cm[4] = {1,1,1,1}, dm = 1;
    float cc[4] = {0,0,0,0};
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    a_glGetIntegerv(0x0C10, box);
    a_glGetIntegerv(0x0BA2, vp);
    a_ft_GetBooleanv(0x0C23, cm);
    a_ft_GetBooleanv(0x0B72, &dm);
    if (a_ft_GetFloatv) a_ft_GetFloatv(0x0C22 /* COLOR_CLEAR_VALUE */, cc);
    int sc = a_ft_IsEnabled(0x0C11) ? 1 : 0;
    fprintf(stderr,
            "  [ftrace] f%u s%-4u CLEAR         fb%-3d mask 0x%x colour "
            "(%.3f %.3f %.3f %.3f) vp %d,%d %dx%d colourmask %d%d%d%d "
            "depthmask %d scissor %d box %d,%d %dx%d\n",
            g_ftrace_frame, g_ftrace_seq++, fb, mask,
            (double)cc[0], (double)cc[1], (double)cc[2], (double)cc[3],
            vp[0], vp[1], vp[2], vp[3],
            cm[0] ? 1 : 0, cm[1] ? 1 : 0, cm[2] ? 1 : 0, cm[3] ? 1 : 0,
            dm ? 1 : 0, sc, box[0], box[1], box[2], box[3]);
}

static void klfb_ftrace_bind(int32_t from, int32_t to) {
    if (from == to) return;
    if (!klfb_ftrace_live()) return;
    klfb_ftrace_flush();
    if (!klfb_ftrace_budget()) return;
    fprintf(stderr, "  [ftrace] f%u s%-4u BIND          fb%d -> fb%d\n",
            g_ftrace_frame, g_ftrace_seq++, from, to);
}

// The frame boundary, which is the same swap the capture arms and spends its
// budget on -- so the PNG and the trace describe the same frames.
static void klfb_ftrace_swap(void) {
    int n = kl_glfb_frametrace();
    if (n <= 0 || g_ftrace_done) return;
    static int gate = -1, delay;
    static unsigned since;
    if (gate < 0) {
        gate  = kl_env_on("KL_GLFB_OUT_ARM_INTERIOR", 0);
        delay = kl_env_int("KL_GLFB_OUT_DELAY", 0);
        if (delay < 0) delay = 0;
    }
    if (gate && !__atomic_load_n(&g_capture_armed, __ATOMIC_RELAXED)) return;
    if ((int)(since++) < delay) return;
    klfb_ftrace_flush();
    if (!__atomic_load_n(&g_ftrace_on, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_ftrace_on, 1, __ATOMIC_RELAXED);
        g_ftrace_frame = 0;
        fprintf(stderr, "  [ftrace] tracing the next %d frame(s) in order — "
                        "every clear, every draw-framebuffer bind, and every "
                        "run of same-state draws\n", n);
    } else if (++g_ftrace_frame >= (unsigned)n) {
        __atomic_store_n(&g_ftrace_on, 0, __ATOMIC_RELAXED);
        g_ftrace_done = 1;
        fprintf(stderr, "  [ftrace] %d frame(s) traced — the instrument is "
                        "spent for this run\n", n);
        fflush(stderr);
        return;
    }
    g_ftrace_seq   = 0;
    g_ftrace_lines = 0;
    fflush(stderr);
}

// ---------------------------------------------------------------------------
// KL_VRR: make ANGLE re-read the rate-map registry for FBOs it already synced.
//
// ANGLE decides whether a render pass is foveated in
// FramebufferMtl::prepareRenderPass, and that runs ONLY from
// FramebufferMtl::syncState — i.e. when the FBO has dirty bits (an attachment,
// draw buffers, contents changed). The decision, rate map included, is then
// cached in mRenderPassDesc for every later pass on that FBO. So a size rule
// registered AFTER an FBO's first sync never reaches it, and a rule retired
// after it keeps foveating it with a map nobody unwarps any more.
//
// Unity never showed this: its MSAA scene renderbuffer is created after the eye
// textures, which is when the map arrives. Vivecraft shows it on the first
// frame: the "3D Render" FBO (fb9 on launch 3) is created and cleared at
// 4032x3234 BEFORE the first xrEndFrame, which is when the eyes are re-pointed
// and the compositor builds the map (provideEyeTexture -> updateGuestRateMap).
// Without this the size rule would be registered and foveate nothing but the
// swapchain.
//
// The repair is the cheapest dirty bit there is: on the first bind of each FBO
// after the rule changes (a generation counter), re-issue glDrawBuffers with
// the FBO's own current values. Frontend setDrawBuffers sets
// DIRTY_BIT_DRAW_BUFFERS unconditionally, the next draw or clear syncs, and
// prepareRenderPass looks the map up again. State is unchanged. Once per FBO
// per rule change (a handful per session); free otherwise.
static unsigned g_vrr_gen;          // bumped on every size-rule change
static int      g_vrr_rule_w, g_vrr_rule_h;
static unsigned g_vrr_rule_min;
static void    *g_vrr_rule_map;
#define KLFB_VRR_FBO_MAX 4096
static unsigned g_vrr_fbo_gen[KLFB_VRR_FBO_MAX];
static unsigned g_vrr_lines;
static void (*a_glDrawBuffers)(int32_t, const uint32_t *);
static uint32_t klfb_depth_of_fbo(uint32_t fbo);
static int klfb_stage_of_tex(uint32_t tex);

static void klfb_vrr_touch(uint32_t fb) {
    unsigned gen = __atomic_load_n(&g_vrr_gen, __ATOMIC_ACQUIRE);
    if (!gen || !fb) return;
    if (fb >= KLFB_VRR_FBO_MAX) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] vrr: framebuffer name %u is past the %d this "
                            "resync tracks — such FBOs keep whatever rate map "
                            "ANGLE gave them when they were last synced\n",
                    fb, KLFB_VRR_FBO_MAX);
        return;
    }
    if (g_vrr_fbo_gen[fb] == gen) return;
    g_vrr_fbo_gen[fb] = gen;
    if (!a_glDrawBuffers) a_glDrawBuffers = asym("glDrawBuffers");
    if (!a_glDrawBuffers || !a_glGetIntegerv) return;
    static int32_t maxdb;
    if (!maxdb) {
        a_glGetIntegerv(0x8824 /* MAX_DRAW_BUFFERS */, &maxdb);
        if (maxdb < 1) maxdb = 1;
        if (maxdb > 8) maxdb = 8;
    }
    uint32_t bufs[8];
    for (int32_t i = 0; i < maxdb; i++) {
        int32_t v = 0;
        a_glGetIntegerv(0x8825 + (uint32_t)i /* DRAW_BUFFERi */, &v);
        bufs[i] = (uint32_t)v;
    }
    a_glDrawBuffers(maxdb, bufs);

    // The proof line: where the map lands, named by what kl_glfb knows about
    // the FBO. Mirrors ANGLE's rule (identity first, then size + samples);
    // textures are single-sampled (Mojang allocates no MSAA).
    uint32_t col = klfb_color_of_fbo(fb);
    int32_t w = 0, h = 0;
    if (!col || !klfb_tex_info(col, NULL, &w, &h)) return;
    if (g_vrr_lines >= 48) return;
    int eye = klfb_stage_of_tex(col) >= 0;
    if (!g_vrr_rule_map) return;   // a retirement: nothing to name
    if (!eye && (w != g_vrr_rule_w || h != g_vrr_rule_h)) return;
    g_vrr_lines++;
    uint32_t dep = klfb_depth_of_fbo(fb);
    if (eye)
        fprintf(stderr, "  [glfb] vrr: fb%u colour tex %u %dx%d (an EYE texture) -> "
                        "FOVEATED by identity (map %p, rule #%u)\n",
                fb, col, w, h, g_vrr_rule_map, gen);
    else if (g_vrr_rule_min <= 1)
        fprintf(stderr, "  [glfb] vrr: fb%u colour tex %u %dx%d x1, depth tex %u -> "
                        "FOVEATED by the size rule (map %p, rule #%u, min samples "
                        "%u)\n", fb, col, w, h, dep, g_vrr_rule_map, gen,
                g_vrr_rule_min);
    else
        fprintf(stderr, "  [glfb] vrr: fb%u colour tex %u %dx%d x1, depth tex %u is "
                        "eye-sized but single-sampled -> NOT foveated (min samples "
                        "%u; KL_VRR_MIN_SAMPLES=1 would)\n",
                fb, col, w, h, dep, g_vrr_rule_min);
}

// The other half of the post rewrite's bargain: it assumes the texture it
// samples is the size of the target it draws into. Checked on the first post
// draws after each rule change, and on every 4096th one after that, so a
// mismatch (renderScaleFactor != 1, FSAA on) names itself instead of being a
// cropped picture.
static void klfb_vrr_post_check(int32_t fb) {
    if (!g_vrpost_nprog || !g_cur_prog || fb <= 0) return;
    int hit = 0;
    for (unsigned i = 0; i < g_vrpost_nprog; i++)
        if (g_vrpost_prog[i] == g_cur_prog) hit = 1;
    if (!hit) return;
    if (!a_glGetIntegerv) return;
    static void (*act)(uint32_t);
    if (!act) act = asym("glActiveTexture");
    if (!act) return;
    unsigned gen = __atomic_load_n(&g_vrr_gen, __ATOMIC_ACQUIRE);
    // Every post draw (two a frame): which texture it reads. Five GL queries,
    // no sync — cheap against a 17-megapixel full-screen pass.
    int32_t unit = 0, src = 0;
    a_glGetIntegerv(0x84E0 /* ACTIVE_TEXTURE */, &unit);
    act(0x84C0 /* TEXTURE0 */);
    a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &src);
    act((uint32_t)unit);
    uint32_t dst = klfb_color_of_fbo((uint32_t)fb);

    // THE ASSERTION (launch 5's question): the copy is texel-for-texel only if
    // the destination and the source are rasterized under the SAME live map.
    // What this file can observe, per draw:
    //   - the destination FBO went through klfb_vrr_touch under the live rule
    //     (a bind that bypassed our hook — e.g. a wrapper calling ANGLE
    //     directly — leaves an old stamp, and ANGLE an old cached desc);
    //   - the destination is an eye texture of the rule's size (identity);
    //   - the source is the rule's size with min samples 1 (size rule).
    // ANGLE's cached desc itself is not visible from GL; the resync that
    // refreshes it is host-proven (scratch harness, see 7de6988e).
    // Counted always; the first 16 named, then one line per 4096.
    static unsigned long post_draws, mismatches;
    static unsigned said_ok_gen = ~0u;
    static unsigned said_ok;
    post_draws++;
    if (gen && g_vrr_rule_map) {
        int32_t sw = 0, sh = 0, dw = 0, dh = 0;
        klfb_tex_info((uint32_t)src, NULL, &sw, &sh);
        if (dst) klfb_tex_info(dst, NULL, &dw, &dh);
        const char *why = NULL;
        if ((uint32_t)fb < KLFB_VRR_FBO_MAX && g_vrr_fbo_gen[fb] != gen)
            why = "the destination FBO did not re-sync under the live rule";
        else if (!dst || klfb_stage_of_tex(dst) < 0)
            why = "the destination is not an eye texture";
        else if (dw != g_vrr_rule_w || dh != g_vrr_rule_h)
            why = "the destination is not the rule's size (unfoveated)";
        else if (sw != g_vrr_rule_w || sh != g_vrr_rule_h || g_vrr_rule_min > 1)
            why = "the source is not foveated by the size rule";
        if (why) {
            mismatches++;
            if (mismatches <= 16 || !(mismatches & 4095))
                fprintf(stderr, "  [glfb] vrr post MISMATCH #%lu: program %u draws fb%d "
                                "(tex %u %dx%d, stamp #%u) from tex %d %dx%d under live "
                                "rule #%u %dx%d — %s (%lu post draws)\n",
                        mismatches, g_cur_prog, fb, dst, dw, dh,
                        (uint32_t)fb < KLFB_VRR_FBO_MAX ? g_vrr_fbo_gen[fb] : 0,
                        src, sw, sh, gen, g_vrr_rule_w, g_vrr_rule_h, why, post_draws);
        } else if (said_ok_gen != gen || said_ok < 6) {
            // Each destination of the 3x2 swapchain ring, once per rule.
            if (said_ok_gen != gen) { said_ok_gen = gen; said_ok = 0; }
            said_ok++;
            fprintf(stderr, "  [glfb] vrr post: fb%d (eye tex %u) <- tex %d, both "
                            "under live rule #%u (map %p) — consistent\n",
                    fb, dst, src, gen, g_vrr_rule_map);
        }
    }

    // The size assumption: first two post draws per rule, then every 4096th.
    static unsigned long draws;
    static unsigned checked_gen = ~0u;
    static int checks;
    draws++;
    if (gen == checked_gen && checks >= 2 && (draws & 4095)) return;
    if (gen != checked_gen) { checked_gen = gen; checks = 0; }
    checks++;
    int32_t sw = 0, sh = 0, dw = 0, dh = 0;
    klfb_tex_info((uint32_t)src, NULL, &sw, &sh);
    if (dst) klfb_tex_info(dst, NULL, &dw, &dh);
    int match = sw > 0 && sw == dw && sh == dh;
    if (match && checks > 2) return;     // the periodic re-check stays quiet
    fprintf(stderr, "  [glfb] vrr post: program %u draws fb%d (tex %u %dx%d) from "
                    "tex %d %dx%d — %s (rule #%u, map %p %dx%d; %lu mismatch(es) "
                    "so far)\n",
            g_cur_prog, fb, dst, dw, dh, src, sw, sh,
            match ? "sizes MATCH, the physical-UV read is texel-for-texel"
                  : "sizes DIFFER: the physical-UV rewrite reads the WRONG texels "
                    "(renderScaleFactor must be 1.0, useFsaa false; or "
                    "KL_VRR_MIN_SAMPLES=2)",
            gen, g_vrr_rule_map, g_vrr_rule_w, g_vrr_rule_h, mismatches);
}

static uint64_t g_lazy_elided, g_lazy_flushed, g_att_skipped;
static const char *g_lazy_unsafe;   // a binding-dependent entry the guest got unhooked
static int klfb_lazy_unbind_on(void) {
    static int on = -1;
    if (g_lazy_unsafe) return 0;
    if (on < 0) {
        on = kl_env_on("KL_GLFB_LAZY_UNBIND", 0);
        if (on)
            fprintf(stderr, "  [glfb] lazy unbind ON (KL_GLFB_LAZY_UNBIND=1): a bind-0 of the draw "
                            "framebuffer is held until something needs it, and dropped when the "
                            "guest rebinds the same one — back-to-back clears share a render pass "
                            "(0 is the A/B)\n");
    }
    return on;
}
// Who forwarded the held unbind (the report names the top few): the call that
// needed the binding, i.e. what stops the rebind from being dropped.
#define KLFB_LAZY_WHO 16
static struct { const char *who; uint64_t n; } g_lazy_who[KLFB_LAZY_WHO];
static void klfb_lazy_flush_from(const char *who) {
    if (!t_lazy_target) return;
    uint32_t t = t_lazy_target;
    t_lazy_target = 0;
    if (g_real_BindFramebuffer) g_real_BindFramebuffer(t, 0);
    g_lazy_flushed++;
    for (int i = 0; i < KLFB_LAZY_WHO; i++) {
        if (g_lazy_who[i].who == who) { g_lazy_who[i].n++; break; }
        if (!g_lazy_who[i].who) { g_lazy_who[i].who = who; g_lazy_who[i].n = 1; break; }
    }
}
// Once in a while: how many unbinds were dropped vs forwarded late.
// For callers that reach ANGLE without a kl_glfb thunk (kl_app.c's native
// gateway hands ltw some entry points raw, for the Darwin stack ABI): forward a
// held KL_GLFB_LAZY_UNBIND bind before the call. One TLS load when none is held.
void kl_glfb_lazy_flush_now(void) { KLFB_LAZY_FLUSH(); }

static void kl_glfb_lazy_report(void) {
    if (!klfb_lazy_unbind_on()) return;
    static uint64_t last_e, last_f;
    static int n;
    if (n++ % 600) return;
    static uint64_t last_a;
    fprintf(stderr, "  [glfb] lazy unbind: %llu rebinds dropped (render passes kept open), %llu "
                    "unbinds forwarded late, %llu identical re-attaches skipped, since the last line\n",
            (unsigned long long)(g_lazy_elided - last_e),
            (unsigned long long)(g_lazy_flushed - last_f),
            (unsigned long long)(g_att_skipped - last_a));
    last_a = g_att_skipped;
    last_e = g_lazy_elided; last_f = g_lazy_flushed;
    char buf[512]; int o = 0;
    for (int i = 0; i < KLFB_LAZY_WHO && g_lazy_who[i].who && o < (int)sizeof buf - 64; i++)
        o += snprintf(buf + o, sizeof buf - (size_t)o, " %s %llu", g_lazy_who[i].who,
                      (unsigned long long)g_lazy_who[i].n);
    if (o) fprintf(stderr, "  [glfb] lazy unbind: forwarded by (run totals):%s\n", buf);
}

// KL_PERF_PASSES (kl_perf.m): every Metal encoder ANGLE opens on the frame
// thread is timed; this names the guest framebuffer it belongs to ("fb34 t1080"
// = FBO 34, colour texture 1080), so the per-encoder table reads in the
// guest's own names. The label rides the thread until the next draw bind.
static int klfb_pass_labels_on(void) {
    static int on = -1;
    if (on < 0) on = kl_perf_on() && kl_env_on("KL_PERF_PASSES", 0);
    return on;
}
static uint32_t klfb_color_of_fbo(uint32_t fbo);
static void klfb_pass_label_fb(uint32_t fb) {
    if (!klfb_pass_labels_on()) return;
    static char names[512][32];
    char *n = names[fb % 512];
    uint32_t t = klfb_color_of_fbo(fb);
    if (t) snprintf(n, sizeof names[0], "fb%u t%u", fb, t);
    else   snprintf(n, sizeof names[0], "fb%u", fb);
    kl_perf_pass_label(n);
}

static void klfb_BindFramebuffer(uint32_t target, uint32_t fb) {
    if (klfb_trace_fbo())
        fprintf(stderr, "  [glfb] t%llu glBindFramebuffer(0x%x, %u) <- %s\n",
                (unsigned long long)klfb_tid(), target, fb,
                klfb_caller(__builtin_return_address(0)));
    {
        unsigned i = __atomic_fetch_add(&g_bindlog_n, 1, __ATOMIC_RELAXED) % KLFB_BINDLOG;
        g_bindlog[i].target = target;
        g_bindlog[i].fb     = fb;
        g_bindlog[i].tid    = klfb_tid();
        // klfb_caller's buffer is per-thread and reused, so it cannot be stored;
        // the raw return address can, and kl_addr_image resolves it at print time.
        g_bindlog[i].site   = (const char *)__builtin_return_address(0);
        g_bindlog[i].ctx    = kl_egl_current_context();
        g_bindlog_err[i]    = 0xffffffffu;   // "not asked"
        g_bindlog_slot      = i;
    }
    // KL_GLFB_LAZY_UNBIND. Minecraft 26.2's blaze3d binds a framebuffer, clears
    // it, and binds 0 — three times in a row on the eye's "3D Render" target
    // (black, 0, then the fog colour), then once more to draw. Each bind-away
    // ends ANGLE's render pass, so each clear is a whole pass of its own: a
    // full-size colour+depth store per clear, and a load at the next — per eye,
    // per frame (vivecraft/logs, [ftrace] s23-s31 / s48-s56). With the unbind
    // held back, the rebind of the same framebuffer is a no-op for ANGLE, the
    // clears land in ONE pass (later clears just replace the load action) and
    // the draws follow in it. Everything that reads the binding — every draw,
    // clear, blit, read, attachment, query, swap, and the framebuffer-dependent
    // calls hooked below for this alone — forwards the held bind first.
    int lazy_skip = 0;
    // READ binds pass straight through and leave a held unbind alone: ANGLE's
    // render pass follows the DRAW framebuffer only. (ltw splits every
    // GL_FRAMEBUFFER bind into READ then DRAW, so this is the common shape.)
    if (klfb_lazy_unbind_on() && (target == 0x8D40 || target == 0x8CA9)) {
        int both = target == 0x8D40;
        if (t_lazy_target) {
            if (fb == t_lazy_real) {
                t_lazy_target = 0;                      // back where ANGLE already is
                lazy_skip = 1;
                g_lazy_elided++;
            } else if (fb == 0) {
                lazy_skip = 1;                          // still unbound; still held
            } else {
                klfb_lazy_flush();
            }
        } else if (fb == 0 && g_draw_fb != 0) {
            t_lazy_target = 0x8CA9;                     // DRAW only; READ forwarded below
            t_lazy_real = g_draw_fb;
            lazy_skip = 1;
        }
        if (lazy_skip && both && g_real_BindFramebuffer)
            g_real_BindFramebuffer(0x8CA8 /* READ */, fb);
    }
    if (klfb_is_read_target(target)) g_read_fb = fb;
    if (klfb_is_draw_target(target)) {
        klfb_ftrace_bind((int32_t)g_draw_fb, (int32_t)fb);   // r26, no-op when off
        g_draw_fb = fb;
        klfb_pass_label_fb(fb);   // KL_PERF_PASSES: name the guest FBO in the table
        // Which stage this frame is going into. Sticky: a guest that binds an
        // eye FBO and then bounces through others (shadow maps, post) has still
        // last committed to this stage, and the next eye bind is what moves it.
        klfb_note_render_stage(klfb_stage_of_fbo(fb));
    }
    // r32: one of the two moments the supersample factor in force can change.
    // Inert (both factors 1, one env compare) unless KL_GLFB_RT_SCALE is set.
    klfb_rt_refresh(g_draw_fb, g_read_fb);
    if (lazy_skip) return;          // ANGLE's binding is unchanged: nothing below applies
    if (g_real_BindFramebuffer) g_real_BindFramebuffer(target, fb);
    // Did GL take the name? Only asked when ERRSCAN is already draining the
    // error queue every call — otherwise this would eat exactly the errors the
    // guest's own glGetError is looking for.
    if (glfb_errscan() && a_glGetError) {
        unsigned i = g_bindlog_slot % KLFB_BINDLOG;
        g_bindlog_err[i] = a_glGetError();
    }
    // The draw target moved, so the depth attachment the injected sampler is
    // supposed to read moved with it. No-op unless a rewritten program is
    // current — see klfb_depth_fetch_apply.
    if (klfb_is_draw_target(target)) klfb_depth_fetch_apply();
    // KL_VRR: an FBO ANGLE synced before the size rule changed re-reads it
    // (once per FBO per change; a counter compare otherwise).
    if (klfb_is_draw_target(target)) klfb_vrr_touch(fb);
}
// ---------------------------------------------------------------------------
// Depth-attachment census (r24). The question it answers: does any texture the
// guest renders DEPTH into ever come back as a SAMPLER — and if so, at what
// size relative to the colour target it was rendered with?
//
// That pairing is the whole point. A soft-particle / DepthFade material samples
// scene depth at the fragment's own screen UV, and the UV is derived from a
// buffer size the guest believes in. We present DoubleWide, so if the depth
// texture the guest resolves is per-EYE while the UV math is DoubleWide (or the
// reverse), every sample lands at the wrong screen X — and a screen-FIXED
// artefact over a world-locked object is exactly what that produces.
#define KLFB_MAX_DEPTHTEX 64
static struct { uint32_t tex; uint32_t color; unsigned attached, sampled; }
    g_depthtex[KLFB_MAX_DEPTHTEX];
static unsigned g_ndepthtex;

static void klfb_depth_attach_filters(uint32_t tex);

// "The world is on screen" as a signal a capture can be armed on, without a
// frame count and without anyone driving the guest. A framebuffer with a depth
// attachment and NO colour attachment is a shadow pass, and a shadow pass only
// exists once a level is loaded and being drawn -- the front end, the videos
// and the menus never build one. See klfb_capture_due().
static int g_shadow_pass_seen;

static void klfb_note_depth_attach(uint32_t tex, uint32_t color) {
    if (!tex) return;
    if (!color) __atomic_store_n(&g_shadow_pass_seen, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_compile_lock);
    for (unsigned i = 0; i < g_ndepthtex; i++)
        if (g_depthtex[i].tex == tex) {
            g_depthtex[i].attached++;
            if (color) g_depthtex[i].color = color;
            pthread_mutex_unlock(&g_compile_lock);
            return;
        }
    if (g_ndepthtex < KLFB_MAX_DEPTHTEX) {
        g_depthtex[g_ndepthtex].tex = tex;
        g_depthtex[g_ndepthtex].color = color;
        g_depthtex[g_ndepthtex].attached = 1;
        g_ndepthtex++;
    }
    pthread_mutex_unlock(&g_compile_lock);
}

// Called from glBindTexture: a depth attachment bound to a texture unit is a
// depth attachment being READ, which is the case the census exists to catch.
static void klfb_note_maybe_depth_sample(uint32_t tex) {
    if (!tex || !g_ndepthtex) return;
    for (unsigned i = 0; i < g_ndepthtex; i++)
        if (g_depthtex[i].tex == tex) {
            __atomic_fetch_add(&g_depthtex[i].sampled, 1, __ATOMIC_RELAXED);
            return;
        }
}

static int klfb_is_depth_attachment(uint32_t att) {
    return att == 0x8D00     /* GL_DEPTH_ATTACHMENT */
        || att == 0x821A     /* GL_DEPTH_STENCIL_ATTACHMENT */
        || att == 0x8D20;    /* GL_STENCIL_ATTACHMENT */
}

// ---------------------------------------------------------------------------
// ...and the half that gives klfb_rewrite_depth_fetch something to read (r25).
//
// The rewritten shader samples `kl_SceneDepth`, a uniform the guest has never
// heard of and will never bind. We bind it: the depth texture currently
// attached to the DRAW framebuffer, on a texture unit the guest does not use.
//
// Sampling a texture that is also the current depth ATTACHMENT is a feedback
// loop, and it is the reason this is safe rather than lucky. GLES leaves it
// undefined only where the attachment can be WRITTEN; ANGLE encodes exactly
// that rule (Framebuffer::formsRenderingFeedbackLoopWith takes
// AllowedFeedbackLoop::ReadOnlyDepthStencil and returns "loop" for a depth
// attachment only when glState.isDepthWriteEnabled()), and it only enforces it
// for WebGL and hardened contexts in the first place. Translucency — every
// draw this rewrite touches — runs with depth writes off, which is the
// read-only case both the spec and Metal allow.
//
// Two places have to re-bind, and between them they cover every draw without
// touching a draw entry point: glUseProgram (a rewritten program becomes
// current) and glBindFramebuffer (the depth attachment under a already-current
// rewritten program changes).
#define KLFB_FBO_DEPTH 48
static struct { uint32_t fbo, tex; } g_fbo_depth[KLFB_FBO_DEPTH];
static unsigned g_nfbo_depth;

static void klfb_note_fbo_depth(uint32_t fbo, uint32_t tex) {
    if (!fbo) return;
    for (unsigned i = 0; i < g_nfbo_depth; i++)
        if (g_fbo_depth[i].fbo == fbo) { g_fbo_depth[i].tex = tex; return; }
    if (g_nfbo_depth < KLFB_FBO_DEPTH) {
        g_fbo_depth[g_nfbo_depth].fbo = fbo;
        g_fbo_depth[g_nfbo_depth].tex = tex;
        g_nfbo_depth++;
    }
}
static uint32_t klfb_depth_of_fbo(uint32_t fbo) {
    for (unsigned i = 0; i < g_nfbo_depth; i++)
        if (g_fbo_depth[i].fbo == fbo) return g_fbo_depth[i].tex;
    return 0;
}

// The unit. Taken from the DRIVER's own GL_MAX_TEXTURE_IMAGE_UNITS and not from
// the number we describe to the guest, because the point is to be above
// anything the guest can select: ANGLE on Metal reports 64 per stage and UE4
// mobile never reaches past sixteen.
#define KLFB_GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define KLFB_GL_TEXTURE0                0x84C0
static int      g_df_unit = -1;
static uint32_t g_df_active_unit = KLFB_GL_TEXTURE0;   // the guest's selection
static uint32_t g_df_bound_tex;                        // what is on g_df_unit
static unsigned g_df_applied, g_df_nodepth, g_df_progs;
#define KLFB_DF_CACHE 64
static struct { uint32_t prog; int32_t loc; } g_df_cache[KLFB_DF_CACHE];
static unsigned g_df_ncache;

static int32_t klfb_df_loc(uint32_t prog) {
    for (unsigned i = 0; i < g_df_ncache; i++)
        if (g_df_cache[i].prog == prog) return g_df_cache[i].loc;
    int32_t (*getloc)(uint32_t, const char *) = asym("glGetUniformLocation");
    int32_t loc = getloc ? getloc(prog, KLFB_DEPTH_UNIFORM) : -1;
    if (g_df_ncache < KLFB_DF_CACHE) {
        g_df_cache[g_df_ncache].prog = prog;
        g_df_cache[g_df_ncache].loc  = loc;
        g_df_ncache++;
    }
    if (loc >= 0 && g_df_progs++ == 0)
        fprintf(stderr, "  [glfb] scene depth: program %u carries %s at location "
                        "%d — it is bound to texture unit %d from the draw "
                        "framebuffer's own depth attachment\n",
                prog, KLFB_DEPTH_UNIFORM, loc, g_df_unit);
    return loc;
}

// A depth texture the guest only ever RENDERED into carries the default filter
// state, and NEAREST_MIPMAP_LINEAR with no mip chain is sampler-INCOMPLETE —
// a texelFetch on an incomplete texture reads zero, which is the exact bug this
// whole rewrite exists to remove, silently. Fix it once per texture. Safe to
// change: the census says the guest never samples these itself (`bound as
// sampler 0 x`), and filter state does not affect a texture's use as an
// attachment.
static void klfb_df_prep(uint32_t tex) {
    static uint32_t done[16];
    static unsigned ndone;
    for (unsigned i = 0; i < ndone; i++) if (done[i] == tex) return;
    if (ndone < 16) done[ndone++] = tex;
    void (*tp)(uint32_t, uint32_t, int32_t) = asym("glTexParameteri");
    if (!tp) return;
    tp(0x0DE1 /* TEXTURE_2D */, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */);
    tp(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2600);
    tp(0x0DE1, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
    tp(0x0DE1, 0x2803 /* WRAP_T */, 0x812F);
    tp(0x0DE1, 0x884C /* TEXTURE_COMPARE_MODE */, 0 /* NONE */);
    tp(0x0DE1, 0x90EA /* DEPTH_STENCIL_TEXTURE_MODE */, 0x1902 /* DEPTH_COMPONENT */);
}

static void klfb_depth_fetch_apply(void) {
    if (klfb_depth_fetch_mode() != 1 || !g_cur_prog) return;
    if (g_df_unit < 0) {
        int32_t n = 0;
        if (a_glGetIntegerv) a_glGetIntegerv(KLFB_GL_MAX_TEXTURE_IMAGE_UNITS, &n);
        g_df_unit = n > 1 ? n - 1 : 15;
    }
    int32_t loc = klfb_df_loc(g_cur_prog);
    if (loc < 0) return;
    uint32_t tex = klfb_depth_of_fbo(g_draw_fb);
    if (!tex) {
        // A renderbuffer depth attachment, or one attached before we were
        // watching: nothing to sample. The sampler then reads ANGLE's
        // incomplete-texture black, i.e. exactly the pre-r25 constant — the
        // failure degrades to the bug it replaces rather than to a crash.
        if (g_df_nodepth++ == 0)
            fprintf(stderr, "  [glfb] scene depth: fb %u has no depth TEXTURE to "
                            "sample (renderbuffer, or never attached) — those draws "
                            "keep the old constant\n", g_draw_fb);
        return;
    }
    void (*u1i)(int32_t, int32_t)        = asym("glUniform1i");
    void (*act)(uint32_t)                = asym("glActiveTexture");
    void (*bindt)(uint32_t, uint32_t)    = asym("glBindTexture");
    if (!u1i || !act || !bindt) return;
    u1i(loc, g_df_unit);
    if (g_df_bound_tex != tex) {
        act((uint32_t)(KLFB_GL_TEXTURE0 + g_df_unit));
        bindt(0x0DE1 /* TEXTURE_2D */, tex);
        klfb_df_prep(tex);
        act(g_df_active_unit);            // the guest's selection, restored
        g_df_bound_tex = tex;
    }
    g_df_applied++;
}

void kl_glfb_depth_fetch_census(FILE *f) {
    if (!g_df_rewritten && klfb_depth_fetch_mode() == 1) return;
    fprintf(f, "  [glfb] scene depth (%s): %u shader(s) rewritten, %u program(s) "
               "carry the sampler, %u bind(s) applied, %u with no depth texture; "
               "unit %d, texture %u\n",
            klfb_depth_fetch_mode() == 1 ? "sampled" :
            klfb_depth_fetch_mode() == 2 ? "forced NEAR" : "left as UE4 cooked it",
            g_df_rewritten, g_df_progs, g_df_applied, g_df_nodepth,
            g_df_unit, g_df_bound_tex);
}

// ------------------------------------------------- the SHAR radar-HUD trace
//
// KL_SHAR_RADARTRACE=1. SHAR captures its minimap by drawing the HUD's map
// group into an offscreen RGBA texture (RADAR_TEXTURE 1440x1080 + a
// DEPTH_COMPONENT16 renderbuffer), then presents that texture as a quad on the
// hand and on the dashboard. On device the quad is an opaque BLACK SQUARE, and
// the guest's own readback says why it cannot tell us more: every pixel it
// finds is RGB 0 / alpha 255.
//
// So this reads the pass from underneath: for each draw that lands in the radar
// framebuffer, the program, the texture actually bound on unit 0 with its
// level-0 size/format and filters, and the blend/mask/depth/cull/viewport state
// that decides what those texels become. Sampling an INCOMPLETE texture in ES
// returns (0,0,0,1), which through SHAR's `texture2D(tex,tc)*cpri + csec` with a
// white vertex colour is exactly opaque black — so "what is on unit 0, and is it
// complete" is the question, and the trace answers it per draw.
//
// The framebuffer is recognised by its colour attachment: a 2D texture of
// exactly KL_SHAR_RADAR_W x KL_SHAR_RADAR_H (1440x1080, the guest's constants).
// Nothing else in this guest allocates that size. The mission-HUD captures are
// 720x540 and are deliberately NOT traced.
//
// Volume control, because the pass runs every frame for the life of the game:
// KL_SHAR_RADARTRACE_PASSES (default 3) capture passes are traced, and at most
// KL_SHAR_RADARTRACE_DRAWS (default 40) draws in each, and at most
// KL_SHAR_RADARTRACE_DUMPS (default 12) PNGs. A pass boundary is the
// guest's own glReadPixels on the framebuffer — which is also where the texture
// is written out as a PNG (KL_SHAR_RADARTRACE_DIR, default off) straight from
// the buffer the guest already asked for, so the dump costs no extra GL.
//
// Read-only and default off.
static int klfb_write_png(const char *path, const uint8_t *px,
                          int32_t w, int32_t h);
static int32_t  g_radar_fb = -1;
static uint32_t g_radar_tex;
static int32_t  g_radar_w = 1440, g_radar_h = 1080;
static unsigned g_radar_pass, g_radar_pass_draws, g_radar_dumps;

// Round 37. The pass counter used to advance on the guest's own glReadPixels,
// which happens ONCE — `textureLogged` latches on the first scan that finds a
// pixel and the guest never reads back again. So passes 2 and 3 of launch #35
// were not radar passes at all: they were mission-HUD readbacks that landed in
// the radar framebuffer because of the very bug fixed below. The pass boundary
// is now EndRadarCapture itself (kl_glfb_shar_radar_endpass, driven from the
// interposer in kl_sdl.c), which is one pass per eye per frame for the life of
// the run — so a STEADY-STATE pass can be traced, which is the whole point.
static int g_radar_endpass_hooked;

// Attribute 0 is `position` in every SHAR GLES program, and the Scrooby sprite
// path submits it from a CLIENT array (pglContext::EndPrims). The pointer is
// recorded here at glVertexAttribPointer time so the trace can print the first
// four positions of each draw — which is what says WHERE a draw landed, the one
// question the state dump cannot answer.
static const void *g_radar_a0_ptr;
static int32_t     g_radar_a0_size = 0, g_radar_a0_stride = 0;
static uint32_t    g_radar_a0_type = 0, g_radar_a0_buf = 0;

// ...and WHICH vertices this draw owns. Without them the position dump is an
// unbounded read: it does not know how many vertices the call submits, and a
// two- or three-vertex draw into the same framebuffer (DrawRadarPlane and
// ApplyIrisBlackout both land there whenever KL_SHAR_RADARFIX is off) would be
// read four vertices deep, off the end of a client array that never had them.
// The draw entry points fill these in before klfb_note_draw runs; `verts` of 0
// means "this draw's extent is not known", and then nothing is read.
static int32_t     g_radar_draw_first, g_radar_draw_verts;
static const void *g_radar_draw_idx;
static uint32_t    g_radar_draw_idxtype, g_radar_elembuf;

static int klfb_radartrace(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_SHAR_RADARTRACE", 0);
    return on;
}
static void klfb_radar_note_color(int32_t fb, uint32_t tex) {
    if (!klfb_radartrace() || fb <= 0 || !tex) return;
    static int32_t rw = 0, rh = 0;
    if (!rw) { rw = kl_env_int("KL_SHAR_RADAR_W", 1440);
               rh = kl_env_int("KL_SHAR_RADAR_H", 1080); }
    int32_t tw = 0, th = 0;
    uint32_t fmt = 0;
    if (!klfb_tex_info(tex, &fmt, &tw, &th)) return;
    if (tw != rw || th != rh) return;
    if (g_radar_fb == fb && g_radar_tex == tex) return;
    g_radar_fb  = fb;
    g_radar_tex = tex;
    g_radar_w   = tw;
    g_radar_h   = th;
    fprintf(stderr, "  [radar] framebuffer %d is the radar capture target "
                    "(colour0 tex %u %dx%d fmt=0x%x)\n", fb, tex, tw, th, fmt);
}

// ---- round 37: is THIS pass in the traced window ---------------------------
//
// KL_SHAR_RADARTRACE_FROM (default 0) is the first pass traced and
// KL_SHAR_RADARTRACE_PASSES (default 3) is how many from there. Launch #35 only
// ever saw pass 1 — the FIRST capture, whose 31 sprite draws happen while
// Pure3D is still preparing the map drawable, so it is guaranteed to be the
// least representative frame of the run. `FROM=200 PASSES=2` traces a
// steady-state pass instead, and that is the pass that names the black square.
#define KLFB_RADAR_WINDOWS 4
static int klfb_radar_pass_traced(void) {
    static int from[KLFB_RADAR_WINDOWS], nfrom = -1, passes;
    if (nfrom < 0) {
        nfrom = 0;
        passes = kl_env_int("KL_SHAR_RADARTRACE_PASSES", 3);
        const char *e = kl_env_str("KL_SHAR_RADARTRACE_FROM", "0");
        for (const char *q = e; *q && nfrom < KLFB_RADAR_WINDOWS; ) {
            from[nfrom++] = (int)strtol(q, (char **)&q, 10);
            while (*q == ',' || *q == ' ') q++;
        }
        if (!nfrom) { from[0] = 0; nfrom = 1; }
        fprintf(stderr, "  [radar] tracing %d pass(es) from each of:", passes);
        for (int i = 0; i < nfrom; i++) fprintf(stderr, " %d", from[i]);
        fprintf(stderr, "  (KL_SHAR_RADARTRACE_FROM / _PASSES)\n");
    }
    for (int i = 0; i < nfrom; i++)
        if ((int)g_radar_pass >= from[i] && (int)g_radar_pass < from[i] + passes)
            return 1;
    return 0;
}

// The scratch the coverage scan and the PNG dump share. One allocation for the
// life of the run, sized from the framebuffer we recognised.
static uint8_t *klfb_radar_scratch(size_t *cap) {
    static uint8_t *buf; static size_t sz;
    size_t want = (size_t)g_radar_w * (size_t)g_radar_h * 4u;
    if (!want) return NULL;
    if (want > sz) { uint8_t *n = realloc(buf, want);
                     if (!n) return NULL; buf = n; sz = want; }
    if (cap) *cap = sz;
    return buf;
}

// What is actually IN the radar texture right now: how many pixels are non-zero
// (which is the guest's own test — see EndRadarCapture), their bounds, and the
// brightest colour and alpha found. maxRGB is the field that matters: launch
// #35's whole capture came back maxRGB=0 maxAlpha=255, i.e. pure OPAQUE BLACK,
// and opaque black is what the guest counts as "the minimap is here".
typedef struct { unsigned nonzero; int x0, y0, x1, y1;
                 unsigned maxrgb, maxalpha; } klfb_radar_cover;
static int klfb_radar_scan(klfb_radar_cover *c) {
    if (!a_glReadPixels || g_radar_w <= 0 || g_radar_h <= 0) return 0;
    uint8_t *px = klfb_radar_scratch(NULL);
    if (!px) return 0;
    if (a_glGetError) (void)a_glGetError();
    a_glReadPixels(0, 0, g_radar_w, g_radar_h, 0x1908 /* RGBA */,
                   0x1401 /* UNSIGNED_BYTE */, px);
    if (a_glGetError && a_glGetError() != 0) return 0;
    memset(c, 0, sizeof *c);
    c->x0 = g_radar_w; c->y0 = g_radar_h; c->x1 = -1; c->y1 = -1;
    for (int y = 0; y < g_radar_h; y++)
        for (int x = 0; x < g_radar_w; x++) {
            const uint8_t *q = px + ((size_t)y * g_radar_w + x) * 4u;
            unsigned rgb = q[0] > q[1] ? q[0] : q[1];
            if (q[2] > rgb) rgb = q[2];
            if (rgb > c->maxrgb)   c->maxrgb = rgb;
            if (q[3] > c->maxalpha) c->maxalpha = q[3];
            if (q[0] || q[1] || q[2] || q[3]) {
                c->nonzero++;
                if (x < c->x0) c->x0 = x;
                if (x > c->x1) c->x1 = x;
                if (y < c->y0) c->y0 = y;
                if (y > c->y1) c->y1 = y;
            }
        }
    return 1;
}

// The PNG, from the scratch the scan just filled. Bottom-up like every
// glReadPixels, so the image is vertically mirrored — said here because it is
// cheap to say and expensive to misread.
static void klfb_radar_dump(const char *tag, unsigned draw) {
    const char *dir = kl_env_str("KL_SHAR_RADARTRACE_DIR", NULL);
    if (!dir) return;
    if ((int)g_radar_dumps >= kl_env_int("KL_SHAR_RADARTRACE_DUMPS", 12)) return;
    uint8_t *px = klfb_radar_scratch(NULL);
    if (!px) return;
    char path[600];
    snprintf(path, sizeof path, "%s/radar_p%03u_%s%03u.png",
             dir, g_radar_pass, tag, draw);
    if (klfb_write_png(path, px, g_radar_w, g_radar_h)) {
        g_radar_dumps++;
        fprintf(stderr, "  [radar]   -> %s (%dx%d, bottom-up so the image is "
                        "flipped)\n", path, g_radar_w, g_radar_h);
    }
}

// The two 4x4 matrices SHAR's sprite program is driven by. They are printed
// only when they CHANGE, because 31 draws share three or four of them and the
// interesting fact is which draw switched.
static void klfb_radar_note_matrices(int32_t prog) {
    static int32_t (*r_loc)(uint32_t, const char *);
    static void (*r_getf)(uint32_t, int32_t, float *);
    static int looked;
    if (!looked) { looked = 1;
        r_loc  = asym("glGetUniformLocation");
        r_getf = asym("glGetUniformfv"); }
    if (!r_loc || !r_getf || prog <= 0) return;
    static const char *names[2] = { "projection", "modelview" };
    static float last[2][16];
    static int32_t last_prog = -1;
    for (int m = 0; m < 2; m++) {
        int32_t loc = r_loc((uint32_t)prog, names[m]);
        if (loc < 0) continue;
        float v[16];
        memset(v, 0, sizeof v);
        r_getf((uint32_t)prog, loc, v);
        if (prog == last_prog && !memcmp(v, last[m], sizeof v)) continue;
        memcpy(last[m], v, sizeof v);
        fprintf(stderr, "  [radar]   %-10s [%.4f %.4f %.4f %.4f | "
                        "%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | "
                        "%.4f %.4f %.4f %.4f]\n", names[m],
                (double)v[0],  (double)v[1],  (double)v[2],  (double)v[3],
                (double)v[4],  (double)v[5],  (double)v[6],  (double)v[7],
                (double)v[8],  (double)v[9],  (double)v[10], (double)v[11],
                (double)v[12], (double)v[13], (double)v[14], (double)v[15]);
    }
    last_prog = prog;
}

// The first four positions the draw submits, straight out of the client array.
// A sprite is four vertices, so for the Scrooby path this IS the quad — and a
// quad is the only thing that says whether a draw covers the top and bottom
// bands of the texture or the small rectangle the minimap is supposed to be.
static void klfb_radar_note_positions(void) {
    if (g_radar_a0_buf) {
        fprintf(stderr, "  [radar]   pos: from VBO %u (not a client array; "
                        "size=%d type=0x%x stride=%d off=%p)\n",
                g_radar_a0_buf, g_radar_a0_size, (unsigned)g_radar_a0_type,
                g_radar_a0_stride, g_radar_a0_ptr);
        return;
    }
    if (!g_radar_a0_ptr || g_radar_a0_type != 0x1406 /* FLOAT */ ||
        g_radar_a0_size < 2 || g_radar_a0_size > 4) {
        fprintf(stderr, "  [radar]   pos: unreadable (ptr=%p size=%d type=0x%x)\n",
                g_radar_a0_ptr, g_radar_a0_size, (unsigned)g_radar_a0_type);
        return;
    }
    if (g_radar_draw_verts <= 0) {
        fprintf(stderr, "  [radar]   pos: draw extent unknown — not read\n");
        return;
    }
    // Client-side indices are read the same way and bounded the same way: the
    // index array says which vertices the draw actually touches, so following
    // it is what makes the position read legal rather than a guess.
    if (g_radar_draw_idx && g_radar_elembuf) {
        fprintf(stderr, "  [radar]   pos: indexed from EBO %u — not read\n",
                g_radar_elembuf);
        return;
    }
    int n = g_radar_draw_verts < 4 ? g_radar_draw_verts : 4;
    int stride = g_radar_a0_stride ? g_radar_a0_stride
                                   : (int)(g_radar_a0_size * (int)sizeof(float));
    char line[320]; size_t o = 0;
    o += (size_t)snprintf(line + o, sizeof line - o, "  [radar]   pos:");
    for (int v = 0; v < n && o < sizeof line - 64; v++) {
        int32_t vi = g_radar_draw_first + v;
        if (g_radar_draw_idx) {
            const void *ix = g_radar_draw_idx;
            switch (g_radar_draw_idxtype) {
                case 0x1401: vi = ((const uint8_t  *)ix)[v]; break; /* UBYTE  */
                case 0x1403: vi = ((const uint16_t *)ix)[v]; break; /* USHORT */
                case 0x1405: vi = (int32_t)((const uint32_t *)ix)[v]; break;
                default:
                    fprintf(stderr, "  [radar]   pos: index type 0x%x — not "
                                    "read\n", (unsigned)g_radar_draw_idxtype);
                    return;
            }
        }
        const float *f = (const float *)((const uint8_t *)g_radar_a0_ptr +
                                         (size_t)vi * (size_t)stride);
        o += (size_t)snprintf(line + o, sizeof line - o, " (");
        for (int k = 0; k < g_radar_a0_size && o < sizeof line - 24; k++)
            o += (size_t)snprintf(line + o, sizeof line - o, "%s%.3f",
                                  k ? "," : "", (double)f[k]);
        o += (size_t)snprintf(line + o, sizeof line - o, ")");
    }
    snprintf(line + o, sizeof line - o, "  [%d of %d vert%s, size=%d stride=%d]",
             n, g_radar_draw_verts, g_radar_draw_verts == 1 ? "" : "s",
             g_radar_a0_size, stride);
    fprintf(stderr, "%s\n", line);
}

// One line per draw into the radar framebuffer.
static void klfb_radar_note_draw(int32_t fb) {
    if (!klfb_radartrace() || fb < 0 || fb != g_radar_fb) return;
    static int maxdraws = -1;
    if (maxdraws < 0) maxdraws = kl_env_int("KL_SHAR_RADARTRACE_DRAWS", 40);
    if (!klfb_radar_pass_traced()) { g_radar_pass_draws++; return; }
    if ((int)g_radar_pass_draws >= maxdraws) { g_radar_pass_draws++; return; }
    unsigned n = g_radar_pass_draws++;

    static void (*a_GetBooleanv)(uint32_t, uint8_t *);
    static uint8_t (*a_IsEnabled)(uint32_t);
    static void (*a_GetTexParameteriv)(uint32_t, uint32_t, int32_t *);
    static int looked;
    if (!looked) { looked = 1;
        a_GetBooleanv       = asym("glGetBooleanv");
        a_IsEnabled         = asym("glIsEnabled");
        a_GetTexParameteriv = asym("glGetTexParameteriv"); }
    if (!a_glGetIntegerv || !a_GetBooleanv || !a_IsEnabled) return;

    int32_t prog = -1, unit = 0, tex0 = -1, vp[4] = {0}, box[4] = {0};
    int32_t sfa = 0, dfa = 0, srgb = 0, drgb = 0, dfunc = 0;
    a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */,      &prog);
    a_glGetIntegerv(0x84E0 /* ACTIVE_TEXTURE */,       &unit);
    a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */,   &tex0);
    a_glGetIntegerv(0x0BA2 /* VIEWPORT */,              vp);
    a_glGetIntegerv(0x0C10 /* SCISSOR_BOX */,           box);
    a_glGetIntegerv(0x80C9 /* BLEND_SRC_RGB */,        &srgb);
    a_glGetIntegerv(0x80C8 /* BLEND_DST_RGB */,        &drgb);
    a_glGetIntegerv(0x80CB /* BLEND_SRC_ALPHA */,      &sfa);
    a_glGetIntegerv(0x80CA /* BLEND_DST_ALPHA */,      &dfa);
    a_glGetIntegerv(0x0B74 /* DEPTH_FUNC */,           &dfunc);
    uint8_t cm[4] = {1,1,1,1}, dm = 1;
    a_GetBooleanv(0x0C23 /* COLOR_WRITEMASK */, cm);
    a_GetBooleanv(0x0B72 /* DEPTH_WRITEMASK */, &dm);
    int bl = a_IsEnabled(0x0BE2) ? 1 : 0, dt = a_IsEnabled(0x0B71) ? 1 : 0;
    int cu = a_IsEnabled(0x0B44) ? 1 : 0, sc = a_IsEnabled(0x0C11) ? 1 : 0;

    // What is actually on unit 0. `complete` is the whole point: an ES texture
    // with no level 0 — or a mip filter and no chain — samples as (0,0,0,1).
    char tbuf[160];
    if (tex0 > 0) {
        uint32_t tfmt = 0; int32_t tw = 0, th = 0, mn = 0, mg = 0;
        int known = klfb_tex_info((uint32_t)tex0, &tfmt, &tw, &th);
        if (a_GetTexParameteriv) {
            a_GetTexParameteriv(0x0DE1 /* TEXTURE_2D */, 0x2801 /* MIN_FILTER */, &mn);
            a_GetTexParameteriv(0x0DE1, 0x2800 /* MAG_FILTER */, &mg);
        }
        int mipfilter = (mn == 0x2700 || mn == 0x2701 || mn == 0x2702 || mn == 0x2703);
        snprintf(tbuf, sizeof tbuf,
                 "tex0=%d %dx%d fmt=0x%x min=0x%x mag=0x%x%s%s", tex0,
                 known ? tw : -1, known ? th : -1, known ? tfmt : 0u,
                 (unsigned)mn, (unsigned)mg,
                 (known && (tw <= 0 || th <= 0))
                   ? "  <<< NO LEVEL 0 — samples (0,0,0,1), i.e. OPAQUE BLACK" : "",
                 mipfilter ? "  <<< MIP FILTER (this guest builds no chains)" : "");
    } else {
        snprintf(tbuf, sizeof tbuf,
                 "tex0=0  <<< NOTHING BOUND — samples (0,0,0,1), i.e. OPAQUE BLACK");
    }
    fprintf(stderr, "  [radar] pass %u draw %u: program %d unit=0x%x %s | "
                    "blend %d %s (rgb 0x%x/0x%x alpha 0x%x/0x%x) mask %d%d%d%d "
                    "depth %d func 0x%x write %d cull %d scissor %d %d,%d %dx%d "
                    "vp %d,%d %dx%d err 0x%x\n",
            g_radar_pass, n, prog, (unsigned)unit, tbuf,
            bl, bl ? "on" : "OFF", (unsigned)srgb, (unsigned)drgb,
            (unsigned)sfa, (unsigned)dfa,
            cm[0]?1:0, cm[1]?1:0, cm[2]?1:0, cm[3]?1:0,
            dt, (unsigned)dfunc, dm?1:0, cu, sc, box[0], box[1], box[2], box[3],
            vp[0], vp[1], vp[2], vp[3],
            a_glGetError ? (unsigned)a_glGetError() : 0u);

    // Round 37. The state above was IDENTICAL on the host and the device in
    // launch #35 — same programs, same complete BGRA textures, same blend, same
    // viewport — and the host drew the map while the device drew opaque black.
    // So the difference is not state, it is WHERE the geometry landed, and
    // these three say where: the matrices the vertices go through, the vertices
    // themselves, and what the texture actually holds after the draw.
    klfb_radar_note_matrices(prog);
    klfb_radar_note_positions();

    // The coverage scan is a full glReadPixels per traced draw. That is
    // deliberate and it is why the traced window is small: it turns "31 draws
    // and the result is a black band" into "draw K is the one that painted it",
    // which no amount of state logging can. KL_SHAR_RADARTRACE_COVER=0 removes
    // it; a PNG is dumped whenever one draw adds more than a fiftieth of the
    // texture, so the dumps land on the culprit without being asked to.
    // KL_SHAR_RADARTRACE_COVER is a COUNT of draws per pass, not a flag, and the
    // default is small on purpose: each scan is a full 1440x1080 glReadPixels,
    // which is a pipeline flush, and the visionOS compositor kills a client that
    // misses a frame for 2.0 s (see the "no frame for 2.0s" class). Eight is
    // enough to name a culprit among the opening draws and cheap enough not to
    // cost a frame; the endpass scan below always runs and is one per pass.
    static int cover = -1;
    if (cover < 0) cover = kl_env_int("KL_SHAR_RADARTRACE_COVER", 8);
    if ((int)n < cover) {
        static unsigned prev;
        if (n == 0) prev = 0;
        klfb_radar_cover c;
        if (klfb_radar_scan(&c)) {
            long delta = (long)c.nonzero - (long)prev;
            fprintf(stderr, "  [radar]   cover %+ld -> %u px  bounds %d,%d..%d,%d"
                            "  maxRGB=%u maxAlpha=%u%s\n",
                    delta, c.nonzero, c.x0, c.y0, c.x1, c.y1,
                    c.maxrgb, c.maxalpha,
                    (delta > 0 && c.maxrgb == 0)
                      ? "   <<< THIS DRAW PAINTED OPAQUE BLACK (no colour at "
                        "all), which is what the guest latches its crop from"
                      : "");
            if (delta > (long)((size_t)g_radar_w * (size_t)g_radar_h / 50u))
                klfb_radar_dump("d", n);
            prev = c.nonzero;
        }
    }
}

// The guest's own readback closes the pass. It has already asked for the whole
// 1440x1080 in RGBA/UNSIGNED_BYTE, so the PNG is free — and it is the picture
// the black square is made of.
static void klfb_radar_note_read(int32_t x, int32_t y, int32_t w, int32_t h,
                                 uint32_t format, uint32_t type,
                                 const void *pixels) {
    if (!klfb_radartrace() || g_radar_fb < 0) return;
    // Round 37: EndRadarCapture is the pass boundary now (below). Leaving this
    // path advancing as well is what mislabelled two mission-HUD readbacks as
    // radar passes 2 and 3 in launch #35.
    if (g_radar_endpass_hooked) return;
    int32_t rfb = -1;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &rfb);
    if (rfb != g_radar_fb) return;
    const char *dir = kl_env_str("KL_SHAR_RADARTRACE_DIR", NULL);
    if (dir && pixels && x == 0 && y == 0 && w > 0 && h > 0 &&
        format == 0x1908 /* RGBA */ && type == 0x1401 /* UNSIGNED_BYTE */ &&
        (int)g_radar_dumps < kl_env_int("KL_SHAR_RADARTRACE_DUMPS", 12)) {
        char path[600];
        snprintf(path, sizeof path, "%s/radar_pass%03u.png", dir, g_radar_pass);
        // glReadPixels is bottom-up, so the PNG is vertically mirrored. That
        // costs nothing to say and a lot to get wrong.
        if (klfb_write_png(path, (const uint8_t *)pixels, w, h)) {
            g_radar_dumps++;
            fprintf(stderr, "  [radar] pass %u readback -> %s (%dx%d, "
                            "bottom-up so the image is flipped)\n",
                    g_radar_pass, path, w, h);
        }
    }
    if (klfb_radartrace())
        fprintf(stderr, "  [radar] pass %u closed after %u draw(s)\n",
                g_radar_pass, g_radar_pass_draws);
    g_radar_pass++;
    g_radar_pass_draws = 0;
}

// ---- round 37: EndRadarCapture, from the interposer in kl_sdl.c ------------
//
// THE BUG, and it is the guest's. BeginRadarCapture creates its texture and
// framebuffer on the first call — and that creation block BINDS the radar
// framebuffer — BEFORE it reaches
//
//     glGetIntegerv(GL_FRAMEBUFFER_BINDING, &g.radarSavedFramebuffer);
//
// so on the first call the capture saves ITSELF as "the target to go back to".
// EndRadarCapture then dutifully restores it, and the radar framebuffer stays
// bound for the whole rest of that eye's frame: the remaining GUI, the HUD
// planes, DrawRadarPlane and ApplyIrisBlackout all draw INTO the minimap
// texture. Launch #35 caught it red-handed — `pass 2 draw 0: program 72 ...
// tex0=261 ... vp 0,0 4128x4416` is DrawRadarPlane, the eye-sized present,
// executing with framebuffer 11 bound. From frame 2 on BeginEye rebinds the eye
// target, so the damage is frame 1 plus whatever the guest latched out of it —
// and the guest latches its crop from the first scan that finds any pixel.
//
// The fix is the smallest one that exists: remember what was bound when
// BeginRadarCapture was ENTERED, and put it back after EndRadarCapture returns.
// Nothing else about the guest's save/restore is touched — the viewport,
// scissor, depth, cull and colour mask it saves are all read after the creation
// block but the creation block changes none of them; the framebuffer is the
// only one it corrupts. KL_SHAR_RADARFIX=0 backs it out.
int32_t kl_glfb_shar_radar_binding(void) {
    int32_t fb = -1;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    return fb;
}

void kl_glfb_shar_radar_restore(int32_t want) {
    static void (*r_bind)(uint32_t, uint32_t);
    static int looked;
    if (want < 0) return;
    if (!looked) { looked = 1; r_bind = asym("glBindFramebuffer"); }
    if (!r_bind) return;
    int32_t now = kl_glfb_shar_radar_binding();
    if (now == want) return;
    r_bind(0x8D40 /* FRAMEBUFFER */, (uint32_t)want);
    static int said;
    if (said < 4) {
        said++;
        fprintf(stderr, "  [radar] EndRadarCapture left framebuffer %d bound, "
                        "not %d — rebound (the first BeginRadarCapture saved "
                        "ITSELF; KL_SHAR_RADARFIX=0 backs this out)\n",
                now, want);
    }
}

// The pass boundary, called while the radar framebuffer is still bound. It is
// also the only place a STEADY-STATE picture of the minimap can be taken: the
// guest's own glReadPixels fires once and never again.
void kl_glfb_shar_radar_endpass(void) {
    g_radar_endpass_hooked = 1;
    if (!klfb_radartrace() || g_radar_fb < 0) return;
    if (klfb_radar_pass_traced()) {
        klfb_radar_cover c;
        if (klfb_radar_scan(&c)) {
            fprintf(stderr, "  [radar] pass %u closed after %u draw(s): %u px "
                            "bounds %d,%d..%d,%d maxRGB=%u maxAlpha=%u%s\n",
                    g_radar_pass, g_radar_pass_draws, c.nonzero,
                    c.x0, c.y0, c.x1, c.y1, c.maxrgb, c.maxalpha,
                    (c.nonzero && c.maxrgb == 0)
                      ? "   <<< OPAQUE BLACK ONLY — the guest will latch this "
                        "as the minimap and stop looking"
                      : "");
            klfb_radar_dump("end", g_radar_pass_draws);
        }
    }
    g_radar_pass++;
    g_radar_pass_draws = 0;
}

static void klfb_FramebufferTexture2D(uint32_t target, uint32_t attachment,
                                      uint32_t textarget, uint32_t texture,
                                      int32_t level) {
    KLFB_LAZY_FLUSH();
    if (klfb_is_depth_attachment(attachment) && klfb_is_draw_target(target)) {
        klfb_note_depth_attach(texture, klfb_color_of_fbo(g_draw_fb));
        if (attachment != 0x8D20 /* not stencil-only */)
            klfb_note_fbo_depth(g_draw_fb, texture);
        klfb_depth_attach_filters(texture);
    }
    if (klfb_trace_fbo())
        fprintf(stderr, "  [glfb] t%llu glFramebufferTexture2D(target=0x%x fb=%u "
                        "att=0x%x, tex=%u)\n",
                (unsigned long long)klfb_tid(), target,
                klfb_fb_for_target(target), attachment, texture);
    // Colour attachment 0 only: the eye texture is what the picture lands in,
    // and a depth or stencil attachment says nothing about which stage it is.
    if (klfb_is_draw_target(target) && attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */) {
        klfb_note_fbo_color(g_draw_fb, texture, -1);
        klfb_rt_refresh(g_draw_fb, g_read_fb);   // r32 — the other such moment
        klfb_radar_note_color(g_draw_fb, texture);
        klfb_map_fbo(g_draw_fb, texture);
        // Unity may attach per frame rather than bind a pre-built FBO, so the
        // attachment itself has to count as committing to a stage.
        // The capture's hint, and it is taken HERE and not at the bind above:
        // this call carries the texture, where a bind only carries an FBO name
        // whose attachment we have to look up in a map that a re-created
        // swapchain has already made a liar (klfb_note_eye_fbo).
        int st = klfb_stage_of_tex(texture);
        klfb_note_eye_fbo(st >= 0 ? g_draw_fb : 0, g_draw_fb);
        klfb_note_render_stage(st);
        // r32: an offscreen colour target the allowlist did not match, named
        // once per distinct size. Eye stages are excluded — they are the thing
        // the allowlist exists to stay away from.
        if (st < 0 && klfb_rt_scale_of_tex(texture) == 1) {
            int32_t tw = 0, th = 0;
            if (klfb_tex_info(texture, NULL, &tw, &th))
                klfb_rt_note_candidate(tw, th);
        }
    }
    if (klfb_lazy_unbind_on()) {
        uint32_t afb = klfb_fb_for_target(target);
        if (klfb_att_same(afb, attachment, textarget, texture, level)) {
            g_att_skipped++;
            return;                     // already attached: a no-op ANGLE would not see as one
        }
        klfb_att_note(afb, attachment, textarget, texture, level);
    }
    if (g_real_FramebufferTexture2D)
        g_real_FramebufferTexture2D(target, attachment, textarget, texture, level);
}

// KL_XR_ARRAY_DIRECT (kl_openxr.c, vivecraft 26.x): an ARRAY swapchain whose
// slices are served as 2D textures of their own. Vivecraft 26.2 asks for ONE
// arraySize-2 swapchain and only ever touches it through
// glFramebufferTextureLayer(image, layer) + glBlitFramebuffer into that FBO, so
// each (image, layer) attachment is answered with a GL_TEXTURE_2D whose storage
// is the compositor's own MTLTexture (kl_glfb_bind_eye_mtl_texture, the 1.21.x
// re-point). The guest's blit then lands where the compositor samples: the
// per-eye mirror copy (two full-size unfoveated passes a frame) is gone, and the
// blit is 2D -> 2D, which ANGLE's physical passthrough honours (the array-slice
// source is what it did not — launch 10's double warp).
#define KLFB_ARRAY_REDIR 16
static struct { uint32_t arr; int layer; uint32_t tex; } g_arr_redir[KLFB_ARRAY_REDIR];

int kl_glfb_array_redirect(uint32_t array_tex, int layer, uint32_t tex2d) {
    if (!array_tex) return 0;
    for (int i = 0; i < KLFB_ARRAY_REDIR; i++)
        if (g_arr_redir[i].arr == array_tex && g_arr_redir[i].layer == layer) {
            if (tex2d) g_arr_redir[i].tex = tex2d;
            else g_arr_redir[i] = (__typeof__(g_arr_redir[0])){0};
            return 1;
        }
    if (!tex2d) return 1;
    for (int i = 0; i < KLFB_ARRAY_REDIR; i++)
        if (!g_arr_redir[i].arr) {
            g_arr_redir[i].arr = array_tex; g_arr_redir[i].layer = layer;
            g_arr_redir[i].tex = tex2d;
            return 1;
        }
    fprintf(stderr, "  [glfb] array redirect table full (%d) — array %u layer %d is "
                    "NOT redirected\n", KLFB_ARRAY_REDIR, array_tex, layer);
    return 0;
}

static uint32_t klfb_array_redirected(uint32_t array_tex, int layer) {
    if (!array_tex) return 0;
    for (int i = 0; i < KLFB_ARRAY_REDIR; i++)
        if (g_arr_redir[i].arr == array_tex && g_arr_redir[i].layer == layer)
            return g_arr_redir[i].tex;
    return 0;
}

uint32_t kl_glfb_eye_gl_texture(int eye, int stage);

static void klfb_FramebufferTextureLayer(uint32_t target, uint32_t attachment,
                                         uint32_t texture, int32_t level,
                                         int32_t layer) {
    KLFB_LAZY_FLUSH();
    {
        uint32_t t2 = klfb_array_redirected(texture, layer);
        if (t2) {
            static int said;
            if (said++ < 6)
                fprintf(stderr, "  [glfb] array direct: fb%u attaches array %u layer %d "
                                "-> 2D texture %u (the compositor's eye storage; no "
                                "mirror copy — KL_XR_ARRAY_DIRECT=0 is the A/B)\n",
                        klfb_fb_for_target(target), texture, layer, t2);
            klfb_FramebufferTexture2D(target, attachment, 0x0DE1 /* TEXTURE_2D */, t2, level);
            return;
        }
    }
    klfb_att_forget(klfb_fb_for_target(target), attachment);
    if (klfb_trace_fbo()) {
        uint32_t f = 0; int32_t tw = 0, th = 0;
        klfb_tex_info(texture, &f, &tw, &th);
        fprintf(stderr, "  [glfb] t%llu glFramebufferTextureLayer(target=0x%x "
                        "fb=%u att=0x%x tex=%u level=%d layer=%d) [%dx%d fmt=0x%x]"
                        " <- %s\n",
                (unsigned long long)klfb_tid(), target,
                klfb_fb_for_target(target), attachment, texture,
                level, layer, tw, th, f,
                klfb_caller(__builtin_return_address(0)));
    }
    klfb_note_tex3d_event(texture, 0, 1, 0);   // a volume the guest renders into
    if (klfb_is_depth_attachment(attachment)) {
        klfb_note_depth_attach(texture,
                               klfb_color_of_fbo(klfb_fb_for_target(target)));
        if (attachment != 0x8D20 /* not stencil-only */)
            klfb_note_fbo_depth(klfb_fb_for_target(target), texture);
        klfb_depth_attach_filters(texture);
    }
    if (attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */) {
        uint32_t fb = klfb_fb_for_target(target);
        klfb_note_fbo_color(fb, texture, layer);
        klfb_rt_refresh(g_draw_fb, g_read_fb);   // r32
        klfb_map_fbo(fb, texture);
        if (klfb_is_draw_target(target)) {
            int st = klfb_stage_of_tex(texture);
            klfb_note_eye_fbo(st >= 0 ? fb : 0, fb);
            klfb_note_render_stage(st);
        }
    }
    // An attach to the READ target while the READ binding is 0 is
    // GL_INVALID_OPERATION — you cannot give the default framebuffer an
    // attachment — and it is what VRChat's Unity does for EVERY eye copy:
    //
    //   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 4)
    //   glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, COLOR0, tex29, 0, layer)
    //   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, <swapchain image>)
    //   glBlitFramebuffer(...)
    //
    // libunity's own error handler reports the INVALID_OPERATION and carries
    // on, the read binding stays 0, and the blit copies the default framebuffer
    // — which on this path is a window nothing draws into — over the eye. The
    // picture is really there: Unity's eye array reads 3.5M of 5.5M texels lit
    // at the same instant (KL_GLFB_PROBE_TEX).
    //
    // The repair is not a guess about what the guest meant, which is the line
    // this file does not cross: the attach STATES the source completely —
    // texture, level and array layer — and the only thing missing is an object
    // that may legally carry it. So the same attachment is made on a
    // framebuffer of ours and that is bound as READ, and the guest's next read
    // bind takes it away again, which is exactly the lifetime the guest gave
    // it. The illegal call is not forwarded: all it can do is set an error.
    //
    // Its own FBO, never the probe's: klfb_read_from_texture_layer's is
    // re-pointed on every probe, and a diagnostic that silently moved the
    // guest's blit source is an instrument corrupting what it measures.
    // KL_GLFB_READ_ATTACH_FIX=0 restores the failing configuration exactly.
    static int read_fix = -1;
    if (read_fix < 0) read_fix = kl_env_on("KL_GLFB_READ_ATTACH_FIX", 1);
    if (read_fix && target == 0x8CA8 /* READ_FRAMEBUFFER */ && g_read_fb == 0 &&
        texture && attachment == 0x8CE0 /* COLOR_ATTACHMENT0 */) {
        static void (*r_Gen)(int32_t, uint32_t *);
        static void (*r_Bind)(uint32_t, uint32_t);
        static uint32_t (*r_Check)(uint32_t);
        static int resolved;
        if (!resolved) {
            resolved = 1;
            r_Gen = asym("glGenFramebuffers");
            r_Bind = asym("glBindFramebuffer");
            r_Check = asym("glCheckFramebufferStatus");
        }
        static uint32_t fix_fb;
        if (!fix_fb && r_Gen) r_Gen(1, &fix_fb);
        if (fix_fb && r_Bind && g_real_FramebufferTextureLayer) {
            r_Bind(0x8CA8, fix_fb);
            g_real_FramebufferTextureLayer(0x8CA8, attachment, texture, level, layer);
            uint32_t st = r_Check ? r_Check(0x8CA8) : 0x8CD5;
            static int said;
            if (!said++)
                fprintf(stderr, "  [glfb] READ_ATTACH_FIX: the guest attached tex %u "
                                "layer %d to the READ target with read framebuffer 0; "
                                "carried it on fb %u instead (status 0x%x)\n",
                        texture, layer, fix_fb, st);
            if (st != 0x8CD5) r_Bind(0x8CA8, 0);   // no worse than what it had
            return;
        }
    }
    if (g_real_FramebufferTextureLayer)
        g_real_FramebufferTextureLayer(target, attachment, texture, level, layer);
}

// KL_GLFB_NOSWIZZLE=1 drops the four swizzle parameters on the floor. The guest's
// single-channel textures then sample as plain red instead of as GL_ALPHA, so the
// picture is wrong — but it answers whether swizzle is *necessary* to the AGX
// abort, which a standalone repro has so far failed to settle. The abort is
// deterministic (call #312, five runs of five), so one run each way is conclusive.
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_A 0x8E45

// ---------------------------------------------------------------------------
// THE SAMPLER-LOD CENSUS (r21) — does the guest's content even HAVE mips?
//
// r20 read the tetris blue fill as a texture sampled at too high a mip level,
// on the grounds that a rasterization rate map makes the fragment shader
// derive its LOD in PHYSICAL space. That reading only has a mechanism if the
// textures being sampled carry a mip chain at all: a single-level allocation
// samples level 0 whatever the derivatives say, and a MIN_FILTER without
// _MIPMAP_ in its name never leaves level 0 either. Neither fact was in any
// log, so the theory could be neither confirmed nor killed from the guest's
// own side of the seam.
//
// So count them. Free (a switch on an enum, inside calls the guest already
// makes), printed with the other live censuses, and it answers the question in
// one worn launch.
static uint64_t g_lod_minfilter_mip, g_lod_minfilter_flat;
static uint64_t g_lod_filter_llmm, g_lod_filter_other_mip;
static uint64_t g_lod_maxlevel, g_lod_maxlevel_zero, g_lod_baselevel;
static uint64_t g_lod_clamp, g_lod_bias, g_lod_aniso;
static uint64_t g_lod_via_sampler;
static float    g_lod_aniso_worst = 1.0f;
static uint64_t g_genmipmap_calls;
static uint64_t g_texstore_calls, g_texstore_mipped, g_texstore_deepest;

#define KLFB_TEX_MIN_FILTER   0x2801
#define KLFB_TEX_BASE_LEVEL   0x813C
#define KLFB_TEX_MIN_LOD      0x813A
#define KLFB_TEX_MAX_LOD      0x813B
#define KLFB_TEX_MAX_LEVEL    0x813D
#define KLFB_TEX_LOD_BIAS     0x8501
#define KLFB_TEX_MAX_ANISO    0x84FE

// One entry point for all four spellings the guest can reach (texture or
// sampler object, integer or float), because a UE4 GL RHI sets its filtering
// through SAMPLER OBJECTS — a census that watched only glTexParameteri would
// have reported zero mipmapped filters and been believed.
static void klfb_note_lod(uint32_t pname, double param, int via_sampler) {
    switch (pname) {
    case KLFB_TEX_MIN_FILTER: {
        int32_t v = (int32_t)param;
        // 0x2700..0x2703 are the four _MIPMAP_ filters; 0x2600/0x2601 are not.
        if (v >= 0x2700 && v <= 0x2703) {
            __atomic_fetch_add(&g_lod_minfilter_mip, 1, __ATOMIC_RELAXED);
            if (v == 0x2703 /* LINEAR_MIPMAP_LINEAR */)
                __atomic_fetch_add(&g_lod_filter_llmm, 1, __ATOMIC_RELAXED);
            else
                __atomic_fetch_add(&g_lod_filter_other_mip, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_add(&g_lod_minfilter_flat, 1, __ATOMIC_RELAXED);
        }
        break;
    }
    case KLFB_TEX_MAX_LEVEL:
        __atomic_fetch_add(&g_lod_maxlevel, 1, __ATOMIC_RELAXED);
        // A cap of 0 pins that sampler to level 0, which makes the mip reading
        // impossible for whatever it samples — so it is counted separately.
        if ((int32_t)param == 0)
            __atomic_fetch_add(&g_lod_maxlevel_zero, 1, __ATOMIC_RELAXED);
        break;
    case KLFB_TEX_BASE_LEVEL:
        __atomic_fetch_add(&g_lod_baselevel, 1, __ATOMIC_RELAXED); break;
    case KLFB_TEX_MIN_LOD:
    case KLFB_TEX_MAX_LOD:
        __atomic_fetch_add(&g_lod_clamp, 1, __ATOMIC_RELAXED); break;
    case KLFB_TEX_LOD_BIAS:
        __atomic_fetch_add(&g_lod_bias, 1, __ATOMIC_RELAXED); break;
    case KLFB_TEX_MAX_ANISO:
        __atomic_fetch_add(&g_lod_aniso, 1, __ATOMIC_RELAXED);
        if ((float)param > g_lod_aniso_worst) g_lod_aniso_worst = (float)param;
        break;
    default: return;   // not an LOD parameter; do not book it as one
    }
    if (via_sampler) __atomic_fetch_add(&g_lod_via_sampler, 1, __ATOMIC_RELAXED);
}

static void klfb_note_tex_levels(int32_t levels) {
    __atomic_fetch_add(&g_texstore_calls, 1, __ATOMIC_RELAXED);
    if (levels > 1) {
        __atomic_fetch_add(&g_texstore_mipped, 1, __ATOMIC_RELAXED);
        uint64_t d = __atomic_load_n(&g_texstore_deepest, __ATOMIC_RELAXED);
        if ((uint64_t)levels > d)
            __atomic_store_n(&g_texstore_deepest, (uint64_t)levels, __ATOMIC_RELAXED);
    }
}

void kl_glfb_lod_census(FILE *f) {
    if (!f) return;
    uint64_t mip = g_lod_minfilter_mip, flat = g_lod_minfilter_flat;
    fprintf(f, "  [lod] MIN_FILTER %llu set(s): %llu mipmapped (%llu trilinear, "
               "%llu other), %llu level-0 only%s\n",
            (unsigned long long)(mip + flat), (unsigned long long)mip,
            (unsigned long long)g_lod_filter_llmm,
            (unsigned long long)g_lod_filter_other_mip,
            (unsigned long long)flat,
            (mip + flat) == 0 ? "  <- the guest sets NO minification filter" : "");
    fprintf(f, "  [lod] MAX_LEVEL %llu (%llu pinned to 0), BASE_LEVEL %llu, "
               "MIN/MAX_LOD %llu, LOD_BIAS %llu, MAX_ANISOTROPY %llu (worst %.1f); "
               "%llu of these came through sampler objects\n",
            (unsigned long long)g_lod_maxlevel,
            (unsigned long long)g_lod_maxlevel_zero,
            (unsigned long long)g_lod_baselevel,
            (unsigned long long)g_lod_clamp,
            (unsigned long long)g_lod_bias,
            (unsigned long long)g_lod_aniso, (double)g_lod_aniso_worst,
            (unsigned long long)g_lod_via_sampler);
    fprintf(f, "  [lod] glGenerateMipmap %llu; glTexStorage 2D/3D %llu allocation(s), "
               "%llu with more than one level (deepest %llu)%s\n",
            (unsigned long long)g_genmipmap_calls,
            (unsigned long long)g_texstore_calls,
            (unsigned long long)g_texstore_mipped,
            (unsigned long long)g_texstore_deepest,
            (g_texstore_mipped == 0 && g_genmipmap_calls == 0 && mip == 0)
                ? "  <- NOTHING in this run carries a mip chain, so no LOD"
                  " reading can be the fill"
                : "");
}

// ---- KL_GLFB_NOMIP: the LOD A/B, from the sampler side ---------------------
//
// r29's arithmetic left exactly one shape of cause standing for the flat fill:
// every fragment of the named draw reads the SAME texel. There are two ways to
// arrive there — degenerate texture coordinates, or a LOD high enough that a
// 1024x1024 texture has collapsed to its top mip — and they are told apart by
// one knob rather than by a fifth argument from shader text.
//
// KL_GLFB_NOMIP=1 demotes every MIPMAP min filter to its non-mipmapped base
// (LINEAR_MIPMAP_* -> LINEAR, NEAREST_MIPMAP_* -> NEAREST), on textures and on
// sampler objects both, so NO sample in the run can come from anywhere but
// level 0. Fill gone -> the fill is a mip level and the LOD question is the
// round after (r20's family, alive again on a different arm than KL_VRR_FLOOR
// tested). Fill still there -> LOD cannot paint it at any level, and the
// coordinates are the whole remaining answer.
//
// It is a DIAGNOSTIC: forcing level 0 everywhere costs minification aliasing
// and texture bandwidth across the entire title. Default off; baked on for
// tetris for the one build that asks the question.
static int klfb_nomip(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_NOMIP", 0);
    return on;
}
static uint64_t g_nomip_demoted;
static int klfb_nomip_filter(uint32_t pname, int32_t param) {
    static unsigned said;
    if (pname != 0x2801 /* TEXTURE_MIN_FILTER */ || !klfb_nomip()) return param;
    int32_t out = param;
    if (param == 0x2700 || param == 0x2702) out = 0x2600;      // NEAREST_MIPMAP_*
    if (param == 0x2701 || param == 0x2703) out = 0x2601;      // LINEAR_MIPMAP_*
    // The banner is capped at three lines, so a run that engaged the arm on
    // three filters and a run that engaged it on seven hundred read the same.
    // The count is what says how much of the title the diagnostic is costing.
    if (out != param) __atomic_fetch_add(&g_nomip_demoted, 1, __ATOMIC_RELAXED);
    if (out != param && said++ < 3)
        fprintf(stderr, "  [glfb] NOMIP: min filter 0x%04x -> 0x%04x (level 0 only; "
                        "KL_GLFB_NOMIP=0 restores the guest's choice)\n",
                (unsigned)param, (unsigned)out);
    return out;
}


// ---- depth textures sampled with LINEAR, which ES leaves UNDEFINED ---------
//
// A depth texture is not filterable unless the driver says so. ES 3.0 §3.8.13:
// a texture whose internal format is DEPTH_COMPONENT* is incomplete for
// sampling if its minification or magnification filter is anything but NEAREST
// **and** its compare mode is NONE — and an incomplete texture samples as
// (0,0,0,1). Hardware PCF (compare mode COMPARE_REF_TO_TEXTURE) is the one case
// where LINEAR is legal, because the filtering then happens on the COMPARISON
// rather than on the depth value.
//
// Qualcomm samples it anyway. Metal does not. The failure is silent, has no GL
// error, no incomplete-framebuffer status and no link error: the shadow lookup
// simply returns 0.0, every `depth > shadowSample` test passes, and the whole
// world is in shadow. That is a BLACK WORLD with a perfectly healthy log —
// which is what Simpsons Hit & Run's Springfield is, and its cascades announce
// the exact combination in the game's own words: "cascade 0 framebuffer ready
// (2048x2048, depth-only, manual bilinear PCF)". Manual bilinear PCF means the
// shader samples four texels and lerps them ITSELF, so it needs NEAREST; a
// LINEAR filter left on the texture is not a filtering choice, it is a
// leftover.
//
// So: for a texture this runtime knows to be depth-formatted, a LINEAR filter
// written while the compare mode is NONE is rewritten to NEAREST, and the swap
// is UNDONE if the guest later turns compare mode on (the order of the two
// writes is the guest's business, and a hardware-PCF texture is entitled to its
// LINEAR). Reported once per texture either way, because "your shadows are
// undefined" is worth a line whichever way it goes.
//
// KL_GLFB_DEPTH_LINEAR=1 leaves the guest's write alone — the A/B that says
// whether this was the black world or merely a defect beside it.
#define KLFB_MAX_DTEX 32
static struct {
    uint32_t name;
    int      cmp;          // compare mode is COMPARE_REF_TO_TEXTURE
    int      want_min, want_mag;   // what the guest asked for
    int      forced;       // we rewrote at least one filter for it
    int      said;
    int      probed;       // its real filters have been read back once
} g_dtex[KLFB_MAX_DTEX];
static unsigned g_ndtex;

static int klfb_fmt_is_depth(uint32_t f) {
    return f == 0x81A5 /* DEPTH_COMPONENT16 */ || f == 0x81A6 /* DEPTH_COMPONENT24 */ ||
           f == 0x81A7 /* DEPTH_COMPONENT32 */ || f == 0x8CAC /* DEPTH_COMPONENT32F */ ||
           f == 0x88F0 /* DEPTH24_STENCIL8 */  || f == 0x8CAD /* DEPTH32F_STENCIL8 */ ||
           f == 0x1902 /* DEPTH_COMPONENT */;
}

static unsigned klfb_dtex_slot(uint32_t name) {
    for (unsigned i = 0; i < g_ndtex; i++) if (g_dtex[i].name == name) return i;
    if (g_ndtex >= KLFB_MAX_DTEX) return (unsigned)-1;
    g_dtex[g_ndtex].name = name;
    g_dtex[g_ndtex].want_min = g_dtex[g_ndtex].want_mag = 0x2600 /* NEAREST */;
    return g_ndtex++;
}

// Returns the parameter to actually pass. `name` is the bound texture.
static int32_t klfb_depth_filter_fix(uint32_t name, uint32_t pname, int32_t param) {
    static int off = -1;
    if (off < 0) off = kl_env_on("KL_GLFB_DEPTH_LINEAR", 0);
    if (off || !name) return param;
    uint32_t fmt = 0;
    if (!klfb_tex_info(name, &fmt, NULL, NULL) || !klfb_fmt_is_depth(fmt)) return param;
    unsigned i = klfb_dtex_slot(name);
    if (i == (unsigned)-1) return param;

    if (pname == 0x884C /* TEXTURE_COMPARE_MODE */) {
        int now_cmp = (param == 0x884E /* COMPARE_REF_TO_TEXTURE */);
        if (now_cmp && !g_dtex[i].cmp && g_dtex[i].forced) {
            // The guest wants hardware PCF after all: give the filters back.
            if (g_real_TexParameteri) {
                g_real_TexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, g_dtex[i].want_min);
                g_real_TexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, g_dtex[i].want_mag);
            }
            fprintf(stderr, "  [glfb] depth tex %u went into COMPARE_REF_TO_TEXTURE — "
                            "its LINEAR filters are legal now and have been restored\n",
                    name);
            g_dtex[i].forced = 0;
        }
        g_dtex[i].cmp = now_cmp;
        return param;
    }
    if (pname != 0x2801 /* MIN_FILTER */ && pname != 0x2800 /* MAG_FILTER */)
        return param;
    if (pname == 0x2801) g_dtex[i].want_min = param; else g_dtex[i].want_mag = param;
    // NEAREST and NEAREST_MIPMAP_NEAREST are the two complete choices; anything
    // else filters, and filtering a depth texture with compare mode NONE is the
    // undefined case.
    if (g_dtex[i].cmp) return param;
    if (param == 0x2600 /* NEAREST */ || param == 0x2700 /* NEAREST_MIPMAP_NEAREST */)
        return param;
    if (!g_dtex[i].said++)
        fprintf(stderr, "  [glfb] depth tex %u (fmt 0x%x) asked for filter 0x%x with "
                        "compare mode NONE — ES leaves that sample UNDEFINED and Metal "
                        "returns 0, which reads as 'everything is in shadow'. Forced to "
                        "NEAREST (KL_GLFB_DEPTH_LINEAR=1 restores the guest's write)\n",
                name, fmt, param);
    g_dtex[i].forced = 1;
    return 0x2600 /* NEAREST */;
}

// ---- ...and the same rule asked of GL instead of guessed from a write ------
//
// r5 rewrote a LINEAR filter to NEAREST at the moment the guest WROTE it, and
// launch #5's log does not carry the line at all. That is NOT proof the filters
// were already NEAREST, which is how it was read at the time. The write hook
// can miss the state three separate ways:
//
//   * the guest sets its filters BEFORE glTexImage2D gives the texture a depth
//     format, so klfb_tex_info answers "not depth" and the hook declines;
//   * the guest sets them through glTexParameterf / glTexParameterfv / a
//     sampler object, none of which the r5 hook is on;
//   * the guest never sets a min filter at all — and the ES DEFAULT is
//     NEAREST_MIPMAP_LINEAR, which is a filtering filter on a texture with no
//     mip chain. That is the incomplete case arrived at by doing nothing, and
//     no write ever happens for a hook to see.
//
// So read it back instead, at the one moment the texture is known for certain
// to be a depth texture: when it is ATTACHED as one. Three queries and a bind,
// once per texture, on a path that runs a handful of times a level. The state
// is PRINTED whatever it is — this is the evidence launch #5 should have had —
// and rewritten to NEAREST only when the pair is the undefined one.
//
// KL_GLFB_DEPTH_LINEAR=1 reports without rewriting: the A/B.
static void klfb_depth_attach_filters(uint32_t tex) {
    if (!tex) return;
    unsigned i = klfb_dtex_slot(tex);
    if (i == (unsigned)-1 || g_dtex[i].probed) return;
    static void (*a_GetTexParameteriv)(uint32_t, uint32_t, int32_t *);
    static void (*a_BindTexture)(uint32_t, uint32_t);
    static int  looked;
    if (!looked) {
        looked = 1;
        a_GetTexParameteriv = asym("glGetTexParameteriv");
        a_BindTexture       = asym("glBindTexture");
    }
    if (!a_GetTexParameteriv || !a_BindTexture || !a_glGetIntegerv ||
        !g_real_TexParameteri) return;
    g_dtex[i].probed = 1;

    int32_t saved = 0;
    a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &saved);
    a_BindTexture(0x0DE1 /* TEXTURE_2D */, tex);
    int32_t mn = 0, mg = 0, cm = 0;
    a_GetTexParameteriv(0x0DE1, 0x2801 /* MIN_FILTER */,   &mn);
    a_GetTexParameteriv(0x0DE1, 0x2800 /* MAG_FILTER */,   &mg);
    a_GetTexParameteriv(0x0DE1, 0x884C /* COMPARE_MODE */, &cm);

    int cmp = (cm == 0x884E /* COMPARE_REF_TO_TEXTURE */);
    int bad = !cmp && ((mn != 0x2600 && mn != 0x2700) || mg != 0x2600);
    static int off = -1;
    if (off < 0) off = kl_env_on("KL_GLFB_DEPTH_LINEAR", 0);
    if (bad && !off) {
        g_dtex[i].want_min = mn;
        g_dtex[i].want_mag = mg;
        g_real_TexParameteri(0x0DE1, 0x2801, 0x2600 /* NEAREST */);
        g_real_TexParameteri(0x0DE1, 0x2800, 0x2600 /* NEAREST */);
        g_dtex[i].forced = 1;
    }
    g_dtex[i].cmp = cmp;
    a_BindTexture(0x0DE1, (uint32_t)(saved > 0 ? saved : 0));

    int32_t dw = 0, dh = 0; uint32_t dfmt = 0;
    klfb_tex_info(tex, &dfmt, &dw, &dh);
    fprintf(stderr, "  [glfb] depth tex %u %dx%d fmt=0x%x attached: MIN 0x%x MAG 0x%x "
                    "COMPARE %s -> %s\n",
            tex, dw, dh, dfmt, mn, mg, cmp ? "REF_TO_TEXTURE" : "NONE",
            !bad     ? "COMPLETE, left alone"
            : off    ? "INCOMPLETE for sampling (Metal returns 0) — LEFT AS-IS, "
                       "KL_GLFB_DEPTH_LINEAR=1"
                     : "INCOMPLETE for sampling (Metal returns 0) — forced to NEAREST");
}

static void klfb_TexParameteri(uint32_t target, uint32_t pname, int32_t param) {
    klfb_note_lod(pname, (double)param, 0);
    if (klfb_trace_tex())
        fprintf(stderr, "  [glfb] #%u glTexParameteri target=0x%04x pname=0x%04x "
                        "param=0x%04x\n", g_texcalls++, target, pname, param);
    static int noswz = -1;
    if (noswz < 0) noswz = kl_env_on("KL_GLFB_NOSWIZZLE", 0);
    if (noswz && pname >= GL_TEXTURE_SWIZZLE_R && pname <= GL_TEXTURE_SWIZZLE_A) return;
    // TEXTURE_SRGB_DECODE_EXT writes: no EXT_texture_sRGB_decode on this ANGLE,
    // so the write can only raise INVALID_ENUM — and does, about once per
    // frame, leaving a sticky error that Unity then reports attributed to
    // innocent calls (and that error-bracketing probes read as everyone's
    // fault). Dropping it is semantics-preserving here: the decode behaviour
    // it would select does not exist on this driver either way.
    if (pname == 0x8A48 /* TEXTURE_SRGB_DECODE_EXT */) return;
    // TEXTURE_MAX_ANISOTROPY_EXT, same shape: we do not advertise
    // EXT_texture_filter_anisotropic (kl_egl's rule — an extension is named
    // only if the ES level we claim makes it core), so the write's only
    // possible effect is the error. srceng's togl writes it per texture.
    if (pname == 0x84FE /* TEXTURE_MAX_ANISOTROPY_EXT */) return;
    // The external-image retarget again — see klfb_detarget. The guest sets
    // filtering and wrap on GL_TEXTURE_EXTERNAL_OES right after binding it, and
    // those calls have to land on the same object the bind did.
    if (target == 0x8D65 /* TEXTURE_EXTERNAL_OES */) target = 0x0DE1;
    param = klfb_nomip_filter(pname, param);
    // ...and the depth-texture completeness rule, which needs to know WHICH
    // texture is bound rather than only what was asked for.
    if (target == 0x0DE1 /* TEXTURE_2D */ && a_glGetIntegerv) {
        int32_t bound = 0;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
        if (bound > 0) param = klfb_depth_filter_fix((uint32_t)bound, pname, param);
        if (pname == 0x2801 /* MIN_FILTER */ && g_real_TexParameteri) {
            g_real_TexParameteri(target, pname, param);
            klfb_aniso_inject(target, bound, param);   // no-op unless KL_GLFB_ANISO
            goto set;
        }
    }
    if (g_real_TexParameteri) g_real_TexParameteri(target, pname, param);
set:
    if (kl_env_on("KL_GLFB_ERRPROBE", 0) && a_glGetError) {
        uint32_t e = a_glGetError();
        if (e) fprintf(stderr, "  [glfb] glTexParameteri(0x%04x, 0x%04x) -> err 0x%x\n",
                       pname, param, e);
    }
}

// The other three spellings, thunked for the census ALONE — every one of them
// forwards unconditionally and changes nothing about the call. glTexParameteri
// above is the only one of the four with behaviour of its own.
static void (*g_real_TexParameterf)(uint32_t, uint32_t, float);
static void klfb_TexParameterf(uint32_t target, uint32_t pname, float param) {
    klfb_note_lod(pname, (double)param, 0);
    param = (float)klfb_nomip_filter(pname, (int32_t)param);
    if (g_real_TexParameterf) g_real_TexParameterf(target, pname, param);
    // KL_GLFB_ANISO, for a guest that spells MIN_FILTER as a float.
    if (pname == 0x2801 && target == 0x0DE1 && klfb_aniso_wanted() > 1.0f &&
        a_glGetIntegerv) {
        int32_t bound = 0;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
        klfb_aniso_inject(target, bound, (int32_t)param);
    }
}
static void (*g_real_SamplerParameteri)(uint32_t, uint32_t, int32_t);
static void klfb_SamplerParameteri(uint32_t sampler, uint32_t pname, int32_t param) {
    klfb_note_lod(pname, (double)param, 1);
    param = klfb_nomip_filter(pname, param);
    if (g_real_SamplerParameteri) g_real_SamplerParameteri(sampler, pname, param);
}
static void (*g_real_SamplerParameterf)(uint32_t, uint32_t, float);
static void klfb_SamplerParameterf(uint32_t sampler, uint32_t pname, float param) {
    klfb_note_lod(pname, (double)param, 1);
    param = (float)klfb_nomip_filter(pname, (int32_t)param);
    if (g_real_SamplerParameterf) g_real_SamplerParameterf(sampler, pname, param);
}

static void (*g_real_TexParameterfv)(uint32_t, uint32_t, const float *);
static void klfb_TexParameterfv(uint32_t target, uint32_t pname, const float *param) {
    // Wrapped because ANGLE rejected togl's fv writes with "Extension is not
    // enabled" and the integer form's trace could not see them; the drop
    // rules below mirror klfb_TexParameteri's, one pname family over.
    if (klfb_trace_tex())
        fprintf(stderr, "  [glfb] #%u glTexParameterfv target=0x%04x pname=0x%04x "
                        "param=%.2f\n", g_texcalls++, target, pname,
                        param ? param[0] : 0.0f);
    // Same two drops as the integer form, same reasoning (see
    // klfb_TexParameteri): togl writes the border color with the fv form.
    if (pname == 0x8A48 /* TEXTURE_SRGB_DECODE_EXT */) return;
    if (pname == 0x84FE /* TEXTURE_MAX_ANISOTROPY_EXT */) return;
    if (pname == 0x1004 /* TEXTURE_BORDER_COLOR — no EXT_texture_border_clamp */)
        return;
    if (g_real_TexParameterfv) g_real_TexParameterfv(target, pname, param);
}

// KL_GLFB_TEX_LIMIT=N performs only the first N uploads and drops the rest. The
// guest's texture stream is deterministic — every perturbation tried so far leaves
// it at exactly 327 calls — so it can be bisected by index, which is what the
// standalone replays could not do: they reproduced the calls but not the context
// around them. Binary-searching N finds the upload the abort actually needs, in
// situ. Storage allocations are always performed, so the textures still exist and
// only their contents go missing.
static int klfb_upload_budget(void) {
    static int n = -2;
    if (n == -2) {
        n = kl_env_int("KL_GLFB_TEX_LIMIT", -1);
    }
    return n;
}
static unsigned g_uploads;

static void klfb_TexSubImage2D(uint32_t target, int32_t level, int32_t xoff,
                               int32_t yoff, int32_t w, int32_t h, uint32_t format,
                               uint32_t type, const void *pixels) {
    int budget = klfb_upload_budget();
    unsigned idx = g_uploads++;
    if (budget >= 0 && (int)idx >= budget) {
        if (klfb_trace_tex())
            fprintf(stderr, "  [glfb] upload %u DROPPED (budget %d)\n", idx, budget);
        return;
    }
    if (klfb_trace_tex())
        fprintf(stderr, "  [glfb] #%u glTexSubImage2D target=0x%04x level=%d "
                        "%dx%d at %d,%d fmt=0x%04x type=0x%04x%s\n",
                g_texcalls++, target, level, w, h, xoff, yoff, format, type,
                pixels ? "" : " (NULL)");
    // The mip-chain record's uncompressed half. Bytes are not known here
    // without the pixel-store state, so the level is marked WRITTEN with zero
    // bytes — which is all the "was this level ever filled" question needs.
    if (target == 0x0DE1 /* TEXTURE_2D */) klfb_note_tex_level(g_bound2d, level, 0);
    // r32: an UPLOAD into a supersampled render target would land in a corner of
    // storage the guest does not know it has, and there is no honest fix for it
    // here (the pixels are the guest's, at the guest's size). A render target
    // is written by drawing, so this should never fire — it is named rather
    // than silently mis-placed, because the alternative is a corrupted texture
    // with no line in the log.
    if (target == 0x0DE1 && klfb_rt_scale_of_tex(g_bound2d) > 1) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] RT supersample: glTexSubImage2D into "
                            "SCALED tex %u (level %d, %dx%d at %d,%d) — this "
                            "target is not a pure render target and the upload "
                            "lands at the unscaled offset\n",
                    g_bound2d, level, w, h, xoff, yoff);
    }
    if (g_real_TexSubImage2D)
        g_real_TexSubImage2D(target, level, xoff, yoff, w, h, format, type, pixels);
}

static void klfb_TexStorage2D(uint32_t target, int32_t levels, uint32_t fmt,
                              int32_t w, int32_t h) {
    klfb_note_format(fmt, w, h, 1, "glTexStorage2D");
    klfb_aniso_forget(target, 0);       // KL_GLFB_ANISO: a re-specified name
    klfb_note_tex_levels(levels);
    if (klfb_trace_tex()) {
        int32_t bound = -1;
        if (a_glGetIntegerv) a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
        fprintf(stderr, "  [glfb] #%u glTexStorage2D target=0x%04x levels=%d "
                        "fmt=0x%04x %dx%d (tex=%d)\n", g_texcalls++, target,
                levels, fmt, w, h, bound);
    }
    fmt = klfb_maybe_unsrgb(fmt);
    // r32: the immutable half of the render-target supersample. See the block
    // above klfb_TexImage2D. Immutable storage cannot be re-specified, so a
    // scaled name here stays scaled until the texture is deleted.
    int rtf = (target == 0x0DE1 /* TEXTURE_2D */) ? klfb_rt_match(w, h) : 1;
    if (rtf > 1) {
        w *= rtf; h *= rtf;
    }
    // Record the allocation per texture name: ES 3.0 has no
    // glGetTexLevelParameteriv (the ANGLE ext entry point rejects on this
    // context — the census's fmt=0x0 readings), so the readback path learns
    // what a texture IS from this table instead of by query.
    if (a_glGetIntegerv) {
        int32_t bound = -1;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
        if (bound > 0) {
            if (rtf > 1) {
                klfb_rt_note_scaled((uint32_t)bound, rtf);
                fprintf(stderr, "  [glfb] RT supersample: tex %d glTexStorage2D "
                                "%dx%d -> %dx%d (fmt=0x%x levels=%d)\n",
                        bound, w / rtf, h / rtf, w, h, fmt, levels);
            }
            klfb_note_tex_storage((uint32_t)bound, fmt, w, h);
            klfb_note_tex_alloc_levels((uint32_t)bound, levels);
            klfb_vram_note(KLC_TEX, (uint32_t)bound,
                           klfb_storage_bytes(fmt, w, h, 1, levels));
        }
    }
    if (g_real_TexStorage2D) g_real_TexStorage2D(target, levels, fmt, w, h);
}
static void klfb_TexStorage3D(uint32_t target, int32_t levels, uint32_t fmt,
                              int32_t w, int32_t h, int32_t d) {
    klfb_note_format(fmt, w, h, d, "glTexStorage3D");
    klfb_note_tex_levels(levels);
    fmt = klfb_maybe_unsrgb(fmt);
    if (a_glGetIntegerv) {
        // 2D_ARRAY and 3D have their own binding points; the target says which.
        int32_t bound = -1;
        a_glGetIntegerv(target == 0x8C1A /* TEXTURE_2D_ARRAY */ ? 0x8C1D
                                                               : 0x806A /* 3D */,
                        &bound);
        if (bound > 0) {
            // The 2D thunk's record, for arrays too: an eye texture that is one
            // array with a slice per eye is the ONLY shape an OpenXR guest in
            // single-pass-instanced layout has, and without this klfb_tex_info
            // answers "never heard of it" for exactly the texture the capture
            // and every trace here are about.
            klfb_note_tex_storage((uint32_t)bound, fmt, w, h);
            // A true 3D texture is the fog-volume shape; a 2D_ARRAY is an eye
            // swapchain or a shadow atlas and belongs to nobody's fog.
            if (target != 0x8C1A /* TEXTURE_2D_ARRAY */)
                klfb_note_tex3d((uint32_t)bound, fmt, w, h, d);
            if (klfb_census_every())
                klfb_vram_note(KLC_TEX, (uint32_t)bound,
                               klfb_storage_bytes(fmt, w, h, d, levels));
        }
    }
    if (g_real_TexStorage3D) g_real_TexStorage3D(target, levels, fmt, w, h, d);
}

void kl_glfb_report_formats(void) {
    if (!g_nfmts) return;
    fprintf(stderr, "  [glfb] immutable texture formats allocated:");
    for (unsigned i = 0; i < g_nfmts; i++)
        fprintf(stderr, " 0x%04x=%u", g_fmts[i].fmt, g_fmts[i].n);
    fprintf(stderr, "\n");
}

// ---------------------------------------------------------- ABI thunks
//
// Per-texture storage record, written by the glTexStorage2D thunk above and
// read by the capture: ES 3.0 has no glGetTexLevelParameteriv (the ANGLE
// extension entry point rejects on this context), so what a texture *is* —
// format and size — has to come from watching the allocation. Cold path, and
// appends are serialised with the compile lock because the eye-texture setup
// arrives on a different thread than some of the guest's own storage calls.
// 4096 rather than 512 because VRChat allocates over 1400 textures and the
// eye render targets are among the LAST — a table that fills drops exactly the
// entries the readback path is about, and every consumer then reads "never
// heard of it". It still has a ceiling, so it names its first drop:
// a full table is a diagnostic going quiet.
// r30 grew the record a MIP-CHAIN half. NOMIP=1 removed the fill, which says
// the sample was coming from a level above 0 — and a level above 0 can be wrong
// two ways: the LOD that selects it, or the level's own contents. The second
// one is answerable on the host with no readback and no GL query at all, by
// watching the uploads: glTexStorage2D says how many levels the texture HAS,
// and every compressed/uncompressed SubImage says which level was actually
// filled and with how many bytes. A level that was allocated and never written
// reads as whatever the driver left there — a constant, which is the symptom.
//
// `levels` is the allocation's level count, `wmask` a bit per level 0..15 that
// received an upload, `lb` the bytes each level received. 4096 entries at 13
// levels is ~213 KB of BSS and costs nothing per call: the destination texture
// name comes from g_bound2d (tracked in the glBindTexture thunk), not from a
// glGetIntegerv round trip, which is what kept this measurement behind a knob
// before.
#define KLFB_MAX_TEX 4096
#define KLFB_MAX_LVL 13
static struct {
    uint32_t name, fmt; int32_t w, h;
    int16_t  levels;                  // from glTexStorage2D, 0 = never seen
    uint16_t wmask;                   // levels that received an upload
    uint32_t lb[KLFB_MAX_LVL];        // bytes uploaded per level
} g_tex[KLFB_MAX_TEX];
static unsigned g_ntex;

static void klfb_note_tex_storage(uint32_t name, uint32_t fmt, int32_t w,
                                  int32_t h) {
    pthread_mutex_lock(&g_compile_lock);
    for (unsigned i = 0; i < g_ntex; i++)
        if (g_tex[i].name == name) {          // reallocation replaces
            g_tex[i].fmt = fmt; g_tex[i].w = w; g_tex[i].h = h;
            // ...and a reallocation is a NEW chain: the old level record
            // describes storage that no longer exists, and keeping it would
            // report a fresh allocation as fully written.
            g_tex[i].levels = 0; g_tex[i].wmask = 0;
            memset(g_tex[i].lb, 0, sizeof g_tex[i].lb);
            pthread_mutex_unlock(&g_compile_lock);
            return;
        }
    if (g_ntex < KLFB_MAX_TEX) {
        g_tex[g_ntex].name = name; g_tex[g_ntex].fmt = fmt;
        g_tex[g_ntex].w = w; g_tex[g_ntex].h = h;
        g_ntex++;
    } else {
        static int said_full;
        if (!said_full++)
            fprintf(stderr, "  [glfb] the texture allocation table is full at %d "
                            "entries — every texture from here on (tex %u is the "
                            "first) is unknown to the readback paths, which will "
                            "report the size rather than guess\n",
                    KLFB_MAX_TEX, name);
    }
    pthread_mutex_unlock(&g_compile_lock);
}

static int klfb_tex_info(uint32_t name, uint32_t *fmt, int32_t *w, int32_t *h) {
    int found = 0;
    pthread_mutex_lock(&g_compile_lock);
    for (unsigned i = 0; i < g_ntex; i++)
        if (g_tex[i].name == name) {
            if (fmt) *fmt = g_tex[i].fmt;
            if (w) *w = g_tex[i].w;
            if (h) *h = g_tex[i].h;
            found = 1;
            break;
        }
    pthread_mutex_unlock(&g_compile_lock);
    return found;
}

// ---- the mip-chain half of the ledger (r30) --------------------------------
//
// Two writers, both on paths that already hold the texture name for free.
// `levels` comes from glTexStorage2D; the per-level bytes come from every
// upload thunk. Neither one issues a GL call, so this is measurement that
// cannot perturb what it measures — the trap the r28 probe knob was built to
// avoid.
static void klfb_note_tex_alloc_levels(uint32_t name, int32_t levels) {
    if (!name) return;
    pthread_mutex_lock(&g_compile_lock);
    for (unsigned i = 0; i < g_ntex; i++)
        if (g_tex[i].name == name) {
            g_tex[i].levels = (int16_t)(levels > 0 ? levels : 1);
            g_tex[i].wmask  = 0;
            memset(g_tex[i].lb, 0, sizeof g_tex[i].lb);
            break;
        }
    pthread_mutex_unlock(&g_compile_lock);
}

// What a texture's chain looks like, for the consumer that has a texture name
// and wants the one-line verdict rather than the whole census.
static int klfb_tex_chain(uint32_t name, int *levels, unsigned *wmask) {
    int found = 0;
    pthread_mutex_lock(&g_compile_lock);
    for (unsigned i = 0; i < g_ntex; i++)
        if (g_tex[i].name == name) {
            if (levels) *levels = g_tex[i].levels;
            if (wmask)  *wmask  = g_tex[i].wmask;
            found = 1;
            break;
        }
    pthread_mutex_unlock(&g_compile_lock);
    return found;
}

static void klfb_note_tex_level(uint32_t name, int32_t level, long bytes) {
    if (!name || level < 0 || level >= KLFB_MAX_LVL) return;
    pthread_mutex_lock(&g_compile_lock);
    for (unsigned i = 0; i < g_ntex; i++)
        if (g_tex[i].name == name) {
            g_tex[i].wmask |= (uint16_t)(1u << level);
            if (bytes > 0) g_tex[i].lb[level] += (uint32_t)bytes;
            break;
        }
    pthread_mutex_unlock(&g_compile_lock);
}

// The other half of the depth-attachment census: sizes, which only became
// knowable once g_tex existed. One line per depth texture, and the line the
// blue fill needs is the one where the depth size and the colour size DISAGREE
// — that is a scene-depth read landing at the wrong screen coordinate.
void kl_glfb_depth_tex_census(FILE *f) {
    if (!g_ndepthtex) return;
    fprintf(f, "  [glfb] depth attachments: %u distinct\n", g_ndepthtex);
    for (unsigned i = 0; i < g_ndepthtex; i++) {
        uint32_t dfmt = 0, cfmt = 0;
        int32_t dw = 0, dh = 0, cw = 0, ch = 0;
        klfb_tex_info(g_depthtex[i].tex, &dfmt, &dw, &dh);
        if (g_depthtex[i].color) klfb_tex_info(g_depthtex[i].color, &cfmt, &cw, &ch);
        const char *verdict = "";
        if (g_depthtex[i].sampled && g_depthtex[i].color && dw && cw &&
            (dw != cw || dh != ch))
            verdict = "  <<< SAMPLED AND SIZE-MISMATCHED vs its colour target";
        else if (g_depthtex[i].sampled)
            verdict = "  <- sampled";
        fprintf(f, "  [glfb]   depth tex %u %dx%d fmt=0x%x attached %u x, bound as "
                   "sampler %u x; colour tex %u %dx%d fmt=0x%x%s\n",
                g_depthtex[i].tex, dw, dh, dfmt, g_depthtex[i].attached,
                g_depthtex[i].sampled, g_depthtex[i].color, cw, ch, cfmt, verdict);
    }
}

// The eye textures, by name, as kl_ovrp's SetupEyeTexture2 allocates them
// (latest registration wins — Unity re-creates them on resize). The capture
// finds "the framebuffer with the picture" by looking for the FBO whose
// color attachment is one of these.
static uint32_t g_eye_tex[2];

// ...and by (eye, stage), which is what the stage observation needs. Each stage
// is a distinct GL texture name — Unity asks for storage per (eye, stage)
// through ovrp_SetupEyeTexture2 — so the name the guest draws into IS the
// stage, with nothing to infer.
#define KLFB_MAX_STAGES 4
static uint32_t g_eye_tex_stage[2][KLFB_MAX_STAGES];
static int      g_eye_ever;   // an eye texture was registered at some point

void kl_glfb_note_eye_texture(int eye, int stage, uint32_t tex) {
    if (eye < 0 || eye > 1 || !tex) return;
    g_eye_tex[eye] = tex;
    // An eye pair is what distinguishes a VR guest from a flat one, and it is
    // the stronger signal of the two kl_present watches: Unity creates an EGL
    // window surface as well, being an Android app, so the window alone cannot
    // tell them apart. See kl_present.h.
    kl_present_note_eye_texture();
    if ((unsigned)stage >= KLFB_MAX_STAGES) return;
    // GL recycles names, and Unity re-creates the whole eye swapchain
    // on resize. A name that meant some other (eye, stage) must stop answering
    // for it the moment it is re-registered: klfb_stage_of_tex
    // returns the FIRST match, so one stale entry files a frame's pose against
    // a different stage's picture for the rest of the run — permanently, and
    // silently, because every count still looks healthy.
    for (int e = 0; e < 2; e++)
        for (int s = 0; s < KLFB_MAX_STAGES; s++)
            if (g_eye_tex_stage[e][s] == tex) g_eye_tex_stage[e][s] = 0;
    g_eye_tex_stage[eye][stage] = tex;
    // "This run has eye textures" is a fact about the RUN, not about the table,
    // and the census needs it: an all-zero table is the single most important
    // thing it can report, and gating on a non-empty table is how the census
    // fell silent on exactly the run it was written for.
    g_eye_ever = 1;
}

// Which of an eye's images the guest most recently PRESENTED.
//
// kl_glfb_note_eye_texture registers a whole swapchain, and the last call wins,
// so g_eye_tex ends up naming whichever image was registered last — image 2 of
// 3, i.e. the right picture one frame in three. That is fine for "is this an
// eye texture at all" and useless for "read the frame the guest just drew".
// The OpenXR path knows exactly which image that is (xrReleaseSwapchainImage
// says so) and says it here, so the capture reads the presented image rather
// than whichever one the rotation happens to be pointing at.
// ...and everything else the capture would otherwise have to GUESS about it.
//
// The name alone was not enough, twice over. (a) The size: kl_glfb learns a
// texture's dimensions from its own glTexStorage2D thunk, and an OpenXR
// swapchain is allocated through kl_egl_sym — the real ANGLE entry point —
// so klfb_tex_info has never heard of it. The capture then kept the pbuffer's
// size and read a 2290x2400 eye as 1832x1920, which comes back as the window.
// (b) The LAYER: this guest's eye swapchain is ONE texture with two array
// slices, so both eyes share a name and differ only by layer, and
// glFramebufferTexture2D cannot attach a slice of a 2D array at all.
//
// So the OpenXR path states the whole thing — name, size, layer — because it
// is the one place that knows it, and the capture stops searching. It is not a
// search because a GL name is a slot, not an identity: names are reissued.
static struct { uint32_t tex; int32_t w, h; int layer; } g_live_eye[2];

void kl_glfb_set_live_eye_image(int eye, uint32_t tex, int32_t w, int32_t h, int layer) {
    if (eye < 0 || eye > 1 || !tex) return;
    g_eye_tex[eye] = tex;
    g_live_eye[eye].tex = tex;
    g_live_eye[eye].w = w;
    g_live_eye[eye].h = h;
    g_live_eye[eye].layer = layer;
}

void kl_glfb_set_live_eye_texture(int eye, uint32_t tex) {
    kl_glfb_set_live_eye_image(eye, tex, 0, 0, -1);
}

// Which stage is the current draw target, and which was last drawn into.
//
// Two thunks maintain this between them: glFramebufferTexture2D says which
// texture an FBO draws into, glBindFramebuffer says which FBO is current. The
// map is tiny and the lookups are linear because there are two or three eye
// FBOs in this title and the alternative is a hash table for six entries.
//
// The map stores the TEXTURE, not the stage it resolved to at the time. The
// texture is the identity; the stage is a fact about the texture that changes
// when Unity re-creates the swapchain, and an FBO whose attachment is unchanged
// must follow it. Caching the resolved stage instead leaves entries that
// outlive their meaning and answer confidently afterwards.
#define KLFB_FBO_MAP 16
static struct { uint32_t fbo, tex; } g_fbo_stage[KLFB_FBO_MAP];
static int      g_render_stage = -1;    // the last eye stage actually bound for drawing

int kl_glfb_last_render_stage(void) {
    return __atomic_load_n(&g_render_stage, __ATOMIC_RELAXED);
}

// The stage an eye texture name belongs to, or -1.
static int klfb_stage_of_tex(uint32_t tex) {
    if (!tex) return -1;
    for (int e = 0; e < 2; e++)
        for (int s = 0; s < KLFB_MAX_STAGES; s++)
            if (g_eye_tex_stage[e][s] == tex) return s;
    return -1;
}

// How often the sixteen-slot map could not answer and the fallback below did.
// r11's evidence for why the fallback exists, kept as a running count so the
// exit report can say whether it is load-bearing on this guest or dead weight.
static uint64_t g_stage_via_color;   // resolved through g_fbo_color instead
static uint64_t g_map_full;          // an eye FBO that found no free slot
// ...and r12's: a release that would have wiped a registration already made for
// the texture replacing it. Declared here so the census below can print it.
static uint64_t g_forget_refused;

static int klfb_stage_of_fbo(uint32_t fbo) {
    // A hit that RESOLVES wins; a hit that resolves to -1 does not end the
    // search. The narrow map's row can name a texture that has since stopped
    // being an eye texture while the colour table has the FBO's current
    // attachment, and returning the stale -1 there is a second way to lose a
    // frame that was drawn into a perfectly good eye stage.
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (g_fbo_stage[i].fbo == fbo) {
            int s = klfb_stage_of_tex(g_fbo_stage[i].tex);
            if (s >= 0) return s;
            break;
        }
    // ---------------------------------------------------------- the fallback
    //
    // r11, on device: the guest drew 189,574 times into the three eye-stage
    // textures — 63,209 into fb 20 (tex 2471), 63,189 into fb 21 (tex 133),
    // 63,176 into fb 22 (tex 134) — and every one of its 19,945 frames was
    // recorded as having "drawn into no eye stage". Nothing was ever filed, so
    // the compositor had no complete stage to composite and showed 22,735
    // consecutive black frames over an open immersive space. The rendering was
    // right; only this lookup was wrong.
    //
    // g_fbo_stage is the narrower of two tables that are filled from the SAME
    // two call sites, one line apart: klfb_note_fbo_color takes every
    // framebuffer into 48 slots, klfb_map_fbo takes only those whose colour
    // attachment is a registered eye texture AT THE MOMENT OF ATTACHMENT, into
    // 16. Two ways for the narrow one to miss what the wide one has, and this
    // guest re-creates its eye swapchain (DestroyLayer, then a second
    // SetupLayer at twice the width for DoubleWide) so it gets both chances:
    // an attachment made before the texture is registered is refused a slot
    // permanently, and sixteen slots are not many once two swapchains' worth of
    // eye FBOs have each taken one.
    //
    // The texture is the identity either way — klfb_stage_of_tex is still the
    // only authority on which stage a name means, and it is re-read here rather
    // than cached — so consulting the wider table can only add resolutions the
    // narrow one should have had. An FBO whose colour attachment is not an eye
    // texture still answers -1, exactly as before.
    for (unsigned i = 0; i < g_nfbo_color; i++)
        if (g_fbo_color[i].fbo == fbo) {
            int s = klfb_stage_of_tex(g_fbo_color[i].tex);
            if (s >= 0) __atomic_fetch_add(&g_stage_via_color, 1, __ATOMIC_RELAXED);
            return s;
        }
    return -1;
}

static void klfb_map_fbo(uint32_t fbo, uint32_t tex) {
    if (!fbo) return;
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (g_fbo_stage[i].fbo == fbo) { g_fbo_stage[i].tex = tex; return; }
    // Only eye FBOs earn one of the sixteen slots. This title has plenty of
    // other framebuffers, and remembering the first sixteen it happens to
    // create would leave no room for the ones the observation is about.
    if (klfb_stage_of_tex(tex) < 0) return;
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (!g_fbo_stage[i].fbo) { g_fbo_stage[i].fbo = fbo;
                                   g_fbo_stage[i].tex = tex; return; }
    // Full. Not fatal any more — klfb_stage_of_fbo falls back to g_fbo_color —
    // but worth counting, because it is one of the two ways r11 lost every
    // frame it drew.
    __atomic_fetch_add(&g_map_full, 1, __ATOMIC_RELAXED);
}

// The association state, printed into the exit report. r11 could say the guest
// had drawn 189,574 times into eye textures and that not one frame was filed,
// and had nothing in between to say WHY — this is that middle.
void kl_glfb_eye_map_census(FILE *f) {
    // g_eye_ever, not "the table is non-empty": an EMPTY table on a run that
    // registered eye textures is the whole of r12's bug, and a census that
    // returns early there is silent on precisely the run it exists for.
    if (!g_eye_ever && !g_stage_via_color) return;
    fprintf(f, "  [eyemap] registered eye textures:");
    for (int e = 0; e < 2; e++) {
        fprintf(f, " eye%d[", e);
        for (int s = 0; s < KLFB_MAX_STAGES; s++)
            fprintf(f, "%s%u", s ? " " : "", g_eye_tex_stage[e][s]);
        fprintf(f, "]");
    }
    fprintf(f, "\n");
    fprintf(f, "  [eyemap] fbo->stage map (%d slots):", KLFB_FBO_MAP);
    int used = 0;
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (g_fbo_stage[i].fbo) {
            used++;
            fprintf(f, " fb%u=tex%u->s%d", g_fbo_stage[i].fbo,
                    g_fbo_stage[i].tex, klfb_stage_of_tex(g_fbo_stage[i].tex));
        }
    fprintf(f, "%s\n", used ? "" : " (empty)");
    // The wide table, restricted to the rows the narrow one should have had.
    fprintf(f, "  [eyemap] eye FBOs the colour table knows:");
    int extra = 0;
    for (unsigned i = 0; i < g_nfbo_color; i++) {
        int s = klfb_stage_of_tex(g_fbo_color[i].tex);
        if (s < 0) continue;
        int mapped = 0;
        for (int k = 0; k < KLFB_FBO_MAP; k++)
            if (g_fbo_stage[k].fbo == g_fbo_color[i].fbo) { mapped = 1; break; }
        fprintf(f, " fb%u=tex%u->s%d%s", g_fbo_color[i].fbo,
                g_fbo_color[i].tex, s, mapped ? "" : " (NOT in the map)");
        if (!mapped) extra++;
    }
    fprintf(f, "\n  [eyemap] %llu lookup(s) answered by the fallback, "
               "%d eye FBO(s) missing from the map, %llu slot-full refusal(s), "
               "%llu release(s) that kept a re-registered slot\n",
            (unsigned long long)g_stage_via_color, extra,
            (unsigned long long)g_map_full,
            (unsigned long long)g_forget_refused);
    fprintf(f, "  [eyemap] eye binds per stage:");
    for (int s = 0; s < KLFB_MAX_STAGES; s++)
        fprintf(f, " %llu", (unsigned long long)kl_glfb_stage_draw_count(s));
    fprintf(f, "\n");
}

// ---------------------------------------------------------------------------
// The same censuses, PERIODICALLY, because on device the exit report is
// unreachable.
//
// KL_EXIT_ON_BACKGROUND=0 is baked for this target — the app deliberately does
// not exit when its window closes — and the only other way a worn session ends
// is a force quit, which is SIGKILL. So kl_egl_report has never run on device,
// and two rounds in a row were diagnosed from an exit report written by a HOST
// run of a bug that only exists on device. Everything the exit report knows
// that matters mid-run is printed here instead, on a timer, always on.
//
// KL_GLFB_CENSUS_SECS sets the period (default 15 s, 0 turns it off).
// kl_ovrp's half of the live block. Declared here rather than by including
// kl_ovrp.h: kl_glfb is the layer BELOW the XR seam and has no other reason to
// know about it.
void kl_ovrp_eye_census(FILE *f);

static int klfb_census_secs(void) {
    static int secs = -1;
    if (secs < 0) secs = kl_env_int("KL_GLFB_CENSUS_SECS", 15);
    return secs;
}

// `why` names the flush point in the log: "15s" for the timer, "layer paused"
// for the one event a closed window is guaranteed to deliver.
void kl_glfb_live_census(const char *why) {
    int secs = klfb_census_secs();
    if (!secs && !why) return;
    static long long next;
    long long now = (long long)time(NULL);
    if (!why) {
        long long due = __atomic_load_n(&next, __ATOMIC_RELAXED);
        if (now < due) return;
        // Racing here costs a duplicate block, not a wrong number.
        __atomic_store_n(&next, now + secs, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&next, now + secs, __ATOMIC_RELAXED);
    }
    fprintf(stderr, "  [census] --- live (%s) ---\n", why ? why : "timer");
    kl_glfb_eye_map_census(stderr);
    kl_glfb_draw_census(stderr);
    kl_glfb_link_census(stderr);
    kl_glfb_lod_census(stderr);
    kl_glfb_mipchain_census(stderr);
    klfb_depth_scan_report();
    kl_glfb_depth_tex_census(stderr);
    kl_glfb_depth_fetch_census(stderr);
    kl_ovrp_eye_census(stderr);
}

// Forgetting, which is the half that was missing.
//
// kl_glfb_note_eye_texture already drops a stale (eye, stage) when a NAME is
// re-registered, but nothing dropped one when the storage was DESTROYED — and
// between ovrp_DestroyEyeTexture and the next ovrp_SetupEyeTexture2 there is a
// window six textures wide on this title. In it, `stages: s0/s1/s2` and "drew
// into N stages" keep answering for names that no longer mean anything, and GL
// hands those names straight back out, so an unrelated object inherits a dead
// eye texture's identity and is filed as that stage. Every count still looks
// healthy, which is what made the "one Begin/End window drew into 2 stages
// (mask 0x5)" line on this port unreadable. The sixteen FBO slots leak the same
// way: filled once per swapchain generation and never reclaimed, the map
// saturates after two reconfigures and stops recording the eye FBOs it exists
// for.
static void klfb_forget_fbo(uint32_t fbo) {
    if (!fbo) return;
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (g_fbo_stage[i].fbo == fbo) { g_fbo_stage[i].fbo = 0;
                                         g_fbo_stage[i].tex = 0; }
}

// `released` is the GL name whose storage is going away, or 0 when the caller
// cannot say. The registration is only dropped when it still names THAT
// texture.
//
// r12, on device: it did not, and every eye stage lost its identity in the
// first two seconds of the run. klovrp_GetLayerTexture2 registers the new name
// and THEN binds the Metal storage:
//
//     kl_glfb_note_eye_texture(eye, stage, name);      // (eye,stage) := 2471
//     kl_glfb_bind_eye_mtl_texture(eye, stage, name, ...);
//
// and inside that bind sits the "replaced with no teardown" backstop, which
// calls kl_glfb_release_eye_texture(eye, stage) for the texture the slot USED
// to hold. The release then reached here and cleared the registration made one
// line earlier. Six calls, six wipes, and g_eye_tex_stage was empty for the
// rest of the run — so klfb_stage_of_tex answered -1 for every name, so
// klfb_stage_of_fbo answered -1 through both the sixteen-slot map AND r11's
// wider colour-table fallback, so all 29,587 guest frames were booked as
// "drew into no eye stage" while the guest drew 189,574 times into those very
// textures. The compositor sat on serial 0 and composited black over an open
// immersive space.
//
// The backstop only fires when a replacement arrives with no preceding
// ovrp_DestroyEyeTexture, which is why no host run ever saw this: on host the
// line "replaced with no teardown" appears 0 times and the ordinary teardown
// path releases while the slot still holds the OLD name, where the guard below
// agrees and the clear happens exactly as before.
static void klfb_forget_eye_stage(int eye, int stage, uint32_t released) {
    if ((unsigned)eye > 1 || (unsigned)stage >= KLFB_MAX_STAGES) return;
    uint32_t tex = g_eye_tex_stage[eye][stage];
    // KL_EYE_FORGET_STRICT=0 restores the unconditional clear, which is the A/B
    // for the whole of r12: with the repro above
    // (KL_GLFB_MTL=1 KL_OVRP_SKIP_EYE_TEARDOWN=1) it turns a clean association
    // into "every frame drew into no eye stage" on host, exactly as on device.
    static int strict = -1;
    if (strict < 0) strict = kl_env_on("KL_EYE_FORGET_STRICT", 1);
    if (strict && released && tex && tex != released) {
        __atomic_fetch_add(&g_forget_refused, 1, __ATOMIC_RELAXED);
        fprintf(stderr, "  [glfb] eye=%d stage=%d release of GL texture %u kept "
                        "the registration: the slot already names %u, which is "
                        "the texture that replaced it\n",
                eye, stage, released, tex);
        // The FBO rows for the DEAD name are still stale, though, and leaving
        // them saturates the sixteen slots one generation at a time.
        for (int i = 0; i < KLFB_FBO_MAP; i++)
            if (g_fbo_stage[i].tex == released) { g_fbo_stage[i].fbo = 0;
                                                  g_fbo_stage[i].tex = 0; }
        return;
    }
    g_eye_tex_stage[eye][stage] = 0;
    if (!tex) return;
    // An FBO whose attachment just died is unattributable, not stage-something:
    // klfb_stage_of_fbo would resolve it through a texture with no stage and
    // answer -1 anyway, so freeing the slot loses nothing and un-saturates the
    // map for the swapchain generation that follows.
    for (int i = 0; i < KLFB_FBO_MAP; i++)
        if (g_fbo_stage[i].tex == tex) { g_fbo_stage[i].fbo = 0;
                                         g_fbo_stage[i].tex = 0; }
}

// --- The observation window -------------------------------------------------
//
// `g_render_stage` alone is a sticky global, and a sticky global answers even
// when nothing happened. That is the whole weakness of the association: at
// ovrp_EndFrame it reports *a* stage whether or not this frame drew into one,
// so a guest whose draws land outside the Begin..End window — a different
// thread, an entry point these thunks do not watch — gets the previous frame's
// answer with no way to tell.
//
// So the window is explicit. kl_ovrp opens one at BeginFrame and reads it at
// EndFrame, and what comes back is not just "which stage" but *how many* eye
// binds happened inside the window and on which thread. Those two numbers turn
// three different failures into three different readings:
//
//   binds == 0        nothing drew into an eye texture between Begin and End.
//                     The stage below is the PREVIOUS frame's, i.e. exactly the
//                     off-by-one that pairs a fresh pose with a stale picture.
//   two stages set    the frame committed to more than one stage, so "one
//                     stage per frame" — which the pose record assumes — is
//                     false for this title.
//   tid != EndFrame's the draws are on another thread and the window bounds
//                     nothing; ordering, not observation, is the problem.
//
// Counted rather than sampled, because the artefact this is chasing alternates:
// a probe that fires every N frames sees one phase of it and reports it as the
// steady state.
static uint32_t g_render_mask;          // bit per stage drawn into, this window
static uint32_t g_render_binds;         // eye binds this window
static uint64_t g_render_tid;           // who did the last of them
static uint64_t g_stage_binds[KLFB_MAX_STAGES];   // ...and for the whole run

// pthread_threadid_np is cheap but not free, and this sits under every
// glBindFramebuffer. Once per thread is enough — the id cannot change.
static uint64_t klfb_tid_cached(void) {
    static __thread uint64_t t;
    if (!t) pthread_threadid_np(NULL, &t);
    return t;
}

void kl_glfb_begin_render_window(void) {
    __atomic_store_n(&g_render_mask, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_render_binds, 0, __ATOMIC_RELAXED);
}

int kl_glfb_render_stages(uint32_t *mask, uint32_t *binds, uint64_t *tid) {
    if (mask)  *mask  = __atomic_load_n(&g_render_mask, __ATOMIC_RELAXED);
    if (binds) *binds = __atomic_load_n(&g_render_binds, __ATOMIC_RELAXED);
    if (tid)   *tid   = __atomic_load_n(&g_render_tid, __ATOMIC_RELAXED);
    return __atomic_load_n(&g_render_stage, __ATOMIC_RELAXED);
}

uint64_t kl_glfb_stage_draw_count(int stage) {
    if ((unsigned)stage >= KLFB_MAX_STAGES) return 0;
    return __atomic_load_n(&g_stage_binds[stage], __ATOMIC_RELAXED);
}

// The framebuffer the guest ITSELF last drove with an eye texture attached —
// and the reason the capture cannot just scan for one.
//
// A guest that re-creates its eye swapchain (1.40 does, 2290x2400 ->
// 2748x2880) deletes the old textures, and GL reissues the very same names to
// the new ones. Framebuffers the guest built for the old generation and then
// abandoned still REPORT one of those names as their colour attachment, so a
// scan matching on name finds a stale FBO first, reads the orphaned storage,
// and prints "0 lit" on a run whose eye textures are full — with the eye
// plainly attached and the size plainly right. Recency is what tells the two
// apart, and only the guest's own calls carry it.
static uint32_t g_last_eye_fbo;

// `fbo` is the framebuffer that now holds an eye texture, or 0 if `touched` no
// longer does — a guest that re-points the same FBO at something else must not
// leave the hint asserting otherwise.
static void klfb_note_eye_fbo(uint32_t fbo, uint32_t touched) {
    if (fbo) __atomic_store_n(&g_last_eye_fbo, fbo, __ATOMIC_RELAXED);
    else if (__atomic_load_n(&g_last_eye_fbo, __ATOMIC_RELAXED) == touched)
        __atomic_store_n(&g_last_eye_fbo, 0, __ATOMIC_RELAXED);
}

// Called from both thunks: a stage is now the draw target. Reported once, so a
// log says whether the observation is working at all rather than leaving the
// counter fallback to be discovered by its symptoms.
static void klfb_note_render_stage(int stage) {
    if (stage < 0) return;
    int was = __atomic_exchange_n(&g_render_stage, stage, __ATOMIC_RELAXED);
    __atomic_fetch_or(&g_render_mask, 1u << stage, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_render_binds, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_stage_binds[stage], 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_render_tid, klfb_tid_cached(), __ATOMIC_RELAXED);
    if (was < 0)
        fprintf(stderr, "  [glfb] eye stage observed from the draw target "
                        "(first: stage %d)\n", stage);
}

// ---------------------------------------------------------------------------
// The Metal interop: eglCreateImageKHR rather than a pbuffer.
//
// The extension is unconditional on ANGLE's Metal backend (DisplayMtl.mm sets
// both mtlTextureClientBuffer and EGLImageOES), so there is no capability path
// to take here — if the entry points resolve, it works, and if they do not the
// caller falls back to ordinary GL storage.
#define EGL_NO_CONTEXT_          ((void *)0)
#define EGL_NO_IMAGE_            ((void *)0)
#define EGL_NONE_                0x3038
#define EGL_DEVICE_EXT_                      0x322C
#define EGL_METAL_DEVICE_ANGLE_              0x34A6
#define EGL_METAL_TEXTURE_ANGLE_             0x34A7
#define EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE_ 0x34DD
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE_   0x345D

static void *(*a_eglCreateImageKHR)(void *, void *, uint32_t, void *, const int32_t *);
static unsigned (*a_eglDestroyImageKHR)(void *, void *);
static unsigned (*a_eglQueryDisplayAttribEXT)(void *, int32_t, intptr_t *);
static unsigned (*a_eglQueryDeviceAttribEXT)(void *, int32_t, intptr_t *);
static void (*a_glEGLImageTargetTexture2DOES)(uint32_t, void *);
static void (*a_glBindTexture_mtl)(uint32_t, uint32_t);

// Foveation. These two are OURS — they exist only in the patched ANGLE
// (angle-patches/klepton.patch), so a NULL here means the loaded libGLESv2
// predates the patch rather than that anything is broken, and it is worth
// saying so by name rather than silently not foveating.
static void (*a_SetRateMap)(void *texture, void *map);
static void (*a_SetRateMapForSize)(uint32_t w, uint32_t h, uint32_t min_samples, void *map);

// The map in force, and the eye size it was built for. One map for both eyes
// and every stage: the two render targets in a per-eye chain have to share one
// or the resolve between them stops being a physical-to-physical copy, and
// there is no reason for the stages to differ from each other.
static void *g_eye_rate_map;
static int   g_eye_rate_w, g_eye_rate_h;
static int   g_eye_rate_zx, g_eye_rate_zy;

// The multisampled scene target is the one the size rule is for; a
// single-sampled target of the same size is a post-processing intermediate and
// must NOT be foveated. See kl_glfb.h. This is the DEFAULT:
// KL_VRR_MIN_SAMPLES=1 (klfb_rate_min_samples) is the per-target override for
// a guest whose scene target is single-sampled and whose post copy reads it at
// the physical position (Vivecraft — see klfb_rewrite_vr_post).
#define KL_RATE_MIN_SAMPLES 2


static kl_glfb_mtl_provider g_mtl_provider;
static void *g_mtl_provider_ctx;
// One record per (eye, slot). KL_MTL_MAX_STAGES is in the header, because a
// PROVIDER has to be sized by the same number and providers live outside this
// file — a provider that holds fewer declines every slot past its own end, and
// kl_glfb then has no storage for those layers at all.
// w/h are carried so a rate map can be matched against the texture it would be
// attached to. A map is built for one screen size; attaching it to a texture of
// another is a warp against coordinates that do not exist, and nothing in Metal
// or ANGLE reports it.
// `top_left` records which way up the guest rendered — see
// kl_glfb_eye_mtl_origin_top_left. It stays 0 for everything that arrives
// through the GL path below, which is every entry this table had until the
// Vulkan seam existed.
static struct { void *tex; int slice; void *image; uint32_t gl_tex; int w, h;
                int top_left; }
    g_eye_mtl[2][KL_MTL_MAX_STAGES];

void kl_glfb_set_mtl_provider(kl_glfb_mtl_provider fn, void *ctx) {
    g_mtl_provider = fn;
    g_mtl_provider_ctx = ctx;
}
int kl_glfb_has_mtl_provider(void) { return g_mtl_provider != NULL; }

// --- what is actually IN an eye texture -------------------------------------
//
// Everything above observes *addressing*: which stage was bound as the draw
// target, how often, from which thread. None of it can tell a stage full of
// SUPERHOT from a stage full of black — and on the SUPERHOT bring-up that was
// the whole question. 20,000 live guest frames, every association healthy,
// `0 stale in a row` at the compositor, and a black headset. The counters read
// the same either way, which is what made them unfalsifiable.
//
// The one existing reader of eye pixels, KL_GLFB_DUMP_SINK, needs a registered
// frame sink — only the host viewer has one — so on device there was no way to
// ask the question at all.
//
// So ask it directly and cheaply: two 32x32 patches per (eye, stage), one at
// the centre and one at (25%, 25%), reported as min/mean/max over RGB. Two
// patches because a centre sample of a scene whose middle happens to be a wall
// says nothing on its own, and a single off-centre one measures vignetting
// rather than content. Alpha is excluded deliberately: it is 1.0 in a black
// frame as readily as in a lit one, and averaging it in hides exactly the
// answer being looked for. 8 KiB of readback against a 4032x3234 texture.
//
// The read goes through a scratch FBO of ours, not the guest's. The guest
// attaches its eye texture per frame and the fbo->stage map only holds FBOs it
// happened to bind, so a missing entry would report "no picture" when the truth
// is "no FBO recorded" — the probe must not be able to fail in the same
// direction as the thing it is measuring. The READ_FRAMEBUFFER binding is saved
// and restored around the sweep; nothing else is touched.
//
// KL_GLFB_STAGE_PROBE=N — report every Nth ovrp_EndFrame (0/unset: off).
// The other half of the same question, and the one the bind counters cannot
// reach: was anything WRITTEN into the stage, or was it only ADDRESSED?
//
// `g_stage_binds` counts glBindFramebuffer/glFramebufferTexture2D — a guest can
// run those 27,000 times against an eye texture it never puts a pixel in, which
// is exactly what "26,990 eye-stage draws" turned out to mean on this port. So
// count the calls that can actually change the contents — draws, clears, and
// the resolve blit's destination — against the stage that was the draw target
// at the time, and report them beside the pixels. "Frozen picture, zero writes"
// and "frozen picture, thousands of writes" are different bugs in different
// files, and nothing before this could tell them apart.
static uint64_t g_stage_draws_w[KLFB_MAX_STAGES];
static uint64_t g_stage_clears[KLFB_MAX_STAGES];
static uint64_t g_stage_blits[KLFB_MAX_STAGES];

static int klfb_stage_probe_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_int("KL_GLFB_STAGE_PROBE", 0) > 0;
    return on;
}

// `what`: 0 draw, 1 clear, 2 blit destination. Gated on the knob because it
// sits under every draw call — off, it is one predictable branch.
static void klfb_note_stage_write(int32_t fb, int what) {
    if (!klfb_stage_probe_on() || fb < 0) return;
    int s = klfb_stage_of_fbo((uint32_t)fb);
    if (s < 0) return;
    uint64_t *c = what == 0 ? g_stage_draws_w : what == 1 ? g_stage_clears
                                                          : g_stage_blits;
    __atomic_fetch_add(&c[s], 1, __ATOMIC_RELAXED);
}

// ---- and the same accounting for EVERY framebuffer, not just the eye ones ---
//
// "The eye stages take 0 draws and a resolve blit, and the blit's source holds
// only the loading UI" leaves the real question open: where do the scene
// camera's draws GO? The guest is demonstrably simulating a level — enemies
// spawn, are shot and die in the managed log — so either those draws land in
// some framebuffer we can name, or they do not happen at all, and those are
// completely different bugs.
//
// So keep a census keyed by FBO name: draws, clears, blits out of it, blits
// into it. At report time each one is resolved to its colour attachment (type,
// name, size, format) so the line reads as "fb 9 is the 2290x2400 RGBA16F
// scene target, 3,400 draws, blitted out 266 times" rather than as a number.
// FBO 0 (the window) is counted too — a guest that renders its scene to the
// default framebuffer is exactly the kind of thing this is looking for.
#define KLFB_FBO_CENSUS 48
static struct { uint32_t fbo; int used; uint64_t draws, clears, blit_out, blit_in; }
    g_fbo_census[KLFB_FBO_CENSUS];

static uint64_t g_draws_all, g_draws_uncensused;

// `what`: 0 draw, 1 clear, 2 blit destination, 3 blit source.
static void klfb_note_fbo_use(int32_t fb, int what) {
    if (!klfb_stage_probe_on() || fb < 0) return;
    int slot = -1;
    for (int i = 0; i < KLFB_FBO_CENSUS; i++) {
        if (g_fbo_census[i].used && g_fbo_census[i].fbo == (uint32_t)fb) { slot = i; break; }
        if (!g_fbo_census[i].used && slot < 0) slot = i;
    }
    // Every draw, everywhere, so the census's own coverage can be checked: a
    // per-FBO table that accounts for 8 of a frame's draws is only evidence if
    // the frame HAS 8 draws, and a saturated 48-slot table silently reports a
    // fraction of the truth as the whole of it.
    if (what == 0) __atomic_fetch_add(&g_draws_all, 1, __ATOMIC_RELAXED);
    if (slot < 0) {                             // census full: drop, do not wrap
        if (what == 0) __atomic_fetch_add(&g_draws_uncensused, 1, __ATOMIC_RELAXED);
        return;
    }
    if (!g_fbo_census[slot].used) {
        g_fbo_census[slot].used = 1;
        g_fbo_census[slot].fbo  = (uint32_t)fb;
    }
    uint64_t *c = what == 0 ? &g_fbo_census[slot].draws
                : what == 1 ? &g_fbo_census[slot].clears
                : what == 2 ? &g_fbo_census[slot].blit_in
                            : &g_fbo_census[slot].blit_out;
    __atomic_fetch_add(c, 1, __ATOMIC_RELAXED);
}

#define KLFB_PATCH 32

static float klfb_half_to_float(uint16_t h);   // defined with the capture below

// The readback goes through a PIXEL PACK BUFFER, and on device that is not a
// preference — it is the difference between a number and a SIGSEGV.
//
// glReadPixels into our own memory sends ANGLE's Metal backend down the last
// resort in FrameBufferMtl.mm's readPixelsImpl, which ends in
// mtl::Texture::getBytes -> -[MTLTexture getBytes:...]. That call is legal only
// on a CPU-accessible texture, and ANGLE says so with ASSERT(isCPUAccessible())
// — an assert, which a release ANGLE compiles out. Our eye stages arrive
// through an EGLImage over an MTLTexture that KleptonCompositor allocates
// `storageMode = .private`, because it is a render target the CPU never
// touches; isCPUAccessible() is false for exactly that, so with the assert gone
// Metal reads a CPU mapping that was never made. It is a fault inside the
// driver, not a GL error, so no amount of glGetError tolerance around the call
// can see it coming. The buffer path that would be safe here is real but gated:
// copyTextureToBufferForReadOptimization is `isAMD()` in DisplayMtl.mm, never
// true on an Apple GPU.
//
// With a pack buffer bound, readPixelsImpl takes readPixelsToPBO instead, which
// copies texture -> buffer through a blit command encoder — a private texture
// is that path's ordinary case. Host reads the same way, so the path that has
// to survive the headset is the one every host run exercises.
//
// Resolved from ANGLE directly rather than from the guest's import table: this
// is our buffer, and a guest that never linked glMapBufferRange must not be
// able to disarm the probe's only safe road.
static void  (*rp_GenBuffers)(int32_t, uint32_t *);
static void  (*rp_BindBuffer)(uint32_t, uint32_t);
static void  (*rp_BufferData)(uint32_t, intptr_t, const void *, uint32_t);
static void *(*rp_MapBufferRange)(uint32_t, intptr_t, intptr_t, uint32_t);
static uint8_t (*rp_UnmapBuffer)(uint32_t);
static uint32_t g_pack_pbo;

// Must be asked BEFORE a Metal-backed stage is read: there, "no pack buffer"
// means "do not read at all", not "read the slow way".
static int klfb_pack_pbo_ready(void) {
    static int tried;
    if (!tried) {
        tried = 1;
        rp_GenBuffers     = asym("glGenBuffers");
        rp_BindBuffer     = asym("glBindBuffer");
        rp_BufferData     = asym("glBufferData");
        rp_MapBufferRange = asym("glMapBufferRange");    // GLES 3.0
        rp_UnmapBuffer    = asym("glUnmapBuffer");       // GLES 3.0
        if (rp_GenBuffers && rp_BindBuffer && rp_BufferData &&
            rp_MapBufferRange && rp_UnmapBuffer)
            rp_GenBuffers(1, &g_pack_pbo);
    }
    return g_pack_pbo != 0;
}

// One patch's min/mean/max over RGB, in the texture's own units (8-bit reads
// are normalised to 0..1 so the two cases are comparable; a float target can
// legitimately exceed 1.0 and is left alone). 0 if nothing usable came back.
static int klfb_patch_stats(int32_t x, int32_t y, uint32_t rfmt, uint32_t rtype,
                            float *mn, float *mean, float *mx) {
    static uint8_t buf[KLFB_PATCH * KLFB_PATCH * 4 * 4];   // widest: RGBA float
    int comps = (rfmt == 0x1907 /* RGB */) ? 3 : 4;
    size_t esz = (rtype == 0x1401 /* UNSIGNED_BYTE */) ? 1
               : (rtype == 0x140B /* HALF_FLOAT */)    ? 2
               : (rtype == 0x1406 /* FLOAT */)         ? 4 : 0;
    if (!esz || !a_glReadPixels) return 0;
    size_t need = (size_t)KLFB_PATCH * KLFB_PATCH * (size_t)comps * esz;
    memset(buf, 0, need);
    // Drained first, so an error after the read is provably this read's rather
    // than something the guest left sticky.
    if (a_glGetError) while (a_glGetError()) {}

    if (klfb_pack_pbo_ready()) {
        int32_t save_pack = 0;
        if (a_glGetIntegerv)
            a_glGetIntegerv(0x88ED /* PIXEL_PACK_BUFFER_BINDING */, &save_pack);
        rp_BindBuffer(0x88EB /* PIXEL_PACK_BUFFER */, g_pack_pbo);
        rp_BufferData(0x88EB, (intptr_t)need, NULL, 0x88E1 /* STREAM_READ */);
        // The pointer is an OFFSET into the bound buffer once one is bound.
        a_glReadPixels(x, y, KLFB_PATCH, KLFB_PATCH, rfmt, rtype, (void *)0);
        int bad = a_glGetError && a_glGetError() != 0;
        if (!bad) {
            void *m = rp_MapBufferRange(0x88EB, 0, (intptr_t)need,
                                        0x0001 /* MAP_READ_BIT */);
            if (m) { memcpy(buf, m, need); rp_UnmapBuffer(0x88EB); }
            else     bad = 1;
        }
        rp_BindBuffer(0x88EB, (uint32_t)save_pack);
        if (a_glGetError) while (a_glGetError()) {}
        if (bad) return 0;
    } else {
        // No GLES 3 buffer mapping: host null-driver and GLES2-only runs, where
        // the storage is ANGLE's own and CPU-accessible. The caller refuses
        // this branch for a Metal-backed stage before we ever get here.
        a_glReadPixels(x, y, KLFB_PATCH, KLFB_PATCH, rfmt, rtype, buf);
        if (a_glGetError && a_glGetError()) return 0;
    }

    double sum = 0;
    float lo = 3.4e38f, hi = -3.4e38f;
    int n = 0;
    for (int i = 0; i < KLFB_PATCH * KLFB_PATCH; i++)
        for (int c = 0; c < 3; c++) {
            const uint8_t *p = buf + ((size_t)i * comps + c) * esz;
            float v;
            if (esz == 1)      v = p[0] / 255.0f;
            else if (esz == 2) { uint16_t h; memcpy(&h, p, 2); v = klfb_half_to_float(h); }
            else               memcpy(&v, p, 4);
            sum += v;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            n++;
        }
    if (!n) return 0;
    *mn = lo; *mx = hi; *mean = (float)(sum / n);
    return 1;
}

static unsigned long klfb_probe_fbo(uint32_t fb, float *fbuf, uint8_t *bbuf,
                                    char *note, size_t note_n,
                                    uint8_t *dump, int32_t *dw, int32_t *dh,
                                    int32_t hint_w, int32_t hint_h);

void kl_glfb_probe_stages(int frame) {
    static int every = -1;
    if (every < 0) every = kl_env_int("KL_GLFB_STAGE_PROBE", 0);
    if (every <= 0) return;
    static uint64_t seen;
    if ((seen++ % (uint64_t)every) != 0) return;
    if (!g_ready || !a_glReadPixels || !a_glGetIntegerv) return;
    // A context, on THIS thread. EndFrame has never been observed off the draw
    // thread on either title, but a readback issued without a current context
    // is an abort rather than a bad number, and a diagnostic must not be able
    // to end the run it is diagnosing.
    pthread_once(&g_tls_once, klfb_tls_init);
    klfb_thread_gl *tg = pthread_getspecific(g_tls_key);
    if (!tg || !tg->ctx) return;

    static void (*r_GenFramebuffers)(int32_t, uint32_t *);
    static void (*r_BindFramebuffer)(uint32_t, uint32_t);
    static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t,
                                          int32_t);
    static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
    if (!r_GenFramebuffers) {
        r_GenFramebuffers        = asym("glGenFramebuffers");
        r_BindFramebuffer        = asym("glBindFramebuffer");
        r_FramebufferTexture2D   = asym("glFramebufferTexture2D");
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
    }
    if (!r_GenFramebuffers || !r_BindFramebuffer || !r_FramebufferTexture2D ||
        !r_CheckFramebufferStatus) return;
    static uint32_t probe_fb;
    if (!probe_fb) r_GenFramebuffers(1, &probe_fb);
    if (!probe_fb) return;

    int32_t save_read = 0;
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &save_read);

    for (int e = 0; e < 2; e++)
        for (int s = 0; s < KLFB_MAX_STAGES; s++) {
            uint32_t tex = g_eye_tex_stage[e][s];
            if (!tex) continue;
            // Size from whichever table knows this name: the Metal records on
            // device (the storage came through an EGLImage, so the glTexStorage2D
            // census never saw it) and the census on host.
            int32_t w = 0, h = 0;
            if (g_eye_mtl[e][s].gl_tex == tex && g_eye_mtl[e][s].w > 0) {
                w = g_eye_mtl[e][s].w;
                h = g_eye_mtl[e][s].h;
            } else if (!klfb_tex_info(tex, NULL, &w, &h)) {
                w = h = 0;
            }
            if (w < KLFB_PATCH * 4 || h < KLFB_PATCH * 4) continue;

            // A stage whose storage is one of the compositor's private
            // MTLTextures must never be read without the pack buffer — see
            // klfb_patch_stats. Refusing costs a line in the log; not refusing
            // costs the run, which is what it cost on 2026-08-12.
            if (g_eye_mtl[e][s].gl_tex == tex && g_eye_mtl[e][s].tex &&
                !klfb_pack_pbo_ready()) {
                fprintf(stderr, "  [glfb] stage probe f%d eye=%d stage=%d tex=%u:"
                                " refused — private MTLTexture storage, and no"
                                " pixel pack buffer to read it through\n",
                        frame, e, s, tex);
                continue;
            }

            r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, probe_fb);
            r_FramebufferTexture2D(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                                   0x0DE1 /* TEXTURE_2D */, tex, 0);
            if (r_CheckFramebufferStatus(0x8CA8) != 0x8CD5 /* COMPLETE */) {
                fprintf(stderr, "  [glfb] stage probe f%d eye=%d stage=%d tex=%u:"
                                " incomplete as a read target\n", frame, e, s, tex);
                continue;
            }
            // The read pair the driver will actually accept. An RGBA16F eye
            // texture rejects GL_RGBA/UNSIGNED_BYTE outright, which is how the
            // first FBO census in this file managed to measure a lit frame as
            // black.
            int32_t rfmt = 0, rtype = 0;
            a_glGetIntegerv(0x8B9B /* IMPLEMENTATION_COLOR_READ_FORMAT */, &rfmt);
            a_glGetIntegerv(0x8B9A /* IMPLEMENTATION_COLOR_READ_TYPE */, &rtype);
            // KL_GLFB_STAGE_PAINT=1 — the readback's own control, and it is
            // DESTRUCTIVE: clear this stage to a colour nothing in a scene
            // produces and read it straight back. The eye textures take draws
            // and NO clears, so the guest never hands this probe a known value
            // to check itself against; without one, "BLACK" cannot be told
            // apart from "the read is looking at storage the draws never
            // reached". If the paint reads back, the read path is sound and the
            // draws genuinely produce nothing.
            {
                static int paint = -1;
                static void (*p_col)(float, float, float, float);
                static void (*p_clear)(uint32_t);
                static void (*p_disable)(uint32_t);
                static void (*p_mask)(uint8_t, uint8_t, uint8_t, uint8_t);
                if (paint < 0) {
                    paint     = kl_env_on("KL_GLFB_STAGE_PAINT", 0);
                    p_col     = asym("glClearColor");
                    p_clear   = asym("glClear");
                    p_disable = asym("glDisable");
                    p_mask    = asym("glColorMask");
                }
                if (paint && p_col && p_clear) {
                    r_BindFramebuffer(0x8CA9 /* DRAW_FRAMEBUFFER */, probe_fb);
                    if (p_disable) p_disable(0x0C11 /* SCISSOR_TEST */);
                    if (p_mask) p_mask(1, 1, 1, 1);
                    p_col(0.0f, 1.0f, 0.0f, 1.0f);
                    p_clear(0x4000 /* COLOR_BUFFER_BIT */);
                    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, probe_fb);
                }
            }
            // KL_GLFB_STAGE_PROBE_FULL=1 — count lit pixels over the WHOLE
            // texture instead of trusting two 32x32 patches. A loading screen
            // is a banner and a spinner: 17,000 lit pixels out of 11 million,
            // none of them anywhere near the centre or the quarter point. The
            // patches called that BLACK for a whole round.
            {
                static int full = -1;
                if (full < 0) full = kl_env_on("KL_GLFB_STAGE_PROBE_FULL", 0);
                if (full) {
                    static float *ff; static uint8_t *fb8; static size_t fcap;
                    size_t need = (size_t)w * (size_t)h;
                    if (need > fcap) {
                        free(ff); free(fb8);
                        ff = malloc(need * 16); fb8 = malloc(need * 4);
                        fcap = (ff && fb8) ? need : 0;
                    }
                    if (fcap) {
                        char n2[160] = "";
                        unsigned long lit = klfb_probe_fbo(probe_fb, ff, fb8,
                                                           n2, sizeof n2,
                                                           NULL, NULL, NULL, w, h);
                        fprintf(stderr, "  [glfb] stage probe f%d eye=%d stage=%d "
                                        "tex=%u FULL: %lu of %zu lit (%s)\n",
                                frame, e, s, tex, lit, need, n2);
                        r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, probe_fb);
                    }
                }
            }
            // KL_GLFB_STAGE_PROBE_HALVES=1 — the DoubleWide question, and the
            // one thing a lit-pixel count cannot answer however honest it is.
            // A guest that renders both eyes into one texture puts a DIFFERENT
            // picture in each half. Every failure this round produced — both
            // eyes handed the left half, a split that copied one half over the
            // other, an eye reading a slice nothing wrote — leaves the TOTAL
            // count looking exactly like success. So count each half on its own
            // and compare them PIXEL BY PIXEL: identical halves are one picture
            // duplicated and there is no stereo, however lit they both are.
            //
            // A sampled patch cannot do this job: 157,357 lit of 10,992,000 is
            // 1.4% of the texture, and four patches read 0.0000 through a scene
            // that was plainly there. The whole surface, or nothing.
            {
                static int halves = -1;
                if (halves < 0) halves = kl_env_on("KL_GLFB_STAGE_PROBE_HALVES", 0);
                if (halves && w >= 2) {
                    static uint8_t *hf, *hb, *hd; static size_t hcap;
                    size_t need = (size_t)w * (size_t)h;
                    if (need > hcap) {
                        free(hf); free(hb); free(hd);
                        hf = malloc(need * 16); hb = malloc(need * 4);
                        hd = malloc(need * 4);
                        hcap = (hf && hb && hd) ? need : 0;
                    }
                    if (hcap) {
                        char n3[160] = "";
                        klfb_probe_fbo(probe_fb, (float *)hf, hb, n3, sizeof n3,
                                       hd, NULL, NULL, w, h);
                        int32_t half = w / 2;
                        unsigned long ll = 0, rl = 0, same = 0, cmp = 0;
                        for (int32_t y = 0; y < h; y++) {
                            const uint8_t *row = hd + (size_t)y * w * 4;
                            for (int32_t x = 0; x < half; x++) {
                                const uint8_t *a = row + (size_t)x * 4;
                                const uint8_t *b = row + (size_t)(x + half) * 4;
                                if (a[0] + a[1] + a[2] > 12) ll++;
                                if (b[0] + b[1] + b[2] > 12) rl++;
                                cmp++;
                                if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2])
                                    same++;
                            }
                        }
                        // Two eyes of the same scene agree over flat sky and
                        // disagree everywhere there is parallax, so "identical"
                        // is the verdict and "mostly identical" is not — a real
                        // stereo pair of a mostly-empty menu is 98% equal.
                        // What separates them is whether ANY lit pixel differs.
                        int stereo = (ll || rl) && same < (unsigned long)cmp;
                        fprintf(stderr, "  [glfb] stage probe f%d eye=%d stage=%d "
                                        "tex=%u HALVES %dx%d each: left %lu lit, "
                                        "right %lu lit, %lu of %lu texels "
                                        "identical -> %s\n",
                                frame, e, s, tex, half, h, ll, rl, same, cmp,
                                !ll && !rl ? "BOTH HALVES EMPTY"
                                : !rl ? "RIGHT HALF EMPTY — the right eye has no picture"
                                : !ll ? "LEFT HALF EMPTY — the left eye has no picture"
                                : stereo ? "a stereo pair"
                                : "IDENTICAL — one picture in both halves, no stereo");
                        r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, probe_fb);
                    }
                }
            }
            float cn = 0, cm = 0, cx = 0, qn = 0, qm = 0, qx = 0;
            int okc = klfb_patch_stats(w / 2 - KLFB_PATCH / 2,
                                       h / 2 - KLFB_PATCH / 2,
                                       (uint32_t)rfmt, (uint32_t)rtype,
                                       &cn, &cm, &cx);
            int okq = klfb_patch_stats(w / 4, h / 4,
                                       (uint32_t)rfmt, (uint32_t)rtype,
                                       &qn, &qm, &qx);
            if (!okc && !okq) {
                fprintf(stderr, "  [glfb] stage probe f%d eye=%d stage=%d tex=%u:"
                                " unreadable (read fmt 0x%x type 0x%x)\n",
                        frame, e, s, tex, rfmt, rtype);
                continue;
            }
            // The verdict word is the point of the whole probe: it is what a
            // device log can be grepped for without re-deriving the thresholds
            // from six floats every time.
            const char *verdict = (okc && cx > 0) || (okq && qx > 0)
                                ? "content" : "BLACK";
            fprintf(stderr, "  [glfb] stage probe f%d eye=%d stage=%d tex=%u "
                            "%dx%d centre %.4f/%.4f/%.4f quarter %.4f/%.4f/%.4f "
                            "(min/mean/max, RGB) writes since last: %llu draw / "
                            "%llu clear / %llu blit -> %s\n",
                    frame, e, s, tex, w, h, cn, cm, cx, qn, qm, qx,
                    (unsigned long long)g_stage_draws_w[s],
                    (unsigned long long)g_stage_clears[s],
                    (unsigned long long)g_stage_blits[s], verdict);
        }

    // Reset after the sweep, not inside it: both eyes of a stage report the
    // same counters (the stage is the draw target, the eye is a slice of it),
    // and zeroing on the first eye would make the second read zero always.
    for (int s = 0; s < KLFB_MAX_STAGES; s++) {
        g_stage_draws_w[s] = g_stage_clears[s] = g_stage_blits[s] = 0;
    }

    // ---- the per-FBO census, resolved to what each framebuffer actually IS.
    //
    // Bound as READ so the attachment queries are legal; the whole sweep is
    // already inside the save/restore of READ_FRAMEBUFFER_BINDING below.
    {
        static void (*r_GetFBAttachParam)(uint32_t, uint32_t, uint32_t, int32_t *);
        static void (*r_BindRenderbuffer)(uint32_t, uint32_t);
        static void (*r_GetRenderbufferParam)(uint32_t, uint32_t, int32_t *);
        if (!r_GetFBAttachParam) {
            r_GetFBAttachParam    = asym("glGetFramebufferAttachmentParameteriv");
            r_BindRenderbuffer    = asym("glBindRenderbuffer");
            r_GetRenderbufferParam = asym("glGetRenderbufferParameteriv");
        }
        int32_t save_rb = 0;
        if (a_glGetIntegerv) a_glGetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &save_rb);
        {
            int used = 0;
            for (int i = 0; i < KLFB_FBO_CENSUS; i++) used += g_fbo_census[i].used;
            fprintf(stderr, "  [glfb] fbo census f%d: %llu draws total, %llu of them"
                            " into framebuffers the table had no slot for"
                            " (%d/%d slots used)\n",
                    frame, (unsigned long long)g_draws_all,
                    (unsigned long long)g_draws_uncensused, used, KLFB_FBO_CENSUS);
            g_draws_all = g_draws_uncensused = 0;
        }
        for (int i = 0; i < KLFB_FBO_CENSUS; i++) {
            if (!g_fbo_census[i].used) continue;
            uint64_t d = g_fbo_census[i].draws,  c = g_fbo_census[i].clears,
                     bo = g_fbo_census[i].blit_out, bi = g_fbo_census[i].blit_in;
            g_fbo_census[i].draws = g_fbo_census[i].clears = 0;
            g_fbo_census[i].blit_out = g_fbo_census[i].blit_in = 0;
            if (!d && !c && !bo && !bi) continue;
            uint32_t fb = g_fbo_census[i].fbo;
            char what[96] = "default framebuffer";
            if (fb && r_GetFBAttachParam) {
                r_BindFramebuffer(0x8CA8, fb);
                int32_t otype = 0, oname = 0;
                r_GetFBAttachParam(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                                   0x8CD0 /* OBJECT_TYPE */, &otype);
                r_GetFBAttachParam(0x8CA8, 0x8CE0, 0x8CD1 /* OBJECT_NAME */, &oname);
                if (otype == 0x1702 /* TEXTURE */) {
                    uint32_t f = 0; int32_t w = 0, h = 0;
                    klfb_tex_info((uint32_t)oname, &f, &w, &h);
                    int st = klfb_stage_of_tex((uint32_t)oname);
                    snprintf(what, sizeof what, "tex %d %dx%d fmt 0x%x%s",
                             oname, w, h, f,
                             st >= 0 ? " [EYE STAGE]" : "");
                } else if (otype == 0x8D41 /* RENDERBUFFER */ && r_BindRenderbuffer) {
                    int32_t f = 0, w = 0, h = 0, sm = 0;
                    r_BindRenderbuffer(0x8D41, (uint32_t)oname);
                    r_GetRenderbufferParam(0x8D41, 0x8D44 /* INTERNAL_FORMAT */, &f);
                    r_GetRenderbufferParam(0x8D41, 0x8D42 /* WIDTH */, &w);
                    r_GetRenderbufferParam(0x8D41, 0x8D43 /* HEIGHT */, &h);
                    r_GetRenderbufferParam(0x8D41, 0x8CAB /* SAMPLES */, &sm);
                    snprintf(what, sizeof what, "rb %d %dx%d fmt 0x%x samples %d",
                             oname, w, h, f, sm);
                } else {
                    snprintf(what, sizeof what, "no colour attachment (type 0x%x)",
                             otype);
                }
            }
            fprintf(stderr, "  [glfb] fbo census f%d: fb %-3u %6llu draw %5llu clear "
                            "%5llu blit-in %5llu blit-out  <- %s\n",
                    frame, fb, (unsigned long long)d, (unsigned long long)c,
                    (unsigned long long)bi, (unsigned long long)bo, what);
        }
        if (r_BindRenderbuffer) r_BindRenderbuffer(0x8D41, (uint32_t)save_rb);
    }

    r_BindFramebuffer(0x8CA8, (uint32_t)save_read);
}

// ---------------------------------------------------------------------------
// DoubleWide (kl_glfb.h has the why, and why the split below is NOT the answer).
//
// The guest drew both eyes into eye 0's texture, one half-width viewport each,
// and left eye 1's swapchain untouched. Two ways to serve that:
//
//   ALIAS (the default, and what r13 measured right) — hand eye 1 eye 0's
//   MTLTexture and eye 0's SLICE. Both eyes then read the one texture the guest
//   actually drew, and the compositor's per-eye render viewport (kl_reproject's
//   `grid_per_eye`, built from the two rects ovrp_EndFrame4 filed) crops the
//   right half out of it. No copy, no resample, and — decisively — the grid
//   runs its coordinates through the eye texture's RATE MAP, so it is correct
//   on foveated storage.
//
//   SPLIT (KL_GLFB_DOUBLEWIDE_SPLIT=1, opt-in) — three blits per stage:
//
//     eye0[right half] -> eye1[whole]
//     eye0[left  half] -> scratch[whole]
//     scratch[whole]   -> eye0[whole]
//
//   The detour through the scratch is not caution — glBlitFramebuffer's source
//   and destination rectangles may not overlap when they are the same image.
//
// **The split is wrong on device and r13 measured how.** It works in LOGICAL
// screen coordinates (0..w) on storage that is variable-rate FOVEATED: the
// guest's picture lives squeezed into the physical sub-rect (5208x1624 of a
// 8064x3234 texture on Tetris Effect), so `blit x 4032..8064` reads mostly
// unwritten texels and `blit -> 0..8064` un-squeezes what it does copy. What
// reached the headset was: eye 0 carrying a doubly-magnified, displaced picture
// (the "stretched, blurry, low and left" image), eye 1 carrying a sliver of
// nothing (RIGHT EYE BLACK), and — because the split only runs on the stages in
// the frame's mask — un-split stages compositing correctly in between, which is
// the FLICKER. All three symptoms, one cause. It is kept because a present path
// with no per-eye viewport at all (the host viewer) still has nothing better.
static int g_dw_on;

int kl_glfb_doublewide(void) {
    static int forced = -2;
    if (forced == -2) forced = kl_env_int("KL_GLFB_DOUBLEWIDE", -1);
    return forced >= 0 ? forced : g_dw_on;
}

void kl_glfb_set_doublewide(int on) {
    static int said;
    if (g_dw_on == !!on) return;
    g_dw_on = !!on;
    if (!said++)
        fprintf(stderr, "  [glfb] DoubleWide %s — eye 1 reads eye 0's texture "
                        "and slice, and the compositor's per-eye render "
                        "viewport crops the halves (KL_GLFB_DOUBLEWIDE=0/1 "
                        "overrides; KL_GLFB_DOUBLEWIDE_SPLIT=1 goes back to the "
                        "blit, which is wrong on foveated storage)\n",
                kl_glfb_doublewide() ? "ON" : "off");
}

void kl_glfb_split_doublewide(uint32_t stage_mask) {
    // Opt-in, and no longer implied by the layout bridge. See the note above.
    static int split = -1;
    if (split < 0) split = kl_env_on("KL_GLFB_DOUBLEWIDE_SPLIT", 0);
    int on = split && kl_glfb_doublewide();
    if (!on || !stage_mask || !g_ready || !a_glGetIntegerv) return;

    // Same guard as the stage probe: a blit issued with no current context is
    // an abort, not an error, and this runs on the guest's own render thread.
    pthread_once(&g_tls_once, klfb_tls_init);
    klfb_thread_gl *tg = pthread_getspecific(g_tls_key);
    if (!tg || !tg->ctx) return;

    static void (*b_GenFramebuffers)(int32_t, uint32_t *);
    static void (*b_BindFramebuffer)(uint32_t, uint32_t);
    static void (*b_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t,
                                          int32_t);
    static void (*b_BlitFramebuffer)(int32_t, int32_t, int32_t, int32_t,
                                     int32_t, int32_t, int32_t, int32_t,
                                     uint32_t, uint32_t);
    static void (*b_GenTextures)(int32_t, uint32_t *);
    static void (*b_BindTexture)(uint32_t, uint32_t);
    static void (*b_TexStorage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t);
    static void (*b_Enable)(uint32_t), (*b_Disable)(uint32_t);
    static uint8_t (*b_IsEnabled)(uint32_t);
    static int tried;
    if (!tried) {
        tried = 1;
        b_GenFramebuffers      = asym("glGenFramebuffers");
        b_BindFramebuffer      = asym("glBindFramebuffer");
        b_FramebufferTexture2D = asym("glFramebufferTexture2D");
        b_BlitFramebuffer      = asym("glBlitFramebuffer");
        b_GenTextures          = asym("glGenTextures");
        b_BindTexture          = asym("glBindTexture");
        b_TexStorage2D         = asym("glTexStorage2D");
        b_Enable               = asym("glEnable");
        b_Disable              = asym("glDisable");
        b_IsEnabled            = asym("glIsEnabled");
    }
    if (!b_BlitFramebuffer || !b_GenFramebuffers || !b_BindFramebuffer ||
        !b_FramebufferTexture2D || !b_GenTextures || !b_TexStorage2D) return;

    static uint32_t src_fb, dst_fb, scratch, scratch_fmt;
    static int32_t scratch_w, scratch_h;
    if (!src_fb) { b_GenFramebuffers(1, &src_fb); b_GenFramebuffers(1, &dst_fb); }
    if (!src_fb || !dst_fb) return;

    int32_t save_read = 0, save_draw = 0;
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &save_read);
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &save_draw);
    // A blit is clipped by the scissor box, and the guest leaves the test on
    // with the last eye's half in it — which would make this copy exactly the
    // region it is trying to move.
    int sc = b_IsEnabled ? b_IsEnabled(0x0C11 /* SCISSOR_TEST */) : 0;
    if (sc && b_Disable) b_Disable(0x0C11);

    for (int s = 0; s < KLFB_MAX_STAGES; s++) {
        if (!(stage_mask & (1u << s))) continue;
        uint32_t s0 = g_eye_tex_stage[0][s], s1 = g_eye_tex_stage[1][s];
        if (!s0 || !s1 || s0 == s1) continue;
        // Size from whichever table knows this name — the Metal records on
        // device, where the storage came through an EGLImage and the
        // glTexStorage2D census never saw it, and the census on host.
        uint32_t fmt = 0; int32_t w = 0, h = 0;
        if (g_eye_mtl[0][s].gl_tex == s0 && g_eye_mtl[0][s].w > 0) {
            w = g_eye_mtl[0][s].w; h = g_eye_mtl[0][s].h;
            klfb_tex_info(s0, &fmt, NULL, NULL);
        } else if (!klfb_tex_info(s0, &fmt, &w, &h)) {
            continue;
        }
        if (w < 2 || h < 1) continue;
        int32_t half = w / 2;

        // eye0's right half -> the whole of eye 1.
        b_BindFramebuffer(0x8CA8 /* READ */, src_fb);
        b_FramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, s0, 0);
        b_BindFramebuffer(0x8CA9 /* DRAW */, dst_fb);
        b_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, s1, 0);
        b_BlitFramebuffer(half, 0, w, h, 0, 0, w, h,
                          0x4000 /* COLOR_BUFFER_BIT */, 0x2601 /* LINEAR */);

        // eye0's left half -> scratch -> the whole of eye 0. Two hops because
        // the one hop would be a self-blit with overlapping rectangles.
        if (!scratch || scratch_w != w || scratch_h != h || scratch_fmt != fmt) {
            if (scratch) { scratch = 0; }   // names are cheap; the old one leaks
                                            // only if the eye size changes, which
                                            // happens once at most.
            b_GenTextures(1, &scratch);
            if (!scratch) continue;
            if (b_BindTexture) b_BindTexture(0x0DE1, scratch);
            b_TexStorage2D(0x0DE1, 1, fmt ? fmt : 0x8C43 /* SRGB8_ALPHA8 */, w, h);
            scratch_w = w; scratch_h = h; scratch_fmt = fmt;
        }
        b_BindFramebuffer(0x8CA9, dst_fb);
        b_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, scratch, 0);
        b_BlitFramebuffer(0, 0, half, h, 0, 0, w, h, 0x4000, 0x2601);

        b_BindFramebuffer(0x8CA8, src_fb);
        b_FramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1, scratch, 0);
        b_BindFramebuffer(0x8CA9, dst_fb);
        b_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, s0, 0);
        b_BlitFramebuffer(0, 0, w, h, 0, 0, w, h, 0x4000, 0x2601);

        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] DoubleWide split: stage %d, eye texture "
                            "%dx%d, halves at 0..%d and %d..%d blitted out to "
                            "GL %u and %u\n",
                    s, w, h, half, half, w, s0, s1);
    }

    if (sc && b_Enable) b_Enable(0x0C11);
    b_BindFramebuffer(0x8CA8, (uint32_t)save_read);
    b_BindFramebuffer(0x8CA9, (uint32_t)save_draw);
}

static int mtl_resolve(void) {
    if (a_eglCreateImageKHR) return 1;
    if (!g_ready) return 0;
    a_eglCreateImageKHR        = asym("eglCreateImageKHR");
    a_eglDestroyImageKHR       = asym("eglDestroyImageKHR");
    a_eglQueryDisplayAttribEXT = asym("eglQueryDisplayAttribEXT");
    a_eglQueryDeviceAttribEXT  = asym("eglQueryDeviceAttribEXT");
    a_glEGLImageTargetTexture2DOES = asym("glEGLImageTargetTexture2DOES");
    a_glBindTexture_mtl        = asym("glBindTexture");
    // Ours, from angle-patches/klepton.patch. Deliberately NOT in the return
    // below: an ANGLE without them can still do the interop, it just cannot
    // foveate, and failing the whole Metal path over that would trade a
    // performance feature for the picture.
    a_SetRateMap        = asym("ANGLEMetalSetRasterizationRateMap");
    a_SetRateMapForSize = asym("ANGLEMetalSetRasterizationRateMapForSize");
    return a_eglCreateImageKHR && a_glEGLImageTargetTexture2DOES
        && a_eglQueryDeviceAttribEXT && a_glBindTexture_mtl;
}

void *kl_glfb_mtl_device(void) {
    static void *dev;
    static int tried;
    if (tried) return dev;
    tried = 1;
    if (!kl_glfb_init() || !mtl_resolve()) return NULL;
    intptr_t egl_dev = 0, mtl = 0;
    if (!a_eglQueryDisplayAttribEXT ||
        !a_eglQueryDisplayAttribEXT(g_dpy, EGL_DEVICE_EXT_, &egl_dev) || !egl_dev) {
        fprintf(stderr, "  [glfb] eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) failed — "
                        "no MTLDevice to share\n");
        return NULL;
    }
    if (!a_eglQueryDeviceAttribEXT((void *)egl_dev, EGL_METAL_DEVICE_ANGLE_, &mtl) || !mtl) {
        fprintf(stderr, "  [glfb] eglQueryDeviceAttribEXT(EGL_METAL_DEVICE_ANGLE) failed — "
                        "is this the Metal backend?\n");
        return NULL;
    }
    dev = (void *)mtl;
    // KL_PERF=1 on a GL guest: kl_vulkan installs the GPU clock on MoltenVK's
    // device, and nothing did on ANGLE's, so a GLES/ltw guest's [perf] line had
    // no `gpu … busy` — the one number that says whether a frame is GPU-bound
    // (resolution, foveation) or CPU-bound (Vivecraft's render thread). Every
    // command buffer lands as "other" (ANGLE does not tag); busy% is the union.
    // A no-op unless KL_PERF=1, and once per process.
    kl_perf_gpu_install(dev);
    return dev;
}

void *kl_glfb_eye_rate_map(void) { return g_eye_rate_map; }
static int g_eye_mirroring;
static int klfb_vrr_mirror_on(void);
int kl_glfb_eye_mirror_foveated(void) {
    return g_eye_mirroring && g_eye_rate_map && klfb_vrr_mirror_on();
}

void kl_glfb_eye_rate_zones(int *zones_x, int *zones_y) {
    if (zones_x) *zones_x = g_eye_rate_zx;
    if (zones_y) *zones_y = g_eye_rate_zy;
}

// The policy, not the object. Both builders read it from here so a device run
// foveates exactly as the host measurement did — see kl_glfb.h.
// ON by default since 2026-08-09, once the device leg landed: every path that
// samples an eye texture as a picture rather than as storage now unwarps it
// (both compositors, through kl_reproject's grid). What does NOT is the
// diagnostic readback — KL_GLFB_OUT and t_mtl_provider's lit-pixel count — and
// those show the squeeze on purpose, as the cheapest confirmation it engaged.
// KL_VRR=0 is the A/B.
int kl_glfb_foveation_wanted(int *zones, float *edge) {
    if (!kl_env_on("KL_VRR", 1)) return 0;
    int z = kl_env_int("KL_VRR_ZONES", 16);
    if (z < 1) z = 1;
    if (z > KL_FOVEATION_MAX_ZONES) z = KL_FOVEATION_MAX_ZONES;
    float e = kl_env_float("KL_VRR_EDGE", 0.35f);
    // A rate map's quality is a fraction of full rate; 0 would ask Metal for
    // zero-area zones, and >1 is not a thing. Out of range means the knob was
    // fat-fingered, so fall back rather than clamp to a silently different
    // picture.
    if (!(e > 0.05f) || !(e <= 1.0f)) e = 0.35f;
    if (zones) *zones = z;
    if (edge)  *edge  = e;
    return 1;
}

void kl_glfb_foveation_quality(float *q, int n, float edge) {
    for (int i = 0; i < n; i++) {
        // The zone centre in [-1, 1], so the curve is symmetric about the
        // middle of the screen — where the fovea is, absent eye tracking we
        // are not given.
        float t = n > 1 ? (2.f * ((float)i + 0.5f) / (float)n - 1.f) : 0.f;
        if (t < 0) t = -t;
        q[i] = 1.f - (1.f - edge) * t;
    }
}

// Set once the array-mirror path (kl_glfb_mirror_eye_layer) owns an eye. See
// kl_glfb_set_eye_rate_map for why it forbids foveation.
static int g_eye_mirroring;

// KL_VRR_MIRROR=1: foveate THROUGH the array-swapchain copy (vivecraft 26.x).
//
// The refusal below is right for a guest whose swapchain carries no map — the
// copy would be the only foveated surface. It is wrong for a guest that renders
// its whole eye chain at the eye size and single-sampled, under
// KL_VRR_MIN_SAMPLES=1: then the size rule covers every link — the scene target,
// the post pass's destination, the swapchain LAYER the guest blits into (the
// rule is keyed on the texture's size, not its type), and our mirror
// destination. Every surface holds the picture in the same physical layout, the
// unscaled whole-surface blits between them are physical-to-physical (ANGLE's
// passthrough: same map object on both sides), the post shader reads at the
// fragment's own physical position, and the compositor unwarps the mirror as it
// unwarps a re-pointed eye. Vivecraft 26.2 moved to ONE array swapchain
// (arraySize 2) and lost foveation with it: launch 9 rendered 2 x 5088x4081
// unfoveated, GPU busy ~18 ms a frame against launch 7's ~9, 38 fps in-world.
// Any link that misses the map degrades to a raw texel copy (passthrough into
// an unfoveated pass, or the rewritten post shader in one), which keeps the
// physical layout too — so the failure is not a double warp.
//
// Opt-in per row; 0 is the A/B (launch 9's unfoveated path).
static int klfb_vrr_mirror_on(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_VRR_MIRROR", 0) && klfb_rate_min_samples() <= 1;
        if (kl_env_on("KL_VRR_MIRROR", 0) && !on)
            fprintf(stderr, "  [glfb] KL_VRR_MIRROR=1 ignored: it needs "
                            "KL_VRR_MIN_SAMPLES=1, or the guest's single-sampled "
                            "swapchain carries no map and only the copy would\n");
    }
    return on;
}

void kl_glfb_set_eye_rate_map(int w, int h, int zones_x, int zones_y, void *rate_map) {
    if (!kl_glfb_init() || !mtl_resolve()) return;
    // Not available on the copy path: there is nothing there to save.
    // Foveation is a bargain with the GUEST's rasterizer: it
    // writes fewer fragments, and the compositor's grid unwarps what it wrote.
    // On the array-mirror path the guest rasterizes into a swapchain of its own
    // that carries no map (klxr_CreateSwapchain gives it plain glTexStorage3D
    // storage), and the only thing a map could attach to is the COPY — so the
    // full-resolution picture would be squeezed on the way in and stretched back
    // out on the way to the display, paying twice for a saving nobody made.
    //
    // And if the blit does not rasterize at all, the destination holds an
    // unwarped picture the compositor unwarps anyway: a magnified centre in a
    // correct-looking frame, which is the failure this file has no instrument
    // for. Refused rather than left to
    // the frontends, because both of them build a map from the display and
    // neither can know which guest is underneath.
    if (g_eye_mirroring && rate_map && klfb_vrr_mirror_on()) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] vrr mirror: foveating THROUGH the array-swapchain "
                            "copy (KL_VRR_MIRROR=1) — the guest's eye-sized targets, its "
                            "swapchain layers and our mirror all take the one size-rule "
                            "map; the copies are physical-to-physical and the compositor "
                            "unwarps the mirror (KL_VRR_MIRROR=0 is the A/B)\n");
    } else if (g_eye_mirroring && rate_map) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] foveation is not available for this guest: its "
                            "eyes reach the compositor by COPY (an array swapchain), "
                            "and a rate map on the copy costs resolution and saves "
                            "nothing — the map is dropped\n");
        rate_map = NULL;
    }
    if (!a_SetRateMapForSize || !a_SetRateMap) {
        fprintf(stderr, "  [glfb] this ANGLE has no rasterization-rate entry points — "
                        "foveation is unavailable (is it the patched build?)\n");
        return;
    }
    // Retire the previous size rule before installing another: the eye size
    // changes under us when Unity re-creates its swapchain, and a rule left
    // behind on the old size would foveate a target nothing unwarps.
    unsigned min_samples = klfb_rate_min_samples();
    if (min_samples != KL_RATE_MIN_SAMPLES) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] KL_VRR_MIN_SAMPLES=%u — render targets of the "
                            "eye size with >= %u sample(s) take the rate map (the "
                            "default %d keeps it to multisampled scene targets)%s\n",
                    min_samples, min_samples, KL_RATE_MIN_SAMPLES,
                    klfb_vrr_post_wanted()
                      ? "; the VR post shader reads its source at the physical "
                        "position (KL_VRR_POST)" : "");
    }
    void *old_map = g_eye_rate_map;
    int old_w = g_eye_rate_w, old_h = g_eye_rate_h;
    if (g_eye_rate_w && (g_eye_rate_w != w || g_eye_rate_h != h || !rate_map))
        a_SetRateMapForSize((uint32_t)g_eye_rate_w, (uint32_t)g_eye_rate_h,
                            min_samples, NULL);

    g_eye_rate_map = rate_map;
    g_eye_rate_w   = rate_map ? w : 0;
    g_eye_rate_h   = rate_map ? h : 0;
    g_eye_rate_zx  = rate_map ? zones_x : 0;
    g_eye_rate_zy  = rate_map ? zones_y : 0;

    if (rate_map)
        a_SetRateMapForSize((uint32_t)w, (uint32_t)h, min_samples, rate_map);

    // Every FBO ANGLE has already synced cached its old answer — see
    // klfb_vrr_touch. A changed rule (new map, new size, or retired) is a new
    // generation, and each FBO re-syncs on its next bind.
    if (rate_map != old_map || g_eye_rate_w != old_w || g_eye_rate_h != old_h) {
        g_vrr_rule_w   = g_eye_rate_w;
        g_vrr_rule_h   = g_eye_rate_h;
        g_vrr_rule_min = min_samples;
        g_vrr_rule_map = rate_map;
        __atomic_add_fetch(&g_vrr_gen, 1, __ATOMIC_RELEASE);
    }

    // Eye textures already bound do not come back through the bind path, so
    // re-register (or clear) each of them here — but only those the map was
    // actually built for. A frontend that re-drives this on a size change (both
    // of ours do, keyed on size) can still be holding textures of the OLD size
    // at this moment, and handing them the new map would foveate them against a
    // screen size they do not have. Clearing is always safe, so a mismatch
    // clears rather than skips.
    for (int e = 0; e < 2; e++)
        for (int s = 0; s < KL_MTL_MAX_STAGES; s++) {
            if (!g_eye_mtl[e][s].tex) continue;
            int fits = rate_map && g_eye_mtl[e][s].w == w && g_eye_mtl[e][s].h == h;
            a_SetRateMap(g_eye_mtl[e][s].tex, fits ? rate_map : NULL);
            if (rate_map && !fits)
                fprintf(stderr, "  [glfb] eye=%d stage=%d is %dx%d, not the %dx%d this "
                                "rate map was built for — left unfoveated\n",
                        e, s, g_eye_mtl[e][s].w, g_eye_mtl[e][s].h, w, h);
        }

    fprintf(stderr, "  [glfb] eye rate map %p for %dx%d, %dx%d zones (targets of "
                    "that size with >= %u sample(s), plus every bound eye texture; "
                    "rule #%u)\n",
            rate_map, w, h, zones_x, zones_y, min_samples, g_vrr_gen);
}

void *kl_glfb_eye_mtl_texture(int eye, int stage, int *out_slice) {
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return NULL;
    // DoubleWide: the guest put BOTH eyes in eye 0's texture and never bound a
    // framebuffer to eye 1's, so eye 1's storage is black — literally never
    // written, which is what "right eye renders nothing" was. Hand back eye 0's
    // texture AND eye 0's slice; the two eyes' render viewports differ, so the
    // compositor builds a per-eye unwarp block and each eye crops its own half
    // out of the one picture. Slice matters as much as the texture: the pair is
    // ONE 2-slice array, and eye 1 left on slice 1 reads the untouched slice.
    //
    // Only when eye 0 really has storage — during the gap between a teardown
    // and the next SetupEyeTexture2 the honest answer is still "nothing here".
    if (eye == 1 && kl_glfb_doublewide() && g_eye_mtl[0][stage].tex) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] DoubleWide: eye 1 stage %d is served eye "
                            "0's MTLTexture %p slice %d — the guest never drew "
                            "into eye 1's own storage\n",
                    stage, g_eye_mtl[0][stage].tex, g_eye_mtl[0][stage].slice);
        eye = 0;
    }
    if (out_slice) *out_slice = g_eye_mtl[eye][stage].slice;
    return g_eye_mtl[eye][stage].tex;
}

// The Vulkan path's way into the same table — see kl_glfb.h. There is no
// EGLImage and no GL name on that path, so those two fields stay zero and
// kl_glfb_release_eye_texture has nothing of its own to drop; the VkImage is
// kl_vulkan.c's to free.
void kl_glfb_note_eye_mtl_texture(int eye, int stage, void *texture, int slice,
                                  int w, int h) {
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return;
    if (g_eye_mtl[eye][stage].tex == texture &&
        g_eye_mtl[eye][stage].slice == slice) return;
    g_eye_mtl[eye][stage].tex   = texture;
    g_eye_mtl[eye][stage].slice = slice;
    g_eye_mtl[eye][stage].image = NULL;
    g_eye_mtl[eye][stage].gl_tex = 0;
    g_eye_mtl[eye][stage].w = w;
    g_eye_mtl[eye][stage].h = h;
    // Vulkan's framebuffer origin is the top left, like Metal's and unlike GL's.
    g_eye_mtl[eye][stage].top_left = 1;
    fprintf(stderr, "  [glfb] eye %d stage %d is MTLTexture %p slice %d (%dx%d), "
                    "from Vulkan — top-left origin, the compositor can sample it\n",
            eye, stage, texture, slice, w, h);
}

uint32_t kl_glfb_eye_gl_texture(int eye, int stage) {
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return 0;
    return g_eye_mtl[eye][stage].gl_tex;
}

int kl_glfb_eye_mtl_origin_top_left(int eye, int stage) {
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return 0;
    return g_eye_mtl[eye][stage].top_left;
}

// The teardown half, called from ovrp_DestroyEyeTexture — the only thing in the
// trace that ever says an eye texture is finished with. Two references have to
// go for the storage to be reclaimed, and dropping either one alone reclaims
// nothing:
//
//   - the EGLImage, which is ANGLE's handle on the MTLTexture, and
//   - the GL texture, which took a reference of its own at
//     glEGLImageTargetTexture2DOES and keeps it until the name is deleted.
//
// The provider's own reference then goes when it replaces its cache entry, and
// only at that point does the memory actually come back. That is why the leak
// looked like the provider's: it *was* releasing on reallocation, into a
// texture ANGLE still held.
//
// The GL delete needs a current context, and this call arrives on the guest's
// render thread inside its graphics teardown, where there is one. If there
// isn't, the delete is a silent GL no-op — so it is reported rather than
// assumed.
void kl_glfb_release_eye_texture(int eye, int stage) {
    // KL_EYE_RELEASE=0 restores the old behaviour — the teardown call arrives
    // and nothing is released. The A/B is worth a knob: this is the difference
    // between a swapchain per loading transition and one swapchain, and the
    // cost of being wrong about the ownership is a texture the guest still
    // wants. Compare `frames per stage` in the OVRPlugin report either way.
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_EYE_RELEASE", 1);
    if (!on) return;
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) {
        fprintf(stderr, "  [glfb] release eye=%d stage=%d: out of range, "
                        "nothing released\n", eye, stage);
        return;
    }
    uint32_t gl_tex = g_eye_mtl[eye][stage].gl_tex;
    void    *img    = g_eye_mtl[eye][stage].image;
    if (!gl_tex && !img) return;                 // never bound, or already gone

    // Unbind the map first. The registry RETAINS the texture it is keyed on, so
    // a binding left behind here outlives the storage it describes — and worse,
    // the address can be recycled into an unrelated texture that would then be
    // silently foveated (mtl_common.mm says why it retains).
    if (g_eye_mtl[eye][stage].tex && a_SetRateMap)
        a_SetRateMap(g_eye_mtl[eye][stage].tex, NULL);
    if (img && a_eglDestroyImageKHR) a_eglDestroyImageKHR(g_dpy, img);
    if (gl_tex) {
        static void (*r_DeleteTextures)(int32_t, const uint32_t *);
        static void *(*r_GetCurrentContext)(void);
        if (!r_DeleteTextures) {
            r_DeleteTextures = asym("glDeleteTextures");
            r_GetCurrentContext = asym("eglGetCurrentContext");
        }
        if (r_GetCurrentContext && !r_GetCurrentContext())
            fprintf(stderr, "  [glfb] release eye=%d stage=%d: no current context, "
                            "the GL texture %u keeps its storage\n",
                    eye, stage, gl_tex);
        if (r_DeleteTextures) r_DeleteTextures(1, &gl_tex);
        klfb_vram_forget(KLC_TEX, gl_tex);
        klfb_census_killed(KLC_TEX, 1);
    }
    g_eye_mtl[eye][stage] = (typeof(g_eye_mtl[0][0])){0};
    // ...and the telemetry that named it — but ONLY if the slot still names the
    // texture being released. See klfb_forget_eye_stage: the "replaced with no
    // teardown" backstop above calls in here AFTER the new name has already
    // been registered, and clearing unconditionally is how r11 and r12 lost
    // every frame they drew.
    klfb_forget_eye_stage(eye, stage, gl_tex);
    fprintf(stderr, "  [glfb] eye=%d stage=%d released (GL texture %u, image %p)\n",
            eye, stage, gl_tex, img);
}

// ---------------------------------------------------------------------------
// GL_FRAMEBUFFER_SRGB, and why a cap we do not have is worth intercepting.
//
// `EXT_sRGB_write_control` lets an app turn the linear->sRGB encode off for a
// framebuffer whose attachment is an sRGB format — i.e. say "the values I am
// writing are ALREADY sRGB code values, store them as they are". Steam Link
// uses it exactly that way: it disables the encode, renders its decoded video
// into an SRGB8_ALPHA8 swapchain, and re-enables it after
// (`XRConstruct::RenderFrame`, `QSVLRendererXR::FlipFrame` — 883 of each per
// run). A Quest has the extension. ANGLE does not expose it, so both calls
// raised INVALID_ENUM and ES applied the encode anyway.
//
// **The error was never the problem; the encode was.** The stored byte becomes
// `encode(V)` where the guest wrote `V`, and sampling an sRGB texture decodes
// once — so the composite receives `V`, an sRGB code value, and treats it as
// linear. That is the picture reading too bright, and it has no error surface
// at all: every call after the two INVALID_ENUMs succeeds and the frame is
// perfectly well-formed. "An encode on write and a decode on sample cancel, so
// this is precision rather than gamma" is true about the pair and wrong about
// the conclusion: what they cancel back to is the guest's sRGB code value, and
// the composite needs
// linear.
//
// So the state is RECORDED rather than forwarded. Swallowing it also removes
// 1766 GL errors a run from a log where an unexplained error is a lead.
//
// Sticky, and deliberately: this asks "does this guest render sRGB code values
// into its eye texture", which is a property of the guest, not of the instant.
// The composite runs on another thread entirely and sampling a live GL enable
// from it would be a race with no right answer.
#define KLFB_GL_FRAMEBUFFER_SRGB 0x8DB9
static int g_srgb_write_off;        // the guest asked for the encode OFF
static int g_eye_fmt_is_srgb;       // ...and an eye texture is an sRGB format

// Both halves have to hold, and neither is knowable without the other: an eye
// texture that is not sRGB takes no encode to undo (Beat Saber's is RGBA16F),
// and a guest that never disables the encode meant the one it got.
static void klfb_srgb_settle(void) {
    kl_reproject_set_srgb_decode(g_srgb_write_off && g_eye_fmt_is_srgb);
}

int kl_glfb_bind_eye_mtl_texture(int eye, int stage, uint32_t gl_tex,
                                 int w, int h, uint32_t internal_fmt) {
    if (!g_mtl_provider || eye < 0 || eye > 1 || !gl_tex) return 0;
    if (stage < 0 || stage >= KL_MTL_MAX_STAGES) {
        fprintf(stderr, "  [glfb] eye stage %d is beyond KL_MTL_MAX_STAGES (%d)\n",
                stage, KL_MTL_MAX_STAGES);
        return 0;
    }
    if (!kl_glfb_init() || !mtl_resolve()) {
        fprintf(stderr, "  [glfb] a Metal texture provider is registered but ANGLE's "
                        "interop entry points are missing — falling back to GL storage\n");
        return 0;
    }
    kl_mtl_eye_texture t = { NULL, 0, 0, 0 };
    if (!g_mtl_provider(eye, stage, w, h, internal_fmt, &t, g_mtl_provider_ctx) || !t.texture) {
        fprintf(stderr, "  [glfb] provider declined eye=%d stage=%d %dx%d\n",
                eye, stage, w, h);
        return 0;
    }
    // The size check the extension will not do for us. eglCreateImageKHR takes
    // the dimensions from the MTLTexture and succeeds regardless of what we ask
    // for, so without this a wrong-sized texture produces a guest rendering into
    // storage smaller than it believes it has — and nothing reports it. Refuse
    // instead: ordinary GL storage of the right size is wrong in a way that
    // shows up immediately, which is strictly better than being subtly wrong.
    if (t.w != w || t.h != h) {
        fprintf(stderr, "  [glfb] provider returned a %dx%d texture for a %dx%d eye "
                        "(eye=%d stage=%d) — refusing; the guest would render into "
                        "storage it does not have\n", t.w, t.h, w, h, eye, stage);
        return 0;
    }
    const int32_t attrs[] = {
        EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE_, t.slice,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE_,   (int32_t)internal_fmt,
        EGL_NONE_,
    };
    void *img = a_eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT_, EGL_METAL_TEXTURE_ANGLE_,
                                   t.texture, attrs);
    if (img == EGL_NO_IMAGE_) {
        // Most likely cause by far: the texture was not allocated on ANGLE's own
        // MTLDevice, which the extension requires. Named because nothing else
        // about the failure points at it.
        fprintf(stderr, "  [glfb] eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE) failed for "
                        "eye=%d stage=%d — was the texture made on "
                        "kl_glfb_mtl_device()?\n", eye, stage);
        return 0;
    }
    a_glBindTexture_mtl(0x0DE1 /* GL_TEXTURE_2D */, gl_tex);
    while (a_glGetError && a_glGetError() != 0) {}
    a_glEGLImageTargetTexture2DOES(0x0DE1, img);
    uint32_t e = a_glGetError ? a_glGetError() : 0;
    if (e) {
        fprintf(stderr, "  [glfb] glEGLImageTargetTexture2DOES -> GL error 0x%x "
                        "(eye=%d stage=%d)\n", e, eye, stage);
        if (a_eglDestroyImageKHR) a_eglDestroyImageKHR(g_dpy, img);
        return 0;
    }
    // A backstop, and in this title an unexercised one: every measured
    // replacement is preceded by ovrp_DestroyEyeTexture, so this slot is empty
    // by the time a new texture arrives (24 binds, 18 teardowns — the six
    // without a teardown are the first generation, which has no predecessor).
    // It exists because the guest never calls glDeleteTextures on these names
    // itself: if a teardown ever goes missing, destroying only the EGLImage
    // leaves ANGLE's texture holding the MTLTexture and the provider's release
    // reclaims nothing. It says so when it fires — a silent backstop is how a
    // leak comes back.
    //
    // The same name re-bound to new storage is not a replacement, so it falls
    // through to the image destroy alone.
    if (g_eye_mtl[eye][stage].gl_tex && g_eye_mtl[eye][stage].gl_tex != gl_tex) {
        fprintf(stderr, "  [glfb] eye=%d stage=%d replaced with no teardown "
                        "(GL texture %u -> %u) — releasing the old one here\n",
                eye, stage, g_eye_mtl[eye][stage].gl_tex, gl_tex);
        kl_glfb_release_eye_texture(eye, stage);   // no-op under KL_EYE_RELEASE=0
    }
    if (g_eye_mtl[eye][stage].image && a_eglDestroyImageKHR)
        a_eglDestroyImageKHR(g_dpy, g_eye_mtl[eye][stage].image);
    g_eye_mtl[eye][stage].tex    = t.texture;
    g_eye_mtl[eye][stage].slice  = t.slice;
    g_eye_mtl[eye][stage].image  = img;
    g_eye_mtl[eye][stage].gl_tex = gl_tex;
    g_eye_mtl[eye][stage].w      = w;
    g_eye_mtl[eye][stage].h      = h;
    // ...and record the storage, exactly as the glTexStorage2D thunk would have
    // if this texture had got ordinary GL storage. Nothing else can: an
    // EGLImage-backed texture never passes through that thunk, and ES 3.0 has
    // no glGetTexLevelParameteriv to ask with. Without it the capture finds the
    // eye FBO by attachment and then reads it at the PBUFFER's size, which on
    // this host is 4000x3200 against a 2748x2880 eye — a black PNG on the one
    // path that has the picture, with the eye plainly attached.
    klfb_note_tex_storage(gl_tex, internal_fmt, w, h);
    // Foveation, if a map is in force AND it was built for this size. Before the
    // guest renders into this texture, not after: ANGLE caches a framebuffer's
    // render pass descriptor and only rebuilds it on a GL state sync, so a map
    // bound afterwards misses the first pass with nothing reporting it
    //.
    //
    // The size test is the invariant, held HERE rather than trusted to the
    // frontend. Both of ours re-drive kl_glfb_set_eye_rate_map from the provider
    // before this runs, so in practice the map is already the right one — but
    // the ordering is a convention, and the failure if it is ever broken is a
    // wrong picture with no error anywhere. Beat Saber 1.6.0 is what makes this
    // reachable: it re-creates its eye textures mid-run at a different size
    // (2400x2290 -> 2880x2748 -> back), where 2019.4 picks one and keeps it.
    if (g_eye_rate_map && a_SetRateMap) {
        if (w == g_eye_rate_w && h == g_eye_rate_h) {
            a_SetRateMap(t.texture, g_eye_rate_map);
        } else {
            fprintf(stderr, "  [glfb] eye=%d stage=%d is %dx%d but the rate map is for "
                            "%dx%d — left unfoveated (the frontend has not re-driven "
                            "kl_glfb_set_eye_rate_map for the new size)\n",
                    eye, stage, w, h, g_eye_rate_w, g_eye_rate_h);
        }
    }
    // The other half of the sRGB question (klfb_srgb_settle). The format is not
    // ours to choose — it is whatever the guest asked its swapchain for — so
    // this is where it becomes known, and it can arrive either side of the
    // guest's first glDisable.
    if (internal_fmt == GL_SRGB8_ALPHA8) g_eye_fmt_is_srgb = 1;
    klfb_srgb_settle();
    fprintf(stderr, "  [glfb] eye=%d stage=%d tex=%u is now backed by MTLTexture %p "
                    "slice %d (%dx%d fmt 0x%x)\n",
            eye, stage, gl_tex, t.texture, t.slice, w, h, internal_fmt);
    // The eye textures take their storage through the EGLImage, not through
    // glTexStorage*, so the census would otherwise be blind to the single
    // largest allocation in the process. One eye is one slice of the shared
    // array, so the per-name figure is w*h*bpp and the pair adds up to the
    // whole texture. glDeleteTextures on the old name is what removes it —
    // which makes this the measurement of whether the guest lets go of the old
    // swapchain at all.
    klfb_vram_note(KLC_TEX, gl_tex, klfb_storage_bytes(internal_fmt, w, h, 1, 1));
    // Unity re-creates the eye swapchain at every loading transition, which is
    // exactly the event a leak hunt wants a census either side of. Rare enough
    // to print unconditionally when the census is on.
    if (klfb_census_every()) kl_glfb_gl_census(stderr);
    return 1;
}

// ---------------------------------------------------------------------------
// The NON-EYE layer's storage — see kl_glfb.h. Everything here is the eye path
// above with the key widened and the eye-specific parts left out: no rate map
// (foveation is a property of the eye render, and a UI panel is not rendered
// through one), no sRGB settle (that decides how the EYE picture is decoded),
// and no vram census key of its own.
#define KL_MTL_MAX_LAYERS 4
static kl_glfb_mtl_layer_provider g_mtl_layer_provider;
static void *g_mtl_layer_provider_ctx;
// Keyed rather than indexed: a layer id is the caller's own (kl_openxr uses the
// swapchain's slot, which is 0..63), so the row carries the key it was filed
// under. `layer` is -1 in a free row, which is a valid id nowhere.
static struct { int layer, stage; void *tex; int slice; void *image;
                uint32_t gl_tex; int w, h; }
    g_layer_mtl[KL_MTL_MAX_LAYERS * KL_MTL_MAX_STAGES];
static int g_layer_mtl_init;

static void klfb_layer_rows_init(void) {
    if (g_layer_mtl_init) return;
    g_layer_mtl_init = 1;
    for (unsigned i = 0; i < sizeof g_layer_mtl / sizeof *g_layer_mtl; i++)
        g_layer_mtl[i].layer = -1;
}

// The row for (layer, stage), or the first free one when `make_room` is set.
static int klfb_layer_row(int layer, int stage, int make_room) {
    klfb_layer_rows_init();
    int free_row = -1;
    for (unsigned i = 0; i < sizeof g_layer_mtl / sizeof *g_layer_mtl; i++) {
        if (g_layer_mtl[i].layer == layer && g_layer_mtl[i].stage == stage)
            return (int)i;
        if (g_layer_mtl[i].layer < 0 && free_row < 0) free_row = (int)i;
    }
    return make_room ? free_row : -1;
}

void kl_glfb_set_mtl_layer_provider(kl_glfb_mtl_layer_provider fn, void *ctx) {
    g_mtl_layer_provider = fn;
    g_mtl_layer_provider_ctx = ctx;
}
int kl_glfb_has_mtl_layer_provider(void) { return g_mtl_layer_provider != NULL; }

// The GL a blit needs, resolved once. Split out because BOTH mirrors use it —
// the array eye's and the non-eye layer's — and two copies of an entry-point
// table is two chances for one of them to be missing a null check.
static void (*r_GenFramebuffers)(int32_t, uint32_t *);
static void (*r_BindFramebuffer)(uint32_t, uint32_t);
static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
static void (*r_FramebufferTextureLayer)(uint32_t, uint32_t, uint32_t, int32_t, int32_t);
static void (*r_BlitFramebuffer)(int32_t, int32_t, int32_t, int32_t,
                                 int32_t, int32_t, int32_t, int32_t,
                                 uint32_t, uint32_t);
static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
static void (*r_GenTextures)(int32_t, uint32_t *);
static void (*r_DeleteTextures)(int32_t, const uint32_t *);
static uint8_t (*r_IsEnabled)(uint32_t);
static void (*r_Enable)(uint32_t);
static void (*r_Disable)(uint32_t);

static int klfb_mirror_resolve(int need_layer) {
    static int resolved;
    if (!resolved) {
        resolved = 1;
        r_GenFramebuffers        = asym("glGenFramebuffers");
        r_BindFramebuffer        = asym("glBindFramebuffer");
        r_FramebufferTexture2D   = asym("glFramebufferTexture2D");
        r_FramebufferTextureLayer= asym("glFramebufferTextureLayer");
        r_BlitFramebuffer        = asym("glBlitFramebuffer");
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
        r_GenTextures            = asym("glGenTextures");
        r_DeleteTextures         = asym("glDeleteTextures");
        r_IsEnabled              = asym("glIsEnabled");
        r_Enable                 = asym("glEnable");
        r_Disable                = asym("glDisable");
    }
    if (!r_GenFramebuffers || !r_BindFramebuffer || !r_BlitFramebuffer ||
        !r_FramebufferTexture2D || !r_GenTextures) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror: ANGLE is missing a blit entry point — "
                            "the guest's picture cannot be copied\n");
        return 0;
    }
    if (need_layer && !r_FramebufferTextureLayer) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror: no glFramebufferTextureLayer — an array "
                            "slice cannot be read\n");
        return 0;
    }
    if (!a_glGetIntegerv) {
        // Before anything touches GL state, because everything below has to put
        // it back: the framebuffer bindings and the texture binding are all read
        // through this. Restoring them to 0 instead would hand the guest the
        // default framebuffer as its render target partway through its own
        // frame. Refusing costs a black picture; guessing costs the whole one.
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror: no glGetIntegerv, so the guest's GL "
                            "state cannot be restored — refusing\n");
        return 0;
    }
    return 1;
}

// One texture (or one array slice of one) into another, same size, same format.
// Returns the GL error, or 0.
//
// The guest's state is the guest's. A blit is affected by the scissor test (and
// by nothing else in the fragment pipeline — the write masks do not apply), so
// that is what has to come off, and the two framebuffer bindings are read back
// rather than assumed: this runs inside the guest's frame, between its own
// calls.
static uint32_t klfb_mirror_blit(uint32_t src_tex, int src_layer,
                                 uint32_t dst_tex, int w, int h, int resync) {
    int32_t save_read = 0, save_draw = 0;
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &save_read);
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &save_draw);
    int scissor = r_IsEnabled ? (int)r_IsEnabled(0x0C11 /* SCISSOR_TEST */) : 0;
    if (scissor && r_Disable) r_Disable(0x0C11);

    static uint32_t read_fb, draw_fb;
    if (!read_fb) r_GenFramebuffers(1, &read_fb);
    if (!draw_fb) r_GenFramebuffers(1, &draw_fb);

    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, read_fb);
    if (src_layer >= 0)
        r_FramebufferTextureLayer(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                                  src_tex, 0, src_layer);
    else
        r_FramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1 /* TEXTURE_2D */, src_tex, 0);
    r_BindFramebuffer(0x8CA9 /* DRAW_FRAMEBUFFER */, draw_fb);
    // `resync`: detach first, so the re-attach is a real state change and ANGLE
    // rebuilds the destination's cached render pass descriptor — the rate map
    // it carries is looked up only then (see klfb_vrr_touch).
    if (resync) r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, 0, 0);
    r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, dst_tex, 0);

    // Both sides checked once. An incomplete framebuffer makes the blit a no-op
    // that raises INVALID_FRAMEBUFFER_OPERATION, i.e. a black picture and one
    // error in a log full of them — the failure this whole path exists to end.
    static int checked;
    if (!checked && r_CheckFramebufferStatus) {
        checked = 1;
        uint32_t rs = r_CheckFramebufferStatus(0x8CA8);
        uint32_t ds = r_CheckFramebufferStatus(0x8CA9);
        if (rs != 0x8CD5 || ds != 0x8CD5)
            fprintf(stderr, "  [glfb] mirror: read fb status 0x%x, draw fb status 0x%x "
                            "(0x8CD5 is complete) — the picture will stay black\n",
                    rs, ds);
    }

    if (a_glGetError) while (a_glGetError()) {}
    // Source and destination are the same format, so the sRGB decode on read and
    // encode on write are each other's inverse and this is a copy. NEAREST for
    // the same reason: the rectangles are identical, so no filter is consulted.
    r_BlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                      0x4000 /* COLOR_BUFFER_BIT */, 0x2600 /* NEAREST */);
    uint32_t e = a_glGetError ? a_glGetError() : 0;

    r_BindFramebuffer(0x8CA8, (uint32_t)save_read);
    r_BindFramebuffer(0x8CA9, (uint32_t)save_draw);
    if (scissor && r_Enable) r_Enable(0x0C11);
    return e;
}

// Blit src_tex (full sw×sh) into a SUB-RECTANGLE of dst_tex, scaling. The
// foveal-inset composite: the base eye is already in dst_tex, and this lays the
// narrow high-resolution centre image over the middle of it, where its field of
// view maps. LINEAR because the inset is scaled UP into the base (a filter IS
// consulted, unlike the identity copy klfb_mirror_blit does).
static uint32_t klfb_blit_subrect(uint32_t src_tex, int src_layer, uint32_t dst_tex,
                                  int sw, int sh, int dx0, int dy0, int dx1, int dy1) {
    int32_t save_read = 0, save_draw = 0;
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &save_read);
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &save_draw);
    int scissor = r_IsEnabled ? (int)r_IsEnabled(0x0C11 /* SCISSOR_TEST */) : 0;
    if (scissor && r_Disable) r_Disable(0x0C11);

    static uint32_t iread_fb, idraw_fb;
    if (!iread_fb) r_GenFramebuffers(1, &iread_fb);
    if (!idraw_fb) r_GenFramebuffers(1, &idraw_fb);

    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, iread_fb);
    if (src_layer >= 0)
        r_FramebufferTextureLayer(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                                  src_tex, 0, src_layer);
    else
        r_FramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1 /* TEXTURE_2D */, src_tex, 0);
    r_BindFramebuffer(0x8CA9 /* DRAW_FRAMEBUFFER */, idraw_fb);
    r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, dst_tex, 0);

    if (a_glGetError) while (a_glGetError()) {}
    r_BlitFramebuffer(0, 0, sw, sh, dx0, dy0, dx1, dy1,
                      0x4000 /* COLOR_BUFFER_BIT */, 0x2601 /* LINEAR */);
    uint32_t e = a_glGetError ? a_glGetError() : 0;

    r_BindFramebuffer(0x8CA8, (uint32_t)save_read);
    r_BindFramebuffer(0x8CA9, (uint32_t)save_draw);
    if (scissor && r_Enable) r_Enable(0x0C11);
    return e;
}

// Feathered inset: draw src_tex into the dest rect of dst_tex as a textured quad
// with a RADIAL alpha falloff, blended over what is already there (the base).
// This is what turns the hard square edge of a straight blit into a soft round
// centre that fades into the periphery — the eye sees "sharper toward the middle"
// rather than "a sharp rectangle stuck on". `inner` is the fraction of the radius
// that stays fully opaque before the fade begins (0..1). Returns 0 on success;
// nonzero means the caller should fall back to the plain blit. All GL state it
// touches is saved and restored — it runs inside the guest's own frame.
static int klfb_feather_blit(uint32_t dst_tex, int dstw, int dsth,
                             uint32_t src_tex, int dx0, int dy0, int dx1, int dy1,
                             float inner) {
    static int tried, ok_program;
    static uint32_t prog, vao, vbo;
    static int loc_tex, loc_inner, loc_pos, loc_uv;
    static uint32_t (*r_CreateShader)(uint32_t);
    static void (*r_ShaderSource)(uint32_t, int32_t, const char *const *, const int32_t *);
    static void (*r_CompileShader)(uint32_t);
    static void (*r_GetShaderiv)(uint32_t, uint32_t, int32_t *);
    static uint32_t (*r_CreateProgram)(void);
    static void (*r_AttachShader)(uint32_t, uint32_t);
    static void (*r_LinkProgram)(uint32_t);
    static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
    static void (*r_UseProgram)(uint32_t);
    static int32_t (*r_GetUniformLocation)(uint32_t, const char *);
    static int32_t (*r_GetAttribLocation)(uint32_t, const char *);
    static void (*r_Uniform1i)(int32_t, int32_t);
    static void (*r_Uniform1f)(int32_t, float);
    static void (*r_GenVertexArrays)(int32_t, uint32_t *);
    static void (*r_BindVertexArray)(uint32_t);
    static void (*r_GenBuffers)(int32_t, uint32_t *);
    static void (*r_BindBuffer)(uint32_t, uint32_t);
    static void (*r_BufferData)(uint32_t, long, const void *, uint32_t);
    static void (*r_VertexAttribPointer)(uint32_t, int32_t, uint32_t, uint8_t,
                                         int32_t, const void *);
    static void (*r_EnableVertexAttribArray)(uint32_t);
    static void (*r_DrawArrays)(uint32_t, int32_t, int32_t);
    static void (*r_ActiveTexture)(uint32_t);
    static void (*r_BindTexture)(uint32_t, uint32_t);
    static void (*r_TexParameteri)(uint32_t, uint32_t, int32_t);
    static void (*r_GetTexParameteriv)(uint32_t, uint32_t, int32_t *);
    static void (*r_Enable)(uint32_t);
    static void (*r_Disable)(uint32_t);
    static uint8_t (*r_IsEnabled)(uint32_t);
    static void (*r_BlendFunc)(uint32_t, uint32_t);
    static void (*r_BlendFuncSeparate)(uint32_t, uint32_t, uint32_t, uint32_t);
    static void (*r_Viewport)(int32_t, int32_t, int32_t, int32_t);
    static void (*r_BindFramebuffer2)(uint32_t, uint32_t);
    static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
    static uint32_t (*r_CheckFbStatus)(uint32_t);
    static void (*r_GenFramebuffers)(int32_t, uint32_t *);
    static uint32_t feather_fb;

    if (!tried) {
        tried = 1;
        r_CreateShader = asym("glCreateShader");
        r_ShaderSource = asym("glShaderSource");
        r_CompileShader = asym("glCompileShader");
        r_GetShaderiv = asym("glGetShaderiv");
        r_CreateProgram = asym("glCreateProgram");
        r_AttachShader = asym("glAttachShader");
        r_LinkProgram = asym("glLinkProgram");
        r_GetProgramiv = asym("glGetProgramiv");
        r_UseProgram = asym("glUseProgram");
        r_GetUniformLocation = asym("glGetUniformLocation");
        r_GetAttribLocation = asym("glGetAttribLocation");
        r_Uniform1i = asym("glUniform1i");
        r_Uniform1f = asym("glUniform1f");
        r_GenVertexArrays = asym("glGenVertexArrays");
        r_BindVertexArray = asym("glBindVertexArray");
        r_GenBuffers = asym("glGenBuffers");
        r_BindBuffer = asym("glBindBuffer");
        r_BufferData = asym("glBufferData");
        r_VertexAttribPointer = asym("glVertexAttribPointer");
        r_EnableVertexAttribArray = asym("glEnableVertexAttribArray");
        r_DrawArrays = asym("glDrawArrays");
        r_ActiveTexture = asym("glActiveTexture");
        r_BindTexture = asym("glBindTexture");
        r_TexParameteri = asym("glTexParameteri");
        r_GetTexParameteriv = asym("glGetTexParameteriv");
        r_Enable = asym("glEnable");
        r_Disable = asym("glDisable");
        r_IsEnabled = asym("glIsEnabled");
        r_BlendFunc = asym("glBlendFunc");
        r_BlendFuncSeparate = asym("glBlendFuncSeparate");
        r_Viewport = asym("glViewport");
        r_BindFramebuffer2 = asym("glBindFramebuffer");
        r_FramebufferTexture2D = asym("glFramebufferTexture2D");
        r_CheckFbStatus = asym("glCheckFramebufferStatus");
        r_GenFramebuffers = asym("glGenFramebuffers");
        if (r_CreateShader && r_CreateProgram && r_UseProgram && r_DrawArrays &&
            r_GenVertexArrays && r_GenBuffers && r_VertexAttribPointer &&
            r_BindFramebuffer2 && r_FramebufferTexture2D && r_GenFramebuffers) {
            const char *vs =
                "#version 300 es\n"
                "in vec2 aPos; in vec2 aUV; out vec2 vUV;\n"
                "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
            const char *fs =
                "#version 300 es\n"
                "precision highp float;\n"
                "in vec2 vUV; uniform sampler2D uTex; uniform float uInner;\n"
                "out vec4 o;\n"
                "void main(){\n"
                "  vec3 c = texture(uTex, vUV).rgb;\n"
                "  vec2 d = (vUV - 0.5) * 2.0;\n"
                "  float r = length(d);\n"
                "  float a = 1.0 - smoothstep(uInner, 1.0, r);\n"
                "  o = vec4(c, a);\n"
                "}\n";
            uint32_t v = r_CreateShader(0x8B31 /* VERTEX_SHADER */);
            uint32_t f = r_CreateShader(0x8B30 /* FRAGMENT_SHADER */);
            int32_t okv = 0, okf = 0, okl = 0;
            r_ShaderSource(v, 1, &vs, NULL); r_CompileShader(v);
            r_ShaderSource(f, 1, &fs, NULL); r_CompileShader(f);
            if (r_GetShaderiv) { r_GetShaderiv(v, 0x8B81, &okv);
                                 r_GetShaderiv(f, 0x8B81, &okf); }
            else okv = okf = 1;
            prog = r_CreateProgram();
            r_AttachShader(prog, v); r_AttachShader(prog, f);
            r_LinkProgram(prog);
            if (r_GetProgramiv) r_GetProgramiv(prog, 0x8B82 /* LINK_STATUS */, &okl);
            else okl = 1;
            if (okv && okf && okl) {
                loc_tex   = r_GetUniformLocation(prog, "uTex");
                loc_inner = r_GetUniformLocation(prog, "uInner");
                loc_pos   = r_GetAttribLocation(prog, "aPos");
                loc_uv    = r_GetAttribLocation(prog, "aUV");
                r_GenVertexArrays(1, &vao);
                r_GenBuffers(1, &vbo);
                r_GenFramebuffers(1, &feather_fb);
                if (loc_pos >= 0 && loc_uv >= 0 && vao && vbo && feather_fb)
                    ok_program = 1;
            }
            if (!ok_program)
                fprintf(stderr, "  [glfb] fovea feather: shader unavailable, "
                                "falling back to a hard-edged inset\n");
        }
    }
    if (!ok_program) return 1;

    // Save every piece of state the draw disturbs.
    int32_t s_prog = 0, s_vao = 0, s_vbo = 0, s_at = 0, s_tex2d = 0, s_vp[4];
    int32_t s_draw_fb = 0;
    int32_t s_bsr = 0x1, s_bdr = 0, s_bsa = 0x1, s_bda = 0;
    a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &s_prog);
    a_glGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &s_vao);
    a_glGetIntegerv(0x8894 /* ARRAY_BUFFER_BINDING */, &s_vbo);
    a_glGetIntegerv(0x84E0 /* ACTIVE_TEXTURE */, &s_at);
    a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &s_tex2d);
    a_glGetIntegerv(0x0BA2 /* VIEWPORT */, s_vp);
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &s_draw_fb);
    a_glGetIntegerv(0x80C9 /* BLEND_SRC_RGB */,   &s_bsr);
    a_glGetIntegerv(0x80C8 /* BLEND_DST_RGB */,   &s_bdr);
    a_glGetIntegerv(0x80CB /* BLEND_SRC_ALPHA */, &s_bsa);
    a_glGetIntegerv(0x80CA /* BLEND_DST_ALPHA */, &s_bda);
    int blend_was = r_IsEnabled ? (int)r_IsEnabled(0x0BE2 /* BLEND */) : 0;
    int depth_was = r_IsEnabled ? (int)r_IsEnabled(0x0B71 /* DEPTH_TEST */) : 0;
    int cull_was  = r_IsEnabled ? (int)r_IsEnabled(0x0B44 /* CULL_FACE */) : 0;
    int sciss_was = r_IsEnabled ? (int)r_IsEnabled(0x0C11 /* SCISSOR_TEST */) : 0;

    r_BindFramebuffer2(0x8CA9 /* DRAW_FRAMEBUFFER */, feather_fb);
    r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, dst_tex, 0);
    if (r_CheckFbStatus && r_CheckFbStatus(0x8CA9) != 0x8CD5) {
        r_BindFramebuffer2(0x8CA9, (uint32_t)s_draw_fb);
        return 1;   // let the caller blit instead
    }

    if (depth_was && r_Disable) r_Disable(0x0B71);
    if (cull_was  && r_Disable) r_Disable(0x0B44);
    if (sciss_was && r_Disable) r_Disable(0x0C11);
    if (r_Enable) r_Enable(0x0BE2 /* BLEND */);
    if (r_BlendFunc) r_BlendFunc(0x0302 /* SRC_ALPHA */, 0x0303 /* ONE_MINUS_SRC_ALPHA */);
    r_Viewport(0, 0, dstw, dsth);

    float nx0 = 2.0f * dx0 / dstw - 1.0f, nx1 = 2.0f * dx1 / dstw - 1.0f;
    float ny0 = 2.0f * dy0 / dsth - 1.0f, ny1 = 2.0f * dy1 / dsth - 1.0f;
    float quad[16] = {
        nx0, ny0, 0.0f, 0.0f,
        nx1, ny0, 1.0f, 0.0f,
        nx0, ny1, 0.0f, 1.0f,
        nx1, ny1, 1.0f, 1.0f,
    };
    r_BindVertexArray(vao);
    r_BindBuffer(0x8892 /* ARRAY_BUFFER */, vbo);
    r_BufferData(0x8892, (long)sizeof quad, quad, 0x88E0 /* DYNAMIC_DRAW */);
    r_EnableVertexAttribArray((uint32_t)loc_pos);
    r_VertexAttribPointer((uint32_t)loc_pos, 2, 0x1406 /* FLOAT */, 0,
                          4 * (int)sizeof(float), (const void *)0);
    r_EnableVertexAttribArray((uint32_t)loc_uv);
    r_VertexAttribPointer((uint32_t)loc_uv, 2, 0x1406, 0,
                          4 * (int)sizeof(float),
                          (const void *)(2 * sizeof(float)));
    r_UseProgram(prog);
    r_ActiveTexture(0x84C0 /* TEXTURE0 */);
    r_BindTexture(0x0DE1, src_tex);
    // The shader samples with texture(): the inset must have a mipmap-free filter
    // (a swapchain texture can default to NEAREST_MIPMAP_* and sample as black)
    // and clamp so the fade does not wrap. Saved and restored on the guest's own
    // texture.
    int32_t s_min = 0x2601, s_mag = 0x2601, s_ws = 0x812F, s_wt = 0x812F;
    if (r_GetTexParameteriv) {
        r_GetTexParameteriv(0x0DE1, 0x2801 /* MIN_FILTER */, &s_min);
        r_GetTexParameteriv(0x0DE1, 0x2800 /* MAG_FILTER */, &s_mag);
        r_GetTexParameteriv(0x0DE1, 0x2802 /* WRAP_S */, &s_ws);
        r_GetTexParameteriv(0x0DE1, 0x2803 /* WRAP_T */, &s_wt);
    }
    if (r_TexParameteri) {
        r_TexParameteri(0x0DE1, 0x2801, 0x2601 /* LINEAR */);
        r_TexParameteri(0x0DE1, 0x2800, 0x2601);
        r_TexParameteri(0x0DE1, 0x2802, 0x812F /* CLAMP_TO_EDGE */);
        r_TexParameteri(0x0DE1, 0x2803, 0x812F);
    }
    if (loc_tex >= 0)   r_Uniform1i(loc_tex, 0);
    if (loc_inner >= 0) r_Uniform1f(loc_inner, inner);
    r_DrawArrays(0x0005 /* TRIANGLE_STRIP */, 0, 4);

    // Restore the guest's texture parameters while its texture is still bound.
    if (r_TexParameteri) {
        r_TexParameteri(0x0DE1, 0x2801, s_min);
        r_TexParameteri(0x0DE1, 0x2800, s_mag);
        r_TexParameteri(0x0DE1, 0x2802, s_ws);
        r_TexParameteri(0x0DE1, 0x2803, s_wt);
    }

    // Restore everything else, in reverse.
    r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, 0, 0);
    r_BindFramebuffer2(0x8CA9, (uint32_t)s_draw_fb);
    r_UseProgram((uint32_t)s_prog);
    r_BindVertexArray((uint32_t)s_vao);
    r_BindBuffer(0x8892, (uint32_t)s_vbo);
    r_ActiveTexture(0x84C0);
    r_BindTexture(0x0DE1, (uint32_t)s_tex2d);
    if (s_at != 0x84C0) r_ActiveTexture((uint32_t)s_at);
    r_Viewport(s_vp[0], s_vp[1], s_vp[2], s_vp[3]);
    if (r_BlendFuncSeparate)
        r_BlendFuncSeparate((uint32_t)s_bsr, (uint32_t)s_bdr,
                            (uint32_t)s_bsa, (uint32_t)s_bda);
    if (!blend_was && r_Disable) r_Disable(0x0BE2);
    if (depth_was && r_Enable) r_Enable(0x0B71);
    if (cull_was  && r_Enable) r_Enable(0x0B44);
    if (sciss_was && r_Enable) r_Enable(0x0C11);
    return 0;
}

// Give one GL name storage the layer provider owns. Internal: the only caller
// is kl_glfb_mirror_layer below, and the name it passes is one of OURS — see
// that function's header comment for why the guest's own name must not be
// handed to this.
static int klfb_bind_layer_mtl_texture(int layer, int stage, uint32_t gl_tex,
                                       int w, int h, uint32_t internal_fmt) {
    if (!g_mtl_layer_provider || layer < 0 || !gl_tex || w <= 0 || h <= 0) return 0;
    if (stage < 0 || stage >= KL_MTL_MAX_STAGES) {
        fprintf(stderr, "  [glfb] layer stage %d is beyond KL_MTL_MAX_STAGES (%d)\n",
                stage, KL_MTL_MAX_STAGES);
        return 0;
    }
    if (!kl_glfb_init() || !mtl_resolve()) {
        fprintf(stderr, "  [glfb] a Metal layer provider is registered but ANGLE's "
                        "interop entry points are missing — falling back to GL "
                        "storage, so this layer is not composited\n");
        return 0;
    }
    int row = klfb_layer_row(layer, stage, 1);
    if (row < 0) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [glfb] no room for layer %d stage %d — %u (layer, "
                            "stage) pairs fit and this one is NOT composited\n",
                    layer, stage,
                    (unsigned)(sizeof g_layer_mtl / sizeof *g_layer_mtl));
        }
        return 0;
    }
    // Already backed by this same GL name and size: the caller re-asserts it
    // every frame (a quad's swapchain is only known to be a quad at submission),
    // so answering from the row is what keeps that from being an allocation and
    // an eglCreateImageKHR per frame.
    if (g_layer_mtl[row].gl_tex == gl_tex && g_layer_mtl[row].w == w &&
        g_layer_mtl[row].h == h && g_layer_mtl[row].tex)
        return 1;

    kl_mtl_eye_texture t = { NULL, 0, 0, 0 };
    if (!g_mtl_layer_provider(layer, stage, w, h, internal_fmt, &t,
                              g_mtl_layer_provider_ctx) || !t.texture) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [glfb] provider declined layer=%d stage=%d %dx%d "
                            "fmt 0x%x\n", layer, stage, w, h, internal_fmt);
        }
        return 0;
    }
    // The same refusal the eye path makes, for the same reason: the extension
    // takes the size from the MTLTexture and succeeds whatever we asked for, so
    // a mismatch is a guest rendering into storage it does not have.
    if (t.w != w || t.h != h) {
        fprintf(stderr, "  [glfb] provider returned a %dx%d texture for a %dx%d "
                        "layer (layer=%d stage=%d) — refusing\n",
                t.w, t.h, w, h, layer, stage);
        return 0;
    }
    const int32_t attrs[] = {
        EGL_METAL_TEXTURE_ARRAY_SLICE_ANGLE_, t.slice,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE_,   (int32_t)internal_fmt,
        EGL_NONE_,
    };
    void *img = a_eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT_, EGL_METAL_TEXTURE_ANGLE_,
                                    t.texture, attrs);
    if (img == EGL_NO_IMAGE_) {
        fprintf(stderr, "  [glfb] eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE) failed "
                        "for layer=%d stage=%d — was the texture made on "
                        "kl_glfb_mtl_device()?\n", layer, stage);
        return 0;
    }
    a_glBindTexture_mtl(0x0DE1 /* GL_TEXTURE_2D */, gl_tex);
    while (a_glGetError && a_glGetError() != 0) {}
    a_glEGLImageTargetTexture2DOES(0x0DE1, img);
    uint32_t e = a_glGetError ? a_glGetError() : 0;
    if (e) {
        fprintf(stderr, "  [glfb] glEGLImageTargetTexture2DOES -> GL error 0x%x "
                        "(layer=%d stage=%d)\n", e, layer, stage);
        if (a_eglDestroyImageKHR) a_eglDestroyImageKHR(g_dpy, img);
        return 0;
    }
    if (g_layer_mtl[row].image && a_eglDestroyImageKHR)
        a_eglDestroyImageKHR(g_dpy, g_layer_mtl[row].image);
    g_layer_mtl[row].layer  = layer;
    g_layer_mtl[row].stage  = stage;
    g_layer_mtl[row].tex    = t.texture;
    g_layer_mtl[row].slice  = t.slice;
    g_layer_mtl[row].image  = img;
    g_layer_mtl[row].gl_tex = gl_tex;
    g_layer_mtl[row].w      = w;
    g_layer_mtl[row].h      = h;
    // As on the eye path: an EGLImage-backed name never passes through the
    // glTexStorage2D thunk, so nothing else records what size this texture is,
    // and the capture would read it at the pbuffer's size.
    klfb_note_tex_storage(gl_tex, internal_fmt, w, h);
    fprintf(stderr, "  [glfb] layer=%d stage=%d tex=%u is now backed by MTLTexture "
                    "%p slice %d (%dx%d fmt 0x%x)\n",
            layer, stage, gl_tex, t.texture, t.slice, w, h, internal_fmt);
    return 1;
}

// The guest's presented image, copied into storage the compositor can sample.
// See kl_glfb.h for why this is a copy and not the eye path's re-point.
//
// The destination is one GL name per (layer, stage), created here and given
// provider storage by the call above — so the layer table, the compositors and
// the readback all see what they would have seen from a re-point, and the
// guest's own texture is never touched.
static struct { int layer, stage; uint32_t tex; int w, h; uint32_t fmt; }
    g_layer_mirror[KL_MTL_MAX_LAYERS * KL_MTL_MAX_STAGES];

int kl_glfb_mirror_layer(int layer, int stage, uint32_t src_tex, int src_layer,
                         int w, int h, uint32_t internal_fmt) {
    if (!g_mtl_layer_provider || !src_tex || w <= 0 || h <= 0) return 0;
    if (layer < 0 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return 0;
    if (!kl_glfb_init() || !mtl_resolve()) return 0;
    if (!klfb_mirror_resolve(src_layer >= 0)) return 0;

    int row = -1, freerow = -1;
    for (unsigned i = 0; i < sizeof g_layer_mirror / sizeof *g_layer_mirror; i++) {
        if (g_layer_mirror[i].tex && g_layer_mirror[i].layer == layer &&
            g_layer_mirror[i].stage == stage) { row = (int)i; break; }
        if (!g_layer_mirror[i].tex && freerow < 0) freerow = (int)i;
    }
    if (row < 0) row = freerow;
    if (row < 0) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] no room to mirror layer %d stage %d — %u "
                            "(layer, stage) pairs fit and this one is NOT "
                            "composited\n", layer, stage,
                    (unsigned)(sizeof g_layer_mirror / sizeof *g_layer_mirror));
        return 0;
    }
    __typeof__(g_layer_mirror[0]) *m = &g_layer_mirror[row];
    // A size or format change means the guest rebuilt its swapchain, and the old
    // destination describes a picture that no longer exists — the blit would
    // happily scale one into the other.
    if (m->tex && (m->w != w || m->h != h || m->fmt != internal_fmt)) {
        fprintf(stderr, "  [glfb] mirror layer=%d stage=%d: %dx%d fmt 0x%x -> %dx%d "
                        "fmt 0x%x, reallocating\n",
                layer, stage, m->w, m->h, m->fmt, w, h, internal_fmt);
        if (r_DeleteTextures) r_DeleteTextures(1, &m->tex);
        *m = (__typeof__(*m)){0};
    }
    if (!m->tex) {
        uint32_t t = 0;
        r_GenTextures(1, &t);
        if (!t) return 0;
        // klfb_bind_layer_mtl_texture leaves GL_TEXTURE_2D bound to what it was
        // given, and this runs inside the guest's frame, between its calls — so
        // the binding is put back. Once per (layer, stage), not per frame.
        int32_t save_tex = 0;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &save_tex);
        int ok = klfb_bind_layer_mtl_texture(layer, stage, t, w, h, internal_fmt);
        if (a_glBindTexture_mtl) a_glBindTexture_mtl(0x0DE1, (uint32_t)save_tex);
        if (!ok) {
            if (r_DeleteTextures) r_DeleteTextures(1, &t);
            return 0;
        }
        m->layer = layer; m->stage = stage; m->tex = t;
        m->w = w; m->h = h; m->fmt = internal_fmt;
        fprintf(stderr, "  [glfb] mirror layer=%d stage=%d: %dx%d fmt 0x%x <- guest "
                        "tex %u (one blit a frame; the guest's own storage is left "
                        "alone)\n", layer, stage, w, h, internal_fmt, src_tex);
    }

    uint32_t e = klfb_mirror_blit(src_tex, src_layer, m->tex, w, h, 0);
    if (e) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror layer=%d stage=%d: glBlitFramebuffer -> "
                            "GL error 0x%x\n", layer, stage, e);
        return 0;
    }
    return 1;
}

void kl_glfb_note_layer_mtl_texture(int layer, int stage, void *texture,
                                    int w, int h) {
    if (layer < 0 || !texture) return;
    if (stage < 0 || stage >= KL_MTL_MAX_STAGES) return;
    int row = klfb_layer_row(layer, stage, 1);
    if (row < 0) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [glfb] no room for layer %d stage %d — %u (layer, "
                            "stage) pairs fit and this one is NOT composited\n",
                    layer, stage,
                    (unsigned)(sizeof g_layer_mtl / sizeof *g_layer_mtl));
        }
        return;
    }
    if (g_layer_mtl[row].tex == texture && g_layer_mtl[row].w == w) return;
    g_layer_mtl[row].layer = layer;
    g_layer_mtl[row].stage = stage;
    g_layer_mtl[row].tex   = texture;
    g_layer_mtl[row].slice = 0;
    g_layer_mtl[row].w     = w;
    g_layer_mtl[row].h     = h;
    fprintf(stderr, "  [glfb] layer=%d stage=%d is MTLTexture %p (%dx%d), the "
                    "guest's own image\n", layer, stage, texture, w, h);
}

void *kl_glfb_layer_mtl_texture(int layer, int stage, int *out_w, int *out_h) {
    int row = klfb_layer_row(layer, stage, 0);
    if (row < 0 || !g_layer_mtl[row].tex) return NULL;
    if (out_w) *out_w = g_layer_mtl[row].w;
    if (out_h) *out_h = g_layer_mtl[row].h;
    return g_layer_mtl[row].tex;
}

// ---------------------------------------------------------------------------
// The ARRAY swapchain's way onto the compositor — see kl_glfb.h for why a copy
// is the only route and what it costs.
//
// One destination per (eye, stage), allocated the ordinary way: a GL name of
// OURS, given provider storage by the call above. So everything downstream —
// the eye table, the rate map, the sRGB settle, the census, the release path —
// sees exactly what it sees for a Unity/OVRPlugin guest, and this function owns
// nothing but the blit and the name it created.
//
// `src` is the guest texture whose image this slot currently holds, and it is a
// DIAGNOSTIC of a keying problem rather than something the blit needs: `stage`
// is a swapchain's own image index, so two different swapchains both have
// images 0..2 and land in the same slot. A guest submitting more than one
// projection layer therefore has two sources competing for one destination —
// visible as this record reallocating back and forth between their sizes.
static struct { uint32_t tex; int w, h; uint32_t fmt; uint32_t src; }
    g_eye_mirror[2][KL_MTL_MAX_STAGES];

// Which (swapchain, image) each slot currently holds, and the reason a slot is
// allocated rather than being the guest's own image index.
//
// A swapchain's image index runs 0..2 whatever swapchain it belongs to, so two
// of a guest's projection layers name the same index in the same frame and land
// in the same destination. That is why only one layer could ever be composited:
// a second one overwrote the first, and the record reallocated back and forth
// between their sizes. Keyed by (source, image) instead, every image a guest
// keeps in flight has a destination of its own and any number of layers coexist.
//
// The key does NOT include the eye: the destinations are already per-eye
// (g_eye_mirror[2][...]), so one slot number addresses both eyes' copies of the
// same guest image and a layer that names only one eye simply leaves the other
// half unused. That is what makes 16 slots cover Steam Link's three layers of
// three images with room over, instead of half that.
//
// `used` is the allocation clock, for the least-recently-used eviction: a guest
// with more live images than slots has to degrade somehow, and reusing the
// oldest — loudly — is the one that keeps the images it is actually cycling
// through. Evicting silently would present as a layer flickering.
static struct { uint32_t source; int image; uint64_t used; }
    g_eye_slot[KL_MTL_MAX_STAGES];
static uint64_t g_eye_slot_clock;

// The slot holding (source, image), allocating or evicting one if it is new.
// `source` 0 means the caller has no swapchain identity to give, and then the
// image index IS the slot — the OVRPlugin path's direct keying, unchanged.
static int klfb_eye_slot(uint32_t source, int image) {
    if (!source)
        return (image >= 0 && image < KL_MTL_MAX_STAGES) ? image : -1;
    int free_slot = -1, lru = 0;
    for (int i = 0; i < KL_MTL_MAX_STAGES; i++) {
        if (g_eye_slot[i].source == source && g_eye_slot[i].image == image) {
            g_eye_slot[i].used = ++g_eye_slot_clock;
            return i;
        }
        if (!g_eye_slot[i].source && free_slot < 0) free_slot = i;
        if (g_eye_slot[i].used < g_eye_slot[lru].used) lru = i;
    }
    int slot = free_slot >= 0 ? free_slot : lru;
    if (free_slot < 0) {
        // Named, and named every time: an eviction is a destination being
        // reused for a different picture, so a guest cycling through more
        // images than there are slots would otherwise show layers alternating
        // with each other and nothing would say why.
        fprintf(stderr, "  [glfb] eye slot pool full (%d): evicting swapchain %u "
                        "image %d for swapchain %u image %d — this guest keeps "
                        "more images live than there are slots\n",
                KL_MTL_MAX_STAGES, g_eye_slot[slot].source, g_eye_slot[slot].image,
                source, image);
        for (int e = 0; e < 2; e++) {
            if (!g_eye_mirror[e][slot].tex) continue;
            kl_glfb_release_eye_texture(e, slot);
            g_eye_mirror[e][slot] = (__typeof__(g_eye_mirror[0][0])){0};
        }
    }
    g_eye_slot[slot].source = source;
    g_eye_slot[slot].image  = image;
    g_eye_slot[slot].used   = ++g_eye_slot_clock;
    return slot;
}

// Forward-declared for the KL_GLFB_PROBE_VIDEO readbacks (here and in
// kl_glfb_image_bind); both are defined further down with the probe machinery.
static unsigned long klfb_probe_fbo(uint32_t, float *, uint8_t *, char *, size_t,
                                    uint8_t *, int32_t *, int32_t *, int32_t, int32_t);
static uint32_t klfb_read_from_texture_layer(uint32_t, int);

// KL_GLFB_PROBE_VIDEO shared scratch + a lit-count readback of any texture. Used
// to answer "did the guest's draw land lit" (the eye texture, here) vs "is the
// decoded frame in its texture" (the video texture, in kl_glfb_image_bind).
static void klfb_probe_named_tex(const char *tag, uint32_t tex, int layer,
                                 int hint_w, int hint_h) {
    static int pv_on = -1;
    if (pv_on < 0) pv_on = kl_env_on("KL_GLFB_PROBE_VIDEO", 0);
    if (!pv_on) return;
    // Cap the number of readbacks, not just the logging: a full-frame glReadPixels
    // with glFinish every eye every frame would starve the stream. 48 is enough to
    // see both textures settle over the first frames.
    static unsigned runs;
    if (runs++ >= 48) return;
    static float   *pf;
    static uint8_t *pb;
    static long     cap;
    long need = (long)hint_w * hint_h;
    if (need <= 0) return;
    if (need > cap) {
        free(pf); free(pb);
        pf = malloc((size_t)need * 16);
        pb = malloc((size_t)need * 4);
        cap = (pf && pb) ? need : 0;
    }
    if (cap < need) return;
    static void (*r_bindfb)(uint32_t, uint32_t);
    static void (*r_bindtex)(uint32_t, uint32_t);
    if (!r_bindfb)  r_bindfb  = asym("glBindFramebuffer");
    if (!r_bindtex) r_bindtex = asym("glBindTexture");
    int32_t s_t = 0, s_rf = 0, s_df = 0;
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8069, &s_t);    // TEXTURE_BINDING_2D
        a_glGetIntegerv(0x8CAA, &s_rf);   // READ_FRAMEBUFFER_BINDING
        a_glGetIntegerv(0x8CA6, &s_df);   // DRAW_FRAMEBUFFER_BINDING
    }
    char nt[160] = "no framebuffer";
    unsigned long lit = 0;
    uint32_t fb = klfb_read_from_texture_layer(tex, layer);
    if (fb) lit = klfb_probe_fbo(fb, pf, pb, nt, sizeof nt, NULL, NULL, NULL,
                                 hint_w, hint_h);
    if (r_bindfb) { r_bindfb(0x8CA8, (uint32_t)s_rf); r_bindfb(0x8CA9, (uint32_t)s_df); }
    if (r_bindtex) r_bindtex(0x0DE1, (uint32_t)s_t);
    static unsigned said;
    if (said++ < 24)
        fprintf(stderr, "  [glfb] PROBE_%s tex=%u layer=%d: %lu lit of %dx%d (%s)\n",
                tag, tex, layer, lit, hint_w, hint_h, nt);
}

// Both defined further down with the PNG/probe machinery.
static int klfb_write_png(const char *path, const uint8_t *px,
                          int32_t w, int32_t h);

// Read a texture back and write it as a PNG. The alignment instrument: a seam
// whose two sides are DOUBLED rather than merely differently sharp is a
// disagreement between the field of view a layer states and the image behind
// it, and no amount of reasoning about stated frustums can measure that — the
// two images have to be compared to each other. Diagnostic-only and unthrottled,
// because the caller dumps once.
int kl_glfb_dump_tex(const char *path, uint32_t tex, int layer, int w, int h) {
    if (!path || !tex || w <= 0 || h <= 0) return 0;
    static void (*r_bindfb)(uint32_t, uint32_t);
    static void (*r_bindtex)(uint32_t, uint32_t);
    static void (*r_bindbuf)(uint32_t, uint32_t);
    static void (*r_pixelstorei)(uint32_t, int32_t);
    if (!r_bindfb)      r_bindfb      = asym("glBindFramebuffer");
    if (!r_bindtex)     r_bindtex     = asym("glBindTexture");
    if (!r_bindbuf)     r_bindbuf     = asym("glBindBuffer");
    if (!r_pixelstorei) r_pixelstorei = asym("glPixelStorei");
    if (!a_glReadPixels || !r_bindfb) return 0;

    // The size is the CALLER's, taken from the swapchain that owns the image,
    // and it is checked against the allocation table rather than discovered
    // from the framebuffer. The first cut of this read a size back out of the
    // attachment and got the WINDOW (1280x800) for a 1536x1536 layer, then
    // wrote a perfectly plausible PNG of the wrong surface — which is worse
    // than writing nothing, because the picture invites you to measure it.
    uint32_t tfmt = 0; int32_t tw = 0, th = 0;
    klfb_tex_info(tex, &tfmt, &tw, &th);
    if (tw > 0 && th > 0 && (tw != w || th != h)) {
        fprintf(stderr, "  [glfb] dump %s: asked for %dx%d but texture %u is "
                        "%dx%d — refusing rather than writing the wrong "
                        "picture\n", path, w, h, tex, tw, th);
        return 0;
    }

    uint32_t fb = klfb_read_from_texture_layer(tex, layer);
    if (!fb) {
        fprintf(stderr, "  [glfb] dump %s: texture %u layer %d could not be "
                        "attached for reading — nothing written\n",
                path, tex, layer);
        return 0;
    }
    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) return 0;

    int32_t s_rf = 0, s_df = 0, s_t = 0, pack_buf = 0, pack_align = 4,
            pack_rowlen = 0;
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CAA, &s_rf);
        a_glGetIntegerv(0x8CA6, &s_df);
        a_glGetIntegerv(0x8069, &s_t);
        a_glGetIntegerv(0x88ED, &pack_buf);      // PIXEL_PACK_BUFFER_BINDING
        a_glGetIntegerv(0x0D05, &pack_align);
        a_glGetIntegerv(0x0D02, &pack_rowlen);
    }
    // A bound PIXEL_PACK_BUFFER turns the pointer below into an OFFSET: zeroes,
    // no error. Neutralised and restored, like every other read here.
    if (pack_buf && r_bindbuf) r_bindbuf(0x88EB, 0);
    if (r_pixelstorei) {
        if (pack_align != 4) r_pixelstorei(0x0D05, 4);
        if (pack_rowlen)     r_pixelstorei(0x0D02, 0);
    }
    r_bindfb(0x8CA8 /* READ_FRAMEBUFFER */, fb);
    // The read buffer is part of the framebuffer's state and a stale one is a
    // GL_INVALID_ENUM on the read, not a wrong picture — set it explicitly.
    static void (*r_readbuffer)(uint32_t);
    if (!r_readbuffer) r_readbuffer = asym("glReadBuffer");
    if (r_readbuffer) r_readbuffer(0x8CE0 /* COLOR_ATTACHMENT0 */);
    if (a_glFinish) a_glFinish();
    a_glReadPixels(0, 0, w, h, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, px);
    uint32_t err = a_glGetError ? a_glGetError() : 0;

    if (pack_buf && r_bindbuf) r_bindbuf(0x88EB, (uint32_t)pack_buf);
    if (r_pixelstorei) {
        if (pack_align != 4) r_pixelstorei(0x0D05, pack_align);
        if (pack_rowlen)     r_pixelstorei(0x0D02, pack_rowlen);
    }
    if (r_bindfb) { r_bindfb(0x8CA8, (uint32_t)s_rf); r_bindfb(0x8CA9, (uint32_t)s_df); }
    if (r_bindtex) r_bindtex(0x0DE1, (uint32_t)s_t);

    int ok = 0;
    if (err) {
        fprintf(stderr, "  [glfb] dump %s: glReadPixels error 0x%x — nothing "
                        "written\n", path, err);
    } else {
        errno = 0;
        ok = klfb_write_png(path, px, (int32_t)w, (int32_t)h) != 0;
        int werr = errno;
        // The mean is the instrument checking itself: a uniform 0 here is a
        // read that succeeded and saw nothing, which is a different fault from
        // the two refusals above and must not look like a picture.
        double sum = 0;
        for (long i = 0; i < (long)w * h; i++)
            sum += (px[i*4] + px[i*4+1] + px[i*4+2]) / 3.0;
        // Name the errno: on a device the usual answer is that the directory
        // is outside the sandbox, and "write FAILED" alone sends you looking at
        // the GL instead of at the path.
        fprintf(stderr, "  [glfb] dump %s: tex=%u layer=%d %dx%d mean %.2f -> %s%s%s\n",
                path, tex, layer, w, h, sum / ((double)w * h),
                ok ? "written" : "write FAILED",
                ok || !werr ? "" : " — ", ok || !werr ? "" : strerror(werr));
    }
    free(px);
    return ok;
}

void kl_glfb_probe_tex(const char *tag, uint32_t tex, int layer,
                       int hint_w, int hint_h) {
    klfb_probe_named_tex(tag, tex, layer, hint_w, hint_h);
}

// A foveated guest (Steam Link) hands us a low-resolution base eye and a
// separate high-resolution centre inset. If the compositor's eye texture is the
// base's own size, laying the inset into its centre SUB-rectangle scales the
// inset DOWN to fit — throwing away the very detail it carries, so the centre
// ends up no sharper than the periphery. So the eye texture is allocated LARGER
// than the base (the base is upscaled into it, which costs the base nothing it
// had) and the inset then lands at close to full resolution. num/den is the
// factor; 1/1 (the default) is the old behaviour for every non-foveated guest.
static int g_eye_scale_num = 1, g_eye_scale_den = 1, g_eye_cap = 3456;
void kl_glfb_set_eye_mirror_scale(int num, int den) {
    if (num > 0 && den > 0) { g_eye_scale_num = num; g_eye_scale_den = den; }
}
void kl_glfb_set_eye_mirror_cap(int px) { if (px > 0) g_eye_cap = px; }
// Uniform upscale by num/den, aspect preserved, capped so the larger side never
// exceeds g_eye_cap — a base already near the display's resolution (Steam Link's
// 3072-wide menu layers) must not be doubled to 6144 and churn the allocator,
// while the small streamed base (1152) still grows enough to host the inset.
static void klfb_eye_scaled(int w, int h, int *ow, int *oh) {
    double s = (double)g_eye_scale_num / (double)g_eye_scale_den;
    if (s < 1.0) s = 1.0;
    int mx = w > h ? w : h;
    if (g_eye_cap > 0 && (double)mx * s > (double)g_eye_cap)
        s = (double)g_eye_cap / (double)mx;
    int mw = (int)(w * s + 0.5), mh = (int)(h * s + 0.5);
    if (mw < w) mw = w;
    if (mh < h) mh = h;
    *ow = mw; *oh = mh;
}

// Every slot this swapchain owns, released together — the teardown half of the
// allocator above. Without it a destroyed swapchain's slots stay claimed by a
// source that no longer exists, and a guest that rebuilds its swapchains fills
// the pool and starts evicting the ones it is using: layers alternating with
// each other, every counter healthy.
void kl_glfb_forget_eye_source(uint32_t source) {
    if (!source) return;
    int freed = 0;
    for (int i = 0; i < KL_MTL_MAX_STAGES; i++) {
        if (g_eye_slot[i].source != source) continue;
        for (int e = 0; e < 2; e++) {
            if (!g_eye_mirror[e][i].tex) continue;
            kl_glfb_release_eye_texture(e, i);
            g_eye_mirror[e][i] = (__typeof__(g_eye_mirror[0][0])){0};
        }
        g_eye_slot[i] = (__typeof__(g_eye_slot[0])){0};
        freed++;
    }
    if (freed)
        fprintf(stderr, "  [glfb] swapchain %u destroyed: %d eye slot(s) freed\n",
                source, freed);
}

int kl_glfb_mirror_eye_layer(int eye, int stage, uint32_t source,
                             uint32_t src_tex, int src_layer,
                             int w, int h, uint32_t internal_fmt) {
    if (!g_mtl_provider || !src_tex || w <= 0 || h <= 0) return -1;
    if (eye < 0 || eye > 1 || stage < 0) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror eye=%d image=%d is out of range — "
                            "this eye is not composited\n", eye, stage);
        return -1;
    }
    if (!kl_glfb_init() || !mtl_resolve()) return -1;

    // The guest's (swapchain, image) pair, mapped to a destination of its own.
    // See klfb_eye_slot: the image index alone is not unique across a guest's
    // swapchains, which is why several projection layers used to collide in one
    // destination and only one of them could be composited.
    int slot = klfb_eye_slot(source, stage);
    if (slot < 0) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror image %d has no slot (%d slots, no "
                            "swapchain identity given) — not composited\n",
                    stage, KL_MTL_MAX_STAGES);
        return -1;
    }

    // Before the first bind below, because that is what asks the provider for
    // storage and both providers build a rate map from the size they are asked
    // for. A map already in force (KleptonCompositor builds one in primeDisplay,
    // before the guest has rendered anything) is retired here.
    if (!g_eye_mirroring) {
        g_eye_mirroring = 1;
        if (g_eye_rate_map && !klfb_vrr_mirror_on())
            kl_glfb_set_eye_rate_map(0, 0, 0, 0, NULL);
    }

    if (!klfb_mirror_resolve(src_layer >= 0)) return -1;

    // The destination is allocated at the SCALED size (see klfb_eye_scaled): for
    // a foveated guest it is larger than the source eye so the centre inset laid
    // over it later keeps its detail. The base is upscaled into it here.
    int mw, mh;
    klfb_eye_scaled(w, h, &mw, &mh);

    // (Re)allocate the destination. A size or format change means the guest
    // rebuilt its swapchain, and the old destination describes a picture that no
    // longer exists — release it rather than blit a mismatch, which the blit
    // would happily scale.
    __typeof__(g_eye_mirror[0][0]) *m = &g_eye_mirror[eye][slot];
    // ...and the eye table is the authority on whether our name still exists.
    // kl_glfb_release_eye_texture deletes it, and it has callers that know
    // nothing about this record — a swapchain teardown, a re-bind backstop — so
    // a destination remembered here and gone there would be a blit into a
    // deleted name: GL_INVALID_OPERATION, once, and a black eye for the rest of
    // the run.
    if (m->tex && g_eye_mtl[eye][slot].gl_tex != m->tex) *m = (__typeof__(*m)){0};
    if (m->tex && (m->w != mw || m->h != mh || m->fmt != internal_fmt)) {
        fprintf(stderr, "  [glfb] mirror eye=%d slot=%d: %dx%d fmt 0x%x (guest tex "
                        "%u) -> %dx%d fmt 0x%x (guest tex %u), reallocating — the "
                        "guest rebuilt this swapchain\n",
                eye, slot, m->w, m->h, m->fmt, m->src, mw, mh, internal_fmt, src_tex);
        kl_glfb_release_eye_texture(eye, slot);   // drops the EGLImage AND our name
        *m = (__typeof__(*m)){0};
    }
    if (!m->tex) {
        uint32_t t = 0;
        r_GenTextures(1, &t);
        if (!t) return -1;
        // kl_glfb_bind_eye_mtl_texture leaves GL_TEXTURE_2D bound to whatever it
        // was given, which is fine where it is normally called from (the guest's
        // own graphics setup, in ovrp_SetupEyeTexture2) and is not fine here:
        // this runs inside the guest's frame, between its calls. Six times a run
        // rather than every frame, and the failure would be a texture binding
        // the guest never asked for surviving into its next draw.
        int32_t save_tex = 0;
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &save_tex);
        int ok = kl_glfb_bind_eye_mtl_texture(eye, slot, t, mw, mh, internal_fmt);
        if (a_glBindTexture_mtl) a_glBindTexture_mtl(0x0DE1, (uint32_t)save_tex);
        if (!ok) {
            // The provider declined, so the name is ours and nothing else took a
            // reference to it. kl_glfb_bind_eye_mtl_texture has already said why.
            if (r_DeleteTextures) r_DeleteTextures(1, &t);
            return -1;
        }
        m->tex = t; m->w = mw; m->h = mh; m->fmt = internal_fmt; m->src = src_tex;
        fprintf(stderr, "  [glfb] mirror eye=%d slot=%d: %dx%d fmt 0x%x <- swapchain "
                        "%u image %d (guest tex %u layer %d) %s(one blit a frame; "
                        "the array swapchain cannot be re-pointed)\n",
                eye, slot, mw, mh, internal_fmt, source, stage, src_tex, src_layer,
                (mw != w || mh != h) ? "UPSCALED from source " : "");
    }

    // KL_GLFB_PROBE_VIDEO: read the guest's rendered EYE texture just before we
    // copy it to the compositor. Bright here => the guest DID render the video,
    // and the black is between this and the display; dark here => the guest's
    // draw itself produced black from the (proven-bright) video texture.
    klfb_probe_named_tex("EYE", src_tex, src_layer, w, h);

    // KL_VRR_MIRROR=1 (vivecraft 26.x, launch 10): the copy must be RAW.
    //
    // The source slice and our destination both carry the one size-rule map, so
    // ANGLE marks the blit a physical passthrough — but its blit shader honours
    // that flag for a 2D source only; an ARRAY-SLICE source (this one) is still
    // sampled through screen space, under a foveated destination pass. The
    // already-warped picture is warped a second time, and the compositor's one
    // unwarp leaves it in physical layout: squeezed into the physical rect
    // (3932x2664 of 5088x4081: menu low, squash on pitch) with stale texels in
    // the rest — launch 5's letterbox, filled with Minecraft instead of black.
    // Host repro: `make vrr` [Q7] (lit 125 where 189 is right).
    //
    // So for the copy alone, NEITHER side carries a map: the size rule and our
    // destination's own registration come off, the blit is an ordinary unscaled
    // NEAREST draw (texel for texel, physical layout kept), and both go back on.
    // The destination is re-attached so ANGLE re-reads its (now absent) map; the
    // guest's FBOs keep the descriptors they cached, so nothing of theirs moves.
    // Costs one unfoveated full-size copy per eye — launch 9's mirror cost.
    int raw = mw == w && mh == h && src_layer >= 0 && klfb_vrr_mirror_on() &&
              g_eye_rate_map && a_SetRateMap && a_SetRateMapForSize &&
              w == g_eye_rate_w && h == g_eye_rate_h;
    void *dst_mtl = g_eye_mtl[eye][slot].tex;
    if (raw) {
        static int said[2][KL_MTL_MAX_STAGES];
        if (slot < KL_MTL_MAX_STAGES && !said[eye][slot]++)
            fprintf(stderr, "  [glfb] vrr mirror: eye=%d slot=%d copies swapchain slice %d "
                            "RAW (maps off for the copy: ANGLE's array-slice blit has no "
                            "physical passthrough, it would warp twice) — the mirror holds "
                            "the physical picture and the compositor unwarps it\n",
                    eye, slot, src_layer);
        a_SetRateMapForSize((uint32_t)w, (uint32_t)h, klfb_rate_min_samples(), NULL);
        if (dst_mtl) a_SetRateMap(dst_mtl, NULL);
    }
    // Identity copy at 1:1 (NEAREST, exact); an upscale into a larger eye
    // texture goes through the LINEAR sub-rect blit, filling it whole.
    if (klfb_pass_labels_on()) kl_perf_pass_label(eye ? "kl mirror copy R" : "kl mirror copy L");
    uint32_t e = (mw == w && mh == h)
        ? klfb_mirror_blit(src_tex, src_layer, m->tex, w, h, raw)
        : klfb_blit_subrect(src_tex, src_layer, m->tex, w, h, 0, 0, mw, mh);
    klfb_pass_label_fb(g_draw_fb);
    if (raw) {
        // Re-read: put back what is in force NOW, not what was snapshotted.
        void *map = g_eye_rate_map;
        if (map && g_eye_rate_w == w && g_eye_rate_h == h) {
            a_SetRateMapForSize((uint32_t)w, (uint32_t)h, klfb_rate_min_samples(), map);
            if (dst_mtl && g_eye_mtl[eye][slot].w == w && g_eye_mtl[eye][slot].h == h)
                a_SetRateMap(dst_mtl, map);
        }
    }
    if (e) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] mirror eye=%d slot=%d: glBlitFramebuffer -> "
                            "GL error 0x%x\n", eye, slot, e);
        return -1;
    }
    return slot;
}

// Lay a foveal inset over the base eye already mirrored into (eye, stage). The
// caller (the OpenXR layer loop) has computed, from the two layers' fields of
// view, where the inset's frustum falls inside the base's — as fractions of the
// eye in [0,1] — so this only turns those into pixels and blits. No-op until the
// base is present (m->tex): the inset is meaningless on its own.
void kl_glfb_overlay_eye_inset(int eye, int stage, uint32_t tex, int layer,
                               int iw, int ih,
                               float nx0, float ny0, float nx1, float ny1) {
    if (!g_mtl_provider || !tex || iw <= 0 || ih <= 0) return;
    if (eye < 0 || eye > 1 || stage < 0 || stage >= KL_MTL_MAX_STAGES) return;
    __typeof__(g_eye_mirror[0][0]) *m = &g_eye_mirror[eye][stage];
    if (!m->tex || m->w <= 0 || m->h <= 0) return;    // base not mirrored yet
    if (nx0 < 0) nx0 = 0; if (nx0 > 1) nx0 = 1;
    if (nx1 < 0) nx1 = 0; if (nx1 > 1) nx1 = 1;
    if (ny0 < 0) ny0 = 0; if (ny0 > 1) ny0 = 1;
    if (ny1 < 0) ny1 = 0; if (ny1 > 1) ny1 = 1;
    int dx0 = (int)(nx0 * m->w + 0.5f), dx1 = (int)(nx1 * m->w + 0.5f);
    int dy0 = (int)(ny0 * m->h + 0.5f), dy1 = (int)(ny1 * m->h + 0.5f);
    if (dx1 <= dx0 || dy1 <= dy0) return;
    if (!klfb_mirror_resolve(layer >= 0)) return;

    // Feather the edge into a soft round centre, unless disabled or the inset is
    // an ARRAY slice (the feather shader samples a plain sampler2D). The feather
    // returns nonzero if its program could not be built, so a straight blit is
    // always the fallback — the sharp centre still lands, just with a hard edge.
    static int feather = -1, inner_x100 = -1;
    if (feather < 0)     feather = kl_env_on("KL_XR_FOVEA_FEATHER", 1);
    // Higher = WIDER fully-sharp core before the fade begins (0..100). 78 keeps
    // most of the inset crisp with the fade concentrated at the very rim.
    if (inner_x100 < 0)  inner_x100 = kl_env_int("KL_XR_FOVEA_INNER", 78);
    const char *how = "blit";
    uint32_t e = 0;
    if (feather && layer < 0 &&
        klfb_feather_blit(m->tex, m->w, m->h, tex, dx0, dy0, dx1, dy1,
                          inner_x100 / 100.0f) == 0) {
        how = "feather";
    } else {
        e = klfb_blit_subrect(tex, layer, m->tex, iw, ih, dx0, dy0, dx1, dy1);
    }
    static int said;
    if (said++ < 6)
        fprintf(stderr, "  [glfb] fovea inset eye=%d stage=%d: %dx%d -> "
                        "eye rect [%d,%d..%d,%d] of %dx%d (%s)%s\n",
                eye, stage, iw, ih, dx0, dy0, dx1, dy1, m->w, m->h, how,
                e ? " GL error" : "");
}

// ---------------------------------------------------------------------------
// The decoded-video image. An AHardwareBuffer — which is one of our
// CVPixelBuffers, see kl_mediandk.h — sampled as a GL texture.
//
// The guest's sequence is Android's, and every step of it is missing here:
//
//     buf = eglGetNativeClientBufferANDROID(ahardwarebuffer)
//     img = eglCreateImageKHR(dpy, NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, buf, ...)
//     glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex)
//     glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img)
//
// ANGLE's Metal backend has no external images at all (DisplayMtl.mm:984)
// and no notion of an Android buffer, so there is nothing to forward to. What it
// *does* have is EGL_ANGLE_iosurface_client_buffer, and a VideoToolbox pixel
// buffer is IOSurface-backed by construction (kl_vtdec.c asks for it) — so the
// image becomes a pbuffer over the same IOSurface, and the target call becomes
// eglBindTexImage. DisplayMtl's own comment is the licence: "Metal can bind
// IOSurfaces to regular 2D textures", which is why its configs report
// bindToTextureTarget == EGL_TEXTURE_2D rather than the rectangle target the
// desktop-GL backend needs.
//
// The guest cannot tell the difference. Its sampler is already a plain
// `sampler2D` on GL_TEXTURE_2D by the time this runs (klfb_rewrite_glsl and
// klfb_detarget did that), and BGRA output means the YUV→RGB conversion
// an external image would have promised has already happened in the decoder.
#define EGL_IOSURFACE_ANGLE_        0x3454
#define EGL_IOSURFACE_PLANE_ANGLE_  0x345A
#define EGL_TEXTURE_TYPE_ANGLE_     0x345C
#define EGL_TEXTURE_FORMAT_         0x3080
#define EGL_TEXTURE_RGBA_           0x305E
#define EGL_TEXTURE_TARGET_         0x3081
#define EGL_TEXTURE_2D_EGL_         0x305F
#define EGL_BACK_BUFFER_            0x3084
#define GL_BGRA_EXT_                0x80E1
#define GL_UNSIGNED_BYTE_           0x1401
#define GL_TEXTURE_BINDING_2D_      0x8069

static void *(*a_eglCreatePbufferFromClientBuffer)(void *, uint32_t, void *, void *,
                                                   const int32_t *);
static unsigned (*a_eglDestroySurface_img)(void *, void *);
static unsigned (*a_eglBindTexImage)(void *, void *, int32_t);
static unsigned (*a_eglReleaseTexImage)(void *, void *, int32_t);
static int32_t  (*a_eglGetError_img)(void);
static void     (*a_glTexParameteri_img)(uint32_t, uint32_t, int32_t);
static void     (*a_glGetTexParameteriv_img)(uint32_t, uint32_t, int32_t *);

typedef struct klfb_image {
    uint32_t magic;
    // Exactly one of the two storages is set, and it is the frame's pixel
    // format that chose: BGRA (KL_VTDEC_BGRA=1, or the Simulator) rides an
    // IOSurface pbuffer bound with eglBindTexImage; the decoder's native
    // biplanar YCbCr rides an EGLImage over a private-format MTLTexture,
    // targeted with glEGLImageTargetTexture2DOES.
    void    *pbuf;              // the EGLSurface over the IOSurface
    void    *img;               // the EGLImage over the MTLTexture
    void    *cvtex;             // ...whose storage the CVMetalTexture keeps alive
    void    *pixels;            // ...over this retained CVPixelBuffer
    int      w, h;
    uint32_t bound_tex;         // the GL name it is bound to, 0 if none
    struct klfb_image *next;
} klfb_image;

#define KLFB_IMAGE_MAGIC 0x4b4c4749u   /* 'KLGI' */

// A registry rather than a bare magic check, because kl_glfb_is_image() is asked
// about pointers the guest chose: an EGLImage from somewhere else is a valid
// object we must forward rather than dereference for a magic word.
static klfb_image  *g_images;
static pthread_mutex_t g_images_lk = PTHREAD_MUTEX_INITIALIZER;

static int img_resolve(void) {
    if (a_eglCreatePbufferFromClientBuffer) return 1;
    if (!g_ready) return 0;
    a_eglCreatePbufferFromClientBuffer = asym("eglCreatePbufferFromClientBuffer");
    a_eglDestroySurface_img            = asym("eglDestroySurface");
    a_eglBindTexImage                  = asym("eglBindTexImage");
    a_eglReleaseTexImage               = asym("eglReleaseTexImage");
    a_eglGetError_img                  = asym("eglGetError");
    a_glTexParameteri_img              = asym("glTexParameteri");
    a_glGetTexParameteriv_img          = asym("glGetTexParameteriv");
    return a_eglCreatePbufferFromClientBuffer && a_eglBindTexImage
        && a_eglReleaseTexImage && a_eglDestroySurface_img;
}

int kl_glfb_is_image(const void *h) {
    if (!h) return 0;
    int found = 0;
    pthread_mutex_lock(&g_images_lk);
    for (klfb_image *i = g_images; i; i = i->next)
        if (i == h) { found = 1; break; }
    pthread_mutex_unlock(&g_images_lk);
    return found;
}

// The decoder's native biplanar YCbCr, as ONE texture whose SAMPLER performs
// the YUV->RGB conversion — which is the promise samplerExternalOES makes on
// Android, kept in hardware. The pixel-format numbers are Apple's private
// single-plane YCbCr MTLPixelFormats (WebKit's MetalSPI.h; the patched ANGLE's
// format table knows them). The NON-sRGB variants, deliberately: sampling
// returns the transfer-encoded code values, exactly what the BGRA path
// returned, so the meaning of what the guest writes into its eye texture does
// not move (the sRGB variants would hand the shader linear instead).
//
// The 10-bit row is the PACKED family, not P010: packed (as its
// lossless/lossy compressed forms) is what VideoToolbox actually hands back
// for a 10-bit stream when no format is requested — a Steam Link HEVC Main10
// session decodes to '&xf0'. The unpacked biplanar 10-bit forms are never
// produced unconstrained, so they are deliberately unmapped: anything outside
// these families hits the refusal below, which names the fourcc, and
// KL_VTDEC_BGRA=1 is the escape hatch.
static unsigned long klfb_video_mtl_format(uint32_t pf) {
    switch (pf) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
    case kCVPixelFormatType_Lossless_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_Lossless_420YpCbCr8BiPlanarFullRange:
    case kCVPixelFormatType_Lossy_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_Lossy_420YpCbCr8BiPlanarFullRange:
        return 500;             // MTLPixelFormatYCBCR8_420_2P
    case kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarVideoRange:
    case kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarFullRange:
    case kCVPixelFormatType_Lossy_420YpCbCr10PackedBiPlanarVideoRange:
        return 508;             // MTLPixelFormatYCBCR10_420_2P_PACKED
    default:
        return 0;
    }
}

static void *klfb_image_from_native(void *pixels, uint32_t pf,
                                    int *out_w, int *out_h) {
    unsigned long fmt = klfb_video_mtl_format(pf);
    if (!fmt) {
        static unsigned said;
        if (said++ < 4)
            fprintf(stderr, "  [glfb] decoded frame is pixel format '%c%c%c%c', which "
                            "has no single-plane YCbCr MTLPixelFormat here — "
                            "KL_VTDEC_BGRA=1 restores the BGRA path\n",
                    (char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8), (char)pf);
        return NULL;
    }
    if (!mtl_resolve()) {
        fprintf(stderr, "  [glfb] no ANGLE Metal interop entry points — a native-"
                        "format frame cannot be turned into a texture\n");
        return NULL;
    }
    void *cvtex = NULL;
    void *tex = kl_cvmtl_texture(pixels, fmt, &cvtex);
    if (!tex) return NULL;
    const int32_t attrs[] = { EGL_NONE_ };
    void *img = a_eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT_, EGL_METAL_TEXTURE_ANGLE_,
                                    tex, attrs);
    if (img == EGL_NO_IMAGE_) {
        fprintf(stderr, "  [glfb] eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE) failed "
                        "for a '%c%c%c%c' video frame — is the vendored ANGLE the "
                        "patched one (it knows the private YCbCr formats by "
                        "number)?\n",
                (char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8), (char)pf);
        kl_cvmtl_release(cvtex);
        return NULL;
    }
    klfb_image *im = calloc(1, sizeof *im);
    if (!im) {
        if (a_eglDestroyImageKHR) a_eglDestroyImageKHR(g_dpy, img);
        kl_cvmtl_release(cvtex);
        return NULL;
    }
    im->magic  = KLFB_IMAGE_MAGIC;
    im->img    = img;
    im->cvtex  = cvtex;
    im->pixels = pixels;
    CVPixelBufferRetain((CVPixelBufferRef)pixels);
    im->w = (int)CVPixelBufferGetWidth((CVPixelBufferRef)pixels);
    im->h = (int)CVPixelBufferGetHeight((CVPixelBufferRef)pixels);
    pthread_mutex_lock(&g_images_lk);
    im->next = g_images;
    g_images = im;
    pthread_mutex_unlock(&g_images_lk);
    if (out_w) *out_w = im->w;
    if (out_h) *out_h = im->h;
    static unsigned made;
    if (made++ < 4)
        fprintf(stderr, "  [glfb] video image %p: '%c%c%c%c' frame as a %dx%d "
                        "YCbCr MTLTexture (format %lu, sampler converts)\n",
                (void *)im, (char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8),
                (char)pf, im->w, im->h, fmt);
    return im;
}

void *kl_glfb_image_from_pixels(void *pixels, int *out_w, int *out_h) {
    if (!pixels) return NULL;
    if (!kl_glfb_init() || !img_resolve()) {
        fprintf(stderr, "  [glfb] no ANGLE IOSurface entry points — a decoded frame "
                        "cannot be turned into a texture (is KL_GLFB=1?)\n");
        return NULL;
    }
    // The frame's own pixel format picks the storage: anything that is not BGRA
    // goes to the Metal-texture route, whose format switch is the authority on
    // what is servable. BGRA continues below on the IOSurface pbuffer.
    OSType pf = CVPixelBufferGetPixelFormatType((CVPixelBufferRef)pixels);
    if (pf != kCVPixelFormatType_32BGRA)
        return klfb_image_from_native(pixels, (uint32_t)pf, out_w, out_h);
    IOSurfaceRef surf = CVPixelBufferGetIOSurface((CVPixelBufferRef)pixels);
    if (!surf) {
        fprintf(stderr, "  [glfb] the decoded frame is not IOSurface-backed — "
                        "kCVPixelBufferIOSurfacePropertiesKey is what makes it so\n");
        return NULL;
    }
    // The attribute list below states BGRA/UNSIGNED_BYTE to ANGLE, and ANGLE only
    // WARNs when the IOSurface disagrees (IOSurfaceSurfaceMtl::ValidateAttributes
    // compares bytes-per-element, which every 4-byte format passes). The pixel
    // format was checked above, so a decoder handing back something else routes
    // to the Metal path rather than being described here as what it is not.
    int w = (int)CVPixelBufferGetWidth((CVPixelBufferRef)pixels);
    int h = (int)CVPixelBufferGetHeight((CVPixelBufferRef)pixels);
    const int32_t attrs[] = {
        EGL_WIDTH,                          w,
        EGL_HEIGHT,                         h,
        EGL_IOSURFACE_PLANE_ANGLE_,         0,
        EGL_TEXTURE_TARGET_,                EGL_TEXTURE_2D_EGL_,
        EGL_TEXTURE_FORMAT_,                EGL_TEXTURE_RGBA_,
        EGL_TEXTURE_TYPE_ANGLE_,            GL_UNSIGNED_BYTE_,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE_, GL_BGRA_EXT_,
        EGL_NONE_,
    };
    // ANGLE validates all six of those as REQUIRED for EGL_IOSURFACE_ANGLE and
    // fails the call if any is missing, so this list is a contract, not a
    // preference.
    void *pbuf = a_eglCreatePbufferFromClientBuffer(g_dpy, EGL_IOSURFACE_ANGLE_,
                                                    (void *)surf, g_cfg, attrs);
    if (!pbuf) {
        fprintf(stderr, "  [glfb] eglCreatePbufferFromClientBuffer(EGL_IOSURFACE_ANGLE) "
                        "failed for %dx%d — EGL error 0x%x\n",
                w, h, a_eglGetError_img ? a_eglGetError_img() : 0);
        return NULL;
    }
    klfb_image *im = calloc(1, sizeof *im);
    if (!im) { a_eglDestroySurface_img(g_dpy, pbuf); return NULL; }
    im->magic = KLFB_IMAGE_MAGIC;
    im->pbuf  = pbuf;
    im->w = w; im->h = h;
    pthread_mutex_lock(&g_images_lk);
    im->next = g_images;
    g_images = im;
    pthread_mutex_unlock(&g_images_lk);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    static unsigned made;
    if (made++ < 4)
        fprintf(stderr, "  [glfb] video image %p: IOSurface %p as a %dx%d BGRA pbuffer\n",
                (void *)im, (void *)surf, w, h);
    return im;
}

// The other half of the detarget, and it is the difference between a picture and
// a black rectangle.
//
// GL_OES_EGL_image_external specifies its OWN sampler defaults, and they are not
// GL_TEXTURE_2D's: MIN_FILTER defaults to LINEAR (external textures have no
// mipmaps at all) and WRAP_S/T to CLAMP_TO_EDGE. A guest targeting Android is
// entitled to rely on them and never set any of it. Once we retarget the same
// texture to GL_TEXTURE_2D, the defaults it inherits are NEAREST_MIPMAP_LINEAR
// and REPEAT — and a 2D texture with a mipmap min filter and no mip levels is
// *incomplete*, which samples as BLACK with no GL error anywhere. The frame
// arrives, the bind succeeds, the draw succeeds, and nothing is on screen.
//
// So the external defaults are applied here, and only where the state is still
// the 2D default: a guest that did set a filter deliberately keeps it. That
// test is what stops this being a blanket override of the guest's choices.
#define GL_TEXTURE_2D_IMG_     0x0DE1
#define GL_TEXTURE_MIN_FILTER_ 0x2801
#define GL_TEXTURE_MAG_FILTER_ 0x2800
#define GL_TEXTURE_WRAP_S_     0x2802
#define GL_TEXTURE_WRAP_T_     0x2803
#define GL_NEAREST_MIPMAP_LINEAR_ 0x2702
#define GL_LINEAR_             0x2601
#define GL_REPEAT_             0x2901
#define GL_CLAMP_TO_EDGE_      0x812F
static void klfb_external_defaults(int32_t tex) {
    if (!a_glTexParameteri_img || !a_glGetTexParameteriv_img) return;
    int32_t v = 0;
    a_glGetTexParameteriv_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_MIN_FILTER_, &v);
    if (v == GL_NEAREST_MIPMAP_LINEAR_) {          // untouched 2D default
        a_glTexParameteri_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
        static unsigned said;
        if (said++ < 2)
            fprintf(stderr, "  [glfb] texture %d had the GL_TEXTURE_2D mipmap min "
                            "filter and no mip levels — incomplete, and would have "
                            "sampled black; set to LINEAR, which is what "
                            "GL_TEXTURE_EXTERNAL_OES defaults to\n", tex);
    }
    a_glGetTexParameteriv_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_WRAP_S_, &v);
    if (v == GL_REPEAT_)
        a_glTexParameteri_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
    a_glGetTexParameteriv_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_WRAP_T_, &v);
    if (v == GL_REPEAT_)
        a_glTexParameteri_img(GL_TEXTURE_2D_IMG_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    (void)GL_TEXTURE_MAG_FILTER_;   // its 2D default IS external's (LINEAR)
}

// eglBindTexImage binds to whatever is bound to GL_TEXTURE_2D on the active unit
// of the calling thread's context — which is exactly where the guest's
// glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex) landed after klfb_detarget. So the
// only work here is the bookkeeping ANGLE will not do for us: a surface may be
// bound to at most one texture, and binding a second surface into a texture that
// already has one is undefined (Texture::bindTexImageFromSurface asserts on it).
int kl_glfb_image_bind(void *image) {
    klfb_image *im = image;
    if (!kl_glfb_is_image(im) || !img_resolve()) return 0;
    int32_t tex = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(GL_TEXTURE_BINDING_2D_, &tex);
    if (!tex) {
        fprintf(stderr, "  [glfb] video image %p: no texture bound to GL_TEXTURE_2D — "
                        "the guest's bind did not reach us\n", image);
        return 0;
    }
    if (im->bound_tex == (uint32_t)tex) return 1;      // already there
    pthread_mutex_lock(&g_images_lk);
    if (im->bound_tex && im->pbuf) {
        a_eglReleaseTexImage(g_dpy, im->pbuf, EGL_BACK_BUFFER_);
        im->bound_tex = 0;
    }
    // Whatever else believed it was this texture's storage is not any more: a
    // pbuffer is released, an EGLImage merely forgets (retargeting replaces the
    // storage wholesale, but a stale bound_tex would satisfy the "already
    // there" shortcut above and skip a real rebind).
    for (klfb_image *o = g_images; o; o = o->next)
        if (o != im && o->bound_tex == (uint32_t)tex) {
            if (o->pbuf) a_eglReleaseTexImage(g_dpy, o->pbuf, EGL_BACK_BUFFER_);
            o->bound_tex = 0;
        }
    pthread_mutex_unlock(&g_images_lk);
    while (a_glGetError && a_glGetError()) {}
    if (im->img) {
        // The Metal route: the frame is an EGLImage, and the guest's texture
        // takes it as its storage. Same call the eye path makes.
        a_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D_IMG_, im->img);
        uint32_t e = a_glGetError ? a_glGetError() : 0;
        if (e) {
            static int esaid;
            if (esaid++ < 8)
                fprintf(stderr, "  [glfb] glEGLImageTargetTexture2DOES(video image "
                                "%p -> texture %d) -> GL error 0x%x\n",
                        image, tex, e);
            return 0;
        }
        im->bound_tex = (uint32_t)tex;
        klfb_external_defaults(tex);
        static unsigned mbound;
        if (mbound++ < 4)
            fprintf(stderr, "  [glfb] video image %p is now texture %d (%dx%d, "
                            "YCbCr sampled in hardware)\n", image, tex, im->w, im->h);
        return 1;
    }
    if (!a_eglBindTexImage(g_dpy, im->pbuf, EGL_BACK_BUFFER_)) {
        static int said;
        if (said++ < 8)
            fprintf(stderr, "  [glfb] eglBindTexImage(video image %p -> texture %d) "
                            "failed, EGL error 0x%x\n", image, tex,
                    a_eglGetError_img ? a_eglGetError_img() : 0);
        return 0;
    }
    im->bound_tex = (uint32_t)tex;
    klfb_external_defaults(tex);
    static unsigned bound;
    if (bound++ < 4)
        fprintf(stderr, "  [glfb] video image %p is now texture %d (%dx%d)\n",
                image, tex, im->w, im->h);

    // KL_GLFB_PROBE_VIDEO=1: is the decoded frame actually IN this texture?
    // (proven yes — bright, changing content — which is why the eye probe in the
    // mirror path is now the interesting one.)
    klfb_probe_named_tex("VIDEO", (uint32_t)tex, 0, im->w, im->h);
    return 1;
}

void kl_glfb_image_destroy(void *image) {
    klfb_image *im = image;
    if (!kl_glfb_is_image(im)) return;
    pthread_mutex_lock(&g_images_lk);
    for (klfb_image **p = &g_images; *p; p = &(*p)->next)
        if (*p == im) { *p = im->next; break; }
    pthread_mutex_unlock(&g_images_lk);
    // Release before destroy: a surface still bound to a texture keeps ANGLE's
    // reference on the IOSurface, and the texture would go on sampling storage
    // the decoder has recycled.
    if (im->pbuf) {
        if (im->bound_tex && a_eglReleaseTexImage)
            a_eglReleaseTexImage(g_dpy, im->pbuf, EGL_BACK_BUFFER_);
        if (a_eglDestroySurface_img) a_eglDestroySurface_img(g_dpy, im->pbuf);
    }
    if (im->img) {
        // A texture still holding this image as storage keeps it (EGL images
        // are refcounted under their siblings), so destruction order is free
        // here; the CVMetalTexture and the pixel buffer go with the image.
        if (a_eglDestroyImageKHR) a_eglDestroyImageKHR(g_dpy, im->img);
        kl_cvmtl_release(im->cvtex);
        CVPixelBufferRelease((CVPixelBufferRef)im->pixels);
    }
    im->magic = 0;
    free(im);
}

static void (*g_real_BlitFramebuffer)(int32_t, int32_t, int32_t, int32_t, int32_t,
                                      int32_t, int32_t, int32_t, uint32_t, uint32_t);

static void klfb_errprobe(const char *what, const char *detail);
// Defined with the capture below; the blit probe uses it. dump/dw/dh are an
// optional pixel out: when dump is non-NULL the probe tone-maps the readback
// into it (g_w*g_h*4 bytes, bottom-up rows) and reports the clipped size.
// hint_w/hint_h are the size to read when the attachment's own size is not
// known — see the definition; 0,0 means "no hint".
static unsigned long klfb_probe_fbo(uint32_t fb, float *fbuf, uint8_t *bbuf,
                                    char *note, size_t note_n,
                                    uint8_t *dump, int32_t *dw, int32_t *dh,
                                    int32_t hint_w, int32_t hint_h);
static uint32_t klfb_read_from_texture_layer(uint32_t tex, int layer);

// ----------------------------------------------- r32: the coordinate thunks
//
// Everything the render-target supersample has to move so the guest's picture
// stays where the guest thinks it is. Before this round NONE of these entry
// points had a thunk: glViewport, glScissor, glReadPixels, glCopyTexSubImage2D
// and glInvalidateSubFramebuffer went straight to the driver, and every
// instrument in this file that wanted the viewport asked ANGLE for it with
// glGetIntegerv(GL_VIEWPORT) rather than watching the guest set it.
//
// THE INERT GUARANTEE. `g_rt_draw_scale` and `g_rt_read_scale` are 1 unless
// KL_GLFB_RT_SCALE named a size that was actually allocated, so on every target
// but tetris — and on tetris with the knob cleared — each of these is a single
// `<= 1` compare in front of the forwarding call, with the SAME arguments the
// guest passed. Nothing about them is title-specific; the title-specific part
// is entirely in the knob.
static void (*g_real_Viewport)(int32_t, int32_t, int32_t, int32_t);
static void (*g_real_Scissor)(int32_t, int32_t, int32_t, int32_t);
static void (*g_real_ReadPixels)(int32_t, int32_t, int32_t, int32_t,
                                 uint32_t, uint32_t, void *);
static void (*g_real_CopyTexSubImage2D)(uint32_t, int32_t, int32_t, int32_t,
                                        int32_t, int32_t, int32_t, int32_t);
static void (*g_real_InvalidateSubFramebuffer)(uint32_t, int32_t,
                                               const uint32_t *, int32_t,
                                               int32_t, int32_t, int32_t);

// The guest's current viewport, per thread, read by the draw census. Per thread
// because that is what GL state is — two contexts on two threads have two
// viewports — and because it makes the draw path free: no glGetIntegerv, no
// round trip, just a read of a thread-local the guest itself last wrote.
//
// `set` distinguishes "the guest chose this" from the GL default, which is the
// surface size and tells us nothing about intent. Recorded UNSCALED: the census
// answers "what did the guest ask for", not what r32 then multiplied it by.
static __thread int32_t g_vp[4];
static __thread int g_vp_set;

static void klfb_Viewport(int32_t x, int32_t y, int32_t w, int32_t h) {
    g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h;
    g_vp_set = 1;
    int s = g_rt_draw_scale;
    if (s > 1) {
        static unsigned said;
        if (said < 4) {
            said++;
            fprintf(stderr, "  [glfb] RT supersample: viewport %d,%d %dx%d -> "
                            "%d,%d %dx%d\n", x, y, w, h,
                    x * s, y * s, w * s, h * s);
        }
        x *= s; y *= s; w *= s; h *= s;
    }
    if (g_real_Viewport) g_real_Viewport(x, y, w, h);
}

// ---- the scissor box that is zero, and the world that is therefore black ----
//
// GL_SCISSOR_TEST with a box of width or height 0 discards EVERY fragment --
// draws and glClear alike. It raises no error, leaves the framebuffer complete,
// and lets every counter in the guest come out right. A whole world drawn into
// a target that never receives one pixel of it, including its own sky-blue
// clear, is exactly what launch #7 measured: both eye images pure zero,
// `clear (0.604 0.761 0.871 1.0)` on the passes around it, and 2.3 million
// draws that painted nothing.
//
// And launch #7's draw-state census reads `scissor 1 box 0,0 0x0` -- the box is
// ZERO on every line it ever printed, in the front end and in the world.
//
// A box is zero for one of two reasons and they need different words:
//
//   * THE GUEST NEVER SET ONE. ES says the initial scissor box is the size of
//     the surface the context was first made current to. Ours is an ANGLE
//     pbuffer, and a guest that enables the test relying on "the default box is
//     the whole surface" -- which is what it is on a real Android window --
//     gets whatever that pbuffer's dimensions were.
//   * THE GUEST COMPUTED ONE FROM A WINDOW SIZE WE ANSWERED AS ZERO. A VR guest
//     asks SDL or EGL how big its window is while rendering into an eye
//     framebuffer that has nothing to do with it.
//
// The obvious repair is to substitute the VIEWPORT for a degenerate box, and
// this runtime can do it -- KL_GLFB_SCISSOR_FIX=1.
//
// IT IS OFF BY DEFAULT BECAUSE IT WAS TESTED AND IT IS WRONG. Two host runs,
// same delay, same spacing, same three captures:
//
//   KL_GLFB_SCISSOR_FIX=0   eye0 239036 / 239009 / 239109 lit — the menu
//   KL_GLFB_SCISSOR_FIX=1   eye0      0 /      0 /      0 lit — black
//
// Repairing the box does not reveal a picture, it DESTROYS one. So the draws
// the zero box was discarding are not missing scenery: at least one of them
// paints the whole target black under this renderer, and the degenerate scissor
// has been the only reason the front end was ever visible. That is a lead, and
// it is the opposite of the fix it looked like.
//
// The diagnostics stay on regardless, because the fact itself is worth having
// in every log: this guest NEVER CALLS glScissor and inherits whatever box the
// context was born with, which on a real Android window would be the window and
// here is zero.
static unsigned g_scissor_calls;        // the guest set a box itself
static unsigned g_scissor_zero_asked;   // ...and it was degenerate
static unsigned g_scissor_repaired;     // we substituted the viewport
static unsigned g_scissor_said;
static int32_t  g_scissor_last[4];

static int klfb_scissor_fix_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_SCISSOR_FIX", 0);
    return on;
}

// Substitute the viewport for a degenerate box. Returns 1 if it rewrote.
static int klfb_scissor_repair(int32_t *x, int32_t *y, int32_t *w, int32_t *h,
                               const char *who) {
    if (*w > 0 && *h > 0) return 0;
    if (!klfb_scissor_fix_on() || !a_glGetIntegerv) return 0;
    int32_t vp[4] = {0,0,0,0};
    a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return 0;
    if (g_scissor_said++ < 6) {
        int32_t dfb = -1;
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &dfb);
        fprintf(stderr, "  [glfb] SCISSOR BOX IS %d,%d %dx%d at %s (fb%d) — a "
                        "zero-size scissor discards every fragment, clears "
                        "included. Substituting the viewport %d,%d %dx%d "
                        "(KL_GLFB_SCISSOR_FIX=0 keeps the guest's box)\n",
                *x, *y, *w, *h, who, dfb, vp[0], vp[1], vp[2], vp[3]);
    }
    *x = vp[0]; *y = vp[1]; *w = vp[2]; *h = vp[3];
    g_scissor_repaired++;
    return 1;
}

static void klfb_Scissor(int32_t x, int32_t y, int32_t w, int32_t h) {
    int s = g_rt_draw_scale;
    if (s > 1) { x *= s; y *= s; w *= s; h *= s; }
    g_scissor_calls++;
    if (w <= 0 || h <= 0) g_scissor_zero_asked++;
    // Every glScissor the guest makes, with the caller, for as long as there
    // are only a handful of them. Two calls in a whole launch is not a hot
    // path, and "what exactly did it ask for, and from where" is the question.
    if (g_scissor_calls <= 8) {
        fprintf(stderr, "  [glfb] glScissor(%d, %d, %d, %d) <- %s\n",
                x, y, w, h, klfb_caller(__builtin_return_address(0)));
        // ...and the frames above it, because the immediate caller is
        // pglContext::SetScissor and the question is who handed IT a rect.
        // AAPCS64: [fp] is the previous fp, [fp+8] is that frame's return
        // address. Guest arm64 keeps frame pointers; a chain that walks off is
        // simply shorter, which is why every step is bounded and checked.
        void **fp = (void **)__builtin_frame_address(0);
        for (int d = 0; d < 6 && fp; d++) {
            void *lr = fp[1];
            if (!lr) break;
            fprintf(stderr, "  [glfb]     frame %d: %s\n", d, klfb_caller(lr));
            void **next = (void **)fp[0];
            if (next <= fp || (uintptr_t)next & 7) break;
            fp = next;
        }
    }
    klfb_scissor_repair(&x, &y, &w, &h, "glScissor");
    g_scissor_last[0] = x; g_scissor_last[1] = y;
    g_scissor_last[2] = w; g_scissor_last[3] = h;
    if (g_real_Scissor) g_real_Scissor(x, y, w, h);
}

// ...and the other door into the same defect: the guest never calls glScissor
// at all and simply enables the test, inheriting whatever box the context was
// born with. Checked at the enable rather than at every draw, because that is
// the only moment the state changes and there are 2.3 million draws.
static void klfb_scissor_enable_check(void) {
    if (!a_glGetIntegerv || !g_real_Scissor) return;
    int32_t box[4] = {0,0,0,0};
    a_glGetIntegerv(0x0C10 /* SCISSOR_BOX */, box);
    if (box[2] > 0 && box[3] > 0) return;
    int32_t x = box[0], y = box[1], w = box[2], h = box[3];
    if (klfb_scissor_repair(&x, &y, &w, &h, "glEnable(GL_SCISSOR_TEST)"))
        g_real_Scissor(x, y, w, h);
}

void kl_glfb_scissor_census(FILE *f) {
    if (!g_scissor_calls && !g_scissor_repaired) return;
    fprintf(f, "  [glfb] scissor: %u glScissor call(s) from the guest, %u of them "
               "degenerate; %u box(es) repaired to the viewport; last box %d,%d "
               "%dx%d%s\n",
            g_scissor_calls, g_scissor_zero_asked, g_scissor_repaired,
            g_scissor_last[0], g_scissor_last[1], g_scissor_last[2],
            g_scissor_last[3],
            (!g_scissor_calls && g_scissor_repaired)
              ? "  <- the guest NEVER set a box; it inherited the context's"
              : "");
}

// A readback names a rectangle of the READ framebuffer, so it takes the read
// factor, not the draw one.
static void klfb_ReadPixels(int32_t x, int32_t y, int32_t w, int32_t h,
                            uint32_t format, uint32_t type, void *pixels) {
    KLFB_LAZY_FLUSH();
    int s = g_rt_read_scale;
    if (s > 1) {
        // The destination buffer is the guest's and is sized for the UNSCALED
        // rectangle, so the rect cannot simply be multiplied — that would write
        // s*s times as many pixels into it. Read the same number of pixels from
        // the scaled origin instead: a decimated read of the larger surface,
        // which is what the guest asked for at the resolution it can hold.
        x *= s; y *= s;
    }
    if (g_real_ReadPixels) g_real_ReadPixels(x, y, w, h, format, type, pixels);
    klfb_radar_note_read(x, y, w, h, format, type, pixels);  // r36, off by default
}

static void klfb_CopyTexSubImage2D(uint32_t target, int32_t level, int32_t xoff,
                                   int32_t yoff, int32_t x, int32_t y,
                                   int32_t w, int32_t h) {
    KLFB_LAZY_FLUSH();
    int s = g_rt_read_scale;                 // the SOURCE is the read fb
    if (s > 1) { x *= s; y *= s; }
    // ...and the DESTINATION offset scales only if the destination is itself a
    // scaled target, which is a different question with a different answer.
    if (target == 0x0DE1 /* TEXTURE_2D */) {
        int d = klfb_rt_scale_of_tex(g_bound2d);
        if (d > 1) { xoff *= d; yoff *= d; }
    }
    if (g_real_CopyTexSubImage2D)
        g_real_CopyTexSubImage2D(target, level, xoff, yoff, x, y, w, h);
}

static void klfb_InvalidateSubFramebuffer(uint32_t target, int32_t n,
                                          const uint32_t *attachments,
                                          int32_t x, int32_t y,
                                          int32_t w, int32_t h) {
    KLFB_LAZY_FLUSH();
    int s = klfb_is_draw_target(target) ? g_rt_draw_scale : g_rt_read_scale;
    if (s > 1) { x *= s; y *= s; w *= s; h *= s; }
    if (g_real_InvalidateSubFramebuffer)
        g_real_InvalidateSubFramebuffer(target, n, attachments, x, y, w, h);
}

// glInvalidateFramebuffer matters here because ANGLE's Metal backend actually
// discards (memoryless attachments): an invalidate of the scene color
// renderbuffer placed BEFORE the eye-resolve blit reads as "the blit copied
// black" downstream. The thunk exists to put the call on the probe timeline.
static void (*g_real_InvalidateFramebuffer)(uint32_t, int32_t, const uint32_t *);

static void klfb_InvalidateFramebuffer(int64_t target, int32_t n,
                                       const uint32_t *attachments) {
    KLFB_LAZY_FLUSH();
    if (kl_env_on("KL_GLFB_BLIT_PROBE", 0) && a_glGetIntegerv) {
        int32_t dfb = -1;
        a_glGetIntegerv(0x8CA6, &dfb);
        // Name the color attachment being discarded — an invalidate of "fb N's
        // COLOR_ATTACHMENT0" discards the *storage* behind it, which another
        // FBO can share. The eye blit reading black right after a lit scene
        // draw is only consistent with that.
        static void (*getfap_)(uint32_t, uint32_t, uint32_t, int32_t *);
        if (!getfap_) getfap_ = asym("glGetFramebufferAttachmentParameteriv");
        int32_t otype = 0, oname = 0;
        if (getfap_) {
            getfap_((uint32_t)target, 0x8CE0, 0x8CD0, &otype);
            getfap_((uint32_t)target, 0x8CE0, 0x8CD1, &oname);
        }
        fprintf(stderr, "  [glfb] glInvalidateFramebuffer(target=0x%llx, n=%d,"
                        " att0=0x%x) on draw_fb=%d color0=%s%d atts=",
                (long long)target, n, attachments ? attachments[0] : 0, dfb,
                otype == 0x8D41 ? "rb" : otype == 0x1702 ? "tex" : "?",
                oname);
        for (int32_t i = 0; i < n; i++)
            fprintf(stderr, "%s0x%x", i ? "," : "", attachments ? attachments[i] : 0);
        fprintf(stderr, "\n");
    }
    // KL_GLFB_NO_INVALIDATE=1: don't forward. ANGLE's Metal backend gives the
    // MSAA scene renderbuffers memoryless behavior — the depth+stencil-only
    // invalidate Unity issues after each eye's draws is the only call between
    // "draw probe reads the scene lit" and "the eye-resolve blit reads black",
    // so the discard's blast radius is the open question. Dropping the call
    // answers it: lit blits after this mean the invalidate was the eraser.
    static int no_inv = -1;
    if (no_inv < 0) no_inv = kl_env_on("KL_GLFB_NO_INVALIDATE", 0);
    if (g_real_InvalidateFramebuffer && !no_inv)
        g_real_InvalidateFramebuffer((uint32_t)target, n, attachments);
}

// The eye-blit-reads-black timeline work found the scene renderbuffer lit at
// draw time and black at blit time with no invalidate in between, so the
// eraser is a clear or a storage reallocation. These thunks put glClear and
// renderbuffer (re)storage on the same timeline. Logging is behind
// KL_GLFB_BLIT_PROBE; the forwarding is unconditional.
static void (*g_real_Clear)(uint32_t);
static void (*g_real_ClearColor)(float, float, float, float);
static void (*g_real_RenderbufferStorageMultisample)(uint32_t, int32_t, uint32_t,
                                                     int32_t, int32_t);
static void (*g_real_RenderbufferStorage)(uint32_t, uint32_t, int32_t, int32_t);

static int klfb_timeline(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_BLIT_PROBE", 0);
    return on;
}

// What a CLEAR is made with, on the same terms as the draw-state census.
//
// A clear is the one operation whose result is knowable without knowing
// anything about shading: if the target holds the clear colour, the pass
// reached it; if the target is zero and the clear colour was sky blue, nothing
// the pass did reached it at all. Launch #7 measured the second, which is why
// this exists. Budgeted and re-armed exactly like the draw census.
#define KLFB_DSTATE_FBS 12
static struct { int32_t fb; unsigned said; uint32_t sig; } g_cstate[KLFB_DSTATE_FBS];
static unsigned g_ncstate;

static void klfb_note_clear_state(uint32_t mask) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_DRAW_STATE", 1);
    if (!on || !a_glGetIntegerv) return;
    static int armed_at;
    if (!armed_at && __atomic_load_n(&g_shadow_pass_seen, __ATOMIC_RELAXED)) {
        armed_at = 1;
        for (unsigned k = 0; k < g_ncstate; k++) { g_cstate[k].said = 0; g_cstate[k].sig = 0; }
    }
    int32_t fb = -1;
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    unsigned i;
    for (i = 0; i < g_ncstate; i++) if (g_cstate[i].fb == fb) break;
    if (i == g_ncstate) {
        if (g_ncstate >= KLFB_DSTATE_FBS) return;
        g_cstate[g_ncstate].fb = fb;
        i = g_ncstate++;
    }
    if (g_cstate[i].said >= 6) return;

    static uint8_t (*a_IsEnabled2)(uint32_t);
    static void (*a_GetFloatv2)(uint32_t, float *);
    static int looked2;
    if (!looked2) {
        looked2 = 1;
        a_IsEnabled2 = asym("glIsEnabled");
        a_GetFloatv2 = asym("glGetFloatv");
    }
    int sc = (a_IsEnabled2 && a_IsEnabled2(0x0C11)) ? 1 : 0;
    int32_t box[4] = {0,0,0,0}, vp[4] = {0,0,0,0};
    a_glGetIntegerv(0x0C10 /* SCISSOR_BOX */, box);
    a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    float cc[4] = {0,0,0,0};
    if (a_GetFloatv2) a_GetFloatv2(0x0C22, cc);
    uint32_t sig = (uint32_t)(mask ^ (uint32_t)(sc << 20) ^
                              (uint32_t)(box[2] * 7 + box[3] * 13) ^
                              (uint32_t)(vp[2] * 31 + vp[3] * 17));
    if (g_cstate[i].said && sig == g_cstate[i].sig) return;
    g_cstate[i].sig = sig;
    g_cstate[i].said++;
    // ...and WHICH IMAGE this framebuffer is pointed at, because "the clear
    // happened" and "the clear happened into the picture the compositor is
    // handed" are different claims and only the second one matters. Both eyes
    // are slices of one texture name, so the layer is half the identity.
    int32_t oname = 0, olayer = 0, otype = 0;
    static void (*a_GetFbAtt)(uint32_t, uint32_t, uint32_t, int32_t *);
    if (!a_GetFbAtt) a_GetFbAtt = asym("glGetFramebufferAttachmentParameteriv");
    if (a_GetFbAtt) {
        a_GetFbAtt(0x8CA9 /* DRAW_FRAMEBUFFER */, 0x8CE0, 0x8CD0, &otype);
        if (otype) {
            a_GetFbAtt(0x8CA9, 0x8CE0, 0x8CD1 /* OBJECT_NAME */, &oname);
            if (otype == 0x1702)
                a_GetFbAtt(0x8CA9, 0x8CE0, 0x8CD4 /* TEXTURE_LAYER */, &olayer);
        }
    }
    fprintf(stderr, "  [glfb] clear fb%d mask 0x%x -> colour0 %s=%d layer=%d, "
                    "colour (%.3f %.3f %.3f %.3f) scissor %d box %d,%d %dx%d "
                    "viewport %d,%d %dx%d%s\n",
            fb, mask, otype == 0x1702 ? "tex" : otype ? "rb" : "none",
            oname, olayer, (double)cc[0], (double)cc[1], (double)cc[2],
            (double)cc[3], sc, box[0], box[1], box[2], box[3],
            vp[0], vp[1], vp[2], vp[3],
            (sc && (box[2] <= 0 || box[3] <= 0))
              ? "   <<< SCISSORED TO NOTHING — this clear paints no pixels"
              : "");
}

static void klfb_Clear(uint32_t mask) {
    KLFB_LAZY_FLUSH();
    klfb_note_clear_state(mask);
    klfb_ftrace_clear(mask);                                 // r26, no-op when off
    klfb_note_stage_write((int32_t)g_draw_fb, 1);
    klfb_note_fbo_use((int32_t)g_draw_fb, 1);
    if (klfb_timeline() && a_glGetIntegerv) {
        int32_t dfb = -1;
        a_glGetIntegerv(0x8CA6, &dfb);
        // The completeness of the framebuffer being cleared, and the error the
        // clear itself raises. A clear to a non-zero colour followed by a
        // readback of zero has exactly two readings — the clear did not happen,
        // or the readback is looking somewhere else — and only the first of
        // them leaves a trace, here.
        static uint32_t (*ck)(uint32_t);
        if (!ck) ck = asym("glCheckFramebufferStatus");
        uint32_t st = ck ? ck(0x8CA9) : 0;
        if (a_glGetError) while (a_glGetError()) {}
        {
            static void (*getfv)(uint32_t, float *);
            if (!getfv) getfv = asym("glGetFloatv");
            float cc[4] = {-1,-1,-1,-1};
            if (getfv) getfv(0x0C22 /* COLOR_CLEAR_VALUE */, cc);
            static int said_cc;
            if (said_cc++ < 3)
                fprintf(stderr, "  [glfb]   clear value as GL holds it: "
                                "%.3f %.3f %.3f %.3f (real glClear=%s, "
                                "real glClearColor=%s)\n",
                        cc[0], cc[1], cc[2], cc[3],
                        g_real_Clear ? "bound" : "NULL",
                        g_real_ClearColor ? "bound" : "NULL");
        }
        if (g_real_Clear) g_real_Clear(mask);
        uint32_t e = a_glGetError ? a_glGetError() : 0;
        // The three pieces of state that make a clear write nothing without
        // raising anything: the scissor box (a clear IS scissored), the colour
        // write mask, and rasteriser discard.
        int32_t sc[4] = {0,0,0,0}; uint8_t cm[4] = {1,1,1,1};
        int32_t sc_on = 0, rd = 0, vp[4] = {0,0,0,0};
        static uint8_t (*isen)(uint32_t);
        static void (*getbv)(uint32_t, uint8_t *);
        if (!isen) { isen = asym("glIsEnabled"); getbv = asym("glGetBooleanv"); }
        a_glGetIntegerv(0x0C10 /* SCISSOR_BOX */, sc);
        a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
        if (isen) { sc_on = isen(0x0C11 /* SCISSOR_TEST */);
                    rd = isen(0x8C89 /* RASTERIZER_DISCARD */); }
        if (getbv) getbv(0x0C23 /* COLOR_WRITEMASK */, cm);
        // ...and the fourth: a clear only touches the colour buffers ENABLED
        // FOR WRITING, so glDrawBuffers({GL_NONE}) makes both the clear and
        // every draw a silent no-op. It is per-framebuffer state, so it
        // survives every bind.
        int32_t db0 = 0, rb0 = 0;
        a_glGetIntegerv(0x8825 /* DRAW_BUFFER0 */, &db0);
        a_glGetIntegerv(0x0C02 /* READ_BUFFER */,  &rb0);
        fprintf(stderr, "  [glfb] glClear(mask=0x%x) on draw_fb=%d status=0x%x%s "
                        "scissor=%d(%d,%d %dx%d) mask=%d%d%d%d discard=%d vp=%dx%d "
                        "drawbuf0=0x%x readbuf=0x%x\n",
                mask, dfb, st, e ? " -> ERROR" : "", sc_on, sc[0], sc[1], sc[2], sc[3],
                cm[0], cm[1], cm[2], cm[3], rd, vp[2], vp[3], db0, rb0);
        if (e) fprintf(stderr, "  [glfb]   the clear raised 0x%x\n", e);
        // ...and read the cleared framebuffer straight back. This is the
        // readback path's own control: whatever the guest draws afterwards, the
        // colour it just cleared to is known, so a zero here indicts the
        // instrument and a non-zero one indicts the draws.
        // KL_GLFB_CLEAR_PROBE_N picks WHICH clear to read back. Not the first:
        // a run's first glClear happens during start-up, before the guest has
        // ever set a clear colour, so probing it reports GL's default
        // (0,0,0,1) and reads back an opaque black that is entirely correct —
        // an instrument answering honestly about the wrong frame.
        static int clear_n = -1, clears;
        if (clear_n < 0) clear_n = kl_env_int("KL_GLFB_CLEAR_PROBE_N", 200);
        if (++clears == clear_n) {
            static float *cf; static uint8_t *cb;
            static void (*cbind)(uint32_t, uint32_t);
            if (!cf) {
                cf = malloc((size_t)g_w * g_h * 16);
                cb = malloc((size_t)g_w * g_h * 4);
                cbind = asym("glBindFramebuffer");
            }
            if (cf && cb && cbind && dfb >= 0) {
                int32_t keep = -1;
                a_glGetIntegerv(0x8CAA, &keep);
                char n[160] = "";
                unsigned long l = klfb_probe_fbo((uint32_t)dfb, cf, cb, n, sizeof n,
                                                 NULL, NULL, NULL, vp[2], vp[3]);
                fprintf(stderr, "  [glfb]   read straight back: %lu lit (%s)\n", l, n);
                // ...and four raw bytes, because "0 lit" is a summary and the
                // question at this point is whether ANY value came back.
                if (a_glReadPixels) {
                    uint8_t px[16] = {0};
                    cbind(0x8CA8, (uint32_t)dfb);
                    while (a_glGetError()) {}
                    a_glReadPixels(64, 64, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    uint32_t re = a_glGetError();
                    fprintf(stderr, "  [glfb]   raw px @64,64: %u %u %u %u  err=0x%x\n",
                            px[0], px[1], px[2], px[3], re);
                    // The control, and it is DESTRUCTIVE — it overwrites one of
                    // the guest's frames — so it has a knob of its own rather
                    // than riding on the probe: clear the SAME framebuffer
                    // ourselves, to a value nothing else would produce, and
                    // read it straight back. If ours reads back and the guest's
                    // does not, the readback is sound and the guest's clear is
                    // being lost; if neither does, the instrument is the liar.
                    // That distinction is worth a corrupted frame — it is what
                    // separated "the eye is black" from "the probe reads black".
                    static void (*ccol)(float, float, float, float);
                    static void (*cclear)(uint32_t);
                    if (!ccol) { ccol = asym("glClearColor"); cclear = asym("glClear"); }
                    if (ccol && cclear && kl_env_on("KL_GLFB_CLEAR_CONTROL", 0)) {
                        cbind(0x8CA9, (uint32_t)dfb);
                        ccol(1.0f, 0.0f, 1.0f, 1.0f);
                        cclear(0x4000 /* COLOR_BUFFER_BIT */);
                        memset(px, 0, sizeof px);
                        cbind(0x8CA8, (uint32_t)dfb);
                        a_glReadPixels(64, 64, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
                        fprintf(stderr, "  [glfb]   our own magenta clear reads "
                                        "back: %u %u %u %u\n",
                                px[0], px[1], px[2], px[3]);
                        ccol(0, 0, 0, 0);
                    }
                }
                cbind(0x8CA8, (uint32_t)(keep >= 0 ? keep : 0));
                cbind(0x8CA9, (uint32_t)dfb);
            }
        }
        return;
    }
    if (g_real_Clear) g_real_Clear(mask);
}
static void klfb_ClearColor(float r, float g, float b, float a) {
    if (klfb_timeline())
        fprintf(stderr, "  [glfb] glClearColor(%.2f, %.2f, %.2f, %.2f)\n",
                r, g, b, a);
    if (g_real_ClearColor) g_real_ClearColor(r, g, b, a);
}
static void klfb_RenderbufferStorageMultisample(uint32_t target, int32_t samples,
                                                uint32_t fmt, int32_t w, int32_t h) {
    if (klfb_timeline() && a_glGetIntegerv) {
        int32_t rb = -1;
        a_glGetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &rb);
        fprintf(stderr, "  [glfb] glRenderbufferStorageMultisample rb=%d "
                        "fmt=0x%x %dx%d samples=%d (storage wiped)\n", rb, fmt,
                w, h, samples);
    }
    if (klfb_rt_scale_rb(fmt, &w, &h) > 1)
        fprintf(stderr, "  [glfb] RT supersample: renderbuffer (multisample) "
                        "fmt=0x%x -> %dx%d\n", fmt, w, h);
    if (a_glGetIntegerv && klfb_census_every()) {
        int32_t rb = -1;
        a_glGetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &rb);
        if (rb > 0)
            klfb_vram_note(KLC_RBO, (uint32_t)rb,
                           klfb_storage_bytes(fmt, w, h, 1, 1) *
                               (uint64_t)(samples > 0 ? samples : 1));
    }
    if (g_real_RenderbufferStorageMultisample)
        g_real_RenderbufferStorageMultisample(target, samples, fmt, w, h);
}
static void klfb_RenderbufferStorage(uint32_t target, uint32_t fmt,
                                     int32_t w, int32_t h) {
    if (klfb_timeline() && a_glGetIntegerv) {
        int32_t rb = -1;
        a_glGetIntegerv(0x8CA7, &rb);
        fprintf(stderr, "  [glfb] glRenderbufferStorage rb=%d fmt=0x%x %dx%d "
                        "(storage wiped)\n", rb, fmt, w, h);
    }
    if (klfb_rt_scale_rb(fmt, &w, &h) > 1)
        fprintf(stderr, "  [glfb] RT supersample: renderbuffer fmt=0x%x -> "
                        "%dx%d\n", fmt, w, h);
    if (a_glGetIntegerv && klfb_census_every()) {
        int32_t rb = -1;
        a_glGetIntegerv(0x8CA7, &rb);
        if (rb > 0)
            klfb_vram_note(KLC_RBO, (uint32_t)rb,
                           klfb_storage_bytes(fmt, w, h, 1, 1));
    }
    if (g_real_RenderbufferStorage) g_real_RenderbufferStorage(target, fmt, w, h);
}

static void klfb_BlitFramebuffer(int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                                 int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1,
                                 int64_t mask, int64_t filter) {
    KLFB_LAZY_FLUSH();
    // KL_VRR_MIN_SAMPLES=1 makes every eye-sized single-sampled target a
    // foveated one. A COLOUR blit out of one into a differently-sized region
    // copies warped texels into a target nothing unwarps (ANGLE's passthrough
    // only covers same-map, same-size copies). Named once; not corrected.
    if (g_vrr_rule_map && g_vrr_rule_min <= 1 && (mask & 0x4000)) {
        int32_t sw = sx1 > sx0 ? sx1 - sx0 : sx0 - sx1;
        int32_t sh = sy1 > sy0 ? sy1 - sy0 : sy0 - sy1;
        int32_t dw = dx1 > dx0 ? dx1 - dx0 : dx0 - dx1;
        int32_t dh = dy1 > dy0 ? dy1 - dy0 : dy0 - dy1;
        static int said;
        if (sw == g_vrr_rule_w && sh == g_vrr_rule_h &&
            (dw != sw || dh != sh) && said < 4) {
            said++;
            fprintf(stderr, "  [glfb] vrr: a colour blit reads an eye-sized %dx%d "
                            "region (foveated under KL_VRR_MIN_SAMPLES=1) into "
                            "%dx%d — its picture is WARPED (fb%u -> fb%u)\n",
                    sw, sh, dw, dh, g_read_fb, g_draw_fb);
        }
    }
    klfb_note_stage_write((int32_t)g_draw_fb, 2);
    klfb_note_fbo_use((int32_t)g_draw_fb, 2);
    // The blit's SOURCE is the half that names where the picture came from,
    // and it is the question this census exists for.
    if (klfb_stage_probe_on() && a_glGetIntegerv) {
        int32_t rfb = -1;
        a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &rfb);
        klfb_note_fbo_use(rfb, 3);
    }
    klfb_errprobe("glBlitFramebuffer(before)", NULL);
    static int blit_log = -1;
    if (blit_log < 0) blit_log = kl_env_on("KL_GLFB_ERRPROBE", 0);
    // WHO is blitting. A GL call that arrives with the wrong state bound is a
    // question about the caller, not about the call, and the guest is thirteen
    // libraries — "Unity" and "the OpenXR plugin" issue GL through completely
    // different code and only the frame walk tells them apart.
    if (blit_log) {
        static int said_who;
        if (!said_who) {
            said_who = 1;
            fprintf(stderr, "  [glfb] the first glBlitFramebuffer comes from:\n");
            kl_fault_print_frames(stderr, NULL);
        }
    }
    if (blit_log && a_glGetIntegerv) {
        int32_t dfb = -1, rfb = -1, rb = -1;
        a_glGetIntegerv(0x8CA6, &dfb);
        a_glGetIntegerv(0x8CAA, &rfb);
        a_glGetIntegerv(0x0C02, &rb);
        fprintf(stderr, "  [glfb] blit (%d,%d)-(%d,%d) -> (%d,%d)-(%d,%d) "
                "mask=0x%llx filter=0x%llx draw_fb=%d read_fb=%d readbuf=0x%x\n",
                sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                (long long)mask, (long long)filter, dfb, rfb, rb);
        static uint32_t (*gl_CheckFbStatus)(uint32_t);
        if (!gl_CheckFbStatus) gl_CheckFbStatus = asym("glCheckFramebufferStatus");
        uint32_t (*bindfb_)(uint32_t, uint32_t) = asym("glBindFramebuffer");
        void (*getfap)(uint32_t, uint32_t, uint32_t, int32_t *) =
            asym("glGetFramebufferAttachmentParameteriv");
        if (bindfb_ && gl_CheckFbStatus && getfap) {
            for (int i = 0; i < 2; i++) {
                int32_t fb = i ? dfb : rfb;
                bindfb_(0x8CA9, fb);
                int32_t otype = -1, oname = -1;
                getfap(0x8CA9, 0x8CE0, 0x8CD0 /* OBJECT_TYPE */, &otype);
                getfap(0x8CA9, 0x8CE0, 0x8CD1 /* OBJECT_NAME */, &oname);
                fprintf(stderr, "  [glfb]   fb %d status=0x%x color0 type=0x%x name=%d",
                        fb, gl_CheckFbStatus(0x8CA9), otype, oname);
                if (otype == 0x8D41 /* RENDERBUFFER */) {
                    void (*bindrb)(uint32_t, uint32_t) = asym("glBindRenderbuffer");
                    void (*getrbp)(uint32_t, uint32_t, int32_t *) =
                        asym("glGetRenderbufferParameteriv");
                    if (bindrb && getrbp) {
                        int32_t fmt = -1, samples = -1, w = -1, h = -1;
                        bindrb(0x8D41, oname);
                        getrbp(0x8D41, 0x8D44 /* INTERNAL_FORMAT */, &fmt);
                        getrbp(0x8D41, 0x8CAB /* SAMPLES */, &samples);
                        getrbp(0x8D41, 0x8D42 /* WIDTH */, &w);
                        getrbp(0x8D41, 0x8D43 /* HEIGHT */, &h);
                        fprintf(stderr, " rb fmt=0x%x samples=%d %dx%d",
                                fmt, samples, w, h);
                    }
                }
                fprintf(stderr, "\n");
            }
            bindfb_(0x8CA8, rfb);   // restore: READ<-rfb, DRAW<-dfb
            bindfb_(0x8CA9, dfb);
        }
    }
    // A blit whose read framebuffer is 0 is counted, not repaired: the repair
    // lives at the attach (KL_GLFB_READ_ATTACH_FIX), which carries the
    // attachment the guest named rather than a framebuffer chosen here.
    if (a_glGetIntegerv) {
        int32_t rfb2 = -1;
        a_glGetIntegerv(0x8CAA, &rfb2);
        g_blits++;
        if (rfb2 == 0) g_blits_read0++;
    }
    // KL_GLFB_BLIT_PROBE=1: probe the blit's source BEFORE the blit and its
    // destination AFTER — the swap-time capture found every FBO black, which
    // has two very different readings: the pixels were gone by then
    // (glInvalidateFramebuffer discards), or the draws themselves produce
    // black. Probing both sides of the blit puts the loss on the timeline.
    static int blit_probe = -1;
    if (blit_probe < 0) blit_probe = kl_env_on("KL_GLFB_BLIT_PROBE", 0);
    static float *pfb;
    static uint8_t *pbb;
    static void (*bp_bind)(uint32_t, uint32_t);
    int32_t dfb = -1, rfb = -1;
    if (blit_probe && a_glGetIntegerv) {
        if (!pfb) {
            pfb = malloc((size_t)g_w * g_h * 16);
            pbb = malloc((size_t)g_w * g_h * 4);
            bp_bind = asym("glBindFramebuffer");
        }
        a_glGetIntegerv(0x8CA6, &dfb);
        a_glGetIntegerv(0x8CAA, &rfb);
        if (pfb && pbb && bp_bind) {
            char ns[160] = "";
            // The blit NAMES its source rectangle, so the source's size never
            // has to be looked up: this is the one reader that always knows.
            int32_t srw = sx1 > sx0 ? sx1 - sx0 : sx0 - sx1;
            int32_t srh = sy1 > sy0 ? sy1 - sy0 : sy0 - sy1;
            unsigned long ls = klfb_probe_fbo((uint32_t)rfb, pfb, pbb, ns, sizeof ns,
                                              NULL, NULL, NULL, srw, srh);
            fprintf(stderr, "  [glfb] BLIT_PROBE before: read_fb=%d %lu lit (%s)\n",
                    rfb, ls, ns);
            // KL_GLFB_PROBE_TEX=<name>: read a NAMED texture back, both array
            // layers, at the same moment. A guest that renders into its own
            // target and copies from it puts two failures on the same
            // timeline — "nothing was drawn" and "the copy read the wrong
            // source" — and probing only the blit's own source cannot tell
            // them apart, because the source is whatever the guest bound. The
            // texture is named rather than guessed: an instrument that
            // assumes reports a working pipeline as a broken one. Layer 1 on a
            // non-array texture
            // reports "incomplete", which is the honest answer.
            static int probe_tex = -1;
            if (probe_tex < 0) probe_tex = kl_env_int("KL_GLFB_PROBE_TEX", 0);
            if (probe_tex > 0) {
                for (int L = 0; L < 2; L++) {
                    char nt[160] = "no framebuffer";
                    unsigned long lt = 0;
                    uint32_t tfb = klfb_read_from_texture_layer((uint32_t)probe_tex, L);
                    if (tfb) lt = klfb_probe_fbo(tfb, pfb, pbb, nt, sizeof nt,
                                                 NULL, NULL, NULL, srw, srh);
                    fprintf(stderr, "  [glfb] PROBE_TEX tex=%d layer=%d: %lu lit (%s)\n",
                            probe_tex, L, lt, nt);
                }
            }
            // ...and the framebuffer the guest was drawing into immediately
            // before, measured in the SAME instant. "The source is empty" and
            // "the scene went somewhere else" are different bugs, and telling
            // them apart by correlating two runs' framebuffer numbers does not
            // work — the guest's FBO-to-texture mapping is not stable between
            // runs. One line, both answers, no correlation.
            if (g_last_draw_fb && (int32_t)g_last_draw_fb != rfb) {
                char np[160] = "";
                unsigned long lp = klfb_probe_fbo(g_last_draw_fb, pfb, pbb,
                                                  np, sizeof np, NULL, NULL, NULL,
                                                  srw, srh);
                fprintf(stderr, "  [glfb] BLIT_PROBE last draw target: fb=%u %lu lit"
                                " (%s)\n", g_last_draw_fb, lp, np);
            }
            bp_bind(0x8CA8, (uint32_t)rfb);
            bp_bind(0x8CA9, (uint32_t)dfb);
        }
        // The guest's own blit, error-bracketed honestly: drain first, so an
        // error after is provably this blit's. The question it answers is
        // whether the eye resolve itself fails on this driver.
        if (a_glGetError) while (a_glGetError()) {}
    }
    // r32: a blit names a rectangle in each framebuffer, and the two can carry
    // different supersample factors — a scaled HUD target resolved into an
    // ordinary one is exactly the shape the engine uses. Scale each end by its
    // own. Both are 1 unless KL_GLFB_RT_SCALE is on.
    if (g_rt_read_scale > 1) {
        int s = g_rt_read_scale;
        sx0 *= s; sy0 *= s; sx1 *= s; sy1 *= s;
    }
    if (g_rt_draw_scale > 1) {
        int s = g_rt_draw_scale;
        dx0 *= s; dy0 *= s; dx1 *= s; dy1 *= s;
    }
    if (g_real_BlitFramebuffer)
        g_real_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                               (uint32_t)mask, (uint32_t)filter);
    if (blit_probe && a_glGetError) {
        uint32_t be = a_glGetError();
        if (be)
            fprintf(stderr, "  [glfb] BLIT_PROBE: guest blit fb%d -> fb%d "
                            "raised 0x%x\n", rfb, dfb, be);
    }
    if (blit_probe && pfb && pbb && bp_bind) {
        char nd[160] = "";
        int32_t drw = dx1 > dx0 ? dx1 - dx0 : dx0 - dx1;
        int32_t drh = dy1 > dy0 ? dy1 - dy0 : dy0 - dy1;
        unsigned long ld = klfb_probe_fbo((uint32_t)dfb, pfb, pbb, nd, sizeof nd,
                                          NULL, NULL, NULL, drw, drh);
        fprintf(stderr, "  [glfb] BLIT_PROBE after: draw_fb=%d %lu lit (%s)\n",
                dfb, ld, nd);
        bp_bind(0x8CA8, (uint32_t)rfb);          // restore the guest's bindings
        bp_bind(0x8CA9, (uint32_t)dfb);
    }
    klfb_errprobe("glBlitFramebuffer(after)", NULL);
}

static void (*g_real_TexSubImage3D)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                    int32_t, int32_t, int32_t, uint32_t, uint32_t,
                                    const void *);

static void klfb_TexSubImage3D(uint32_t target, int32_t level, int32_t xoff,
                               int32_t yoff, int32_t zoff, int32_t w, int32_t h,
                               int32_t d, int64_t format, int64_t type,
                               const void *pixels) {
    if (target == 0x806F /* TEXTURE_3D */ && a_glGetIntegerv) {
        int32_t bound = -1;
        a_glGetIntegerv(0x806A /* TEXTURE_BINDING_3D */, &bound);
        if (bound > 0) klfb_note_tex3d_event((uint32_t)bound, 1, 0, 0);
    }
    if (g_real_TexSubImage3D)
        g_real_TexSubImage3D(target, level, xoff, yoff, zoff, w, h, d,
                             (uint32_t)format, (uint32_t)type, pixels);
}

// ----------------------------------------- the compressed-upload probe (ETC2)
//
// The Simulator dies inside ANGLE on a compressed upload — signal 10 in
// _platform_memmove under TextureMtl::setPerSliceSubImage, from
// glCompressedTexSubImage2D of 0x9279 at 2048x1024. A memmove overruns, so
// something disagrees about how many bytes the call describes. Everything about
// the *format* has already been ruled out: the emulated-format
// table entry is gated on macOS/Catalyst rather than the simulator, the
// simulator block maps ETC2 to native EAC, and a standalone
// EAC_RGBA8_sRGB + replaceRegion with exactly the guest's 2 MB succeeds.
//
// What was never measured is the guest's own arithmetic. This computes the size
// the dimensions imply and compares it with the imageSize the guest passed —
// and separately checks the sub-region against the mip level's real extent,
// because a region that overruns the level is the other way to make a correct
// imageSize describe the wrong destination.
//
// Block geometry, not bytes-per-pixel: every format here is 4x4-blocked except
// ASTC, whose footprint is in its enum. Returns 0 for "not a compressed format
// I know", which is reported rather than guessed at.
static int klfb_block_geom(uint32_t fmt, int *bw, int *bh, int *bytes) {
    *bw = *bh = 4;
    switch (fmt) {
        case 0x9270: case 0x9271:                       // R11_EAC (+signed)
        case 0x9274: case 0x9275:                       // RGB8_ETC2, SRGB8_ETC2
        case 0x9276: case 0x9277:                       // ...A1 punchthrough
            *bytes = 8;  return 1;
        case 0x9272: case 0x9273:                       // RG11_EAC (+signed)
        case 0x9278: case 0x9279:                       // RGBA8_ETC2_EAC, SRGB8_A8
            *bytes = 16; return 1;
        default: break;
    }
    // ASTC: 0x93B0-0x93BD LDR, 0x93D0-0x93DD sRGB, same footprint order. Always
    // 16 bytes per block; only the block dimensions vary.
    static const struct { int w, h; } astc[] = {
        {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
        {10,5},{10,6},{10,8},{10,10},{12,10},{12,12},
    };
    unsigned i = 0xffffffffu;
    if (fmt >= 0x93B0 && fmt <= 0x93BD) i = fmt - 0x93B0;
    if (fmt >= 0x93D0 && fmt <= 0x93DD) i = fmt - 0x93D0;
    if (i < sizeof astc / sizeof *astc) {
        *bw = astc[i].w; *bh = astc[i].h; *bytes = 16; return 1;
    }
    return 0;
}

// KL_GLFB_PROBE_CTEX=1 — also check each compressed upload against the
// allocation glTexStorage2D recorded, which costs a GL query per upload.
static int klfb_probe_ctex(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_PROBE_CTEX", 0);
    return on;
}

static void (*g_real_CompressedTexSubImage2D)(uint32_t, int32_t, int32_t, int32_t,
                                              int32_t, int32_t, uint32_t, int32_t,
                                              const void *);

static void klfb_CompressedTexSubImage2D(uint32_t target, int32_t level, int32_t xoff,
                                         int32_t yoff, int32_t w, int32_t h,
                                         int64_t format, int64_t imageSize,
                                         const void *data) {
    uint32_t fmt = (uint32_t)format;
    int32_t  sz  = (int32_t)imageSize;
    int bw, bh, bpb, known = klfb_block_geom(fmt, &bw, &bh, &bpb);
    long expect = known ? (long)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb : -1;

    // What the level is actually supposed to be, from the glTexStorage2D record.
    //
    // Behind its own knob, because unlike `expect` above this is not free: it is
    // a glGetIntegerv per compressed upload, a synchronous round trip into
    // ANGLE, and instrumentation that perturbs the thing it measures has bitten
    // this file before. It cannot be folded into "only look when the size is
    // already wrong" either — a region that overruns its mip level has a
    // perfectly correct imageSize, and that is exactly the case still open.
    // r30: the destination now comes from the bind tracker rather than a
    // glGetIntegerv round trip, so the three verdicts below are free and no
    // longer wait on KL_GLFB_PROBE_CTEX. The query remains as the fallback for
    // a context whose binds never came through our thunk.
    int32_t bound = g_bound2d ? (int32_t)g_bound2d : -1, tw = 0, th = 0;
    uint32_t tfmt = 0;
    int have = 0;
    if (bound <= 0 && (klfb_probe_ctex() || klfb_trace_tex()) && a_glGetIntegerv)
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &bound);
    if (bound > 0) have = klfb_tex_info((uint32_t)bound, &tfmt, &tw, &th);

    // The mip-chain record: which level this call filled, and with how much.
    if (bound > 0) klfb_note_tex_level((uint32_t)bound, level, (long)sz);
    int32_t lw = have ? (tw >> level ? tw >> level : 1) : 0;
    int32_t lh = have ? (th >> level ? th >> level : 1) : 0;

    // Three ways this call can be wrong, each named separately so the log says
    // which one rather than just "something is off".
    const char *verdict = "";
    if (!known)                        verdict = "  <-- UNKNOWN COMPRESSED FORMAT";
    else if (expect != sz)             verdict = "  <-- SIZE MISMATCH";
    else if (have && (xoff + w > lw || yoff + h > lh))
                                       verdict = "  <-- REGION OVERRUNS LEVEL";
    else if (have && fmt != tfmt)      verdict = "  <-- FORMAT != ALLOCATED";

    // A verdict always prints, even with no knob set — a wrong upload is worth
    // a line in any run. The knobs add the clean ones, so the host and the
    // Simulator are diffable.
    // ...but a verdict that is true of EVERY upload would now print thousands
    // of identical lines, because r30 made the check free and therefore always
    // on. Capped; the mip-chain census below carries the totals either way.
    static unsigned said_verdict;
    if (klfb_trace_tex() || klfb_probe_ctex() || (*verdict && said_verdict++ < 40))
        fprintf(stderr, "  [glfb] #%u glCompressedTexSubImage2D target=0x%04x "
                        "level=%d %dx%d at %d,%d fmt=0x%04x imageSize=%d "
                        "expect=%ld (tex=%d alloc 0x%04x %dx%d, level %dx%d)%s\n",
                g_texcalls++, target, level, w, h, xoff, yoff, fmt, sz, expect,
                bound, tfmt, tw, th, lw, lh, verdict);

    if (g_real_CompressedTexSubImage2D)
        g_real_CompressedTexSubImage2D(target, level, xoff, yoff, w, h, fmt, sz, data);
}

// ---- the mip-chain census (r30) --------------------------------------------
//
// r29 forced every sampler to level 0 and Austin's next worn run said the blue
// fill was gone. That closes the LOD family's first question — the sample WAS
// coming from a level above 0 — and opens the one that decides whether the fix
// is ours to make: is the level it read a real mip, or a level that was
// allocated and never filled?
//
// This answers it with arithmetic over what the guest uploaded. For every
// texture allocated with more than one level it compares the allocation's level
// count against the levels that actually received an upload, and prints the
// bytes each level got beside the bytes its dimensions imply. A chain with a
// hole in it is a picture that goes constant exactly where the hole starts, and
// that is a defect our upload path can own. A chain that is complete and
// correctly sized moves the whole question to the LOD that selects the level,
// which is the coordinate half and a different instrument.
//
// Ordinary and expected, and NOT a defect: UE4 streams texture mips. A
// streaming texture is allocated at its full chain and only the resident tail
// is uploaded, with BASE_LEVEL walked down as the rest arrives. So the verdict
// distinguishes a MISSING TAIL (levels 0..k written, k+1..n empty — the shape
// streaming makes, and harmless because BASE_LEVEL should exclude them) from a
// MISSING HEAD or an INTERIOR HOLE, neither of which streaming produces.
void kl_glfb_mipchain_census(FILE *f) {
    if (!f) return;
    unsigned mipped = 0, complete = 0, tail = 0, hole = 0, head = 0, printed = 0;
    pthread_mutex_lock(&g_compile_lock);
    for (unsigned i = 0; i < g_ntex; i++) {
        int levels = g_tex[i].levels;
        if (levels <= 1) continue;
        if (levels > KLFB_MAX_LVL) levels = KLFB_MAX_LVL;
        mipped++;
        unsigned mask = g_tex[i].wmask & ((1u << levels) - 1u);
        unsigned full = (1u << levels) - 1u;
        // Classify the gap by SHAPE, not by count.
        int first = -1, gaps = 0, prev = 0;
        for (int l = 0; l < levels; l++) {
            int on = (mask >> l) & 1;
            if (on && first < 0) first = l;
            if (on && !prev && first >= 0 && l > first) gaps++;
            prev = on;
        }
        const char *verdict;
        if (mask == full)                     { verdict = "complete"; complete++; }
        else if (mask == 0)                   { verdict = "NEVER WRITTEN AT ALL"; head++; }
        else if (gaps > 0)                    { verdict = "INTERIOR HOLE"; hole++; }
        else if (first > 0)                   { verdict = "MISSING HEAD"; head++; }
        else                                  { verdict = "missing tail (streaming)"; tail++; }

        // Print the ones that matter: every defect, and the big art textures
        // whether or not they are defective, because "complete and correctly
        // sized" is the result that moves the round to the other half.
        int big = g_tex[i].w >= 512 && g_tex[i].h >= 512;
        if ((mask == full && !big) || printed >= 24) continue;
        printed++;
        int bw, bh, bpb, known = klfb_block_geom(g_tex[i].fmt, &bw, &bh, &bpb);
        char line[512]; int n = 0;
        n += snprintf(line + n, sizeof line - n,
                      "  [mipchain] tex %u %dx%d fmt=0x%04x levels=%d written=%d/%d"
                      " — %s\n              ", g_tex[i].name, (int)g_tex[i].w,
                      (int)g_tex[i].h, g_tex[i].fmt, levels,
                      __builtin_popcount(mask), levels, verdict);
        for (int l = 0; l < levels && n < (int)sizeof line - 64; l++) {
            int32_t lw = g_tex[i].w >> l ? g_tex[i].w >> l : 1;
            int32_t lh = g_tex[i].h >> l ? g_tex[i].h >> l : 1;
            long expect = known
                ? (long)((lw + bw - 1) / bw) * ((lh + bh - 1) / bh) * bpb : -1;
            unsigned got = g_tex[i].lb[l];
            n += snprintf(line + n, sizeof line - n, " L%d=%u%s", l, got,
                          (expect > 0 && got && (long)got != expect) ? "!" : "");
            if (expect > 0 && got && (long)got != expect)
                n += snprintf(line + n, sizeof line - n, "(want %ld)", expect);
        }
        fprintf(f, "%s\n", line);
    }
    pthread_mutex_unlock(&g_compile_lock);
    fprintf(f, "  [mipchain] %u mipped allocation(s): %u complete, %u missing a "
               "tail (streaming), %u with an INTERIOR HOLE, %u with a MISSING "
               "HEAD%s\n",
            mipped, complete, tail, hole, head,
            (hole || head)
                ? "  <- a hole or a missing head is a level that reads whatever "
                  "the driver left there, which is the flat fill's shape"
                : (mipped && !hole && !head)
                    ? "  <- every chain the guest allocated is filled from level "
                      "0 down, so the level the fill read had real content in it "
                      "and the LOD that SELECTED it is the remaining half"
                    : "");
    // How much of the title the NOMIP diagnostic is actually costing. The
    // banner is capped at three lines and says only that the arm engaged.
    if (g_nomip_demoted)
        fprintf(f, "  [mipchain] NOMIP demoted %llu mipmap min filter(s) to "
                   "level 0 — the blue fill went away under this, and this is "
                   "the number of samplers paying for it\n",
                (unsigned long long)g_nomip_demoted);
}

static void (*g_real_CompressedTexSubImage3D)(uint32_t, int32_t, int32_t, int32_t,
                                              int32_t, int32_t, int32_t, int32_t,
                                              uint32_t, int32_t, const void *);

static void klfb_CompressedTexSubImage3D(uint32_t target, int32_t level, int32_t xoff,
                                         int32_t yoff, int32_t zoff, int32_t w,
                                         int32_t h, int32_t d, int64_t format,
                                         int64_t imageSize, const void *data) {
    if (g_real_CompressedTexSubImage3D)
        g_real_CompressedTexSubImage3D(target, level, xoff, yoff, zoff, w, h, d,
                                       (uint32_t)format, (int32_t)imageSize, data);
}

// ------------------------------------------------------- draw-target census
//
// The frame reads back black even though the capture is now correct, so the
// question is where the draws are going. Unity believes it is in VR
// (ovrp_Initialize5 answers success), and a VR frame does not present to the
// backbuffer — it renders into eye textures and hands them to a compositor. We
// serve no eye textures, so the engine may be drawing somewhere it never shows us.
//
// This counts draws per bound framebuffer, which distinguishes the two cases
// outright: draws on framebuffer 0 mean a flat frame we are failing to capture,
// draws only on other framebuffers mean the eye-buffer path and a different job.
// r33: 16 was FULL on the tetris menu — sixteen entries listed and at least six
// more widget framebuffers drawing into targets the census could not name, so
// "draws per framebuffer" silently stopped being a complete answer exactly where
// the question was being asked. 48 matches KLFB_FBO_COLOR/KLFB_FBO_DEPTH; the
// table is diagnostic-only and costs 16 bytes an entry.
#define KLFB_MAX_FBS 48
// ...and through WHICH VIEWPORT, which is the column that was missing.
//
// Nothing in this file saw glViewport before: it was one of the two hundred
// entry points handed straight to ANGLE, so "did the guest render into all of
// the eye texture we gave it?" had no answer short of KL_GLFB_TIMELINE's
// per-draw firehose — and a knob is no use at all for a Home-View launch, which
// carries no environment.
//
// It is worth a column because a sub-rect viewport is what a wrong picture
// looks like from the outside: the compositor stretches the WHOLE eye texture
// across the whole field, so a guest that drew into a corner of it shows a
// picture pinned to a corner of the view that shears as the head turns. That is
// The Last Clockwinder's boot #2 report almost word for word, and the eye layer
// itself was already exonerated — ovrp_CalculateEyeViewportRect answered the
// full 4032x3234, and [rp] logged the guest's projection as a MATCH for the
// drawable's on both eyes. What the guest then DID with that viewport is the
// remaining unknown, and this is it.
//
// DISTINCT rects, not a count: a splash pass drawing 2290x2400 into a 4032x3234
// eye texture and a scene pass drawing all of it are the same row otherwise, and
// the whole question is whether those two numbers differ.
#define KLFB_MAX_VPS 4
static struct {
    int32_t fb; unsigned draws; uint64_t tid;
    struct { int32_t x, y, w, h; unsigned draws; } vp[KLFB_MAX_VPS];
    unsigned nvp, vp_dropped;
} g_draw_fbs[KLFB_MAX_FBS];
static unsigned g_ndraw_fbs;

// Fold this draw's viewport into the row's rect list. Called with the row index
// so it costs one comparison per known rect and nothing else.
static void klfb_note_draw_viewport(unsigned row) {
    if (!g_vp_set || row >= KLFB_MAX_FBS) return;
    for (unsigned i = 0; i < g_draw_fbs[row].nvp; i++)
        if (g_draw_fbs[row].vp[i].x == g_vp[0] && g_draw_fbs[row].vp[i].y == g_vp[1] &&
            g_draw_fbs[row].vp[i].w == g_vp[2] && g_draw_fbs[row].vp[i].h == g_vp[3]) {
            g_draw_fbs[row].vp[i].draws++;
            return;
        }
    if (g_draw_fbs[row].nvp >= KLFB_MAX_VPS) {
        // Counted rather than silently forgotten: a row that overflowed is a row
        // whose rect list is a sample, and the reader has to know that before
        // concluding anything from the four it can see.
        g_draw_fbs[row].vp_dropped++;
        return;
    }
    unsigned i = g_draw_fbs[row].nvp++;
    g_draw_fbs[row].vp[i].x = g_vp[0]; g_draw_fbs[row].vp[i].y = g_vp[1];
    g_draw_fbs[row].vp[i].w = g_vp[2]; g_draw_fbs[row].vp[i].h = g_vp[3];
    g_draw_fbs[row].vp[i].draws = 1;
}

// The eye an FBO's colour attachment belongs to, or -1. The stage map is
// already the right table — an eye texture is filed under (eye, stage) — so
// this is klfb_stage_of_fbo's other half rather than a second registry.
static int klfb_eye_of_fbo(uint32_t fbo) {
    if (!fbo) return -1;
    for (int i = 0; i < KLFB_FBO_MAP; i++) {
        if (g_fbo_stage[i].fbo != fbo || !g_fbo_stage[i].tex) continue;
        for (int e = 0; e < 2; e++)
            for (int st = 0; st < KLFB_MAX_STAGES; st++)
                if (g_eye_tex_stage[e][st] == g_fbo_stage[i].tex) return e;
        return -1;
    }
    return -1;
}

// kl_view_id on the current program, written only when it would change.
//
// The location is cached per program — including the -1 that means "this
// program was not de-sugared", which is most of them — because a
// glGetUniformLocation under every draw call is exactly the cost this file
// avoids elsewhere by tracking glUseProgram instead of querying it.
#define KLFB_MV_PROGS 512
static struct { uint32_t prog; int32_t loc; int view; } g_mv_prog[KLFB_MV_PROGS];
static unsigned g_mv_nprog;
static void (*a_glUniform1ui)(int32_t, uint32_t);

static void klfb_mv_bind(void) {
    if (!g_mv_shaders || !g_cur_prog || !g_real_GetUniformLocation) return;
    unsigned i;
    for (i = 0; i < g_mv_nprog; i++)
        if (g_mv_prog[i].prog == g_cur_prog) break;
    if (i == g_mv_nprog) {
        if (g_mv_nprog >= KLFB_MV_PROGS) return;
        // Asked for by OUR name, so the builtin-rename table in
        // klfb_uniform_location must not be consulted: this uniform is one this
        // file put in the text.
        int32_t loc = g_real_GetUniformLocation(g_cur_prog, KLFB_MV_UNIFORM);
        g_mv_prog[i].prog = g_cur_prog;
        g_mv_prog[i].loc  = loc;
        g_mv_prog[i].view = -1;
        g_mv_nprog++;
    }
    if (g_mv_prog[i].loc < 0 || g_mv_prog[i].view == g_mv_view_id) return;
    if (!a_glUniform1ui) a_glUniform1ui = asym("glUniform1ui");
    if (!a_glUniform1ui) { g_mv_prog[i].loc = -1; return; }
    a_glUniform1ui(g_mv_prog[i].loc, (uint32_t)g_mv_view_id);
    g_mv_prog[i].view = g_mv_view_id;
    g_mv_sets++;
}

// ---- what STATE a framebuffer's draws are actually made with ---------------
//
// Six rounds of "the world is drawn and the world is black" and nobody has ever
// looked at the colour mask. The classic mechanism for a fully-drawn, fully
// black pass is a depth-only shadow pass that turns colour writes OFF and a
// main pass that never turns them back on: every draw call succeeds, the
// triangle counters are right, the framebuffer is complete, there is no GL
// error, and nothing is painted. Simpsons Hit & Run runs three depth-only
// cascades (fb7/fb8/fb9 in launch #6, 1.59 M draws between them) immediately
// before the pass that is black, which is exactly the shape of it.
//
// Sampled, not traced: the first two draws into each framebuffer and then one
// in every 4096, logged only when the state CHANGES and at most eight times per
// framebuffer. Reading ten pieces of GL state on all 6.5 million draws of a
// launch is not an instrument, it is a second frame budget.
// KL_GLFB_DRAW_STATE=0 turns it off.
static struct { int32_t fb; unsigned n, said; uint32_t sig; }
    g_dstate[KLFB_DSTATE_FBS];
static unsigned g_ndstate;

static void klfb_note_draw_state(int32_t fb) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_DRAW_STATE", 1);
    if (!on || !a_glGetIntegerv) return;
    // THE BUDGET IS RE-ARMED WHEN THE WORLD STARTS. Launch #7 spent every one of
    // fb1's eight lines on the front end and printed NOTHING for the pass that
    // is actually black -- the census was answering a question nobody was
    // asking. The first shadow pass is the same signal the capture arms on, so
    // both instruments start looking at the same moment.
    static int armed_at;
    if (!armed_at && __atomic_load_n(&g_shadow_pass_seen, __ATOMIC_RELAXED)) {
        armed_at = 1;
        for (unsigned k = 0; k < g_ndstate; k++) {
            g_dstate[k].said = 0;
            g_dstate[k].n = 0;
            g_dstate[k].sig = 0;
        }
        fprintf(stderr, "  [glfb] draw-state census re-armed — a level is being "
                        "drawn, and every line below is IN-WORLD state\n");
    }
    unsigned i;
    for (i = 0; i < g_ndstate; i++) if (g_dstate[i].fb == fb) break;
    if (i == g_ndstate) {
        if (g_ndstate >= KLFB_DSTATE_FBS) return;
        g_dstate[g_ndstate].fb = fb;
        i = g_ndstate++;
    }
    if (g_dstate[i].said >= 8) return;
    unsigned n = g_dstate[i].n++;
    if (n >= 2 && (n & 0xFFF)) return;

    static void (*a_GetBooleanv)(uint32_t, uint8_t *);
    static uint8_t (*a_IsEnabled)(uint32_t);
    static void (*a_GetFloatv)(uint32_t, float *);
    static int looked;
    if (!looked) {
        looked = 1;
        a_GetBooleanv = asym("glGetBooleanv");
        a_IsEnabled   = asym("glIsEnabled");
        a_GetFloatv   = asym("glGetFloatv");
    }
    if (!a_GetBooleanv || !a_IsEnabled) return;

    uint8_t cm[4] = {1,1,1,1}, dm = 1;
    a_GetBooleanv(0x0C23 /* COLOR_WRITEMASK */, cm);
    a_GetBooleanv(0x0B72 /* DEPTH_WRITEMASK */, &dm);
    int sc = a_IsEnabled(0x0C11 /* SCISSOR_TEST */) ? 1 : 0;
    int bl = a_IsEnabled(0x0BE2 /* BLEND */)        ? 1 : 0;
    int dt = a_IsEnabled(0x0B71 /* DEPTH_TEST */)   ? 1 : 0;
    int cu = a_IsEnabled(0x0B44 /* CULL_FACE */)    ? 1 : 0;
    int32_t box[4] = {0,0,0,0}, df = 0;
    a_glGetIntegerv(0x0C10 /* SCISSOR_BOX */, box);
    a_glGetIntegerv(0x0B74 /* DEPTH_FUNC */, &df);
    float cc[4] = {0,0,0,0};
    if (a_GetFloatv) a_GetFloatv(0x0C22 /* COLOR_CLEAR_VALUE */, cc);

    uint32_t sig = (uint32_t)((cm[0]?1:0) | (cm[1]?2:0) | (cm[2]?4:0) |
                              (cm[3]?8:0) | (dm?16:0) | (sc<<5) | (bl<<6) |
                              (dt<<7) | (cu<<8))
                 ^ ((uint32_t)df << 12)
                 ^ (uint32_t)(box[2] * 7 + box[3] * 13);
    if (g_dstate[i].said && sig == g_dstate[i].sig) return;
    g_dstate[i].sig = sig;
    g_dstate[i].said++;
    fprintf(stderr, "  [glfb] draw state fb%d: colourmask %d%d%d%d depthmask %d "
                    "depthtest %d func 0x%x scissor %d box %d,%d %dx%d blend %d "
                    "cull %d clear (%.3f %.3f %.3f %.3f)%s\n",
            fb, cm[0]?1:0, cm[1]?1:0, cm[2]?1:0, cm[3]?1:0, dm?1:0, dt,
            (unsigned)df, sc, box[0], box[1], box[2], box[3], bl, cu,
            (double)cc[0], (double)cc[1], (double)cc[2], (double)cc[3],
            (!cm[0] && !cm[1] && !cm[2])
              ? "   <<< COLOUR WRITES ARE OFF — every draw into this "
                "framebuffer paints NOTHING"
              : "");
}

static void klfb_note_draw(void) {
    int32_t fb = -1;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    // The framebuffer the last DRAW CALL went to, which is not the last binding:
    // this guest draws into one framebuffer and blits out of another.
    if (fb > 0) g_last_draw_fb = (uint32_t)fb;
    klfb_note_draw_state(fb);
    klfb_ftrace_draw(fb);                                    // r26, no-op when off
    klfb_vrr_post_check(fb);      // KL_VRR_MIN_SAMPLES=1 only; a compare otherwise
    klfb_radar_note_draw(fb);     // r36, no-op unless KL_SHAR_RADARTRACE
    klfb_rt_note_draw();          // r32 — free unless a scaled target is bound
    // The multiview view index, for the shaders klfb_rewrite_glsl de-sugared.
    // A draw INTO an eye texture is that eye's resolve, so it renders as that
    // eye and the pass after it is the other one.
    if (g_mv_shaders) {
        int mve = fb > 0 ? klfb_eye_of_fbo((uint32_t)fb) : -1;
        if (mve >= 0) { g_mv_view_id = mve; g_mv_eye_pass[mve]++; }
        klfb_mv_bind();
        if (mve >= 0) g_mv_view_id = 1 - mve;
    }
    klfb_note_stage_write(fb, 0);
    klfb_note_fbo_use(fb, 0);
    // The same timeline the blit probe reads: order the draws against the
    // clears/invalidates/blits, because "black at blit time" is meaningless
    // without knowing whether a draw came after the last clear. Program,
    // viewport and tex0 name the pass — the frame's last draw paints the
    // whole scene target black, and identifying it is the whole game now.
    if (klfb_timeline()) {
        int32_t prog = -1, vp[4] = {0,0,0,0}, tex0 = -1;
        if (a_glGetIntegerv) {
            a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
            a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
            a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &tex0);
        }
        uint32_t tfmt = 0; int32_t tw = 0, th = 0;
        const char *tinfo = "";
        char tbuf[48];
        if (tex0 > 0 && klfb_tex_info((uint32_t)tex0, &tfmt, &tw, &th)) {
            snprintf(tbuf, sizeof tbuf, " fmt=0x%x %dx%d", tfmt, tw, th);
            tinfo = tbuf;
        }
        fprintf(stderr, "  [glfb] draw on fb=%d program=%d vp=%dx%d tex0=%d%s\n",
                fb, prog, vp[2], vp[3], tex0, tinfo);
    }
    uint64_t tid = 0; pthread_threadid_np(NULL, &tid);
    for (unsigned i = 0; i < g_ndraw_fbs; i++)
        if (g_draw_fbs[i].fb == fb && g_draw_fbs[i].tid == tid) {
            g_draw_fbs[i].draws++;
            klfb_note_draw_viewport(i);
            return;
        }
    if (g_ndraw_fbs < KLFB_MAX_FBS) {
        g_draw_fbs[g_ndraw_fbs].fb = fb;
        g_draw_fbs[g_ndraw_fbs].draws = 1;
        g_draw_fbs[g_ndraw_fbs].tid = tid;
        klfb_note_draw_viewport(g_ndraw_fbs);
        g_ndraw_fbs++;
    }
}

// ...and the table SAID so, to nobody: it was filled from the first day of the
// GL path and never printed, so "where is the guest actually drawing?" — the
// question it was built to answer — still needed a bespoke trace every time it
// came up. Each row's colour attachment is resolved at report time rather than
// at draw time, because an FBO's attachment is re-pointed per frame and the
// last one is the one that matters for reading the row.
void kl_glfb_draw_census(FILE *f) {
    if (g_blits)
        fprintf(f, "  [glfb] glBlitFramebuffer: %lu, of which %lu read the "
                   "DEFAULT framebuffer\n", g_blits, g_blits_read0);
    // The multiview de-sugar, and whether its view index was ever driven.
    // "0 uniform writes" with shaders de-sugared means every one of them
    // rendered as view 0 — both eyes the same picture — and the eye-pass
    // counts say whether the eye framebuffers were recognised at all.
    if (g_mv_shaders || g_mv_no_room)
        fprintf(f, "  [glfb] multiview de-sugared: %lu shader(s), %lu view-id "
                   "write(s), passes ending on eye0 %lu eye1 %lu%s\n",
                g_mv_shaders, g_mv_sets, g_mv_eye_pass[0], g_mv_eye_pass[1],
                g_mv_no_room ? " (and some directives too short to rewrite "
                               "in — those still fail to compile)" : "");
    if (!g_ndraw_fbs) return;
    fprintf(f, "  [glfb] draws per framebuffer:\n");
    for (unsigned i = 0; i < g_ndraw_fbs; i++) {
        int32_t fb = g_draw_fbs[i].fb;
        char what[96] = "";
        if (fb == 0) snprintf(what, sizeof what, " (the default framebuffer)");
        else {
            uint32_t tex = 0; int layer = -1;
            for (unsigned k = 0; k < g_nfbo_color; k++)
                if (g_fbo_color[k].fbo == (uint32_t)fb) {
                    tex = g_fbo_color[k].tex; layer = g_fbo_color[k].layer; break;
                }
            if (tex) {
                uint32_t fmt = 0; int32_t w = 0, h = 0;
                char lb[24] = "";
                klfb_tex_info(tex, &fmt, &w, &h);
                if (layer >= 0) snprintf(lb, sizeof lb, " layer %d", layer);
                snprintf(what, sizeof what, " (colour0 tex %u%s %dx%d fmt 0x%x)",
                         tex, lb, w, h, fmt);
            }
        }
        fprintf(f, "    fb %-3d thread %-8llu %u draws%s\n", fb,
                (unsigned long long)g_draw_fbs[i].tid, g_draw_fbs[i].draws, what);
        for (unsigned v = 0; v < g_draw_fbs[i].nvp; v++)
            fprintf(f, "        viewport %d,%d %dx%d  %u draws\n",
                    g_draw_fbs[i].vp[v].x, g_draw_fbs[i].vp[v].y,
                    g_draw_fbs[i].vp[v].w, g_draw_fbs[i].vp[v].h,
                    g_draw_fbs[i].vp[v].draws);
        if (g_draw_fbs[i].vp_dropped)
            fprintf(f, "        (and %u draw(s) through further rects, not "
                       "listed — this row's list is a sample)\n",
                    g_draw_fbs[i].vp_dropped);
        if (!g_draw_fbs[i].nvp)
            fprintf(f, "        (the guest never set a viewport on this "
                       "thread — GL's default, the surface size, applied)\n");
    }
}

static void (*g_real_DrawElements)(uint32_t, int32_t, uint32_t, const void *);
static void (*g_real_DrawArrays)(uint32_t, int32_t, int32_t);
static void (*g_real_DrawElementsInstanced)(uint32_t, int32_t, uint32_t, const void *,
                                            int32_t);
static void (*g_real_DrawElementsBaseVertex)(uint32_t, int32_t, uint32_t, const void *,
                                             int32_t);
static void (*g_real_DrawArraysInstanced)(uint32_t, int32_t, int32_t, int32_t);
static void klfb_service_capture(void);

// KL_GLFB_ERRPROBE=1: check the GL error after each draw and print the first
// few with the state around them. The guest accumulates errors it never reads
// (the capture keeps meeting them as pre-existing), so one of its own calls is
// failing silently — this names it. Consumes the error; the guest demonstrably
// was not reading them anyway.
static void klfb_errprobe(const char *what, const char *detail) {
    static int on = -1, said;
    if (on < 0) on = kl_env_on("KL_GLFB_ERRPROBE", 0);
    if (!on || !a_glGetError) return;
    // Consumes the pending error. Called before and after the real call:
    // "pre=0xN" on the BEFORE line is an earlier unwrapped call's leftover, on
    // the AFTER line it is provably this call's own error.
    uint32_t err = a_glGetError();
    if (!err || said >= 20) return;
    said++;
    int32_t fb = -1, prog = -1, vp[4] = {0,0,0,0};
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
        a_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
        a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    }
    fprintf(stderr, "  [glfb] ERRPROBE %s: pre=0x%x fb=%d program=%d viewport %dx%d%s\n",
            what, err, fb, prog, vp[2], vp[3], detail ? detail : "");
    if (err == 0x506 /* INVALID_FRAMEBUFFER_OPERATION */) {
        static uint32_t (*ck)(uint32_t);
        if (!ck) ck = asym("glCheckFramebufferStatus");
        if (ck)
            fprintf(stderr, "  [glfb] ERRPROBE %s: CheckFramebufferStatus(DRAW)=0x%x\n",
                    what, ck(0x8CA9));
    }
}

static void klfb_errprobe(const char *what, const char *detail);
#define klfb_errprobe0(w) klfb_errprobe((w), NULL)

// ---------------------------------------------------------------------------
// FILLHIST (r26) — pixel history: WHICH DRAW paints the flat fill.
//
// r25 named a mechanism (UE4's DepthbufferFetchES2 compiling to a constant on a
// backend with no GL_ARM_shader_framebuffer_fetch_depth_stencil), fixed it, and
// the fix engaged perfectly and changed nothing Austin could see. The reason is
// in r26's own census and it is arithmetic: the five shaders that carry the
// idiom are made current TEN times in a four-minute run — once each, during the
// load-time warm-up — against seven hundred thousand draws per eye
// framebuffer. Whatever paints the fill is one of the other 605 programs and it
// never asks for scene depth at all.
//
// So stop reasoning about candidates and ask the picture. Once every
// KL_GLFB_FILLHIST_EVERY seconds, for ONE eye framebuffer's run of draws, read
// a horizontal strip of the draw's own viewport back after every draw and
// bucket it. The draw that turns a wide band of the strip into a flat colour IS
// the fill, by construction — no theory in between. The report names its
// program, vertex count, viewport and bound texture, and writes that program's
// GLSL to KL_GLFB_DUMP_GLSL_FILL so the next round starts from the text.
//
// Cost is bounded and deliberate: a readback per draw serialises the frame, so
// this hitches for one frame per pass, and it is off unless asked. Defaults:
// first pass at 45 s (past the load), then every 30 s, four passes, 256 draws.
// The shape of the draw in flight, published by every wrapper just before the
// probe runs. FILLHIST needs the FIRST VERTEX, not the count: a 6-vertex quad
// out of a shared vertex buffer starts wherever the batch put it, and reading
// element 0 of the buffer would report a different mesh's attributes with total
// confidence. glDrawArrays says `first`; glDrawElements says it in the index
// buffer, and that is one more read.
static struct {
    int32_t     first;        // glDrawArrays' first, or -1 when indexed
    const void *indices;      // glDrawElements' offset into ELEMENT_ARRAY_BUFFER
    uint32_t    itype;        // ...and its index type
    int32_t     basevertex;
} g_lastdraw = { 0, NULL, 0, 0 };

static void klfb_draw_shape_arrays(int32_t first) {
    g_lastdraw.first = first; g_lastdraw.indices = NULL;
    g_lastdraw.itype = 0; g_lastdraw.basevertex = 0;
}
static void klfb_draw_shape_elements(const void *indices, uint32_t itype,
                                     int32_t basevertex) {
    g_lastdraw.first = -1; g_lastdraw.indices = indices;
    g_lastdraw.itype = itype; g_lastdraw.basevertex = basevertex;
}

#define KLFB_FH_BUCK   32
#define KLFB_FH_DRAWS  256
//
// r27 added the other axis and a colour-agnostic detector, because r26's single
// horizontal row could not see what Austin's screenshots are made of. The fill
// is cut by boundaries that are HORIZONTAL in screen space — a row that lies
// inside one band measures that band and nothing else, and the row it lands on
// depends on where his head is pointing. A COLUMN through the middle of the
// viewport crosses every band in one readback, so the log prints the band
// structure itself. And the pass no longer has to be told the fill's colour:
// `flat` is the longest run of near-equal buckets, so the draw that turns a
// wide band of the picture into ANY one constant is named the same way.
//
// r29 added the two things the uniform dump could not answer. The named draw's
// colour reduced, arithmetically, to `pc2_m[1] * (a scalar)` — a UNIFORM times
// a bracket built entirely out of texture reads — and both remaining unknowns
// are per-draw GL state that is gone by report time:
//
//   * WHAT THE SAMPLERS' STATE IS. A 1024x1024 texture read at its top mip is
//     one texel for every fragment: flat, zoom-invariant, fog-invariant. So the
//     unit walk now records the min filter, the base/max level and any bound
//     SAMPLER OBJECT (which overrides the texture's own parameters and would
//     make the texture-side numbers a lie).
//   * WHAT THE VERTEX ATTRIBUTES CONTAIN. The UVs of this draw are
//     `in_ATTRIBUTE4 * 1.0 + time * 0.01`, so a degenerate or wrongly-decoded
//     attribute 4 makes every fragment sample the same texel — the same flat
//     result, from the other end. The census upstream prints attribute
//     DESCRIPTORS; this one reads the buffer and prints VALUES, for the draw's
//     own first vertices.
#define KLFB_FH_ATTRS  6
#define KLFB_FH_VERTS  3
typedef struct {
    int8_t   idx;                        // attribute location, -1 = unused slot
    uint8_t  size, norm, nv;
    uint32_t type;
    int32_t  stride, buf;
    uint32_t off;
    float    v[KLFB_FH_VERTS][4];
} klfb_fh_attr;
typedef struct {
    int32_t  prog, verts, vpw, vph, tex0;
    int32_t  unit[8];                    // GL_TEXTURE0..7's bound 2D texture
    int32_t  smp[8];                     // ...and the sampler object over it
    int32_t  minf[8], basel[8], maxl[8]; // ...and the LOD state that selects a mip
    int32_t  basevert;                   // the draw's first vertex, resolved
    klfb_fh_attr attr[KLFB_FH_ATTRS];
    uint8_t  rgb[KLFB_FH_BUCK][3];       // a row across the viewport's middle
    uint8_t  col[KLFB_FH_BUCK][3];       // a column down the viewport's middle
    uint8_t  flatrgb[3];                 // the colour of that longest flat run
    uint16_t changed, fill, flat;
} klfb_fh_rec;
static klfb_fh_rec  g_fh[KLFB_FH_DRAWS];
static unsigned     g_fh_n;
static int          g_fh_on = -1, g_fh_tol, g_fh_target[3];
static int          g_fh_passes, g_fh_pass;
static double       g_fh_every, g_fh_first, g_fh_next;
static int          g_fh_active, g_fh_stage;
static uint32_t     g_fh_fb;
static uint8_t     *g_fh_strip;
static int          g_fh_stripcap;

// ---- read the fog's uniforms instead of inferring them (r28) ---------------
//
// Three rounds have now reasoned about what UE4's fog uniforms must contain.
// Nobody has looked. Program 359's vertex shader takes its whole fog from
// `vc2_h[0..5]` — UE4's ExponentialFogParameters family: density, height
// falloff, cutoff distance, min opacity, the directional-inscattering colour —
// and every one of them is a plain `uniform vec4[]` set with glUniform4fv, not
// a UBO. Which means glGetUniformfv can READ THEM BACK, per program, at any
// time, with no guest cooperation and no guessing about std140 layout.
//
// So when FILLHIST names the draw, dump that program's entire active-uniform
// state beside its GLSL. If the fog transmittance is going to zero because the
// density is enormous or the camera origin is nonsense or a MinOpacity we never
// set reads as garbage, the numbers say so directly. If they are all sane, the
// fog was never the mechanism and the surface term is.
//
// Uniform blocks are printed too (binding, size) even though this program has
// none — a different program named on a different day may.
// What the named draw SAMPLES. r28 removed the fog composite from every
// fragment shader and Austin still saw the fill, which leaves `surface` — and
// `surface` is these textures. Printed with the 2D ledger's own verdict on
// each, so a render target nobody ever wrote is visible as one.
static const char *klfb_filter_name(int32_t f) {
    switch (f) {
    case 0x2600: return "NEAREST";
    case 0x2601: return "LINEAR";
    case 0x2700: return "NEAREST_MIPMAP_NEAREST";
    case 0x2701: return "LINEAR_MIPMAP_NEAREST";
    case 0x2702: return "NEAREST_MIPMAP_LINEAR";
    case 0x2703: return "LINEAR_MIPMAP_LINEAR";
    default:     return "?";
    }
}
static int klfb_filter_is_mipped(int32_t f) { return f >= 0x2700 && f <= 0x2703; }

static void klfb_fh_units(const klfb_fh_rec *r) {
    for (int u = 0; u < 8; u++) {
        if (r->unit[u] <= 0) continue;
        uint32_t fmt = 0; int32_t w = 0, h = 0;
        char lod[192];
        // The LOD state, because a mipped 1024x1024 read at its top level is a
        // 1x1 read and paints exactly the flat constant this round is chasing.
        // A bound sampler OBJECT overrides all of it, so say when one is there
        // rather than reporting the texture's parameters as if they applied.
        snprintf(lod, sizeof lod, " min=%s base=%d max=%d%s%s",
                 klfb_filter_name(r->minf[u]), (int)r->basel[u], (int)r->maxl[u],
                 klfb_filter_is_mipped(r->minf[u]) ? "  <- MIPPED FILTER" : "",
                 r->smp[u] > 0 ? "  (a SAMPLER OBJECT is bound — it overrides "
                                 "the texture's own parameters)" : "");
        // ...and the chain, because the unit line is where "which level did it
        // read" and "was that level ever written" finally meet on one row.
        char chain[96] = "";
        int lv = 0; unsigned wm = 0;
        if (klfb_tex_chain((uint32_t)r->unit[u], &lv, &wm) && lv > 1) {
            unsigned full = (1u << (lv > KLFB_MAX_LVL ? KLFB_MAX_LVL : lv)) - 1u;
            snprintf(chain, sizeof chain, " chain=%d level(s), %d written%s",
                     lv, __builtin_popcount(wm & full),
                     (wm & full) == full ? "" : "  <- INCOMPLETE CHAIN");
        } else if (lv == 1) {
            snprintf(chain, sizeof chain, " chain=1 level (no mips)");
        }
        if (klfb_tex_info((uint32_t)r->unit[u], &fmt, &w, &h))
            fprintf(stderr, "  [glfb]   unit %d -> tex %d  %dx%d fmt=0x%04x%s%s\n",
                    u, (int)r->unit[u], (int)w, (int)h, fmt, chain, lod);
        else
            fprintf(stderr, "  [glfb]   unit %d -> tex %d  (not in the 2D ledger "
                            "— never allocated through glTexStorage/glTexImage "
                            "here)%s\n", u, (int)r->unit[u], lod);
    }
}

// The attribute VALUES the named draw actually fed the vertex shader. The
// descriptors are printed beside them because a value that looks wrong and a
// decode that is wrong are the same number until the type is on the line too.
static void klfb_fh_attrs(const klfb_fh_rec *r) {
    if (r->attr[0].idx < 0) return;
    fprintf(stderr, "  [glfb]   vertex attributes at that draw (first vertex %d):\n",
            (int)r->basevert);
    for (int a = 0; a < KLFB_FH_ATTRS; a++) {
        const klfb_fh_attr *at = &r->attr[a];
        if (at->idx < 0) continue;
        char line[320]; int n = 0;
        n += snprintf(line + n, sizeof line - n,
                      "  [glfb]     attr %-2d size=%d type=0x%04x%s stride=%d "
                      "buf=%d off=%u", (int)at->idx, (int)at->size, at->type,
                      at->norm ? " NORM" : "", (int)at->stride, (int)at->buf,
                      at->off);
        if (!at->nv) {
            snprintf(line + n, sizeof line - n, "   (buffer not readable here)");
        } else {
            int flat = 1;
            for (uint8_t v = 1; v < at->nv && flat; v++)
                for (int c = 0; c < at->size; c++)
                    if (at->v[v][c] != at->v[0][c]) { flat = 0; break; }
            for (uint8_t v = 0; v < at->nv && n < (int)sizeof line - 80; v++) {
                n += snprintf(line + n, sizeof line - n, "  [");
                for (int c = 0; c < at->size && c < 4; c++)
                    n += snprintf(line + n, sizeof line - n, "%s% .5g",
                                  c ? " " : "", at->v[v][c]);
                n += snprintf(line + n, sizeof line - n, "]");
            }
            if (flat && at->nv > 1)
                snprintf(line + n, sizeof line - n,
                         "   <- IDENTICAL ON EVERY VERTEX");
        }
        fprintf(stderr, "%s\n", line);
    }
}

static void klfb_uniform_dump(int32_t prog) {
    if (prog <= 0) return;
    // Once per program, like the GLSL dump beside it: a uniform state that
    // matters is set at load and does not churn, and a per-pass reprint would
    // bury the log it is supposed to make readable.
    static int32_t udone[8]; static unsigned nudone;
    for (unsigned i = 0; i < nudone; i++) if (udone[i] == prog) return;
    if (nudone < 8) udone[nudone++] = prog; else return;
    static void (*r_progiv)(uint32_t, uint32_t, int32_t *);
    static void (*r_active)(uint32_t, uint32_t, int32_t, int32_t *, int32_t *,
                            uint32_t *, char *);
    static void (*r_getf)(uint32_t, int32_t, float *);
    static void (*r_geti)(uint32_t, int32_t, int32_t *);
    static void (*r_blockiv)(uint32_t, uint32_t, uint32_t, int32_t *);
    static void (*r_blockname)(uint32_t, uint32_t, int32_t, int32_t *, char *);
    static int looked;
    if (!looked) {
        looked = 1;
        r_progiv   = asym("glGetProgramiv");
        r_active   = asym("glGetActiveUniform");
        r_getf     = asym("glGetUniformfv");
        r_geti     = asym("glGetUniformiv");
        r_blockiv  = asym("glGetActiveUniformBlockiv");
        r_blockname= asym("glGetActiveUniformBlockName");
    }
    int32_t (*r_loc)(uint32_t, const char *) = g_real_GetUniformLocation;
    if (!r_loc) r_loc = asym("glGetUniformLocation");
    if (!r_progiv || !r_active || !r_getf || !r_geti || !r_loc) return;

    int32_t n = 0;
    r_progiv((uint32_t)prog, 0x8B86 /* GL_ACTIVE_UNIFORMS */, &n);
    fprintf(stderr, "  [glfb] program %d uniform values (%d active):\n",
            (int)prog, (int)n);
    if (n > 64) n = 64;
    for (int32_t i = 0; i < n; i++) {
        char name[128] = {0};
        int32_t len = 0, size = 1; uint32_t type = 0;
        r_active((uint32_t)prog, (uint32_t)i, (int32_t)sizeof name, &len, &size,
                 &type, name);
        if (!name[0]) continue;
        // "vc2_h[0]" comes back for an array; strip the subscript so element
        // addresses can be rebuilt.
        char base[128];
        snprintf(base, sizeof base, "%s", name);
        char *br = strchr(base, '[');
        if (br) *br = 0;
        // Components per element, by type. Anything not in this table is
        // printed by name only — its value is not what this instrument is for.
        int comp = 0, isf = 1;
        switch (type) {
        case 0x1406: comp = 1; break;                 // GL_FLOAT
        case 0x8B50: comp = 2; break;                 // FLOAT_VEC2
        case 0x8B51: comp = 3; break;                 // FLOAT_VEC3
        case 0x8B52: comp = 4; break;                 // FLOAT_VEC4
        case 0x8B5B: comp = 9; break;                 // FLOAT_MAT3
        case 0x8B5C: comp = 16; break;                // FLOAT_MAT4
        case 0x1404: comp = 1; isf = 0; break;        // GL_INT
        default:
            // Samplers and everything else: an int, and the unit is the
            // interesting part (it says WHICH texture the shader reads).
            if (type >= 0x8B5D && type <= 0x8DFF) { comp = 1; isf = 0; }
            break;
        }
        if (!comp) { fprintf(stderr, "  [glfb]   %-24s type=0x%04x size=%d\n",
                             base, type, (int)size); continue; }
        int elems = size > 8 ? 8 : size;             // 8 vec4s is 32 floats
        for (int e = 0; e < elems; e++) {
            char elname[160];
            if (size > 1) snprintf(elname, sizeof elname, "%s[%d]", base, e);
            else          snprintf(elname, sizeof elname, "%s", base);
            int32_t loc = r_loc((uint32_t)prog, elname);
            if (loc < 0) continue;
            char line[320]; int at = 0;
            at += snprintf(line + at, sizeof line - at, "  [glfb]   %-24s", elname);
            if (isf) {
                float v[16] = {0};
                r_getf((uint32_t)prog, loc, v);
                for (int c = 0; c < comp && at < (int)sizeof line - 24; c++)
                    at += snprintf(line + at, sizeof line - at, " % .5g", v[c]);
            } else {
                int32_t v[16] = {0};
                r_geti((uint32_t)prog, loc, v);
                for (int c = 0; c < comp && at < (int)sizeof line - 24; c++)
                    at += snprintf(line + at, sizeof line - at, " %d", (int)v[c]);
            }
            fprintf(stderr, "%s\n", line);
        }
        if (size > elems)
            fprintf(stderr, "  [glfb]   %-24s ... %d more element(s)\n", base,
                    (int)(size - elems));
    }
    // Uniform blocks, if this program has any: the binding and the size are
    // what a mis-indexed glBindBufferBase would show up in.
    if (r_blockiv && r_blockname) {
        int32_t nb = 0;
        r_progiv((uint32_t)prog, 0x8A36 /* ACTIVE_UNIFORM_BLOCKS */, &nb);
        for (int32_t b = 0; b < nb && b < 16; b++) {
            char bn[128] = {0}; int32_t bl = 0, sz = 0, bind = -1;
            r_blockname((uint32_t)prog, (uint32_t)b, (int32_t)sizeof bn, &bl, bn);
            r_blockiv((uint32_t)prog, (uint32_t)b, 0x8A40 /* DATA_SIZE */, &sz);
            r_blockiv((uint32_t)prog, (uint32_t)b, 0x8A3F /* BINDING */, &bind);
            fprintf(stderr, "  [glfb]   block %-18s binding=%d size=%d\n",
                    bn, (int)bind, (int)sz);
        }
    }
}

static void klfb_fh_dump_prog(int32_t prog) {
    static const char *dir = (const char *)-1;
    if (dir == (const char *)-1) dir = kl_env_str("KL_GLFB_DUMP_GLSL_FILL", NULL);
    if (!dir || prog <= 0) return;
    static int32_t done[8]; static unsigned ndone;
    for (unsigned i = 0; i < ndone; i++) if (done[i] == prog) return;
    if (ndone < 8) done[ndone++] = prog;
    static void (*r_att)(uint32_t, int32_t, int32_t *, uint32_t *);
    if (!r_att) r_att = asym("glGetAttachedShaders");
    if (!r_att) return;
    uint32_t sh[8]; int32_t n = 0;
    r_att((uint32_t)prog, 8, &n, sh);
    for (int32_t i = 0; i < n; i++) {
        const char *src = klfb_shader_src(sh[i]);
        if (!src) continue;
        char path[600];
        snprintf(path, sizeof path, "%s/fill_p%04d_sh%06u.glsl", dir, (int)prog,
                 sh[i]);
        FILE *f = fopen(path, "w");
        if (f) { fputs(src, f); fclose(f); }
    }
    if (n == 0)
        fprintf(stderr, "  [glfb] FILLHIST: program %d has NO attached shaders — it "
                        "was linked from a PROGRAM BINARY, so this process never saw "
                        "its source. That is why r29's log carried no GLSL for it; "
                        "bump KL_GLFB_REWRITE_GEN to drop the cache and the next "
                        "launch dumps it.\n", (int)prog);
    else
        fprintf(stderr, "  [glfb] FILLHIST: wrote program %d's %d shader(s) to %s\n",
                (int)prog, (int)n, dir);
}

static void klfb_fh_report(void) {
    if (!g_fh_n) { g_fh_active = 0; return; }
    fprintf(stderr, "  [glfb] FILLHIST pass %d: fb %u (eye stage %d), %u draw(s) "
                    "sampled, %d buckets across the draw's own viewport; fill "
                    "target sRGB(%d,%d,%d) +-%d\n",
            g_fh_pass, g_fh_fb, g_fh_stage, g_fh_n, KLFB_FH_BUCK,
            g_fh_target[0], g_fh_target[1], g_fh_target[2], g_fh_tol);
    int prev_fill = 0, prev_flat = 0, printed = 0;
    int best = -1, best_gain = 0, bestf = -1, bestf_gain = 0;
    for (unsigned i = 0; i < g_fh_n; i++) {
        int gain = (int)g_fh[i].fill - prev_fill;
        prev_fill = g_fh[i].fill;
        int fgain = (int)g_fh[i].flat - prev_flat;
        prev_flat = g_fh[i].flat;
        // A long run of PURE BLACK is the cleared eye texture, not a fill —
        // r28's passes 1 and 2 nominated a draw for turning a viewport black
        // and that is noise. The blue fill is sRGB(0,24,46); anything this
        // dark in all three channels is the clear.
        int notblack = g_fh[i].flatrgb[0] > 6 || g_fh[i].flatrgb[1] > 6
                                              || g_fh[i].flatrgb[2] > 6;
        if (fgain > bestf_gain && notblack) { bestf_gain = fgain; bestf = (int)i; }
        if (gain > best_gain) { best_gain = gain; best = (int)i; }
        // Print the draws that matter: a wide change, or a jump in
        // fill-coloured buckets. The rest is one small sprite among dozens.
        int loud = g_fh[i].changed >= KLFB_FH_BUCK / 4 || gain >= 2 || fgain >= 4;
        if (!loud || printed >= 24) continue;
        printed++;
        fprintf(stderr, "  [glfb]   draw %3u prog=%d verts=%d vp=%dx%d tex0=%d "
                        "-> %u/%d bucket(s) changed, %u fill-coloured (%+d), "
                        "flat run %u (%+d)"
                        "   [%02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x]\n",
                i, (int)g_fh[i].prog, (int)g_fh[i].verts, (int)g_fh[i].vpw,
                (int)g_fh[i].vph, (int)g_fh[i].tex0, g_fh[i].changed,
                KLFB_FH_BUCK, g_fh[i].fill, gain, g_fh[i].flat, fgain,
                g_fh[i].rgb[4][0],  g_fh[i].rgb[4][1],  g_fh[i].rgb[4][2],
                g_fh[i].rgb[12][0], g_fh[i].rgb[12][1], g_fh[i].rgb[12][2],
                g_fh[i].rgb[20][0], g_fh[i].rgb[20][1], g_fh[i].rgb[20][2],
                g_fh[i].rgb[28][0], g_fh[i].rgb[28][1], g_fh[i].rgb[28][2]);
    }
    // The strip as it ended the pass, every bucket — ground truth about the
    // fill's colour, measured off the eye texture itself rather than off a
    // screenshot that has been through the compositor and a display.
    const klfb_fh_rec *last = &g_fh[g_fh_n - 1];
    fprintf(stderr, "  [glfb]   final strip (across):");
    for (int b = 0; b < KLFB_FH_BUCK; b++)
        fprintf(stderr, " %02x%02x%02x", last->rgb[b][0], last->rgb[b][1],
                last->rgb[b][2]);
    fprintf(stderr, "\n");
    // The column is the one that answers r20's screenshots: top of the
    // viewport on the left of the line, bottom on the right. A flat navy run
    // followed by black followed by texture IS the banding, measured.
    fprintf(stderr, "  [glfb]   final column (top->bottom):");
    for (int b = 0; b < KLFB_FH_BUCK; b++)
        fprintf(stderr, " %02x%02x%02x", last->col[b][0], last->col[b][1],
                last->col[b][2]);
    fprintf(stderr, "\n");
    // Name the draw whichever way it can be named. The colour-matched answer
    // first, because it is the more specific claim; the colour-agnostic
    // longest-flat-run answer second, because r26's pass proved the target
    // colour can be off and a pass that only knows one colour goes silent
    // instead of reporting the draw it did in fact see.
    if (best >= 0 && best_gain >= 2) {
        fprintf(stderr, "  [glfb]   THE FILL IS DRAW %d: program %d turned %d "
                        "bucket(s) fill-coloured in one call (%d verts, "
                        "tex0=%d)\n",
                best, (int)g_fh[best].prog, best_gain, (int)g_fh[best].verts,
                (int)g_fh[best].tex0);
        klfb_fh_units(&g_fh[best]);
        klfb_fh_attrs(&g_fh[best]);
        klfb_uniform_dump(g_fh[best].prog);
        klfb_fh_dump_prog(g_fh[best].prog);
    } else if (bestf >= 0 && bestf_gain >= 4) {
        fprintf(stderr, "  [glfb]   THE FILL IS DRAW %d (by FLAT RUN, colour "
                        "not matched): program %d grew the longest constant run "
                        "by %d bucket(s) to %d, colour %02x%02x%02x (%d verts, "
                        "tex0=%d)\n",
                bestf, (int)g_fh[bestf].prog, bestf_gain, (int)g_fh[bestf].flat,
                g_fh[bestf].flatrgb[0], g_fh[bestf].flatrgb[1],
                g_fh[bestf].flatrgb[2],
                (int)g_fh[bestf].verts, (int)g_fh[bestf].tex0);
        klfb_fh_units(&g_fh[bestf]);
        klfb_fh_attrs(&g_fh[bestf]);
        klfb_uniform_dump(g_fh[bestf].prog);
        klfb_fh_dump_prog(g_fh[bestf].prog);
    } else {
        fprintf(stderr, "  [glfb]   no draw in this pass turned a wide band "
                        "constant in a colour that is not the clear — the fill "
                        "was not on either sample, or it was already there when "
                        "the pass opened. Read the two lines above.\n");
    }

    // ---- the SHAPE key (r30) ----------------------------------------------
    //
    // Every nomination above is keyed on the fill's COLOUR, and r29's build
    // took the fill away: Austin wore NOMIP=1 and the navy is gone, so the
    // instrument that found the draw for five rounds now goes silent on the
    // build that most needs to describe it. All four of this run's passes
    // named nothing, which is not evidence of anything.
    //
    // The draw is still being made, and r29 measured its shape exactly:
    // program 276, SIX vertices, the full eye viewport, two 1024x1024 ASTC
    // units. KL_GLFB_FH_SHAPE=<verts> nominates by that instead — vertex count
    // and a viewport at least half the eye's width — so the units, the
    // uniforms and above all the vertex-ATTRIBUTE values print on a build where
    // the picture is fixed. Independent of the colour nomination and printed
    // beside it: if both name the same draw the log says so by the index.
    //
    // Off by default; baked to 6 for tetris, which is the one title whose fill
    // draw has a measured vertex count.
    static int shape = -1;
    if (shape < 0) shape = kl_env_int("KL_GLFB_FH_SHAPE", 0);
    if (shape > 0) {
        int hit = -1;
        for (unsigned i = 0; i < g_fh_n; i++)
            if (g_fh[i].verts == shape && g_fh[i].vpw >= 2048 && g_fh[i].vph >= 2048)
                hit = (int)i;              // the LAST one, which is the topmost
        if (hit >= 0) {
            fprintf(stderr, "  [glfb]   SHAPE MATCH: draw %d is program %d with "
                            "%d verts into vp=%dx%d — r29's fill draw by shape, "
                            "named without needing the fill's colour\n",
                    hit, (int)g_fh[hit].prog, (int)g_fh[hit].verts,
                    (int)g_fh[hit].vpw, (int)g_fh[hit].vph);
            klfb_fh_units(&g_fh[hit]);
            klfb_fh_attrs(&g_fh[hit]);
            klfb_uniform_dump(g_fh[hit].prog);
            klfb_fh_dump_prog(g_fh[hit].prog);
        } else {
            fprintf(stderr, "  [glfb]   SHAPE MATCH: no draw in this pass had "
                            "%d vertices into a half-eye-or-wider viewport\n",
                    shape);
        }
    }
    g_fh_active = 0;
    g_fh_n = 0;
}

// GL_HALF_FLOAT is a perfectly ordinary vertex format and printing its bits as
// an integer would be a fifth theory rather than a measurement.
static float klfb_attr_half(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) & 1u, e = (uint32_t)(h >> 10) & 0x1fu,
             m = (uint32_t)h & 0x3ffu, bits;
    if (e == 0) {
        if (!m) bits = s << 31;                       // +-0
        else {                                        // subnormal
            e = 1;
            while (!(m & 0x400u)) { m <<= 1; e--; }
            m &= 0x3ffu;
            bits = (s << 31) | ((e + 127 - 15) << 23) | (m << 13);
        }
    } else if (e == 31) {
        bits = (s << 31) | 0x7f800000u | (m << 13);   // inf/nan
    } else {
        bits = (s << 31) | ((e + 127 - 15) << 23) | (m << 13);
    }
    float f; memcpy(&f, &bits, sizeof f); return f;
}

static int klfb_attr_typesize(uint32_t t) {
    switch (t) {
    case 0x1400: case 0x1401: return 1;               // (UNSIGNED_)BYTE
    case 0x1402: case 0x1403: return 2;               // (UNSIGNED_)SHORT
    case 0x140B: return 2;                            // HALF_FLOAT
    case 0x1404: case 0x1405: case 0x1406: return 4;  // (U)INT, FLOAT
    case 0x8368: case 0x8D9F: return 4;               // 2_10_10_10_REV, packed
    default: return 0;
    }
}

// The first vertex the draw actually reads. glDrawArrays states it; an indexed
// draw hides it in the element buffer, and reading element 0 of the vertex
// buffer instead would confidently describe a different mesh.
static int32_t klfb_fh_basevert(void) {
    if (g_lastdraw.first >= 0) return g_lastdraw.first;
    static void *(*r_map)(uint32_t, intptr_t, intptr_t, uint32_t);
    static uint8_t (*r_unmap)(uint32_t);
    static int looked;
    if (!looked) {
        looked = 1;
        r_map   = asym("glMapBufferRange");
        r_unmap = asym("glUnmapBuffer");
    }
    int32_t ebo = 0;
    a_glGetIntegerv(0x8895 /* ELEMENT_ARRAY_BUFFER_BINDING */, &ebo);
    if (ebo <= 0 || !r_map || !r_unmap) return g_lastdraw.basevertex;
    int isz = klfb_attr_typesize(g_lastdraw.itype);
    if (isz != 1 && isz != 2 && isz != 4) return g_lastdraw.basevertex;
    void *p = r_map(0x8893 /* ELEMENT_ARRAY_BUFFER */,
                    (intptr_t)(uintptr_t)g_lastdraw.indices, (intptr_t)isz,
                    0x0001 /* MAP_READ_BIT */);
    if (!p) return g_lastdraw.basevertex;
    uint32_t idx = 0;
    if (isz == 1) { uint8_t v;  memcpy(&v, p, 1); idx = v; }
    if (isz == 2) { uint16_t v; memcpy(&v, p, 2); idx = v; }
    if (isz == 4) { uint32_t v; memcpy(&v, p, 4); idx = v; }
    r_unmap(0x8893);
    return (int32_t)idx + g_lastdraw.basevertex;
}

// KL_GLFB_FH_ATTRVALS=0 turns the value read off and leaves the descriptors.
// The read maps the guest's own vertex buffers, which is safe HERE and nowhere
// else: FILLHIST already serialises this frame with a readback per draw, so
// the implicit sync a map forces is one that has been paid already.
static void klfb_fh_grab_attribs(klfb_fh_rec *r) {
    for (int a = 0; a < KLFB_FH_ATTRS; a++) r->attr[a].idx = -1;
    r->basevert = 0;
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_FH_ATTRVALS", 1);
    static void (*r_attriv)(uint32_t, uint32_t, int32_t *);
    static void (*r_attrptr)(uint32_t, uint32_t, void **);
    static void (*r_bindbuf)(uint32_t, uint32_t);
    static void *(*r_map)(uint32_t, intptr_t, intptr_t, uint32_t);
    static uint8_t (*r_unmap)(uint32_t);
    static int looked;
    if (!looked) {
        looked = 1;
        r_attriv  = asym("glGetVertexAttribiv");
        r_attrptr = asym("glGetVertexAttribPointerv");
        r_bindbuf = asym("glBindBuffer");
        r_map     = asym("glMapBufferRange");
        r_unmap   = asym("glUnmapBuffer");
    }
    if (!on || !r_attriv) return;
    r->basevert = klfb_fh_basevert();
    int32_t wasbuf = 0;
    a_glGetIntegerv(0x8894 /* ARRAY_BUFFER_BINDING */, &wasbuf);
    int slot = 0;
    for (int32_t i = 0; i < 16 && slot < KLFB_FH_ATTRS; i++) {
        int32_t en = 0, buf = 0, size = 0, stride = 0, ty = 0, norm = 0;
        void *ptr = NULL;
        r_attriv((uint32_t)i, 0x8622 /* ARRAY_ENABLED */, &en);
        if (!en) continue;
        r_attriv((uint32_t)i, 0x889F /* ARRAY_BUFFER_BINDING */, &buf);
        r_attriv((uint32_t)i, 0x8623 /* ARRAY_SIZE */, &size);
        r_attriv((uint32_t)i, 0x8624 /* ARRAY_STRIDE */, &stride);
        r_attriv((uint32_t)i, 0x8625 /* ARRAY_TYPE */, &ty);
        r_attriv((uint32_t)i, 0x886A /* ARRAY_NORMALIZED */, &norm);
        if (r_attrptr) r_attrptr((uint32_t)i, 0x8645 /* ARRAY_POINTER */, &ptr);
        klfb_fh_attr *at = &r->attr[slot++];
        at->idx = (int8_t)i; at->size = (uint8_t)(size > 4 ? 4 : size);
        at->norm = (uint8_t)!!norm; at->type = (uint32_t)ty;
        at->stride = stride; at->buf = buf; at->off = (uint32_t)(uintptr_t)ptr;
        at->nv = 0;
        int tsz = klfb_attr_typesize((uint32_t)ty);
        int packed = (ty == 0x8368 || ty == 0x8D9F);
        if (!tsz || buf <= 0 || !r_bindbuf || !r_map || !r_unmap) continue;
        int elem = packed ? 4 : tsz * (int)at->size;
        int st = stride ? stride : elem;
        intptr_t base = (intptr_t)at->off + (intptr_t)r->basevert * st;
        intptr_t len  = (intptr_t)st * (KLFB_FH_VERTS - 1) + elem;
        r_bindbuf(0x8892 /* ARRAY_BUFFER */, (uint32_t)buf);
        uint8_t *p = r_map(0x8892, base, len, 0x0001 /* MAP_READ_BIT */);
        if (p) {
            for (int v = 0; v < KLFB_FH_VERTS; v++) {
                const uint8_t *q = p + (size_t)v * (size_t)st;
                if (packed) {
                    uint32_t w; memcpy(&w, q, 4);
                    for (int c = 0; c < 3; c++) {
                        int32_t x = (int32_t)((w >> (c * 10)) & 0x3ffu);
                        if (ty == 0x8D9F && x > 511) x -= 1024;      // signed
                        at->v[v][c] = norm ? (float)x / (ty == 0x8D9F ? 511.f : 1023.f)
                                           : (float)x;
                    }
                    at->v[v][3] = (float)((w >> 30) & 3u);
                    continue;
                }
                for (int c = 0; c < (int)at->size; c++) {
                    const uint8_t *e = q + (size_t)c * (size_t)tsz;
                    float f = 0;
                    switch (ty) {
                    case 0x1406: memcpy(&f, e, 4); break;
                    case 0x140B: { uint16_t h; memcpy(&h, e, 2);
                                   f = klfb_attr_half(h); } break;
                    case 0x1400: { int8_t x;   memcpy(&x, e, 1);
                                   f = norm ? (float)x / 127.f : (float)x; } break;
                    case 0x1401: { uint8_t x;  memcpy(&x, e, 1);
                                   f = norm ? (float)x / 255.f : (float)x; } break;
                    case 0x1402: { int16_t x;  memcpy(&x, e, 2);
                                   f = norm ? (float)x / 32767.f : (float)x; } break;
                    case 0x1403: { uint16_t x; memcpy(&x, e, 2);
                                   f = norm ? (float)x / 65535.f : (float)x; } break;
                    case 0x1404: { int32_t x;  memcpy(&x, e, 4); f = (float)x; } break;
                    case 0x1405: { uint32_t x; memcpy(&x, e, 4); f = (float)x; } break;
                    default: break;
                    }
                    at->v[v][c] = f;
                }
            }
            at->nv = KLFB_FH_VERTS;
            r_unmap(0x8892);
        }
    }
    if (r_bindbuf) r_bindbuf(0x8892, (uint32_t)(wasbuf > 0 ? wasbuf : 0));
    if (a_glGetError) while (a_glGetError()) {}
}

static void klfb_fillhist(int verts) {
    if (g_fh_on < 0) {
        g_fh_on = kl_env_on("KL_GLFB_FILLHIST", 0);
        const char *rgb = kl_env_str("KL_GLFB_FILLHIST_RGB", "0,24,46");
        if (!rgb || sscanf(rgb, "%d,%d,%d", &g_fh_target[0], &g_fh_target[1],
                           &g_fh_target[2]) != 3) {
            g_fh_target[0] = 0; g_fh_target[1] = 24; g_fh_target[2] = 46;
        }
        g_fh_tol    = kl_env_int("KL_GLFB_FILLHIST_TOL", 10);
        g_fh_every  = kl_env_int("KL_GLFB_FILLHIST_EVERY", 30);
        g_fh_first  = kl_env_int("KL_GLFB_FILLHIST_FIRST", 45);
        g_fh_passes = kl_env_int("KL_GLFB_FILLHIST_N", 4);
        g_fh_next   = klfb_now() + g_fh_first;
    }
    if (!g_fh_on || !a_glReadPixels || !a_glGetIntegerv) return;

    // Ask GL, not the interception ledger: g_draw_fb tracks glBindFramebuffer,
    // and a guest that binds through a path we do not wrap would put the strip
    // on the wrong target silently. The binding is what the draw just used.
    int32_t dfb = 0;
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &dfb);
    if (dfb <= 0) return;
    int stage = klfb_stage_of_fbo((uint32_t)dfb);
    if (!g_fh_active) {
        if (g_fh_pass >= g_fh_passes || stage < 0) return;
        if (klfb_now() < g_fh_next) return;
        g_fh_active = 1; g_fh_pass++; g_fh_n = 0;
        g_fh_fb = (uint32_t)dfb; g_fh_stage = stage;
        g_fh_next = klfb_now() + g_fh_every;
    }
    // The pass is one eye framebuffer's run of draws. A draw on a DIFFERENT eye
    // framebuffer is the next frame (three stages, round-robin) and closes it;
    // a draw on a non-eye target (shadow maps, the GPU-particle simulation) is
    // simply not sampled, because it cannot paint anything Austin sees.
    if ((uint32_t)dfb != g_fh_fb) {
        if (stage >= 0) klfb_fh_report();
        return;
    }
    if (g_fh_n >= KLFB_FH_DRAWS) { klfb_fh_report(); return; }

    int32_t vp[4] = {0,0,0,0}, prog = -1, tex0 = -1, rfb = -1;
    a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
    a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &tex0);
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &rfb);
    int w = vp[2], h = vp[3];
    if (w < KLFB_FH_BUCK || h < KLFB_FH_BUCK) return;
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;
    int need = w > h ? w : h;
    if (need > g_fh_stripcap) {
        uint8_t *p = realloc(g_fh_strip, (size_t)need * 4);
        if (!p) return;
        g_fh_strip = p; g_fh_stripcap = need;
    }
    static void (*bindfb)(uint32_t, uint32_t);
    if (!bindfb) bindfb = asym("glBindFramebuffer");
    if (!bindfb) return;
    klfb_fh_rec *r = &g_fh[g_fh_n];
    const klfb_fh_rec *prev = g_fh_n ? &g_fh[g_fh_n - 1] : NULL;
    r->prog = prog; r->verts = verts; r->vpw = vp[2]; r->vph = vp[3];
    r->tex0 = tex0; r->changed = 0; r->fill = 0; r->flat = 0;
    r->flatrgb[0] = r->flatrgb[1] = r->flatrgb[2] = 0;
    // r28 closed the fog family, so the next question is what this draw
    // SAMPLES: tex0 alone is unit 0, and program 359's fragment shader has
    // three sampler2Ds. Walk units 0..7 and remember what is bound, restoring
    // the active unit afterwards. This pass already serialises the frame with
    // a readback per draw, so the extra state queries cost nothing that is not
    // already being paid.
    for (int u = 0; u < 8; u++) {
        r->unit[u] = -1; r->smp[u] = 0;
        r->minf[u] = 0; r->basel[u] = -1; r->maxl[u] = -1;
    }
    {
        static void (*r_active_tex)(uint32_t);
        static void (*r_gettexpiv)(uint32_t, uint32_t, int32_t *);
        static int looked;
        if (!looked) {
            looked = 1;
            r_active_tex = asym("glActiveTexture");
            r_gettexpiv  = asym("glGetTexParameteriv");
        }
        if (r_active_tex) {
            int32_t was = 0x84C0;
            a_glGetIntegerv(0x84E0 /* ACTIVE_TEXTURE */, &was);
            for (int u = 0; u < 8; u++) {
                r_active_tex((uint32_t)(0x84C0 + u));
                int32_t t = -1;
                a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &t);
                r->unit[u] = t;
                a_glGetIntegerv(0x8919 /* SAMPLER_BINDING */, &r->smp[u]);
                if (t > 0 && r_gettexpiv) {
                    r_gettexpiv(0x0DE1, 0x2801 /* MIN_FILTER */, &r->minf[u]);
                    r_gettexpiv(0x0DE1, 0x813C /* TEXTURE_BASE_LEVEL */, &r->basel[u]);
                    r_gettexpiv(0x0DE1, 0x813D /* TEXTURE_MAX_LEVEL */, &r->maxl[u]);
                }
            }
            r_active_tex((uint32_t)was);
        }
    }
    klfb_fh_grab_attribs(r);

    if (a_glGetError) while (a_glGetError()) {}
    bindfb(0x8CA8 /* READ_FRAMEBUFFER */, g_fh_fb);
    // Two reads, two axes. Across first, then down the middle of the same
    // viewport — the column is what crosses the screen-horizontal band edges
    // the fill is cut by.
    a_glReadPixels(vp[0], vp[1] + h / 2, w, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                   g_fh_strip);
    for (int b = 0; b < KLFB_FH_BUCK; b++) {
        int x0 = b * w / KLFB_FH_BUCK, x1 = (b + 1) * w / KLFB_FH_BUCK;
        if (x1 <= x0) x1 = x0 + 1;
        unsigned sum[3] = {0,0,0};
        for (int x = x0; x < x1; x++) {
            sum[0] += g_fh_strip[x * 4 + 0];
            sum[1] += g_fh_strip[x * 4 + 1];
            sum[2] += g_fh_strip[x * 4 + 2];
        }
        unsigned n = (unsigned)(x1 - x0);
        for (int c = 0; c < 3; c++) r->rgb[b][c] = (uint8_t)(sum[c] / n);
        if (prev && (abs((int)r->rgb[b][0] - (int)prev->rgb[b][0]) > 1
                  || abs((int)r->rgb[b][1] - (int)prev->rgb[b][1]) > 1
                  || abs((int)r->rgb[b][2] - (int)prev->rgb[b][2]) > 1))
            r->changed++;
        if (abs((int)r->rgb[b][0] - g_fh_target[0]) <= g_fh_tol
         && abs((int)r->rgb[b][1] - g_fh_target[1]) <= g_fh_tol
         && abs((int)r->rgb[b][2] - g_fh_target[2]) <= g_fh_tol)
            r->fill++;
    }
    // GL's origin is bottom-left; the column is stored TOP-first so the log
    // reads the way the picture looks.
    a_glReadPixels(vp[0] + w / 2, vp[1], 1, h, GL_RGBA, GL_UNSIGNED_BYTE,
                   g_fh_strip);
    bindfb(0x8CA8, (uint32_t)(rfb >= 0 ? rfb : 0));
    // Drain: the guest leaves errors unread, and a readback this instrument
    // could not do must not become the guest's next glGetError.
    if (a_glGetError) while (a_glGetError()) {}
    for (int b = 0; b < KLFB_FH_BUCK; b++) {
        int y0 = b * h / KLFB_FH_BUCK, y1 = (b + 1) * h / KLFB_FH_BUCK;
        if (y1 <= y0) y1 = y0 + 1;
        unsigned sum[3] = {0,0,0};
        for (int y = y0; y < y1; y++) {
            sum[0] += g_fh_strip[y * 4 + 0];
            sum[1] += g_fh_strip[y * 4 + 1];
            sum[2] += g_fh_strip[y * 4 + 2];
        }
        unsigned n = (unsigned)(y1 - y0);
        int t = KLFB_FH_BUCK - 1 - b;             // flip to top-first
        for (int c = 0; c < 3; c++) r->col[t][c] = (uint8_t)(sum[c] / n);
    }
    // Longest run of near-equal buckets down the column, colour-agnostic. This
    // is what makes the instrument able to name a fill whose colour nobody
    // guessed right — and a black picture reports a long run too, which is
    // itself worth seeing rather than being reported as nothing.
    {
        int run = 1, bestrun = 1, bestat = 0;
        for (int b = 1; b < KLFB_FH_BUCK; b++) {
            if (abs((int)r->col[b][0] - (int)r->col[b-1][0]) <= 2
             && abs((int)r->col[b][1] - (int)r->col[b-1][1]) <= 2
             && abs((int)r->col[b][2] - (int)r->col[b-1][2]) <= 2) run++;
            else run = 1;
            if (run > bestrun) { bestrun = run; bestat = b; }
        }
        r->flat = (uint16_t)bestrun;
        // The run's OWN colour, not the middle bucket's: r28's log named
        // program 60 by a flat run of pure black, which is the cleared
        // framebuffer rather than a fill, and the report needs to be able to
        // tell those apart.
        for (int c = 0; c < 3; c++) r->flatrgb[c] = r->col[bestat][c];
    }
    g_fh_n++;
}

// KL_GLFB_DRAW_PROBE=1: after each of the first few draws, read back the
// current DRAW framebuffer and say whether the draw emitted anything.
// Distinguishes "draws run but output black" from "content drawn but lost
// before the capture" — the readback happens inside the same command stream,
// so there is no timing or thread question left. The read goes through
// klfb_probe_fbo: the scene target is an RGBA16F 4xMSAA renderbuffer, which a
// direct RGBA/UNSIGNED_BYTE readback cannot see — it answers err 0x500.
static void klfb_draw_probe(int verts) {
    // Every draw wrapper calls this AFTER the real draw, which is exactly where
    // the pixel history has to sample — so it rides here rather than adding a
    // sixth call site to five wrappers.
    klfb_fillhist(verts);
    static int on = -1, said, quota, skip, seen;
    if (on < 0) {
        on = kl_env_on("KL_GLFB_DRAW_PROBE", 0);
        // KL_GLFB_DRAW_PROBE_N overrides the 12-line default; scene frames
        // burn the quota on early frames otherwise. 0 means unlimited.
        quota = kl_env_int("KL_GLFB_DRAW_PROBE_N", 12);
        // ...and KL_GLFB_DRAW_PROBE_SKIP is the other end of that problem: a
        // guest whose STARTUP renders and whose steady state does not can only
        // be caught after the transition, and "unlimited from draw 0" is a
        // readback per draw for the whole run. Skip the first N eligible draws
        // and spend the quota where the question is.
        skip = kl_env_int("KL_GLFB_DRAW_PROBE_SKIP", 0);
    }
    if (!on || (quota && said >= quota) || !a_glReadPixels) return;
    // KL_GLFB_DRAW_PROBE_MIN lowers the 32-vert floor — the frame's last few
    // draws are small, and one of them is a suspect in the scene's erasure.
    static int minv = -1;
    if (minv < 0) {
        minv = kl_env_int("KL_GLFB_DRAW_PROBE_MIN", 12);
    }
    if (verts < minv) return;
    if (seen++ < skip) return;
    int32_t fb = -1, rfb = -1;
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
        a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &rfb);
    }
    if (a_glFinish) a_glFinish();
    said++;
    static float *pfb;
    static uint8_t *pbb;
    static void (*dp_bind)(uint32_t, uint32_t);
    if (!pfb) {
        pfb = malloc((size_t)g_w * g_h * 16);
        pbb = malloc((size_t)g_w * g_h * 4);
        dp_bind = asym("glBindFramebuffer");
    }
    char note[160] = "";
    unsigned long lit = 0;
    // The viewport is the size the draw itself was aimed at, which is the best
    // available answer for a target the allocation table never saw.
    int32_t dvp[4] = {0, 0, 0, 0};
    if (a_glGetIntegerv) a_glGetIntegerv(0x0BA2 /* VIEWPORT */, dvp);
    if (pfb && pbb)
        lit = klfb_probe_fbo((uint32_t)fb, pfb, pbb, note, sizeof note,
                             NULL, NULL, NULL, dvp[2], dvp[3]);
    if (dp_bind) dp_bind(0x8CA8, (uint32_t)(rfb >= 0 ? rfb : 0));
    fprintf(stderr, "  [glfb] DRAW_PROBE fb=%d: %lu lit (%s)\n", fb, lit, note);
}

// KL_GLFB_ATTRIB_DUMP=N: before each of the first N draws, print the whole
// vertex-attribute table AS THE DRIVER SEES IT — one line per attribute index
// (enabled, the buffer bound to its binding, its client pointer, size/type/
// stride/divisor) plus the linked program's active attributes and the location
// the linker gave each of them. Read from GL rather than from an interception
// ledger on purpose: ANGLE's "client-side attribute" test is exactly
// `enabled && bound buffer == 0`, and this is that test's own inputs.
static void klfb_attrib_dump(const char *what) {
    static int quota = -1, done;
    if (quota < 0) quota = kl_env_int("KL_GLFB_ATTRIB_DUMP", 0);
    if (quota <= 0 || done >= quota || !a_glGetIntegerv) return;
    static void (*r_GetVertexAttribiv)(uint32_t, uint32_t, int32_t *);
    static void (*r_GetVertexAttribPointerv)(uint32_t, uint32_t, void **);
    static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
    static void (*r_GetActiveAttrib)(uint32_t, uint32_t, int32_t, int32_t *,
                                     int32_t *, uint32_t *, char *);
    static int32_t (*r_GetAttribLocation)(uint32_t, const char *);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        r_GetVertexAttribiv       = asym("glGetVertexAttribiv");
        r_GetVertexAttribPointerv = asym("glGetVertexAttribPointerv");
        r_GetProgramiv            = asym("glGetProgramiv");
        r_GetActiveAttrib         = asym("glGetActiveAttrib");
        r_GetAttribLocation       = asym("glGetAttribLocation");
    }
    if (!r_GetVertexAttribiv) return;
    done++;

    int32_t maxa = 0, prog = -1, vao = -1, arraybuf = -1;
    a_glGetIntegerv(0x8869 /* MAX_VERTEX_ATTRIBS */, &maxa);
    a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
    a_glGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &vao);
    a_glGetIntegerv(0x8894 /* ARRAY_BUFFER_BINDING */, &arraybuf);
    fprintf(stderr, "  [glfb] ATTRIB_DUMP #%d at %s: t%llu program=%d "
                    "VAO=%d ARRAY_BUFFER=%d max_attribs=%d\n",
            done, what ? what : "?",
            (unsigned long long)klfb_tid(), prog, vao, arraybuf, maxa);
    if (maxa > 32) maxa = 32;
    for (int32_t i = 0; i < maxa; i++) {
        int32_t en = 0, buf = 0, size = 0, stride = 0, div = 0;
        uint32_t ty = 0;
        void *ptr = NULL;
        int32_t t = 0;
        r_GetVertexAttribiv((uint32_t)i, 0x8622 /* ARRAY_ENABLED */, &en);
        r_GetVertexAttribiv((uint32_t)i, 0x889F /* ARRAY_BUFFER_BINDING */, &buf);
        r_GetVertexAttribiv((uint32_t)i, 0x8623 /* ARRAY_SIZE */, &size);
        r_GetVertexAttribiv((uint32_t)i, 0x8624 /* ARRAY_STRIDE */, &stride);
        r_GetVertexAttribiv((uint32_t)i, 0x8625 /* ARRAY_TYPE */, &t);
        ty = (uint32_t)t;
        r_GetVertexAttribiv((uint32_t)i, 0x88FE /* ARRAY_DIVISOR */, &div);
        if (r_GetVertexAttribPointerv)
            r_GetVertexAttribPointerv((uint32_t)i, 0x8645 /* ARRAY_POINTER */, &ptr);
        if (!en && !buf && !ptr) continue;          // untouched default state
        fprintf(stderr, "  [glfb]   attr %-2d %s buffer=%-4d pointer=%p "
                        "size=%d type=0x%x stride=%d divisor=%d%s\n",
                i, en ? "ENABLED " : "disabled", buf, ptr, size, ty, stride, div,
                (en && buf == 0) ? (ptr ? "   <- CLIENT ARRAY"
                                        : "   <- CLIENT ARRAY, POINTER IS NULL")
                                 : "");
    }
    if (prog > 0 && r_GetProgramiv && r_GetActiveAttrib) {
        int32_t n = 0, len = 0;
        r_GetProgramiv((uint32_t)prog, 0x8B89 /* ACTIVE_ATTRIBUTES */, &n);
        r_GetProgramiv((uint32_t)prog, 0x8B8A /* ACTIVE_ATTRIBUTE_MAX_LENGTH */, &len);
        fprintf(stderr, "  [glfb]   program %d has %d active attributes\n", prog, n);
        for (int32_t i = 0; i < n && i < 32; i++) {
            char nm[128] = "";
            int32_t wrote = 0, sz = 0;
            uint32_t ty = 0;
            r_GetActiveAttrib((uint32_t)prog, (uint32_t)i, (int32_t)sizeof nm,
                              &wrote, &sz, &ty, nm);
            int32_t loc = r_GetAttribLocation ? r_GetAttribLocation((uint32_t)prog, nm)
                                              : -1;
            fprintf(stderr, "  [glfb]     active attr[%d] \"%s\" location=%d "
                            "size=%d type=0x%x\n", i, nm, loc, sz, ty);
        }
    }
}

// KL_GLFB_ATTRIB_TRACE=N: the first N calls of the vertex-array setup family,
// each stamped with the thread and the REAL ANGLE context it landed on. This
// is the half the dump above cannot show — the dump says what the state IS at
// the draw, the trace says which context each half of that state was written
// to. VAOs and attribute state are per-context even inside a share group, so a
// setup call and its draw on different contexts is a silent state loss.
static void *klfb_real_ctx_here(void) {
    klfb_thread_gl *t = pthread_getspecific(g_tls_key);
    return t ? t->ctx : NULL;
}
static int g_atrace_quota = -1, g_atrace_used;
static int klfb_atrace_armed(void) {
    if (g_atrace_quota < 0) g_atrace_quota = kl_env_int("KL_GLFB_ATTRIB_TRACE", 0);
    return g_atrace_quota > 0 && g_atrace_used < g_atrace_quota;
}
#define KLFB_ATRACE(fmt, ...)                                                 \
    do { if (klfb_atrace_armed()) {                                           \
        g_atrace_used++;                                                      \
        fprintf(stderr, "  [glfb] ATRACE t%llu ctx=%p " fmt "\n",             \
                (unsigned long long)klfb_tid(), klfb_real_ctx_here(),         \
                __VA_ARGS__);                                                 \
    } } while (0)

// ------------------------------------------------------- the SHAR fourth wall
//
// **Simpsons Hit & Run models its interiors as STAGE SETS.** `art/l1i01.p3d`
// (and the level-4/7 twins `l4i01`/`l7i01`) gives the Kwik-E-Mart only THREE
// walls: the mesh `int_kwik_shelfShape07_032` (shader `int_kwik_wall_m`,
// lavender texture, 112 vertices) carries the two sides at x 492.98 and x
// 510.40 and the z -289.68 side with the exit door in it, and NOTHING at all on
// the z -307.13 side. The retail cameras — `int_kwik_camera01`/`10` — sit at
// z -308.5, OUTSIDE the room, and film the interior in through that hole, so on
// a TV the missing wall is never in frame. `InteriorEntryStart` (z -305) drops
// the player right beside it.
//
// In VR the player stands inside the room and can look anywhere, and the
// interior is parked 20 units UNDER the map in empty space — so the fourth side
// is the clear colour with the level floating in the distance. That is the
// "white void" of rounds 24-27, and it was never a lost draw: floor, ceiling,
// props and the three real walls all render, which is exactly what the captures
// showed.
//
// The fix has to be at the GL level because the geometry is a shipped asset.
// The wall mesh is drawn a SECOND time, translated by -17.40 on z, which puts
// its z -289.68 front section at z -307.08 — just inside the floor/ceiling edge
// at -307.13, so there is no seam. The translated SIDE walls run out past the
// new wall and are hidden behind it; the door and the glass are separate meshes
// and are deliberately not copied. The copy's front faces point away from the
// room, so the extra draw runs with culling off and we see its back face; the
// texture is near-uniform lavender, so the mirrored UVs do not read.
//
// Three pieces: glBufferData names the wall VBO by its bounding box and builds
// a private "shadow" VBO holding the same 112 vertices with z shifted; the
// vertex-array setup family remembers which VAOs feed attribute 0 (position,
// float3 at offset 0 of a 36-byte stride) from that VBO; and the draw thunks
// repeat any draw from such a VAO against the shadow. Every framebuffer,
// including the shadow pass — the copy should cast too.
//
// KL_SHAR_FOURTHWALL=0 is the backout, KL_SHAR_FOURTHWALL_DZ moves the copy.
// Default OFF here and set to 1 for the simpsons target alone by kl_sdl.c's
// SHAR door, so nothing changes for any other guest.
enum { KLFB_FW_STRIDE = 36, KLFB_FW_NMESH = 3, KLFB_FW_MAXVAOS = 12, KLFB_FW_PLUG_VERTS = 12 };
#define KLFB_GL_TRIANGLES     0x0004
#define KLFB_GL_UNSIGNED_BYTE 0x1401
// The meshes that make up the closed wall: the wall itself (three sides, with
// a doorway cut out of the front), the exit door that fills the doorway, and
// the small glass pane in it. Boxes measured off l1i01.p3d; x and z identify
// the mesh and are held to a tenth, y is loose because the level-4 and
// level-7 twins sit a few tenths off. Sizes are verts * the fixed 36-byte
// pglPrimBuffer stride, position float3 at offset 0.
typedef struct {
    const char *what;
    int         bytes, verts;
    float       mn[3], mx[3];
    uint32_t    vbo, shadow;    // 0 until matched
} klfb_fw_mesh;
static klfb_fw_mesh g_fw_mesh[KLFB_FW_NMESH] = {
    { "wall",  4032, 112, { 492.98f, -20.01f, -307.13f }, { 510.40f, -16.51f, -289.68f }, 0, 0 },
    { "door",  3096,  86, { 501.38f, -20.01f, -296.63f }, { 507.71f, -17.58f, -289.70f }, 0, 0 },
    { "glass",  288,   8, { 501.38f, -19.41f, -291.50f }, { 505.48f, -18.83f, -289.70f }, 0, 0 },
};
#define KLFB_GL_ARRAY_BUFFER 0x8892
#define KLFB_GL_STATIC_DRAW  0x88E4
#define KLFB_GL_CULL_FACE    0x0B44
#define KLFB_GL_FLOAT_T      0x1406

static int      g_fw_on = -1;
static float    g_fw_dz = 17.40f;

// ANGLE's own entry points, taken straight from the library: everything this
// block issues must miss the thunks above it, or the extra draw would recurse
// through the census and the state tracking would record our own bookkeeping.
static void    (*fw_GenBuffers)(int32_t, uint32_t *);
static void    (*fw_DeleteBuffers)(int32_t, const uint32_t *);
static void    (*fw_BindBuffer)(uint32_t, uint32_t);
static void    (*fw_BufferData)(uint32_t, intptr_t, const void *, uint32_t);
static void    (*fw_VertexAttribPointer)(uint32_t, int32_t, uint32_t, uint8_t,
                                         int32_t, const void *);
static void    (*fw_DrawElements)(uint32_t, int32_t, uint32_t, const void *);
static void    (*fw_DrawArrays)(uint32_t, int32_t, int32_t);
static void    (*fw_Enable)(uint32_t);
static void    (*fw_Disable)(uint32_t);
static uint8_t (*fw_IsEnabled)(uint32_t);

static void klfb_fw_resolve(void) {
    static int done;
    if (done) return;
    done = 1;
    fw_GenBuffers          = asym("glGenBuffers");
    fw_DeleteBuffers       = asym("glDeleteBuffers");
    fw_BindBuffer          = asym("glBindBuffer");
    fw_BufferData          = asym("glBufferData");
    fw_VertexAttribPointer = asym("glVertexAttribPointer");
    fw_DrawElements        = asym("glDrawElements");
    fw_DrawArrays          = asym("glDrawArrays");
    fw_Enable              = asym("glEnable");
    fw_Disable             = asym("glDisable");
    fw_IsEnabled           = asym("glIsEnabled");
}

static int klfb_fw_on(void) {
    if (g_fw_on < 0) {
        g_fw_on = kl_env_on("KL_SHAR_FOURTHWALL", 0);
        g_fw_dz = kl_env_float("KL_SHAR_FOURTHWALL_DZ", 17.40f);
        if (g_fw_on)
            fprintf(stderr, "  [glfb] fourth wall: on (dz %.2f)\n", (double)g_fw_dz);
    }
    return g_fw_on;
}

// Which VAOs feed attribute 0 from the wall VBO. A set rather than a
// VAO->buffer map on purpose: the guest makes one VAO per prim buffer, so a
// general map would need thousands of rows and evict the one row that matters,
// where the set only ever holds the handful that draw the wall. Reads on the
// draw path are one thread-local int; the set itself moves only when the setup
// family runs.
static uint32_t g_fw_vaos[KLFB_FW_MAXVAOS];
static int      g_fw_vao_mesh[KLFB_FW_MAXVAOS];
static int      g_fw_nvaos;
static pthread_mutex_t g_fw_lock = PTHREAD_MUTEX_INITIALIZER;
// Per-thread because both are per-context state and the context is current on
// one thread at a time.
static __thread uint32_t g_fw_vao;        // the bound VAO (0 = none)
static __thread uint32_t g_fw_arraybuf;   // the GL_ARRAY_BUFFER binding
static __thread int      g_fw_hot;        // 1 + mesh index when g_fw_vao is in the set, else 0

static int klfb_fw_vao_hot(uint32_t vao) {
    int hot = 0;
    pthread_mutex_lock(&g_fw_lock);
    for (int i = 0; i < g_fw_nvaos; i++)
        if (g_fw_vaos[i] == vao) { hot = g_fw_vao_mesh[i] + 1; break; }
    pthread_mutex_unlock(&g_fw_lock);
    return hot;
}
// mesh < 0 removes the VAO from the set.
static void klfb_fw_mark_vao(uint32_t vao, int mesh) {
    pthread_mutex_lock(&g_fw_lock);
    int at = -1;
    for (int i = 0; i < g_fw_nvaos; i++) if (g_fw_vaos[i] == vao) { at = i; break; }
    if (mesh >= 0 && at >= 0)
        g_fw_vao_mesh[at] = mesh;
    else if (mesh >= 0 && g_fw_nvaos < KLFB_FW_MAXVAOS) {
        g_fw_vaos[g_fw_nvaos] = vao; g_fw_vao_mesh[g_fw_nvaos++] = mesh;
    } else if (mesh < 0 && at >= 0) {
        --g_fw_nvaos;
        g_fw_vaos[at] = g_fw_vaos[g_fw_nvaos]; g_fw_vao_mesh[at] = g_fw_vao_mesh[g_fw_nvaos];
    }
    pthread_mutex_unlock(&g_fw_lock);
}
static int klfb_fw_mesh_of_vbo(uint32_t vbo) {
    if (!vbo) return -1;
    for (int m = 0; m < KLFB_FW_NMESH; m++) if (g_fw_mesh[m].vbo == vbo) return m;
    return -1;
}

// A matched VBO is gone (deleted, or re-uploaded with different contents), so
// the shadow that mirrors it goes with it — GL recycles buffer names, and a
// match kept past the delete would clone whatever inherits the name next.
static void klfb_fw_forget_mesh(int m) {
    klfb_fw_mesh *M = &g_fw_mesh[m];
    if (M->shadow && fw_DeleteBuffers) {
        uint32_t s = M->shadow;
        fw_DeleteBuffers(1, &s);
    }
    M->shadow = 0;
    M->vbo    = 0;
    pthread_mutex_lock(&g_fw_lock);
    for (int i = 0; i < g_fw_nvaos; ) {
        if (g_fw_vao_mesh[i] == m) {
            --g_fw_nvaos;
            g_fw_vaos[i] = g_fw_vaos[g_fw_nvaos]; g_fw_vao_mesh[i] = g_fw_vao_mesh[g_fw_nvaos];
        } else i++;
    }
    pthread_mutex_unlock(&g_fw_lock);
    if (g_fw_hot == m + 1) g_fw_hot = 0;
}
static void klfb_fw_forget_buffer(uint32_t name) {
    int m = klfb_fw_mesh_of_vbo(name);
    if (m >= 0) klfb_fw_forget_mesh(m);
}

// glBufferData's half: name the wall by its bounding box and stand the shadow
// up beside it. The bbox walk costs 112 iterations and runs only for a
// 4032-byte STATIC_DRAW array upload (pglPrimBuffer pads every vertex to a fixed 36-byte stride; position is float3 at offset 0), which is a handful of buffers a level.
static void klfb_fw_note_bufferdata(uint32_t target, intptr_t size,
                                    const void *data, uint32_t usage) {
    if (!klfb_fw_on() || target != KLFB_GL_ARRAY_BUFFER) return;
    uint32_t cur = g_fw_arraybuf;
    if (!cur) return;
    // A re-upload into a matched buffer invalidates its shadow whatever the
    // new contents are; if they match again the loop below remakes it.
    int old = klfb_fw_mesh_of_vbo(cur);
    if (old >= 0) klfb_fw_forget_mesh(old);
    if (!data || usage != KLFB_GL_STATIC_DRAW) return;

    for (int m = 0; m < KLFB_FW_NMESH; m++) {
        klfb_fw_mesh *M = &g_fw_mesh[m];
        if (M->vbo || size != (intptr_t)M->bytes) continue;

        const unsigned char *p = (const unsigned char *)data;
        float mn[3], mx[3];
        memcpy(mn, p, sizeof mn);
        memcpy(mx, p, sizeof mx);
        for (int i = 1; i < M->verts; i++) {
            float v[3];
            memcpy(v, p + (size_t)i * KLFB_FW_STRIDE, sizeof v);
            for (int c = 0; c < 3; c++) {
                if (v[c] < mn[c]) mn[c] = v[c];
                if (v[c] > mx[c]) mx[c] = v[c];
            }
        }
        static const float tol[3] = { 0.10f, 0.50f, 0.10f };
        int ok = 1;
        for (int c = 0; c < 3 && ok; c++)
            if (fabsf(mn[c] - M->mn[c]) > tol[c] || fabsf(mx[c] - M->mx[c]) > tol[c])
                ok = 0;
        if (!ok) continue;

        // The wall's front plane (z -289.68) has an 8-unit window opening,
        // x 493..501 by y -19.9..-17.6, with no glass mesh behind it in the
        // file -- the void shows through the real wall and the copy alike.
        // Two plug quads (12 verts, same 36-byte layout: pos 0, uv 12,
        // colour 20 -- vertexType 0x2021 carries no normal) ride at the end
        // of the wall's shadow VBO and go out with glDrawArrays after the
        // copy: one in the real wall's plane, one in the copy's.
        size_t plug = (m == 0) ? (size_t)KLFB_FW_PLUG_VERTS * KLFB_FW_STRIDE : 0;
        unsigned char *copy = (unsigned char *)malloc((size_t)M->bytes + plug);
        if (!copy) return;
        memcpy(copy, data, (size_t)M->bytes);
        for (int i = 0; i < M->verts; i++) {
            float z;
            memcpy(&z, copy + (size_t)i * KLFB_FW_STRIDE + 8, sizeof z);
            z -= g_fw_dz;
            memcpy(copy + (size_t)i * KLFB_FW_STRIDE + 8, &z, sizeof z);
        }
        if (plug) {
            // Vertex colour = the wall's own average, so the plug takes the
            // same baked shade as the wall around it.
            unsigned acc[4] = {0, 0, 0, 0};
            for (int i = 0; i < M->verts; i++)
                for (int c = 0; c < 4; c++) acc[c] += p[(size_t)i * KLFB_FW_STRIDE + 20 + c];
            unsigned char col[4];
            for (int c = 0; c < 4; c++) col[c] = (unsigned char)(acc[c] / (unsigned)M->verts);
            static const float qx[6] = { 492.9f, 501.1f, 501.1f, 492.9f, 501.1f, 492.9f };
            static const float qy[6] = { -20.0f, -20.0f, -17.4f, -20.0f, -17.4f, -17.4f };
            unsigned char *q = copy + (size_t)M->bytes;
            for (int k = 0; k < KLFB_FW_PLUG_VERTS; k++, q += KLFB_FW_STRIDE) {
                float pos[3] = { qx[k % 6], qy[k % 6], -289.70f - (k >= 6 ? g_fw_dz : 0.0f) };
                float uv[2]  = { (qx[k % 6] - 493.0f) / 4.0f, (qy[k % 6] + 19.9f) / 2.3f };
                memset(q, 0, KLFB_FW_STRIDE);
                memcpy(q, pos, sizeof pos);
                memcpy(q + 12, uv, sizeof uv);
                memcpy(q + 20, col, sizeof col);
            }
        }
        klfb_fw_resolve();
        uint32_t shadow = 0;
        if (fw_GenBuffers && fw_BindBuffer && fw_BufferData) {
            fw_GenBuffers(1, &shadow);
            if (shadow) {
                fw_BindBuffer(KLFB_GL_ARRAY_BUFFER, shadow);
                fw_BufferData(KLFB_GL_ARRAY_BUFFER, (intptr_t)(M->bytes + plug), copy,
                              KLFB_GL_STATIC_DRAW);
                // We are inside the guest's own glBufferData for `cur`, so that
                // is the binding the caller is entitled to find on the way out.
                fw_BindBuffer(KLFB_GL_ARRAY_BUFFER, cur);
            }
        }
        free(copy);
        if (!shadow) return;
        M->vbo    = cur;
        M->shadow = shadow;
        fprintf(stderr, "  [glfb] fourth wall: %s VBO %u matched (bbox %.2f %.2f %.2f "
                        ".. %.2f %.2f %.2f), shadow VBO %u\n",
                M->what, cur, (double)mn[0], (double)mn[1], (double)mn[2],
                (double)mx[0], (double)mx[1], (double)mx[2], shadow);
        return;
    }
}

// The draw's half. Identical arguments against the shadow, culling off, and
// every piece of state we touched put back the way the guest left it.
static void klfb_fw_extra_elements(uint32_t mode, int32_t count, uint32_t type,
                                   const void *indices) {
    int m = g_fw_hot - 1;
    if (m < 0 || m >= KLFB_FW_NMESH) return;
    klfb_fw_mesh *M = &g_fw_mesh[m];
    if (!M->shadow || !fw_BindBuffer || !fw_VertexAttribPointer || !fw_DrawElements)
        return;
    uint32_t saved = g_fw_arraybuf;
    uint8_t  cull  = fw_IsEnabled ? fw_IsEnabled(KLFB_GL_CULL_FACE) : 1;
    int plug = (m == 0) && fw_DrawArrays;
    fw_BindBuffer(KLFB_GL_ARRAY_BUFFER, M->shadow);
    fw_VertexAttribPointer(0, 3, KLFB_GL_FLOAT_T, 0, KLFB_FW_STRIDE, (const void *)0);
    if (plug) {
        // The plug verts live past the wall's 112, so uv and colour must read
        // from the shadow too (the copy's own verts are identical either way).
        fw_VertexAttribPointer(2, 2, KLFB_GL_FLOAT_T, 0, KLFB_FW_STRIDE, (const void *)12);
        fw_VertexAttribPointer(3, 4, KLFB_GL_UNSIGNED_BYTE, 1, KLFB_FW_STRIDE, (const void *)20);
    }
    if (fw_Disable) fw_Disable(KLFB_GL_CULL_FACE);
    fw_DrawElements(mode, count, type, indices);
    if (plug) fw_DrawArrays(KLFB_GL_TRIANGLES, M->verts, KLFB_FW_PLUG_VERTS);
    fw_BindBuffer(KLFB_GL_ARRAY_BUFFER, M->vbo);
    fw_VertexAttribPointer(0, 3, KLFB_GL_FLOAT_T, 0, KLFB_FW_STRIDE, (const void *)0);
    if (plug) {
        fw_VertexAttribPointer(2, 2, KLFB_GL_FLOAT_T, 0, KLFB_FW_STRIDE, (const void *)12);
        fw_VertexAttribPointer(3, 4, KLFB_GL_UNSIGNED_BYTE, 1, KLFB_FW_STRIDE, (const void *)20);
    }
    if (cull && fw_Enable) fw_Enable(KLFB_GL_CULL_FACE);
    if (saved != M->vbo) fw_BindBuffer(KLFB_GL_ARRAY_BUFFER, saved);
}
#define KLFB_FW_DRAW(mode, count, type, indices)                              \
    do { if (g_fw_hot) klfb_fw_extra_elements((mode), (count), (type), (indices)); } while (0)

static void (*g_real_BindVertexArray)(uint32_t);
static void klfb_BindVertexArray(uint32_t vao) {
    KLFB_ATRACE("glBindVertexArray(%u)", vao);
    g_fw_vao = vao;
    g_fw_hot = klfb_fw_vao_hot(vao);
    if (g_real_BindVertexArray) g_real_BindVertexArray(vao);
}
// The OES spelling, which is what a glad-bound guest calls: SHAR's Pure3D layer
// resolves `glad_glBindVertexArrayOES` (see the GL_OES_vertex_array_object note
// in kl_egl.c), so without this row the fourth wall's VAO tracking would never
// see a bind at all. An alias of the core call inside ANGLE; the thunk adds the
// bookkeeping and forwards.
static void (*g_real_BindVertexArrayOES)(uint32_t);
static void klfb_BindVertexArrayOES(uint32_t vao) {
    KLFB_ATRACE("glBindVertexArrayOES(%u)", vao);
    g_fw_vao = vao;
    g_fw_hot = klfb_fw_vao_hot(vao);
    if (g_real_BindVertexArrayOES) g_real_BindVertexArrayOES(vao);
}
static void (*g_real_VertexAttribPointer)(uint32_t, int32_t, uint32_t, uint8_t,
                                          int32_t, const void *);
static void klfb_VertexAttribPointer(uint32_t idx, int32_t size, uint32_t type,
                                     uint8_t norm, int32_t stride,
                                     const void *ptr) {
    int32_t vao = -1, buf = -1;
    if (klfb_atrace_armed() && a_glGetIntegerv) {
        a_glGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &vao);
        a_glGetIntegerv(0x8894 /* ARRAY_BUFFER_BINDING */, &buf);
    }
    KLFB_ATRACE("glVertexAttribPointer(%u, size=%d type=0x%x stride=%d ptr=%p) "
                "VAO=%d ARRAY_BUFFER=%d", idx, size, type, stride, ptr, vao, buf);
    // The fourth wall's other half: attribute 0 is position, so the VAO this
    // call lands in draws the wall exactly when the wall VBO is the source.
    if (idx == 0 && klfb_radartrace()) {
        // Round 37: attribute 0 is `position`; the radar trace prints the first
        // four of them per draw. Recorded here rather than queried at draw time
        // because a CLIENT array has no GL object to ask about.
        g_radar_a0_ptr    = ptr;
        g_radar_a0_size   = size;
        g_radar_a0_type   = type;
        g_radar_a0_stride = stride;
        g_radar_a0_buf    = g_fw_arraybuf;
    }
    if (idx == 0 && klfb_fw_on()) {
        int mesh = klfb_fw_mesh_of_vbo(g_fw_arraybuf);
        klfb_fw_mark_vao(g_fw_vao, mesh);
        g_fw_hot = mesh >= 0 ? mesh + 1 : 0;
    }
    if (g_real_VertexAttribPointer)
        g_real_VertexAttribPointer(idx, size, type, norm, stride, ptr);
}
static void (*g_real_EnableVertexAttribArray)(uint32_t);
static void klfb_EnableVertexAttribArray(uint32_t idx) {
    int32_t vao = -1;
    if (klfb_atrace_armed() && a_glGetIntegerv) a_glGetIntegerv(0x85B5, &vao);
    KLFB_ATRACE("glEnableVertexAttribArray(%u) VAO=%d", idx, vao);
    if (g_real_EnableVertexAttribArray) g_real_EnableVertexAttribArray(idx);
}
static void (*g_real_DisableVertexAttribArray)(uint32_t);
static void klfb_DisableVertexAttribArray(uint32_t idx) {
    KLFB_ATRACE("glDisableVertexAttribArray(%u)", idx);
    if (g_real_DisableVertexAttribArray) g_real_DisableVertexAttribArray(idx);
}
static void (*g_real_BindBufferGL)(uint32_t, uint32_t);
static void klfb_BindBufferGL(uint32_t target, uint32_t buf) {
    // Off by default even when the trace is on: buffer binds outnumber the
    // attribute calls by three orders of magnitude during asset streaming and
    // ate the whole quota before the first draw. KL_GLFB_ATTRIB_TRACE_BUF=1
    // puts them back.
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_ATTRIB_TRACE_BUF", 0);
    if (on && (target == 0x8892 /* ARRAY_BUFFER */ || target == 0x8893 /* ELEMENT_ARRAY */))
        KLFB_ATRACE("glBindBuffer(%s, %u)",
                    target == 0x8892 ? "ARRAY_BUFFER" : "ELEMENT_ARRAY_BUFFER", buf);
    if (target == 0x8892 /* ARRAY_BUFFER */)  g_fw_arraybuf = buf;
    if (target == 0x8893 /* ELEMENT_ARRAY */) g_radar_elembuf = buf;
    if (g_real_BindBufferGL) g_real_BindBufferGL(target, buf);
}

// glBufferData is thunked for one reason: it is where the SHAR fourth wall's
// missing mesh is recognised. Nothing else about the call changes — the guest's
// upload goes through first, and the match runs against the bytes it just
// handed us.
static void (*g_real_BufferData)(uint32_t, intptr_t, const void *, uint32_t);
static void klfb_BufferData(uint32_t target, intptr_t size, const void *data,
                            uint32_t usage) {
    if (g_real_BufferData) g_real_BufferData(target, size, data, usage);
    klfb_fw_note_bufferdata(target, size, data, usage);
}

// ---- ES 3.1 vertex-attrib-binding, served on an ES 3.0 backend --------------
//
// The gateway DESCRIBES itself as GLES 3.2, and kl_egl.c says why: Unity gates
// renderable float and its whole post stack on the version NUMBER rather than
// on any extension string, so an honest 3.0 costs more than the overstatement
// does. UE4 4.26 reads that same number and, at ES 3.1 or better, sets its
// vertex streams up through the ARB_vertex_attrib_binding family —
// glVertexAttribFormat / glVertexAttribBinding / glBindVertexBuffer — instead
// of glVertexAttribPointer.
//
// ANGLE's Metal backend is an ES 3.0 context. It EXPORTS that family (the
// entry points are in libGLESv2 regardless) and rejects every call with
// GL_INVALID_OPERATION, so none of the state lands. The attributes UE4 then
// enables are enabled with no buffer on their binding and a NULL client
// pointer — which is precisely the `ASSERT(src)` in VertexArrayMtl that
// stopped round 6's first draw, and on a release ANGLE would be a NULL read.
//
// Nothing is lost translating it: format is per-attribute, buffer/offset/
// stride/divisor are per-binding, and glVertexAttribPointer takes all of them
// at once. Keep both halves per (context, VAO) — VAOs are per-context even
// inside a share group — and re-issue a pointer call whenever either half
// moves. KL_GLFB_VAB=0 hands the calls back to ANGLE unchanged.
#define KLFB_VAB_N 32
typedef struct {
    void    *ctx;
    uint32_t vao;
    int      used;
    struct { uint32_t buffer; long offset; int32_t stride; int32_t divisor;
             int set; } bind[KLFB_VAB_N];
    struct { int32_t size; uint32_t type; uint8_t norm; int integer;
             uint32_t reloff; uint32_t binding; int set; } attr[KLFB_VAB_N];
} klfb_vab;
static klfb_vab g_vab[16];
static pthread_mutex_t g_vab_lock = PTHREAD_MUTEX_INITIALIZER;

static void (*g_real_VertexAttribIPointer)(uint32_t, int32_t, uint32_t, int32_t,
                                           const void *);
static void (*g_real_VertexAttribDivisor)(uint32_t, uint32_t);
// The ES 3.1 family itself, kept so KL_GLFB_VAB=0 can hand the calls straight
// back to ANGLE (which rejects them — that is the A/B).
static void (*g_real_VertexAttribFormat)(uint32_t, int32_t, uint32_t, uint8_t, uint32_t);
static void (*g_real_VertexAttribIFormat)(uint32_t, int32_t, uint32_t, uint32_t);
static void (*g_real_VertexAttribBinding)(uint32_t, uint32_t);
static void (*g_real_BindVertexBuffer)(uint32_t, uint32_t, long, int32_t);
static void (*g_real_VertexBindingDivisor)(uint32_t, uint32_t);

static int klfb_vab_on(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_VAB", 1);
    if (on && !g_real_VertexAttribIPointer) {
        // Not thunked (nothing else needs to see them), so taken straight from
        // ANGLE the first time the translation needs them.
        g_real_VertexAttribIPointer = asym("glVertexAttribIPointer");
        g_real_VertexAttribDivisor  = asym("glVertexAttribDivisor");
    }
    return on;
}

// Caller holds g_vab_lock.
static klfb_vab *klfb_vab_here(void) {
    void *ctx = klfb_real_ctx_here();
    int32_t vao = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &vao);
    for (unsigned i = 0; i < sizeof g_vab / sizeof g_vab[0]; i++)
        if (g_vab[i].used && g_vab[i].ctx == ctx && g_vab[i].vao == (uint32_t)vao)
            return &g_vab[i];
    for (unsigned i = 0; i < sizeof g_vab / sizeof g_vab[0]; i++) {
        if (g_vab[i].used) continue;
        memset(&g_vab[i], 0, sizeof g_vab[i]);
        g_vab[i].used = 1; g_vab[i].ctx = ctx; g_vab[i].vao = (uint32_t)vao;
        // GL's own default: attribute i draws from binding i.
        for (unsigned a = 0; a < KLFB_VAB_N; a++) g_vab[i].attr[a].binding = a;
        return &g_vab[i];
    }
    static int said;
    if (!said) { said = 1;
        fprintf(stderr, "  [glfb] more than %zu (context, VAO) pairs use the ES 3.1 "
                        "attrib-binding path — the extras are not translated\n",
                sizeof g_vab / sizeof g_vab[0]); }
    return NULL;
}

// Caller holds g_vab_lock. Re-materialise one attribute as an ES 3.0 pointer
// call. Silent until both halves have been given, because GL's own semantics
// are that neither alone describes a stream.
static void klfb_vab_apply(klfb_vab *v, uint32_t index) {
    if (!v || index >= KLFB_VAB_N) return;
    if (!v->attr[index].set) return;
    uint32_t b = v->attr[index].binding;
    if (b >= KLFB_VAB_N || !v->bind[b].set) return;
    if (!g_real_VertexAttribPointer || !g_real_BindBufferGL) return;

    int32_t saved = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8894 /* ARRAY_BUFFER_BINDING */, &saved);
    g_real_BindBufferGL(0x8892 /* ARRAY_BUFFER */, v->bind[b].buffer);
    const void *p = (const void *)(uintptr_t)(v->bind[b].offset + v->attr[index].reloff);
    if (v->attr[index].integer && g_real_VertexAttribIPointer)
        g_real_VertexAttribIPointer(index, v->attr[index].size, v->attr[index].type,
                                    v->bind[b].stride, p);
    else
        g_real_VertexAttribPointer(index, v->attr[index].size, v->attr[index].type,
                                   v->attr[index].norm, v->bind[b].stride, p);
    if (g_real_VertexAttribDivisor)
        g_real_VertexAttribDivisor(index, (uint32_t)v->bind[b].divisor);
    g_real_BindBufferGL(0x8892, (uint32_t)saved);

    // The one place the two APIs genuinely differ: a VAB stride of 0 means
    // "every vertex reads the same element", while glVertexAttribPointer's 0
    // means "tightly packed". Nothing can express the former in ES 3.0, so say
    // so rather than render it wrong in silence.
    if (v->bind[b].stride == 0 && v->bind[b].buffer) {
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "  [glfb] VAB: binding %u has stride 0, which ES 3.1 reads "
                            "as one element for every vertex and ES 3.0 reads as "
                            "tightly packed — this attribute may be wrong\n", b); }
    }
}

static void klfb_VertexAttribFormat(uint32_t index, int32_t size, uint32_t type,
                                    uint8_t norm, uint32_t reloff) {
    KLFB_ATRACE("glVertexAttribFormat(%u, size=%d type=0x%x norm=%d reloff=%u)",
                index, size, type, norm, reloff);
    if (!klfb_vab_on() || index >= KLFB_VAB_N) {
        if (g_real_VertexAttribFormat)
            g_real_VertexAttribFormat(index, size, type, norm, reloff);
        return;
    }
    pthread_mutex_lock(&g_vab_lock);
    klfb_vab *v = klfb_vab_here();
    if (v) {
        v->attr[index].size = size; v->attr[index].type = type;
        v->attr[index].norm = norm; v->attr[index].integer = 0;
        v->attr[index].reloff = reloff; v->attr[index].set = 1;
        klfb_vab_apply(v, index);
    }
    pthread_mutex_unlock(&g_vab_lock);
}
static void klfb_VertexAttribIFormat(uint32_t index, int32_t size, uint32_t type,
                                     uint32_t reloff) {
    KLFB_ATRACE("glVertexAttribIFormat(%u, size=%d type=0x%x reloff=%u)",
                index, size, type, reloff);
    if (!klfb_vab_on() || index >= KLFB_VAB_N) {
        if (g_real_VertexAttribIFormat)
            g_real_VertexAttribIFormat(index, size, type, reloff);
        return;
    }
    pthread_mutex_lock(&g_vab_lock);
    klfb_vab *v = klfb_vab_here();
    if (v) {
        v->attr[index].size = size; v->attr[index].type = type;
        v->attr[index].norm = 0; v->attr[index].integer = 1;
        v->attr[index].reloff = reloff; v->attr[index].set = 1;
        klfb_vab_apply(v, index);
    }
    pthread_mutex_unlock(&g_vab_lock);
}
static void klfb_VertexAttribBinding(uint32_t index, uint32_t binding) {
    KLFB_ATRACE("glVertexAttribBinding(%u, %u)", index, binding);
    if (!klfb_vab_on() || index >= KLFB_VAB_N || binding >= KLFB_VAB_N) {
        if (g_real_VertexAttribBinding) g_real_VertexAttribBinding(index, binding);
        return;
    }
    pthread_mutex_lock(&g_vab_lock);
    klfb_vab *v = klfb_vab_here();
    if (v) { v->attr[index].binding = binding; klfb_vab_apply(v, index); }
    pthread_mutex_unlock(&g_vab_lock);
}
static void klfb_BindVertexBuffer(uint32_t binding, uint32_t buffer,
                                  long offset, int32_t stride) {
    KLFB_ATRACE("glBindVertexBuffer(%u, buffer=%u offset=%ld stride=%d)",
                binding, buffer, offset, stride);
    if (!klfb_vab_on() || binding >= KLFB_VAB_N) {
        if (g_real_BindVertexBuffer)
            g_real_BindVertexBuffer(binding, buffer, offset, stride);
        return;
    }
    pthread_mutex_lock(&g_vab_lock);
    klfb_vab *v = klfb_vab_here();
    if (v) {
        v->bind[binding].buffer = buffer; v->bind[binding].offset = offset;
        v->bind[binding].stride = stride; v->bind[binding].set = 1;
        for (uint32_t a = 0; a < KLFB_VAB_N; a++)
            if (v->attr[a].set && v->attr[a].binding == binding)
                klfb_vab_apply(v, a);
    }
    pthread_mutex_unlock(&g_vab_lock);
}
static void klfb_VertexBindingDivisor(uint32_t binding, uint32_t divisor) {
    KLFB_ATRACE("glVertexBindingDivisor(%u, %u)", binding, divisor);
    if (!klfb_vab_on() || binding >= KLFB_VAB_N) {
        if (g_real_VertexBindingDivisor) g_real_VertexBindingDivisor(binding, divisor);
        return;
    }
    pthread_mutex_lock(&g_vab_lock);
    klfb_vab *v = klfb_vab_here();
    if (v) {
        v->bind[binding].divisor = (int32_t)divisor;
        v->bind[binding].set = 1;
        for (uint32_t a = 0; a < KLFB_VAB_N; a++)
            if (v->attr[a].set && v->attr[a].binding == binding)
                klfb_vab_apply(v, a);
    }
    pthread_mutex_unlock(&g_vab_lock);
}

static void klfb_DrawElements(uint32_t mode, int32_t count, uint32_t type,
                              const void *indices) {
    KLFB_LAZY_FLUSH();
    // A pending capture first: it runs on THIS thread, which is the one that
    // draws, so it reads the pbuffer the frame actually landed in — not the
    // swap thread's, which is empty by construction.
    klfb_service_capture();
    // What the radar trace's position dump is allowed to read. Free, and the
    // difference between a bounded read and an over-read off the end of a
    // client array — see klfb_radar_note_positions.
    g_radar_draw_first   = 0;
    g_radar_draw_verts   = count;
    g_radar_draw_idx     = indices;
    g_radar_draw_idxtype = type;
    klfb_note_draw();
    klfb_attrib_dump("glDrawElements");
    klfb_errprobe0("glDrawElements(before)");
    if (g_real_DrawElements) g_real_DrawElements(mode, count, type, indices);
    KLFB_FW_DRAW(mode, count, type, indices);
    klfb_draw_shape_elements(indices, type, 0);
    klfb_draw_probe(count);
    char d[64];
    snprintf(d, sizeof d, " mode=0x%x count=%d type=0x%x", mode, count, type);
    klfb_errprobe("glDrawElements(after)", d);
}
static void klfb_DrawArrays(uint32_t mode, int32_t first, int32_t count) {
    KLFB_LAZY_FLUSH();
    klfb_service_capture();
    g_radar_draw_first   = first;
    g_radar_draw_verts   = count;
    g_radar_draw_idx     = NULL;
    g_radar_draw_idxtype = 0;
    klfb_note_draw();
    klfb_attrib_dump("glDrawArrays");
    klfb_errprobe0("glDrawArrays(before)");
    if (g_real_DrawArrays) g_real_DrawArrays(mode, first, count);
    klfb_draw_shape_arrays(first);
    klfb_draw_probe(count);
    klfb_errprobe0("glDrawArrays(after)");
}
// The instanced/basevertex variants: Unity's actual scene geometry goes
// through these, not the plain two — the first census wrapped only those and
// was blind to the scene entirely.
static void klfb_DrawElementsInstanced(uint32_t mode, int32_t count, uint32_t type,
                                       const void *indices, int32_t instances) {
    KLFB_LAZY_FLUSH();
    g_radar_draw_verts = 0;   // extent not tracked here; see klfb_radar_note_positions
    klfb_note_draw();
    if (g_real_DrawElementsInstanced)
        g_real_DrawElementsInstanced(mode, count, type, indices, instances);
    klfb_draw_shape_elements(indices, type, 0);
    klfb_draw_probe(count * instances);
}
// glDrawElementsBaseVertex is core in GLES 3.2 but NOT in ES 3.0 — and ES 3.0
// is what ANGLE's Metal backend gives us, while kl_egl describes 3.2, so Unity
// calls it freely. ANGLE *resolves* the entry point and then rejects every call
// at validation with INVALID_OPERATION, so the draw silently does not happen.
// That is what ate the UI text: Unity's canvas/TMP batching draws sub-ranges of
// one shared mesh buffer, and a sub-range is exactly what basevertex expresses,
// so text draws failed while ordinary geometry (plain glDrawElements) rendered.
// Same version-gap class as the Bloom fix, one API family over.
//
// basevertex == 0 is definitionally plain glDrawElements, which ES 3.0 does
// have; otherwise prefer an EXT/OES entry point if ANGLE exposes one. Anything
// left over falls through to the core call and is named once, rather than
// silently dropping the draw the way the unconditional forward did.
static void (*g_real_DrawElementsBaseVertexEXT)(uint32_t, int32_t, uint32_t,
                                                const void *, int32_t);
static void klfb_DrawElementsBaseVertex(uint32_t mode, int32_t count, uint32_t type,
                                        const void *indices, int32_t basevertex) {
    KLFB_LAZY_FLUSH();
    g_radar_draw_verts = 0;   // extent not tracked here; see klfb_radar_note_positions
    klfb_note_draw();
    static int resolved;
    if (!resolved) {
        resolved = 1;
        g_real_DrawElementsBaseVertexEXT = asym("glDrawElementsBaseVertexEXT");
        if (!g_real_DrawElementsBaseVertexEXT)
            g_real_DrawElementsBaseVertexEXT = asym("glDrawElementsBaseVertexOES");
        fprintf(stderr, "  [glfb] glDrawElementsBaseVertex: core call is invalid on "
                "ANGLE's ES 3.0; EXT/OES entry point %s\n",
                g_real_DrawElementsBaseVertexEXT ? "available" : "ABSENT "
                "(basevertex!=0 draws will still fail)");
    }
    if (basevertex == 0 && g_real_DrawElements) {
        g_real_DrawElements(mode, count, type, indices);
    } else if (g_real_DrawElementsBaseVertexEXT) {
        g_real_DrawElementsBaseVertexEXT(mode, count, type, indices, basevertex);
    } else {
        static int said;
        if (said++ < 5)
            fprintf(stderr, "  [glfb] glDrawElementsBaseVertex(basevertex=%d) has no "
                    "ES 3.0 route — draw dropped by ANGLE\n", basevertex);
        if (g_real_DrawElementsBaseVertex)
            g_real_DrawElementsBaseVertex(mode, count, type, indices, basevertex);
    }
    klfb_draw_shape_elements(indices, type, basevertex);
    klfb_draw_probe(count);
}
static void klfb_DrawArraysInstanced(uint32_t mode, int32_t first, int32_t count,
                                     int32_t instances) {
    KLFB_LAZY_FLUSH();
    g_radar_draw_verts = 0;   // extent not tracked here; see klfb_radar_note_positions
    klfb_note_draw();
    if (g_real_DrawArraysInstanced)
        g_real_DrawArraysInstanced(mode, first, count, instances);
    klfb_draw_shape_arrays(first);
    klfb_draw_probe(count * instances);
}

// glDrawRangeElements is srceng/togl's ordinary scene draw (the DX9-shaped
// renderer prefers it everywhere), and it rode the raw passthrough — which is
// how a run with 28,960 counted blits and 5,590 swaps reported "no draws seen
// at all". Wrapped for the same reason the instanced variants were: the
// census cannot describe what it cannot see.
static void (*g_real_DrawRangeElements)(uint32_t, uint32_t, uint32_t, int32_t,
                                        uint32_t, const void *);
static void klfb_DrawRangeElements(uint32_t mode, uint32_t start, uint32_t end,
                                   int32_t count, uint32_t type,
                                   const void *indices) {
    KLFB_LAZY_FLUSH();
    g_radar_draw_verts = 0;   // extent not tracked here; see klfb_radar_note_positions
    klfb_service_capture();
    klfb_note_draw();
    if (g_real_DrawRangeElements)
        g_real_DrawRangeElements(mode, start, end, count, type, indices);
    // The copy goes out as a plain glDrawElements: start/end are a hint about
    // the index range, not part of what is drawn.
    KLFB_FW_DRAW(mode, count, type, indices);
    klfb_draw_probe(count);
}
// ...and its basevertex twin, core only in ES 3.2 — same gap as
// glDrawElementsBaseVertex above, and until round 12 NOT the same translation.
//
// **This is the faces/eyes/neck bug.** srceng's togl funnels EVERY indexed draw
// through this one entry point (togl/linuxwin/dxabstract.cpp:5395 ->
// glmgr.cpp:5187), passing D3D's BaseVertexIndex straight through. The wrapper
// above preferred `g_real_DrawRangeElementsBaseVertex` — ANGLE's *core ES 3.2*
// entry point — whenever basevertex != 0. On our ES 3.0 context that function
// never draws anything:
//
//   vendor/src/libGLESv2/entry_points_gles_3_2_autogen.cpp, GL_DrawRangeElementsBaseVertex:
//       if (ANGLE_LIKELY(context->getClientVersion() >= ES_3_2)) { ...validate... }
//       else { RecordVersionErrorES32(context, ...); }        // isCallValid stays false
//
// so the call records INVALID_OPERATION and returns without reaching
// context->drawRangeElementsBaseVertex. The EXT branch below it was dead code:
// g_real_DrawRangeElementsBaseVertex is filled in from the shim table and is
// never NULL, so the `else if` after it could not be reached.
//
// basevertex == 0 is definitionally plain glDrawRangeElements (valid in ES 3.0),
// which is why the world, the props and the HUD all render: those are static
// vertex buffers and their BaseVertexIndex is 0. basevertex != 0 is what a
// DYNAMIC vertex buffer looks like — the mesh starts partway into a shared
// dynamic VB — and in studiorender that is exactly three things:
//
//   * flexed faces   (r_studiodraw.cpp:2449 GetDynamicMeshEx, when r_flex 1)
//   * eyeballs       (R_StudioDrawEyeball, r_studiodraw.cpp:2657 GetDynamicMesh —
//                     taken whenever the eyeball mesh is flexed and the driver
//                     has no stream offset, REGARDLESS of r_flex)
//   * any mesh group that lost MESHGROUP_IS_HWSKINNED and falls to swSkin
//
// which is the exact symptom set: r_flex 1 loses the whole face, r_flex 0 gets
// the face back but the eyes and the neck stay missing.
//
// ANGLE's Metal backend DOES have the extension — DisplayMtl.mm:1098 sets
// drawElementsBaseVertexEXT/OES from hasBaseVertexInstancedDraw, and the
// framework exports glDrawRangeElementsBaseVertexEXT/OES — so preferring the
// EXT/OES range form is a real draw instead of a validation error.
//
// KL_GLFB_BASEVERTEX_CORE=1 restores the old (broken) preference for the A/B.
static void (*g_real_DrawRangeElementsBaseVertex)(uint32_t, uint32_t, uint32_t,
                                                  int32_t, uint32_t, const void *,
                                                  int32_t);
static void (*g_real_DrawRangeElementsBaseVertexEXT)(uint32_t, uint32_t, uint32_t,
                                                     int32_t, uint32_t, const void *,
                                                     int32_t);
static unsigned long g_bv_zero, g_bv_nonzero;   // census: basevertex == 0 vs != 0
static const char *g_bv_route = "(no basevertex draw yet)";
static uint32_t g_bv_first_err;                 // GL error after the first real one
static int g_bv_err_taken;

static void klfb_bv_resolve(void) {
    static int resolved;
    if (resolved) return;
    resolved = 1;
    g_real_DrawRangeElementsBaseVertexEXT = asym("glDrawRangeElementsBaseVertexEXT");
    if (!g_real_DrawRangeElementsBaseVertexEXT)
        g_real_DrawRangeElementsBaseVertexEXT = asym("glDrawRangeElementsBaseVertexOES");
    if (!g_real_DrawElementsBaseVertexEXT) {
        // The lazy resolution in klfb_DrawElementsBaseVertex only runs when
        // THAT wrapper is called; togl uses this form instead.
        g_real_DrawElementsBaseVertexEXT = asym("glDrawElementsBaseVertexEXT");
        if (!g_real_DrawElementsBaseVertexEXT)
            g_real_DrawElementsBaseVertexEXT = asym("glDrawElementsBaseVertexOES");
    }
    if (kl_env_on("KL_GLFB_BASEVERTEX_CORE", 0)) {
        g_bv_route = "CORE ES 3.2 (KL_GLFB_BASEVERTEX_CORE=1 — expect every "
                     "basevertex!=0 draw to be dropped)";
    } else if (g_real_DrawRangeElementsBaseVertexEXT) {
        g_bv_route = "glDrawRangeElementsBaseVertex EXT/OES";
    } else if (g_real_DrawElementsBaseVertexEXT) {
        g_bv_route = "glDrawElementsBaseVertex EXT/OES (no range form)";
    } else {
        g_bv_route = "CORE ES 3.2 — no EXT/OES route on this driver, "
                     "basevertex!=0 draws WILL be dropped";
    }
    fprintf(stderr, "  [glfb] basevertex draws route: %s (rangeEXT=%s eltEXT=%s)\n",
            g_bv_route,
            g_real_DrawRangeElementsBaseVertexEXT ? "yes" : "no",
            g_real_DrawElementsBaseVertexEXT ? "yes" : "no");
}

// The basevertex census. srceng routes every indexed draw through
// glDrawRangeElementsBaseVertex, so this splits the frame into "static vertex
// buffer" (basevertex 0) and "dynamic vertex buffer" (basevertex != 0) — and
// the second bucket is the flexed faces, the eyeballs and the software-skinned
// mesh groups. A run with citizens in it and basevertex!=0 == 0 means the guest
// never asked; a nonzero count with first error 0x502 means we answered with a
// validation error instead of a draw.
static void klfb_report_basevertex(void) {
    if (!g_bv_zero && !g_bv_nonzero) return;
    fprintf(stderr, "  [glfb] basevertex draws: %lu with basevertex==0, "
            "%lu with basevertex!=0 (route: %s; first error 0x%x)\n",
            g_bv_zero, g_bv_nonzero, g_bv_route, g_bv_first_err);
}

static void klfb_DrawRangeElementsBaseVertex(uint32_t mode, uint32_t start,
                                             uint32_t end, int32_t count,
                                             uint32_t type, const void *indices,
                                             int32_t basevertex) {
    KLFB_LAZY_FLUSH();
    klfb_note_draw();
    klfb_bv_resolve();
    if (basevertex == 0 && g_real_DrawRangeElements) {
        g_bv_zero++;
        g_real_DrawRangeElements(mode, start, end, count, type, indices);
    } else {
        int core = kl_env_on("KL_GLFB_BASEVERTEX_CORE", 0);
        g_bv_nonzero++;
        if (!core && g_real_DrawRangeElementsBaseVertexEXT) {
            g_real_DrawRangeElementsBaseVertexEXT(mode, start, end, count, type,
                                                  indices, basevertex);
        } else if (!core && g_real_DrawElementsBaseVertexEXT) {
            // No range form on this driver: the range is a hint GL may ignore,
            // so the elements form is the whole draw.
            g_real_DrawElementsBaseVertexEXT(mode, count, type, indices, basevertex);
        } else if (g_real_DrawRangeElementsBaseVertex) {
            g_real_DrawRangeElementsBaseVertex(mode, start, end, count, type,
                                               indices, basevertex);
        }
        // One error read, on the first basevertex!=0 draw of the run. A
        // glGetError per draw would cost a flush; one is free and it is the
        // whole difference between "the route works" and "ANGLE refused it".
        if (!g_bv_err_taken && a_glGetError) {
            g_bv_err_taken = 1;
            g_bv_first_err = a_glGetError();
            fprintf(stderr, "  [glfb] first basevertex!=0 draw (basevertex=%d "
                    "count=%d): GL error 0x%x via %s\n",
                    basevertex, count, g_bv_first_err, g_bv_route);
        }
        // klfb_report_draws() only runs from the PNG capture path, which round
        // 9 took out of the bake — so the census says itself, on a stride, or
        // a device log would never carry it.
        if ((g_bv_nonzero % 20000) == 0) klfb_report_basevertex();
    }
    klfb_draw_probe(count);
}

// The pin table, out loud. A table that never fires and one that re-points half
// the guest's uniforms are indistinguishable from the picture, and the second
// one cost Beat Saber its menu UI for nine commits.
static void klfb_report_pins(void) {
    if (!g_prog_pins_n) return;
    fprintf(stderr, "  [glfb] uniform pins: %u program(s) pinned, %u of them resolve "
                    "by name (pins not honoured there); %u remap%s fired, %u changed "
                    "a location\n",
            g_prog_pins_n, g_pin_byname_progs, g_pin_remap_hits,
            g_pin_remap_hits == 1 ? "" : "s", g_pin_remap_changed);
}

static void klfb_report_draws(void) {
    klfb_report_pins();
    klfb_report_basevertex();
    if (!g_ndraw_fbs) { fprintf(stderr, "  [glfb] no draws seen at all\n"); return; }
    kl_glfb_scissor_census(stderr);
    fprintf(stderr, "  [glfb] draws by target framebuffer:");
    for (unsigned i = 0; i < g_ndraw_fbs; i++)
        fprintf(stderr, " fb%d=%u(t%llu)", g_draw_fbs[i].fb, g_draw_fbs[i].draws,
                (unsigned long long)g_draw_fbs[i].tid);
    fprintf(stderr, "\n");
}

// ---------------------- ES 3.1 program-interface queries, on an ES 3.0 driver
//
// We describe GLES 3.2 (kl_egl.c), so Unity 2019.4's program setup enumerates
// uniforms and uniform blocks through the ES 3.1 program-interface family —
// glGetProgramInterfaceiv / glGetProgramResourceiv / glGetProgramResourceName /
// glGetProgramResourceIndex / glGetProgramResourceLocation. ANGLE's context is
// ES 3.0 and *rejects the whole family*: RecordVersionErrorES31 sets a
// validation error, the out-params stay unwritten, and in the debug build each
// call also pays a gl::Trace log line. Sampled in the act: 1925 of 3698 samples
// of a guest worker inside glGetProgramResourceiv error logging — the guest
// spinning on a garbage resource count while the main thread futex-waits on
// it. That was the "IL2CPP abort" end state the Bloom fallback walked into.
//
// Everything this family can ask about uniforms, uniform blocks and vertex
// inputs is ES 3.0-queryable (glGetActiveUniform{s,}iv, glGetActiveUniformBlock*,
// glGetActiveAttrib), so translate. The ES 3.1 GL_UNIFORM resource index IS the
// ES 3.0 active-uniform index; same for GL_UNIFORM_BLOCK. Interfaces with no
// ES 3.0 source (program outputs, SSBOs, atomics) answer empty/invalid and log
// once per shape — if Unity ever needs one, the log says which.

#define KLFB_IF_UNIFORM        0x92E1
#define KLFB_IF_UNIFORM_BLOCK  0x92E2
#define KLFB_IF_PROGRAM_INPUT  0x92E3
#define KLFB_IF_PROGRAM_OUTPUT 0x92E4
#define KLFB_IF_BUFFER_VARIABLE 0x92E5
#define KLFB_IF_SHADER_STORAGE_BLOCK 0x92E6
#define KLFB_IF_TRANSFORM_FEEDBACK_VARYING 0x92F4
#define KLFB_ACTIVE_RESOURCES  0x92F5
#define KLFB_MAX_NAME_LENGTH   0x92F6
#define KLFB_MAX_NUM_ACTIVE_VARIABLES 0x92F7
// resource props (gl31.h)
#define KLFB_P_NAME_LENGTH     0x92F9
#define KLFB_P_TYPE            0x92FA
#define KLFB_P_ARRAY_SIZE      0x92FB
#define KLFB_P_OFFSET          0x92FC
#define KLFB_P_BLOCK_INDEX     0x92FD
#define KLFB_P_ARRAY_STRIDE    0x92FE
#define KLFB_P_MATRIX_STRIDE   0x92FF
#define KLFB_P_IS_ROW_MAJOR    0x9300
#define KLFB_P_ATOMIC_COUNTER_BUFFER_INDEX 0x9301
#define KLFB_P_BUFFER_BINDING  0x9302
#define KLFB_P_BUFFER_DATA_SIZE 0x9303
#define KLFB_P_NUM_ACTIVE_VARIABLES 0x9304
#define KLFB_P_ACTIVE_VARIABLES 0x9305
#define KLFB_P_REFERENCED_BY_VERTEX   0x9306
#define KLFB_P_REFERENCED_BY_FRAGMENT 0x930A
#define KLFB_P_LOCATION        0x930E
// ES 3.0 introspection (gl3.h)
#define KLFB_ACTIVE_UNIFORMS        0x8B86
#define KLFB_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87
#define KLFB_ACTIVE_ATTRIBUTES      0x8B89
#define KLFB_ACTIVE_ATTRIBUTE_MAX_LENGTH 0x8B8A
#define KLFB_ACTIVE_UNIFORM_BLOCKS  0x8A36
#define KLFB_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH 0x8A35
#define KLFB_U_TYPE            0x8A37
#define KLFB_U_SIZE            0x8A38
#define KLFB_U_NAME_LENGTH     0x8A39
#define KLFB_U_BLOCK_INDEX     0x8A3A
#define KLFB_U_OFFSET          0x8A3B
#define KLFB_U_ARRAY_STRIDE    0x8A3C
#define KLFB_U_MATRIX_STRIDE   0x8A3D
#define KLFB_U_IS_ROW_MAJOR    0x8A3E
#define KLFB_UB_BINDING        0x8A3F
#define KLFB_UB_DATA_SIZE      0x8A40
#define KLFB_UB_NAME_LENGTH    0x8A41
#define KLFB_UB_ACTIVE_UNIFORMS 0x8A42
#define KLFB_UB_ACTIVE_UNIFORM_INDICES 0x8A43
#define KLFB_UB_REF_BY_VERTEX   0x8A44
#define KLFB_UB_REF_BY_FRAGMENT 0x8A46
#define KLFB_INVALID_INDEX     0xFFFFFFFFu

static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
static void (*r_GetActiveUniform)(uint32_t, uint32_t, int32_t, int32_t *,
                                  int32_t *, uint32_t *, char *);
static int32_t (*r_GetUniformLocation)(uint32_t, const char *);
static void (*r_GetUniformIndices)(uint32_t, int32_t, const char *const *,
                                   uint32_t *);
static void (*r_GetActiveUniformsiv)(uint32_t, int32_t, const uint32_t *,
                                     uint32_t, int32_t *);
static void (*r_GetActiveUniformBlockiv)(uint32_t, uint32_t, uint32_t, int32_t *);
static void (*r_GetActiveUniformBlockName)(uint32_t, uint32_t, int32_t,
                                           int32_t *, char *);
static uint32_t (*r_GetUniformBlockIndex)(uint32_t, const char *);
static void (*r_GetActiveAttrib)(uint32_t, uint32_t, int32_t, int32_t *,
                                 int32_t *, uint32_t *, char *);

static void klfb_res_resolve(void) {
    if (r_GetProgramiv) return;
    r_GetProgramiv   = (void *)asym("glGetProgramiv");
    r_GetActiveUniform = (void *)asym("glGetActiveUniform");
    r_GetUniformLocation = (void *)asym("glGetUniformLocation");
    r_GetUniformIndices  = (void *)asym("glGetUniformIndices");
    r_GetActiveUniformsiv = (void *)asym("glGetActiveUniformsiv");
    r_GetActiveUniformBlockiv = (void *)asym("glGetActiveUniformBlockiv");
    r_GetActiveUniformBlockName = (void *)asym("glGetActiveUniformBlockName");
    r_GetUniformBlockIndex = (void *)asym("glGetUniformBlockIndex");
    r_GetActiveAttrib = (void *)asym("glGetActiveAttrib");
}

// The g_thunks real-slot for the program-interface family: the table wires
// *real = fn for every entry, but these thunks self-resolve through asym
// (klfb_res_resolve), so their slot points here and is never read.
static void *g_res_sink;

// Scout data, not spam: each unhandled (interface, prop) shape logs once.
static struct { uint32_t iface, prop; } g_res_unhandled[32];
static unsigned g_res_nunhandled;

static void klfb_res_log_unhandled(const char *what, uint32_t iface, uint32_t prop) {
    for (unsigned i = 0; i < g_res_nunhandled; i++)
        if (g_res_unhandled[i].iface == iface && g_res_unhandled[i].prop == prop)
            return;
    if (g_res_nunhandled < 32) {
        g_res_unhandled[g_res_nunhandled].iface = iface;
        g_res_unhandled[g_res_nunhandled].prop  = prop;
        g_res_nunhandled++;
    }
    fprintf(stderr, "  [glfb] %s: unhandled interface 0x%x prop 0x%x "
                    "(answered empty/invalid)\n", what, iface, prop);
}

static void klfb_err_say(const char *what, uint32_t a0);   // with the thunk table

static void klfb_GetProgramInterfaceiv(uint32_t program, uint32_t iface,
                                       uint32_t pname, int32_t *params) {
    klfb_res_resolve();
    // Unity's "Invalid texture unit!" lands immediately after this call in the
    // trace: its own GL-error check misattributes whatever we raise here. A
    // drain/report bracket names the query that actually sets the error.
    if (a_glGetError) while (a_glGetError()) {}
    if (!params) return;
    *params = 0;
    uint32_t src = 0;
    if (pname == KLFB_ACTIVE_RESOURCES) {
        switch (iface) {
        case KLFB_IF_UNIFORM:        src = KLFB_ACTIVE_UNIFORMS; break;
        case KLFB_IF_UNIFORM_BLOCK:  src = KLFB_ACTIVE_UNIFORM_BLOCKS; break;
        case KLFB_IF_PROGRAM_INPUT:  src = KLFB_ACTIVE_ATTRIBUTES; break;
        default:
            klfb_res_log_unhandled("glGetProgramInterfaceiv", iface, pname);
            return;
        }
    } else if (pname == KLFB_MAX_NAME_LENGTH) {
        switch (iface) {
        case KLFB_IF_UNIFORM:        src = KLFB_ACTIVE_UNIFORM_MAX_LENGTH; break;
        case KLFB_IF_UNIFORM_BLOCK:  src = KLFB_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH; break;
        case KLFB_IF_PROGRAM_INPUT:  src = KLFB_ACTIVE_ATTRIBUTE_MAX_LENGTH; break;
        default:
            klfb_res_log_unhandled("glGetProgramInterfaceiv", iface, pname);
            return;
        }
    } else if (pname == KLFB_MAX_NUM_ACTIVE_VARIABLES && iface == KLFB_IF_UNIFORM_BLOCK) {
        // No single ES 3.0 query: the max over blocks of their active-uniform
        // counts. Cold path, so just walk them.
        int32_t nb = 0, best = 0;
        if (r_GetProgramiv) r_GetProgramiv(program, KLFB_ACTIVE_UNIFORM_BLOCKS, &nb);
        for (int32_t i = 0; i < nb; i++) {
            int32_t n = 0;
            if (r_GetActiveUniformBlockiv)
                r_GetActiveUniformBlockiv(program, (uint32_t)i, KLFB_UB_ACTIVE_UNIFORMS, &n);
            if (n > best) best = n;
        }
        *params = best;
        return;
    } else {
        klfb_res_log_unhandled("glGetProgramInterfaceiv", iface, pname);
        return;
    }
    if (r_GetProgramiv) r_GetProgramiv(program, src, params);
    klfb_err_say("GetProgramInterfaceiv/GetProgramiv", src);
}

// One scalar prop of one GL_UNIFORM resource, via the ES 3.0 uniform queries.
// Returns 1 when *out was written.
static int klfb_uniform_prop(uint32_t program, uint32_t index, uint32_t prop,
                             int32_t *out) {
    uint32_t es30 = 0;
    switch (prop) {
    case KLFB_P_NAME_LENGTH:   es30 = KLFB_U_NAME_LENGTH; break;
    case KLFB_P_TYPE:          es30 = KLFB_U_TYPE; break;
    case KLFB_P_ARRAY_SIZE:    es30 = KLFB_U_SIZE; break;
    case KLFB_P_OFFSET:        es30 = KLFB_U_OFFSET; break;
    case KLFB_P_BLOCK_INDEX:   es30 = KLFB_U_BLOCK_INDEX; break;
    case KLFB_P_ARRAY_STRIDE:  es30 = KLFB_U_ARRAY_STRIDE; break;
    case KLFB_P_MATRIX_STRIDE: es30 = KLFB_U_MATRIX_STRIDE; break;
    case KLFB_P_IS_ROW_MAJOR:  es30 = KLFB_U_IS_ROW_MAJOR; break;
    case KLFB_P_REFERENCED_BY_VERTEX:
    case KLFB_P_REFERENCED_BY_FRAGMENT:
        // glGetActiveUniformsiv has no per-stage referenced flag for uniforms
        // (only for blocks). "Referenced" is the safe answer: it keeps Unity
        // from stripping a live uniform.
        *out = 1;
        return 1;
    case KLFB_P_LOCATION: {
        // By name: ES 3.0 has no location query by index.
        char name[256];
        int32_t len = 0, size = 0;
        uint32_t type = 0;
        if (!r_GetActiveUniform || !r_GetUniformLocation) return 0;
        r_GetActiveUniform(program, index, sizeof name, &len, &size, &type, name);
        name[sizeof name - 1] = 0;
        *out = r_GetUniformLocation(program, name);
        return 1;
    }
    default:
        klfb_res_log_unhandled("glGetProgramResourceiv", KLFB_IF_UNIFORM, prop);
        *out = 0;
        return 1;
    }
    if (!r_GetActiveUniformsiv) return 0;
    r_GetActiveUniformsiv(program, 1, &index, es30, out);
    return 1;
}

static void klfb_GetProgramResourceiv(uint32_t program, uint32_t iface,
                                      uint32_t index, int32_t propCount,
                                      const uint32_t *props, int32_t bufSize,
                                      int32_t *length, int32_t *params) {
    klfb_res_resolve();
    int32_t want = propCount < bufSize ? propCount : bufSize;
    if (length) *length = 0;
    if (!props || !params || want <= 0) return;
    int32_t written = 0;
    for (int32_t i = 0; i < want; i++) {
        int32_t v = 0;
        int ok = 0;
        if (iface == KLFB_IF_UNIFORM) {
            ok = klfb_uniform_prop(program, index, props[i], &v);
        } else if (iface == KLFB_IF_UNIFORM_BLOCK) {
            uint32_t es30 = 0;
            switch (props[i]) {
            case KLFB_P_NAME_LENGTH:   es30 = KLFB_UB_NAME_LENGTH; break;
            case KLFB_P_BUFFER_DATA_SIZE: es30 = KLFB_UB_DATA_SIZE; break;
            case KLFB_P_NUM_ACTIVE_VARIABLES: es30 = KLFB_UB_ACTIVE_UNIFORMS; break;
            case KLFB_P_BUFFER_BINDING: es30 = KLFB_UB_BINDING; break;
            case KLFB_P_REFERENCED_BY_VERTEX:   es30 = KLFB_UB_REF_BY_VERTEX; break;
            case KLFB_P_REFERENCED_BY_FRAGMENT: es30 = KLFB_UB_REF_BY_FRAGMENT; break;
            case KLFB_P_ACTIVE_VARIABLES: {
                // Multi-value prop: fills the rest of params with the member
                // uniform indices, spec-style.
                int32_t n = 0;
                if (r_GetActiveUniformBlockiv)
                    r_GetActiveUniformBlockiv(program, index,
                                              KLFB_UB_ACTIVE_UNIFORMS, &n);
                int32_t room = want - i;
                if (n > room) n = room;
                if (n > 0 && r_GetActiveUniformBlockiv)
                    r_GetActiveUniformBlockiv(program, index,
                                              KLFB_UB_ACTIVE_UNIFORM_INDICES,
                                              params + i);
                if (length) *length = i + n;
                return;
            }
            default:
                klfb_res_log_unhandled("glGetProgramResourceiv", iface, props[i]);
                v = 0; ok = 1;
                break;
            }
            if (es30) {
                if (r_GetActiveUniformBlockiv)
                    r_GetActiveUniformBlockiv(program, index, es30, &v);
                ok = 1;
            }
        } else if (iface == KLFB_IF_PROGRAM_INPUT) {
            // The ES 3.1 program-input resource index is the attribute index.
            switch (props[i]) {
            case KLFB_P_NAME_LENGTH:
            case KLFB_P_TYPE:
            case KLFB_P_ARRAY_SIZE:
            case KLFB_P_LOCATION: {
                char name[256];
                int32_t len = 0, size = 0;
                uint32_t type = 0;
                if (!r_GetActiveAttrib) break;
                r_GetActiveAttrib(program, index, sizeof name, &len, &size, &type, name);
                if (props[i] == KLFB_P_NAME_LENGTH)   v = len + 1;
                if (props[i] == KLFB_P_TYPE)          v = (int32_t)type;
                if (props[i] == KLFB_P_ARRAY_SIZE)    v = size;
                if (props[i] == KLFB_P_LOCATION)      v = (int32_t)index; // see ResourceLocation
                ok = 1;
                break;
            }
            case KLFB_P_REFERENCED_BY_VERTEX:   v = 1; ok = 1; break;
            case KLFB_P_REFERENCED_BY_FRAGMENT: v = 0; ok = 1; break;
            default:
                klfb_res_log_unhandled("glGetProgramResourceiv", iface, props[i]);
                v = 0; ok = 1;
                break;
            }
        } else {
            klfb_res_log_unhandled("glGetProgramResourceiv", iface, props[i]);
            ok = 1;
        }
        if (ok) { params[i] = v; written = i + 1; }
    }
    if (length) *length = written;
    klfb_err_say("GetProgramResourceiv", iface);
}

static void klfb_GetProgramResourceName(uint32_t program, uint32_t iface,
                                        uint32_t index, int32_t bufSize,
                                        int32_t *length, char *name) {
    klfb_res_resolve();
    if (length) *length = 0;
    if (!name || bufSize <= 0) return;
    name[0] = 0;
    if (iface == KLFB_IF_UNIFORM) {
        int32_t len = 0, size = 0;
        uint32_t type = 0;
        if (r_GetActiveUniform)
            r_GetActiveUniform(program, index, bufSize, &len, &size, &type, name);
        if (length) *length = len;
    } else if (iface == KLFB_IF_UNIFORM_BLOCK) {
        int32_t len = 0;
        if (r_GetActiveUniformBlockName)
            r_GetActiveUniformBlockName(program, index, bufSize, &len, name);
        if (length) *length = len;
    } else if (iface == KLFB_IF_PROGRAM_INPUT) {
        int32_t len = 0, size = 0;
        uint32_t type = 0;
        if (r_GetActiveAttrib)
            r_GetActiveAttrib(program, index, bufSize, &len, &size, &type, name);
        if (length) *length = len;
    } else {
        klfb_res_log_unhandled("glGetProgramResourceName", iface, 0);
    }
}

static uint32_t klfb_GetProgramResourceIndex(uint32_t program, uint32_t iface,
                                             const char *name) {
    klfb_res_resolve();
    if (!name) return KLFB_INVALID_INDEX;
    if (iface == KLFB_IF_UNIFORM) {
        uint32_t idx = KLFB_INVALID_INDEX;
        if (r_GetUniformIndices) r_GetUniformIndices(program, 1, &name, &idx);
        return idx;
    }
    if (iface == KLFB_IF_UNIFORM_BLOCK)
        return r_GetUniformBlockIndex ? r_GetUniformBlockIndex(program, name)
                                      : KLFB_INVALID_INDEX;
    klfb_res_log_unhandled("glGetProgramResourceIndex", iface, 0);
    return KLFB_INVALID_INDEX;
}

static int32_t klfb_GetUniformLocation(uint32_t prog, const char *name);
static int32_t klfb_GetProgramResourceLocation(uint32_t program, uint32_t iface,
                                               const char *name) {
    klfb_res_resolve();
    // THE door Unity 2019.4 uses — it never calls glGetUniformLocation. Routed
    // through the same thunk rather than to the driver, so the two entry points
    // cannot answer differently about the same program; a raw driver location
    // here and a pin remap on the glUniform* side is what broke Beat Saber's
    // menu UI (see klfb_pins_byname).
    if (name && iface == KLFB_IF_UNIFORM && r_GetUniformLocation)
        return klfb_GetUniformLocation(program, name);
    if (name && iface == KLFB_IF_PROGRAM_INPUT) {
        // Attrib "location" is queried by name too.
        static int32_t (*r_GetAttribLocation)(uint32_t, const char *);
        if (!r_GetAttribLocation) r_GetAttribLocation = (void *)asym("glGetAttribLocation");
        if (r_GetAttribLocation) return r_GetAttribLocation(program, name);
    }
    klfb_res_log_unhandled("glGetProgramResourceLocation", iface, 0);
    return -1;
}

static int32_t klfb_GetProgramResourceLocationIndex(uint32_t program,
                                                    uint32_t iface,
                                                    const char *name) {
    (void)program; (void)name;
    // Fragment-output location indices exist for dual-source blending, which
    // this title does not use.
    klfb_res_log_unhandled("glGetProgramResourceLocationIndex", iface, 0);
    return -1;
}

// The capture has to happen on the thread that owns the context, and the guest's
// eglSwapBuffers does not arrive there — measured: the GL thread owns the context,
// the swap comes in on a different one, and EGL refuses to migrate (EGL_BAD_ACCESS).
//
// So a swap only *requests* a capture, and the request is serviced from a call the
// GL thread makes itself. glFlush/glFinish are the natural place: the guest issues
// them at frame boundaries, and by definition they run where the drawing did.
static int  g_capture_pending;
static char g_capture_dir[512];
static unsigned glfb_capture_now(const char *dir);

// The frame-out seam (kl_glfb.h). Set once by the frontend before the guest
// starts, read on the GL thread at capture time; plain stores are fine for the
// same reason as the pose seam — registration happens long before the first
// swap, so there is no race to arbitrate.
static kl_glfb_frame_sink g_frame_sink;
static void              *g_frame_sink_ctx;
static unsigned long      g_last_frame_lit;

void kl_glfb_set_frame_sink(kl_glfb_frame_sink fn, void *ctx) {
    g_frame_sink = fn;
    g_frame_sink_ctx = ctx;
}
int kl_glfb_has_frame_sink(void) { return g_frame_sink != NULL; }
unsigned long kl_glfb_last_frame_lit(void) { return g_last_frame_lit; }

static void (*g_real_Flush)(void);
static void (*g_real_Finish)(void);

// ---- the GPU frame seam (kl_glfb.h) -----------------------------------------
//
// EGL_SYNC_METAL_SHARED_EVENT_ANGLE, not eglCreateSyncKHR's EGLint attribute
// list: the shared event arrives as a 64-bit pointer and an EGLint cannot hold
// one. eglCreateSync (EGL 1.5) takes EGLAttrib, which can. Getting this wrong
// truncates the pointer and ANGLE creates its OWN event instead — a sync that
// works perfectly and orders nothing we can wait on.
#define EGL_SYNC_METAL_SHARED_EVENT_ANGLE_              0x34D8
#define EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE_       0x34D9
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_    0x34DA
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_    0x34DB

static void *(*a_eglCreateSync)(void *, uint32_t, const intptr_t *);
static unsigned (*a_eglDestroySync)(void *, void *);
// Resolved from ANGLE directly rather than reusing g_real_Flush: that one is
// only filled in once the GUEST has resolved glFlush through the trampoline
// table, and a fence created but never committed leaves the compositor waiting
// on a value the GPU never reaches.
static void (*a_glFlush_fence)(void);
static void     *g_gpu_fence;        // an id<MTLSharedEvent>, owned by the caller
static void     *g_gpu_fence_sync;   // last frame's EGLSync, destroyed one frame late
static uint64_t  g_gpu_fence_value;

void kl_glfb_set_gpu_fence(void *mtl_shared_event) {
    g_gpu_fence = mtl_shared_event;
}
int kl_glfb_has_gpu_fence(void) { return g_gpu_fence != NULL; }
uint64_t kl_glfb_gpu_fence_value(void) {
    return __atomic_load_n(&g_gpu_fence_value, __ATOMIC_ACQUIRE);
}

// Runs on the context-owning thread, in place of glfb_capture_now, once per
// swap. Encodes "the guest's frame is done" into ANGLE's command stream and
// publishes the value that will carry it.
static unsigned klfb_gpu_frame_now(void) {
    if (!g_ready || !g_gpu_fence) return g_presented;
    if (!a_eglCreateSync) {
        a_eglCreateSync  = asym("eglCreateSync");
        a_eglDestroySync = asym("eglDestroySync");
        a_glFlush_fence  = asym("glFlush");
        // eglCreateSync, not eglCreateSyncKHR — the KHR form's attribute list is
        // EGLint, and a 64-bit MTLSharedEvent pointer does not fit in one.
        if (!a_eglCreateSync || !a_glFlush_fence) {
            fprintf(stderr, "  [glfb] this ANGLE has no %s — the GPU compositor "
                            "cannot order against the guest's frame\n",
                    a_eglCreateSync ? "glFlush" : "eglCreateSync");
            g_gpu_fence = NULL;
            return g_presented;
        }
    }
    uint64_t v = g_gpu_fence_value + 1;
    const intptr_t attrs[] = {
        EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE_,    (intptr_t)g_gpu_fence,
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_, (intptr_t)(uint32_t)v,
        EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_, (intptr_t)(uint32_t)(v >> 32),
        EGL_NONE_,
    };
    void *sync = a_eglCreateSync(g_dpy, EGL_SYNC_METAL_SHARED_EVENT_ANGLE_, attrs);
    if (!sync) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [glfb] eglCreateSync(METAL_SHARED_EVENT) failed — "
                            "the compositor will show whatever is in the eye "
                            "texture, unordered\n");
        return g_presented;
    }
    // Commit, so the signal actually reaches the GPU: eglCreateSync only
    // *queues* the event onto ANGLE's current command buffer. No wait here —
    // a glFinish would be the stall this whole path exists to remove.
    a_glFlush_fence();
    __atomic_store_n(&g_gpu_fence_value, v, __ATOMIC_RELEASE);
    // One frame late, so the object outlives the encode it belongs to. Nothing
    // documents that destroying it immediately is safe, and the failure mode
    // would be a signal that never arrives — i.e. a compositor that hangs.
    if (g_gpu_fence_sync && a_eglDestroySync)
        a_eglDestroySync(g_dpy, g_gpu_fence_sync);
    g_gpu_fence_sync = sync;
    return ++g_presented;
}

static void klfb_service_capture(void) {
    if (!g_capture_pending) return;
    g_capture_pending = 0;
    if (g_gpu_fence) { klfb_gpu_frame_now(); return; }
    glfb_capture_now(g_capture_dir);
}
static void klfb_Flush(void)  { if (g_real_Flush)  g_real_Flush();  klfb_service_capture(); }
static void klfb_Finish(void) { if (g_real_Finish) g_real_Finish(); klfb_service_capture(); }

static void klfb_GetIntegerv(uint32_t, int32_t *);
static void klfb_GetFloatv(uint32_t, float *);
static void klfb_GetBooleanv(uint32_t, uint8_t *);
static void klfb_GetInteger64v(uint32_t, int64_t *);
static void klfb_GetIntegeri_v(uint32_t, uint32_t, int32_t *);
static void klfb_GetInternalformativ(uint32_t, uint32_t, uint32_t, int32_t, int32_t *);
static void (*g_real_GetIntegerv)(uint32_t, int32_t *);
static void (*g_real_GetFloatv)(uint32_t, float *);
static void (*g_real_GetBooleanv)(uint32_t, uint8_t *);
static void (*g_real_GetInteger64v)(uint32_t, int64_t *);
static void (*g_real_GetIntegeri_v)(uint32_t, uint32_t, int32_t *);
static void (*g_real_GetInternalformativ)(uint32_t, uint32_t, uint32_t, int32_t, int32_t *);

// Unity spams "OpenGL Error: Invalid texture unit!" ~6x/frame from the very
// first frames. glActiveTexture is the only unit-selecting call; bracket it
// and print the first failures with the unit requested. The comparison to make
// is against kl_gl_cap_max_combined_texture_units(): a failing unit BELOW it is
// the driver disagreeing with the description, and a unit at or above it is the
// guest over-reading what it was told. Since the description is now adopted from
// the driver, the first case should no longer be reachable.
static void (*g_real_ActiveTexture)(uint32_t);
static void klfb_ActiveTexture(uint32_t unit) {
    // Drain first: the guest leaves errors unread, so an undrained bracket
    // reports a leftover as glActiveTexture's own. Errors after this point are
    // provably this call's.
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_ActiveTexture) g_real_ActiveTexture(unit);
    // The guest's own selection, which the scene-depth bind has to put back
    // after it borrows the reserved unit (klfb_depth_fetch_apply).
    g_df_active_unit = unit;
    // Which units does the guest actually select? Unity's "Invalid texture
    // unit" check rejects above its own cap — if that preempts the bind, no
    // high unit ever reaches GL, and measuring the request stream proves it.
    {
        static int logu = -1;
        // Sized to the frontend ceiling (gl::IMPLEMENTATION_MAX_ACTIVE_TEXTURES),
        // not to the current cap: this array exists to observe the guest asking
        // for units ABOVE what we advertise, so bounding it by the advertised
        // number would blind it to the only interesting case.
        static uint8_t seen[192];
        if (logu < 0) logu = kl_env_on("KL_GLFB_LOG_UNITS", 0);
        int u = (int)(unit - 0x84C0);
        if (logu && u >= 0 && u < (int)(sizeof seen) && !seen[u]) {
            seen[u] = 1;
            fprintf(stderr, "  [glfb] glActiveTexture: first use of unit %d\n", u);
        }
    }
    static int said;
    if (said < 20 && a_glGetError) {
        uint32_t e = a_glGetError();
        if (e) {
            said++;
            fprintf(stderr, "  [glfb] glActiveTexture(0x%x) (unit %d) -> GL error 0x%x\n",
                    unit, (int)(unit - 0x84C0), e);
        }
    }
}

// The 0x502 the errprobe meets before program-7 draws is generated by a call
// the probe family doesn't wrap. Bracket the per-draw state setters the same
// drain-and-report way; the one that prints is the generator.
static void klfb_err_say(const char *what, uint32_t a0) {
    static int said;
    if (said < 30 && a_glGetError) {
        uint32_t e = a_glGetError();
        if (e) {
            said++;
            int32_t prog = -1, fb = -1;
            if (a_glGetIntegerv) {
                a_glGetIntegerv(0x8B8D, &prog);
                a_glGetIntegerv(0x8CA6, &fb);
            }
            fprintf(stderr, "  [glfb] ERRSRC %s(0x%x) -> GL error 0x%x "
                            "(program=%d fb=%d)\n", what, a0, e, prog, fb);
        }
    }
}
static void (*g_real_UseProgram)(uint32_t);
static void klfb_UseProgram(uint32_t p) {
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_UseProgram) g_real_UseProgram(p);
    g_cur_prog = p;                       // what the uniform remap is scoped to
    // ...and, if this program is one klfb_rewrite_depth_fetch touched, point its
    // injected sampler at the draw framebuffer's depth attachment. No-op for
    // every other program, which is 605 of the 610 this title links.
    klfb_depth_fetch_apply();
    klfb_err_say("glUseProgram", p);
}
// The other half of the external-image retarget in klfb_rewrite_glsl. The
// shader now declares sampler2D, so every target that names an external texture
// has to become GL_TEXTURE_2D or the sampler reads nothing — and ANGLE would
// reject the target anyway, since it does not advertise the extension that
// defines it.
#define GL_TEXTURE_EXTERNAL_OES_ 0x8D65
#define GL_TEXTURE_2D_           0x0DE1
#define GL_TEXTURE_BUFFER_       0x8C2A
static uint32_t klfb_detarget(uint32_t t) {
    if (t == GL_TEXTURE_EXTERNAL_OES_ || t == GL_TEXTURE_BUFFER_)
        return GL_TEXTURE_2D_;
    return t;
}

// ...and the third piece of it. An image of OURS is not an ANGLE EGLImage at
// all — it is a pbuffer over the decoded frame's IOSurface — so the call becomes
// eglBindTexImage. Anything else is forwarded, detargeted, so a guest that ever
// does have a real external image is unaffected by our presence.
static void (*g_real_EGLImageTargetTexture2DOES)(uint32_t, void *);
static void klfb_EGLImageTargetTexture2DOES(uint32_t target, void *image) {
    if (kl_glfb_is_image(image)) { kl_glfb_image_bind(image); return; }
    if (g_real_EGLImageTargetTexture2DOES)
        g_real_EGLImageTargetTexture2DOES(klfb_detarget(target), image);
}

static void (*g_real_BindTexture)(uint32_t, uint32_t);
static void klfb_texbuf_refresh(uint32_t tex);        // the other half, below
static void klfb_BindTexture(uint32_t t, uint32_t n) {
    if (a_glGetError) while (a_glGetError()) {}
    // The mip-chain ledger's destination, remembered instead of queried. Only
    // the guest's own binds land here — the runtime's scratch binds go through
    // g_real_BindTexture directly and cannot corrupt the record.
    if (t == GL_TEXTURE_2D_) g_bound2d = n;
    if (g_real_BindTexture) g_real_BindTexture(klfb_detarget(t), n);
    // A buffer texture has no storage of its own — it IS whatever the buffer
    // currently holds — so the copy has to be re-made whenever the guest binds
    // it, not once at glTexBuffer time. UE4 rebinds per draw, which is exactly
    // the point at which the contents have to be current.
    klfb_texbuf_refresh(n);
    if (t == 0x806F /* TEXTURE_3D */) klfb_note_tex3d_event(n, 0, 0, 1);
    klfb_note_maybe_depth_sample(n);
    klfb_err_say("glBindTexture", n);
}

// ---- glTexBuffer, served as a copy into a 2-D texture ----------------------
//
// The shader half is klfb_rewrite_texbuffer: `samplerBuffer` becomes
// `sampler2D` and the flat index splits into (i & MASK, i >> SHIFT). This half
// gives that sampler something to read. GL_PIXEL_UNPACK_BUFFER is ES 3.0 core,
// so buffer -> texture is a server-side copy with no readback and no staging
// allocation of ours.
struct klfb_texbuf { uint32_t tex, buffer, ifmt; long offset; long size; int used; };
static struct klfb_texbuf g_texbuf[32];
static pthread_mutex_t g_texbuf_lock = PTHREAD_MUTEX_INITIALIZER;

// internalformat -> (format, type, bytes per texel). Only the sized formats a
// buffer texture is allowed to carry; anything else is refused out loud rather
// than uploaded as the wrong thing.
static int klfb_texbuf_fmt(uint32_t ifmt, uint32_t *fmt, uint32_t *type, int *bpp) {
    switch (ifmt) {
    case 0x8229 /* R8     */: *fmt=0x1903; *type=0x1401; *bpp=1;  return 1;
    case 0x822B /* RG8    */: *fmt=0x8227; *type=0x1401; *bpp=2;  return 1;
    case 0x8058 /* RGBA8  */: *fmt=0x1908; *type=0x1401; *bpp=4;  return 1;
    case 0x822D /* R16F   */: *fmt=0x1903; *type=0x140B; *bpp=2;  return 1;
    case 0x822F /* RG16F  */: *fmt=0x8227; *type=0x140B; *bpp=4;  return 1;
    case 0x881A /* RGBA16F*/: *fmt=0x1908; *type=0x140B; *bpp=8;  return 1;
    case 0x822E /* R32F   */: *fmt=0x1903; *type=0x1406; *bpp=4;  return 1;
    case 0x8230 /* RG32F  */: *fmt=0x8227; *type=0x1406; *bpp=8;  return 1;
    case 0x8814 /* RGBA32F*/: *fmt=0x1908; *type=0x1406; *bpp=16; return 1;
    case 0x8231 /* R8I    */: *fmt=0x8D94; *type=0x1400; *bpp=1;  return 1;
    case 0x8232 /* R8UI   */: *fmt=0x8D94; *type=0x1401; *bpp=1;  return 1;
    case 0x8233 /* R16I   */: *fmt=0x8D94; *type=0x1402; *bpp=2;  return 1;
    case 0x8234 /* R16UI  */: *fmt=0x8D94; *type=0x1403; *bpp=2;  return 1;
    case 0x8237 /* RG8I   */: *fmt=0x8228; *type=0x1400; *bpp=2;  return 1;
    case 0x8238 /* RG8UI  */: *fmt=0x8228; *type=0x1401; *bpp=2;  return 1;
    case 0x8239 /* RG16I  */: *fmt=0x8228; *type=0x1402; *bpp=4;  return 1;
    case 0x823A /* RG16UI */: *fmt=0x8228; *type=0x1403; *bpp=4;  return 1;
    case 0x8D8E /* RGBA8I */: *fmt=0x8D99; *type=0x1400; *bpp=4;  return 1;
    case 0x8D7C /* RGBA8UI*/: *fmt=0x8D99; *type=0x1401; *bpp=4;  return 1;
    case 0x8D88 /* RGBA16I*/: *fmt=0x8D99; *type=0x1402; *bpp=8;  return 1;
    case 0x8D76 /* RGBA16UI*/:*fmt=0x8D99; *type=0x1403; *bpp=8;  return 1;
    case 0x8235 /* R32I   */: *fmt=0x8D94; *type=0x1404; *bpp=4;  return 1;
    case 0x8236 /* R32UI  */: *fmt=0x8D94; *type=0x1405; *bpp=4;  return 1;
    case 0x823B /* RG32I  */: *fmt=0x8228; *type=0x1404; *bpp=8;  return 1;
    case 0x823C /* RG32UI */: *fmt=0x8228; *type=0x1405; *bpp=8;  return 1;
    case 0x8D82 /* RGBA32I*/: *fmt=0x8D99; *type=0x1404; *bpp=16; return 1;
    case 0x8D70 /* RGBA32UI*/:*fmt=0x8D99; *type=0x1405; *bpp=16; return 1;
    }
    return 0;
}

static void klfb_texbuf_copy(struct klfb_texbuf *e) {
    uint32_t fmt = 0, type = 0;
    int bpp = 0;
    if (!klfb_texbuf_fmt(e->ifmt, &fmt, &type, &bpp)) {
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "  [glfb] glTexBuffer: internalformat 0x%x is not in the "
                            "buffer-texture table — the sampler will read nothing\n",
                    e->ifmt); }
        return;
    }
    static void (*r_TexImage2D)(uint32_t,int32_t,int32_t,int32_t,int32_t,int32_t,
                                uint32_t,uint32_t,const void*);
    static void (*r_TexSubImage2D)(uint32_t,int32_t,int32_t,int32_t,int32_t,int32_t,
                                   uint32_t,uint32_t,const void*);
    static void (*r_TexParameteri)(uint32_t,uint32_t,int32_t);
    static void (*r_GetBufferParameteriv)(uint32_t,uint32_t,int32_t*);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        r_TexImage2D           = asym("glTexImage2D");
        r_TexSubImage2D        = asym("glTexSubImage2D");
        r_TexParameteri        = asym("glTexParameteri");
        r_GetBufferParameteriv = asym("glGetBufferParameteriv");
    }
    if (!r_TexImage2D || !r_TexSubImage2D || !g_real_BindBufferGL || !g_real_BindTexture)
        return;

    int32_t saved_pack = 0, saved_tex = 0;
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8895 /* PIXEL_UNPACK_BUFFER_BINDING */, &saved_pack);
        a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &saved_tex);
    }
    g_real_BindBufferGL(0x88EC /* PIXEL_UNPACK_BUFFER */, e->buffer);

    long size = e->size;
    if (size <= 0 && r_GetBufferParameteriv) {
        // The buffer's whole extent, minus whatever glTexBufferRange skipped.
        int32_t whole = 0;
        static void (*r_BindBufferQ)(uint32_t, uint32_t);
        (void)r_BindBufferQ;
        g_real_BindBufferGL(0x8892 /* ARRAY_BUFFER */, e->buffer);
        r_GetBufferParameteriv(0x8892, 0x8764 /* BUFFER_SIZE */, &whole);
        g_real_BindBufferGL(0x8892, 0);
        size = (long)whole - e->offset;
    }
    long texels = size > 0 ? size / bpp : 0;
    if (texels <= 0) { g_real_BindBufferGL(0x88EC, (uint32_t)saved_pack); return; }
    long rows = (texels + KLFB_TEXBUF_W - 1) / KLFB_TEXBUF_W;

    g_real_BindTexture(GL_TEXTURE_2D_, e->tex);
    // Reallocated only when the shape changes; glTexImage2D with a NULL pointer
    // while a PBO is bound would be read as an offset, so the allocation
    // happens with the PBO unbound.
    static long last_rows[32];
    unsigned slot = (unsigned)(e - g_texbuf);
    if (slot < 32 && last_rows[slot] != rows) {
        g_real_BindBufferGL(0x88EC, 0);
        r_TexImage2D(GL_TEXTURE_2D_, 0, (int32_t)e->ifmt, KLFB_TEXBUF_W,
                     (int32_t)rows, 0, fmt, type, NULL);
        if (r_TexParameteri) {
            r_TexParameteri(GL_TEXTURE_2D_, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */);
            r_TexParameteri(GL_TEXTURE_2D_, 0x2800 /* MAG_FILTER */, 0x2600);
            r_TexParameteri(GL_TEXTURE_2D_, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
            r_TexParameteri(GL_TEXTURE_2D_, 0x2803 /* WRAP_T */, 0x812F);
            r_TexParameteri(GL_TEXTURE_2D_, 0x813D /* MAX_LEVEL */, 0);
        }
        last_rows[slot] = rows;
        g_real_BindBufferGL(0x88EC, e->buffer);
    }
    long full = texels / KLFB_TEXBUF_W, rem = texels % KLFB_TEXBUF_W;
    if (full)
        r_TexSubImage2D(GL_TEXTURE_2D_, 0, 0, 0, KLFB_TEXBUF_W, (int32_t)full,
                        fmt, type, (const void *)(uintptr_t)e->offset);
    if (rem)
        r_TexSubImage2D(GL_TEXTURE_2D_, 0, 0, (int32_t)full, (int32_t)rem, 1,
                        fmt, type,
                        (const void *)(uintptr_t)(e->offset + full * KLFB_TEXBUF_W * bpp));

    g_real_BindBufferGL(0x88EC, (uint32_t)saved_pack);
    g_real_BindTexture(GL_TEXTURE_2D_, (uint32_t)saved_tex);
    if (a_glGetError) while (a_glGetError()) {}
}

static void klfb_texbuf_refresh(uint32_t tex) {
    if (!tex) return;
    pthread_mutex_lock(&g_texbuf_lock);
    for (unsigned i = 0; i < 32; i++)
        if (g_texbuf[i].used && g_texbuf[i].tex == tex) {
            klfb_texbuf_copy(&g_texbuf[i]);
            break;
        }
    pthread_mutex_unlock(&g_texbuf_lock);
}

static void klfb_texbuf_note(uint32_t ifmt, uint32_t buffer, long offset, long size) {
    int32_t tex = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8069 /* TEXTURE_BINDING_2D */, &tex);
    if (tex <= 0) return;                       // nothing bound: nothing to serve
    pthread_mutex_lock(&g_texbuf_lock);
    struct klfb_texbuf *e = NULL;
    for (unsigned i = 0; i < 32; i++)
        if (g_texbuf[i].used && g_texbuf[i].tex == (uint32_t)tex) { e = &g_texbuf[i]; break; }
    if (!e)
        for (unsigned i = 0; i < 32; i++)
            if (!g_texbuf[i].used) { e = &g_texbuf[i]; e->used = 1; e->tex = (uint32_t)tex; break; }
    if (e) {
        e->buffer = buffer; e->ifmt = ifmt; e->offset = offset; e->size = size;
        static int said;
        if (!said) { said = 1;
            fprintf(stderr, "  [glfb] glTexBuffer(texture %u <- buffer %u, "
                            "internalformat 0x%x): copied into a %d-wide 2D texture "
                            "on every bind (ANGLE's Metal backend has no "
                            "GL_EXT_texture_buffer)\n",
                    (uint32_t)tex, buffer, ifmt, KLFB_TEXBUF_W); }
        klfb_texbuf_copy(e);
    }
    pthread_mutex_unlock(&g_texbuf_lock);
}

// ---- the indexed (per-draw-buffer) state family, on an ES 3.0 backend -------
//
// glColorMaski / glEnablei / glDisablei / glBlendFuncSeparatei /
// glBlendEquationSeparatei are ES 3.2 (EXT_draw_buffers_indexed). ANGLE's Metal
// backend is ES 3.0 and rejects all of them with GL_INVALID_OPERATION — which
// UE4 never checks, so a colour mask it believes it wrote is simply not there,
// and a frame drawn with the previous mask is a frame that can be entirely
// black. KL_GLFB_ERRSCAN=0x502 named them: glColorMaski, glDisablei, glEnablei,
// glBlendFuncSeparatei and glBlendEquationSeparatei, every frame.
//
// Draw buffer 0 is the whole of what this gateway's targets have, and for
// buffer 0 the indexed call means exactly what the unindexed one means. So
// index 0 is served by the ES 3.0 entry point and any other index is reported
// once and dropped — dropping it is what ANGLE was doing anyway, but silently.
static void (*g_real_ColorMask)(uint8_t, uint8_t, uint8_t, uint8_t);
// The thunk table stores ANGLE's pointer unconditionally; these slots exist for
// that and are never called (ANGLE rejects every one of them).
static void *g_real_ColorMaski_, *g_real_ColorMaskiEXT_;
static void *g_real_Enablei_, *g_real_EnableiEXT_;
static void *g_real_Disablei_, *g_real_DisableiEXT_;
static void *g_real_BlendFuncSeparatei_, *g_real_BlendFuncSeparateiEXT_;
static void *g_real_BlendFunci_, *g_real_BlendFunciEXT_;
static void *g_real_BlendEquationSeparatei_, *g_real_BlendEquationSeparateiEXT_;
static void *g_real_BlendEquationi_, *g_real_BlendEquationiEXT_;
static void (*g_real_BlendFuncSeparate)(uint32_t, uint32_t, uint32_t, uint32_t);
static void (*g_real_BlendEquationSeparate)(uint32_t, uint32_t);
static void (*g_real_EnableCap)(uint32_t);
static void (*g_real_DisableCap)(uint32_t);
static int klfb_indexed_resolve(void) {
    static int done;
    if (!done) {
        done = 1;
        g_real_ColorMask            = asym("glColorMask");
        g_real_BlendFuncSeparate    = asym("glBlendFuncSeparate");
        g_real_BlendEquationSeparate= asym("glBlendEquationSeparate");
        g_real_EnableCap            = asym("glEnable");
        g_real_DisableCap           = asym("glDisable");
    }
    return 1;
}
static void klfb_indexed_drop(const char *what, uint32_t buf) {
    static int said;
    if (said++ < 4)
        fprintf(stderr, "  [glfb] %s(draw buffer %u) dropped — ES 3.0 has no "
                        "per-draw-buffer state and this target has one buffer\n",
                what, buf);
}
static void klfb_ColorMaski(uint32_t buf, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    klfb_indexed_resolve();
    if (buf != 0) { klfb_indexed_drop("glColorMaski", buf); return; }
    if (g_real_ColorMask) g_real_ColorMask(r, g, b, a);
}
static void klfb_Enablei(uint32_t cap, uint32_t buf) {
    klfb_indexed_resolve();
    if (buf != 0) { klfb_indexed_drop("glEnablei", buf); return; }
    if (g_real_EnableCap) g_real_EnableCap(cap);
}
static void klfb_Disablei(uint32_t cap, uint32_t buf) {
    klfb_indexed_resolve();
    if (buf != 0) { klfb_indexed_drop("glDisablei", buf); return; }
    if (g_real_DisableCap) g_real_DisableCap(cap);
}
static void klfb_BlendFuncSeparatei(uint32_t buf, uint32_t sr, uint32_t dr,
                                    uint32_t sa, uint32_t da) {
    klfb_indexed_resolve();
    if (buf != 0) { klfb_indexed_drop("glBlendFuncSeparatei", buf); return; }
    if (g_real_BlendFuncSeparate) g_real_BlendFuncSeparate(sr, dr, sa, da);
}
static void klfb_BlendFunci(uint32_t buf, uint32_t src, uint32_t dst) {
    klfb_BlendFuncSeparatei(buf, src, dst, src, dst);
}
static void klfb_BlendEquationSeparatei(uint32_t buf, uint32_t rgb, uint32_t alpha) {
    klfb_indexed_resolve();
    if (buf != 0) { klfb_indexed_drop("glBlendEquationSeparatei", buf); return; }
    if (g_real_BlendEquationSeparate) g_real_BlendEquationSeparate(rgb, alpha);
}
static void klfb_BlendEquationi(uint32_t buf, uint32_t mode) {
    klfb_BlendEquationSeparatei(buf, mode, mode);
}

// ANGLE's own entry points are never called — it has no texture buffers — but
// the thunk table stores the real pointer unconditionally, so the slots exist.
static void *g_real_TexBuffer, *g_real_TexBufferEXT, *g_real_TexBufferOES;
static void *g_real_TexBufferRange, *g_real_TexBufferRangeEXT, *g_real_TexBufferRangeOES;
static void klfb_TexBuffer(uint32_t target, uint32_t ifmt, uint32_t buffer) {
    (void)target;
    klfb_texbuf_note(ifmt, buffer, 0, 0);
}
static void klfb_TexBufferRange(uint32_t target, uint32_t ifmt, uint32_t buffer,
                                long offset, long size) {
    (void)target;
    klfb_texbuf_note(ifmt, buffer, offset, size);
}
// glGetUniformLocation, so the built-in-name rename above stays invisible to the
// guest: it still asks for `length` and still gets the location of the uniform it
// declared. Only a miss is retried, so a shader we did not rewrite is untouched
// and a real -1 stays -1.
// ...and every answer it gives is recorded, so a uniform call that fails can say
// where its location CAME from. "Location 2 is a mat4" is only half a finding:
// the other half is which program the guest was looking at when it asked, and
// nothing else on the path knows that.
#define KLFB_LOCS 512
static struct { uint32_t prog; int32_t loc; char name[48]; } g_uloc[KLFB_LOCS];
static unsigned g_uloc_n;

static int32_t klfb_GetUniformLocation(uint32_t prog, const char *name) {
    // Resolved here as well as in the thunk table: the ES 3.1 door below calls
    // this for a guest that never resolves glGetUniformLocation itself, so the
    // thunk's real slot can still be empty.
    if (!g_real_GetUniformLocation)
        g_real_GetUniformLocation = (void *)asym("glGetUniformLocation");
    if (!g_real_GetUniformLocation || !name) return -1;
    // Asking is the whole decision: from here on this program is described to
    // the guest in the LINKER's numbering and glUniform* leaves it alone. The
    // alternative — answer in the guest's pinned numbering and remap on the way
    // back — was what shipped, and it cannot be made right: the two numberings
    // share one integer space, so an unpinned uniform whose linker location
    // happens to equal some other uniform's pinned one is redirected into it,
    // silently. There is no such ambiguity once only one numbering is in use.
    klfb_pins_byname(prog);
    int32_t loc = klfb_actual_loc(prog, name);
    if (loc >= 0) {
        pthread_mutex_lock(&g_compile_lock);
        unsigned i = 0;
        for (; i < g_uloc_n; i++)
            if (g_uloc[i].prog == prog && g_uloc[i].loc == loc) break;
        if (i == g_uloc_n && g_uloc_n < KLFB_LOCS) g_uloc_n++;
        if (i < KLFB_LOCS) {
            g_uloc[i].prog = prog; g_uloc[i].loc = loc;
            snprintf(g_uloc[i].name, sizeof g_uloc[i].name, "%s", name);
        }
        pthread_mutex_unlock(&g_compile_lock);
    }
    return loc;
}

// What the guest was told about this program, in the order it asked. Appends to
// `out`. The WHOLE list, not just the matching location: "why is the guest
// writing a float at location 1" is answered by the guest never having asked
// for location 1, and only the whole list says so.
static void klfb_locs_say(uint32_t prog, char *out, size_t n) {
    size_t at = strlen(out);
    pthread_mutex_lock(&g_compile_lock);
    at += (size_t)snprintf(out + at, n - at, " — it was told:");
    int any = 0;
    for (unsigned i = 0; i < g_uloc_n && at + 40 < n; i++)
        if (g_uloc[i].prog == prog) {
            at += (size_t)snprintf(out + at, n - at, " %s=%d",
                                   g_uloc[i].name, g_uloc[i].loc);
            any = 1;
        }
    if (!any) snprintf(out + at, n - at, " nothing (it never asked)");
    pthread_mutex_unlock(&g_compile_lock);
}
static void (*g_real_BindSampler)(uint32_t, uint32_t);
static void klfb_BindSampler(uint32_t u, uint32_t s) {
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_BindSampler) g_real_BindSampler(u, s);
    klfb_err_say("glBindSampler", s);
}
// The uniform family, every entry point of it, for one reason: each one carries
// a location the guest may have pinned in the shader, and a location the guest
// pinned means something different to the linker. klfb_remap_loc is the whole
// body — it is the identity for any guest that resolves uniforms by name, which
// is why this can be unconditional. The two hand-written members below
// (glUniform1i, glUniform1f) also carry error probes and predate this.
#define KLFB_UNIFORM_N(fn, T, N, PARAMS, ARGS)                                \
    static void (*g_real_##fn) PARAMS;                                        \
    static void klfb_##fn PARAMS {                                            \
        if (g_real_##fn) g_real_##fn ARGS;                                    \
    }
#define KLFB_U1(fn, T) KLFB_UNIFORM_N(fn, T, 1, (int32_t l, T a), \
                                      (klfb_remap_loc(l), a))
#define KLFB_U2(fn, T) KLFB_UNIFORM_N(fn, T, 2, (int32_t l, T a, T b), \
                                      (klfb_remap_loc(l), a, b))
#define KLFB_U3(fn, T) KLFB_UNIFORM_N(fn, T, 3, (int32_t l, T a, T b, T c), \
                                      (klfb_remap_loc(l), a, b, c))
#define KLFB_U4(fn, T) KLFB_UNIFORM_N(fn, T, 4, (int32_t l, T a, T b, T c, T d), \
                                      (klfb_remap_loc(l), a, b, c, d))
#define KLFB_UV(fn, T) KLFB_UNIFORM_N(fn, T, 0, (int32_t l, int32_t n, const T *v), \
                                      (klfb_remap_loc(l), n, v))
#define KLFB_UM(fn)    KLFB_UNIFORM_N(fn, float, 0, \
                                      (int32_t l, int32_t n, uint8_t t, const float *v), \
                                      (klfb_remap_loc(l), n, t, v))

KLFB_U2(Uniform2f, float) KLFB_U3(Uniform3f, float) KLFB_U4(Uniform4f, float)
KLFB_U2(Uniform2i, int32_t) KLFB_U3(Uniform3i, int32_t) KLFB_U4(Uniform4i, int32_t)
KLFB_U1(Uniform1ui, uint32_t) KLFB_U2(Uniform2ui, uint32_t)
KLFB_U3(Uniform3ui, uint32_t) KLFB_U4(Uniform4ui, uint32_t)
KLFB_UV(Uniform1fv, float) KLFB_UV(Uniform2fv, float)
KLFB_UV(Uniform3fv, float) KLFB_UV(Uniform4fv, float)
KLFB_UV(Uniform1iv, int32_t) KLFB_UV(Uniform2iv, int32_t)
KLFB_UV(Uniform3iv, int32_t) KLFB_UV(Uniform4iv, int32_t)
KLFB_UV(Uniform1uiv, uint32_t) KLFB_UV(Uniform2uiv, uint32_t)
KLFB_UV(Uniform3uiv, uint32_t) KLFB_UV(Uniform4uiv, uint32_t)
KLFB_UM(UniformMatrix2fv) KLFB_UM(UniformMatrix3fv) KLFB_UM(UniformMatrix4fv)
KLFB_UM(UniformMatrix2x3fv) KLFB_UM(UniformMatrix3x2fv)
KLFB_UM(UniformMatrix2x4fv) KLFB_UM(UniformMatrix4x2fv)
KLFB_UM(UniformMatrix3x4fv) KLFB_UM(UniformMatrix4x3fv)

static void (*g_real_Uniform1i)(int32_t, int32_t);
static void klfb_Uniform1i(int32_t loc, int32_t v) {
    if (a_glGetError) while (a_glGetError()) {}
    loc = klfb_remap_loc(loc);
    if (g_real_Uniform1i) g_real_Uniform1i(loc, v);
    static int said;
    if (said < 30 && a_glGetError) {
        uint32_t e = a_glGetError();
        if (e) {
            said++;
            int32_t prog = -1;
            if (a_glGetIntegerv) a_glGetIntegerv(0x8B8D, &prog);
            fprintf(stderr, "  [glfb] ERRSRC glUniform1i(loc=%d, %d) -> 0x%x "
                            "(program=%d)\n", loc, v, e, prog);
        }
    }
}
// glUniform1f is INVALID_OPERATION about twelve times a frame on the Steam Link
// VR path (glutils.cpp:205, ~17k a run), and the error code alone cannot say
// which of its three causes it is: no current program, a location belonging to
// a different program, or a location whose uniform is not a float. So name the
// uniform. glGetActiveUniform indexes the CURRENT program's own list, and
// glGetUniformLocation maps each name back to a location — if none of them
// matches, the location came from somewhere else, and that is the answer.
static void (*g_real_Uniform1f)(int32_t, float);
static void klfb_Uniform1f(int32_t loc, float v) {
    if (a_glGetError) while (a_glGetError()) {}
    loc = klfb_remap_loc(loc);
    if (g_real_Uniform1f) g_real_Uniform1f(loc, v);
    static int said;
    if (said >= 20 || !a_glGetError) return;
    uint32_t e = a_glGetError();
    if (!e) return;
    said++;
    int32_t prog = -1;
    if (a_glGetIntegerv) a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);

    static void (*r_GetProgramiv)(uint32_t, uint32_t, int32_t *);
    static void (*r_GetActiveUniform)(uint32_t, uint32_t, int32_t, int32_t *,
                                      int32_t *, uint32_t *, char *);
    if (!r_GetProgramiv) {
        r_GetProgramiv = asym("glGetProgramiv");
        r_GetActiveUniform = asym("glGetActiveUniform");
    }
    char who[512];
    snprintf(who, sizeof who, "no current program");
    if (prog > 0 && r_GetProgramiv && r_GetActiveUniform && g_real_GetUniformLocation) {
        int32_t n = 0;
        r_GetProgramiv((uint32_t)prog, 0x8B86 /* ACTIVE_UNIFORMS */, &n);
        snprintf(who, sizeof who, "no uniform of program %d has location %d "
                                  "(it has %d)", prog, loc, n);
        for (int32_t i = 0; i < n; i++) {
            char name[96] = ""; int32_t len = 0, size = 0; uint32_t type = 0;
            r_GetActiveUniform((uint32_t)prog, (uint32_t)i, (int32_t)sizeof name,
                               &len, &size, &type, name);
            if (g_real_GetUniformLocation((uint32_t)prog, name) != loc) continue;
            snprintf(who, sizeof who, "`%s` of program %d is type 0x%x[%d], "
                                      "not a float", name, prog, type, size);
            break;
        }
    }
    if (prog > 0) klfb_locs_say((uint32_t)prog, who, sizeof who);
    fprintf(stderr, "  [glfb] ERRSRC glUniform1f(loc=%d, %g) -> 0x%x — %s\n",
            loc, (double)v, e, who);
}

// The originals live in `g_real_EnableCap` / `g_real_DisableCap`, declared with
// the indexed (`glEnablei`) hooks that share them — see the thunk table entries
// for glEnable/glDisable, which fill THOSE. There is deliberately no second
// pointer pair here: a `g_real_Enable` the table filled and this body never read
// is exactly how every glEnable in the process became a silent no-op (ett r49).
//
// The table's fill is the primary; `klfb_indexed_resolve()` is the fallback for
// a guest that reaches these hooks by some path that left the table row unset.
static void klfb_srgb_note(int enabled) {
    static int said;
    if (g_srgb_write_off == !enabled) return;
    if (!enabled) g_srgb_write_off = 1;      // sticky — see above
    if (!said++)
        fprintf(stderr, "  [glfb] the guest %s GL_FRAMEBUFFER_SRGB — ANGLE has no "
                        "EXT_sRGB_write_control, so the encode is applied "
                        "regardless and the composite undoes it\n",
                enabled ? "enabled" : "disabled");
    klfb_srgb_settle();
}

static void klfb_Enable(uint32_t cap) {
    if (cap == KLFB_GL_FRAMEBUFFER_SRGB) { klfb_srgb_note(1); return; }
    if (!g_real_EnableCap) klfb_indexed_resolve();
    // Before the test goes on, not after: an enable followed by a draw is the
    // whole window in which a zero box can discard a frame.
    if (cap == 0x0C11 /* GL_SCISSOR_TEST */) klfb_scissor_enable_check();
    if (g_real_EnableCap) g_real_EnableCap(cap);
}

static void klfb_Disable(uint32_t cap) {
    if (cap == KLFB_GL_FRAMEBUFFER_SRGB) { klfb_srgb_note(0); return; }
    if (!g_real_DisableCap) klfb_indexed_resolve();
    if (g_real_DisableCap) g_real_DisableCap(cap);
}

static void (*g_real_GenerateMipmap)(uint32_t);
static void klfb_GenerateMipmap(uint32_t t) {
    KLFB_LAZY_FLUSH();
    __atomic_fetch_add(&g_genmipmap_calls, 1, __ATOMIC_RELAXED);
    if (a_glGetError) while (a_glGetError()) {}
    if (g_real_GenerateMipmap) g_real_GenerateMipmap(t);
    klfb_err_say("glGenerateMipmap", t);
}

// KL_GLFB_LAZY_UNBIND's flush points that nothing else hooks: every other
// entry point whose meaning depends on the framebuffer binding. Pure
// pass-throughs (the held unbind is forwarded first; a no-op when it is off).
static void (*g_lz_DrawBuffers)(int32_t, const uint32_t *);
static void klfb_lz_DrawBuffers(int32_t n, const uint32_t *b) { KLFB_LAZY_FLUSH(); g_lz_DrawBuffers(n, b); }
static void (*g_lz_ReadBuffer)(uint32_t);
static void klfb_lz_ReadBuffer(uint32_t m) { KLFB_LAZY_FLUSH(); g_lz_ReadBuffer(m); }
static uint32_t (*g_lz_CheckFramebufferStatus)(uint32_t);
static uint32_t klfb_lz_CheckFramebufferStatus(uint32_t t) { KLFB_LAZY_FLUSH(); return g_lz_CheckFramebufferStatus(t); }
static void (*g_lz_ClearBufferfv)(uint32_t, int32_t, const float *);
static void klfb_lz_ClearBufferfv(uint32_t b, int32_t d, const float *v) { KLFB_LAZY_FLUSH(); g_lz_ClearBufferfv(b, d, v); }
static void (*g_lz_ClearBufferiv)(uint32_t, int32_t, const int32_t *);
static void klfb_lz_ClearBufferiv(uint32_t b, int32_t d, const int32_t *v) { KLFB_LAZY_FLUSH(); g_lz_ClearBufferiv(b, d, v); }
static void (*g_lz_ClearBufferuiv)(uint32_t, int32_t, const uint32_t *);
static void klfb_lz_ClearBufferuiv(uint32_t b, int32_t d, const uint32_t *v) { KLFB_LAZY_FLUSH(); g_lz_ClearBufferuiv(b, d, v); }
static void (*g_lz_ClearBufferfi)(uint32_t, int32_t, float, int32_t);
static void klfb_lz_ClearBufferfi(uint32_t b, int32_t d, float f, int32_t st) { KLFB_LAZY_FLUSH(); g_lz_ClearBufferfi(b, d, f, st); }
static void (*g_lz_FramebufferRenderbuffer)(uint32_t, uint32_t, uint32_t, uint32_t);
static void klfb_lz_FramebufferRenderbuffer(uint32_t t, uint32_t a, uint32_t rt, uint32_t rb) { KLFB_LAZY_FLUSH(); klfb_att_forget(klfb_fb_for_target(t), a); g_lz_FramebufferRenderbuffer(t, a, rt, rb); }
static void (*g_lz_CopyTexImage2D)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
static void klfb_lz_CopyTexImage2D(uint32_t t, int32_t l, uint32_t f, int32_t x, int32_t y, int32_t w, int32_t h, int32_t b) { KLFB_LAZY_FLUSH(); g_lz_CopyTexImage2D(t, l, f, x, y, w, h, b); }
static void (*g_lz_CopyTexSubImage3D)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
static void klfb_lz_CopyTexSubImage3D(uint32_t t, int32_t l, int32_t xo, int32_t yo, int32_t zo, int32_t x, int32_t y, int32_t w, int32_t h) { KLFB_LAZY_FLUSH(); g_lz_CopyTexSubImage3D(t, l, xo, yo, zo, x, y, w, h); }
static void (*g_lz_GetFramebufferAttachmentParameteriv)(uint32_t, uint32_t, uint32_t, int32_t *);
static void klfb_lz_GetFramebufferAttachmentParameteriv(uint32_t t, uint32_t a, uint32_t pn, int32_t *v) { KLFB_LAZY_FLUSH(); g_lz_GetFramebufferAttachmentParameteriv(t, a, pn, v); }
static void (*g_lz_DrawElementsInstancedBaseVertex)(uint32_t, int32_t, uint32_t, const void *, int32_t, int32_t);
static void klfb_lz_DrawElementsInstancedBaseVertex(uint32_t m, int32_t c, uint32_t ty, const void *i, int32_t n, int32_t bv) { KLFB_LAZY_FLUSH(); g_lz_DrawElementsInstancedBaseVertex(m, c, ty, i, n, bv); }
static void (*g_lz_DrawArraysIndirect)(uint32_t, const void *);
static void klfb_lz_DrawArraysIndirect(uint32_t m, const void *i) { KLFB_LAZY_FLUSH(); g_lz_DrawArraysIndirect(m, i); }
static void (*g_lz_DrawElementsIndirect)(uint32_t, uint32_t, const void *);
static void klfb_lz_DrawElementsIndirect(uint32_t m, uint32_t t, const void *i) { KLFB_LAZY_FLUSH(); g_lz_DrawElementsIndirect(m, t, i); }
// ...and the suffixed and multi-draw spellings ANGLE exports and ltw resolves
// (ltw_darwin_gl.c routes Sodium's glMultiDrawElementsBaseVertex through
// glDrawElementsBaseVertexEXT on the device). Anything binding-dependent that
// still has no thunk here switches lazy unbind OFF for the run when the guest
// resolves it (kl_glfb_sym, klfb_lazy_unsafe_name) — the set is closed by
// construction, not by this list being complete.
#define KLFB_LZV(name, params, args) \
    static void (*g_lz_##name) params; \
    static void klfb_lz_##name params { KLFB_LAZY_FLUSH(); g_lz_##name args; }
KLFB_LZV(DrawElementsBaseVertexEXT, (uint32_t m, int32_t c, uint32_t t, const void *i, int32_t bv), (m, c, t, i, bv))
KLFB_LZV(DrawElementsBaseVertexOES, (uint32_t m, int32_t c, uint32_t t, const void *i, int32_t bv), (m, c, t, i, bv))
KLFB_LZV(DrawRangeElementsBaseVertexEXT, (uint32_t m, uint32_t s, uint32_t e, int32_t c, uint32_t t, const void *i, int32_t bv), (m, s, e, c, t, i, bv))
KLFB_LZV(DrawRangeElementsBaseVertexOES, (uint32_t m, uint32_t s, uint32_t e, int32_t c, uint32_t t, const void *i, int32_t bv), (m, s, e, c, t, i, bv))
KLFB_LZV(DrawElementsInstancedBaseVertexEXT, (uint32_t m, int32_t c, uint32_t t, const void *i, int32_t n, int32_t bv), (m, c, t, i, n, bv))
KLFB_LZV(DrawElementsInstancedBaseVertexOES, (uint32_t m, int32_t c, uint32_t t, const void *i, int32_t n, int32_t bv), (m, c, t, i, n, bv))
KLFB_LZV(DrawArraysInstancedBaseInstanceANGLE, (uint32_t m, int32_t f, int32_t c, int32_t n, uint32_t bi), (m, f, c, n, bi))
KLFB_LZV(DrawArraysInstancedBaseInstanceEXT, (uint32_t m, int32_t f, int32_t c, int32_t n, uint32_t bi), (m, f, c, n, bi))
KLFB_LZV(DrawElementsInstancedBaseInstanceEXT, (uint32_t m, int32_t c, uint32_t t, const void *i, int32_t n, uint32_t bi), (m, c, t, i, n, bi))
KLFB_LZV(DrawElementsInstancedBaseVertexBaseInstanceANGLE, (uint32_t m, int32_t c, uint32_t t, const void *i, int32_t n, int32_t bv, uint32_t bi), (m, c, t, i, n, bv, bi))
KLFB_LZV(DrawElementsInstancedBaseVertexBaseInstanceEXT, (uint32_t m, int32_t c, uint32_t t, const void *i, int32_t n, int32_t bv, uint32_t bi), (m, c, t, i, n, bv, bi))
KLFB_LZV(DrawArraysInstancedANGLE, (uint32_t m, int32_t f, int32_t c, int32_t n), (m, f, c, n))
KLFB_LZV(DrawArraysInstancedEXT, (uint32_t m, int32_t f, int32_t c, int32_t n), (m, f, c, n))
KLFB_LZV(DrawElementsInstancedANGLE, (uint32_t m, int32_t c, uint32_t t, const void *i, int32_t n), (m, c, t, i, n))
KLFB_LZV(DrawElementsInstancedEXT, (uint32_t m, int32_t c, uint32_t t, const void *i, int32_t n), (m, c, t, i, n))
KLFB_LZV(MultiDrawArraysEXT, (uint32_t m, const int32_t *f, const int32_t *c, int32_t n), (m, f, c, n))
KLFB_LZV(MultiDrawElementsEXT, (uint32_t m, const int32_t *c, uint32_t t, const void *const *i, int32_t n), (m, c, t, i, n))
KLFB_LZV(MultiDrawElementsBaseVertexEXT, (uint32_t m, const int32_t *c, uint32_t t, const void *const *i, int32_t n, const int32_t *bv), (m, c, t, i, n, bv))
KLFB_LZV(MultiDrawArraysANGLE, (uint32_t m, const int32_t *f, const int32_t *c, int32_t n), (m, f, c, n))
KLFB_LZV(MultiDrawElementsANGLE, (uint32_t m, const int32_t *c, uint32_t t, const void *const *i, int32_t n), (m, c, t, i, n))
KLFB_LZV(MultiDrawArraysIndirectEXT, (uint32_t m, const void *ind, int32_t n, int32_t st), (m, ind, n, st))
KLFB_LZV(MultiDrawElementsIndirectEXT, (uint32_t m, uint32_t t, const void *ind, int32_t n, int32_t st), (m, t, ind, n, st))
KLFB_LZV(DrawBuffersEXT, (int32_t n, const uint32_t *b), (n, b))
KLFB_LZV(ReadnPixels, (int32_t x, int32_t y, int32_t w, int32_t h, uint32_t f, uint32_t t, int32_t bs, void *d), (x, y, w, h, f, t, bs, d))
KLFB_LZV(ReadnPixelsEXT, (int32_t x, int32_t y, int32_t w, int32_t h, uint32_t f, uint32_t t, int32_t bs, void *d), (x, y, w, h, f, t, bs, d))
KLFB_LZV(ReadnPixelsKHR, (int32_t x, int32_t y, int32_t w, int32_t h, uint32_t f, uint32_t t, int32_t bs, void *d), (x, y, w, h, f, t, bs, d))
KLFB_LZV(GetInteger64i_v, (uint32_t t, uint32_t i, int64_t *d), (t, i, d))
#undef KLFB_LZV
// glFramebufferTexture{,EXT,OES}: an attach — flush AND drop the ledger entry.
#define KLFB_LZFT(name) \
    static void (*g_lz_##name)(uint32_t, uint32_t, uint32_t, int32_t); \
    static void klfb_lz_##name(uint32_t t, uint32_t a, uint32_t tex, int32_t l) { \
        KLFB_LAZY_FLUSH(); klfb_att_forget(klfb_fb_for_target(t), a); g_lz_##name(t, a, tex, l); }
KLFB_LZFT(FramebufferTexture)
KLFB_LZFT(FramebufferTextureEXT)
KLFB_LZFT(FramebufferTextureOES)
#undef KLFB_LZFT


static const struct { const char *name; void *thunk; void **real; } g_thunks[] = {
    {"glActiveTexture", (void *)klfb_ActiveTexture, (void **)&g_real_ActiveTexture},
    {"glUseProgram",  (void *)klfb_UseProgram,  (void **)&g_real_UseProgram},
    {"glBindTexture", (void *)klfb_BindTexture, (void **)&g_real_BindTexture},
    // texture buffers, served as a 2-D copy (see klfb_texbuf_copy)
    // the indexed per-draw-buffer state family, served for draw buffer 0
#define KLFB_IDX(fn, real) {"gl" #fn, (void *)klfb_##fn, (void **)&(real)}
    {"glColorMaski", (void *)klfb_ColorMaski, (void **)&g_real_ColorMaski_},
    {"glColorMaskiEXT", (void *)klfb_ColorMaski, (void **)&g_real_ColorMaskiEXT_},
    {"glEnablei", (void *)klfb_Enablei, (void **)&g_real_Enablei_},
    {"glEnableiEXT", (void *)klfb_Enablei, (void **)&g_real_EnableiEXT_},
    {"glDisablei", (void *)klfb_Disablei, (void **)&g_real_Disablei_},
    {"glDisableiEXT", (void *)klfb_Disablei, (void **)&g_real_DisableiEXT_},
    {"glBlendFuncSeparatei", (void *)klfb_BlendFuncSeparatei, (void **)&g_real_BlendFuncSeparatei_},
    {"glBlendFuncSeparateiEXT", (void *)klfb_BlendFuncSeparatei, (void **)&g_real_BlendFuncSeparateiEXT_},
    {"glBlendFunci", (void *)klfb_BlendFunci, (void **)&g_real_BlendFunci_},
    {"glBlendFunciEXT", (void *)klfb_BlendFunci, (void **)&g_real_BlendFunciEXT_},
    {"glBlendEquationSeparatei", (void *)klfb_BlendEquationSeparatei, (void **)&g_real_BlendEquationSeparatei_},
    {"glBlendEquationSeparateiEXT", (void *)klfb_BlendEquationSeparatei, (void **)&g_real_BlendEquationSeparateiEXT_},
    {"glBlendEquationi", (void *)klfb_BlendEquationi, (void **)&g_real_BlendEquationi_},
    {"glBlendEquationiEXT", (void *)klfb_BlendEquationi, (void **)&g_real_BlendEquationiEXT_},
#undef KLFB_IDX
    {"glTexBuffer", (void *)klfb_TexBuffer, &g_real_TexBuffer},
    {"glTexBufferEXT", (void *)klfb_TexBuffer, &g_real_TexBufferEXT},
    {"glTexBufferOES", (void *)klfb_TexBuffer, &g_real_TexBufferOES},
    {"glTexBufferRange", (void *)klfb_TexBufferRange, &g_real_TexBufferRange},
    {"glTexBufferRangeEXT", (void *)klfb_TexBufferRange, &g_real_TexBufferRangeEXT},
    {"glTexBufferRangeOES", (void *)klfb_TexBufferRange, &g_real_TexBufferRangeOES},
    {"glEGLImageTargetTexture2DOES", (void *)klfb_EGLImageTargetTexture2DOES,
                                     (void **)&g_real_EGLImageTargetTexture2DOES},
    {"glBindSampler", (void *)klfb_BindSampler, (void **)&g_real_BindSampler},
    {"glGetUniformLocation", (void *)klfb_GetUniformLocation, (void **)&g_real_GetUniformLocation},
    // the uniform family — every one of them, so an explicitly pinned location
    // is translated to the linker's own (see klfb_strip_uniform_layout)
    {"glUniform1i",   (void *)klfb_Uniform1i,   (void **)&g_real_Uniform1i},
    {"glUniform1f",   (void *)klfb_Uniform1f,   (void **)&g_real_Uniform1f},
#define KLFB_UT(fn) {"gl" #fn, (void *)klfb_##fn, (void **)&g_real_##fn}
    KLFB_UT(Uniform2f), KLFB_UT(Uniform3f), KLFB_UT(Uniform4f),
    KLFB_UT(Uniform2i), KLFB_UT(Uniform3i), KLFB_UT(Uniform4i),
    KLFB_UT(Uniform1ui), KLFB_UT(Uniform2ui), KLFB_UT(Uniform3ui), KLFB_UT(Uniform4ui),
    KLFB_UT(Uniform1fv), KLFB_UT(Uniform2fv), KLFB_UT(Uniform3fv), KLFB_UT(Uniform4fv),
    KLFB_UT(Uniform1iv), KLFB_UT(Uniform2iv), KLFB_UT(Uniform3iv), KLFB_UT(Uniform4iv),
    KLFB_UT(Uniform1uiv), KLFB_UT(Uniform2uiv), KLFB_UT(Uniform3uiv), KLFB_UT(Uniform4uiv),
    KLFB_UT(UniformMatrix2fv), KLFB_UT(UniformMatrix3fv), KLFB_UT(UniformMatrix4fv),
    KLFB_UT(UniformMatrix2x3fv), KLFB_UT(UniformMatrix3x2fv),
    KLFB_UT(UniformMatrix2x4fv), KLFB_UT(UniformMatrix4x2fv),
    KLFB_UT(UniformMatrix3x4fv), KLFB_UT(UniformMatrix4x3fv),
#undef KLFB_UT
    {"glGenerateMipmap", (void *)klfb_GenerateMipmap, (void **)&g_real_GenerateMipmap},
    // GL_FRAMEBUFFER_SRGB only — every other cap falls straight through
    {"glEnable",  (void *)klfb_Enable,  (void **)&g_real_EnableCap},
    {"glDisable", (void *)klfb_Disable, (void **)&g_real_DisableCap},
    {"glFlush",  (void *)klfb_Flush,  (void **)&g_real_Flush},
    {"glDrawElements", (void *)klfb_DrawElements, (void **)&g_real_DrawElements},
    {"glDrawArrays",   (void *)klfb_DrawArrays,   (void **)&g_real_DrawArrays},
    {"glDrawElementsInstanced", (void *)klfb_DrawElementsInstanced, (void **)&g_real_DrawElementsInstanced},
    {"glDrawElementsBaseVertex", (void *)klfb_DrawElementsBaseVertex, (void **)&g_real_DrawElementsBaseVertex},
    {"glDrawRangeElements", (void *)klfb_DrawRangeElements, (void **)&g_real_DrawRangeElements},
    {"glDrawRangeElementsBaseVertex", (void *)klfb_DrawRangeElementsBaseVertex, (void **)&g_real_DrawRangeElementsBaseVertex},
    {"glDrawArraysInstanced", (void *)klfb_DrawArraysInstanced, (void **)&g_real_DrawArraysInstanced},
    {"glFinish", (void *)klfb_Finish, (void **)&g_real_Finish},
    {"glBlitFramebuffer",         (void *)klfb_BlitFramebuffer,         (void **)&g_real_BlitFramebuffer},
    {"glInvalidateFramebuffer",   (void *)klfb_InvalidateFramebuffer,   (void **)&g_real_InvalidateFramebuffer},
    // r32 — the render-target supersample's coordinate half. All five forward
    // their arguments unchanged unless KL_GLFB_RT_SCALE named a size that was
    // actually allocated; see the block above klfb_Viewport.
    {"glViewport",                (void *)klfb_Viewport,                (void **)&g_real_Viewport},
    {"glScissor",                 (void *)klfb_Scissor,                 (void **)&g_real_Scissor},
    {"glReadPixels",              (void *)klfb_ReadPixels,              (void **)&g_real_ReadPixels},
    {"glCopyTexSubImage2D",       (void *)klfb_CopyTexSubImage2D,       (void **)&g_real_CopyTexSubImage2D},
    {"glInvalidateSubFramebuffer",(void *)klfb_InvalidateSubFramebuffer,(void **)&g_real_InvalidateSubFramebuffer},
    {"glClear",                   (void *)klfb_Clear,                   (void **)&g_real_Clear},
    {"glClearColor",              (void *)klfb_ClearColor,              (void **)&g_real_ClearColor},
    {"glRenderbufferStorageMultisample", (void *)klfb_RenderbufferStorageMultisample, (void **)&g_real_RenderbufferStorageMultisample},
    {"glRenderbufferStorage",     (void *)klfb_RenderbufferStorage,     (void **)&g_real_RenderbufferStorage},
    {"glTexSubImage3D",           (void *)klfb_TexSubImage3D,           (void **)&g_real_TexSubImage3D},
    {"glCompressedTexSubImage2D", (void *)klfb_CompressedTexSubImage2D, (void **)&g_real_CompressedTexSubImage2D},
    {"glCompressedTexSubImage3D", (void *)klfb_CompressedTexSubImage3D, (void **)&g_real_CompressedTexSubImage3D},
    // the shared-context compile lock — see the block above s10's numbers
    {"glShaderSource", (void *)klfb_ShaderSource, (void **)&g_real_ShaderSource},
    {"glCompileShader", (void *)klfb_CompileShader, (void **)&g_real_CompileShader},
    {"glLinkProgram",   (void *)klfb_LinkProgram,   (void **)&g_real_LinkProgram},
    // the other way a program gets linked, and the only one that was silent
    {"glProgramBinary", (void *)klfb_ProgramBinary, (void **)&g_real_ProgramBinary},
    // the texture storage census — which format the AGX abort follows
    {"glTexStorage2D", (void *)klfb_TexStorage2D, (void **)&g_real_TexStorage2D},
    {"glTexStorage3D", (void *)klfb_TexStorage3D, (void **)&g_real_TexStorage3D},
    {"glTexSubImage2D", (void *)klfb_TexSubImage2D, (void **)&g_real_TexSubImage2D},
    {"glTexParameteri", (void *)klfb_TexParameteri, (void **)&g_real_TexParameteri},
    // census only — the sampler-LOD counters (r21); all three forward verbatim
    {"glTexParameterf", (void *)klfb_TexParameterf, (void **)&g_real_TexParameterf},
    {"glSamplerParameteri", (void *)klfb_SamplerParameteri, (void **)&g_real_SamplerParameteri},
    {"glSamplerParameterf", (void *)klfb_SamplerParameterf, (void **)&g_real_SamplerParameterf},
    {"glTexParameterfv", (void *)klfb_TexParameterfv, (void **)&g_real_TexParameterfv},
    {"glGenFramebuffers", (void *)klfb_GenFramebuffers, (void **)&g_real_GenFramebuffers},
    {"glBindFramebuffer", (void *)klfb_BindFramebuffer, (void **)&g_real_BindFramebuffer},
    // the object census — every gen/delete pair, so a class that never shrinks
    // names itself (KL_GL_CENSUS)
    {"glDeleteFramebuffers", (void *)klfb_DeleteFramebuffers, (void **)&g_real_DeleteFramebuffers},
    {"glGenTextures",     (void *)klfb_GenTextures,     (void **)&g_real_GenTextures},
    {"glDeleteTextures",  (void *)klfb_DeleteTextures,  (void **)&g_real_DeleteTextures},
    {"glGenRenderbuffers",    (void *)klfb_GenRenderbuffers,    (void **)&g_real_GenRenderbuffers},
    {"glDeleteRenderbuffers", (void *)klfb_DeleteRenderbuffers, (void **)&g_real_DeleteRenderbuffers},
    {"glGenBuffers",      (void *)klfb_GenBuffers,      (void **)&g_real_GenBuffers},
    {"glDeleteBuffers",   (void *)klfb_DeleteBuffers,   (void **)&g_real_DeleteBuffers},
    // the vertex-array setup family — the ATTRIB_TRACE instrument
    {"glBindVertexArray", (void *)klfb_BindVertexArray, (void **)&g_real_BindVertexArray},
    {"glBindVertexArrayOES", (void *)klfb_BindVertexArrayOES, (void **)&g_real_BindVertexArrayOES},
    // the ES 3.1 attrib-binding family, translated to ES 3.0 pointer calls
    {"glVertexAttribFormat", (void *)klfb_VertexAttribFormat, (void **)&g_real_VertexAttribFormat},
    {"glVertexAttribIFormat", (void *)klfb_VertexAttribIFormat, (void **)&g_real_VertexAttribIFormat},
    {"glVertexAttribBinding", (void *)klfb_VertexAttribBinding, (void **)&g_real_VertexAttribBinding},
    {"glBindVertexBuffer", (void *)klfb_BindVertexBuffer, (void **)&g_real_BindVertexBuffer},
    {"glVertexBindingDivisor", (void *)klfb_VertexBindingDivisor, (void **)&g_real_VertexBindingDivisor},

    {"glVertexAttribPointer", (void *)klfb_VertexAttribPointer, (void **)&g_real_VertexAttribPointer},
    {"glEnableVertexAttribArray", (void *)klfb_EnableVertexAttribArray, (void **)&g_real_EnableVertexAttribArray},
    {"glDisableVertexAttribArray", (void *)klfb_DisableVertexAttribArray, (void **)&g_real_DisableVertexAttribArray},
    {"glBindBuffer", (void *)klfb_BindBufferGL, (void **)&g_real_BindBufferGL},
    {"glBufferData", (void *)klfb_BufferData, (void **)&g_real_BufferData},
    {"glGenVertexArrays", (void *)klfb_GenVertexArrays, (void **)&g_real_GenVertexArrays},
    {"glDeleteVertexArrays", (void *)klfb_DeleteVertexArrays, (void **)&g_real_DeleteVertexArrays},
    {"glCreateShader",  (void *)klfb_CreateShader,  (void **)&g_real_CreateShader},
    {"glDeleteShader",  (void *)klfb_DeleteShader,  (void **)&g_real_DeleteShader},
    {"glCreateProgram", (void *)klfb_CreateProgram, (void **)&g_real_CreateProgram},
    {"glDeleteProgram", (void *)klfb_DeleteProgram, (void **)&g_real_DeleteProgram},
    {"glTexImage2D",    (void *)klfb_TexImage2D,    (void **)&g_real_TexImage2D},
    {"glFramebufferTexture2D", (void *)klfb_FramebufferTexture2D, (void **)&g_real_FramebufferTexture2D},
    {"glFramebufferTextureLayer", (void *)klfb_FramebufferTextureLayer, (void **)&g_real_FramebufferTextureLayer},
    // KL_GLFB_LAZY_UNBIND flush points (pass-throughs)
    {"glDrawBuffers", (void *)klfb_lz_DrawBuffers, (void **)&g_lz_DrawBuffers},
    {"glReadBuffer", (void *)klfb_lz_ReadBuffer, (void **)&g_lz_ReadBuffer},
    {"glCheckFramebufferStatus", (void *)klfb_lz_CheckFramebufferStatus, (void **)&g_lz_CheckFramebufferStatus},
    {"glClearBufferfv", (void *)klfb_lz_ClearBufferfv, (void **)&g_lz_ClearBufferfv},
    {"glClearBufferiv", (void *)klfb_lz_ClearBufferiv, (void **)&g_lz_ClearBufferiv},
    {"glClearBufferuiv", (void *)klfb_lz_ClearBufferuiv, (void **)&g_lz_ClearBufferuiv},
    {"glClearBufferfi", (void *)klfb_lz_ClearBufferfi, (void **)&g_lz_ClearBufferfi},
    {"glFramebufferRenderbuffer", (void *)klfb_lz_FramebufferRenderbuffer, (void **)&g_lz_FramebufferRenderbuffer},
    {"glCopyTexImage2D", (void *)klfb_lz_CopyTexImage2D, (void **)&g_lz_CopyTexImage2D},
    {"glCopyTexSubImage3D", (void *)klfb_lz_CopyTexSubImage3D, (void **)&g_lz_CopyTexSubImage3D},
    {"glGetFramebufferAttachmentParameteriv", (void *)klfb_lz_GetFramebufferAttachmentParameteriv, (void **)&g_lz_GetFramebufferAttachmentParameteriv},
    {"glDrawElementsInstancedBaseVertex", (void *)klfb_lz_DrawElementsInstancedBaseVertex, (void **)&g_lz_DrawElementsInstancedBaseVertex},
    {"glDrawArraysIndirect", (void *)klfb_lz_DrawArraysIndirect, (void **)&g_lz_DrawArraysIndirect},
    {"glDrawElementsIndirect", (void *)klfb_lz_DrawElementsIndirect, (void **)&g_lz_DrawElementsIndirect},
    {"glDrawElementsBaseVertexEXT", (void *)klfb_lz_DrawElementsBaseVertexEXT, (void **)&g_lz_DrawElementsBaseVertexEXT},
    {"glDrawElementsBaseVertexOES", (void *)klfb_lz_DrawElementsBaseVertexOES, (void **)&g_lz_DrawElementsBaseVertexOES},
    {"glDrawRangeElementsBaseVertexEXT", (void *)klfb_lz_DrawRangeElementsBaseVertexEXT, (void **)&g_lz_DrawRangeElementsBaseVertexEXT},
    {"glDrawRangeElementsBaseVertexOES", (void *)klfb_lz_DrawRangeElementsBaseVertexOES, (void **)&g_lz_DrawRangeElementsBaseVertexOES},
    {"glDrawElementsInstancedBaseVertexEXT", (void *)klfb_lz_DrawElementsInstancedBaseVertexEXT, (void **)&g_lz_DrawElementsInstancedBaseVertexEXT},
    {"glDrawElementsInstancedBaseVertexOES", (void *)klfb_lz_DrawElementsInstancedBaseVertexOES, (void **)&g_lz_DrawElementsInstancedBaseVertexOES},
    {"glDrawArraysInstancedBaseInstanceANGLE", (void *)klfb_lz_DrawArraysInstancedBaseInstanceANGLE, (void **)&g_lz_DrawArraysInstancedBaseInstanceANGLE},
    {"glDrawArraysInstancedBaseInstanceEXT", (void *)klfb_lz_DrawArraysInstancedBaseInstanceEXT, (void **)&g_lz_DrawArraysInstancedBaseInstanceEXT},
    {"glDrawElementsInstancedBaseInstanceEXT", (void *)klfb_lz_DrawElementsInstancedBaseInstanceEXT, (void **)&g_lz_DrawElementsInstancedBaseInstanceEXT},
    {"glDrawElementsInstancedBaseVertexBaseInstanceANGLE", (void *)klfb_lz_DrawElementsInstancedBaseVertexBaseInstanceANGLE, (void **)&g_lz_DrawElementsInstancedBaseVertexBaseInstanceANGLE},
    {"glDrawElementsInstancedBaseVertexBaseInstanceEXT", (void *)klfb_lz_DrawElementsInstancedBaseVertexBaseInstanceEXT, (void **)&g_lz_DrawElementsInstancedBaseVertexBaseInstanceEXT},
    {"glDrawArraysInstancedANGLE", (void *)klfb_lz_DrawArraysInstancedANGLE, (void **)&g_lz_DrawArraysInstancedANGLE},
    {"glDrawArraysInstancedEXT", (void *)klfb_lz_DrawArraysInstancedEXT, (void **)&g_lz_DrawArraysInstancedEXT},
    {"glDrawElementsInstancedANGLE", (void *)klfb_lz_DrawElementsInstancedANGLE, (void **)&g_lz_DrawElementsInstancedANGLE},
    {"glDrawElementsInstancedEXT", (void *)klfb_lz_DrawElementsInstancedEXT, (void **)&g_lz_DrawElementsInstancedEXT},
    {"glMultiDrawArraysEXT", (void *)klfb_lz_MultiDrawArraysEXT, (void **)&g_lz_MultiDrawArraysEXT},
    {"glMultiDrawElementsEXT", (void *)klfb_lz_MultiDrawElementsEXT, (void **)&g_lz_MultiDrawElementsEXT},
    {"glMultiDrawElementsBaseVertexEXT", (void *)klfb_lz_MultiDrawElementsBaseVertexEXT, (void **)&g_lz_MultiDrawElementsBaseVertexEXT},
    {"glMultiDrawArraysANGLE", (void *)klfb_lz_MultiDrawArraysANGLE, (void **)&g_lz_MultiDrawArraysANGLE},
    {"glMultiDrawElementsANGLE", (void *)klfb_lz_MultiDrawElementsANGLE, (void **)&g_lz_MultiDrawElementsANGLE},
    {"glMultiDrawArraysIndirectEXT", (void *)klfb_lz_MultiDrawArraysIndirectEXT, (void **)&g_lz_MultiDrawArraysIndirectEXT},
    {"glMultiDrawElementsIndirectEXT", (void *)klfb_lz_MultiDrawElementsIndirectEXT, (void **)&g_lz_MultiDrawElementsIndirectEXT},
    {"glDrawBuffersEXT", (void *)klfb_lz_DrawBuffersEXT, (void **)&g_lz_DrawBuffersEXT},
    {"glReadnPixels", (void *)klfb_lz_ReadnPixels, (void **)&g_lz_ReadnPixels},
    {"glReadnPixelsEXT", (void *)klfb_lz_ReadnPixelsEXT, (void **)&g_lz_ReadnPixelsEXT},
    {"glReadnPixelsKHR", (void *)klfb_lz_ReadnPixelsKHR, (void **)&g_lz_ReadnPixelsKHR},
    {"glGetInteger64i_v", (void *)klfb_lz_GetInteger64i_v, (void **)&g_lz_GetInteger64i_v},
    {"glFramebufferTexture", (void *)klfb_lz_FramebufferTexture, (void **)&g_lz_FramebufferTexture},
    {"glFramebufferTextureEXT", (void *)klfb_lz_FramebufferTextureEXT, (void **)&g_lz_FramebufferTextureEXT},
    {"glFramebufferTextureOES", (void *)klfb_lz_FramebufferTextureOES, (void **)&g_lz_FramebufferTextureOES},
    // the hybrid query family — ours if the capability tables describe the
    // pname, ANGLE's otherwise (see the gateway comment below)
    {"glGetIntegerv", (void *)klfb_GetIntegerv, (void **)&g_real_GetIntegerv},
    {"glGetFloatv",   (void *)klfb_GetFloatv,   (void **)&g_real_GetFloatv},
    {"glGetBooleanv", (void *)klfb_GetBooleanv, (void **)&g_real_GetBooleanv},
    {"glGetInteger64v", (void *)klfb_GetInteger64v, (void **)&g_real_GetInteger64v},
    {"glGetIntegeri_v", (void *)klfb_GetIntegeri_v, (void **)&g_real_GetIntegeri_v},
    {"glGetInternalformativ", (void *)klfb_GetInternalformativ, (void **)&g_real_GetInternalformativ},
    // the ES 3.1 program-interface family — translated to ES 3.0 introspection
    // in the block above; the real pointers self-resolve via asym, so the
    // table's real slot is a sink
    {"glGetProgramInterfaceiv", (void *)klfb_GetProgramInterfaceiv, (void **)&g_res_sink},
    {"glGetProgramResourceiv",  (void *)klfb_GetProgramResourceiv,  (void **)&g_res_sink},
    {"glGetProgramResourceName",(void *)klfb_GetProgramResourceName,(void **)&g_res_sink},
    {"glGetProgramResourceIndex",(void *)klfb_GetProgramResourceIndex, (void **)&g_res_sink},
    {"glGetProgramResourceLocation",(void *)klfb_GetProgramResourceLocation, (void **)&g_res_sink},
    {"glGetProgramResourceLocationIndex",(void *)klfb_GetProgramResourceLocationIndex, (void **)&g_res_sink},
};

// ---------------------------------------------------------- the gateway
//
// Capability answers stay ours. kl_egl.c tells the guest it is driving a GLES 3.2
// device with no extensions, and Unity built its renderer against that answer;
// letting ANGLE answer instead would change the description underneath a decision
// already made. Everything operational goes to ANGLE.
//
// The query family is the hybrid: the tables answer the pnames they describe,
// and anything else is dynamic *state* (READ_BUFFER, bindings, viewport) that
// only ANGLE tracks. Answering those with the tables' 0 was not neutral: a 0
// for GL_READ_BUFFER became GL_INVALID_ENUM one glReadBuffer later, and the
// eye blit died GL_INVALID_FRAMEBUFFER_OPERATION every frame.
// The query family is the hybrid, but the split is not "table vs rest":
// ANGLE's context here is ES 3.0 while we describe 3.2 to the guest, so a
// capability pname the table does not list (the SSBO family, 0x90d6..) must
// NOT go to ANGLE — there it is a real GL_INVALID_ENUM, and the null driver's
// named zero is exactly what keeps Unity off those paths. Only genuine
// dynamic *state* — Unity's save/restore set around its blits — belongs to
// ANGLE, which alone tracks it. The proof was 0x0C02 (READ_BUFFER): our 0
// became GL_INVALID_ENUM one glReadBuffer later, and the eye blit died
// GL_INVALID_FRAMEBUFFER_OPERATION every frame.
static int glfb_state_pname(uint32_t p) {
    switch (p) {
    case 0x0C01: case 0x0C02:   // DRAW_BUFFER, READ_BUFFER
    case 0x8CA6: case 0x8CAA:   // DRAW/READ_FRAMEBUFFER_BINDING
    case 0x8B8D:                // CURRENT_PROGRAM
    case 0x84E0:                // ACTIVE_TEXTURE
    case 0x0BA2: case 0x0C10:   // VIEWPORT, SCISSOR_BOX
    // Session 05 (simpsons, round 35): the rest of the save/restore set a
    // guest brackets a pass with. SHAR's VR HUD reads BLEND_SRC/DST_RGB before
    // its iris-blackout quad and restores them after, every eye, every frame
    // (openxrmanager.cpp ApplyIrisBlackout); our 0 is GL_ZERO, a VALID
    // factor, so the restore became glBlendFunc(GL_ZERO, GL_ZERO) and the
    // whole HUD capture that followed blended to black — the black square on
    // the hand and dashboard where the minimap should be. The bindings and
    // the depth/cull enums are the same family (a 0 there is at worst an
    // ignored INVALID_ENUM, but a real answer is what the guest wrote).
    case 0x80C8: case 0x80C9:   // BLEND_DST_RGB, BLEND_SRC_RGB
    case 0x80CA: case 0x80CB:   // BLEND_DST_ALPHA, BLEND_SRC_ALPHA
    case 0x0BE0: case 0x0BE1:   // BLEND_DST, BLEND_SRC (ES 1.x names, same state)
    case 0x8009: case 0x883D:   // BLEND_EQUATION_RGB, BLEND_EQUATION_ALPHA
    case 0x8894: case 0x8895:   // ARRAY_BUFFER_BINDING, ELEMENT_ARRAY_BUFFER_BINDING
    case 0x85B5:                // VERTEX_ARRAY_BINDING
    case 0x8069: case 0x8514:   // TEXTURE_BINDING_2D, TEXTURE_BINDING_CUBE_MAP
    case 0x8CA7:                // RENDERBUFFER_BINDING
    case 0x0B74: case 0x0B45:   // DEPTH_FUNC, CULL_FACE_MODE
    case 0x0B71: case 0x0BE2:   // DEPTH_TEST, BLEND (as integers)
        return 1;
    }
    return 0;
}

// The one place the advertised texture-unit caps meet the driver that will
// enforce them. Everything else in the description is ours to assert; these
// three are not, because Unity sizes its texture-unit cache from what we say
// and then indexes it by unit with no bound of its own — advertise one unit
// more than ANGLE accepts and the surplus binds come back GL_INVALID_VALUE
// while Unity writes past the end of its own array anyway. That is the Beat
// Saber 1.40 song-load SIGBUS.
//
// So ask, once, and adopt. Done lazily from the query path rather than at
// bring-up because that is the first moment the real context is provably
// current — the guest is standing right here asking a question — and it runs
// before any answer of ours is returned. Two static reads afterwards.
static void klfb_adopt_texture_unit_caps(void) {
    static int done;
    if (done || !g_real_GetIntegerv) return;
    done = 1;
    int32_t per_stage = 0, combined = 0;
    g_real_GetIntegerv(0x8872 /* MAX_TEXTURE_IMAGE_UNITS          */, &per_stage);
    g_real_GetIntegerv(0x8B4D /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */, &combined);
    if (a_glGetError) while (a_glGetError()) {}
    kl_gl_cap_adopt_texture_units(per_stage, combined);
}

// The pnames that stop being ours the moment an extension is adopted from the
// backend. Each one is a question ABOUT that extension, so the driver that
// answered "I have it" is the only thing that can answer "and here is what it
// costs" — and our tables say 0 to all three, which for a guest reads as "the
// extension is there but useless" and turns the feature straight back off.
//
// GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT is the one that caught this: it is in no
// table of ours, so it reached the "unhandled pname (kept ours)" arm below,
// which answers 0 by contract — and libtogl reading a maximum anisotropy of 0
// clamps mat_forceaniso to 1, which is exactly the setting this round is
// trying to raise.
static int glfb_backend_ext_pname(uint32_t pname) {
    if (kl_gl_cap_adopted_extension_count() == 0) return 0;
    return pname == 0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY_EXT */
        || pname == 0x86A2 /* NUM_COMPRESSED_TEXTURE_FORMATS */
        || pname == 0x86A3 /* COMPRESSED_TEXTURE_FORMATS     */;
}

static void klfb_GetIntegerv(uint32_t pname, int32_t *params) {
    KLFB_LAZY_FLUSH();
    klfb_adopt_texture_unit_caps();
    if (glfb_state_pname(pname) || glfb_backend_ext_pname(pname)) {
        if (g_real_GetIntegerv) g_real_GetIntegerv(pname, params);
        // r32: the driver's viewport is the SCALED one while a supersampled
        // target is bound, and the guest never wrote that number. Hand back the
        // coordinate space the guest is working in, so a save/restore around a
        // pass cannot re-scale an already-scaled rectangle. Inert at factor 1.
        if (params && g_rt_draw_scale > 1 &&
            (pname == 0x0BA2 /* VIEWPORT */ || pname == 0x0C10 /* SCISSOR_BOX */))
            for (int i = 0; i < 4; i++) params[i] /= g_rt_draw_scale;
        return;
    }
    if (kl_gl_cap_integerv(pname, params)) return;
    // Same contract as the null driver: named, not guessed — and crucially
    // no GL error set, which forwarding to the ES 3.0 context would do.
    fprintf(stderr, "  [glfb] glGetIntegerv: unhandled pname 0x%x (kept ours)\n", pname);
    if (params) params[0] = 0;
}
static void klfb_GetFloatv(uint32_t pname, float *data) {
    KLFB_LAZY_FLUSH();
    if (kl_gl_cap_floatv(pname, data)) return;
    if (g_real_GetFloatv) g_real_GetFloatv(pname, data);
}
static void klfb_GetBooleanv(uint32_t pname, uint8_t *data) {
    KLFB_LAZY_FLUSH();
    if (g_real_GetBooleanv) g_real_GetBooleanv(pname, data);   // no table: all state
}
static void klfb_GetInteger64v(uint32_t pname, int64_t *data) {
    KLFB_LAZY_FLUSH();
    if (kl_gl_cap_integer64v(pname, data)) return;
    if (g_real_GetInteger64v) g_real_GetInteger64v(pname, data);
}
static void klfb_GetIntegeri_v(uint32_t target, uint32_t index, int32_t *data) {
    KLFB_LAZY_FLUSH();
    // Indexed *binding* state is dynamic, like the pnames above.
    switch (target) {
    case 0x8A28: case 0x8A29: case 0x8A2A:          // UNIFORM_BUFFER_*
    case 0x8C8C: case 0x8C8D: case 0x8C8F:          // TRANSFORM_FEEDBACK_BUFFER_*
    case 0x8E51:                                    // SAMPLE_MASK_VALUE
        if (g_real_GetIntegeri_v) { g_real_GetIntegeri_v(target, index, data); return; }
    }
    if (kl_gl_cap_integeri_v(target, index, data)) return;
    fprintf(stderr, "  [glfb] glGetIntegeri_v: unhandled target 0x%x (kept ours)\n", target);
    if (data) data[0] = 0;
}
static void klfb_GetInternalformativ(uint32_t target, uint32_t internalformat,
                                     uint32_t pname, int32_t bufSize, int32_t *params) {
    if (kl_gl_cap_internalformativ(target, internalformat, pname, bufSize, params)) return;
    if (g_real_GetInternalformativ)
        g_real_GetInternalformativ(target, internalformat, pname, bufSize, params);
}

static const char *const g_keep_ours[] = {
    "glGetString", "glGetStringi",
};

static int keep_ours(const char *name) {
    for (size_t i = 0; i < sizeof g_keep_ours / sizeof g_keep_ours[0]; i++)
        if (strcmp(g_keep_ours[i], name) == 0) return 1;
    return 0;
}

// KL_GLFB_SKIP=<comma-separated names> hands the listed entry points back to the
// caller, which means they fall through to the null driver and do nothing. That
// makes the ANGLE path *bisectable* by family without a rebuild: no-out the
// compressed uploads, or the blits, or the draws, and see which one the AGX abort
// follows. It is a diagnostic, not a mode — the resulting frame is wrong by
// construction, since whole classes of call have been dropped on the floor.
//
// This exists because under KL_GLFB the guest gets ANGLE's real function pointers
// and kl_egl's call counters never see them: the GL report says "called: 0" on a
// run that plainly called plenty. A per-name tracing trampoline is the other way
// to get that visibility and needs hand-written asm to preserve the arguments
// across the log call; this needed none.
static int glfb_skipped(const char *name) {
    const char *list = kl_env_str("KL_GLFB_SKIP", NULL);
    if (!list || !*list) return 0;
    size_t n = strlen(name);
    for (const char *p = list; *p; ) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == n && strncmp(p, name, n) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

// ------------------------------------------------------------- the self-test
//
// KL_GLFB_SELFTEST=1 runs a trivially correct piece of GL — create a sharing
// context, clear it, read it back — on a *host* thread inside the loaded guest
// process, right after init.
//
// The question it settles is which side is broken. Both ANGLE backends fail the
// same way in this process and neither fails in spikes/s10_shared.c: under Metal
// AGX cannot build a BlitComputeProgramVariant, and under ANGLE's OpenGL backend
// Apple's GLD asserts `clearFunction != 0` in getClearShaderFragmentFunction. Two
// different drivers, both failing to obtain one of their own *built-in* shaders.
// That is a strange thing for guest GL usage to cause and a very ordinary thing
// for a poisoned process to cause.
//
//   selftest aborts  -> the process is the problem: something the shim does
//                       globally (signal handlers, TSD, environment, fork state)
//                       breaks Apple's shader compilation for everyone in it, and
//                       the guest's GL is incidental.
//   selftest passes  -> the process is fine and the guest's own GL is doing
//                       something the drivers dislike; back to bisecting calls.
//
// A host thread, not a guest one, and plain pthread_create: no kl_thread_init, no
// TLS slot, no x18 veneers in the frame. This is deliberately the *most* ordinary
// GL any thread could ask for.
static void *klfb_selftest_thread(void *arg) {
    (void)arg;
    void *(*mkpb)(void *, void *, const int32_t *) = a_eglCreatePbufferSurface;
    void *(*mkctx)(void *, void *, void *, const int32_t *) = a_eglCreateContext;
    if (!mkpb || !mkctx || !a_eglMakeCurrent) return NULL;

    const int32_t surf_attrs[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
    const int32_t ctx_attrs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    void *surf = mkpb(g_dpy, g_cfg, surf_attrs);
    void *ctx  = mkctx(g_dpy, g_cfg, g_ctx, ctx_attrs);
    fprintf(stderr, "  [glfb] selftest: surf=%p ctx=%p\n", surf, ctx);
    if (!surf || !ctx) return NULL;
    if (!a_eglMakeCurrent(g_dpy, surf, surf, ctx)) {
        fprintf(stderr, "  [glfb] selftest: eglMakeCurrent failed\n");
        return NULL;
    }

    void (*clearcolor)(float, float, float, float) = asym("glClearColor");
    void (*clear)(uint32_t) = asym("glClear");
    if (clearcolor) clearcolor(0.25f, 0.5f, 0.75f, 1.0f);
    fprintf(stderr, "  [glfb] selftest: about to glClear (the driver builds its "
                    "internal clear shader here)\n");
    if (clear) clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
    if (a_glFinish) a_glFinish();
    fprintf(stderr, "  [glfb] selftest: glClear survived\n");

    unsigned char px[16] = {0};
    if (a_glReadPixels) a_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(stderr, "  [glfb] selftest: readback %u,%u,%u,%u — PASSED (the process "
                    "is fine; the guest's own GL is the difference)\n",
            px[0], px[1], px[2], px[3]);
    a_eglMakeCurrent(g_dpy, NULL, NULL, NULL);
    return NULL;
}

static void klfb_selftest(void) {
    if (!kl_env_on("KL_GLFB_SELFTEST", 0)) return;
    pthread_t th;
    if (pthread_create(&th, NULL, klfb_selftest_thread, NULL) == 0)
        pthread_join(th, NULL);
}

// ---------------------------------------------------------- the call trace
//
// KL_GLFB_TRACE=1 routes every entry point the guest resolves through a per-name
// forwarding stub, so the log names the exact GL call rather than the family a
// census can reach. The texture trace can only say a crash arrived somewhere
// after an R8 upload, and "somewhere after" spans every call the engine makes
// next.
//
// It is expensive by construction — a log line per GL call — so it is off unless
// asked for. KL_GLFB_TRACE_FROM=<n> starts printing only after the nth call, which
// is how a crash tens of thousands of calls in stays readable.
typedef struct { const char *name; void *real; } klfb_trace_desc;

extern void kl_gl_trace_tramp(void);       // runtime/gfx/kl_gl_trace.S
extern void kl_gl_lock_tramp(void);        // runtime/gfx/kl_gl_lock.S

// KL_GLFB_LOCK=1 wraps every resolved GL entry point in a process-wide lock,
// held ACROSS the call (kl_gl_lock.S). It tests whether the transient
// INVALID_FRAMEBUFFER_OPERATION under KL_GLFB_SHARED is Bug 1 — ANGLE's
// shared-context thread safety — at draw granularity: if the frame renders
// with this on, it is. It serialises all guest GL, so it is a probe, not a
// mode.
//
// The trampoline cannot keep the guest's return address in a register across
// the call (x30 dies at every bl, and any callee-saved register is the
// guest's own — clobbering x19 with it once presented as a SIGBUS in guest
// code, the guest writing through its own ra). It is parked here on a
// per-thread stack instead.
static pthread_mutex_t g_gl_lock = PTHREAD_MUTEX_INITIALIZER;
#define KLGL_LOCK_MAXDEPTH 8
static _Thread_local uint64_t g_ra_stack[KLGL_LOCK_MAXDEPTH];
static _Thread_local unsigned g_ra_depth;
void kl_gl_lock_acquire(uint64_t ra) {
    if (g_ra_depth < KLGL_LOCK_MAXDEPTH) g_ra_stack[g_ra_depth++] = ra;
    pthread_mutex_lock(&g_gl_lock);
}
uint64_t kl_gl_lock_release(void) {
    pthread_mutex_unlock(&g_gl_lock);
    return g_ra_depth ? g_ra_stack[--g_ra_depth] : 0;
}

static unsigned g_trace_calls;
static unsigned g_trace_from;

// Called from the trampoline with the argument registers already saved. Anything
// this touches must be safe on any guest thread, so it stays to stderr and a
// relaxed counter — no allocation, no locks worth deadlocking on.
// KL_GLFB_ERRSCAN=1: name the call that GENERATES a GL error, anywhere in the
// stream — not just the hand-wrapped few ERRSRC covers. The trampoline calls
// this BEFORE the real call, so a non-zero glGetError on entry to call N was
// produced by call N-1: record the previous name and blame it. This is what
// attributes Unity's "OPENGL NATIVE PLUG-IN ERROR" spam, which is only ever
// observed later, at a plugin-event boundary, long after the guilty call.
//
// Two things it changes by existing: it CLEARS the error flag, so the guest's
// own glGetError sees nothing and the Unity spam stops (that disappearance is
// itself confirmation the scan is seeing the same errors); and g_prev is a
// plain cross-thread global, so with several guest threads in GL the blame can
// land one call off. Diagnostic only.
// KL_GLFB_ERRSCAN=<code> reports only that GL error, so the known sticky
// INVALID_ENUM from TEXTURE_SRGB_DECODE_EXT (0x8a48, absent on ANGLE's ES 3.0)
// does not spend the whole report budget thousands of calls before the
// interesting one. =1 reports every code.
static const char *g_err_prev;
static uint32_t g_err_only;
static int getenv_trace_print(void);
static int glfb_errscan(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = kl_env_str("KL_GLFB_ERRSCAN", NULL);
        on = e != NULL;
        if (e) {
            uint32_t v = (uint32_t)strtoul(e, NULL, 0);
            if (v > 1) g_err_only = v;
        }
    }
    return on;
}

// GL_INVALID_FRAMEBUFFER_OPERATION says a command ran against a framebuffer GL
// considers incomplete, and names neither the framebuffer nor the reason. Both
// are askable, and without them the error is only ever "something, somewhere":
// VRChat's world load dies right after one of these, and which attachment is
// wrong is the whole question.
//
// Everything here is a QUERY — no state is set — and the error queue is drained
// afterwards so this cannot become the thing the next scan reports — an
// instrument must not perturb what it measures.
static const char *glfb_fb_status_name(uint32_t s) {
    switch (s) {
    case 0x8CD5: return "COMPLETE";
    case 0x8CD6: return "INCOMPLETE_ATTACHMENT";
    case 0x8CD7: return "INCOMPLETE_MISSING_ATTACHMENT";
    case 0x8CD9: return "INCOMPLETE_DIMENSIONS";
    case 0x8CDD: return "UNSUPPORTED";
    case 0x8D56: return "INCOMPLETE_MULTISAMPLE";
    case 0x8219: return "UNDEFINED";
    case 0x9317: return "INCOMPLETE_LAYER_TARGETS";
    default:     return "?";
    }
}

static void glfb_report_incomplete_fb(const char *what) {
    if (!a_glCheckFramebufferStatus || !a_glGetIntegerv) return;
    int32_t draw = 0, read = 0;
    a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &draw);
    a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &read);
    uint32_t st = a_glCheckFramebufferStatus(0x8CA9 /* DRAW_FRAMEBUFFER */);
    fprintf(stderr, "  [glfb] ...%s drew into draw_fb=%d (read_fb=%d): "
                    "status 0x%x %s\n",
            what, draw, read, st, glfb_fb_status_name(st));

    if (a_glGetFramebufferAttachmentParameteriv) {
        static const struct { uint32_t p; const char *n; } att[] = {
            { 0x8CE0, "COLOR0" }, { 0x8CE1, "COLOR1" },
            { 0x8D00, "DEPTH"  }, { 0x8D20, "STENCIL" },
        };
        for (size_t i = 0; i < sizeof att / sizeof att[0]; i++) {
            int32_t type = 0, obj = 0, layered = 0;
            a_glGetFramebufferAttachmentParameteriv(
                0x8CA9, att[i].p, 0x8CD0 /* OBJECT_TYPE */, &type);
            if (type == 0) continue;                    // GL_NONE: not attached
            a_glGetFramebufferAttachmentParameteriv(
                0x8CA9, att[i].p, 0x8CD1 /* OBJECT_NAME */, &obj);
            a_glGetFramebufferAttachmentParameteriv(
                0x8CA9, att[i].p, 0x8210 /* COMPONENT_TYPE */, &layered);
            fprintf(stderr, "  [glfb]      %-8s type=0x%x name=%d componentType=0x%x\n",
                    att[i].n, type, obj, layered);
        }
    }

    // ...and what the GUEST asked for, which is the other half of the question.
    // GL reporting a binding the guest never named means the bind was refused
    // (candidate b) or ANGLE is reporting a sentinel (candidate a); the two need
    // different fixes and are indistinguishable from the query alone.
    fprintf(stderr, "  [glfb]      guest shadow: draw_fb=%u read_fb=%u\n",
            g_draw_fb, g_read_fb);
    unsigned n = __atomic_load_n(&g_bindlog_n, __ATOMIC_RELAXED);
    unsigned first = n > KLFB_BINDLOG ? n - KLFB_BINDLOG : 0;
    for (unsigned k = first; k < n; k++) {
        unsigned i = k % KLFB_BINDLOG;
        size_t off = 0;
        const char *img = kl_addr_image((void *)g_bindlog[i].site, &off);
        char err[32];
        if (g_bindlog_err[i] == 0xffffffffu) snprintf(err, sizeof err, "err=?");
        else if (g_bindlog_err[i] == 0)      snprintf(err, sizeof err, "ok");
        else snprintf(err, sizeof err, "REFUSED 0x%x", g_bindlog_err[i]);
        fprintf(stderr, "  [glfb]      bind#%u t%llu (0x%x, %u) %s ctx=%p <- %s+0x%zx\n",
                k, (unsigned long long)g_bindlog[i].tid,
                g_bindlog[i].target, g_bindlog[i].fb, err, g_bindlog[i].ctx,
                img ? img : "?", off);
    }
    while (a_glGetError && a_glGetError()) {}
}

void kl_gl_trace_log(const char *name) {
    unsigned n = __atomic_fetch_add(&g_trace_calls, 1, __ATOMIC_RELAXED);
    if (glfb_errscan() && a_glGetError) {
        static unsigned said;   // plain: __atomic_* reject _Atomic-qualified operands
        uint32_t e = a_glGetError();
        if (e && (!g_err_only || e == g_err_only) &&
            __atomic_load_n(&said, __ATOMIC_RELAXED) < 40) {
            __atomic_fetch_add(&said, 1, __ATOMIC_RELAXED);
            fprintf(stderr, "  [glfb] ERRSCAN GL error 0x%x generated by %s "
                    "(before #%u %s)\n",
                    e, g_err_prev ? g_err_prev : "(nothing yet)", n, name);
            while (a_glGetError()) {}          // drain the rest of the queue
            // ...and for the one error that is ABOUT a framebuffer, say which.
            if (e == 0x506)
                glfb_report_incomplete_fb(g_err_prev ? g_err_prev : "a call");
        }
        g_err_prev = name;
    }
    if (getenv_trace_print() && n >= g_trace_from)
        fprintf(stderr, "  [gl] #%u %s\n", n, name);
}

// The per-call log line is KL_GLFB_TRACE's; ERRSCAN needs the same trampoline
// but not the firehose, so the two are separate questions.
static int getenv_trace_print(void) {
    static int on = -1;
    if (on < 0) {
        on = kl_env_on("KL_GLFB_TRACE", 0);
        g_trace_from = kl_env_uint("KL_GLFB_TRACE_FROM", g_trace_from);
    }
    return on;
}

static int glfb_tracing(void) {
    return getenv_trace_print() || glfb_errscan();
}

static int glfb_locking(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_GLFB_LOCK", 0);
    return on;
}

static void *glfb_wrap_traced(const char *name, void *real) {
    if (!real) return real;
    void *tramp = glfb_locking() ? (void *)kl_gl_lock_tramp
                : glfb_tracing() ? (void *)kl_gl_trace_tramp
                : NULL;
    if (!tramp) return real;
    klfb_trace_desc *d = malloc(sizeof *d);
    if (!d) return real;
    d->name = strdup(name);
    d->real = real;
    if (!d->name) { free(d); return real; }
    void *stub = kl_trace_stub(name, d, tramp);
    return stub ? stub : real;
}

// Does this GL entry point read or change the framebuffer binding (draws,
// clears, blits, reads, copies from the framebuffer, attachments, framebuffer
// queries, draw/read buffer selection)? Conservative by name: pure state
// setters (clear values) and object gen/is are the only exclusions.
static int klfb_lazy_binding_dependent(const char *n) {
    if (strncmp(n, "gl", 2) != 0) return 0;
    static const char *const state_only[] = {
        "glClearColor", "glClearColorx", "glClearDepthf", "glClearDepthx", "glClearStencil",
        "glClearTexImageEXT", "glClearTexSubImageEXT", "glGenFramebuffers",
        "glGenFramebuffersOES", "glIsFramebuffer", "glIsFramebufferOES", NULL,
    };
    for (int i = 0; state_only[i]; i++) if (!strcmp(n, state_only[i])) return 0;
    static const char *const marks[] = {
        "Draw", "Blit", "ReadPixels", "ReadnPixels", "ReadBuffer", "Clear", "CopyTexImage",
        "CopyTexSubImage", "Invalidate", "DiscardFramebuffer", "Framebuffer", "GetInteger",
        "GetBoolean", "GetFloat", NULL,
    };
    for (int i = 0; marks[i]; i++) if (strstr(n + 2, marks[i])) return 1;
    return 0;
}

void *kl_glfb_sym(const char *name) {
    if (!kl_glfb_enabled() || !name) return NULL;
    if (!g_ready && !kl_glfb_init()) return NULL;
    if (keep_ours(name)) return NULL;              // caller keeps its own answer
    if (glfb_skipped(name)) {
        fprintf(stderr, "  [glfb] KL_GLFB_SKIP: %s stays on the null driver\n", name);
        return NULL;
    }

    void *fn = asym(name);
    if (!fn) return NULL;                          // ANGLE has no such entry point
    for (size_t i = 0; i < sizeof g_thunks / sizeof g_thunks[0]; i++)
        if (strcmp(g_thunks[i].name, name) == 0) {
            *g_thunks[i].real = fn;
            // Trace in front of the thunk, so the log reflects what the guest
            // called and the thunk's own ABI fixups still happen.
            return glfb_wrap_traced(name, g_thunks[i].thunk);
        }
    // KL_GLFB_LAZY_UNBIND is only sound if EVERY binding-dependent call the
    // guest can make forwards a held unbind first. One that would reach ANGLE
    // unhooked turns the optimisation off for the whole run, said once.
    if (!g_lazy_unsafe && klfb_lazy_binding_dependent(name)) {
        int was = klfb_lazy_unbind_on();
        g_lazy_unsafe = name;
        if (was)
            fprintf(stderr, "  [glfb] lazy unbind OFF for this run: the guest resolved %s, which "
                            "depends on the framebuffer binding and has no flushing thunk\n", name);
    }
    return glfb_wrap_traced(name, fn);
}

// ---------------------------------------------------------- present
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[4]; be32(h, n); fwrite(h, 1, 4, f); fwrite(t, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)t, 4);
    if (n) c = crc32(c, (const Bytef *)d, n);
    be32(h, (uint32_t)c); fwrite(h, 1, 4, f);
}

// Called from the guest's eglSwapBuffers. The swap does not arrive on the GL
// thread — measured — and under KL_GLFB_SHARED each thread's pbuffer is its
// own, so capturing on the swap thread reads a framebuffer nothing was ever
// drawn into (0 lit, by construction). The swap only *requests* a capture;
// klfb_service_capture does the work from a call the GL thread makes itself:
// glFlush/glFinish, and the draw wrappers, which by definition run where the
// drawing did. Reading pixels with no current context is not an error, it is
// silence, which is exactly how that presented: a perfectly black frame with
// the framebuffer queries writing nothing at all.
//
// KL_GLFB_OUT_EVERY=N throttles to every Nth swap (default 1) — a 300-frame
// pump otherwise writes 300 full-size PNGs. The throttle applies to the PNG
// path only: a registered frame sink is a live display, not a batch of files,
// so it wants every swap and dir may be NULL.
// Whether THIS swap should be captured, for a run nobody is driving.
//
// A device build is launched from the Home View by a person who then has to
// walk a menu, watch two videos and load a level before the interesting frame
// exists. "Capture every swap" is useless (thousands of PNGs, most of them a
// menu) and "capture frame N" is guesswork about how long that takes. So the
// capture ARMS on a signal from the guest itself -- the first shadow pass,
// which only a loaded level builds -- and then spends a fixed, small budget.
//
//   KL_GLFB_OUT_ARM_CSM=1   arm on the first depth-only framebuffer (default
//                           off; without it the budget starts at swap 0)
//   KL_GLFB_OUT_ARM_INTERIOR=1  arm on kl_glfb_arm_capture(), which the guest
//                           door calls when it knows the interesting moment has
//                           arrived (SHAR: InteriorManager::Enter). Either arm
//                           is sufficient when both are set.
//   KL_GLFB_OUT_DELAY=N     swaps to let pass after arming, so the picture has
//                           settled (default 0)
//   KL_GLFB_OUT_EVERY=N     spacing between captures (default 1)
//   KL_GLFB_OUT_MAX=N       how many captures in total, 0 = unlimited
//
// The budget is the safety rule as much as the disk rule: each capture stalls
// the guest's render thread, and visionOS kills an immersive app that goes
// 2.0 s without submitting a drawable.
static int klfb_capture_due(void) {
    static int init, arm_csm, arm_int, delay, maxn, every;
    static unsigned n, done;
    if (!init) {
        init    = 1;
        arm_csm = kl_env_on("KL_GLFB_OUT_ARM_CSM", 0);
        arm_int = kl_env_on("KL_GLFB_OUT_ARM_INTERIOR", 0);
        delay   = kl_env_int("KL_GLFB_OUT_DELAY", 0);
        maxn    = kl_env_int("KL_GLFB_OUT_MAX", 0);
        every   = kl_env_int("KL_GLFB_OUT_EVERY", 1);
        if (every < 1) every = 1;
        if (delay < 0) delay = 0;
    }
    // EITHER arm is sufficient when both are set: an interior build must not
    // silently also wait on a shadow pass it has no reason to want.
    if (arm_csm || arm_int) {
        int ok = (arm_csm && __atomic_load_n(&g_shadow_pass_seen, __ATOMIC_RELAXED)) ||
                 (arm_int && __atomic_load_n(&g_capture_armed, __ATOMIC_RELAXED));
        if (!ok) return 0;
    }
    if (maxn > 0 && (int)done >= maxn) return 0;
    unsigned k = n++;
    if ((int)k < delay) return 0;
    if ((k - (unsigned)delay) % (unsigned)every) return 0;
    fprintf(stderr, "  [glfb] capture %u of %d — swap %u after arming%s\n",
            done + 1, maxn, k,
            arm_int ? " on the guest's interior arm"
                    : arm_csm ? " on the first shadow pass" : "");
    done++;
    return 1;
}

unsigned kl_glfb_present(const char *dir) {
    KLFB_LAZY_FLUSH();
    kl_glfb_lazy_report();
    // r26: the frame boundary, ticked BEFORE the early return so a run that
    // registered no sink and no fence still traces.
    klfb_ftrace_swap();
    if (!g_ready || (!dir && !g_frame_sink && !g_gpu_fence)) return g_presented;
    // A registered GPU fence replaces the readback entirely — the compositor
    // samples the eye texture itself and only needs to know when the guest's
    // frame is done. Same thread rule as the capture below: the sync has to be
    // created where the context is current, so a swap off the GL thread defers.
    if (g_gpu_fence) {
        pthread_once(&g_tls_once, klfb_tls_init);
        klfb_thread_gl *tg = pthread_getspecific(g_tls_key);
        if (tg && tg->ctx) {
            // ON DEVICE THE FENCE IS NOT THE WHOLE ANSWER. Every device run
            // registers a fence, and the early return above meant KL_GLFB_OUT
            // was inert on the only build anyone actually wears -- so the FBO
            // census and its PNGs, the one instrument that can say WHICH pass
            // went black, existed only for the host. A host that cannot be
            // driven into a level (see KL_XR_FAKE_PRESS) therefore had no way
            // to look at a level at all.
            //
            // The capture runs FIRST and the fence second: the readback forces
            // the guest's work to complete, so signalling after it is still
            // correct ordering, and the compositor is never told a frame is
            // ready before it is. It costs a full-frame glReadPixels plus one
            // per FBO, which is why klfb_capture_due() hands out a strictly
            // bounded number of them.
            if (dir && klfb_capture_due()) glfb_capture_now(dir);
            return klfb_gpu_frame_now();
        }
        g_capture_pending = 1;
        return g_presented;
    }
    // A registered frame sink is a live display and wants every swap; the PNG
    // path goes through the same arm/delay/spacing/budget gate the device path
    // uses, so a host run and a device run capture the same frames.
    if (!g_frame_sink && !klfb_capture_due()) return g_presented;
    if (dir) snprintf(g_capture_dir, sizeof g_capture_dir, "%s", dir);
    // If the swap arrived on a thread that owns a context — in migration mode
    // the render thread, which is exactly where the frame was drawn — capture
    // NOW, before the next frame's clears overwrite it. (Capturing later, from
    // the next draw call, measured black even where the frame rendered.)
    pthread_once(&g_tls_once, klfb_tls_init);
    klfb_thread_gl *t = pthread_getspecific(g_tls_key);
    if (t && t->ctx) return glfb_capture_now(dir);
    g_capture_pending = 1;
    return g_presented;
}

// ---- which FBO is the picture in?
//
// fb0 is black by construction (the VR frame goes to eye textures, not the
// backbuffer), and the first census was blind twice over: it scanned only
// fbo1..8, and it read everything as GL_RGBA/UNSIGNED_BYTE — which an RGBA16F
// attachment rejects (INVALID_ENUM, the 0x500 in the draw-probe log) and an
// MSAA attachment rejects outright (INVALID_OPERATION). klfb_probe_fbo reads
// one FBO the way its own color attachment allows: the read format/type come
// from GL_IMPLEMENTATION_COLOR_READ_FORMAT/TYPE, the attachment is identified
// against the allocation table the glTexStorage2D thunk keeps (ES 3.0 has no
// glGetTexLevelParameteriv — the ANGLE extension entry point rejects on this
// context), and MSAA attachments are reported rather than misread.
// Returns the lit count and writes a one-line description into note.
//
// It disturbs GL state (binds the FBO, maybe a renderbuffer), so it belongs
// to the debug paths (PNG census, probe runs), not the sink handoff. The
// caller restores the READ_FRAMEBUFFER binding; the renderbuffer binding is
// saved and restored here.
#define KLFB_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#define KLFB_IMPLEMENTATION_COLOR_READ_TYPE   0x8B9A
#define KLFB_HALF_FLOAT 0x140B

// IEEE half -> float, for RGBA16F readbacks. Bit-exact exponent rebase; no
// denormal/inf finesse needed for a lit-pixel census and a debug picture.
static float klfb_half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t out;
    if (exp == 0)        out = sign | (mant << 13);              // ~denormal
    else if (exp == 31)  out = sign | 0x7F800000 | (mant << 13); // inf/nan
    else                 out = sign | ((exp + 112) << 23) | (mant << 13);
    float f;
    memcpy(&f, &out, 4);
    return f;
}

static uint8_t klfb_dbg_tone(float c);   // defined with the capture below

static unsigned long klfb_probe_fbo(uint32_t fb, float *fbuf, uint8_t *bbuf,
                                    char *note, size_t note_n,
                                    uint8_t *dump, int32_t *dw, int32_t *dh,
                                    int32_t hint_w, int32_t hint_h) {
    static void (*r_BindFramebuffer)(uint32_t, uint32_t);
    static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
    static void (*r_GetFramebufferAttachmentParameteriv)(uint32_t, uint32_t,
                                                         uint32_t, int32_t *);
    static void (*r_BindRenderbuffer)(uint32_t, uint32_t);
    static void (*r_GetRenderbufferParameteriv)(uint32_t, uint32_t, int32_t *);
    static void (*r_BindTexture2)(uint32_t, uint32_t);
    if (!r_BindFramebuffer) {
        r_BindFramebuffer = asym("glBindFramebuffer");
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
        r_GetFramebufferAttachmentParameteriv =
            asym("glGetFramebufferAttachmentParameteriv");
        r_BindRenderbuffer = asym("glBindRenderbuffer");
        r_GetRenderbufferParameteriv = asym("glGetRenderbufferParameteriv");
        r_BindTexture2 = asym("glBindTexture");
    }
    if (!r_BindFramebuffer || !r_CheckFramebufferStatus ||
        !r_GetFramebufferAttachmentParameteriv || !a_glGetIntegerv ||
        !a_glReadPixels) {
        snprintf(note, note_n, "no GL entry points");
        return 0;
    }
    // Finish first, for EVERY caller. One probe that finishes and one that does
    // not disagree about the same texture in the same run, and a difference
    // between the instruments then reads as a difference in the pipeline. A
    // debug readback can afford the stall.
    if (a_glFinish) a_glFinish();
    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, fb);
    if (r_CheckFramebufferStatus(0x8CA8) != 0x8CD5) {
        snprintf(note, note_n, "incomplete");
        return 0;
    }
    int32_t otype = 0, oname = 0;
    r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                                          0x8CD0 /* OBJECT_TYPE */, &otype);
    r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8CE0, 0x8CD1 /* OBJECT_NAME */,
                                          &oname);
    int32_t fmt = 0, aw = 0, ah = 0, samples = 0;
    uint32_t resolved_via = 0, resolve_err = 0;
    // Which staging format makes this attachment readable, if it isn't
    // already: MSAA renderbuffers resolve to a SAME-FORMAT texture (a
    // float->unorm blit is the forbidden one), and R11F_G11F_B10F textures —
    // the bloom pyramid — blit to RGBA16F, because this driver's own read
    // format for R11F (0x1907/0x8c3b) is one glReadPixels rejects outright.
    // Both are float->float blits, the legal path.
    uint32_t stage_want = 0;
    int hinted = 0;
    if (otype == 0x1702 /* TEXTURE */) {
        uint32_t ufmt = 0;
        klfb_tex_info((uint32_t)oname, &ufmt, &aw, &ah);
        fmt = (int32_t)ufmt;
        if (ufmt == 0x8C3A /* R11F_G11F_B10F */) stage_want = 0x881A;
        // The allocation table is a WATCH, not an oracle: a texture created
        // through a path it does not see (glTexImage2D, or any allocation once
        // the table is full) leaves aw/ah at 0, and the read below then bails
        // out and returns 0 lit — an instrument reporting "black" for a
        // framebuffer it never looked at. That is exactly the reading that made
        // VRChat's eye copy look like a guest drawing nothing. Where the caller
        // knows a size from the call it is bracketing — a blit names its own
        // source and destination rectangles — take it, and SAY the size is a
        // hint so the two are never confused in the log.
        if ((aw <= 0 || ah <= 0) && hint_w > 0 && hint_h > 0) {
            aw = hint_w; ah = hint_h; hinted = 1;
        }
    } else if (otype == 0x8D41 /* RENDERBUFFER */ && r_BindRenderbuffer &&
               r_GetRenderbufferParameteriv) {
        int32_t save_rb = 0;
        a_glGetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &save_rb);
        r_BindRenderbuffer(0x8D41, (uint32_t)oname);
        r_GetRenderbufferParameteriv(0x8D41, 0x8D44 /* INTERNAL_FORMAT */, &fmt);
        r_GetRenderbufferParameteriv(0x8D41, 0x8D42 /* WIDTH */, &aw);
        r_GetRenderbufferParameteriv(0x8D41, 0x8D43 /* HEIGHT */, &ah);
        r_GetRenderbufferParameteriv(0x8D41, 0x8CAB /* SAMPLES */, &samples);
        r_BindRenderbuffer(0x8D41, (uint32_t)save_rb);
        if (samples > 0) stage_want = (uint32_t)fmt;
    }
    if (stage_want) {
        {
            // MSAA can't be read directly, so resolve through a staging FBO of
            // our own with a SAME-FORMAT texture — a float->unorm blit is the
            // forbidden one, an MSAA resolve to the same format is the legal
            // path. This is how the scene renderbuffer (where the geometry
            // draws actually land) becomes readable. Debug path only: it
            // creates two real GL objects on the guest's shared context.
            static void (*r_GenTextures)(int32_t, uint32_t *);
            static void (*r_GenFramebuffers2)(int32_t, uint32_t *);
            static void (*r_TexStorage2D)(uint32_t, int32_t, uint32_t, int32_t,
                                          int32_t);
            static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t,
                                                  uint32_t, int32_t);
            static void (*r_BlitFramebuffer)(int32_t, int32_t, int32_t, int32_t,
                                             int32_t, int32_t, int32_t, int32_t,
                                             uint32_t, uint32_t);
            if (!r_GenTextures) {
                r_GenTextures = asym("glGenTextures");
                r_GenFramebuffers2 = asym("glGenFramebuffers");
                r_TexStorage2D = asym("glTexStorage2D");
                r_FramebufferTexture2D = asym("glFramebufferTexture2D");
                r_BlitFramebuffer = asym("glBlitFramebuffer");
            }
            static uint32_t stage_tex, stage_fb;
            static int32_t stage_w, stage_h;
            static uint32_t stage_fmt;
            if (!r_GenTextures || !r_BlitFramebuffer) {
                snprintf(note, note_n, "rb=%d MSAA x%d (no staging entry points)",
                         oname, samples);
                return 0;
            }
            if (!stage_tex) {
                r_GenTextures(1, &stage_tex);
                r_GenFramebuffers2(1, &stage_fb);
            }
            if (stage_want != stage_fmt || aw != stage_w || ah != stage_h) {
                int32_t save_tex = 0, save_draw = 0;
                a_glGetIntegerv(0x8069, &save_tex);
                a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &save_draw);
                r_BindTexture2(0x0DE1, stage_tex);
                r_TexStorage2D(0x0DE1, 1, stage_want, aw, ah);
                r_BindTexture2(0x0DE1, (uint32_t)save_tex);
                r_BindFramebuffer(0x8CA9 /* DRAW_FRAMEBUFFER */, stage_fb);
                r_FramebufferTexture2D(0x8CA9, 0x8CE0, 0x0DE1, stage_tex, 0);
                r_BindFramebuffer(0x8CA9, (uint32_t)save_draw);
                stage_fmt = stage_want; stage_w = aw; stage_h = ah;
            }
            int32_t save_draw = 0;
            a_glGetIntegerv(0x8CA6, &save_draw);
            r_BindFramebuffer(0x8CA9, stage_fb);
            // Sentinel first: a pixel the resolve blit did not write keeps the
            // sentinel colour, so a silently-failing resolve reads as sentinel
            // rather than as whatever the staging texture held before.
            // Without it the same renderbuffer reads back differently on
            // consecutive probes.
            static void (*r_ClearColor)(float, float, float, float);
            static void (*r_Clear)(uint32_t);
            if (!r_ClearColor) {
                r_ClearColor = asym("glClearColor");
                r_Clear = asym("glClear");
            }
            if (r_ClearColor && r_Clear) {
                r_ClearColor(0.6f, 0.1f, 0.9f, 1.0f);   // loud magenta
                r_Clear(0x4000);
            }
            // fb is still bound as READ from the top of the probe. Flush the
            // command stream first: the draw probe reads the same renderbuffer
            // lit where the blit probe reads it black a few calls later, and
            // the draw probe glFinish()es first — the outstanding difference.
            if (a_glFinish) a_glFinish();
            r_BlitFramebuffer(0, 0, aw, ah, 0, 0, aw, ah,
                              0x4000 /* COLOR_BUFFER_BIT */, 0x2600 /* NEAREST */);
            if (a_glGetError) resolve_err = a_glGetError();
            r_BindFramebuffer(0x8CA9, (uint32_t)save_draw);
            r_BindFramebuffer(0x8CA8, stage_fb);
            resolved_via = stage_fb;
            // fall through to the ordinary read, now against the staging FBO
        }
    }
    int32_t rfmt = 0, rtype = 0;
    a_glGetIntegerv(KLFB_IMPLEMENTATION_COLOR_READ_FORMAT, &rfmt);
    a_glGetIntegerv(KLFB_IMPLEMENTATION_COLOR_READ_TYPE, &rtype);
    // The LAYER, for an array attachment. Naming the texture without it is not
    // an identification: both eyes of an OpenXR guest are slices of one name,
    // so "tex=29" reads identically for a lit eye and an empty one, and the two
    // readings differ only here.
    int32_t alayer = 0, alevel = 0;
    if (otype == 0x1702) {
        r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8CE0,
                                              0x8CD4 /* TEXTURE_LAYER */, &alayer);
        // The LEVEL, for the same reason as the layer: a name plus a layer is
        // still not an identification while a mip level can differ, and two
        // framebuffers on one texture reading differently is exactly what that
        // looks like.
        r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8CE0,
                                              0x8CD2 /* TEXTURE_LEVEL */, &alevel);
    }
    // A framebuffer with NO colour attachment is not a broken framebuffer, it is
    // a shadow pass -- and "type?=0 … SIZE UNKNOWN, not read" is a terrible way
    // to say so. Launch #6's fb7/fb8/fb9 are Simpsons Hit & Run's three CSM
    // cascades, and the census reported them in the words it uses for a
    // framebuffer it failed to understand. Name the depth attachment instead,
    // and say plainly that ES cannot glReadPixels a depth texture, so the
    // absence of a picture here is a rule and not a defect.
    if (otype == 0) {
        int32_t dname = 0, dtype = 0;
        r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8D00 /* DEPTH */,
                                              0x8CD0 /* OBJECT_TYPE */, &dtype);
        if (dtype) {
            r_GetFramebufferAttachmentParameteriv(0x8CA8, 0x8D00,
                                                  0x8CD1 /* OBJECT_NAME */, &dname);
            uint32_t dfmt = 0; int32_t dw2 = 0, dh2 = 0;
            klfb_tex_info((uint32_t)dname, &dfmt, &dw2, &dh2);
            snprintf(note, note_n,
                     "DEPTH-ONLY (no colour attachment): depth %s=%d %dx%d "
                     "fmt=0x%x — a shadow cascade. ES cannot glReadPixels a "
                     "depth texture, so there is no picture of this and its "
                     "absence is not a defect",
                     dtype == 0x1702 ? "tex" : "rb", dname, dw2, dh2, dfmt);
            return 0;
        }
    }
    snprintf(note, note_n, "%s=%d layer=%d level=%d fmt=0x%x %dx%d%s read=0x%x/0x%x",
             otype == 0x1702 ? "tex" : otype == 0x8D41 ? "rb" : "type?",
             oname, alayer, alevel, fmt, aw, ah,
             hinted ? " (size from the caller)" : "", rfmt, rtype);
    if (resolved_via) {
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " %s resolved-via-fb%u%s%x",
                 samples > 0 ? "MSAA" : "blit", resolved_via,
                 resolve_err ? " resolvebliterr=0x" : "", resolve_err);
    }
    // The buffers are g_w*g_h; a larger attachment (the resized eye textures
    // are 2304x2198 against a 1832x1920 default) reads clipped rather than
    // overflowing.
    if (aw > g_w) aw = g_w;
    if (ah > g_h) ah = g_h;
    if (aw <= 0 || ah <= 0) {
        // Nothing was read. Returning a bare 0 here reads identically to a
        // framebuffer measured and found black, which is the more serious of
        // the two answers — so say which one this is.
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " — SIZE UNKNOWN, not read");
        return 0;
    }
    if (dw) *dw = aw;
    if (dh) *dh = ah;

    // A bound PIXEL_PACK_BUFFER turns the pointer below into an OFFSET, so
    // glReadPixels writes into the guest's buffer object and leaves ours
    // untouched — no error, and a readback of whatever malloc handed us, which
    // is zeroes. Unity binds one for its async readbacks, so this probe read
    // "black" off a framebuffer that had just been cleared to a visible colour.
    // Pack alignment and row length are the same class of ambient state: they
    // are the guest's, and every one of them silently changes what this reads.
    static void (*r_BindBuffer)(uint32_t, uint32_t);
    static void (*r_PixelStorei)(uint32_t, int32_t);
    if (!r_BindBuffer) { r_BindBuffer = asym("glBindBuffer");
                         r_PixelStorei = asym("glPixelStorei"); }
    int32_t pack_buf = 0, pack_align = 4, pack_rowlen = 0, pack_skip_p = 0, pack_skip_r = 0;
    a_glGetIntegerv(0x88ED /* PIXEL_PACK_BUFFER_BINDING */, &pack_buf);
    a_glGetIntegerv(0x0D05 /* PACK_ALIGNMENT */,  &pack_align);
    a_glGetIntegerv(0x0D02 /* PACK_ROW_LENGTH */, &pack_rowlen);
    a_glGetIntegerv(0x0D04 /* PACK_SKIP_PIXELS */, &pack_skip_p);
    a_glGetIntegerv(0x0D03 /* PACK_SKIP_ROWS */,   &pack_skip_r);
    if (pack_buf && r_BindBuffer) r_BindBuffer(0x88EB /* PIXEL_PACK_BUFFER */, 0);
    if (r_PixelStorei) {
        if (pack_align != 4)  r_PixelStorei(0x0D05, 4);
        if (pack_rowlen)      r_PixelStorei(0x0D02, 0);
        if (pack_skip_p)      r_PixelStorei(0x0D04, 0);
        if (pack_skip_r)      r_PixelStorei(0x0D03, 0);
    }
    if (pack_buf || pack_rowlen || pack_skip_p || pack_skip_r || pack_align != 4) {
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " [guest pack: buf=%d align=%d row=%d "
                 "skip=%d,%d — neutralised]", pack_buf, pack_align, pack_rowlen,
                 pack_skip_p, pack_skip_r);
    }
    // ...and READ_BUFFER, which is the same class again and the one this probe
    // was still blind to: it is PER-FRAMEBUFFER state, and it selects WHICH
    // colour attachment glReadPixels takes. The probe identifies COLOR
    // ATTACHMENT0 and then reads whatever this framebuffer's read buffer names,
    // so an MRT framebuffer pointed at attachment 1 reports attachment 0's
    // texture and attachment 1's pixels — with no error, which is how two
    // framebuffers on ONE texture can read differently and look impossible.
    static void (*r_ReadBuffer)(uint32_t);
    if (!r_ReadBuffer) r_ReadBuffer = asym("glReadBuffer");
    int32_t read_buf = 0x8CE0;
    a_glGetIntegerv(0x0C02 /* READ_BUFFER */, &read_buf);
    int read_buf_changed = 0;
    if (fb != 0 && read_buf != 0x8CE0 && r_ReadBuffer) {
        r_ReadBuffer(0x8CE0 /* COLOR_ATTACHMENT0 */);
        read_buf_changed = 1;
    }
    if (read_buf != 0x8CE0) {
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " [read buffer was 0x%x%s]", read_buf,
                 read_buf_changed ? " — neutralised to COLOR_ATTACHMENT0" : "");
    }
    if (a_glGetError) while (a_glGetError()) {}        // drain guest leftovers
    unsigned long lit = 0, sentinel = 0;
    float fsum = 0;
    if (rtype == 0x1406 /* FLOAT */) {
        a_glReadPixels(0, 0, aw, ah, GL_RGBA, GL_FLOAT, fbuf);
        for (int32_t i = 0; i < aw * ah; i++) {
            float r = fbuf[i * 4], g = fbuf[i * 4 + 1], b = fbuf[i * 4 + 2];
            if (resolved_via && r > 0.55f && r < 0.65f && g > 0.05f &&
                g < 0.15f && b > 0.85f && b < 0.95f) { sentinel++; continue; }
            if (dump) {
                dump[i * 4 + 0] = klfb_dbg_tone(r);
                dump[i * 4 + 1] = klfb_dbg_tone(g);
                dump[i * 4 + 2] = klfb_dbg_tone(b);
                dump[i * 4 + 3] = 255;
            }
            float lum = r + g + b;
            fsum += lum;
            if (lum > 0.05f) lit++;
        }
    } else if (rtype == KLFB_HALF_FLOAT) {
        // 8 bytes a pixel — fits in fbuf, which is sized for 16.
        uint16_t *hbuf = (uint16_t *)fbuf;
        a_glReadPixels(0, 0, aw, ah, GL_RGBA, KLFB_HALF_FLOAT, hbuf);
        for (int32_t i = 0; i < aw * ah; i++) {
            float r = klfb_half_to_float(hbuf[i * 4]);
            float g = klfb_half_to_float(hbuf[i * 4 + 1]);
            float b = klfb_half_to_float(hbuf[i * 4 + 2]);
            if (resolved_via && r > 0.55f && r < 0.65f && g > 0.05f &&
                g < 0.15f && b > 0.85f && b < 0.95f) { sentinel++; continue; }
            if (dump) {
                dump[i * 4 + 0] = klfb_dbg_tone(r);
                dump[i * 4 + 1] = klfb_dbg_tone(g);
                dump[i * 4 + 2] = klfb_dbg_tone(b);
                dump[i * 4 + 3] = 255;
            }
            float lum = r + g + b;
            fsum += lum;
            if (lum > 0.05f) lit++;
        }
    } else {
        a_glReadPixels(0, 0, aw, ah, GL_RGBA, GL_UNSIGNED_BYTE, bbuf);
        if (dump) memcpy(dump, bbuf, (size_t)aw * ah * 4);
        for (int32_t i = 0; i < aw * ah; i++) {
            unsigned lum = bbuf[i * 4] + bbuf[i * 4 + 1] + bbuf[i * 4 + 2];
            fsum += (float)lum / 255.0f;
            if (lum > 12) lit++;
        }
    }
    uint32_t err = a_glGetError ? a_glGetError() : 0;
    // Put the guest's pixel-store state back before anything else runs: this is
    // a debug read on the guest's own context, and leaving it changed would
    // turn an instrument into a cause.
    if (pack_buf && r_BindBuffer) r_BindBuffer(0x88EB, (uint32_t)pack_buf);
    if (r_PixelStorei) {
        if (pack_align != 4)  r_PixelStorei(0x0D05, pack_align);
        if (pack_rowlen)      r_PixelStorei(0x0D02, pack_rowlen);
        if (pack_skip_p)      r_PixelStorei(0x0D04, pack_skip_p);
        if (pack_skip_r)      r_PixelStorei(0x0D03, pack_skip_r);
    }
    if (read_buf_changed && r_ReadBuffer) r_ReadBuffer((uint32_t)read_buf);
    if (err) {
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " readerr=0x%x", err);
        return 0;
    }
    {
        size_t l = strlen(note);
        snprintf(note + l, note_n - l, " mean %.4f",
                 aw * ah ? fsum / ((float)aw * ah) : 0.0f);
        if (resolved_via) {
            l = strlen(note);
            snprintf(note + l, note_n - l, " sentinel=%lu", sentinel);
        }
    }
    return lit;
}

// Debug transform, not color management: clamp HDR into [0,1] and apply a
// plain 1/2.2 gamma so linear-space float content is visible on a monitor.
// The reference renderer's job is "is there a picture", not colorimetry.
static uint8_t klfb_dbg_tone(float c) {
    // Debug-only display curve, env-tunable: KL_GLFB_EXPOSURE scales the
    // linear value first, KL_GLFB_GAMMA overrides the 1/2.2 encode exponent.
    // Statics: the 64K LUT caller memoizes this, so the env is read once.
    static float expo = -1, gam = -1;
    if (expo < 0) {
        expo = kl_env_float("KL_GLFB_EXPOSURE", 1.0f);
        gam = kl_env_float("KL_GLFB_GAMMA", 1.0f / 2.2f);
        if (expo <= 0) expo = 1.0f;
        if (gam <= 0) gam = 1.0f / 2.2f;
    }
    if (!(c > 0)) return 0;                    // also NaN
    c *= expo;
    if (c > 1) c = 1;
    return (uint8_t)(powf(c, gam) * 255.0f + 0.5f);
}

// 8-bit RGBA PNG, bottom-up GL rows flipped to top-down. Shared by the swap
// capture and the per-FBO dump; returns 0 on any failure (path, alloc, zlib).
static int klfb_write_png(const char *path, const uint8_t *px,
                          int32_t w, int32_t h) {
    size_t   stride = (size_t)w * 4;
    size_t   raw_n = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    uLongf   cn  = compressBound((uLong)raw_n);
    uint8_t *cb  = malloc(cn);
    if (!raw || !cb) { free(raw); free(cb); return 0; }
    for (int32_t y = 0; y < h; y++) {            // GL bottom-up -> PNG top-down
        raw[(stride + 1) * (size_t)y] = 0;
        memcpy(raw + (stride + 1) * (size_t)y + 1,
               px + stride * (size_t)(h - 1 - y), stride);
    }
    // Z_BEST_SPEED, not the default level: this runs on the GUEST'S RENDER
    // THREAD inside its frame, and on device a capture is ~18 MB of RGBA per
    // FBO. Default-level deflate on that is a multi-second freeze per capture;
    // level 1 is several times faster for a debug picture nobody archives.
    if (compress2(cb, &cn, raw, (uLong)raw_n, Z_BEST_SPEED) != Z_OK) {
        free(raw); free(cb); return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(cb); return 0; }
    static const uint8_t sig[8] = {0x89,'P','N','G',13,10,26,10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h);
    ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", cb, (uint32_t)cn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(cb);
    return 1;
}

// Make a texture readable without going through anything the guest bound.
//
// The FBO scan below asks "which of the guest's framebuffers has an eye texture
// attached", and on the OpenXR path that question can have no answer while the
// frame is perfectly fine: a swapchain rotates three images, and the FBO's
// attachment is whatever the guest last pointed it at, not necessarily the
// image it just presented. Falling back to fb0 there is the worst possible
// answer, because fb0 is black by construction — a run with pictures and a run
// with none read identically. Attaching the named image to a framebuffer of our
// own removes the guest from the question entirely.
//
// Returns the framebuffer, bound as READ_FRAMEBUFFER, or 0. The caller restores
// the previous binding.
// Attach one LAYER of a 2D array texture. Same shape as klfb_read_from_texture
// and deliberately separate: glFramebufferTexture2D takes a target and cannot
// name a slice, so an array eye needs glFramebufferTextureLayer or it is not
// readable at all — which is not a degraded picture, it is an incomplete
// framebuffer and a black PNG.
static uint32_t klfb_read_from_texture_layer(uint32_t tex, int layer) {
    static void (*r_GenFramebuffers)(int32_t, uint32_t *);
    static void (*r_BindFramebuffer)(uint32_t, uint32_t);
    static void (*r_FramebufferTextureLayer)(uint32_t, uint32_t, uint32_t, int32_t, int32_t);
    static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
    static int resolved;
    if (!resolved) {
        resolved = 1;
        r_GenFramebuffers = asym("glGenFramebuffers");
        r_BindFramebuffer = asym("glBindFramebuffer");
        r_FramebufferTextureLayer = asym("glFramebufferTextureLayer");
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
    }
    if (!tex || !r_GenFramebuffers || !r_BindFramebuffer ||
        !r_FramebufferTextureLayer || !r_CheckFramebufferStatus) return 0;
    static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t,
                                          uint32_t, int32_t);
    static void (*r_GetFbAttachmentParam)(uint32_t, uint32_t, uint32_t, int32_t *);
    if (!r_FramebufferTexture2D) {
        r_FramebufferTexture2D = asym("glFramebufferTexture2D");
        r_GetFbAttachmentParam = asym("glGetFramebufferAttachmentParameteriv");
    }
    static uint32_t layer_fb;
    if (!layer_fb) r_GenFramebuffers(1, &layer_fb);
    if (!layer_fb) return 0;
    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, layer_fb);
    // The attach that CANNOT silently succeed on the wrong texture. A layered
    // attach only works on an ARRAY or 3D texture; on a plain 2D texture
    // glFramebufferTextureLayer is INVALID_OPERATION and a NO-OP — it neither
    // attaches the new texture NOR detaches whatever was on this reused FBO from
    // the last call. The old code then saw the STALE attachment still complete
    // and returned it, so every 2D-texture probe read the first texture ever
    // attached (a bright eye) instead of its own. So: pick the attach that
    // matches the layer, then VERIFY the colour attachment is actually `tex`
    // before trusting COMPLETE — a stale-but-complete FBO must read as a miss.
    // OpenXR states layer=-1 ("not an array") for the per-eye-swapchain case, so
    // a NEGATIVE layer is a whole-2D-texture attach.
    //
    // r7: this said `layer > 0`, and layer 0 IS A LAYER. An array swapchain
    // gives both eyes one texture and puts the LEFT eye on slice 0, so the
    // left eye took the TEXTURE_2D branch -- glFramebufferTexture2D on an array
    // texture is INVALID_OPERATION and a no-op, the OBJECT_NAME check below
    // then correctly called it a stale attach, and the caller fell all the way
    // back to fb0. fb0 in a VR run is the pbuffer nothing draws into: black,
    // unchanging, and IDENTICAL ON EVERY CAPTURE.
    //
    // That is exactly what launch #6 produced. Four captures 600 frames apart,
    // while Austin was moving, reported `597046/12800000 lit` and `src=fb0
    // 4000x3200` four times with byte-identical files. The instrument built to
    // answer "is the world black" answered with a picture of something else,
    // and it would have done that whether the world was black or not.
    if (layer >= 0) {
        r_FramebufferTextureLayer(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */, tex, 0,
                                  layer);
    } else if (r_FramebufferTexture2D) {
        r_FramebufferTexture2D(0x8CA8, 0x8CE0, 0x0DE1 /* TEXTURE_2D */, tex, 0);
    } else {
        return 0;
    }
    if (r_CheckFramebufferStatus(0x8CA8) != 0x8CD5) return 0;
    if (r_GetFbAttachmentParam) {
        int32_t oname = 0;
        r_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x8CD1 /* OBJECT_NAME */, &oname);
        if ((uint32_t)oname != tex) return 0;  // stale attach, not ours
    }
    return layer_fb;
}

static uint32_t klfb_read_from_texture(uint32_t tex) {
    static void (*r_GenFramebuffers)(int32_t, uint32_t *);
    static void (*r_BindFramebuffer)(uint32_t, uint32_t);
    static void (*r_FramebufferTexture2D)(uint32_t, uint32_t, uint32_t, uint32_t,
                                          int32_t);
    static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
    if (!r_GenFramebuffers) {
        r_GenFramebuffers = asym("glGenFramebuffers");
        r_BindFramebuffer = asym("glBindFramebuffer");
        r_FramebufferTexture2D = asym("glFramebufferTexture2D");
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
    }
    if (!tex || !r_GenFramebuffers || !r_BindFramebuffer ||
        !r_FramebufferTexture2D || !r_CheckFramebufferStatus) return 0;
    static uint32_t probe_fb;
    if (!probe_fb) r_GenFramebuffers(1, &probe_fb);
    if (!probe_fb) return 0;
    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, probe_fb);
    r_FramebufferTexture2D(0x8CA8, 0x8CE0 /* COLOR_ATTACHMENT0 */,
                           0x0DE1 /* TEXTURE_2D */, tex, 0);
    if (r_CheckFramebufferStatus(0x8CA8) != 0x8CD5) return 0;
    return probe_fb;
}

// Is `fb` complete, and is its colour attachment one of the eye textures? The
// answer is the FBO itself (0 for no), and on a hit the eye's own size, which
// is not the pbuffer's — reading a 2748x2880 eye at the pbuffer's 4000x3200 is
// an out-of-bounds read that comes back black.
static uint32_t klfb_eye_fbo_take(uint32_t fb, int32_t *w, int32_t *h) {
    static uint32_t (*r_CheckFramebufferStatus)(uint32_t);
    static void (*r_BindFramebuffer)(uint32_t, uint32_t);
    static void (*r_GetFbAttachmentParam)(uint32_t, uint32_t, uint32_t, int32_t *);
    if (!r_CheckFramebufferStatus) {
        r_CheckFramebufferStatus = asym("glCheckFramebufferStatus");
        r_BindFramebuffer = asym("glBindFramebuffer");
        r_GetFbAttachmentParam = asym("glGetFramebufferAttachmentParameteriv");
    }
    if (!r_CheckFramebufferStatus || !r_BindFramebuffer || !r_GetFbAttachmentParam)
        return 0;
    r_BindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, fb);
    if (r_CheckFramebufferStatus(0x8CA8) != 0x8CD5) return 0;
    int32_t otype = 0, oname = 0;
    r_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x8CD0, &otype);
    r_GetFbAttachmentParam(0x8CA8, 0x8CE0, 0x8CD1, &oname);
    if (otype != 0x1702 /* TEXTURE */ ||
        ((uint32_t)oname != g_eye_tex[0] && (uint32_t)oname != g_eye_tex[1]))
        return 0;
    int32_t tw = 0, th = 0;
    if (klfb_tex_info((uint32_t)oname, NULL, &tw, &th) && tw > 0 && th > 0) {
        if (w) *w = tw;
        if (h) *h = th;
    }
    return fb;
}

static unsigned glfb_capture_now(const char *dir) {
    if (!g_ready || !a_glReadPixels) return 0;
    if (a_glFinish) a_glFinish();

    // Which framebuffer is the guest actually on at swap time? A black frame from
    // the default framebuffer means either nothing was drawn or it was drawn
    // somewhere else, and those need different fixes.
    int32_t draw_fb = -1, read_fb = -1, vp[4] = {0,0,0,0};
    int32_t fb_status = -1;
    static uint32_t (*a_glCheckFramebufferStatus)(uint32_t);
    static void (*a_BindFramebuffer)(uint32_t, uint32_t);
    static void (*a_GetFbAttachmentParam)(uint32_t, uint32_t, uint32_t, int32_t *);
    if (!a_glCheckFramebufferStatus) {
        a_glCheckFramebufferStatus = asym("glCheckFramebufferStatus");
        a_BindFramebuffer = asym("glBindFramebuffer");
        a_GetFbAttachmentParam = asym("glGetFramebufferAttachmentParameteriv");
    }
    if (a_glGetIntegerv) {
        a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &draw_fb);
        a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &read_fb);
        a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    }
    if (a_glCheckFramebufferStatus)
        fb_status = (int32_t)a_glCheckFramebufferStatus(0x8CA9 /* GL_DRAW_FRAMEBUFFER */);

    // The frame lives in the FBO whose color attachment is an eye texture:
    // fb0 is black by construction (the VR frame goes to eye textures, not
    // the backbuffer — the old capture read fb0 and reported 0 lit on runs
    // full of real draws). Find that FBO by its attachment and read it its
    // own way; fall back to the legacy read of whatever is bound when no eye
    // texture exists yet.
    uint32_t src_fb = 0;
    int32_t src_w = g_w, src_h = g_h;
    if ((g_eye_tex[0] || g_eye_tex[1]) && a_BindFramebuffer &&
        a_GetFbAttachmentParam && a_glCheckFramebufferStatus) {
        // A STATED image beats every search. kl_glfb_set_live_eye_image is the
        // OpenXR path saying which swapchain image the guest just presented,
        // with its size and array layer — none of which can be recovered here,
        // because the swapchain is allocated through the real ANGLE entry
        // points and both eyes are layers of one texture. Everything below is
        // the search for a guest that never states it (Unity through OVRPlugin).
        int cap_eye = kl_env_int("KL_XR_CAPTURE_EYE", 0);
        if (cap_eye < 0 || cap_eye > 1) cap_eye = 0;
        const typeof(g_live_eye[0]) *le = &g_live_eye[cap_eye];
        if (!le->tex && g_live_eye[cap_eye ^ 1].tex) le = &g_live_eye[cap_eye ^ 1];
        if (le->tex && le->w > 0 && le->h > 0) {
            src_fb = le->layer >= 0 ? klfb_read_from_texture_layer(le->tex, le->layer)
                                    : klfb_read_from_texture(le->tex);
            if (src_fb) { src_w = le->w; src_h = le->h; }
        }
        // The guest's own answer next (g_last_eye_fbo): the scan matches on
        // texture NAME, and a name outlives the object it was issued for.
        uint32_t hint = src_fb ? 0 : __atomic_load_n(&g_last_eye_fbo, __ATOMIC_RELAXED);
        if (hint) src_fb = klfb_eye_fbo_take(hint, &src_w, &src_h);
        for (uint32_t i = 1; i <= g_fbomax && !src_fb; i++)
            src_fb = klfb_eye_fbo_take(i, &src_w, &src_h);
        if (!src_fb) {
            // No framebuffer of the guest's has it attached any more — the
            // OpenXR case above. Read the named image directly rather than
            // falling back to fb0, which is black whatever happened.
            uint32_t live = g_eye_tex[0] ? g_eye_tex[0] : g_eye_tex[1];
            src_fb = klfb_read_from_texture(live);
            if (src_fb) {
                int32_t tw = 0, th = 0;
                if (klfb_tex_info(live, NULL, &tw, &th) && tw > 0 && th > 0) {
                    src_w = tw;
                    src_h = th;
                }
            } else {   // not found either: put the guest's binding back
                a_BindFramebuffer(0x8CA8, (uint32_t)(read_fb >= 0 ? read_fb : 0));
            }
        }
    }

    size_t   stride = (size_t)src_w * 4;
    uint8_t *px = malloc(stride * (size_t)src_h);
    if (!px) return 0;

    int32_t rfmt = 0, rtype = 0;
    if (a_glGetIntegerv) {
        a_glGetIntegerv(KLFB_IMPLEMENTATION_COLOR_READ_FORMAT, &rfmt);
        a_glGetIntegerv(KLFB_IMPLEMENTATION_COLOR_READ_TYPE, &rtype);
    }
    uint32_t pre_err = a_glGetError ? a_glGetError() : 0;
    int comps = (rfmt == 0x1907 /* GL_RGB */) ? 3 : 4;
    if (src_fb && (rtype == 0x1406 /* FLOAT */ || rtype == KLFB_HALF_FLOAT)) {
        // Float target: read in the implementation's own format/type and tone
        // map down to 8-bit (klfb_dbg_tone). A direct RGBA/UNSIGNED_BYTE read
        // here is the INVALID_ENUM the old capture died with. The half path
        // goes through a 64K-entry LUT: three powf per pixel per swap put the
        // tone map at the top of the profile (5M pixels at 2304x2198), and a
        // half float has only 16 bits of input to memoize.
        static uint8_t tone16[65536];
        static int tone16_ready;
        if (rtype == KLFB_HALF_FLOAT && !tone16_ready) {
            for (int i = 0; i < 65536; i++)
                tone16[i] = klfb_dbg_tone(klfb_half_to_float((uint16_t)i));
            tone16_ready = 1;
        }
        size_t esz = rtype == 0x1406 ? 4 : 2;
        void *rbuf = malloc((size_t)src_w * src_h * comps * esz);
        if (!rbuf) { free(px); return 0; }
        a_glReadPixels(0, 0, src_w, src_h, rfmt, rtype, rbuf);
        // Raw-value census, one line per 60 captures: is the eye texture dim
        // because the CONTENT is dim (fade still down — max/mean low) or
        // because the debug tone map undersells it (max near/over 1 while the
        // window shows murk)? KL_GLFB_RAWSTATS=0 turns the line off.
        {
            static int rawstats = -1;
            if (rawstats < 0) {
                rawstats = kl_env_int("KL_GLFB_RAWSTATS", 1);
            }
            static unsigned rs_n;
            if (rawstats && rs_n++ % 60 == 0) {
                float vmin = 1e30f, vmax = -1e30f; double vsum = 0;
                int32_t npx = src_w * src_h;
                for (int32_t i = 0; i < npx; i++) {
                    for (int j = 0; j < comps && j < 3; j++) {
                        float v = rtype == 0x1406
                                ? ((const float *)rbuf)[i * comps + j]
                                : klfb_half_to_float(((const uint16_t *)rbuf)[i * comps + j]);
                        if (v < vmin) vmin = v;
                        if (v > vmax) vmax = v;
                        vsum += v;
                    }
                }
                fprintf(stderr,
                        " [glfb] rawstats fb=%u %dx%d fmt=0x%x type=0x%x: "
                        "min=%.4f max=%.4f mean=%.5f\n",
                        src_fb, (int)src_w, (int)src_h, rfmt, rtype,
                        vmin, vmax, vsum / ((double)npx * (comps < 3 ? comps : 3)));
            }
        }
        if (rtype == KLFB_HALF_FLOAT && comps == 4) {
            const uint16_t *h = (const uint16_t *)rbuf;
            for (int32_t i = 0; i < src_w * src_h; i++) {
                px[i * 4 + 0] = tone16[h[i * 4 + 0]];
                px[i * 4 + 1] = tone16[h[i * 4 + 1]];
                px[i * 4 + 2] = tone16[h[i * 4 + 2]];
                px[i * 4 + 3] = 255;
            }
        } else {
            for (int32_t i = 0; i < src_w * src_h; i++) {
                float c[4] = {0, 0, 0, 1};
                for (int j = 0; j < comps; j++)
                    c[j] = rtype == 0x1406
                         ? ((float *)rbuf)[i * comps + j]
                         : klfb_half_to_float(((uint16_t *)rbuf)[i * comps + j]);
                px[i * 4 + 0] = klfb_dbg_tone(c[0]);
                px[i * 4 + 1] = klfb_dbg_tone(c[1]);
                px[i * 4 + 2] = klfb_dbg_tone(c[2]);
                px[i * 4 + 3] = 255;
            }
        }
        free(rbuf);
    } else {
        a_glReadPixels(0, 0, src_w, src_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        // OPAQUE, like the float path two branches up — and for the same
        // reason: this file is a PICTURE, not a copy of the buffer. An eye
        // layer is composited opaque by the runtime, so the guest has no reason
        // to author alpha and Beat Saber 1.40 writes 0 everywhere. Passing that
        // through wrote a fully transparent PNG over a correct RGB frame, which
        // every image viewer then showed as its own background — "mostly black"
        // or blank white, depending on the viewer, with the pixels right there.
        // The float path has always forced 255 and 1.28's eyes were RGBA16F, so
        // the difference arrived with the first 8-bit eye texture.
        for (size_t i = 0; i < (size_t)src_w * (size_t)src_h; i++)
            px[i * 4 + 3] = 255;
    }
    // The error could be the readback's own or one the guest left behind —
    // report both rather than blame the readback. Reading the pre-existing one
    // does clear it; this is a host-only diagnostic and the guest's errors on
    // the reference renderer are exactly what we want to see.
    uint32_t err = a_glGetError ? a_glGetError() : 0;
    char err_buf[64] = "";
    if (err) snprintf(err_buf, sizeof err_buf, " (GL error 0x%x%s)", err,
                      pre_err ? " — but pre-existing, not the readback's" : "");
    if (src_fb && read_fb >= 0 && a_BindFramebuffer)
        a_BindFramebuffer(0x8CA8, (uint32_t)read_fb);   // hand the binding back

    // Say whether anything landed. A frame that is uniformly one colour is a clear
    // with no draw, and that is a different outcome from a frame that drew — a
    // difference invisible in a thumbnail of a dark image.
    unsigned long sum = 0; unsigned lit = 0;
    for (size_t i = 0; i < (size_t)src_w * src_h; i++) {
        unsigned lum = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
        sum += lum;
        if (lum > 12) lit++;
    }
    g_last_frame_lit = lit;

    // ...and WHERE the light is, in sixteenths, top row first. A picture that
    // fills a quarter of the target and a target with nothing in it are the
    // same single "0 lit" number otherwise, and the census that produced that
    // number was also reading a 1280x800 corner of a 1536x1536 eye. Cheap
    // enough to be unconditional: one pass over a buffer already in cache.
    char blocks[128] = "";
    {
        unsigned long bsum[16] = {0};
        for (int32_t y = 0; y < src_h; y++) {
            int by = (int)((int64_t)y * 4 / src_h);
            if (by > 3) by = 3;
            const uint8_t *row = px + (size_t)y * src_w * 4;
            for (int32_t x = 0; x < src_w; x++) {
                int bx = (int)((int64_t)x * 4 / src_w);
                if (bx > 3) bx = 3;
                bsum[by * 4 + bx] += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
            }
        }
        unsigned long n = (unsigned long)src_w * (unsigned long)src_h / 16 * 3;
        size_t at = 0;
        for (int by = 3; by >= 0 && at + 24 < sizeof blocks; by--) {   // top row
            at += (size_t)snprintf(blocks + at, sizeof blocks - at, " |");
            for (int bx = 0; bx < 4; bx++)                             // ...first
                at += (size_t)snprintf(blocks + at, sizeof blocks - at, " %lu",
                                       n ? bsum[by * 4 + bx] / n : 0UL);
        }
    }

    // The sink half of the readback/output split: with a frontend registered
    // the buffer IS the output — hand it over and count the frame presented.
    // The sink runs on this (GL) thread inside the guest's frame, so it must
    // be a memcpy, not a render. Everything below stays the default output:
    // PNG to KL_GLFB_OUT, unchanged when no sink is registered.
    if (g_frame_sink) {
        // KL_GLFB_DUMP_SINK=<dir>: write every Nth sink frame to a PNG
        // (KL_GLFB_DUMP_SINK_EVERY, default 100). The sink and the PNG path
        // share this px buffer, so a window that shows black while these files
        // show content is an SDL-side problem, not a capture-side one.
        // The interval is a knob because the default is ~17 s of a 2D UI at
        // the shell's frame rate — far too coarse to see what a click did.
        static const char *sink_dir;
        static int sink_dir_init, sink_every;
        if (!sink_dir_init) {
            sink_dir = kl_env_str("KL_GLFB_DUMP_SINK", NULL);
            sink_every = kl_env_int("KL_GLFB_DUMP_SINK_EVERY", 100);
            if (sink_every < 1) sink_every = 1;
            sink_dir_init = 1;
        }
        if (sink_dir && g_presented % (unsigned)sink_every == 0) {
            char spath[600];
            snprintf(spath, sizeof spath, "%s/sink_%05u.png", sink_dir, g_presented);
            klfb_write_png(spath, px, src_w, src_h);
        }
        g_frame_sink(px, src_w, src_h, g_frame_sink_ctx);
        free(px);
        return ++g_presented;
    }

    char path[512];
    snprintf(path, sizeof path, "%s/frame_%03u.png", dir, g_presented);
    size_t   raw_n = (stride + 1) * (size_t)src_h;
    uint8_t *raw = malloc(raw_n);
    uLongf   cn  = compressBound((uLong)raw_n);
    uint8_t *cb  = malloc(cn);
    if (!raw || !cb) { free(px); free(raw); free(cb); return 0; }
    for (int y = 0; y < src_h; y++) {              // GL bottom-up -> PNG top-down
        raw[(stride + 1) * (size_t)y] = 0;
        memcpy(raw + (stride + 1) * (size_t)y + 1,
               px + stride * (size_t)(src_h - 1 - y), stride);
    }
    // Z_BEST_SPEED, not the default level: this runs on the GUEST'S RENDER
    // THREAD inside its frame, and on device a capture is ~18 MB of RGBA per
    // FBO. Default-level deflate on that is a multi-second freeze per capture;
    // level 1 is several times faster for a debug picture nobody archives.
    if (compress2(cb, &cn, raw, (uLong)raw_n, Z_BEST_SPEED) != Z_OK) {
        free(px); free(raw); free(cb); return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); free(raw); free(cb); return 0; }
    static const uint8_t sig[8] = {0x89,'P','N','G',13,10,26,10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, (uint32_t)src_w); be32(ih + 4, (uint32_t)src_h);
    ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", cb, (uint32_t)cn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    klfb_report_draws();
    // fb0 has been measured black with every draw landing on FBOs instead
    // (the VR frame goes to eye textures, not the backbuffer). Census the
    // FBOs the guest actually created — format-correctly, see klfb_probe_fbo
    // — and the one with lit pixels names where the picture is.
    {
        static void (*a_glBindFramebuffer)(uint32_t, uint32_t);
        if (!a_glBindFramebuffer) a_glBindFramebuffer = asym("glBindFramebuffer");
        // KL_GLFB_DUMP_FBOS=1: the census also writes one PNG per FBO
        // (frame_NNN_fbM.png, tone-mapped like the capture). Intermediates —
        // the post-processing chain, the MSAA scene target — are otherwise
        // only lit-counts on a log line.
        static int dump_fbos = -1;
        if (dump_fbos < 0) dump_fbos = kl_env_on("KL_GLFB_DUMP_FBOS", 0);
        if (a_glBindFramebuffer) {
            int32_t save_fb = read_fb >= 0 ? read_fb : 0;
            float  *fbuf = malloc((size_t)g_w * g_h * 4 * sizeof(float));
            uint8_t *bbuf = malloc((size_t)g_w * g_h * 4);
            uint8_t *dbuf = dump_fbos ? malloc((size_t)g_w * g_h * 4) : NULL;
            if (fbuf && bbuf) {
                for (uint32_t i = 1; i <= g_fbomax; i++) {
                    char note[160] = "";
                    int32_t dw = 0, dh = 0;
                    // No hint here on purpose: this is a SWEEP over every FBO,
                    // and a size borrowed from elsewhere would read past a
                    // small attachment into undefined pixels. A census must
                    // say it could not look rather than invent a lit count
                    //. The targeted probes (blit, draw, clear) each
                    // know a real size and pass it.
                    unsigned long flit = klfb_probe_fbo(i, fbuf, bbuf,
                                                        note, sizeof note,
                                                        dbuf, &dw, &dh, 0, 0);
                    if (!strcmp(note, "incomplete")) continue;
                    fprintf(stderr, "  [glfb] census fbo%u: %lu lit — %s\n",
                            i, flit, note);
                    if (dbuf && dw > 0 && dh > 0) {
                        char fpath[600];
                        snprintf(fpath, sizeof fpath, "%s/frame_%03u_fb%u.png",
                                 dir, g_presented, i);
                        klfb_write_png(fpath, dbuf, dw, dh);
                    }
                }
            }
            // ...and THE EYE IMAGES THEMSELVES, both slices, by name.
            //
            // Everything above is a sweep over the guest's OWN framebuffers,
            // and on the OpenXR path that question can be answered honestly
            // and still miss the picture: a swapchain rotates three images,
            // each with a slice per eye, and the FBO's attachment at present
            // time is whatever the guest last pointed it at. These two reads
            // remove the guest from the question -- the image it RELEASED,
            // attached to a framebuffer of ours, one file per eye.
            //
            // This is also the pair that catches a one-eye defect, which no
            // single capture can: the runtime picks ONE eye to be `frame_NNN`
            // (KL_XR_CAPTURE_EYE) and a world drawn into slice 1 only would
            // read as a black world from slice 0.
            if (fbuf && bbuf) {
                for (int e = 0; e < 2; e++) {
                    if (!g_live_eye[e].tex || g_live_eye[e].w <= 0 ||
                        g_live_eye[e].h <= 0) continue;
                    uint32_t efb = g_live_eye[e].layer >= 0
                        ? klfb_read_from_texture_layer(g_live_eye[e].tex,
                                                       g_live_eye[e].layer)
                        : klfb_read_from_texture(g_live_eye[e].tex);
                    if (!efb) {
                        fprintf(stderr, "  [glfb] eye%d image tex=%u layer=%d "
                                        "%dx%d COULD NOT BE ATTACHED for "
                                        "reading — no picture written\n",
                                e, g_live_eye[e].tex, g_live_eye[e].layer,
                                g_live_eye[e].w, g_live_eye[e].h);
                        continue;
                    }
                    char enote[160] = "";
                    int32_t ew = 0, eh = 0;
                    unsigned long elit = klfb_probe_fbo(efb, fbuf, bbuf,
                                                        enote, sizeof enote,
                                                        dbuf, &ew, &eh,
                                                        g_live_eye[e].w,
                                                        g_live_eye[e].h);
                    fprintf(stderr, "  [glfb] eye%d image tex=%u layer=%d: "
                                    "%lu lit of %dx%d — %s\n",
                            e, g_live_eye[e].tex, g_live_eye[e].layer, elit,
                            g_live_eye[e].w, g_live_eye[e].h, enote);
                    if (dbuf && ew > 0 && eh > 0) {
                        char epath[600];
                        snprintf(epath, sizeof epath, "%s/frame_%03u_eye%d.png",
                                 dir, g_presented, e);
                        klfb_write_png(epath, dbuf, ew, eh);
                    }
                }
            }
            free(fbuf); free(bbuf); free(dbuf);
            a_glBindFramebuffer(0x8CA8, (uint32_t)save_fb);
        }
    }
    uint64_t cap_tid = 0; pthread_threadid_np(NULL, &cap_tid);
    // src=fb0 IS THE FAILURE CASE AND IT USED TO READ LIKE A RESULT. fb0 in a
    // VR run is the pbuffer nothing draws into -- a capture that lands there
    // reports a lit count and a mean luma for a surface the guest has never
    // touched, and it reports the SAME ones every time. Launch #6 spent four
    // captures and a whole round on that. So the line says outright when the
    // eye image was not found.
    fprintf(stderr, "  [glfb] %s: %u/%u lit, mean luma %lu%s blocks%s "
                    "[draw_fb=%d read_fb=%d src=fb%u%s %dx%d fb_status=0x%x viewport %dx%d+%d+%d] (captured on t%llu)\n", path, lit,
            (unsigned)(src_w * src_h), sum / ((unsigned long)src_w * src_h * 3),
            err_buf, blocks, draw_fb, read_fb, src_fb,
            src_fb ? "" : " <<< THE EYE IMAGE WAS NOT FOUND — this is the "
                          "backbuffer, not the frame, and it is not evidence",
            src_w, src_h, fb_status,
            vp[2], vp[3], vp[0], vp[1],
            (unsigned long long)cap_tid);
    free(px); free(raw); free(cb);
    return ++g_presented;
}
