/*
 * klegl_bridge.c -- libklegl.dylib: libltw's "libEGL" inside the Klepton visionOS app (T8b of the Vivecraft port).
 *
 * ltw takes its whole host GL/EGL surface from ONE function, eglGetProcAddress, of the library it dlopens at load time
 * ($LTW_EGL_LIBRARY, work/natives/ltw/darwin/ltw_darwin_proc.h). Pointing that at ANGLE directly would give the game
 * its own ANGLE display and context, outside the app: no compositor, and a share group kl_openxr's swapchains are not
 * in. So the app points it HERE, and primes this with its own lookup (kl_app.c native_egl_proc) before ltw loads:
 *   egl*        -> kl_egl's table (the guest-facing EGL; kl_glfb backs its contexts with real ANGLE ones)
 *   gl queries  -> ANGLE's real entry points (ltw must see an honest ES 3.0 and the real extension list)
 *   other gl*   -> kl_egl_sym (kl_glfb's thunks / ANGLE), NULL when ANGLE has no such entry point
 * Unprimed, eglGetProcAddress answers NULL and ltw's constructor aborts by name (error_sysegl / error_init), which is
 * the loud failure we want rather than a silent second GL stack.
 */
#include <stddef.h>
#include <stdio.h>
#define KL_EXPORT __attribute__((visibility("default")))
typedef void *(*klegl_proc)(const char *);
static klegl_proc g_proc;
KL_EXPORT void klegl_bridge_set(klegl_proc p) { g_proc = p; }
KL_EXPORT void *eglGetProcAddress(const char *name) {
    if (!g_proc) {
        static int said;
        if (!said++) fprintf(stderr, "[klegl] eglGetProcAddress(%s) before klegl_bridge_set -- the app did not prime "
                                     "libklegl before libltw loaded\n", name ? name : "(null)");
        return NULL;
    }
    return g_proc(name);
}
