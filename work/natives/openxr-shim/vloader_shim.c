/* libvloader.dylib: org.vivecraft.util.VLoader natives (QuestCraft VivecraftMod OpenXR-1.21.5,
   common/.../util/VLoader.java) + pojlib.util.VLoader.setAndroidInitInfo, matching the APK's libvloader.so
   (pojlib src/main/jni/vloader.cpp). EGL comes from whoever owns the GL context in the app (libkl_glfw /
   ANGLE / kl_egl): it may hand us its eglGetProcAddress via vloader_set_egl_proc, else RTLD_DEFAULT.
   kl_openxr never dereferences the Dalvik VM/activity (kl_openxr.c klxr_InitializeLoaderKHR, klxr_CreateInstance),
   so the VM is this JVM's own JavaVM* and the activity is whatever setAndroidInitInfo stored (or a sentinel). */
#include <dlfcn.h>
#include <jni.h>
#include <stdint.h>
#define EXPORT __attribute__((visibility("default")))
typedef void *(*proc_fn)(const char *);
static proc_fn g_proc;
static jobject g_activity;
EXPORT void vloader_set_egl_proc(proc_fn p) { g_proc = p; }
static void *egl(const char *n) { void *f = g_proc ? g_proc(n) : NULL; return f ? f : dlsym(RTLD_DEFAULT, n); }

EXPORT jlong JNICALL Java_org_vivecraft_util_VLoader_getEGLDisplay(JNIEnv *e, jclass c) {
    void *(*f)(void) = (void *(*)(void))egl("eglGetCurrentDisplay"); return f ? (jlong)(intptr_t)f() : 0; }
EXPORT jlong JNICALL Java_org_vivecraft_util_VLoader_getEGLContext(JNIEnv *e, jclass c) {
    void *(*f)(void) = (void *(*)(void))egl("eglGetCurrentContext"); return f ? (jlong)(intptr_t)f() : 0; }
EXPORT jlong JNICALL Java_org_vivecraft_util_VLoader_getEGLConfig(JNIEnv *e, jclass c) {
    /* same attribs as vloader.cpp: RGBA8, depth 24, ES3, pbuffer */
    static const int32_t attribs[] = {0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24,
                                      0x3040, 0x0040, 0x3033, 0x0001, 0x3038};
    void *(*cur)(void) = (void *(*)(void))egl("eglGetCurrentDisplay");
    unsigned (*choose)(void *, const int32_t *, void **, int32_t, int32_t *) =
        (unsigned (*)(void *, const int32_t *, void **, int32_t, int32_t *))egl("eglChooseConfig");
    void *cfg = NULL; int32_t n = 0;
    if (cur && choose) choose(cur(), attribs, &cfg, 1, &n);
    return (jlong)(intptr_t)cfg;
}
EXPORT jlong JNICALL Java_org_vivecraft_util_VLoader_getDalvikVM(JNIEnv *e, jclass c) {
    JavaVM *vm = NULL; (*e)->GetJavaVM(e, &vm); return (jlong)(intptr_t)vm; }
EXPORT jlong JNICALL Java_org_vivecraft_util_VLoader_getDalvikActivity(JNIEnv *e, jclass c) {
    return g_activity ? (jlong)(intptr_t)g_activity : (jlong)(intptr_t)"klepton-activity"; }
/* QuestCraft sets pojlib.API.gameReady so the Unity launcher yields its XR session. No Unity here:
   set it if the class exists, otherwise nothing to hand over. */
EXPORT void JNICALL Java_org_vivecraft_util_VLoader_setupAndroid(JNIEnv *e, jclass c) {
    jclass api = (*e)->FindClass(e, "pojlib/API");
    if (!api) { (*e)->ExceptionClear(e); return; }
    jfieldID f = (*e)->GetStaticFieldID(e, api, "gameReady", "Z");
    if (f) (*e)->SetStaticBooleanField(e, api, f, JNI_TRUE); else (*e)->ExceptionClear(e);
}
EXPORT void JNICALL Java_pojlib_util_VLoader_setAndroidInitInfo(JNIEnv *e, jclass c, jobject ctx) {
    g_activity = ctx ? (*e)->NewGlobalRef(e, ctx) : NULL; }
