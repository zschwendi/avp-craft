// libvloader.dylib stub for the macOS T2/T3 run: JNI for org.vivecraft.util.VLoader (QuestCraft Vivecraft).
// The Klepton build returns the real ANGLE EGLDisplay/EGLConfig/EGLContext of the GLFW stand-in (M1 T4/T6);
// here EGL handles are 0 (Mac GL context is CGL) and the Dalvik VM/activity are non-null dummies, which is all
// kl_openxr's xrInitializeLoaderKHR / XR_KHR_android_create_instance need.
#include <jni.h>
#include <stdio.h>
static char dummy_vm[64], dummy_activity[64];
#define LOG(...) do { fprintf(stderr, "[vloader-stub] " __VA_ARGS__); fputc('\n', stderr); } while (0)
JNIEXPORT jlong JNICALL Java_org_vivecraft_util_VLoader_getEGLContext(JNIEnv *e, jclass c) { LOG("getEGLContext -> 0"); return 0; }
JNIEXPORT jlong JNICALL Java_org_vivecraft_util_VLoader_getEGLConfig(JNIEnv *e, jclass c) { LOG("getEGLConfig -> 0"); return 0; }
JNIEXPORT jlong JNICALL Java_org_vivecraft_util_VLoader_getEGLDisplay(JNIEnv *e, jclass c) { LOG("getEGLDisplay -> 0"); return 0; }
JNIEXPORT jlong JNICALL Java_org_vivecraft_util_VLoader_getDalvikVM(JNIEnv *e, jclass c) { LOG("getDalvikVM -> %p (dummy)", (void *)dummy_vm); return (jlong)dummy_vm; }
JNIEXPORT jlong JNICALL Java_org_vivecraft_util_VLoader_getDalvikActivity(JNIEnv *e, jclass c) { LOG("getDalvikActivity -> %p (dummy)", (void *)dummy_activity); return (jlong)dummy_activity; }
JNIEXPORT void JNICALL Java_org_vivecraft_util_VLoader_setupAndroid(JNIEnv *e, jclass c) { LOG("setupAndroid (no-op)"); }
