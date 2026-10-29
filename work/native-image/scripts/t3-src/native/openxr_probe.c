// libopenxr_loader.dylib PROBE for the macOS T2/T3 run (Darwin stand-in for the Klepton shim, M1 T6).
// LWJGL's XR.create() dlsym's only xrGetInstanceProcAddr; everything else is fetched through it. This probe logs
// every lookup and the arguments of the calls Vivecraft's DeviceCompat.Mobile path makes, i.e. exactly the seam
// that the Klepton shim (xrGetInstanceProcAddr -> kl_openxr) must serve.
//   KL_XR_PROBE_FORWARD=<path to a real libopenxr_loader.dylib>: forward every lookup to it (log results);
//   otherwise serve the four loader-level globals the way kl_openxr would, and FAIL xrCreateInstance with
//   XR_ERROR_RUNTIME_UNAVAILABLE (what a loader returns with no runtime), so the run stops right at the seam.
#include <openxr/openxr.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) do { fprintf(stderr, "[xr-probe] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
// Android structs (openxr_platform.h needs jni/android headers for these; layouts are trivial)
#define TYPE_LOADER_INIT_INFO_ANDROID_KHR 1000089000
#define TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR 1000008000
typedef struct { XrStructureType type; const void *next; void *applicationVM; void *applicationContext; } LoaderInitInfoAndroid;
typedef struct { XrStructureType type; const void *next; void *applicationVM; void *applicationActivity; } InstanceCreateInfoAndroid;
typedef struct { XrStructureType type; const void *next; } BaseIn;

static PFN_xrGetInstanceProcAddr real_gipa;
static PFN_xrVoidFunction real_init, real_enum_ext, real_create;

static void forward_init(void) {
  static int done; if (done) return; done = 1;
  const char *p = getenv("KL_XR_PROBE_FORWARD");
  if (!p || !*p) { LOG("mode: standalone (serves loader globals, xrCreateInstance -> XR_ERROR_RUNTIME_UNAVAILABLE)"); return; }
  void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
  if (!h) { LOG("mode: forward FAILED dlopen(%s): %s", p, dlerror()); return; }
  real_gipa = (PFN_xrGetInstanceProcAddr)dlsym(h, "xrGetInstanceProcAddr");
  LOG("mode: forward to %s (xrGetInstanceProcAddr=%p)", p, (void *)real_gipa);
}

static const char *const kExts[] = { // what kl_openxr advertises unconditionally on the GLES path (kl_openxr.c:809-810)
  "XR_KHR_opengl_es_enable", "XR_KHR_android_create_instance", "XR_KHR_loader_init", "XR_KHR_loader_init_android" };

