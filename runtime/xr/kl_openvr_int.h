// kl_openvr's internals, shared by kl_openvr.c (the bootstrap, System,
// Compositor, Chaperone, Mailbox and the all-stub interfaces) and
// kl_openvr_input.c (IVRInput). Not an API: nothing outside runtime/xr/
// includes this. kl_openvr.h is the API.
//
// Three things live here because both halves need them:
//
//   1. The OpenVR structures a method reads or writes, transcribed from the
//      public header (ValveSoftware/openvr v2.15.6, BSD-3) as C declarations of
//      our own. The header is not vendored, for kl_openxr.c's reason: a dozen
//      structs against 5,000 lines. Every layout carries a static assert with
//      the size the Linux aarch64 CALLER uses — see "packing" below.
//   2. The OpenXR subset kl_openvr calls, transcribed the same way. These are
//      kl_openxr.c's own transcriptions repeated (that file keeps its types
//      private); the spec fixes them, so the two copies cannot drift apart
//      without one being wrong against Khronos.
//   3. The slot-table machinery: one designated-initializer vtable per
//      interface, one logging stub per unimplemented slot.
//
// ---- packing ---------------------------------------------------------------
// openvr.h wraps its types in `#pragma pack(push, 8)` (natural alignment for
// everything here), EXCEPT five structs it re-packs to 4 "on __linux__ ||
// __APPLE__ ... originally defined mis-packed on Linux, preserved for
// compatibility": VREvent_t, VRControllerState001_t, Compositor_FrameTiming,
// RenderModel_TextureMap_t, RenderModel_t. The game is a Linux build, so pack 4
// is what it uses for those five, and since Darwin is in the same #if a
// Darwin compile of the public header would agree. The sizes asserted below
// are the ones measured at libvr's own call sites (OPENVR-VTABLES.md §4):
// PollNextEventWithPose passes 60 (= sizeof VREvent_t under pack 4; 64 under
// 8), GetControllerState passes 60, GetFrameTimings stores m_nSize = 188
// (pack 4; 192 under 8). The input structs (digital 24, analog 48) match the
// reference host's `size=24` / `size=48` telemetry.
//
// ---- ABI ---------------------------------------------------------------------
// The callers are Linux-AAPCS64 C++ virtual calls: `this` in x0, then the
// declared arguments. Two Darwin/Linux differences matter and both are handled
// at the method, not by a wrapper:
//   - Sub-word integer ARGUMENTS (bool, unsigned short) are not extended by a
//     Linux caller; an Apple callee assumes they were. Every such parameter is
//     declared uint32_t here and masked in the body (KLVR_BOOL / & 0xffff).
//   - HmdMatrix34_t / HmdMatrix44_t are returned in memory through x8 (16+
//     floats is not an HFA, >16 bytes). A plain C function returning the struct
//     by value IS that convention on arm64 Darwin, so those methods are written
//     exactly so and nothing sits between the caller and them.
//     HiddenAreaMesh_t (16 bytes, pointer + uint32) comes back in x0:x1, and
//     HmdColor_t (four floats, an HFA) in s0-s3 — also just C.
// Stack-passed arguments are where Darwin and Linux disagree most (Darwin packs
// them by natural size), and no slot here has one: the widest called method,
// GetComponentStateForBinding, is `this` + 6 integer arguments.
#ifndef KL_OPENVR_INT_H
#define KL_OPENVR_INT_H

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include "kl_openvr.h"

// =============================================================== OpenVR types
typedef struct { float m[3][4]; } klvr_HmdMatrix34_t;
typedef struct { float m[4][4]; } klvr_HmdMatrix44_t;
typedef struct { float v[3]; } klvr_HmdVector3_t;
typedef struct { float v[2]; } klvr_HmdVector2_t;
typedef struct { float r, g, b, a; } klvr_HmdColor_t;
typedef struct { klvr_HmdVector3_t vCorners[4]; } klvr_HmdQuad_t;
typedef struct { float rfRed[2], rfGreen[2], rfBlue[2]; } klvr_DistortionCoordinates_t;
typedef struct { const klvr_HmdVector2_t *pVertexData; uint32_t unTriangleCount; }
        klvr_HiddenAreaMesh_t;

