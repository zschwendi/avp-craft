// The OpenVR front door — libopenvr_api.so, answered by the runtime.
//
// A guest built against Valve's OpenVR SDK links `libopenvr_api.so`, whose job
// is to find a SteamVR install (openvrpaths.vrpath), load its vrclient and hand
// back the C++ interface objects vrclient implements. There is no SteamVR here
// and nothing to find, so the soname is REPLACED rather than loaded — the same
// argument that replaced libOVRPlugin and the Khronos OpenXR loader — and the
// interfaces are served by this module, as a translation layer over kl_openxr:
// IVRSystem's projection and device properties out of the OpenXR system and
// views, IVRCompositor's frame loop out of xrWaitFrame/xrBeginFrame/
// xrLocateViews/xrEndFrame, IVRInput's actions out of OpenXR actions on the
// Touch profile. Same category of work as kl_openxr.c serving OpenXR.
//
// The guest binds the C++ vtables directly (`this` in x0, slot N at vtable+8N),
// not openvr_capi.h's FnTables, so every served interface is a C object whose
// first word is a vtable with EXACTLY the slot count and order of the
// interface version it is served under (hla/OPENVR-VTABLES.md). A slot this
// runtime does not implement is its own logging stub — interface, slot index,
// method name, caller — so a first run states which slots the guest uses.
//
// Generic: nothing here knows which title is running. A door that wants a
// particular device identity, render size or action-manifest reader says so
// through kl_openvr_configure before the guest calls VR_InitInternal2.
#ifndef KL_OPENVR_H
#define KL_OPENVR_H
#include <stdint.h>
#include <stdio.h>

// The eight libopenvr_api.so names a guest imports (VR_InitInternal2,
// VR_ShutdownInternal, VR_GetInitToken, VR_GetGenericInterface,
// VR_IsInterfaceVersionValid, VR_GetVRInitErrorAsSymbol,
// VR_GetVRInitErrorAsEnglishDescription, VRControlPanel). NULL for any other
// name. Registered for the soname in kl_dl.c's replaced-soname table.
void *kl_openvr_sym(const char *name);

// What a door may say about the device it presents. Every field is optional:
// NULL / 0 takes the default noted. Strings are copied.
typedef struct {
    const char *tracking_system;    // Prop_TrackingSystemName_String  "oculus"
    const char *manufacturer;       // Prop_ManufacturerName_String    "Meta"
    const char *model;              // Prop_ModelNumber_String         "Quest 3"
    const char *serial;             // Prop_SerialNumber_String        "KLEPTON-OPENXR"
    const char *render_model;       // Prop_RenderModelName_String     "quest3"
    const char *controller_type;    // Prop_ControllerType_String      "oculus_touch"
    const char *runtime_version;    // IVRSystem::GetRuntimeVersion    "2.16.0"
    uint32_t render_width;          // GetRecommendedRenderTargetSize; 0 = OpenXR's
    uint32_t render_height;
    // The action manifest reader. IVRInput::SetActionManifestPath hands it the
    // HOST spelling of the guest's path; it returns 0 and a malloc'd record
    // text (the S/A/P/H/C format, which kl_openvr_input.c parses) or non-zero
    // having logged why. NULL: the manifest is accepted and every action reads
    // inactive. kl_openvr_manifest_flatten (below) is the stock one.
    int (*flatten_manifest)(const char *manifest_path, char **text_out, FILE *log);
} kl_openvr_config;
void kl_openvr_configure(const kl_openvr_config *cfg);

// The stock flatten_manifest (kl_openvr_manifest.c): a SteamVR actions.json
// and the oculus_touch binding it declares, beside it, flattened to records.
int kl_openvr_manifest_flatten(const char *manifest_path, char **text_out, FILE *log);

// The Vulkan objects the guest renders with, for the OpenXR session. The
// session is created at VR_InitInternal2 — before the guest has a VkDevice —
// so it starts with no device of its own (kl_vulkan then makes swapchain
// images on the first device, which in a single-device guest IS the
// renderer's); calling this re-points swapchain images at `device`. The first
// IVRCompositor::Submit calls it with the VRVulkanTextureData_t's objects (A5's
// "v1 binding, deferred"), so a door need not; a later Submit naming another
// device re-binds and rebuilds the eye swapchains on it.
void kl_openvr_bind_vulkan(void *instance, void *physical_device, void *device,
                           uint32_t queue_family, uint32_t queue_index);

// Queue an OpenVR event (VREvent_t.eventType, trackedDeviceIndex) for
// IVRSystem::PollNextEvent. 0, or -1 if the queue is full. A door uses it for
// VREvent_Quit (700) when the app is told to close.
int kl_openvr_post_event(uint32_t type, uint32_t device);

// The eye the guest renders (round F2, foveation): the size of the texture it
// last handed IVRCompositor::Submit (Alyx: both eyes side by side — NOT
// GetRecommendedRenderTargetSize x 2, Valve rounds its own size), and each
// eye's frustum as tangents (left < 0 < right, down < 0 < up), the numbers
// GetProjectionRaw hands the guest. Returns 0 before the first Submit.
int kl_openvr_eye_geometry(uint32_t *w, uint32_t *h, float tan_lrdu[2][4]);
// 1 if the guest has handed this VkImage to Submit (its final composite, which
// is never an eye render target for foveation's purposes).
int kl_openvr_is_submitted_image(uint64_t vk_image);

// Where kl_openvr's lines go (default stderr).
void kl_openvr_set_log(FILE *f);

// Every slot the guest called, per interface — implemented or stub, count and
// first caller. The first live run's answer to "is the called set inside the
// doc's game column".
void kl_openvr_report(FILE *f);

// The eye copy's destination, for a readback (the unit test; a frame dump): the
// VkImage of `eye`'s OpenXR swapchain that the last Submit copied into, its
// size and VkFormat. It is on the device the session is bound to and is left in
// VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL (what OpenXR expects at release).
// Returns 0 when that eye has not been copied yet.
int kl_openvr_eye_image(int eye, uint64_t *vk_image, uint32_t *width, uint32_t *height,
                        int64_t *vk_format);

// Introspection, for the unit test and the report: the served (non-NULL)
// interfaces in registry order.
int  kl_openvr_iface_count(void);
const char *kl_openvr_iface_version(int i);
unsigned    kl_openvr_iface_slots(int i);
// Slot `slot` of interface `version`: its method name, whether it is a stub,
// and how many times it has been called. 0 on success, -1 if out of range.
int kl_openvr_slot_info(const char *version, unsigned slot, const char **name,
                        int *is_stub, uint64_t *calls);

#endif