static XrResult XRAPI_CALL p_xrEnumerateApiLayerProperties(uint32_t cap, uint32_t *count, XrApiLayerProperties *props) {
  LOG("xrEnumerateApiLayerProperties(cap=%u) -> 0 layers", cap); if (count) *count = 0; return XR_SUCCESS;
}
static XrResult XRAPI_CALL p_xrEnumerateInstanceExtensionProperties(const char *layer, uint32_t cap, uint32_t *count, XrExtensionProperties *props) {
  XrResult r;
  if (real_enum_ext) r = ((PFN_xrEnumerateInstanceExtensionProperties)real_enum_ext)(layer, cap, count, props);
  else {
    uint32_t n = sizeof kExts / sizeof *kExts; *count = n; r = XR_SUCCESS;
    if (cap) { if (cap < n) r = XR_ERROR_SIZE_INSUFFICIENT; else for (uint32_t i = 0; i < n; i++) {
      strncpy(props[i].extensionName, kExts[i], XR_MAX_EXTENSION_NAME_SIZE - 1); props[i].extensionVersion = 1; } }
  }
  LOG("xrEnumerateInstanceExtensionProperties(layer=%s, cap=%u) -> %d, count=%u", layer ? layer : "NULL", cap, r, count ? *count : 0);
  if (cap && r == XR_SUCCESS) for (uint32_t i = 0; i < *count; i++) LOG("    ext %s v%u", props[i].extensionName, props[i].extensionVersion);
  return r;
}
static XrResult XRAPI_CALL p_xrInitializeLoaderKHR(const XrLoaderInitInfoBaseHeaderKHR *info) {
  const LoaderInitInfoAndroid *a = (const void *)info;
  LOG("xrInitializeLoaderKHR(type=%d%s, next=%p, applicationVM=%p, applicationContext=%p)", info->type,
      info->type == TYPE_LOADER_INIT_INFO_ANDROID_KHR ? " LOADER_INIT_INFO_ANDROID_KHR" : "", info->next, a->applicationVM, a->applicationContext);
  XrResult r = real_init ? ((PFN_xrInitializeLoaderKHR)real_init)(info) : XR_SUCCESS;
  LOG("  -> %d", r); return r;
}
static XrResult XRAPI_CALL p_xrCreateInstance(const XrInstanceCreateInfo *ci, XrInstance *out) {
  LOG("xrCreateInstance: app='%s' v%u engine='%s' apiVersion=%u.%u.%u flags=0x%llx layers=%u exts=%u",
      ci->applicationInfo.applicationName, ci->applicationInfo.applicationVersion, ci->applicationInfo.engineName,
      (unsigned)XR_VERSION_MAJOR(ci->applicationInfo.apiVersion), (unsigned)XR_VERSION_MINOR(ci->applicationInfo.apiVersion),
      (unsigned)XR_VERSION_PATCH(ci->applicationInfo.apiVersion), (unsigned long long)ci->createFlags,
      ci->enabledApiLayerCount, ci->enabledExtensionCount);
  for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) LOG("    enabled ext %s", ci->enabledExtensionNames[i]);
  for (const BaseIn *n = ci->next; n; n = n->next) {
    if (n->type == TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR) {
      const InstanceCreateInfoAndroid *a = (const void *)n;
      LOG("    next: XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR applicationVM=%p applicationActivity=%p", a->applicationVM, a->applicationActivity);
    } else LOG("    next: type %d", n->type);
  }
  XrResult r = real_create ? ((PFN_xrCreateInstance)real_create)(ci, out) : XR_ERROR_RUNTIME_UNAVAILABLE;
  LOG("  -> %d%s", r, r == XR_ERROR_RUNTIME_UNAVAILABLE ? " (XR_ERROR_RUNTIME_UNAVAILABLE)" : "");
  return r;
}

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance inst, const char *name, PFN_xrVoidFunction *fn) {
  forward_init();
  XrResult r = XR_ERROR_FUNCTION_UNSUPPORTED; PFN_xrVoidFunction mine = NULL, got = NULL;
  if (!strcmp(name, "xrEnumerateApiLayerProperties") && !real_gipa) mine = (PFN_xrVoidFunction)p_xrEnumerateApiLayerProperties;
  if (!strcmp(name, "xrEnumerateInstanceExtensionProperties")) mine = (PFN_xrVoidFunction)p_xrEnumerateInstanceExtensionProperties;
  if (!strcmp(name, "xrInitializeLoaderKHR")) mine = (PFN_xrVoidFunction)p_xrInitializeLoaderKHR;
  if (!strcmp(name, "xrCreateInstance")) mine = (PFN_xrVoidFunction)p_xrCreateInstance;
  if (real_gipa) {
    r = real_gipa(inst, name, &got);
    if (mine == (PFN_xrVoidFunction)p_xrEnumerateInstanceExtensionProperties) real_enum_ext = got;
    else if (mine == (PFN_xrVoidFunction)p_xrInitializeLoaderKHR) real_init = got;
    else if (mine == (PFN_xrVoidFunction)p_xrCreateInstance) real_create = got;
    if (mine && !got) mine = NULL;   // real loader does not provide it: report that faithfully
    *fn = mine ? mine : got;
  } else {
    *fn = mine; r = mine ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
  }
  LOG("xrGetInstanceProcAddr(instance=0x%llx, \"%s\") -> %d, %s", (unsigned long long)inst, name, r, *fn ? "non-NULL" : "NULL");
  return r;
}