// C++ bool is one byte on both sides; uint8_t says so without stdbool.
typedef struct {
    klvr_HmdMatrix34_t mDeviceToAbsoluteTracking;
    klvr_HmdVector3_t  vVelocity, vAngularVelocity;
    int32_t eTrackingResult;
    uint8_t bPoseIsValid, bDeviceIsConnected;
} klvr_TrackedDevicePose_t;

#pragma pack(push, 4)
typedef struct {
    uint32_t eventType, trackedDeviceIndex;
    float    eventAgeSeconds;
    uint8_t  data[48];          // VREvent_Data_t, a union whose widest member is 48
} klvr_VREvent_t;
typedef struct {
    uint32_t unPacketNum;
    uint64_t ulButtonPressed, ulButtonTouched;
    struct { float x, y; } rAxis[5];
} klvr_VRControllerState_t;
typedef struct {
    uint32_t m_nSize, m_nFrameIndex, m_nNumFramePresents, m_nNumMisPresented,
             m_nNumDroppedFrames, m_nReprojectionFlags;
    double   m_flSystemTimeInSeconds;
    float    m_flPreSubmitGpuMs, m_flPostSubmitGpuMs, m_flTotalRenderGpuMs,
             m_flCompositorRenderGpuMs, m_flCompositorRenderCpuMs,
             m_flCompositorIdleCpuMs, m_flClientFrameIntervalMs,
             m_flPresentCallCpuMs, m_flWaitForPresentCpuMs, m_flSubmitFrameMs,
             m_flWaitGetPosesCalledMs, m_flNewPosesReadyMs, m_flNewFrameReadyMs,
             m_flCompositorUpdateStartMs, m_flCompositorUpdateEndMs,
             m_flCompositorRenderStartMs;
    klvr_TrackedDevicePose_t m_HmdPose;
    uint32_t m_nNumVSyncsReadyForUse, m_nNumVSyncsToFirstView;
    float    m_flTransferLatencyMs;
} klvr_Compositor_FrameTiming;
#pragma pack(pop)

typedef struct { void *handle; int32_t eType, eColorSpace; } klvr_Texture_t;
typedef struct { float uMin, vMin, uMax, vMax; } klvr_VRTextureBounds_t;
typedef struct {
    uint64_t m_nImage;                              // VkImage
    void *m_pDevice, *m_pPhysicalDevice, *m_pInstance, *m_pQueue;
    uint32_t m_nQueueFamilyIndex;
    uint32_t m_nWidth, m_nHeight, m_nFormat, m_nSampleCount;
} klvr_VRVulkanTextureData_t;
typedef struct {
    klvr_VRVulkanTextureData_t base;
    uint32_t m_unArrayIndex, m_unArraySize;
} klvr_VRVulkanTextureArrayData_t;
// Texture_t, then (Submit_TextureWithPose, 0x08) the pose it was rendered at,
// then (Submit_TextureWithDepth, 0x10) VRTextureDepthInfo_t, then (0x200,
// motion) VRTextureMotionInfo_t. The game submits 0x218 = all three. Without
// the pose flag the depth info follows Texture_t directly (VRTextureWithDepth_t,
// offset 16) — klvr_submit_decode reads by offset, this struct is the 0x218 view.
typedef struct {
    klvr_Texture_t     tex;
    klvr_HmdMatrix34_t mDeviceToAbsoluteTracking;
    struct { void *handle; klvr_HmdMatrix44_t mProjection; klvr_HmdVector2_t vRange; } depth;
    struct { void *handle; klvr_HmdMatrix44_t mDeltaPose; } motion;
} klvr_VRTextureWithMotion_t;

// IVRInput's structs: natural alignment (outside every pack region).
typedef struct { uint8_t bActive; uint64_t activeOrigin; uint8_t bState, bChanged;
                 float fUpdateTime; } klvr_InputDigitalActionData_t;
