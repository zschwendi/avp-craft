/* libopenxr_loader.dylib: the Darwin-native twin of kl_dl.c:187's
   { "libopenxr_loader.so", "kl_openxr", kl_openxr_sym } claim. LWJGL-OpenXR 3.3.3 (XR.create ->
   XR$GlobalCommands) dlsyms ONLY xrGetInstanceProcAddr and gets xrCreateInstance,
   xrEnumerateInstanceExtensionProperties, xrEnumerateApiLayerProperties and xrInitializeLoaderKHR through
   xrGetInstanceProcAddr(XR_NULL_HANDLE, ...). The other four exports are for loaders that dlsym them.
   kl_openxr_sym lives in the Klepton app image; it is bound at load time by flat lookup (-Wl,-U). */
#include <stdint.h>
#include <stddef.h>
#define XR_EXPORT __attribute__((visibility("default")))
typedef int32_t XrResult;
enum { XR_ERROR_RUNTIME_UNAVAILABLE = -51 };
extern void *kl_openxr_sym(const char *name) __attribute__((weak_import));
/* Opt this guest into kl_openxr's spec-strict input: parent-path bindings,
   boolean thresholds, and activeActionSets (kl_openxr.h, kl_openxr_set_spec_input;
   vivecraft/notes/openxr-contract.md). Only Vivecraft comes through this door, so
   doing it here leaves every APK guest's semantics alone. Weak, so an older
   runtime without the setter still loads. */
extern void kl_openxr_set_spec_input(int on) __attribute__((weak_import));
/* M2: the app primes both pointers explicitly at boot (kl_app.c native_boot, before the game exists), so the
   door never depends on the app image EXPORTING kl_openxr_sym — a Release/stripped device build may not, and a
   flat lookup that misses is a silent XR_ERROR_RUNTIME_UNAVAILABLE. The weak flat-namespace imports stay as the
   fallback for host tests (t_xrvivecraft links the runtime into the test executable). */
static void *(*g_sym)(const char *);
static void (*g_spec)(int);
XR_EXPORT void klxr_shim_set(void *(*sym)(const char *), void (*spec)(int)) { g_sym = sym; g_spec = spec; }
static void *kx(const char *n) {
    static int once;
    void *(*sym)(const char *) = g_sym ? g_sym : (kl_openxr_sym ? kl_openxr_sym : NULL);
    void (*spec)(int) = g_spec ? g_spec : (kl_openxr_set_spec_input ? kl_openxr_set_spec_input : NULL);
    if (!once) { once = 1; if (spec) spec(1); }
    return sym ? sym(n) : NULL;
}
#define FWD(name, params, args) XR_EXPORT XrResult name params { \
    static XrResult (*f) params; if (!f) f = (XrResult (*) params)kx(#name); \
    return f ? f args : XR_ERROR_RUNTIME_UNAVAILABLE; }
/* xrGetInstanceProcAddr writes *function on every path, as the spec requires.
   LWJGL reads the pointer back, so it must not see stack garbage when no runtime is linked. */
XR_EXPORT XrResult xrGetInstanceProcAddr(void *instance, const char *name, void **function) {
    static XrResult (*f)(void *, const char *, void **);
    if (!f) f = (XrResult (*)(void *, const char *, void **))kx("xrGetInstanceProcAddr");
    if (!f) { if (function) *function = NULL; return XR_ERROR_RUNTIME_UNAVAILABLE; }
    return f(instance, name, function);
}
FWD(xrInitializeLoaderKHR, (const void *loaderInitInfo), (loaderInitInfo))
FWD(xrEnumerateApiLayerProperties, (uint32_t cap, uint32_t *count, void *props), (cap, count, props))
FWD(xrEnumerateInstanceExtensionProperties, (const char *layer, uint32_t cap, uint32_t *count, void *props), (layer, cap, count, props))
FWD(xrCreateInstance, (const void *createInfo, void **instance), (createInfo, instance))