typedef struct { uint8_t bActive; uint64_t activeOrigin; float x, y, z;
                 float deltaX, deltaY, deltaZ; float fUpdateTime; } klvr_InputAnalogActionData_t;
typedef struct { uint8_t bActive; uint64_t activeOrigin; klvr_TrackedDevicePose_t pose; }
        klvr_InputPoseActionData_t;
typedef struct { uint8_t bActive; uint64_t activeOrigin; } klvr_InputSkeletalActionData_t;
typedef struct { uint64_t devicePath; uint32_t trackedDeviceIndex;
                 char rchRenderModelComponentName[128]; } klvr_InputOriginInfo_t;
typedef struct { uint64_t ulActionSet, ulRestrictedToDevice, ulSecondaryActionSet;
                 uint32_t unPadding; int32_t nPriority; } klvr_VRActiveActionSet_t;
typedef struct { float flFingerCurl[5], flFingerSplay[4]; } klvr_VRSkeletalSummaryData_t;

_Static_assert(sizeof(klvr_HmdMatrix34_t) == 48, "HmdMatrix34_t");
_Static_assert(sizeof(klvr_HmdMatrix44_t) == 64, "HmdMatrix44_t");
_Static_assert(sizeof(klvr_HiddenAreaMesh_t) == 16, "HiddenAreaMesh_t (x0:x1)");
_Static_assert(sizeof(klvr_TrackedDevicePose_t) == 80, "TrackedDevicePose_t");
_Static_assert(sizeof(klvr_VREvent_t) == 60, "VREvent_t: libvr passes 60 (pack 4)");
_Static_assert(sizeof(klvr_VRControllerState_t) == 60, "VRControllerState_t: libvr passes 60");
_Static_assert(sizeof(klvr_Compositor_FrameTiming) == 188, "Compositor_FrameTiming: m_nSize 188");
_Static_assert(sizeof(klvr_Texture_t) == 16, "Texture_t");
_Static_assert(sizeof(klvr_VRVulkanTextureData_t) == 64, "VRVulkanTextureData_t");
_Static_assert(sizeof(klvr_VRVulkanTextureArrayData_t) == 72, "VRVulkanTextureArrayData_t");
_Static_assert(__builtin_offsetof(klvr_VRTextureWithMotion_t, depth) == 64, "WithPose");
_Static_assert(__builtin_offsetof(klvr_VRTextureWithMotion_t, motion) == 144, "WithPoseAndDepth");
_Static_assert(sizeof(klvr_InputDigitalActionData_t) == 24, "digital: host size=24");
_Static_assert(sizeof(klvr_InputAnalogActionData_t) == 48, "analog: host size=48");
_Static_assert(sizeof(klvr_InputPoseActionData_t) == 96, "InputPoseActionData_t");
_Static_assert(sizeof(klvr_InputOriginInfo_t) == 144, "InputOriginInfo_t");
_Static_assert(sizeof(klvr_VRActiveActionSet_t) == 32, "VRActiveActionSet_t: host size=32");
_Static_assert(sizeof(klvr_VRSkeletalSummaryData_t) == 36, "VRSkeletalSummaryData_t");

// The handful of enum values used by name.
enum {
    KLVR_DEVICE_HMD = 0, KLVR_DEVICE_LEFT = 1, KLVR_DEVICE_RIGHT = 2,
    KLVR_MAX_DEVICES = 64,                      // k_unMaxTrackedDeviceCount
    KLVR_ORIGIN_SEATED = 0, KLVR_ORIGIN_STANDING = 1, KLVR_ORIGIN_RAW = 2,
    KLVR_TRACKING_UNINITIALIZED = 1, KLVR_TRACKING_RUNNING_OK = 200,
    KLVR_TRACKING_RUNNING_OUT_OF_RANGE = 201,
    KLVR_EVENT_QUIT = 700,
    KLVR_COMPOSITOR_OK = 0, KLVR_COMPOSITOR_REQUEST_FAILED = 1,
    KLVR_COMPOSITOR_INDEX_OUT_OF_RANGE = 107,
    // EVRInputError
    KLVR_INPUT_OK = 0, KLVR_INPUT_NAME_NOT_FOUND = 1, KLVR_INPUT_WRONG_TYPE = 2,
    KLVR_INPUT_INVALID_HANDLE = 3, KLVR_INPUT_INVALID_PARAM = 4,
    KLVR_INPUT_INVALID_DEVICE = 9, KLVR_INPUT_NO_DATA = 13,
    KLVR_INPUT_BUFFER_TOO_SMALL = 14,
};

// ================================================================ OpenXR subset
// The calls kl_openvr makes, typed. See kl_openxr.c for every layout's
// provenance; only the ones used here are repeated.
typedef int32_t  klxr_Result;
typedef uint64_t klxr_Path;
typedef struct { float x, y, z, w; } klxr_Quat;
typedef struct { float x, y, z; } klxr_Vec3;
typedef struct { klxr_Quat orientation; klxr_Vec3 position; } klxr_Posef;
typedef struct { float angleLeft, angleRight, angleUp, angleDown; } klxr_Fovf;

typedef struct { int32_t type; void *next; uint64_t locationFlags; klxr_Posef pose; }
        klxr_SpaceLocation;
typedef struct { int32_t type; void *next; uint64_t velocityFlags;
                 klxr_Vec3 linearVelocity, angularVelocity; } klxr_SpaceVelocity;
typedef struct { int32_t type; const void *next; char actionName[64]; int32_t actionType;
                 uint32_t countSubactionPaths; const klxr_Path *subactionPaths;
                 char localizedActionName[128]; } klxr_ActionCreateInfo;
typedef struct { int32_t type; const void *next; void *action; klxr_Path subactionPath; }
        klxr_ActionStateGetInfo;
typedef struct { int32_t type; void *next; uint32_t currentState, changedSinceLastSync;
                 int64_t lastChangeTime; uint32_t isActive; } klxr_ActionStateBoolean;
typedef struct { int32_t type; void *next; float currentState; uint32_t changedSinceLastSync;
                 int64_t lastChangeTime; uint32_t isActive; } klxr_ActionStateFloat;
typedef struct { int32_t type; const void *next; void *action; klxr_Path subactionPath; }
        klxr_HapticActionInfo;
typedef struct { int32_t type; const void *next; int64_t duration;
                 float frequency, amplitude; } klxr_HapticVibration;
typedef struct { void *actionSet; klxr_Path subactionPath; } klxr_ActiveActionSet;
typedef struct { int32_t type; const void *next; uint32_t countActiveActionSets;
                 const klxr_ActiveActionSet *activeActionSets; } klxr_ActionsSyncInfo;

enum {
    KLXR_T_SPACE_LOCATION = 42, KLXR_T_SPACE_VELOCITY = 43,
    KLXR_T_ACTION_STATE_BOOLEAN = 23, KLXR_T_ACTION_STATE_FLOAT = 24,
    KLXR_T_ACTION_STATE_GET_INFO = 58, KLXR_T_ACTIONS_SYNC_INFO = 61,
    KLXR_T_HAPTIC_ACTION_INFO = 59, KLXR_T_HAPTIC_VIBRATION = 13,
    KLXR_T_ACTION_CREATE_INFO = 29,
};
enum { KLXR_LOC_ORIENTATION_VALID = 0x1, KLXR_LOC_POSITION_VALID = 0x2,
       KLXR_LOC_ORIENTATION_TRACKED = 0x4, KLXR_LOC_POSITION_TRACKED = 0x8 };

// The raw controller surface kl_openvr reads every OpenVR action out of: one
// OpenXR action per Touch control, both hands as subaction paths, bound to
// /interaction_profiles/oculus/touch_controller. Every OpenVR digital/analog/
// pose/haptic answer is computed from these per UpdateActionState, so the
// OpenVR binding semantics (thresholds, dead zones, dpad sectors) live in
// kl_openvr_input.c and kl_openxr stays the controller.
enum {
    KLVR_RAW_TRIGGER, KLVR_RAW_SQUEEZE, KLVR_RAW_STICK_X, KLVR_RAW_STICK_Y,
    KLVR_RAW_LOWER, KLVR_RAW_UPPER, KLVR_RAW_STICK_CLICK, KLVR_RAW_MENU,
    KLVR_RAW_GRIP_POSE, KLVR_RAW_AIM_POSE, KLVR_RAW_HAPTIC, KLVR_RAW_N
};

// One hand's controls, sampled at UpdateActionState.
typedef struct {
    float trigger, squeeze, sx, sy;
    int   lower, upper, stick_click, menu;
    int   present;                  // the grip pose action reported active
} klvr_hand_raw;

// ================================================================= shared state
typedef struct {
    int      inited;                // between VR_InitInternal2 and VR_ShutdownInternal
    uint32_t token;
    void    *instance;
    uint64_t system;
    void    *session;
    int      session_state;         // last XrSessionState seen through xrPollEvent
    int      running;               // xrBeginSession accepted
    void    *space_stage, *space_local, *space_view;
    void    *raw_set;
    void    *raw[KLVR_RAW_N];
    void    *grip_space[2], *aim_space[2];
    klxr_Path hand_path[2];
    uint32_t rec_w, rec_h;          // xrEnumerateViewConfigurationViews
    int      origin;                // IVRCompositor tracking space
    // The last frame's poses, in the compositor's origin.
    klvr_TrackedDevicePose_t last_render[KLVR_MAX_DEVICES];
    int64_t  display_time;          // the open frame's predictedDisplayTime
} klvr_state;
extern klvr_state klvr;

// One lock for kl_openxr and this file's state. xrWaitFrame is called with it
// RELEASED (it may block for the display), everything else under it.
extern pthread_mutex_t klvr_lock;

// The OpenXR entry points, resolved through kl_openxr's xrGetInstanceProcAddr
// (KLVR_XR_FUNCS is the name list the resolver walks; the struct is the typed
// view of the same names — prototyped, so no float is ever default-promoted).
#define KLVR_XR_FUNCS(X)                                                        \
    X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance)               \
    X(xrDestroyInstance) X(xrGetSystem) X(xrGetSystemProperties)                \
    X(xrEnumerateViewConfigurationViews) X(xrGetVulkanGraphicsRequirementsKHR)  \
    X(xrCreateSession) X(xrDestroySession) X(xrBeginSession) X(xrEndSession)    \
    X(xrPollEvent) X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateSpace) \
    X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews)               \
    X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet)                \
    X(xrCreateAction) X(xrSuggestInteractionProfileBindings)                    \
    X(xrAttachSessionActionSets) X(xrCreateActionSpace) X(xrSyncActions)        \
    X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStatePose) \
    X(xrApplyHapticFeedback) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages)                         \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage)
typedef struct {
    klxr_Result (*xrEnumerateInstanceExtensionProperties)(const char *, uint32_t,
                                                          uint32_t *, void *);
    klxr_Result (*xrCreateInstance)(const void *, void **);
    klxr_Result (*xrDestroyInstance)(void *);
    klxr_Result (*xrGetSystem)(void *, const void *, uint64_t *);
    klxr_Result (*xrGetSystemProperties)(void *, uint64_t, void *);
    klxr_Result (*xrEnumerateViewConfigurationViews)(void *, uint64_t, int32_t, uint32_t,
                                                     uint32_t *, void *);
    klxr_Result (*xrGetVulkanGraphicsRequirementsKHR)(void *, uint64_t, void *);
    klxr_Result (*xrCreateSession)(void *, const void *, void **);
    klxr_Result (*xrDestroySession)(void *);
    klxr_Result (*xrBeginSession)(void *, const void *);
    klxr_Result (*xrEndSession)(void *);
    klxr_Result (*xrPollEvent)(void *, void *);
    klxr_Result (*xrCreateReferenceSpace)(void *, const void *, void **);
    klxr_Result (*xrDestroySpace)(void *);
    klxr_Result (*xrLocateSpace)(void *, void *, int64_t, void *);
    klxr_Result (*xrWaitFrame)(void *, const void *, void *);
    klxr_Result (*xrBeginFrame)(void *, const void *);
    klxr_Result (*xrEndFrame)(void *, const void *);
    klxr_Result (*xrLocateViews)(void *, const void *, void *, uint32_t, uint32_t *, void *);
    klxr_Result (*xrStringToPath)(void *, const char *, klxr_Path *);
    klxr_Result (*xrCreateActionSet)(void *, const void *, void **);
    klxr_Result (*xrDestroyActionSet)(void *);
    klxr_Result (*xrCreateAction)(void *, const void *, void **);
    klxr_Result (*xrSuggestInteractionProfileBindings)(void *, const void *);
    klxr_Result (*xrAttachSessionActionSets)(void *, const void *);
    klxr_Result (*xrCreateActionSpace)(void *, const void *, void **);
    klxr_Result (*xrSyncActions)(void *, const void *);
    klxr_Result (*xrGetActionStateBoolean)(void *, const void *, void *);
    klxr_Result (*xrGetActionStateFloat)(void *, const void *, void *);
    klxr_Result (*xrGetActionStatePose)(void *, const void *, void *);
    klxr_Result (*xrApplyHapticFeedback)(void *, const void *, const void *);
    klxr_Result (*xrEnumerateSwapchainFormats)(void *, uint32_t, uint32_t *, int64_t *);
    klxr_Result (*xrCreateSwapchain)(void *, const void *, void **);
    klxr_Result (*xrDestroySwapchain)(void *);
    klxr_Result (*xrEnumerateSwapchainImages)(void *, uint32_t, uint32_t *, void *);
    klxr_Result (*xrAcquireSwapchainImage)(void *, const void *, uint32_t *);
    klxr_Result (*xrWaitSwapchainImage)(void *, const void *);
    klxr_Result (*xrReleaseSwapchainImage)(void *, const void *);
} klvr_xr_table;

// The eye swapchains and the projection layer the compositor's frame end
// submits (XR_KHR_vulkan_enable images; the layer is the spec's).
typedef struct { int32_t type; const void *next; uint64_t createFlags, usageFlags;
                 int64_t format; uint32_t sampleCount, width, height, faceCount,
                 arraySize, mipCount; } klxr_SwapchainCreateInfo;
typedef struct { int32_t type; void *next; uint64_t image; } klxr_SwapchainImageVulkan;
typedef struct { void *swapchain; int32_t x, y, w, h; uint32_t imageArrayIndex; }
        klxr_SwapchainSubImage;
typedef struct { int32_t type; const void *next; klxr_Posef pose; klxr_Fovf fov;
                 klxr_SwapchainSubImage subImage; } klxr_ProjectionView;
typedef struct { int32_t type; const void *next; uint64_t layerFlags; void *space;
                 uint32_t viewCount; const klxr_ProjectionView *views; } klxr_ProjectionLayer;
_Static_assert(sizeof(klxr_SwapchainCreateInfo) == 64, "XrSwapchainCreateInfo");
_Static_assert(sizeof(klxr_ProjectionView) == 96, "XrCompositionLayerProjectionView");
_Static_assert(__builtin_offsetof(klxr_ProjectionView, subImage) == 64, "subImage");
_Static_assert(sizeof(klxr_ProjectionLayer) == 48, "XrCompositionLayerProjection");
enum {
    KLXR_T_SWAPCHAIN_CREATE_INFO = 9, KLXR_T_SWAPCHAIN_IMAGE_ACQUIRE_INFO = 55,
    KLXR_T_SWAPCHAIN_IMAGE_WAIT_INFO = 56, KLXR_T_SWAPCHAIN_IMAGE_RELEASE_INFO = 57,
    KLXR_T_COMPOSITION_LAYER_PROJECTION = 35, KLXR_T_COMPOSITION_LAYER_PROJECTION_VIEW = 48,
    KLXR_T_SWAPCHAIN_IMAGE_VULKAN = 1000025001,
};
extern klvr_xr_table klvr_xr;

// ================================================================ the helpers
void klvr_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  klvr_log_this(uint64_t n);        // calls 0-3 and powers of two
int64_t klvr_now_ns(void);
// A pose for one device in `origin`, at the open frame's display time (or the
// last one located). Fills the OpenVR struct; returns bPoseIsValid.
int  klvr_locate_device(uint32_t device, int origin, int64_t xr_time,
                        klvr_TrackedDevicePose_t *out);
void *klvr_origin_space(int origin);
void klvr_pose_from_xr(const klxr_SpaceLocation *l, const klxr_SpaceVelocity *v,
                       klvr_TrackedDevicePose_t *out);
// Is the controller for `hand` there (its grip action active)?
int  klvr_hand_present(int hand);
// The controller pose OpenVR hands the game — SteamVR's /user/hand/*/pose/raw,
// which is what Alyx binds handpose{left,right} to — out of the OpenXR grip
// pose, as the reference host (libalyxhost_f1_m1) derives it: grip composed
// with the inverse of the Touch grip-in-raw offset. `lin`/`ang` (base frame,
// may be NULL) are the grip's velocities; `lin` is moved to the raw origin
// (v + w x r). KL_OPENVR_HAND_POSE=grip hands over the grip unchanged. Pure.
klxr_Posef klvr_hand_raw_from_grip(int hand, klxr_Posef grip, const float ang[3],
                                   float lin[3]);
// The offset itself (grip in raw), for the test's transcription of the reference.
klxr_Posef klvr_touch_grip_in_raw(int hand);
// IVRInput's half of the init, called by kl_openvr.c under the lock.
void klvr_input_reset(void);
// kl_openvr_config.flatten_manifest, as configured (NULL if none).
int (*klvr_config_flattener(void))(const char *, char **, FILE *);

// ============================================================ slot machinery
//
// Each interface is one X-list of (TAG, slot, Method, KIND). KIND is IMPL for a
// method this runtime implements (its function is klvr_<TAG>_<Method>) or one
// of the stub kinds, which name what the slot RETURNS so the zero lands in the
// register the caller reads:
//   Z    x0 = 0          (void, bool, integers, enums, pointers, error codes)
//   E13  x0 = 13         (IVRInput's VRInputError_NoData, as the reference)
//   F    s0 = 0.0f       (float)
//   C    s0..s3 = 0      (HmdColor_t, a four-float HFA)
//   S    x0 = ""         (const char * name-of-enum: an empty string, never NULL,
//                         because a caller formats it)
// Every stub is its own function, so the log line names its slot from the
// function's own constants — never from a shared trampoline guessing at which
// entry it was reached through.
typedef struct {
    const char *name;
    unsigned    slot;
    unsigned char kind;             // KLVR_KIND_*
    uint64_t    calls;
    const void *first_caller;
} klvr_slotrec;
enum { KLVR_KIND_IMPL, KLVR_KIND_Z, KLVR_KIND_E13, KLVR_KIND_F, KLVR_KIND_C, KLVR_KIND_S };

typedef struct {
    const char       *version;      // "IVRSystem_026"
    void *const      *vtable;
    unsigned          slots;
    klvr_slotrec     *recs;
    void             *object;
} klvr_iface_def;

// What a stub does: count, log on 0-3 and powers of two (x1..x5, s0, s1 and the
// return address, as image+offset when it is a guest image).
void klvr_stub_hit(const char *iface, klvr_slotrec *r, const void *ret,
                   uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                   double f0, double f1);
// ...and an implemented slot's counter, for the end-of-run census.
#define KLVR_COUNT(T, N)                                                         \
    (void)__atomic_fetch_add(&g_rec_##T[N].calls, 1, __ATOMIC_RELAXED)

#define KLVR_STUBDEF_IMPL(T, N, M)
#define KLVR_STUB_PARAMS void *self, uint64_t a1, uint64_t a2, uint64_t a3,        \
                         uint64_t a4, uint64_t a5, float f0, float f1
#define KLVR_STUB_HIT(T, N)                                                      \
    (void)self;                                                                  \
    klvr_stub_hit(g_ver_##T, &g_rec_##T[N], __builtin_return_address(0),         \
                  a1, a2, a3, a4, a5, f0, f1)
#define KLVR_STUBDEF_Z(T, N, M)                                                  \
    static uint64_t klvr_stub_##T##_##N(KLVR_STUB_PARAMS) { KLVR_STUB_HIT(T, N); return 0; }
#define KLVR_STUBDEF_E13(T, N, M)                                                \
    static uint64_t klvr_stub_##T##_##N(KLVR_STUB_PARAMS) { KLVR_STUB_HIT(T, N); return 13; }
#define KLVR_STUBDEF_F(T, N, M)                                                  \
    static float klvr_stub_##T##_##N(KLVR_STUB_PARAMS) { KLVR_STUB_HIT(T, N); return 0.0f; }
#define KLVR_STUBDEF_C(T, N, M)                                                  \
    static klvr_HmdColor_t klvr_stub_##T##_##N(KLVR_STUB_PARAMS) {               \
        KLVR_STUB_HIT(T, N); klvr_HmdColor_t c = { 0, 0, 0, 0 }; return c; }
#define KLVR_STUBDEF_S(T, N, M)                                                  \
    static const char *klvr_stub_##T##_##N(KLVR_STUB_PARAMS) { KLVR_STUB_HIT(T, N); return ""; }

#define KLVR_VT_IMPL(T, N, M) (void *)klvr_##T##_##M
#define KLVR_VT_Z(T, N, M)    (void *)klvr_stub_##T##_##N
#define KLVR_VT_E13(T, N, M)  (void *)klvr_stub_##T##_##N
#define KLVR_VT_F(T, N, M)    (void *)klvr_stub_##T##_##N
#define KLVR_VT_C(T, N, M)    (void *)klvr_stub_##T##_##N
#define KLVR_VT_S(T, N, M)    (void *)klvr_stub_##T##_##N

#define KLVR_X_STUBDEF(T, N, M, K) KLVR_STUBDEF_##K(T, N, M)
#define KLVR_X_VT(T, N, M, K)      [N] = KLVR_VT_##K(T, N, M),
#define KLVR_X_REC(T, N, M, K)     [N] = { #M, N, KLVR_KIND_##K, 0, 0 },
#define KLVR_X_PLUS1(T, N, M, K)   + 1

// Everything one interface needs from its X-list, in two halves because the
// implementations sit between them (they count into g_rec_<T>, and the vtable
// names them): RECS first — the version string and the census records — then
// TABLE — the stubs, the vtable, the object, and two compile-time checks: the
// list has exactly NSLOTS rows AND the highest designated slot is NSLOTS-1.
// Together with -Winitializer-overrides (a duplicate slot) that is "exactly
// slots 0..N-1, once each"; the unit test checks no entry is NULL as well.
#define KLVR_INTERFACE_RECS(T, VERSION, NSLOTS, LIST)                            \
    static const char g_ver_##T[] = VERSION;                                     \
    static klvr_slotrec g_rec_##T[NSLOTS] = { LIST(KLVR_X_REC) }
#define KLVR_INTERFACE_TABLE(T, VERSION, NSLOTS, LIST)                           \
    LIST(KLVR_X_STUBDEF)                                                         \
    static void *const g_vt_##T[] = { LIST(KLVR_X_VT) };                          \
    _Static_assert(0 LIST(KLVR_X_PLUS1) == (NSLOTS), VERSION ": row count");     \
    _Static_assert(sizeof g_vt_##T / sizeof g_vt_##T[0] == (NSLOTS),             \
                   VERSION ": slot count");                                      \
    static struct { void *const *vtbl; const char *version; } g_obj_##T =        \
        { g_vt_##T, g_ver_##T }
#define KLVR_IFACE_DEF(T, NSLOTS)                                                \
    { g_ver_##T, g_vt_##T, (NSLOTS), g_rec_##T, &g_obj_##T }

// kl_openvr_input.c's interface, for the registry in kl_openvr.c.
const klvr_iface_def *klvr_input_iface(void);

#endif
