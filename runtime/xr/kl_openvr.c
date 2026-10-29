// libopenvr_api.so, answered by the runtime — see kl_openvr.h for what and why.
//
// This file: the eight bootstrap exports, the interface registry, and the
// objects behind IVRSystem_026, IVRCompositor_029, IVRChaperone_004 and
// IVRMailbox_001, plus the all-stub ones (Overlay, Applications, OverlayView,
// RenderModels, HeadsetView, XrProto, InputInternal). IVRInput_011 is
// kl_openvr_input.c. The slot tables are kl_openvr_slots.h; the structures and
// the slot machinery are kl_openvr_int.h.
//
// What sits underneath is kl_openxr, reached the way any OpenXR app reaches it
// (xrGetInstanceProcAddr), so every pose and every frame boundary this file
// hands out is one kl_openxr also answered — there is one head, one clock and
// one controller state in the runtime, whichever API a guest speaks.
//
// The session. VR_InitInternal2 creates the instance, the session and the
// spaces, and walks the session to FOCUSED before it returns, so the first
// WaitGetPoses already has a frame loop to wait on. The session is bound to
// "the guest's Vulkan device" before the guest HAS one (the renderer creates
// its VkDevice after VR init); kl_openxr only records the binding, and
// kl_vulkan makes swapchain images on the bound device or, unbound, on the
// first one — which in a single-device guest is the renderer's.
// kl_openvr_bind_vulkan re-points it once the device is known, and the first
// Submit is who calls it: the VRVulkanTextureData_t names the renderer's
// instance, physical device, device and queue (A5, "v1 binding, deferred").
//
// How a frame ends. WaitGetPoses opens one (xrWaitFrame + xrBeginFrame). Each
// Submit copies its eye out of the guest's image into that eye's OpenXR
// swapchain image, on the guest's own queue; the second eye's Submit ends the
// frame with ONE projection layer whose two views carry the poses this frame's
// WaitGetPoses handed out (or the pose a Submit_TextureWithPose names).
// PostPresentHandoff, then the next WaitGetPoses, end a frame still open — with
// the layer if both eyes were copied, with ZERO layers otherwise. That keeps
// kl_openxr's wait/begin/end strictly paired, so the session never stalls on a
// frame left open by a guest that submitted one eye or none.
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../gfx/kl_perf.h"
#include "kl_openvr_int.h"
#include "kl_openvr_slots.h"
#include "kl_openxr.h"
#include "kl_ovrp.h"
#include "kl_vulkan.h"
#include "kl_env.h"
#include "klepton.h"

// The eye copy speaks Vulkan; a checkout with no MoltenVK headers builds the
// rest and refuses Submit by name (kl_vulkan.c makes the same split).
#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#define KLVR_HAVE_VK 1
#else
#define KLVR_HAVE_VK 0
#endif

klvr_state      klvr;
klvr_xr_table   klvr_xr;
pthread_mutex_t klvr_lock = PTHREAD_MUTEX_INITIALIZER;

#define KLVR_BOOL(x) (((x) & 0xffu) != 0)     // a Linux caller's unwidened bool

// ------------------------------------------------------------------- logging
static FILE *g_log;
void kl_openvr_set_log(FILE *f) { g_log = f; }

void klvr_log(const char *fmt, ...) {
    FILE *f = g_log ? g_log : stderr;
    va_list ap;
    va_start(ap, fmt);
    fputs("  [openvr] ", f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fflush(f);
}

// Calls 0-3 and every power of two: a per-frame method stays legible across a
// whole boot and its count is still readable off the last line.
int klvr_log_this(uint64_t n) { return n < 4 || (n & (n - 1)) == 0; }

int64_t klvr_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);        // kl_openxr's XrTime clock
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// A return address, as image+offset when it is inside a guest image.
static void klvr_where(const void *pc, char *buf, size_t cap) {
    size_t off = 0;
    const char *img = pc ? kl_addr_image(pc, &off) : NULL;
    if (img) snprintf(buf, cap, "%p (%s+0x%zx)", pc, img, off);
    else     snprintf(buf, cap, "%p", pc);
}

void klvr_stub_hit(const char *iface, klvr_slotrec *r, const void *ret,
                   uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                   double f0, double f1) {
    uint64_t n = __atomic_fetch_add(&r->calls, 1, __ATOMIC_RELAXED);
    if (n == 0) r->first_caller = ret;
    if (!klvr_log_this(n)) return;
    char where[160];
    klvr_where(ret, where, sizeof where);
    klvr_log("unimplemented %s slot=%u %s call#=%llu caller=%s x1=0x%llx x2=0x%llx "
             "x3=0x%llx x4=0x%llx x5=0x%llx s0=%g s1=%g",
             iface, r->slot, r->name, (unsigned long long)n, where,
             (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3,
             (unsigned long long)a4, (unsigned long long)a5, f0, f1);
}

// ------------------------------------------------------------------- config
static struct {
    char tracking_system[64], manufacturer[64], model[64], serial[64],
         render_model[64], controller_type[64], runtime_version[32];
    uint32_t render_w, render_h;
    int (*flatten)(const char *, char **, FILE *);
} g_cfg = { "oculus", "Meta", "Quest 3", "KLEPTON-OPENXR", "quest3", "oculus_touch",
            "2.16.0", 0, 0, NULL };

void kl_openvr_configure(const kl_openvr_config *c) {
    if (!c) return;
#define CP(f, src) if (c->src) snprintf(g_cfg.f, sizeof g_cfg.f, "%s", c->src)
    CP(tracking_system, tracking_system); CP(manufacturer, manufacturer);
    CP(model, model); CP(serial, serial); CP(render_model, render_model);
    CP(controller_type, controller_type); CP(runtime_version, runtime_version);
#undef CP
    g_cfg.render_w = c->render_width;
    g_cfg.render_h = c->render_height;
    g_cfg.flatten  = c->flatten_manifest;
}

// kl_openvr_input.c reads the manifest flattener through this.
int (*klvr_config_flattener(void))(const char *, char **, FILE *) { return g_cfg.flatten; }

// ------------------------------------------------------------------ the math
static klvr_HmdMatrix34_t klvr_identity34(void) {
    klvr_HmdMatrix34_t m;
    memset(&m, 0, sizeof m);
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
    return m;
}

// A pose as OpenVR's row-major 3x4 (rotation | translation).
static klvr_HmdMatrix34_t klvr_mat34(klxr_Posef p) {
    float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z,
          w = p.orientation.w;
    klvr_HmdMatrix34_t m;
    m.m[0][0] = 1 - 2 * (y * y + z * z); m.m[0][1] = 2 * (x * y - z * w);
    m.m[0][2] = 2 * (x * z + y * w);     m.m[0][3] = p.position.x;
    m.m[1][0] = 2 * (x * y + z * w);     m.m[1][1] = 1 - 2 * (x * x + z * z);
    m.m[1][2] = 2 * (y * z - x * w);     m.m[1][3] = p.position.y;
    m.m[2][0] = 2 * (x * z - y * w);     m.m[2][1] = 2 * (y * z + x * w);
    m.m[2][2] = 1 - 2 * (x * x + y * y); m.m[2][3] = p.position.z;
    return m;
}

// ...and back: the rotation's quaternion (Shepperd's branch on the largest
// diagonal term, so no division by a near-zero) and the translation column.
static klxr_Posef klvr_pose_of(const klvr_HmdMatrix34_t *m) {
    const float (*r)[4] = m->m;
    float t = r[0][0] + r[1][1] + r[2][2], x, y, z, w;
    if (t > 0) {
        float s = sqrtf(t + 1.0f) * 2;
        w = 0.25f * s; x = (r[2][1] - r[1][2]) / s;
        y = (r[0][2] - r[2][0]) / s; z = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        float s = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2;
        w = (r[2][1] - r[1][2]) / s; x = 0.25f * s;
        y = (r[0][1] + r[1][0]) / s; z = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        float s = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2;
        w = (r[0][2] - r[2][0]) / s; x = (r[0][1] + r[1][0]) / s;
        y = 0.25f * s; z = (r[1][2] + r[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2;
        w = (r[1][0] - r[0][1]) / s; x = (r[0][2] + r[2][0]) / s;
        y = (r[1][2] + r[2][1]) / s; z = 0.25f * s;
    }
    float n = sqrtf(x * x + y * y + z * z + w * w);
    if (n < 1e-6f) { x = y = z = 0; w = n = 1; }
    klxr_Posef p = { { x / n, y / n, z / n, w / n }, { r[0][3], r[1][3], r[2][3] } };
    return p;
}

// a ∘ b: b expressed in a's frame (the head's pose, then the eye in the head).
static klxr_Posef klvr_pose_mul(klxr_Posef a, klxr_Posef b) {
    klvr_HmdMatrix34_t m = klvr_mat34(a);
    klxr_Posef o;
    o.position.x = m.m[0][0] * b.position.x + m.m[0][1] * b.position.y +
                   m.m[0][2] * b.position.z + m.m[0][3];
    o.position.y = m.m[1][0] * b.position.x + m.m[1][1] * b.position.y +
                   m.m[1][2] * b.position.z + m.m[1][3];
    o.position.z = m.m[2][0] * b.position.x + m.m[2][1] * b.position.y +
                   m.m[2][2] * b.position.z + m.m[2][3];
    const klxr_Quat *p = &a.orientation, *q = &b.orientation;
    o.orientation.w = p->w * q->w - p->x * q->x - p->y * q->y - p->z * q->z;
    o.orientation.x = p->w * q->x + p->x * q->w + p->y * q->z - p->z * q->y;
    o.orientation.y = p->w * q->y - p->x * q->z + p->y * q->w + p->z * q->x;
    o.orientation.z = p->w * q->z + p->x * q->y - p->y * q->x + p->z * q->w;
    return o;
}

// The inverse of a rigid pose: conjugate rotation, translation rotated back.
static klxr_Posef klvr_pose_inv(klxr_Posef p) {
    klxr_Posef i = { { -p.orientation.x, -p.orientation.y, -p.orientation.z, p.orientation.w },
                     { 0, 0, 0 } };
    klvr_HmdMatrix34_t m = klvr_mat34(i);
    i.position.x = -(m.m[0][0] * p.position.x + m.m[0][1] * p.position.y + m.m[0][2] * p.position.z);
    i.position.y = -(m.m[1][0] * p.position.x + m.m[1][1] * p.position.y + m.m[1][2] * p.position.z);
    i.position.z = -(m.m[2][0] * p.position.x + m.m[2][1] * p.position.y + m.m[2][2] * p.position.z);
    return i;
}

// ---- the controller pose the game is handed: SteamVR's /pose/raw
//
// Alyx's own bindings (cfg/bindings_touch.json, flattened by kl_alyx_input.c)
// bind handposeleft/right to /user/hand/{left,right}/pose/raw, and the game
// applies its OWN per-controller raw-to-hand offset to that pose (we report
// Prop_ControllerType "oculus_touch"). SteamVR's Touch raw pose is NOT the
// OpenXR grip: its origin sits ~10 cm ahead of the grip, turned ~20.6 deg about
// X. Handing the grip over as the raw pose leaves Alyx's offset applied to the
// wrong origin — the virtual hand sits a few cm off the real one and, because
// the error is a lever arm, swings away from the hand's centre as the wrist
// rotates (launch 4).
//
// The reference host (libalyxhost_f1_m1) does exactly this conversion and the
// numbers are its own: alyx_input::TouchRaw::Attach (+0x10209c) creates a
// "create-raw-pose-space" action space on the grip pose action per hand with
// poseInActionSpace = alyx_view::RelativePose(A, identity) = A^-1, where A —
// the grip in the raw frame — is
//   orientation (0.17880222, 0, 0, 0.98388505)   [.rodata 0x295d0; 20.6 deg about +X]
//   position    (+0.007 left / -0.007 right, -0.00182941, 0.10194820) m
//               [.rodata 0x29aa0 / 0x29c38, z = 0x3dd0ca38 in the code]
// and alyx_touch_pose::Convert hands that space's location (standing: the
// matrix as is; velocities rotated into the origin) to WaitGetPoses and the
// pose actions — its frame log calls the field "rawStage". So: raw = grip o A^-1.
// KL_OPENVR_HAND_POSE=grip restores the grip as handed over before N12.
klxr_Posef klvr_touch_grip_in_raw(int hand) {
    klxr_Posef a = { { 0.17880222f, 0.0f, 0.0f, 0.98388505f },
                     { hand ? -0.007f : 0.007f, -0.00182941f, 0.10194820f } };
    return a;
}

static int klvr_hand_pose_raw(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("KL_OPENVR_HAND_POSE");
        on = !(e && !strcmp(e, "grip"));
        klvr_log("hand pose: %s (KL_OPENVR_HAND_POSE=%s)",
                 on ? "SteamVR /pose/raw = OpenXR grip o inverse(Touch grip-in-raw: 20.6 deg "
                      "about X, (+-0.007 -0.0018 0.1019) m) — the reference host's raw-pose space"
                    : "the OpenXR grip, unconverted", on ? "raw" : "grip");
    }
    return on;
}

klxr_Posef klvr_hand_raw_from_grip(int hand, klxr_Posef grip, const float ang[3],
                                   float lin[3]) {
    klxr_Posef raw = klvr_pose_mul(grip, klvr_pose_inv(klvr_touch_grip_in_raw(hand)));
    // A rigid body's point velocity: v_raw = v_grip + w x (p_raw - p_grip), all in
    // the base frame. The reference's runtime located the raw space itself, which
    // OpenXR defines to report that space origin's own velocity.
    if (lin && ang) {
        float r[3] = { raw.position.x - grip.position.x, raw.position.y - grip.position.y,
                       raw.position.z - grip.position.z };
        lin[0] += ang[1] * r[2] - ang[2] * r[1];
        lin[1] += ang[2] * r[0] - ang[0] * r[2];
        lin[2] += ang[0] * r[1] - ang[1] * r[0];
    }
    return raw;
}

void klvr_pose_from_xr(const klxr_SpaceLocation *l, const klxr_SpaceVelocity *v,
                       klvr_TrackedDevicePose_t *out) {
    memset(out, 0, sizeof *out);
    const uint64_t need = KLXR_LOC_ORIENTATION_VALID | KLXR_LOC_POSITION_VALID;
    out->bDeviceIsConnected = 1;
    // A zero quaternion is not an orientation. kl_openxr answers one, flagged
    // valid, for a hand located against a frame latched before the frontend
    // first published it; passing it on would hand the guest a singular matrix.
    const klxr_Quat *q = &l->pose.orientation;
    if ((l->locationFlags & need) != need ||
        q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w < 0.5f) {
        out->mDeviceToAbsoluteTracking = klvr_identity34();
        out->eTrackingResult = KLVR_TRACKING_RUNNING_OUT_OF_RANGE;
        return;
    }
    out->mDeviceToAbsoluteTracking = klvr_mat34(l->pose);
    out->bPoseIsValid = 1;
    out->eTrackingResult = KLVR_TRACKING_RUNNING_OK;
    if (v && (v->velocityFlags & 1)) memcpy(out->vVelocity.v, &v->linearVelocity, 12);
    if (v && (v->velocityFlags & 2)) memcpy(out->vAngularVelocity.v, &v->angularVelocity, 12);
}

// ================================================================ the events
enum { KLVR_EVQ = 32 };
static struct { klvr_VREvent_t ev[KLVR_EVQ]; int head, count; } g_evq;

static int klvr_event_push(uint32_t type, uint32_t device) {     // under the lock
    if (g_evq.count >= KLVR_EVQ) return -1;
    klvr_VREvent_t *e = &g_evq.ev[(g_evq.head + g_evq.count) % KLVR_EVQ];
    memset(e, 0, sizeof *e);
    e->eventType = type;
    e->trackedDeviceIndex = device;
    g_evq.count++;
    return 0;
}

int kl_openvr_post_event(uint32_t type, uint32_t device) {
    pthread_mutex_lock(&klvr_lock);
    int r = klvr_event_push(type, device);
    pthread_mutex_unlock(&klvr_lock);
    return r;
}

// Under the lock. Copies at most what the caller says its struct holds — the
// event's size is the CALLER's (60 here), and a short one gets the header.
static int klvr_event_pop(void *dst, uint32_t size, uint32_t *device) {
    if (!g_evq.count || !dst || size < 12) return 0;
    klvr_VREvent_t *e = &g_evq.ev[g_evq.head];
    memcpy(dst, e, size < sizeof *e ? size : sizeof *e);
    if (device) *device = e->trackedDeviceIndex;
    g_evq.head = (g_evq.head + 1) % KLVR_EVQ;
    g_evq.count--;
    return 1;
}

// =========================================================== OpenXR bring-up
//
// Every structure below is filled the way an OpenXR app fills it; the layouts
// are the spec's (kl_openxr.c carries the provenance).
static int32_t (*g_gipa)(void *, const char *, void **);

static int klvr_resolve(void *inst) {
    int missing = 0;
#define X(n)                                                                     \
    if (g_gipa(inst, #n, (void **)&klvr_xr.n) != 0 || !klvr_xr.n) {              \
        if (inst) { klvr_log("OpenXR has no %s", #n); missing++; }               \
    }
    KLVR_XR_FUNCS(X)
#undef X
    return missing;
}

static klxr_Path klvr_path(const char *s) {
    klxr_Path p = 0;
    klvr_xr.xrStringToPath(klvr.instance, s, &p);
    return p;
}

// The raw Touch surface (kl_openvr_int.h): one OpenXR action per control, both
// hands as subaction paths, one suggested-binding call for the whole map
// (kl_openxr, like the spec, treats a second call as a replacement).
static const struct { const char *name; int32_t type; const char *path[2]; } g_raw[KLVR_RAW_N] = {
    [KLVR_RAW_TRIGGER]     = { "trigger", 2, { "/input/trigger/value", "/input/trigger/value" } },
    [KLVR_RAW_SQUEEZE]     = { "squeeze", 2, { "/input/squeeze/value", "/input/squeeze/value" } },
    [KLVR_RAW_STICK_X]     = { "stick_x", 2, { "/input/thumbstick/x", "/input/thumbstick/x" } },
    [KLVR_RAW_STICK_Y]     = { "stick_y", 2, { "/input/thumbstick/y", "/input/thumbstick/y" } },
    // The lower and upper face buttons: X/Y on the left Touch, A/B on the right.
    [KLVR_RAW_LOWER]       = { "lower",   1, { "/input/x/click", "/input/a/click" } },
    [KLVR_RAW_UPPER]       = { "upper",   1, { "/input/y/click", "/input/b/click" } },
    [KLVR_RAW_STICK_CLICK] = { "stick_click", 1, { "/input/thumbstick/click",
                                                   "/input/thumbstick/click" } },
    [KLVR_RAW_MENU]        = { "menu",    1, { "/input/menu/click", NULL } },
    [KLVR_RAW_GRIP_POSE]   = { "grip",    4, { "/input/grip/pose", "/input/grip/pose" } },
    [KLVR_RAW_AIM_POSE]    = { "aim",     4, { "/input/aim/pose", "/input/aim/pose" } },
    [KLVR_RAW_HAPTIC]      = { "haptic",  100, { "/output/haptic", "/output/haptic" } },
};

static int klvr_raw_actions(void) {
    struct { int32_t type; const void *next; char name[64]; char loc[128]; uint32_t prio; } asi;
    memset(&asi, 0, sizeof asi);
    asi.type = 28;                                  // XR_TYPE_ACTION_SET_CREATE_INFO
    snprintf(asi.name, sizeof asi.name, "klepton_openvr");
    snprintf(asi.loc, sizeof asi.loc, "OpenVR (Klepton)");
    if (klvr_xr.xrCreateActionSet(klvr.instance, &asi, &klvr.raw_set) != 0) return -1;

    klvr.hand_path[0] = klvr_path("/user/hand/left");
    klvr.hand_path[1] = klvr_path("/user/hand/right");
    struct { void *action; klxr_Path binding; } sb[KLVR_RAW_N * 2];
    uint32_t nsb = 0;
    for (int i = 0; i < KLVR_RAW_N; i++) {
        klxr_ActionCreateInfo ai;
        memset(&ai, 0, sizeof ai);
        ai.type = KLXR_T_ACTION_CREATE_INFO;
        snprintf(ai.actionName, sizeof ai.actionName, "%s", g_raw[i].name);
        snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", g_raw[i].name);
        ai.actionType = g_raw[i].type;
        ai.countSubactionPaths = 2;
        ai.subactionPaths = klvr.hand_path;
        if (klvr_xr.xrCreateAction(klvr.raw_set, &ai, &klvr.raw[i]) != 0) {
            klvr_log("xrCreateAction(%s) failed", g_raw[i].name);
            return -1;
        }
        for (int h = 0; h < 2; h++) {
            if (!g_raw[i].path[h]) continue;
            char p[128];
            snprintf(p, sizeof p, "%s%s", h ? "/user/hand/right" : "/user/hand/left",
                     g_raw[i].path[h]);
            sb[nsb].action = klvr.raw[i];
            sb[nsb].binding = klvr_path(p);
            nsb++;
        }
    }
    struct { int32_t type; const void *next; klxr_Path profile; uint32_t n; const void *b; } sib;
    memset(&sib, 0, sizeof sib);
    sib.type = 51;                 // XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
    sib.profile = klvr_path("/interaction_profiles/oculus/touch_controller");
    sib.n = nsb;
    sib.b = sb;
    return klvr_xr.xrSuggestInteractionProfileBindings(klvr.instance, &sib) == 0 ? 0 : -1;
}

static void *klvr_ref_space(int32_t type) {
    struct { int32_t type; const void *next; int32_t ref; klxr_Posef pose; } ci;
    memset(&ci, 0, sizeof ci);
    ci.type = 37;                                   // XR_TYPE_REFERENCE_SPACE_CREATE_INFO
    ci.ref = type;
    ci.pose.orientation.w = 1.0f;
    void *sp = NULL;
    return klvr_xr.xrCreateReferenceSpace(klvr.session, &ci, &sp) == 0 ? sp : NULL;
}

static void *klvr_action_space(void *action, klxr_Path sub) {
    struct { int32_t type; const void *next; void *action; klxr_Path sub; klxr_Posef pose; } ci;
    memset(&ci, 0, sizeof ci);
    ci.type = 38;                                   // XR_TYPE_ACTION_SPACE_CREATE_INFO
    ci.action = action;
    ci.sub = sub;
    ci.pose.orientation.w = 1.0f;
    void *sp = NULL;
    return klvr_xr.xrCreateActionSpace(klvr.session, &ci, &sp) == 0 ? sp : NULL;
}

// The guest's Vulkan objects, when somebody has said what they are.
static struct { void *instance, *phys, *device; uint32_t qfam, qidx; int set; } g_vk;

// Session events. READY begins the session, STOPPING ends it, EXITING and
// LOSS_PENDING become VREvent_Quit — the OpenVR spelling of "close now".
static void klvr_pump(void) {
    if (!klvr.instance) return;
    struct { int32_t type; const void *next; uint8_t varying[4000]; } ev;
    for (int i = 0; i < 32; i++) {
        memset(&ev, 0, 16);
        ev.type = 16;                               // XR_TYPE_EVENT_DATA_BUFFER
        if (klvr_xr.xrPollEvent(klvr.instance, &ev) != 0) break;   // 4 = none
        if (ev.type != 18) continue;                // SESSION_STATE_CHANGED
        struct { int32_t type; const void *next; void *session; int32_t state; int64_t time; }
            *sc = (void *)&ev;
        klvr.session_state = sc->state;
        if (sc->state == 2 && !klvr.running) {      // READY
            struct { int32_t type; const void *next; int32_t view_config; } bi =
                { 10, NULL, 2 };                    // SESSION_BEGIN_INFO, PRIMARY_STEREO
            klxr_Result r = klvr_xr.xrBeginSession(klvr.session, &bi);
            klvr.running = r == 0;
            klvr_log("session READY -> xrBeginSession %d", r);
        } else if (sc->state == 6 && klvr.running) {   // STOPPING
            klvr_xr.xrEndSession(klvr.session);
            klvr.running = 0;
            klvr_log("session STOPPING -> xrEndSession");
        } else if (sc->state == 7 || sc->state == 8) { // LOSS_PENDING, EXITING
            klvr_event_push(KLVR_EVENT_QUIT, KLVR_DEVICE_HMD);
            klvr_log("session %s -> VREvent_Quit", sc->state == 8 ? "EXITING" : "LOSS_PENDING");
        } else {
            klvr_log("session state -> %d", sc->state);
        }
    }
}

static int klvr_session_up(void) {
    // XR_KHR_vulkan_enable's gate: the requirements call before the session.
    struct { int32_t type; void *next; uint64_t lo, hi; } req = { 1000025002, NULL, 0, 0 };
    klvr_xr.xrGetVulkanGraphicsRequirementsKHR(klvr.instance, klvr.system, &req);

    struct { int32_t type; const void *next; void *instance, *phys, *device;
             uint32_t qfam, qidx; } vk = {
        1000025000, NULL, g_vk.instance, g_vk.phys, g_vk.device, g_vk.qfam, g_vk.qidx };
    struct { int32_t type; const void *next; uint64_t flags; uint64_t system; } sci =
        { 8, &vk, 0, klvr.system };                 // XR_TYPE_SESSION_CREATE_INFO
    klxr_Result r = klvr_xr.xrCreateSession(klvr.instance, &sci, &klvr.session);
    if (r != 0) {
        klvr_log("xrCreateSession -> %d: no session; WaitGetPoses will fail "
                 "(RequestFailed) until one exists", r);
        klvr.session = NULL;
        return -1;
    }
    klvr_log("session created (Vulkan binding %s: device %p)",
             g_vk.set ? "from kl_openvr_bind_vulkan" : "deferred — swapchain images "
             "follow kl_vulkan's first device until kl_openvr_bind_vulkan", g_vk.device);

    klvr.space_stage = klvr_ref_space(3);
    klvr.space_local = klvr_ref_space(2);
    klvr.space_view  = klvr_ref_space(1);
    for (int h = 0; h < 2; h++) {
        klvr.grip_space[h] = klvr_action_space(klvr.raw[KLVR_RAW_GRIP_POSE], klvr.hand_path[h]);
        klvr.aim_space[h]  = klvr_action_space(klvr.raw[KLVR_RAW_AIM_POSE], klvr.hand_path[h]);
    }
    struct { int32_t type; const void *next; uint32_t n; void *const *sets; } at =
        { 60, NULL, 1, &klvr.raw_set };             // SESSION_ACTION_SETS_ATTACH_INFO
    r = klvr_xr.xrAttachSessionActionSets(klvr.session, &at);
    if (r != 0) klvr_log("xrAttachSessionActionSets -> %d (input reads inactive)", r);
    klvr_pump();                                    // IDLE, READY -> begin, ... FOCUSED
    klvr_pump();
    return 0;
}

static int klvr_xr_up(void) {
    // The per-layer composite's grid density (kl_reproject.h,
    // kl_proj_layer_grid_dim). At the shared default of 1x1 the guard band
    // magnifies this layer's whole picture by 1 + 2*guard, and guard follows
    // the per-frame correction angle — the world zoomed with head speed
    // (launches 3/4: ~1.08x at rest from the Vision Pro's eye cant alone,
    // ~1.19x in a quick turn). 64 confines the stretch to one edge cell, as the
    // eye pass's unfoveated grid already does. Set here so it follows the
    // OpenVR path on every platform and no other guest; overwrite=0, so an
    // explicit KL_PROJ_LAYER_GRID=1 is the A/B.
    if (setenv("KL_PROJ_LAYER_GRID", "64", 0) == 0)
        klvr_log("per-layer composite grid: KL_PROJ_LAYER_GRID=%s (64 = the guard band "
                 "stays at the edge; 1 = the pre-N12-D whole-picture zoom)",
                 getenv("KL_PROJ_LAYER_GRID"));
    g_gipa = (int32_t (*)(void *, const char *, void **))kl_openxr_sym("xrGetInstanceProcAddr");
    if (!g_gipa) { klvr_log("kl_openxr serves no xrGetInstanceProcAddr"); return -1; }
    klvr_resolve(NULL);                             // the bootstrap trio
    if (!klvr_xr.xrCreateInstance) return -1;

    // XR_KHR_vulkan_enable when the runtime has it (it is gated on MoltenVK
    // being reachable); the session's binding structure is v1's either way.
    struct { int32_t type; void *next; char name[128]; uint32_t version; } ext[64];
    uint32_t n = 0;
    const char *want[1];
    uint32_t nwant = 0;
    if (klvr_xr.xrEnumerateInstanceExtensionProperties) {
        klvr_xr.xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
        if (n > 64) n = 64;
        for (uint32_t i = 0; i < n; i++) { ext[i].type = 2; ext[i].next = NULL; }
        klvr_xr.xrEnumerateInstanceExtensionProperties(NULL, n, &n, ext);
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(ext[i].name, "XR_KHR_vulkan_enable")) want[nwant++] = "XR_KHR_vulkan_enable";
    }
    struct {
        int32_t type; const void *next; uint64_t flags;
        struct { char app[128]; uint32_t app_ver; char engine[128]; uint32_t engine_ver;
                 uint64_t api; } app;
        uint32_t nlayers; const char *const *layers;
        uint32_t next_n; const char *const *exts;
    } ci;
    memset(&ci, 0, sizeof ci);
    ci.type = 3;                                    // XR_TYPE_INSTANCE_CREATE_INFO
    snprintf(ci.app.app, sizeof ci.app.app, "openvr");
    snprintf(ci.app.engine, sizeof ci.app.engine, "kl_openvr");
    ci.app.api = (1ULL << 48);                      // 1.0.0
    ci.next_n = nwant;
    ci.exts = want;
    klxr_Result r = klvr_xr.xrCreateInstance(&ci, &klvr.instance);
    if (r != 0) { klvr_log("xrCreateInstance -> %d", r); klvr.instance = NULL; return -1; }
    if (klvr_resolve(klvr.instance)) return -1;

    struct { int32_t type; const void *next; int32_t form; } gi = { 4, NULL, 1 };
    if (klvr_xr.xrGetSystem(klvr.instance, &gi, &klvr.system) != 0) {
        klvr_log("xrGetSystem(HMD) failed");
        return -1;
    }
    struct { int32_t type; void *next; uint64_t id; uint32_t vendor; char name[256];
             uint32_t gfx[3], track[2]; } sp;
    memset(&sp, 0, sizeof sp);
    sp.type = 5;
    klvr_xr.xrGetSystemProperties(klvr.instance, klvr.system, &sp);
    struct { int32_t type; void *next; uint32_t rw, mw, rh, mh, rs, ms; } views[2];
    memset(views, 0, sizeof views);
    views[0].type = views[1].type = 41;
    uint32_t nv = 0;
    if (klvr_xr.xrEnumerateViewConfigurationViews(klvr.instance, klvr.system, 2, 2, &nv,
                                                   views) == 0 && nv == 2) {
        klvr.rec_w = views[0].rw;
        klvr.rec_h = views[0].rh;
    }
    klvr_log("OpenXR system \"%s\" vendor 0x%x, views %ux%u%s", sp.name, sp.vendor,
             klvr.rec_w, klvr.rec_h, nwant ? ", XR_KHR_vulkan_enable" : "");

    if (klvr_raw_actions() != 0) klvr_log("raw Touch actions not created — input inactive");
    klvr_session_up();
    return 0;
}

static void klvr_eyecopy_down(void);

static void klvr_xr_down(void) {
    klvr_eyecopy_down();                            // swapchains before the session
    if (klvr.session) {
        void **sp[] = { &klvr.space_stage, &klvr.space_local, &klvr.space_view,
                        &klvr.grip_space[0], &klvr.grip_space[1],
                        &klvr.aim_space[0], &klvr.aim_space[1] };
        for (size_t i = 0; i < sizeof sp / sizeof *sp; i++)
            if (*sp[i]) { klvr_xr.xrDestroySpace(*sp[i]); *sp[i] = NULL; }
        klvr_xr.xrDestroySession(klvr.session);
    }
    if (klvr.raw_set && klvr_xr.xrDestroyActionSet) klvr_xr.xrDestroyActionSet(klvr.raw_set);
    if (klvr.instance) klvr_xr.xrDestroyInstance(klvr.instance);
    uint32_t token = klvr.token;
    memset(&klvr, 0, sizeof klvr);
    klvr.token = token;
}

void kl_openvr_bind_vulkan(void *instance, void *phys, void *device,
                           uint32_t qfam, uint32_t qidx) {
    pthread_mutex_lock(&klvr_lock);
    g_vk.instance = instance; g_vk.phys = phys; g_vk.device = device;
    g_vk.qfam = qfam; g_vk.qidx = qidx; g_vk.set = 1;
    pthread_mutex_unlock(&klvr_lock);
    // What xrCreateSession does with a binding, done for the live session: the
    // swapchain images A5 creates follow this device.
    kl_vulkan_xr_bind_device(device);
    klvr_log("bind_vulkan: instance %p physical %p device %p queue %u.%u",
             instance, phys, device, qfam, qidx);
}

// =================================================================== poses
void *klvr_origin_space(int origin) {
    return origin == KLVR_ORIGIN_SEATED ? klvr.space_local : klvr.space_stage;
}

int klvr_hand_present(int hand) {
    if (!klvr.session || !klvr.raw[KLVR_RAW_GRIP_POSE]) return 0;
    klxr_ActionStateGetInfo gi = { KLXR_T_ACTION_STATE_GET_INFO, NULL,
                                   klvr.raw[KLVR_RAW_GRIP_POSE], klvr.hand_path[hand] };
    struct { int32_t type; void *next; uint32_t active; } st = { 27, NULL, 0 };
    return klvr_xr.xrGetActionStatePose(klvr.session, &gi, &st) == 0 && st.active;
}

int klvr_locate_device(uint32_t device, int origin, int64_t xr_time,
                       klvr_TrackedDevicePose_t *out) {
    memset(out, 0, sizeof *out);
    out->mDeviceToAbsoluteTracking = klvr_identity34();
    out->eTrackingResult = KLVR_TRACKING_UNINITIALIZED;
    void *space = device == KLVR_DEVICE_HMD   ? klvr.space_view
                : device == KLVR_DEVICE_LEFT  ? klvr.grip_space[0]
                : device == KLVR_DEVICE_RIGHT ? klvr.grip_space[1] : NULL;
    void *base = klvr_origin_space(origin);
    if (!space || !base) return 0;
    if (!xr_time) xr_time = klvr.display_time ? klvr.display_time : klvr_now_ns();
    klxr_SpaceVelocity vel;
    memset(&vel, 0, sizeof vel);
    vel.type = KLXR_T_SPACE_VELOCITY;
    klxr_SpaceLocation loc;
    memset(&loc, 0, sizeof loc);
    loc.type = KLXR_T_SPACE_LOCATION;
    loc.next = &vel;
    if (klvr_xr.xrLocateSpace(space, base, xr_time, &loc) != 0) return 0;
    // The HMD's velocities are NOT handed over, as the reference host never
    // handed them (AlyxOpenVrCompositorWaitGetPoses+0x604 memsets each
    // TrackedDevicePose_t and writes only the matrix, result and flags for the
    // head; only alyx_touch_pose::Convert fills vVelocity/vAngularVelocity, for
    // the controllers). The render poses are already predicted to the frame's
    // display time; a velocity on top is a second prediction the game may
    // apply to its camera that the layer's pose would not carry — an error
    // proportional to head speed. On the headset these are kl_openxr's DERIVED
    // (finite-difference) velocities. KL_OPENVR_HMD_VELOCITY=1 hands them over.
    static int hmd_vel = -1;
    if (hmd_vel < 0) hmd_vel = kl_env_on("KL_OPENVR_HMD_VELOCITY", 0);
    if (device == KLVR_DEVICE_HMD && !hmd_vel) vel.velocityFlags = 0;
    if (device != KLVR_DEVICE_HMD && klvr_hand_pose_raw()) {
        float ang[3] = { vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z };
        float lin[3] = { vel.linearVelocity.x, vel.linearVelocity.y, vel.linearVelocity.z };
        int both = (vel.velocityFlags & 3) == 3;
        loc.pose = klvr_hand_raw_from_grip(device == KLVR_DEVICE_RIGHT, loc.pose,
                                           both ? ang : NULL, both ? lin : NULL);
        if (both) memcpy(&vel.linearVelocity, lin, sizeof lin);
        // A linear velocity alone cannot be moved to the raw origin; drop it
        // rather than hand over the grip's as the raw's.
        else vel.velocityFlags &= ~1ull;
    }
    klvr_pose_from_xr(&loc, &vel, out);
    return out->bPoseIsValid;
}

// The eyes, relative to the head (VIEW space): eye-to-head and the frustum.
// Refreshed per WaitGetPoses; until a session can answer, a symmetric 90°
// frustum and a 63 mm IPD (the headless view configuration's own numbers).
static struct { klxr_Posef pose[2]; klxr_Fovf fov[2]; int located; } g_eyes = {
    { { { 0, 0, 0, 1 }, { -0.0315f, 0, 0 } }, { { 0, 0, 0, 1 }, { 0.0315f, 0, 0 } } },
    { { -0.785398f, 0.785398f, 0.785398f, -0.785398f },
      { -0.785398f, 0.785398f, 0.785398f, -0.785398f } }, 0 };

static void klvr_eyes_update(int64_t xr_time) {
    if (!klvr.session || !klvr.space_view) return;
    struct { int32_t type; const void *next; int32_t cfg; int64_t time; void *space; } li =
        { 6, NULL, 2, xr_time ? xr_time : klvr_now_ns(), klvr.space_view };
    struct { int32_t type; void *next; uint64_t flags; } vs = { 11, NULL, 0 };
    struct { int32_t type; void *next; klxr_Posef pose; klxr_Fovf fov; } v[2];
    memset(v, 0, sizeof v);
    v[0].type = v[1].type = 7;
    uint32_t n = 0;
    if (klvr_xr.xrLocateViews(klvr.session, &li, &vs, 2, &n, v) != 0 || n != 2) return;
    for (int e = 0; e < 2; e++) { g_eyes.pose[e] = v[e].pose; g_eyes.fov[e] = v[e].fov; }
    if (!g_eyes.located++)
        klvr_log("eyes located: eye 0 at (%.4f %.4f %.4f) fov L%.4f R%.4f U%.4f D%.4f",
                 v[0].pose.position.x, v[0].pose.position.y, v[0].pose.position.z,
                 v[0].fov.angleLeft, v[0].fov.angleRight, v[0].fov.angleUp,
                 v[0].fov.angleDown);
}

// The same eye as GetEyeToHeadTransform / GetProjectionRaw hand the guest, for
// the projection layer that carries its picture.
static klxr_Posef klvr_eye_head_pose(int e) { return g_eyes.pose[e ? 1 : 0]; }
static klxr_Fovf  klvr_eye_fov(int e) { return g_eyes.fov[e ? 1 : 0]; }

// ======================================================= the slot tables
KLVR_INTERFACE_RECS(System, "IVRSystem_026", 51, KLVR_SLOTS_System);
KLVR_INTERFACE_RECS(Compositor, "IVRCompositor_029", 53, KLVR_SLOTS_Compositor);
KLVR_INTERFACE_RECS(Chaperone, "IVRChaperone_004", 9, KLVR_SLOTS_Chaperone);
KLVR_INTERFACE_RECS(Overlay, "IVROverlay_028", 82, KLVR_SLOTS_Overlay);
KLVR_INTERFACE_RECS(Applications, "IVRApplications_007", 30, KLVR_SLOTS_Applications);
KLVR_INTERFACE_RECS(OverlayView, "IVROverlayView_003", 4, KLVR_SLOTS_OverlayView);
KLVR_INTERFACE_RECS(RenderModels, "IVRRenderModels_006", 19, KLVR_SLOTS_RenderModels);
KLVR_INTERFACE_RECS(HeadsetView, "IVRHeadsetView_001", 9, KLVR_SLOTS_HeadsetView);
KLVR_INTERFACE_RECS(Mailbox, "IVRMailbox_001", 32, KLVR_SLOTS_Mailbox);
KLVR_INTERFACE_RECS(XrProto, "IXrProto_001", 32, KLVR_SLOTS_XrProto);
KLVR_INTERFACE_RECS(InputInternal, "IVRInputInternal_002", 32, KLVR_SLOTS_InputInternal);

// A per-method log gate: the slot's own counter, read after KLVR_COUNT.
#define KLVR_N(T, N) (__atomic_load_n(&g_rec_##T[N].calls, __ATOMIC_RELAXED) - 1)

// ==================================================================== IVRSystem
// The per-eye size the guest is told to render at — and so the size of the eye
// swapchains the compositor copies into.
//
// KL_OPENVR_RENDER_SCALE=<percent> (10..200, default 100) scales OpenXR's
// recommended size — the DISPLAY's own aspect, unlike a title's fixed eye
// (kl_openvr_config.render_width/height wins over it, as before). The guest
// shades scale^2 of the pixels; the visionOS app sets it on the headset only
// (kl_app.c), where the recommended eye is the display's full 4032x3234.
// Rounded to a multiple of 16 so the guest's tiling has nothing to trim.
static void klvr_render_size(uint32_t *w, uint32_t *h) {
    if (g_cfg.render_w && g_cfg.render_h) {
        *w = g_cfg.render_w;
        *h = g_cfg.render_h;
        return;
    }
    uint32_t rw = klvr.rec_w ? klvr.rec_w : 2290, rh = klvr.rec_h ? klvr.rec_h : 2400;
    static int pct = -1;
    if (pct < 0) {
        int p = kl_env_int("KL_OPENVR_RENDER_SCALE", 100);
        pct = p < 10 || p > 200 ? 100 : p;
    }
    if (pct != 100) {
        uint32_t sw = (uint32_t)(((uint64_t)rw * (uint32_t)pct / 100 + 8) / 16 * 16);
        uint32_t sh = (uint32_t)(((uint64_t)rh * (uint32_t)pct / 100 + 8) / 16 * 16);
        static int said;
        if (!said++)
            klvr_log("KL_OPENVR_RENDER_SCALE=%d%%: eye render size %ux%u (OpenXR recommends "
                     "%ux%u; %.0f%% of its pixels)", pct, sw, sh, rw, rh,
                     100.0 * sw * sh / ((double)rw * rh));
        rw = sw;
        rh = sh;
    }
    *w = rw;
    *h = rh;
}

static void klvr_System_GetRecommendedRenderTargetSize(void *self, uint32_t *w, uint32_t *h) {
    KLVR_COUNT(System, 0);
    uint32_t rw, rh;
    klvr_render_size(&rw, &rh);
    if (w) *w = rw;
    if (h) *h = rh;
    if (klvr_log_this(KLVR_N(System, 0)))
        klvr_log("System.GetRecommendedRenderTargetSize -> %ux%u", rw, rh);
}

static uint32_t g_submit_w, g_submit_h;   // the texture the guest last submitted
static uint64_t g_submit_img[8];           // every image it has submitted (ring)
static uint32_t g_submit_nimg;
int kl_openvr_is_submitted_image(uint64_t img) {
    if (!img) return 0;
    for (int i = 0; i < 8; i++) if (__atomic_load_n(&g_submit_img[i], __ATOMIC_RELAXED) == img) return 1;
    return 0;
}
static void klvr_note_submitted_image(uint64_t img) {
    if (!img || kl_openvr_is_submitted_image(img)) return;
    uint32_t k = __atomic_fetch_add(&g_submit_nimg, 1, __ATOMIC_RELAXED) % 8;
    __atomic_store_n(&g_submit_img[k], img, __ATOMIC_RELAXED);
}
int kl_openvr_eye_geometry(uint32_t *w, uint32_t *h, float tan_lrdu[2][4]) {
    uint32_t rw = __atomic_load_n(&g_submit_w, __ATOMIC_RELAXED);
    uint32_t rh = __atomic_load_n(&g_submit_h, __ATOMIC_RELAXED);
    if (w) *w = rw;
    if (h) *h = rh;
    if (tan_lrdu) {
        pthread_mutex_lock(&klvr_lock);
        for (int e = 0; e < 2; e++) {
            tan_lrdu[e][0] = tanf(g_eyes.fov[e].angleLeft);
            tan_lrdu[e][1] = tanf(g_eyes.fov[e].angleRight);
            tan_lrdu[e][2] = tanf(g_eyes.fov[e].angleDown);
            tan_lrdu[e][3] = tanf(g_eyes.fov[e].angleUp);
        }
        pthread_mutex_unlock(&klvr_lock);
    }
    return rw && rh;
}

// The four tangents of an eye's frustum, in OpenVR's raw spelling. OpenVR's
// documented ComposeProjection builds its y row from (bottom - top) and
// (bottom + top); for that to be an ordinary y-up projection `bottom` must be
// the UP tangent and `top` the DOWN one (negative). Symmetric frusta cannot
// tell the two conventions apart — see the report's note on asymmetric ones.
static void klvr_raw(int eye, float *l, float *r, float *t, float *b) {
    eye = eye ? 1 : 0;
    *l = tanf(g_eyes.fov[eye].angleLeft);
    *r = tanf(g_eyes.fov[eye].angleRight);
    *t = tanf(g_eyes.fov[eye].angleDown);
    *b = tanf(g_eyes.fov[eye].angleUp);
}

static klvr_HmdMatrix44_t klvr_System_GetProjectionMatrix(void *self, uint32_t eye,
                                                          float zn, float zf) {
    KLVR_COUNT(System, 1);
    float l, r, t, b;
    pthread_mutex_lock(&klvr_lock);
    klvr_raw((int)eye, &l, &r, &t, &b);
    pthread_mutex_unlock(&klvr_lock);
    // OpenVR's ComposeProjection, verbatim in shape: D3D clip z in [0, 1].
    float idx = 1.0f / (r - l), idy = 1.0f / (b - t), idz = 1.0f / (zf - zn);
    klvr_HmdMatrix44_t m;
    memset(&m, 0, sizeof m);
    m.m[0][0] = 2 * idx; m.m[0][2] = (r + l) * idx;
    m.m[1][1] = 2 * idy; m.m[1][2] = (b + t) * idy;
    m.m[2][2] = -zf * idz; m.m[2][3] = -zf * zn * idz;
    m.m[3][2] = -1.0f;
    if (klvr_log_this(KLVR_N(System, 1)))
        klvr_log("System.GetProjectionMatrix eye=%u near=%g far=%g call#=%llu", eye, zn, zf,
                 (unsigned long long)KLVR_N(System, 1));
    return m;
}

static void klvr_System_GetProjectionRaw(void *self, uint32_t eye, float *l, float *r,
                                         float *t, float *b) {
    KLVR_COUNT(System, 2);
    float L, R, T, B;
    pthread_mutex_lock(&klvr_lock);
    klvr_raw((int)eye, &L, &R, &T, &B);
    pthread_mutex_unlock(&klvr_lock);
    if (l) *l = L;
    if (r) *r = R;
    if (t) *t = T;
    if (b) *b = B;
    if (klvr_log_this(KLVR_N(System, 2)))
        klvr_log("System.GetProjectionRaw eye=%u l=%f r=%f top=%f bottom=%f", eye, L, R, T, B);
}

// The images are undistorted (the compositor does no lens warp of its own), so
// every channel samples where it was asked.
static uint8_t klvr_System_ComputeDistortion(void *self, uint32_t eye, float u, float v,
                                             klvr_DistortionCoordinates_t *out) {
    KLVR_COUNT(System, 3);
    if (!out) return 0;
    out->rfRed[0] = out->rfGreen[0] = out->rfBlue[0] = u;
    out->rfRed[1] = out->rfGreen[1] = out->rfBlue[1] = v;
    return 1;
}

static klvr_HmdMatrix34_t klvr_System_GetEyeToHeadTransform(void *self, uint32_t eye) {
    KLVR_COUNT(System, 5);
    pthread_mutex_lock(&klvr_lock);
    klvr_HmdMatrix34_t m = klvr_mat34(g_eyes.pose[eye ? 1 : 0]);
    pthread_mutex_unlock(&klvr_lock);
    if (klvr_log_this(KLVR_N(System, 5)))
        klvr_log("System.GetEyeToHeadTransform eye=%u x=%f y=%f z=%f", eye,
                 m.m[0][3], m.m[1][3], m.m[2][3]);
    return m;
}

static void klvr_System_GetDeviceToAbsoluteTrackingPose(void *self, uint32_t origin,
                                                        float seconds,
                                                        klvr_TrackedDevicePose_t *poses,
                                                        uint32_t count) {
    KLVR_COUNT(System, 12);
    if (!poses) return;
    if (count > KLVR_MAX_DEVICES) count = KLVR_MAX_DEVICES;
    pthread_mutex_lock(&klvr_lock);
    int64_t t = klvr_now_ns() + (int64_t)(seconds * 1e9);
    for (uint32_t i = 0; i < count; i++) klvr_locate_device(i, (int)origin, t, &poses[i]);
    pthread_mutex_unlock(&klvr_lock);
}

// The play space IS the standing origin here; seated and raw share it.
static klvr_HmdMatrix34_t klvr_System_GetSeatedZeroPoseToStandingAbsoluteTrackingPose(void *self) {
    KLVR_COUNT(System, 13);
    return klvr_identity34();
}
static klvr_HmdMatrix34_t klvr_System_GetRawZeroPoseToStandingAbsoluteTrackingPose(void *self) {
    KLVR_COUNT(System, 14);
    return klvr_identity34();
}

static uint32_t klvr_System_GetTrackedDeviceIndexForControllerRole(void *self, uint32_t role) {
    KLVR_COUNT(System, 18);
    return role == 1 ? KLVR_DEVICE_LEFT : role == 2 ? KLVR_DEVICE_RIGHT : 0xffffffffu;
}

static uint32_t klvr_System_GetControllerRoleForTrackedDeviceIndex(void *self, uint32_t i) {
    KLVR_COUNT(System, 19);
    return i == KLVR_DEVICE_LEFT ? 1 : i == KLVR_DEVICE_RIGHT ? 2 : 0;
}

static uint32_t klvr_System_GetTrackedDeviceClass(void *self, uint32_t i) {
    KLVR_COUNT(System, 20);
    return i == KLVR_DEVICE_HMD ? 1 : (i == KLVR_DEVICE_LEFT || i == KLVR_DEVICE_RIGHT) ? 2 : 0;
}

// Both controllers are reported connected from the start, as the reference
// did (`openvr-system-device-connected index=1 connected=1`): whether one is
// TRACKED is the pose's bPoseIsValid, a different question.
static uint8_t klvr_System_IsTrackedDeviceConnected(void *self, uint32_t i) {
    KLVR_COUNT(System, 21);
    return i <= KLVR_DEVICE_RIGHT;
}

// ---- tracked-device properties. The values are the reference host's own
// answers (hostboot-round7 telemetry); anything else is UnknownProperty, which
// is what the reference answered for the names it did not know.
enum { KLVR_PROP_OK = 0, KLVR_PROP_BUFFER_TOO_SMALL = 3, KLVR_PROP_UNKNOWN = 4,
       KLVR_PROP_INVALID_DEVICE = 5 };

static void klvr_prop_err(uint32_t *err, uint32_t e) { if (err) *err = e; }

static uint8_t klvr_System_GetBoolTrackedDeviceProperty(void *self, uint32_t dev, uint32_t prop,
                                                        uint32_t *err) {
    KLVR_COUNT(System, 22);
    if (dev > KLVR_DEVICE_RIGHT) { klvr_prop_err(err, KLVR_PROP_INVALID_DEVICE); return 0; }
    // 1004 WillDriftInYaw, 6005 HasDriverDirectModeComponent: no.
    klvr_prop_err(err, (prop == 1004 || prop == 6005) ? KLVR_PROP_OK : KLVR_PROP_UNKNOWN);
    return 0;
}

static float klvr_System_GetFloatTrackedDeviceProperty(void *self, uint32_t dev, uint32_t prop,
                                                       uint32_t *err) {
    KLVR_COUNT(System, 23);
    if (dev > KLVR_DEVICE_RIGHT) { klvr_prop_err(err, KLVR_PROP_INVALID_DEVICE); return 0; }
    float v = 0.0f;
    uint32_t e = KLVR_PROP_UNKNOWN;
    if (dev == KLVR_DEVICE_HMD && prop == 2002) {           // DisplayFrequency
        v = kl_ovrp_display_frequency();
        if (!(v > 0)) v = 90.0f;
        e = KLVR_PROP_OK;
    } else if (dev == KLVR_DEVICE_HMD && prop == 2003) {    // UserIpdMeters
        pthread_mutex_lock(&klvr_lock);
        v = g_eyes.pose[1].position.x - g_eyes.pose[0].position.x;
        pthread_mutex_unlock(&klvr_lock);
        e = KLVR_PROP_OK;
    }
    klvr_prop_err(err, e);
    if (klvr_log_this(KLVR_N(System, 23)))
        klvr_log("System.GetFloatTrackedDeviceProperty dev=%u prop=%u -> %f err=%u", dev, prop, v, e);
    return v;
}

static int32_t klvr_System_GetInt32TrackedDeviceProperty(void *self, uint32_t dev, uint32_t prop,
                                                         uint32_t *err) {
    KLVR_COUNT(System, 24);
    if (dev > KLVR_DEVICE_RIGHT) { klvr_prop_err(err, KLVR_PROP_INVALID_DEVICE); return 0; }
    int32_t v = 0;
    uint32_t e = KLVR_PROP_UNKNOWN;
    if (prop == 1029) { v = dev == KLVR_DEVICE_HMD ? 1 : 2; e = KLVR_PROP_OK; }   // DeviceClass
    else if (dev != KLVR_DEVICE_HMD && prop == 3007) { v = (int32_t)dev; e = KLVR_PROP_OK; } // RoleHint
    // Axis0..4Type: k_eControllerAxis_None — the legacy controller path is not
    // served (GetControllerState), the action path is.
    else if (dev != KLVR_DEVICE_HMD && prop >= 3002 && prop <= 3006) e = KLVR_PROP_OK;
    klvr_prop_err(err, e);
    return v;
}

static uint64_t klvr_System_GetUint64TrackedDeviceProperty(void *self, uint32_t dev,
                                                           uint32_t prop, uint32_t *err) {
    KLVR_COUNT(System, 25);
    if (dev > KLVR_DEVICE_RIGHT) { klvr_prop_err(err, KLVR_PROP_INVALID_DEVICE); return 0; }
    // 3001 SupportedButtons: none on the legacy path (as the reference).
    klvr_prop_err(err, (dev != KLVR_DEVICE_HMD && prop == 3001) ? KLVR_PROP_OK : KLVR_PROP_UNKNOWN);
    return 0;
}

static klvr_HmdMatrix34_t klvr_System_GetMatrix34TrackedDeviceProperty(void *self, uint32_t dev,
                                                                       uint32_t prop,
                                                                       uint32_t *err) {
    KLVR_COUNT(System, 26);
    klvr_prop_err(err, dev > KLVR_DEVICE_RIGHT ? KLVR_PROP_INVALID_DEVICE : KLVR_PROP_UNKNOWN);
    klvr_HmdMatrix34_t m;
    memset(&m, 0, sizeof m);
    return m;
}

static const char *klvr_string_prop(uint32_t dev, uint32_t prop) {
    if (dev == KLVR_DEVICE_HMD) {
        switch (prop) {
        case 1000: return g_cfg.tracking_system;
        case 1001: return g_cfg.model;
        case 1002: return g_cfg.serial;
        case 1003: return g_cfg.render_model;
        case 1005: return g_cfg.manufacturer;
        }
    } else if (dev <= KLVR_DEVICE_RIGHT) {
        if (prop == 7000) return g_cfg.controller_type;
        if (prop == 1000) return g_cfg.tracking_system;
        if (prop == 1005) return g_cfg.manufacturer;
    }
    return NULL;
}

static uint32_t klvr_System_GetStringTrackedDeviceProperty(void *self, uint32_t dev, uint32_t prop,
                                                           char *buf, uint32_t cap,
                                                           uint32_t *err) {
    KLVR_COUNT(System, 28);
    const char *v = klvr_string_prop(dev, prop);
    uint32_t need = v ? (uint32_t)strlen(v) + 1 : 0, e;
    if (dev > KLVR_DEVICE_RIGHT)          e = KLVR_PROP_INVALID_DEVICE;
    else if (!v)                          e = KLVR_PROP_UNKNOWN;
    else if (!buf || cap < need)          e = KLVR_PROP_BUFFER_TOO_SMALL;
    else { memcpy(buf, v, need);          e = KLVR_PROP_OK; }
    if (e != KLVR_PROP_OK && buf && cap) buf[0] = 0;
    klvr_prop_err(err, e);
    if (klvr_log_this(KLVR_N(System, 28)))
        klvr_log("System.GetStringTrackedDeviceProperty dev=%u prop=%u -> \"%s\" required=%u err=%u",
                 dev, prop, v ? v : "", need, e);
    return need;
}

static uint8_t klvr_System_PollNextEvent(void *self, void *ev, uint32_t size) {
    KLVR_COUNT(System, 30);
    pthread_mutex_lock(&klvr_lock);
    klvr_pump();
    int r = klvr_event_pop(ev, size, NULL);
    pthread_mutex_unlock(&klvr_lock);
    if (r) klvr_log("System.PollNextEvent -> type %u (size %u)", ((klvr_VREvent_t *)ev)->eventType, size);
    return (uint8_t)r;
}

static uint8_t klvr_System_PollNextEventWithPose(void *self, uint32_t origin, void *ev,
                                                 uint32_t size, klvr_TrackedDevicePose_t *pose) {
    KLVR_COUNT(System, 31);
    pthread_mutex_lock(&klvr_lock);
    klvr_pump();
    uint32_t dev = 0;
    int r = klvr_event_pop(ev, size, &dev);
    if (r && pose) klvr_locate_device(dev, (int)origin, 0, pose);
    pthread_mutex_unlock(&klvr_lock);
    if (r) klvr_log("System.PollNextEventWithPose -> type %u device %u (size %u)",
                    ((klvr_VREvent_t *)ev)->eventType, dev, size);
    return (uint8_t)r;
}

// The hidden-area mesh (round N15). The reference answered EMPTY on every
// call, kl_openxr advertises no XR_KHR_visibility_mask, and visionOS has no
// lens mask to give one (CompositorServices' only stencil mask is the
// progressive-immersion portal, the whole texture in Full immersion) — so by
// default this stays EMPTY and Valve's vr_stencil_hidden_pixels culls nothing.
//
// KL_OPENVR_HIDDEN_AREA=<fraction> (0 = off, default; 0.02..0.30) hides that
// fraction of each eye's pixels in its four CORNERS: the area outside a
// superellipse |2u-1|^n + |2v-1|^n = 1 inscribed in the eye's UV square, n
// solved so the outside area is the fraction (n = 2, an ellipse, hides 21.5%;
// 0.10 -> n ~ 3.2). The shape is symmetric about the UV centre on both axes, so
// it cannot be mirrored or flipped by a UV-convention mismatch, and it touches
// the rect only at the four edge midpoints. It is NOT a measured lens profile:
// Austin tunes the fraction on the headset (look into the corners of view —
// any dark wedge means too much). Valve leaves hidden pixels at the eye pass's
// clear colour; the compositor's reprojection can drag that into the outer
// edge of view during fast head turns, the same way it already reveals the
// region outside the rendered rect.
//
// Types: 0 Standard = triangles covering the hidden area (corner fans);
// 1 Inverse = triangles covering the visible area (centre fan); 2 LineLoop =
// the boundary, unTriangleCount = vertex count (OpenVR's convention).
#define KLVR_HAM_SEG 16                 // curve segments per quadrant
static double klvr_ham_hidden(double n) {        // outside area / square area
    return 1.0 - tgamma(1.0 + 1.0 / n) * tgamma(1.0 + 1.0 / n) / tgamma(1.0 + 2.0 / n);
}
static float klvr_ham_fraction(void) {
    const char *e = kl_env_str("KL_OPENVR_HIDDEN_AREA", NULL);
    if (!e || !*e) return 0;
    float f = strtof(e, NULL);
    if (!(f >= 0.02f)) return 0;                  // 0, "off", junk
    return f > 0.30f ? 0.30f : f;
}
// The curve point at parameter t in [0,1] across one quadrant, in UV, for the
// quadrant with signs (sx, sy).
static klvr_HmdVector2_t klvr_ham_pt(double n, double t, int sx, int sy) {
    double th = t * M_PI / 2;
    double c = cos(th), s = sin(th);
    double x = pow(fabs(c), 2.0 / n), y = pow(fabs(s), 2.0 / n);
    klvr_HmdVector2_t v = { { (float)(0.5 + 0.5 * sx * x), (float)(0.5 + 0.5 * sy * y) } };
    return v;
}
static klvr_HiddenAreaMesh_t klvr_ham_build(float frac, uint32_t type, double *n_out) {
    static klvr_HmdVector2_t buf[3][4 * KLVR_HAM_SEG * 3];
    static float built[3];
    static uint32_t count[3];
    static double built_n[3];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    klvr_HiddenAreaMesh_t m = { NULL, 0 };
    if (type > 2) return m;
    pthread_mutex_lock(&mu);
    if (built[type] != frac) {
        double lo = 1.2, hi = 64.0;                // hidden(n) falls as n grows
        for (int i = 0; i < 60; i++) {
            double mid = 0.5 * (lo + hi);
            if (klvr_ham_hidden(mid) > frac) lo = mid; else hi = mid;
        }
        double n = 0.5 * (lo + hi);
        static const int qs[4][2] = { { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 } };
        uint32_t k = 0;
        for (int q = 0; q < 4; q++) {
            int sx = qs[q][0], sy = qs[q][1];
            klvr_HmdVector2_t corner = { { sx > 0 ? 1.0f : 0.0f, sy > 0 ? 1.0f : 0.0f } };
            klvr_HmdVector2_t centre = { { 0.5f, 0.5f } };
            for (int i = 0; i < KLVR_HAM_SEG; i++) {
                klvr_HmdVector2_t a = klvr_ham_pt(n, (double)i / KLVR_HAM_SEG, sx, sy);
                klvr_HmdVector2_t b = klvr_ham_pt(n, (double)(i + 1) / KLVR_HAM_SEG, sx, sy);
                if (type == 0) { buf[0][k++] = corner; buf[0][k++] = a; buf[0][k++] = b; }
                else if (type == 1) { buf[1][k++] = centre; buf[1][k++] = a; buf[1][k++] = b; }
                else buf[2][k++] = a;
            }
        }
        count[type] = type == 2 ? k : k / 3;
        built[type] = frac;
        built_n[type] = n;
    }
    m.pVertexData = buf[type];
    m.unTriangleCount = count[type];
    if (n_out) *n_out = built_n[type];
    pthread_mutex_unlock(&mu);
    return m;
}

static klvr_HiddenAreaMesh_t klvr_System_GetHiddenAreaMesh(void *self, uint32_t eye, uint32_t type) {
    KLVR_COUNT(System, 34);
    klvr_HiddenAreaMesh_t m = { NULL, 0 };
    float frac = klvr_ham_fraction();
    double n = 0;
    if (frac > 0) m = klvr_ham_build(frac, type, &n);
    if (klvr_log_this(KLVR_N(System, 34))) {
        if (m.unTriangleCount)
            klvr_log("System.GetHiddenAreaMesh eye=%u type=%u -> %u %s (KL_OPENVR_HIDDEN_AREA=%.3f: "
                     "corners outside a superellipse n=%.2f, symmetric in UV)", eye, type,
                     m.unTriangleCount, type == 2 ? "vertices" : "triangles", frac, n);
        else
            klvr_log("System.GetHiddenAreaMesh eye=%u type=%u -> empty", eye, type);
    }
    return m;
}

static uint8_t klvr_System_IsInputAvailable(void *self) {
    KLVR_COUNT(System, 42);
    return 1;
}

static void klvr_System_AcknowledgeQuit_Exiting(void *self) {
    KLVR_COUNT(System, 47);
    klvr_log("System.AcknowledgeQuit_Exiting");
}

static const char *klvr_System_GetRuntimeVersion(void *self) {
    KLVR_COUNT(System, 49);
    return g_cfg.runtime_version;
}

static int32_t klvr_System_SetSDKVersion(void *self, uint32_t major, uint32_t minor,
                                         uint32_t build) {
    KLVR_COUNT(System, 50);
    klvr_log("System.SetSDKVersion %u.%u.%u", major, minor, build);
    return 0;
}

// ================================================================ IVRCompositor
//
// The frame loop. One WaitGetPoses = xrWaitFrame + xrBeginFrame + the poses
// located at the predicted display time. The frames' poses are kept in a small
// ring keyed by the frame's pose prediction id, because the game asks for "the
// poses of frame N" (GetPosesForFrame) after it has moved on to N+1.
//
// The prediction id is a DISPLAY-INTERVAL counter, not a frame counter (round
// N13-B). Source 2 runs its simulation in lockstep with the compositor: when
// its tick interval equals one display period, libvr+0x9ba60 (the CVR method
// libengine2's clock calls at +0x49f4d4) answers "ticks this frame" = the
// game pose prediction id minus last frame's (libvr+0x9bafc, clamped by a
// max-ticks convar), and the engine advances curtime by ticks x 1/Hz. A real
// compositor's id counts vsyncs, so a frame that took three refreshes is
// three ticks; the reference host's id is its OpenXR loop's frame counter
// (libalyxhost_f1_m1 +0x88904 increments it per xrWaitFrame of ITS OWN display
// loop; AlyxOpenVrCompositorWaitGetPoses +0x7d204 hands the latest to the
// game). Counting frames instead made every frame one tick: the headset at
// 31-43 fps against 90 Hz ran the world at 0.35-0.48x ("slow motion"). The id
// is now round(predictedDisplayTime / predictedDisplayPeriod) relative to the
// first frame, forced to advance at least 1 per frame (a free-running host
// faster than the display keeps the old one-tick-a-frame).
// KL_OPENVR_PREDICTION_ID=frame restores the frame counter for an A/B.
enum { KLVR_RING = 8 };
static struct {
    uint32_t seq;                   // WaitGetPoses frames opened; 0 = none yet
    uint32_t pred;                  // that frame's pose prediction id (display intervals)
    int64_t  pred_base;             // display-interval index the first frame mapped to id 1
    uint64_t pred_skipped;          // display intervals beyond one per frame, summed
    int      begun;                 // xrBeginFrame without its xrEndFrame
    int      should_render;
    uint32_t submitted;             // eye bits Submit was called for this frame
    uint32_t copied;                // ...and the ones whose copy reached a swapchain
    uint64_t ended_empty, ended_layers;
    struct { uint32_t id; klvr_TrackedDevicePose_t p[KLVR_MAX_DEVICES]; } ring[KLVR_RING];
    float    grid_alpha;
    int      suspended, timing_mode;
} g_frame = { .grid_alpha = 1.0f };

// The eye copy's destination: one OpenXR swapchain per eye on the bound device,
// at the recommended render size. `last` is the image the eye's last COMPLETE
// copy filled (its fence seen signalled, the image released), i.e. what the
// next projection layer names, and `ready_head` the head that picture was
// rendered at. `held` is the image a copy is still in flight into: acquired,
// submitted, not yet released — see "sync" in the Submit block below.
// Swapchain calls happen under klvr_lock; the Vulkan recording and the fence
// waits happen on the submitting thread with the lock released.
enum { KLVR_SC_IMAGES = 8 };
static struct {
    void    *device;                // the VkDevice the copy is set up on
    void    *sc[2];                 // XrSwapchain per eye
    uint64_t img[2][KLVR_SC_IMAGES];
    uint32_t nimg[2];
    int      last[2];               // image index of the eye's last complete copy; -1 none
    int      held[2];               // image index with a copy in flight; -1 none
    klxr_Posef pend_head[2], ready_head[2]; // the head of the held / the last picture
    int      pend_sub[2], ready_sub[2];     // ...taken from the Submit's own pose
    klxr_Fovf pend_fov[2], ready_fov[2];    // the frustum it was DRAWN with...
    int      pend_fov_ok[2], ready_fov_ok[2]; // ...when the Submit's projection said
    uint32_t w, h;
    int64_t  fmt;
    uint64_t copies, failures, resolves;
    uint64_t settle_us, settles;    // fence waits since the last present line
    uint32_t pair_src, pair_samples; // the guest format/samples last logged
    int      said_geo, said_layout;
} g_ec = { .last = { -1, -1 }, .held = { -1, -1 } };

// End the open frame. `layers` NULL/0 = an empty frame.
static void klvr_frame_end(const void *const *layers, uint32_t n) {
    if (!g_frame.begun || !klvr.session) return;
    struct { int32_t type; const void *next; int64_t time; int32_t blend; uint32_t n;
             const void *const *layers; } ei =
        { 12, NULL, klvr.display_time, 1 /* OPAQUE */, n, layers };   // FRAME_END_INFO
    klxr_Result r = klvr_xr.xrEndFrame(klvr.session, &ei);
    g_frame.begun = 0;
    g_frame.submitted = g_frame.copied = 0;
    if (n) g_frame.ended_layers++;
    else   g_frame.ended_empty++;
    if (r != 0) klvr_log("xrEndFrame(%u layer(s)) -> %d", n, r);
    // The present counter, in the reference host's spelling so
    // hla/recon/round8-rate.py reads it unchanged: `frames` = frames ended,
    // `pair` = frames that ended with both eyes in a projection layer.
    uint64_t frames = g_frame.ended_layers + g_frame.ended_empty;
    if (frames % 60 == 0) {
        klvr_log("source2-xr-present frames=%llu pair=%llu layer=%u xr=%d copies=%llu "
                 "copyFailures=%llu depthUsed=0 motionUsed=0 fenceWaitUs=%llu",
                 (unsigned long long)frames, (unsigned long long)g_frame.ended_layers, n, r,
                 (unsigned long long)g_ec.copies, (unsigned long long)g_ec.failures,
                 (unsigned long long)(g_ec.settles ? g_ec.settle_us / g_ec.settles : 0));
        g_ec.settle_us = g_ec.settles = 0;
    }
}

// ---- KL_OPENVR_MOTION: a deterministic moving head, for the host
//
// "yaw_deg,pitch_deg,period_frames" (e.g. "25,8,90"; period 0 holds the head
// still at that yaw and pitch, the static control): before each WaitGetPoses
// the head is published to kl_ovrp — as a frontend would publish ARKit's — at
// yaw = Y sin(2 pi n / P), pitch = X sin(2 pi n / (1.618 P)), standing at the
// eye height, n the frame about to be opened. Everything downstream (kl_ovrp's
// latch and prediction, kl_openxr's VIEW and views, this file's poses, the
// layer) is the real chain; the static-head host boot can never move it
// otherwise. Each layer then logs the head its two pictures were drawn at
// ("motion: layer ...") with the swapchain image it names, so a KL_GLFB_OUT
// capture's mtl_eye<e>_s<image>.png can be matched to its pose. Off by default.
static struct { int on; float yaw, pitch, period; } g_motion = { -1, 0, 0, 0 };

static int klvr_motion_on(void) {
    if (g_motion.on < 0) {
        g_motion.on = 0;
        const char *e = getenv("KL_OPENVR_MOTION");
        float y = 0, p = 0, n = 0;
        if (e && sscanf(e, "%f,%f,%f", &y, &p, &n) == 3 && (n >= 2 || n == 0)) {
            g_motion.on = 1;
            g_motion.yaw = y; g_motion.pitch = p; g_motion.period = n;
            klvr_log("KL_OPENVR_MOTION: head yaw %.1f deg, pitch %.1f deg, period %.0f frames "
                     "(pitch period x1.618), published to kl_ovrp before each WaitGetPoses",
                     y, p, n);
        } else if (e) {
            klvr_log("KL_OPENVR_MOTION=\"%s\": expected \"yaw,pitch,period\" — motion OFF", e);
        }
    }
    return g_motion.on;
}

// Yaw (about +Y) and pitch (about the turned +X) of a pose's forward (-Z) axis.
static void klvr_yaw_pitch(klxr_Posef p, float *yaw, float *pitch) {
    klvr_HmdMatrix34_t m = klvr_mat34(p);
    float fx = -m.m[0][2], fy = -m.m[1][2], fz = -m.m[2][2];
    *yaw = atan2f(-fx, -fz) * 57.2957795f;
    *pitch = asinf(fy < -1 ? -1 : fy > 1 ? 1 : fy) * 57.2957795f;
}

static void klvr_motion_publish(uint32_t n) {
    const float tau = 6.28318531f;
    // Period 0: the head held still at (yaw, pitch) — the static control.
    float sy0 = g_motion.period > 0 ? sinf(tau * (float)n / g_motion.period) : 1.0f;
    float sx0 = g_motion.period > 0 ? sinf(tau * (float)n / (1.618f * g_motion.period)) : 1.0f;
    float y = g_motion.yaw * sy0 * 0.0174532925f;
    float x = g_motion.pitch * sx0 * 0.0174532925f;
    // q = Ry(y) * Rx(x): turn, then look up/down about the turned X.
    float cy = cosf(y * 0.5f), sy = sinf(y * 0.5f), cx = cosf(x * 0.5f), sx = sinf(x * 0.5f);
    // At the headless display's eye height (m_boot's rule), not kl_ovrp_eye_height(),
    // which answers in the rebased origin and puts the head on the floor.
    static float eye_y = -1.0f;
    if (eye_y < 0) eye_y = kl_env_float("KL_OVRP_EYE_HEIGHT", 1.6f);
    kl_ovrp_set_head_pose(0.0f, eye_y, 0.0f, cy * sx, sy * cx, -sy * sx, cy * cx);
}

// The frame's end, decided: one projection layer when both eyes' Submits this
// frame left a complete picture in their swapchains (this frame's own when the
// copy is synchronous, the previous frame's when it is asynchronous — see
// "sync" in the Submit block), no layer otherwise. Each view's pose is the head
// that picture was rendered at (its frame's WaitGetPoses head, or the one its
// Submit named) composed with the eye-to-head offset the guest was given
// (GetEyeToHeadTransform reads the same g_eyes), in the compositor's tracking
// origin — so the layer says exactly what the guest rendered with. Under the lock.
static void klvr_frame_finish(void) {
    if (!g_frame.begun) return;
    if (g_frame.copied != 3 || !g_ec.sc[0] || !g_ec.sc[1] ||
        g_ec.last[0] < 0 || g_ec.last[1] < 0) {
        klvr_frame_end(NULL, 0);
        return;
    }
    klxr_ProjectionView v[2];
    memset(v, 0, sizeof v);
    for (int e = 0; e < 2; e++) {
        v[e].type = KLXR_T_COMPOSITION_LAYER_PROJECTION_VIEW;
        v[e].pose = klvr_pose_mul(g_ec.ready_head[e], klvr_eye_head_pose(e));
        v[e].fov = g_ec.ready_fov_ok[e] ? g_ec.ready_fov[e] : klvr_eye_fov(e);
        v[e].subImage.swapchain = g_ec.sc[e];
        v[e].subImage.w = (int32_t)g_ec.w;
        v[e].subImage.h = (int32_t)g_ec.h;
    }
    klxr_ProjectionLayer pl = { KLXR_T_COMPOSITION_LAYER_PROJECTION, NULL, 0,
                                klvr_origin_space(klvr.origin), 2, v };
    if (!g_ec.said_geo++)
        for (int e = 0; e < 2; e++)
            klvr_log("eye-layer eye=%d origin=%d pose=(%.4f %.4f %.4f) q=(%.4f %.4f %.4f %.4f) "
                     "fov=%.6f,%.6f,%.6f,%.6f rect=0,0,%u,%u swapchain=%p%s%s",
                     e, klvr.origin, v[e].pose.position.x, v[e].pose.position.y,
                     v[e].pose.position.z, v[e].pose.orientation.x, v[e].pose.orientation.y,
                     v[e].pose.orientation.z, v[e].pose.orientation.w, v[e].fov.angleLeft,
                     v[e].fov.angleRight, v[e].fov.angleUp, v[e].fov.angleDown,
                     g_ec.w, g_ec.h, g_ec.sc[e],
                     g_ec.ready_sub[e] ? " (pose from Submit)" : " (pose from WaitGetPoses)",
                     g_ec.ready_fov_ok[e] ? ", fov from the Submit's projection"
                                          : ", fov from GetProjectionRaw");
    if (g_motion.on > 0) {
        float y0, p0, y1, p1;
        klvr_yaw_pitch(g_ec.ready_head[0], &y0, &p0);
        klvr_yaw_pitch(g_ec.ready_head[1], &y1, &p1);
        float hy, hp;
        klxr_Posef handed = klvr_pose_of(&klvr.last_render[0].mDeviceToAbsoluteTracking);
        klvr_yaw_pitch(handed, &hy, &hp);
        klvr_log("motion: layer at frame seq=%u: eye0 image %d head yaw %.3f pitch %.3f%s, "
                 "eye1 image %d head yaw %.3f pitch %.3f; this frame's WaitGetPoses head yaw "
                 "%.3f pitch %.3f", g_frame.seq, g_ec.last[0], y0, p0,
                 g_ec.ready_sub[0] ? " (Submit's pose)" : " (WaitGetPoses')",
                 g_ec.last[1], y1, p1, hy, hp);
    }
    const void *layers[1] = { &pl };
    klvr_frame_end(layers, 1);
}

static void klvr_poses_out(klvr_TrackedDevicePose_t *dst, uint32_t n) {
    if (!dst) return;
    if (n > KLVR_MAX_DEVICES) n = KLVR_MAX_DEVICES;
    memcpy(dst, klvr.last_render, n * sizeof *dst);
}

static void klvr_poses_invalid(klvr_TrackedDevicePose_t *dst, uint32_t n) {
    if (!dst) return;
    if (n > KLVR_MAX_DEVICES) n = KLVR_MAX_DEVICES;
    for (uint32_t i = 0; i < n; i++) {
        memset(&dst[i], 0, sizeof dst[i]);
        dst[i].mDeviceToAbsoluteTracking = klvr_identity34();
        dst[i].eTrackingResult = KLVR_TRACKING_UNINITIALIZED;
    }
}

// The pose prediction id for the frame just opened (see the ring's comment):
// display intervals since the first frame, plus one, never less than last
// frame's + 1. Call with klvr_lock held.
static void klvr_pred_advance(int64_t display_time, int64_t period) {
    static int mode = -1;
    if (mode < 0) {
        const char *m = kl_env_str("KL_OPENVR_PREDICTION_ID", "vsync");
        mode = m && !strcmp(m, "frame") ? 0 : 1;
        klvr_log("pose prediction id = %s (KL_OPENVR_PREDICTION_ID=%s)", mode
                 ? "display intervals: Source 2 advances one tick per id step, so the world "
                   "runs at wall speed whatever the frame rate"
                 : "frame counter: ONE tick per frame — the world slows to fps/Hz", mode ? "vsync" : "frame");
    }
    uint32_t next = g_frame.pred + 1;
    if (mode && period > 0 && display_time > 0) {
        int64_t idx = (display_time + period / 2) / period;
        if (!g_frame.pred) g_frame.pred_base = idx - 1;
        int64_t v = idx - g_frame.pred_base;
        if (v > (int64_t)next && v < (int64_t)next + 100000) {
            g_frame.pred_skipped += (uint64_t)(v - (int64_t)next);
            next = (uint32_t)v;
        } else if (v > (int64_t)next) {
            g_frame.pred_base = idx - next;         // a clock jump: re-anchor, one step
        }
    }
    g_frame.pred = next;
}

// ---- host instruments for the frame clock (round N13-B) ----
//
// KL_OPENVR_FRAME_DELAY_MS=<ms>: sleep that long in every WaitGetPoses before
// xrWaitFrame — a guest that misses the display, on a host that otherwise
// keeps up (the headset ran the level at 31-43 fps against a 90 Hz panel).
//
// KL_OPENVR_GAMETIME=<frames>: every <frames> WaitGetPoses, log wall time, the
// frame count and Valve's client globals (libclient's gpGlobals, the 0.174-
// 0.178 payload's +0x2481c58: realtime, framecount, curtime, frametime ...),
// so game seconds per wall second can be read off two lines. Diagnostic only.
static uint8_t *klvr_libclient_base(void) {
    static uint8_t *base;
    if (base) return base;
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    if (!m) return NULL;
    kl_dl_report_images(m);
    fclose(m);
    for (char *l = buf; l && *l; ) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        char name[128];
        void *b = NULL;
        if (sscanf(l, " %127s %p", name, &b) == 2 && !strcmp(name, "libclient.so")) base = b;
        l = nl ? nl + 1 : NULL;
    }
    free(buf);
    return base;
}

static void klvr_gametime_probe(uint32_t seq) {
    static int every = -1;
    if (every < 0) every = kl_env_int("KL_OPENVR_GAMETIME", 0);
    if (every <= 0 || seq % (uint32_t)every) return;
    uint8_t *cb = klvr_libclient_base();
    const uint8_t *gv = cb ? *(const uint8_t *const *)(cb + 0x2481c58) : NULL;
    double wall = (double)klvr_now_ns() / 1e9;
    if (!gv) { klvr_log("gametime seq=%u wall=%.4f (no client globals yet)", seq, wall); return; }
    char line[1024];
    int o = 0;
    for (int i = 0; i < 16 && o < (int)sizeof line - 40; i++) {
        float f; int32_t v;
        memcpy(&f, gv + 4 * i, 4); memcpy(&v, gv + 4 * i, 4);
        o += snprintf(line + o, sizeof line - (size_t)o, " +%02x=%g/%d", 4 * i, f, v);
    }
    klvr_log("gametime seq=%u wall=%.4f%s", seq, wall, line);
}

static int32_t klvr_Compositor_WaitGetPoses(void *self, klvr_TrackedDevicePose_t *render,
                                            uint32_t nrender, klvr_TrackedDevicePose_t *game,
                                            uint32_t ngame) {
    KLVR_COUNT(Compositor, 2);
    uint64_t call = KLVR_N(Compositor, 2);
    pthread_mutex_lock(&klvr_lock);
    klvr_pump();
    if (!klvr.session || !klvr.running) {
        klvr_poses_invalid(render, nrender);
        klvr_poses_invalid(game, ngame);
        pthread_mutex_unlock(&klvr_lock);
        if (klvr_log_this(call))
            klvr_log("Compositor.WaitGetPoses call#=%llu -> RequestFailed (session %s)",
                     (unsigned long long)call, klvr.session ? "not running" : "absent");
        usleep(11000);              // a display period, so a retrying caller cannot spin
        return KLVR_COMPOSITOR_REQUEST_FAILED;
    }
    // The second eye's Submit (or PostPresentHandoff) normally ended the frame
    // this opened last time; this catches one left open — a guest that
    // submitted one eye or none ends it with no layer.
    if (g_frame.begun) klvr_frame_finish();
    void *session = klvr.session;
    uint32_t next_seq = g_frame.seq + 1;
    pthread_mutex_unlock(&klvr_lock);
    if (klvr_motion_on()) klvr_motion_publish(next_seq);

    static int delay_ms = -1;
    if (delay_ms < 0) delay_ms = kl_env_int("KL_OPENVR_FRAME_DELAY_MS", 0);
    if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000);

    int64_t t0 = klvr_now_ns();
    struct { int32_t type; const void *next; } wi = { 33, NULL };     // FRAME_WAIT_INFO
    struct { int32_t type; void *next; int64_t time, period; uint32_t should_render; } fs =
        { 44, NULL, 0, 0, 0 };                                        // FRAME_STATE
    klxr_Result r = klvr_xr.xrWaitFrame(session, &wi, &fs);           // may block
    int64_t waited = klvr_now_ns() - t0;

    pthread_mutex_lock(&klvr_lock);
    if (r == 0) {
        struct { int32_t type; const void *next; } bi = { 46, NULL }; // FRAME_BEGIN_INFO
        r = klvr_xr.xrBeginFrame(klvr.session, &bi);
    }
    if (r != 0) {
        klvr_poses_invalid(render, nrender);
        klvr_poses_invalid(game, ngame);
        pthread_mutex_unlock(&klvr_lock);
        klvr_log("Compositor.WaitGetPoses: xrWaitFrame/xrBeginFrame -> %d", r);
        return KLVR_COMPOSITOR_REQUEST_FAILED;
    }
    g_frame.begun = 1;
    g_frame.should_render = (int)fs.should_render;
    klvr.display_time = fs.time;
    klvr_eyes_update(fs.time);
    for (uint32_t i = 0; i < KLVR_MAX_DEVICES; i++)
        klvr_locate_device(i, klvr.origin, fs.time, &klvr.last_render[i]);
    g_frame.seq++;
    uint32_t pred_was = g_frame.pred;
    klvr_pred_advance(fs.time, fs.period);
    uint32_t ticks = g_frame.pred - pred_was;
    int slot = g_frame.pred % KLVR_RING;
    g_frame.ring[slot].id = g_frame.pred;
    memcpy(g_frame.ring[slot].p, klvr.last_render, sizeof klvr.last_render);
    klvr_poses_out(render, nrender);
    klvr_poses_out(game, ngame);
    uint32_t seq = g_frame.seq, pred = g_frame.pred;
    int valid = klvr.last_render[0].bPoseIsValid;
    const klvr_HmdMatrix34_t *hm = &klvr.last_render[0].mDeviceToAbsoluteTracking;
    float hx = hm->m[0][3], hy = hm->m[1][3], hz = hm->m[2][3];
    pthread_mutex_unlock(&klvr_lock);
    kl_perf_role(KL_PERF_ROLE_WAIT);
    kl_perf_frame(ticks, waited);
    klvr_gametime_probe(seq);
    if (klvr_log_this(call))
        klvr_log("Compositor.WaitGetPoses call#=%llu render=%u game=%u valid=%d seq=%u id=%u "
                 "predicted=%lld waitedUs=%lld origin=%d hmd=(%.3f %.3f %.3f)",
                 (unsigned long long)call, nrender, ngame, valid, seq, pred,
                 (long long)fs.time, (long long)(waited / 1000), klvr.origin, hx, hy, hz);
    return KLVR_COMPOSITOR_OK;
}

static void klvr_Compositor_SetTrackingSpace(void *self, uint32_t origin) {
    KLVR_COUNT(Compositor, 0);
    klvr.origin = (int)origin;
    klvr_log("Compositor.SetTrackingSpace %u", origin);
}

static uint32_t klvr_Compositor_GetTrackingSpace(void *self) {
    KLVR_COUNT(Compositor, 1);
    return (uint32_t)klvr.origin;
}

static int32_t klvr_Compositor_GetLastPoses(void *self, klvr_TrackedDevicePose_t *render,
                                            uint32_t nrender, klvr_TrackedDevicePose_t *game,
                                            uint32_t ngame) {
    KLVR_COUNT(Compositor, 3);
    pthread_mutex_lock(&klvr_lock);
    klvr_poses_out(render, nrender);
    klvr_poses_out(game, ngame);
    pthread_mutex_unlock(&klvr_lock);
    return KLVR_COMPOSITOR_OK;
}

static int32_t klvr_Compositor_GetLastPoseForTrackedDeviceIndex(void *self, uint32_t i,
                                                                klvr_TrackedDevicePose_t *out,
                                                                klvr_TrackedDevicePose_t *game) {
    KLVR_COUNT(Compositor, 4);
    if (i >= KLVR_MAX_DEVICES) return KLVR_COMPOSITOR_INDEX_OUT_OF_RANGE;
    pthread_mutex_lock(&klvr_lock);
    if (out)  *out  = klvr.last_render[i];
    if (game) *game = klvr.last_render[i];
    pthread_mutex_unlock(&klvr_lock);
    return KLVR_COMPOSITOR_OK;
}

// "Copy into this texture instead" — not offered: the guest submits its own
// image (Submit), which is the path the reference saw the game take.
static int32_t klvr_Compositor_GetSubmitTexture(void *self, klvr_Texture_t *out, uint8_t *flush,
                                                uint32_t usage, const klvr_Texture_t *tex,
                                                const klvr_VRTextureBounds_t *bounds,
                                                uint32_t flags) {
    KLVR_COUNT(Compositor, 5);
    klvr_log("Compositor.GetSubmitTexture usage=%u flags=0x%x -> RequestFailed", usage, flags);
    return KLVR_COMPOSITOR_REQUEST_FAILED;
}

// ---- Submit: the eye copy ----------------------------------------------------
//
// One Submit = one eye: the guest's VkImage (a sub-rect of it, chosen by the
// bounds — Source 2 submits ONE 4416x2400 image twice, uv 0..0.5 then 0.5..1)
// is copied into that eye's OpenXR swapchain image, and the second eye's Submit
// ends the frame with the projection layer (klvr_frame_finish).
//
//   binding    the first Submit binds the session to the device the texture
//              names (kl_openvr_bind_vulkan: instance, physical device, device,
//              queue family; the queue INDEX is not in VRVulkanTextureData_t
//              and is taken as 0). A later Submit on another device re-binds and
//              rebuilds the swapchains there.
//   swapchains one per eye, at the recommended render size, created lazily on
//              the first Submit, in the guest's own VkFormat when kl_openxr
//              offers it (else R8G8B8A8_SRGB); the blit converts otherwise.
//   the copy   vkCmdBlitImage from the rect into the whole swapchain image (a
//              scale when the sizes differ: LINEAR if the format can filter).
//              A multisampled source is resolved first (vkCmdResolveImage into a
//              single-sample scratch image of the rect's size), then blitted.
//   layouts    OpenVR's Vulkan contract: the app hands the image over in
//              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL and gets it back in it, so
//              the source needs a barrier but no transition (KL_OPENVR_SUBMIT_
//              LAYOUT=<VkImageLayout> names another, which is transitioned to
//              TRANSFER_SRC and back). The swapchain image goes UNDEFINED ->
//              TRANSFER_DST -> COLOR_ATTACHMENT_OPTIMAL (OpenXR's release layout).
//   sync       recorded per eye and submitted on the guest's own queue (OpenVR
//              has the app guarantee the queue is not in use during Submit), with
//              a per-eye fence. ASYNCHRONOUS by default (`stage=submitted`,
//              round N8): Submit does NOT wait. The swapchain image stays
//              acquired ("held") with the copy in flight; the eye's NEXT Submit
//              waits for that fence (by then normally long signalled — the guest
//              has recorded a whole frame since), releases the image, and only
//              then is it the eye's `last` picture. So the layer a frame ends
//              with carries the PREVIOUS frame's pictures, each with the head it
//              was rendered at: one frame behind, never a half-copied image. A
//              frame whose eyes have no complete picture yet (the first) ends
//              with no layer.
//              Why the wait cannot simply move to the frame's end: xrEndFrame's
//              kl_vulkan_frame_done drains the guest's queue (the compositor's
//              "picture complete" point — it has no GPU wait of its own), which
//              on this path IS the guest's whole frame of GPU work, so the guest's
//              render thread stalled on it either way. With the copy async the
//              named images are already proven complete by our fence, so that
//              drain is switched off for this session
//              (kl_vulkan_frame_sync_external) and the guest records frame N+1
//              while the GPU finishes N.
//              The guest's source image is safe without a copy of our own: the
//              copy is submitted on the guest's queue before Submit returns, and
//              its closing barrier (TRANSFER -> ALL_COMMANDS) orders every later
//              submission on that queue — the guest's next frame's writes
//              included — after the read. (A guest that rewrote the image from
//              ANOTHER queue with no semaphore would race; SteamVR's contract is
//              the same and Source 2 renders and submits on one queue.)
//              KL_OPENVR_EYE_COPY_ASYNC=0 restores the reference host's scheme:
//              wait for the fence inside Submit (`stage=wait-fence`), this
//              frame's pictures, and the queue drain at frame end.
//   extras     Submit_TextureWithPose (0x08): the pose replaces this frame's
//              WaitGetPoses head for that eye's view. Depth (0x10) and motion
//              (0x200) are decoded and IGNORED (`depthUsed=0`, as the reference).
// Flags 0x218 = Submit_TextureWithMotion = pose + depth + motion.
typedef struct {
    const klvr_HmdMatrix34_t *pose;     // 0x08
    const void *depth, *motion;         // 0x10 / 0x200: the handles, for the log
    const klvr_HmdMatrix44_t *proj;     // 0x10: the depth info's mProjection
} klvr_submit_extras;

// Texture_t (16) [+ HmdMatrix34_t (48) if 0x08] [+ VRTextureDepthInfo_t (80) if
// 0x10: handle (8), mProjection (64), vRange (8)] [+ VRTextureMotionInfo_t if
// 0x200, which implies the other two].
static void klvr_submit_decode(const klvr_Texture_t *tex, uint32_t flags,
                               klvr_submit_extras *x) {
    const char *base = (const char *)tex;
    size_t depth_off = (flags & 0x08) ? 64 : 16;
    x->pose   = (flags & 0x08) ? (const klvr_HmdMatrix34_t *)(base + 16) : NULL;
    x->depth  = (flags & 0x10) ? *(void *const *)(base + depth_off) : NULL;
    x->proj   = (flags & 0x10) ? (const klvr_HmdMatrix44_t *)(base + depth_off + 8) : NULL;
    x->motion = (flags & 0x218) == 0x218 ? *(void *const *)(base + 144) : NULL;
}

// The frustum the guest DREW with, out of the projection matrix it submitted
// with its depth — the reference host's alyx_frame::ProjectionFov
// (libalyxhost_f1_m1+0xfe538, called from AlyxOpenVrCompositorSubmit+0x714
// whenever the Submit carries pose AND depth), ported exactly: OpenVR's
// ComposeProjection shape (m[3][2] = -1, the off-axis terms only in column 2),
// every element finite, eps 1e-5; L/R = (m02 -/+ 1)/m00, and the y pair
// (m12 -/+ 1)/m11 is DOWN/UP (OpenVR's `top` is the down tangent — the same
// convention as GetProjectionRaw below). 0 = not that shape (the caller keeps
// GetProjectionRaw's frustum).
//
// Why the layer must say THIS and not what GetProjectionRaw answered: the
// composite re-warps the picture through the display's projection from the
// frustum the layer states. If that is not the frustum the picture was drawn
// with, the picture is placed at the wrong scale/offset, and the error grows
// with the timewarp correction — under head motion the world breathes (zooms)
// at the frame rate in proportion to head speed. Source 2 picks its own render
// height (Auto-Fidelity level 5: 3168 rows for a 3234-row recommendation on the
// headset), so the two need not agree; the reference always used the matrix.
__attribute__((unused)) static int klvr_fov_from_projection(const klvr_HmdMatrix44_t *pm, klxr_Fovf *out) {
    if (!pm) return 0;
    const float eps = 1e-5f;
    float m[4][4];
    memcpy(m, pm->m, sizeof m);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (!isfinite(m[i][j])) return 0;
    if (!(m[0][0] > eps) || !(m[1][1] > eps) || fabsf(m[3][2] + 1.0f) > eps) return 0;
    const float zero[] = { m[0][1], m[0][3], m[1][0], m[1][3], m[2][0], m[2][1],
                           m[3][0], m[3][1], m[3][3] };
    for (unsigned i = 0; i < sizeof zero / sizeof zero[0]; i++)
        if (fabsf(zero[i]) > eps) return 0;
    float l = atanf((m[0][2] - 1.0f) / m[0][0]), r = atanf((m[0][2] + 1.0f) / m[0][0]);
    float d = atanf((m[1][2] - 1.0f) / m[1][1]), u = atanf((m[1][2] + 1.0f) / m[1][1]);
    const float half_pi = 1.5707963f;
    if (!(fabsf(l) < half_pi && fabsf(r) < half_pi && fabsf(u) < half_pi &&
          fabsf(d) < half_pi) || !(u - d > eps) || !(r - l > eps))
        return 0;
    out->angleLeft = l;
    out->angleRight = r;
    out->angleUp = u;
    out->angleDown = d;
    return 1;
}

// The rect the bounds select, in texels. OpenVR's bounds may run backwards (a
// GL app flips v); the blit keeps the order, so a flipped rect is a flipped copy.
static int klvr_bounds_rect(const klvr_VRTextureBounds_t *b, uint32_t w, uint32_t h,
                            int32_t r[4]) {
    float u0 = b ? b->uMin : 0.f, v0 = b ? b->vMin : 0.f;
    float u1 = b ? b->uMax : 1.f, v1 = b ? b->vMax : 1.f;
    float c[4] = { u0 * (float)w, v0 * (float)h, u1 * (float)w, v1 * (float)h };
    float lim[4] = { (float)w, (float)h, (float)w, (float)h };
    for (int i = 0; i < 4; i++) {
        if (!(c[i] >= 0)) c[i] = 0;                 // NaN too
        if (c[i] > lim[i]) c[i] = lim[i];
        r[i] = (int32_t)lroundf(c[i]);
    }
    return r[0] != r[2] && r[1] != r[3];
}

#if KLVR_HAVE_VK
static struct {
    VkQueue  queue;
    uint32_t qfam;
    VkPhysicalDevice phys;
    VkCommandPool pool;
    VkCommandBuffer cmd[2];
    VkFence  fence[2];
    int      pending[2];            // a copy whose fence wait timed out
    VkImage  tmp[2];                // the MSAA resolve scratch, per eye
    VkDeviceMemory tmp_mem[2];
    uint32_t tmp_w[2], tmp_h[2];
    VkFormat tmp_fmt[2];
    int      linear;                // the source format can be LINEAR-filtered
    VkFormat linear_fmt;
#define KLVR_VKFN(X) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)   \
    X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier)    \
    X(CmdBlitImage) X(CmdResolveImage) X(QueueSubmit) X(CreateFence) X(DestroyFence)          \
    X(WaitForFences) X(ResetFences) X(CreateImage) X(DestroyImage)                            \
    X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory)          \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory)       \
    X(MapMemory) X(UnmapMemory) X(CmdCopyImageToBuffer) X(FreeCommandBuffers)
#define X(n) PFN_vk##n n;
    KLVR_VKFN(X)
#undef X
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
} g_vkc;

// Teardown vs. the unlocked Vulkan work. Settle's fence wait and the copy's
// record/submit run with klvr_lock DROPPED (the GPU wait must not serialize the
// other eye or the frame loop), using g_vkc's fences, command buffers and the
// eye swapchain images. klvr_eyecopy_down (session end, a device move) frees
// all of those, so it must not run while any such window is open: each window
// is counted in klvr_ec_busy (under the lock), and teardown waits on
// klvr_ec_idle until the count is 0 before touching anything. Outside g_ec /
// g_vkc on purpose: both are memset by teardown.
static int klvr_ec_busy;
static pthread_cond_t klvr_ec_idle = PTHREAD_COND_INITIALIZER;

static void klvr_ec_enter(void) { klvr_ec_busy++; }            // under the lock
static void klvr_ec_leave(void) {                               // under the lock
    if (--klvr_ec_busy == 0) pthread_cond_broadcast(&klvr_ec_idle);
}

static const VkImageSubresourceRange klvr_color1 = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

static VkImageLayout klvr_src_layout(void) {
    static int l = -1;
    if (l < 0) {
        l = kl_env_int("KL_OPENVR_SUBMIT_LAYOUT", VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        klvr_log("Submit: the guest's image is taken to arrive in VkImageLayout %d%s", l,
                 l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                     ? " (TRANSFER_SRC_OPTIMAL, OpenVR's Vulkan contract) and is left in it"
                     : " (KL_OPENVR_SUBMIT_LAYOUT) and is put back in it after the copy");
    }
    return (VkImageLayout)l;
}

// The Vulkan half of the binding: the entry points (the same door the guest
// came through, kl_vulkan's), one command pool on the queue's family, one
// command buffer and fence per eye. Runs on the submitting thread.
static int klvr_vk_setup(const klvr_VRVulkanTextureData_t *vk) {
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)kl_vulkan_sym("vkGetDeviceProcAddr");
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)kl_vulkan_sym("vkGetInstanceProcAddr");
    if (!gdpa || !gipa) {
        klvr_log("Submit: kl_vulkan serves no vkGetDeviceProcAddr — no eye copy");
        return -1;
    }
    VkDevice dev = (VkDevice)vk->m_pDevice;
    int missing = 0;
#define X(n) if (!(g_vkc.n = (PFN_vk##n)gdpa(dev, "vk" #n))) { klvr_log("Submit: no vk" #n); missing++; }
    KLVR_VKFN(X)
#undef X
    g_vkc.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
        gipa((VkInstance)vk->m_pInstance, "vkGetPhysicalDeviceMemoryProperties");
    g_vkc.GetPhysicalDeviceFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties)
        gipa((VkInstance)vk->m_pInstance, "vkGetPhysicalDeviceFormatProperties");
    if (missing) return -1;
    g_vkc.queue = (VkQueue)vk->m_pQueue;
    g_vkc.qfam = vk->m_nQueueFamilyIndex;
    g_vkc.phys = (VkPhysicalDevice)vk->m_pPhysicalDevice;
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_vkc.qfam };
    if (g_vkc.CreateCommandPool(dev, &pci, NULL, &g_vkc.pool) != VK_SUCCESS) return -1;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
                                       g_vkc.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2 };
    if (g_vkc.AllocateCommandBuffers(dev, &ai, g_vkc.cmd) != VK_SUCCESS) return -1;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    for (int e = 0; e < 2; e++)
        if (g_vkc.CreateFence(dev, &fci, NULL, &g_vkc.fence[e]) != VK_SUCCESS) return -1;
    return 0;
}

// Under the lock, with no settle or copy in its unlocked window (klvr_ec_busy
// 0). Every copy still outstanding — submitted and not yet settled, whether its
// image is held or its settle timed out — is waited before its fence, scratch
// and (in klvr_eyecopy_down) its destination swapchain image are destroyed.
static void klvr_vk_down(void) {
    VkDevice dev = (VkDevice)g_ec.device;
    if (dev && g_vkc.DestroyFence) {
        int waited = 0, hung = 0;
        for (int e = 0; e < 2; e++) {
            if (g_vkc.fence[e]) {
                if ((g_vkc.pending[e] || g_ec.held[e] >= 0) && g_vkc.WaitForFences) {
                    waited++;
                    if (g_vkc.WaitForFences(dev, 1, &g_vkc.fence[e], VK_TRUE,
                                            2000000000ULL) != VK_SUCCESS)
                        hung++;
                }
                g_vkc.DestroyFence(dev, g_vkc.fence[e], NULL);
            }
            if (g_vkc.tmp[e]) g_vkc.DestroyImage(dev, g_vkc.tmp[e], NULL);
            if (g_vkc.tmp_mem[e]) g_vkc.FreeMemory(dev, g_vkc.tmp_mem[e], NULL);
        }
        if (g_vkc.pool) g_vkc.DestroyCommandPool(dev, g_vkc.pool, NULL);
        if (waited)
            klvr_log("eye copy teardown: waited %d outstanding copy fence(s)%s", waited,
                     hung ? " — one did NOT signal in 2 s" : ", all signalled");
    }
    memset(&g_vkc, 0, sizeof g_vkc);
}

// The single-sample scratch an MSAA rect is resolved into, per eye; remade when
// the rect or the format changes.
static VkImage klvr_vk_tmp(int e, VkFormat fmt, uint32_t w, uint32_t h) {
    if (g_vkc.tmp[e] && g_vkc.tmp_w[e] == w && g_vkc.tmp_h[e] == h && g_vkc.tmp_fmt[e] == fmt)
        return g_vkc.tmp[e];
    VkDevice dev = (VkDevice)g_ec.device;
    if (g_vkc.tmp[e]) g_vkc.DestroyImage(dev, g_vkc.tmp[e], NULL);
    if (g_vkc.tmp_mem[e]) g_vkc.FreeMemory(dev, g_vkc.tmp_mem[e], NULL);
    g_vkc.tmp[e] = VK_NULL_HANDLE;
    g_vkc.tmp_mem[e] = VK_NULL_HANDLE;
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = (VkExtent3D){ w, h, 1 };
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    if (g_vkc.CreateImage(dev, &ici, NULL, &img) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    g_vkc.GetImageMemoryRequirements(dev, img, &req);
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof mp);
    if (g_vkc.GetPhysicalDeviceMemoryProperties)
        g_vkc.GetPhysicalDeviceMemoryProperties(g_vkc.phys, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    for (uint32_t i = 0; i < 32 && type == UINT32_MAX; i++)
        if (req.memoryTypeBits & (1u << i)) type = i;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (type == UINT32_MAX || g_vkc.AllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS ||
        g_vkc.BindImageMemory(dev, img, mem, 0) != VK_SUCCESS) {
        if (mem) g_vkc.FreeMemory(dev, mem, NULL);
        g_vkc.DestroyImage(dev, img, NULL);
        return VK_NULL_HANDLE;
    }
    g_vkc.tmp[e] = img; g_vkc.tmp_mem[e] = mem;
    g_vkc.tmp_w[e] = w; g_vkc.tmp_h[e] = h; g_vkc.tmp_fmt[e] = fmt;
    return img;
}

static VkImageMemoryBarrier klvr_barrier(VkImage img, uint32_t layer, VkImageLayout from,
                                         VkImageLayout to, VkAccessFlags src,
                                         VkAccessFlags dst) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src; b.dstAccessMask = dst;
    b.oldLayout = from;    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = klvr_color1;
    b.subresourceRange.baseArrayLayer = layer;
    return b;
}

// Record and submit (no wait: klvr_eyecopy_settle waits). Returns NULL, or a
// reason. On NULL the fence is pending and the eye's image is in flight.
static const char *klvr_vk_copy(int e, const klvr_VRVulkanTextureData_t *vk, uint32_t layer,
                                const int32_t r[4], VkImage dst) {
    VkDevice dev = (VkDevice)g_ec.device;
    VkCommandBuffer cb = g_vkc.cmd[e];
    VkFence fence = g_vkc.fence[e];
    // Diagnostic: KL_OPENVR_EYE_COPY=0 records nothing (Submit then answers
    // RequestFailed), to tell a hang in the copy from one in the guest's frame.
    static int copy_on = -1;
    if (copy_on < 0) copy_on = kl_env_on("KL_OPENVR_EYE_COPY", 1);
    if (!copy_on) return "copy-disabled";
    if (g_vkc.pending[e]) {                     // a copy whose settle timed out
        if (g_vkc.WaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ULL) != VK_SUCCESS)
            return "previous-copy-hung";
        g_vkc.pending[e] = 0;
    }
    VkImage src = (VkImage)(uintptr_t)vk->m_nImage;
    VkImageLayout held = klvr_src_layout();
    uint32_t samples = vk->m_nSampleCount ? vk->m_nSampleCount : 1;
    VkFormat sfmt = (VkFormat)vk->m_nFormat;
    if (g_vkc.linear_fmt != sfmt || !g_vkc.linear) {
        VkFormatProperties fp;
        memset(&fp, 0, sizeof fp);
        if (g_vkc.GetPhysicalDeviceFormatProperties)
            g_vkc.GetPhysicalDeviceFormatProperties(g_vkc.phys, sfmt, &fp);
        g_vkc.linear = (fp.optimalTilingFeatures &
                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? 1 : -1;
        g_vkc.linear_fmt = sfmt;
    }
    VkImage resolve = VK_NULL_HANDLE;
    int32_t rx = r[0] < r[2] ? r[0] : r[2], ry = r[1] < r[3] ? r[1] : r[3];
    uint32_t rw = (uint32_t)abs(r[2] - r[0]), rh = (uint32_t)abs(r[3] - r[1]);
    if (samples > 1 && !(resolve = klvr_vk_tmp(e, sfmt, rw, rh))) return "resolve-scratch-failed";

    if (g_vkc.ResetFences(dev, 1, &fence) != VK_SUCCESS ||
        g_vkc.ResetCommandBuffer(cb, 0) != VK_SUCCESS) return "reset-failed";
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    if (g_vkc.BeginCommandBuffer(cb, &bi) != VK_SUCCESS) return "begin-failed";

    // Everything the guest wrote to its image becomes visible to the copy; the
    // swapchain image's old contents are discarded (the copy covers it whole).
    VkImageMemoryBarrier pre[3] = {
        klvr_barrier(src, layer, held, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        klvr_barrier(dst, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT),
        klvr_barrier(resolve, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    g_vkc.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                             resolve ? 3 : 2, pre);
    VkImage bsrc = src;
    uint32_t blayer = layer;
    int32_t b[4] = { r[0], r[1], r[2], r[3] };
    if (resolve) {
        VkImageResolve rr;
        memset(&rr, 0, sizeof rr);
        rr.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
        rr.srcOffset = (VkOffset3D){ rx, ry, 0 };
        rr.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        rr.extent = (VkExtent3D){ rw, rh, 1 };
        g_vkc.CmdResolveImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolve,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rr);
        VkImageMemoryBarrier mid = klvr_barrier(resolve, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_TRANSFER_READ_BIT);
        g_vkc.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &mid);
        bsrc = resolve;
        blayer = 0;
        // The scratch holds the rect upright; a flipped request flips here.
        b[0] = r[0] <= r[2] ? 0 : (int32_t)rw; b[2] = r[0] <= r[2] ? (int32_t)rw : 0;
        b[1] = r[1] <= r[3] ? 0 : (int32_t)rh; b[3] = r[1] <= r[3] ? (int32_t)rh : 0;
    }
    VkImageBlit bl;
    memset(&bl, 0, sizeof bl);
    bl.srcSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, blayer, 1 };
    bl.srcOffsets[0] = (VkOffset3D){ b[0], b[1], 0 };
    bl.srcOffsets[1] = (VkOffset3D){ b[2], b[3], 1 };
    bl.dstSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bl.dstOffsets[1] = (VkOffset3D){ (int32_t)g_ec.w, (int32_t)g_ec.h, 1 };
    VkFilter filter = (rw == g_ec.w && rh == g_ec.h) || g_vkc.linear < 0
                          ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    g_vkc.CmdBlitImage(cb, bsrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, filter);

    // The swapchain image in OpenXR's release layout; the guest's image back in
    // the layout it handed over, and nothing it does next overtakes the read.
    VkImageMemoryBarrier post[2] = {
        klvr_barrier(dst, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
        klvr_barrier(src, layer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, held,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
    };
    g_vkc.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, post);
    if (g_vkc.EndCommandBuffer(cb) != VK_SUCCESS) return "end-failed";

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    int tag_was = kl_perf_gpu_tag(KL_PERF_GPU_COPY);
    VkResult sr = g_vkc.QueueSubmit(g_vkc.queue, 1, &si, fence);
    kl_perf_gpu_tag(tag_was);
    if (sr != VK_SUCCESS) return "submit-failed";
    g_vkc.pending[e] = 1;
    if (resolve) g_ec.resolves++;
    return NULL;
}

// The eye's copy in flight, finished: wait for its fence (lock NOT held — this
// is the GPU wait), then under the lock release the swapchain image and make it
// the eye's `last` picture with the head it was rendered at. Returns 1 when a
// picture became complete, 0 when none was in flight, -1 when the fence did not
// signal in 2 s (the image is released anyway so the swapchain is not wedged,
// but it is NOT named by a layer, and the next copy into this eye waits again —
// "previous-copy-hung" — before it reuses the command buffer).
static int klvr_eyecopy_settle(int e, int64_t *waited_us) {
    // Everything the wait uses is snapshotted under the lock, and the window is
    // counted (klvr_ec_busy) so teardown cannot free the fence underneath it.
    pthread_mutex_lock(&klvr_lock);
    int held = g_ec.held[e];
    VkDevice dev = (VkDevice)g_ec.device;
    VkFence fence = g_vkc.fence[e];
    int pending = g_vkc.pending[e];
    PFN_vkWaitForFences wait = g_vkc.WaitForFences;
    if (held < 0) {
        pthread_mutex_unlock(&klvr_lock);
        return 0;
    }
    klvr_ec_enter();
    pthread_mutex_unlock(&klvr_lock);
    int64_t t0 = klvr_now_ns();
    VkResult wr = pending && fence && wait && dev
                      ? wait(dev, 1, &fence, VK_TRUE, 2000000000ULL)
                      : VK_SUCCESS;
    int64_t wait_ns = klvr_now_ns() - t0;
    int64_t us = wait_ns / 1000;
    kl_perf_fence_wait(wait_ns);
    if (waited_us) *waited_us += us;
    pthread_mutex_lock(&klvr_lock);
    klvr_ec_leave();
    if (g_ec.held[e] != held || !g_ec.sc[e]) {      // not ours any more
        pthread_mutex_unlock(&klvr_lock);
        return 0;
    }
    struct { int32_t type; const void *next; } ri = { KLXR_T_SWAPCHAIN_IMAGE_RELEASE_INFO, NULL };
    klvr_xr.xrReleaseSwapchainImage(g_ec.sc[e], &ri);
    g_ec.held[e] = -1;
    g_ec.settle_us += (uint64_t)us;
    g_ec.settles++;
    int ok = wr == VK_SUCCESS;
    if (ok) {
        g_vkc.pending[e] = 0;
        g_ec.last[e] = held;
        g_ec.ready_head[e] = g_ec.pend_head[e];
        g_ec.ready_sub[e] = g_ec.pend_sub[e];
        g_ec.ready_fov[e] = g_ec.pend_fov[e];
        g_ec.ready_fov_ok[e] = g_ec.pend_fov_ok[e];
        g_ec.copies++;
    } else {
        g_ec.failures++;
    }
    pthread_mutex_unlock(&klvr_lock);
    if (!ok)
        klvr_log("eye copy: eye=%d fence did not signal in 2 s (VkResult %d) — image %d "
                 "released unnamed; reason=fence-timeout", e, (int)wr, held);
    return ok ? 1 : -1;
}
#endif

static void klvr_eyecopy_down(void) {               // under the lock
#if KLVR_HAVE_VK
    // A settle or copy in its unlocked window is using the fences / images:
    // let it finish (bounded by its 2 s fence timeout) before freeing them.
    while (klvr_ec_busy > 0) pthread_cond_wait(&klvr_ec_idle, &klvr_lock);
    klvr_vk_down();
#endif
    for (int e = 0; e < 2; e++)
        if (g_ec.sc[e] && klvr.session && klvr_xr.xrDestroySwapchain)
            klvr_xr.xrDestroySwapchain(g_ec.sc[e]);
    uint64_t copies = g_ec.copies, failures = g_ec.failures, resolves = g_ec.resolves;
    memset(&g_ec, 0, sizeof g_ec);
    g_ec.last[0] = g_ec.last[1] = -1;
    g_ec.held[0] = g_ec.held[1] = -1;
#if KLVR_HAVE_VK
    kl_vulkan_frame_sync_external(0);               // nothing of ours in flight now
#endif
    g_ec.copies = copies; g_ec.failures = failures; g_ec.resolves = resolves;
}

// The two eye swapchains, made on the first Submit (under the lock), at the
// size of the rect that Submit names — the eye the guest actually drew, so the
// copy is 1:1 (no filter, no stretch). Source 2 does not draw the recommended
// size: on the headset it drew 4032x3168 of a 4032x3234 recommendation (its
// Auto-Fidelity level's height), on the host 2208 of 2290 — the old fixed-size
// swapchain resampled every picture. A later Submit with a different rect is
// still blitted (scaled) into these. KL_OPENVR_EYE_SIZE=rec restores the
// recommended-size swapchains.
static int klvr_eyecopy_swapchains(const klvr_VRVulkanTextureData_t *vk, uint32_t rw,
                                   uint32_t rh) {
    if (g_ec.sc[0] && g_ec.sc[1]) return 0;
    klvr_render_size(&g_ec.w, &g_ec.h);
    static int by_rect = -1;
    if (by_rect < 0) {
        const char *m = kl_env_str("KL_OPENVR_EYE_SIZE", "guest");
        by_rect = m && !strcmp(m, "rec") ? 0 : 1;
    }
    if (by_rect && rw >= 16 && rh >= 16 && rw <= 8192 && rh <= 8192) {
        if (rw != g_ec.w || rh != g_ec.h)
            klvr_log("eye swapchains sized to the guest's eye %ux%u, not the recommended "
                     "%ux%u (KL_OPENVR_EYE_SIZE=rec keeps that)", rw, rh, g_ec.w, g_ec.h);
        g_ec.w = rw;
        g_ec.h = rh;
    }
    int64_t fmts[16];
    uint32_t nf = 0;
    if (klvr_xr.xrEnumerateSwapchainFormats(klvr.session, 16, &nf, fmts) != 0) nf = 0;
    int64_t pick = 0;
    for (uint32_t i = 0; i < nf && !pick; i++)      // the guest's own (a colour format)
        if (fmts[i] == (int64_t)vk->m_nFormat && fmts[i] != 124 && fmts[i] != 126 &&
            fmts[i] != 129)
            pick = fmts[i];
    for (uint32_t i = 0; i < nf && !pick; i++) if (fmts[i] == 43) pick = 43;  // R8G8B8A8_SRGB
    for (uint32_t i = 0; i < nf && !pick; i++) if (fmts[i] == 37) pick = 37;  // ..._UNORM
    if (!pick) {
        klvr_log("Submit: kl_openxr offers no colour swapchain format (%u listed)", nf);
        return -1;
    }
    g_ec.fmt = pick;
    for (int e = 0; e < 2; e++) {
        klxr_SwapchainCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.type = KLXR_T_SWAPCHAIN_CREATE_INFO;
        ci.usageFlags = 0x1 | 0x10 | 0x20;          // COLOR_ATTACHMENT | TRANSFER_DST | SAMPLED
        ci.format = pick;
        ci.sampleCount = ci.faceCount = ci.arraySize = ci.mipCount = 1;
        ci.width = g_ec.w;
        ci.height = g_ec.h;
        klxr_Result r = klvr_xr.xrCreateSwapchain(klvr.session, &ci, &g_ec.sc[e]);
        uint32_t n = 0;
        klxr_SwapchainImageVulkan im[KLVR_SC_IMAGES];
        memset(im, 0, sizeof im);
        for (int i = 0; i < KLVR_SC_IMAGES; i++) im[i].type = KLXR_T_SWAPCHAIN_IMAGE_VULKAN;
        if (r == 0) klvr_xr.xrEnumerateSwapchainImages(g_ec.sc[e], 0, &n, NULL);
        if (r != 0 || !n || n > KLVR_SC_IMAGES ||
            klvr_xr.xrEnumerateSwapchainImages(g_ec.sc[e], n, &n, im) != 0) {
            klvr_log("Submit: eye %d swapchain %ux%u VkFormat %lld -> %d (%u images)", e,
                     g_ec.w, g_ec.h, (long long)pick, r, n);
            if (r == 0) klvr_xr.xrDestroySwapchain(g_ec.sc[e]);
            g_ec.sc[e] = NULL;
            if (e) { klvr_xr.xrDestroySwapchain(g_ec.sc[0]); g_ec.sc[0] = NULL; }
            return -1;
        }
        g_ec.nimg[e] = n;
        for (uint32_t i = 0; i < n; i++) g_ec.img[e][i] = im[i].image;
    }
    klvr_log("eye swapchains: 2 x %ux%u VkFormat %lld, %u image(s) each, on VkDevice %p "
             "(guest image %ux%u VkFormat %u samples %u)", g_ec.w, g_ec.h, (long long)pick,
             g_ec.nimg[0], g_ec.device, vk->m_nWidth, vk->m_nHeight, vk->m_nFormat,
             vk->m_nSampleCount);
    return 0;
}

// Bind the session to the texture's device, once (or again when it moves).
// On the submitting thread, lock NOT held (kl_openvr_bind_vulkan takes it).
static int klvr_eyecopy_bind(const klvr_VRVulkanTextureData_t *vk) {
    if (g_ec.device == vk->m_pDevice) return 0;
    if (g_ec.device) {
        klvr_log("Submit: the renderer moved from VkDevice %p to %p — the eye swapchains "
                 "are rebuilt on the new one", g_ec.device, vk->m_pDevice);
        pthread_mutex_lock(&klvr_lock);
        klvr_eyecopy_down();
        pthread_mutex_unlock(&klvr_lock);
    }
    pthread_mutex_lock(&klvr_lock);
    int bound = g_vk.set && g_vk.device == vk->m_pDevice;
    pthread_mutex_unlock(&klvr_lock);
    if (!bound) {
        klvr_log("Submit: binding the session to the renderer's VkDevice %p (instance %p "
                 "physical %p queue %p family %u; queue index taken as 0 — "
                 "VRVulkanTextureData_t does not carry it)", vk->m_pDevice, vk->m_pInstance,
                 vk->m_pPhysicalDevice, vk->m_pQueue, vk->m_nQueueFamilyIndex);
        kl_openvr_bind_vulkan(vk->m_pInstance, vk->m_pPhysicalDevice, vk->m_pDevice,
                              vk->m_nQueueFamilyIndex, 0);
    }
#if KLVR_HAVE_VK
    g_ec.device = vk->m_pDevice;
    if (klvr_vk_setup(vk) != 0) {
        klvr_log("Submit: the eye copy could not be set up on VkDevice %p", vk->m_pDevice);
        pthread_mutex_lock(&klvr_lock);
        klvr_eyecopy_down();
        pthread_mutex_unlock(&klvr_lock);
        return -1;
    }
    return 0;
#else
    return -1;
#endif
}

// What the guest drew with against what it was told, per eye: the first
// Submit, every change after (the first 8, then powers of two), and once if the
// projection is not OpenVR's shape (the matrix, so the convention can be read).
__attribute__((unused)) static void klvr_fov_note(int e, const klvr_HmdMatrix44_t *pm, int ok, const klxr_Fovf *d,
                          const klxr_Fovf *raw) {
    static int said_bad[2];
    static int have[2];
    static klxr_Fovf last[2];
    static uint64_t changes[2];
    const float k = 57.29578f;
    if (!pm) return;
    if (!ok) {
        if (!said_bad[e]++) {
            const float (*m)[4] = pm->m;
            klvr_log("layer fov: eye=%d the Submit's projection is not OpenVR's shape — "
                     "GetProjectionRaw's frustum is kept. m=[%g %g %g %g | %g %g %g %g | "
                     "%g %g %g %g | %g %g %g %g]", e,
                     m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
                     m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
        }
        return;
    }
    if (have[e] && fabsf(d->angleLeft - last[e].angleLeft) < 1e-4f &&
        fabsf(d->angleRight - last[e].angleRight) < 1e-4f &&
        fabsf(d->angleUp - last[e].angleUp) < 1e-4f &&
        fabsf(d->angleDown - last[e].angleDown) < 1e-4f)
        return;
    uint64_t n = have[e] ? ++changes[e] : 0;
    have[e] = 1;
    last[e] = *d;
    if (n > 8 && !klvr_log_this(n)) return;
    float hs = (tanf(d->angleRight) - tanf(d->angleLeft)) /
               (tanf(raw->angleRight) - tanf(raw->angleLeft));
    float vs = (tanf(d->angleUp) - tanf(d->angleDown)) /
               (tanf(raw->angleUp) - tanf(raw->angleDown));
    klvr_log("layer fov: eye=%d %s: DRAWN with L%.3f R%.3f U%.3f D%.3f deg, TOLD (GetProjectionRaw) "
             "L%.3f R%.3f U%.3f D%.3f deg — tangent span drawn/told %.4f x %.4f%s", e,
             n ? "changed" : "first Submit", d->angleLeft * k, d->angleRight * k,
             d->angleUp * k, d->angleDown * k, raw->angleLeft * k, raw->angleRight * k,
             raw->angleUp * k, raw->angleDown * k, hs, vs,
             fabsf(hs - 1.0f) > 1e-3f || fabsf(vs - 1.0f) > 1e-3f ||
                     fabsf(d->angleUp - raw->angleUp) > 1e-3f ||
                     fabsf(d->angleLeft - raw->angleLeft) > 1e-3f
                 ? " — THEY DIFFER; the layer states the drawn one"
                 : " — the same frustum");
}

// ---- KL_OPENVR_DUMP: the guest's own eye pictures, frame-exact (host debug)
// The dump reads the guest image with Vulkan. A build without the MoltenVK
// headers skips it; Submit already refuses the eye copy in that build.
#if KLVR_HAVE_VK
//
// KL_OPENVR_DUMP=<dir> writes the rect each Submit names — read straight out of
// the guest's image on its own queue, synchronously, before the eye copy —
// for KL_OPENVR_DUMP_N (default 4) consecutive frames from WaitGetPoses frame
// KL_OPENVR_DUMP_FROM (default 1500), as <dir>/openvr_f<seq>_eye<e>.ppm at half
// size (KL_OPENVR_DUMP_FULL=1: full), with a log line naming the head the frame was drawn at. Both eyes of a
// file pair are the SAME guest frame, which an end-of-run swapchain capture
// cannot promise while the game keeps rendering under it. Stalls the frame; a
// debugging instrument only. Single-sample sources only.
static void klvr_dump_eye(uint32_t eye, uint32_t seq, const klvr_VRVulkanTextureData_t *vk,
                          uint32_t layer, const int32_t r[4], klxr_Posef head) {
    static int init, from, count;
    static const char *dir;
    if (!init) {
        init = 1;
        dir = getenv("KL_OPENVR_DUMP");
        from = kl_env_int("KL_OPENVR_DUMP_FROM", 1500);
        count = kl_env_int("KL_OPENVR_DUMP_N", 4);
    }
    if (!dir || !*dir || (int)seq < from || (int)seq >= from + count) return;
    if ((vk->m_nSampleCount ? vk->m_nSampleCount : 1) != 1) return;
    VkDevice dev = (VkDevice)vk->m_pDevice;
    int32_t x0 = r[0] < r[2] ? r[0] : r[2], y0 = r[1] < r[3] ? r[1] : r[3];
    uint32_t w = (uint32_t)abs(r[2] - r[0]), h = (uint32_t)abs(r[3] - r[1]);
    VkDeviceSize size = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void *map = NULL;
    const char *why = NULL;
    if (g_vkc.CreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS) { why = "buffer"; goto out; }
    VkMemoryRequirements req;
    g_vkc.GetBufferMemoryRequirements(dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof mp);
    g_vkc.GetPhysicalDeviceMemoryProperties(g_vkc.phys, &mp);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            type = i;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type };
    if (type == UINT32_MAX || g_vkc.AllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS ||
        g_vkc.BindBufferMemory(dev, buf, mem, 0) != VK_SUCCESS) { why = "memory"; goto out; }
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
                                       g_vkc.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    if (g_vkc.AllocateCommandBuffers(dev, &ai, &cb) != VK_SUCCESS ||
        g_vkc.CreateFence(dev, &fci, NULL, &fence) != VK_SUCCESS) { why = "cmd"; goto out; }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    g_vkc.BeginCommandBuffer(cb, &bi);
    VkImage src = (VkImage)(uintptr_t)vk->m_nImage;
    VkImageLayout held = klvr_src_layout();
    VkImageMemoryBarrier pre = klvr_barrier(src, layer, held, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    g_vkc.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &pre);
    VkBufferImageCopy rg;
    memset(&rg, 0, sizeof rg);
    rg.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
    rg.imageOffset = (VkOffset3D){ x0, y0, 0 };
    rg.imageExtent = (VkExtent3D){ w, h, 1 };
    g_vkc.CmdCopyImageToBuffer(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rg);
    VkImageMemoryBarrier post = klvr_barrier(src, layer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, held,
                                             VK_ACCESS_TRANSFER_READ_BIT,
                                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    g_vkc.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, NULL, 0, NULL, 1, &post);
    g_vkc.EndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (g_vkc.QueueSubmit(g_vkc.queue, 1, &si, fence) != VK_SUCCESS ||
        g_vkc.WaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ULL) != VK_SUCCESS ||
        g_vkc.MapMemory(dev, mem, 0, size, 0, &map) != VK_SUCCESS) { why = "readback"; goto out; }
    {
        char path[1024];
        snprintf(path, sizeof path, "%s/openvr_f%05u_eye%u.ppm", dir, seq, eye);
        FILE *f = fopen(path, "wb");
        if (!f) { why = "fopen"; goto out; }
        // KL_OPENVR_DUMP_FULL=1: every pixel (edge/anti-aliasing comparisons);
        // default every other pixel, point-sampled.
        uint32_t st = kl_env_on("KL_OPENVR_DUMP_FULL", 0) ? 1 : 2;
        uint32_t ow = w / st, oh = h / st;
        fprintf(f, "P6\n%u %u\n255\n", ow, oh);
        const uint8_t *px = map;
        int bgra = vk->m_nFormat == VK_FORMAT_B8G8R8A8_SRGB || vk->m_nFormat == VK_FORMAT_B8G8R8A8_UNORM;
        for (uint32_t y = 0; y < oh; y++)
            for (uint32_t x = 0; x < ow; x++) {
                const uint8_t *p = px + ((size_t)(st * y) * w + st * x) * 4;
                uint8_t rgb[3] = { p[bgra ? 2 : 0], p[1], p[bgra ? 0 : 2] };
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
        float yw, pt;
        klvr_yaw_pitch(head, &yw, &pt);
        klvr_log("dump: frame seq=%u eye %u drawn at head yaw %.3f pitch %.3f -> %s", seq, eye,
                 yw, pt, path);
    }
out:
    if (why) klvr_log("dump: frame seq=%u eye %u FAILED (%s)", seq, eye, why);
    if (map) g_vkc.UnmapMemory(dev, mem);
    if (fence) g_vkc.DestroyFence(dev, fence, NULL);
    if (cb) g_vkc.FreeCommandBuffers(dev, g_vkc.pool, 1, &cb);
    if (buf) g_vkc.DestroyBuffer(dev, buf, NULL);
    if (mem) g_vkc.FreeMemory(dev, mem, NULL);
}
#endif

static int32_t klvr_eye_copy(uint32_t eye, const klvr_VRVulkanTextureData_t *vk, uint32_t layer,
                             const klvr_VRTextureBounds_t *bounds, uint32_t flags,
                             const klvr_submit_extras *x) {
    if (!vk->m_pDevice || !vk->m_nImage || !vk->m_pQueue || !vk->m_nWidth || !vk->m_nHeight)
        return 102;                                 // InvalidTexture
    int32_t r[4];
    if (!klvr_bounds_rect(bounds, vk->m_nWidth, vk->m_nHeight, r)) return 109; // InvalidBounds
    klvr_note_submitted_image((uint64_t)vk->m_nImage);
    __atomic_store_n(&g_submit_w, vk->m_nWidth, __ATOMIC_RELAXED);
    __atomic_store_n(&g_submit_h, vk->m_nHeight, __ATOMIC_RELAXED);
#if !KLVR_HAVE_VK
    static int said;
    if (!said++) klvr_log("Submit: this build has no Vulkan headers — no eye copy");
    return KLVR_COMPOSITOR_REQUEST_FAILED;
#else
    static int said_bind;
    if (klvr_eyecopy_bind(vk) != 0) {
        if (klvr_log_this((uint64_t)said_bind++)) klvr_log("Submit: no eye copy (binding failed)");
        return KLVR_COMPOSITOR_REQUEST_FAILED;
    }
    uint32_t bit = 1u << eye;
    pthread_mutex_lock(&klvr_lock);
    if (!klvr.session || !klvr.running || !g_frame.begun) {
        pthread_mutex_unlock(&klvr_lock);
        static uint64_t no_frame;
        if (klvr_log_this(no_frame++))
            klvr_log("Submit eye=%u with no frame open (WaitGetPoses first) -> RequestFailed", eye);
        return KLVR_COMPOSITOR_REQUEST_FAILED;
    }
    if (g_frame.submitted & bit) {
        pthread_mutex_unlock(&klvr_lock);
        return 108;                                 // AlreadySubmitted
    }
    g_frame.submitted |= bit;
    uint32_t seq = g_frame.seq;
    // The head this picture is drawn at: the Submit's own pose when it names
    // one, else this frame's WaitGetPoses head. Kept with the copy, because
    // under the async scheme the layer that shows it is the NEXT frame's.
    klxr_Posef head = klvr_pose_of(x->pose ? x->pose
                                           : &g_frame.ring[g_frame.pred % KLVR_RING].p[0]
                                                  .mDeviceToAbsoluteTracking);
    // ...and the frustum it is drawn with: the Submit's own projection when it
    // carries one (see klvr_fov_from_projection), else what GetProjectionRaw
    // answered. Kept with the copy for the same reason as the head.
    klxr_Fovf raw = klvr_eye_fov((int)eye), drawn = raw;
    pthread_mutex_unlock(&klvr_lock);
    static int fov_mode = -1;
    if (fov_mode < 0) {
        const char *m = kl_env_str("KL_OPENVR_LAYER_FOV", "submit");
        fov_mode = m && !strcmp(m, "raw") ? 0 : 1;
        klvr_log("layer fov: %s", fov_mode
                 ? "the Submit's projection matrix when it carries one (the reference "
                   "host's ProjectionFov), else GetProjectionRaw's"
                 : "GetProjectionRaw's (KL_OPENVR_LAYER_FOV=raw)");
    }
    int drawn_ok = fov_mode && x->proj && klvr_fov_from_projection(x->proj, &drawn);
    klvr_fov_note((int)eye, x->proj, drawn_ok, &drawn, &raw);

    // Async by default; read per Submit so a run (or the unit test) can switch.
    int async = kl_env_on("KL_OPENVR_EYE_COPY_ASYNC", 1);
    kl_vulkan_frame_sync_external(async);
    static int said_mode = -1;
    if (said_mode != async) {
        said_mode = async;
        klvr_log("eye copy: %s", async
                 ? "ASYNC (stage=submitted): Submit does not wait; the eye's next Submit "
                   "settles the copy, a frame's layer shows the previous frame's pictures, "
                   "and the frame-end queue drain is off"
                 : "SYNC (stage=wait-fence, KL_OPENVR_EYE_COPY_ASYNC=0): the fence is "
                   "waited inside Submit, the frame-end queue drain is on");
    }
    // The eye's previous copy finishes first: its swapchain image is released
    // (there is one acquire per swapchain at a time) and becomes `last`.
    if (getenv("KL_OPENVR_DUMP")) klvr_dump_eye(eye, seq, vk, layer, r, head);
    int64_t waited = 0;
    int got = klvr_eyecopy_settle((int)eye, &waited) > 0;

    pthread_mutex_lock(&klvr_lock);
    uint32_t idx = 0;
    const char *why = NULL;
    if (!klvr.session || !klvr.running || !g_vkc.fence[eye] || !g_ec.device)
        why = "torn-down";                          // the session ended meanwhile
    else if (klvr_eyecopy_swapchains(vk, (uint32_t)abs(r[2] - r[0]),
                                     (uint32_t)abs(r[3] - r[1])) != 0)
        why = "no-swapchain";
    if (!why && g_ec.held[eye] >= 0) why = "previous-copy-hung";   // settle lost a race
    if (!why) {
        struct { int32_t type; const void *next; } ai = { KLXR_T_SWAPCHAIN_IMAGE_ACQUIRE_INFO, NULL };
        struct { int32_t type; const void *next; int64_t timeout; } wi =
            { KLXR_T_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, 1000000000LL };
        if (klvr_xr.xrAcquireSwapchainImage(g_ec.sc[eye], &ai, &idx) != 0 ||
            idx >= g_ec.nimg[eye])
            why = "acquire-failed";
        else if (klvr_xr.xrWaitSwapchainImage(g_ec.sc[eye], &wi) != 0)
            why = "wait-failed";
    }
    VkImage dst = why ? VK_NULL_HANDLE : (VkImage)(uintptr_t)g_ec.img[eye][idx];
    int acquired = !why || !strcmp(why, "wait-failed");
    // Logged once per change: what the copy converts between.
    if (!why && (g_ec.pair_src != vk->m_nFormat || g_ec.pair_samples != vk->m_nSampleCount)) {
        g_ec.pair_src = vk->m_nFormat;
        g_ec.pair_samples = vk->m_nSampleCount;
        klvr_log("eye copy: guest VkFormat %u, %u sample(s), %ux%u -> swapchain VkFormat %lld "
                 "%ux%u: %s%s", vk->m_nFormat, vk->m_nSampleCount, vk->m_nWidth, vk->m_nHeight,
                 (long long)g_ec.fmt, g_ec.w, g_ec.h,
                 vk->m_nSampleCount > 1 ? "RESOLVE (vkCmdResolveImage) then " : "",
                 (int64_t)vk->m_nFormat == g_ec.fmt ? "vkCmdBlitImage (same format)"
                                                    : "vkCmdBlitImage CONVERTS the format");
    }
    if (!why) klvr_ec_enter();                      // teardown waits for the copy
    pthread_mutex_unlock(&klvr_lock);

    int copying = !why;
    if (!why) why = klvr_vk_copy((int)eye, vk, layer, r, dst);

    pthread_mutex_lock(&klvr_lock);
    if (copying) klvr_ec_leave();                   // held is set before teardown can run
    if (!why) {                                     // in flight: held until settled
        g_ec.held[eye] = (int)idx;
        g_ec.pend_head[eye] = head;
        g_ec.pend_sub[eye] = x->pose != NULL;
        g_ec.pend_fov[eye] = drawn;
        g_ec.pend_fov_ok[eye] = drawn_ok;
    } else {
        if (acquired) {
            struct { int32_t type; const void *next; } ri = { KLXR_T_SWAPCHAIN_IMAGE_RELEASE_INFO, NULL };
            klvr_xr.xrReleaseSwapchainImage(g_ec.sc[eye], &ri);
        }
        g_ec.failures++;
    }
    uint64_t n = g_ec.copies + g_ec.failures;
    pthread_mutex_unlock(&klvr_lock);

    // The reference host's scheme: this copy finished before Submit returns.
    if (!why && !async) {
        int sr = klvr_eyecopy_settle((int)eye, &waited);
        if (sr > 0) got = 1;
        else if (sr < 0) { got = 0; why = "fence-timeout"; }
    }

    pthread_mutex_lock(&klvr_lock);
    if (g_frame.begun && g_frame.seq == seq) {
        if (got) g_frame.copied |= bit;
        if ((g_frame.submitted & 3) == 3) klvr_frame_finish();
    }
    pthread_mutex_unlock(&klvr_lock);
    if (why || klvr_log_this(n))
        klvr_log("guest-eye-copy call=%llu eye=%u copied=%d result=%d reason=%s stage=%s "
                 "src=%ux%u samples=%u layer=%u rect=%d,%d,%d,%d dst=%ux%u image=%u "
                 "flags=0x%x depthUsed=0 motionUsed=0 waitedUs=%lld",
                 (unsigned long long)n, eye, !why, why ? 1 : 0, why ? why : "complete",
                 async ? "submitted" : "wait-fence",
                 vk->m_nWidth, vk->m_nHeight, vk->m_nSampleCount, layer, r[0], r[1], r[2], r[3],
                 g_ec.w, g_ec.h, idx, flags, (long long)waited);
    return why ? KLVR_COMPOSITOR_REQUEST_FAILED : KLVR_COMPOSITOR_OK;
#endif
}

int kl_openvr_eye_image(int eye, uint64_t *vk_image, uint32_t *w, uint32_t *h, int64_t *fmt) {
    if (eye < 0 || eye > 1) return 0;
    pthread_mutex_lock(&klvr_lock);
    int i = g_ec.last[eye], ok = i >= 0 && g_ec.sc[eye];
    if (ok) {
        if (vk_image) *vk_image = g_ec.img[eye][i];
        if (w) *w = g_ec.w;
        if (h) *h = g_ec.h;
        if (fmt) *fmt = g_ec.fmt;
    }
    pthread_mutex_unlock(&klvr_lock);
    return ok;
}

static int32_t klvr_submit(const char *what, uint64_t call, uint32_t eye,
                           const klvr_Texture_t *tex, uint32_t array_index,
                           const klvr_VRTextureBounds_t *b, uint32_t flags, const void *caller) {
    if (!tex || eye > 1) return 102;                        // InvalidTexture
    kl_perf_role(KL_PERF_ROLE_SUBMIT);
    klvr_submit_extras x;
    klvr_submit_decode(tex, flags, &x);
    const klvr_VRVulkanTextureData_t *vk =
        tex->eType == 2 ? (const klvr_VRVulkanTextureData_t *)tex->handle : NULL;
    const klvr_VRVulkanTextureArrayData_t *va =
        vk && (flags & 0x40) ? (const klvr_VRVulkanTextureArrayData_t *)tex->handle : NULL;
    // SubmitWithArrayIndex names the layer itself; Submit takes it from the
    // array data when Submit_VulkanTextureWithArrayData says there is some.
    uint32_t layer = !strcmp(what, "SubmitWithArrayIndex") ? array_index
                   : va ? va->m_unArrayIndex : 0;
    // Calls 0-3 and powers of two, and any call whose flags differ from the
    // last one logged: a new submission shape is always worth its line.
    static uint32_t logged_flags = UINT32_MAX;
    if (klvr_log_this(call) || flags != __atomic_load_n(&logged_flags, __ATOMIC_RELAXED)) {
        __atomic_store_n(&logged_flags, flags, __ATOMIC_RELAXED);
        char where[160];
        klvr_where(caller, where, sizeof where);
        char pose[64] = "none";
        if (x.pose)
            snprintf(pose, sizeof pose, "(%.3f %.3f %.3f)", x.pose->m[0][3], x.pose->m[1][3],
                     x.pose->m[2][3]);
        if (vk)
            klvr_log("Compositor.%s eye=%u call#=%llu caller=%s image=0x%llx size=%ux%u format=%u "
                     "samples=%u flags=0x%x color=%d array=%u/%u uv=%f,%f,%f,%f device=%p "
                     "gpu=%p instance=%p queue=%p family=%u pose=%s depth=%p motion=%p",
                     what, eye, (unsigned long long)call, where,
                     (unsigned long long)vk->m_nImage, vk->m_nWidth, vk->m_nHeight,
                     vk->m_nFormat, vk->m_nSampleCount, flags, tex->eColorSpace,
                     layer, va ? va->m_unArraySize : 1,
                     b ? b->uMin : 0.f, b ? b->vMin : 0.f, b ? b->uMax : 1.f, b ? b->vMax : 1.f,
                     vk->m_pDevice, vk->m_pPhysicalDevice, vk->m_pInstance, vk->m_pQueue,
                     vk->m_nQueueFamilyIndex, pose, x.depth, x.motion);
        else
            klvr_log("Compositor.%s eye=%u call#=%llu caller=%s texture type %d handle %p "
                     "flags=0x%x -> InvalidTexture (only Vulkan, type 2, is composited)",
                     what, eye, (unsigned long long)call, where, tex->eType, tex->handle, flags);
    }
    if (!vk) return 102;
    return klvr_eye_copy(eye, vk, layer, b, flags, &x);
}

static int32_t klvr_Compositor_Submit(void *self, uint32_t eye, const klvr_Texture_t *tex,
                                      const klvr_VRTextureBounds_t *b, uint32_t flags) {
    KLVR_COUNT(Compositor, 6);
    return klvr_submit("Submit", KLVR_N(Compositor, 6), eye, tex, 0, b, flags,
                       __builtin_return_address(0));
}

static int32_t klvr_Compositor_SubmitWithArrayIndex(void *self, uint32_t eye,
                                                    const klvr_Texture_t *tex, uint32_t index,
                                                    const klvr_VRTextureBounds_t *b,
                                                    uint32_t flags) {
    KLVR_COUNT(Compositor, 7);
    return klvr_submit("SubmitWithArrayIndex", KLVR_N(Compositor, 7), eye, tex, index, b, flags,
                       __builtin_return_address(0));
}

static void klvr_Compositor_ClearLastSubmittedFrame(void *self) {
    KLVR_COUNT(Compositor, 8);
    klvr_log("Compositor.ClearLastSubmittedFrame");
}

// The guest is done with the frame: end it if the second Submit has not
// (with the layer if both eyes were copied, without one otherwise).
static void klvr_Compositor_PostPresentHandoff(void *self) {
    KLVR_COUNT(Compositor, 9);
    pthread_mutex_lock(&klvr_lock);
    if (g_frame.begun && g_frame.submitted) klvr_frame_finish();
    pthread_mutex_unlock(&klvr_lock);
}

// Frame timing. The reference answered `false` (its stub) thousands of times a
// boot, and Source 2 is one engine that feeds these numbers into its adaptive
// resolution — so the oracle's answer is the default and the filled one is
// KL_OPENVR_FRAME_TIMING=1 (frame index, system time, the HMD pose; no GPU
// times, which nothing here measures).
static int klvr_fill_timing(klvr_Compositor_FrameTiming *t, uint32_t frames_ago) {
    if (!t || t->m_nSize != sizeof *t) return 0;
    memset((char *)t + 4, 0, sizeof *t - 4);
    t->m_nFrameIndex = g_frame.pred > frames_ago ? g_frame.pred - frames_ago : 0;
    t->m_nNumFramePresents = 1;
    t->m_flSystemTimeInSeconds = (double)klvr_now_ns() / 1e9;
    t->m_HmdPose = klvr.last_render[0];
    return 1;
}

static uint8_t klvr_Compositor_GetFrameTiming(void *self, klvr_Compositor_FrameTiming *t,
                                              uint32_t frames_ago) {
    KLVR_COUNT(Compositor, 10);
    if (!kl_env_on("KL_OPENVR_FRAME_TIMING", 0)) return 0;
    pthread_mutex_lock(&klvr_lock);
    int r = klvr_fill_timing(t, frames_ago);
    pthread_mutex_unlock(&klvr_lock);
    return (uint8_t)r;
}

static uint32_t klvr_Compositor_GetFrameTimings(void *self, klvr_Compositor_FrameTiming *t,
                                                uint32_t n) {
    KLVR_COUNT(Compositor, 11);
    if (!kl_env_on("KL_OPENVR_FRAME_TIMING", 0) || !t) return 0;
    uint32_t got = 0;
    pthread_mutex_lock(&klvr_lock);
    for (uint32_t i = 0; i < n; i++) {
        // m_nSize is set on the FIRST entry by the caller; the array stride is it.
        t[i].m_nSize = t[0].m_nSize;
        got += (uint32_t)klvr_fill_timing(&t[i], n - 1 - i);
    }
    pthread_mutex_unlock(&klvr_lock);
    return got;
}

static void klvr_Compositor_FadeGrid(void *self, float seconds, uint32_t fade_in) {
    KLVR_COUNT(Compositor, 16);
    g_frame.grid_alpha = KLVR_BOOL(fade_in) ? 1.0f : 0.0f;
    klvr_log("Compositor.FadeGrid seconds=%g in=%d", seconds, KLVR_BOOL(fade_in));
}

static float klvr_Compositor_GetCurrentGridAlpha(void *self) {
    KLVR_COUNT(Compositor, 17);
    return g_frame.grid_alpha;
}

// No skybox or stage diorama is drawn by this compositor: both are refused,
// as the reference refused them (`result=1`, `accepted=0`).
static int32_t klvr_Compositor_SetSkyboxOverride(void *self, const klvr_Texture_t *t, uint32_t n) {
    KLVR_COUNT(Compositor, 18);
    klvr_log("Compositor.SetSkyboxOverride count=%u type=%d -> RequestFailed", n, t ? t->eType : -1);
    return KLVR_COMPOSITOR_REQUEST_FAILED;
}

static void klvr_Compositor_SuspendRendering(void *self, uint32_t suspend) {
    KLVR_COUNT(Compositor, 34);
    g_frame.suspended = KLVR_BOOL(suspend);
    klvr_log("Compositor.SuspendRendering %d", g_frame.suspended);
}

static int32_t klvr_Compositor_SetStageOverride_Async(void *self, const char *path,
                                                      const klvr_HmdMatrix34_t *xf,
                                                      const void *settings, uint32_t size) {
    KLVR_COUNT(Compositor, 48);
    klvr_log("Compositor.SetStageOverride_Async path=%s settingsSize=%u -> RequestFailed",
             path ? path : "(null)", size);
    return KLVR_COMPOSITOR_REQUEST_FAILED;
}

static void klvr_Compositor_ClearStageOverride(void *self) {
    KLVR_COUNT(Compositor, 49);
    klvr_log("Compositor.ClearStageOverride");
}

static int32_t klvr_Compositor_GetLastPosePredictionIDs(void *self, uint32_t *render,
                                                        uint32_t *game) {
    KLVR_COUNT(Compositor, 51);
    pthread_mutex_lock(&klvr_lock);
    uint32_t id = g_frame.pred;
    pthread_mutex_unlock(&klvr_lock);
    if (render) *render = id;
    if (game) *game = id;
    return id ? KLVR_COMPOSITOR_OK : KLVR_COMPOSITOR_REQUEST_FAILED;
}

static int32_t klvr_Compositor_GetPosesForFrame(void *self, uint32_t id,
                                                klvr_TrackedDevicePose_t *poses, uint32_t n) {
    KLVR_COUNT(Compositor, 52);
    int32_t r = KLVR_COMPOSITOR_REQUEST_FAILED;
    pthread_mutex_lock(&klvr_lock);
    int slot = id % KLVR_RING;
    if (id && g_frame.ring[slot].id == id && poses) {
        if (n > KLVR_MAX_DEVICES) n = KLVR_MAX_DEVICES;
        memcpy(poses, g_frame.ring[slot].p, n * sizeof *poses);
        r = KLVR_COMPOSITOR_OK;
    }
    pthread_mutex_unlock(&klvr_lock);
    return r;
}

// ================================================================ IVRChaperone
static uint32_t klvr_Chaperone_GetCalibrationState(void *self) {
    KLVR_COUNT(Chaperone, 0);
    return 1;                                       // ChaperoneCalibrationState_OK
}
// No play-area rectangle is claimed (the reference's answer).
static uint8_t klvr_Chaperone_GetPlayAreaRect(void *self, klvr_HmdQuad_t *rect) {
    KLVR_COUNT(Chaperone, 2);
    return 0;
}
static uint8_t klvr_Chaperone_AreBoundsVisible(void *self) {
    KLVR_COUNT(Chaperone, 6);
    return 0;
}
static void klvr_Chaperone_ForceBoundsVisible(void *self, uint32_t force) {
    KLVR_COUNT(Chaperone, 7);
    klvr_log("Chaperone.ForceBoundsVisible %d", KLVR_BOOL(force));
}

// ================================================================== IVRMailbox
//
// Private, INFERRED (OPENVR-VTABLES.md §3.10): named in-memory queues. A
// register makes a mailbox; a send to a NAME queues a copy of the body on the
// mailbox registered under it, or fails (1) when there is none — which is the
// reference's answer to `hlvr/interstitials` (no WebView here); a read pops
// the oldest message: 0 read, 2 buffer too small (*required says how big), 1
// nothing queued.
enum { KLVR_MAILBOXES = 16, KLVR_MAILQ = 32 };
static struct {
    char  name[128];
    int   live;
    char *msg[KLVR_MAILQ];
    int   head, count;
} g_mail[KLVR_MAILBOXES];

static int32_t klvr_Mailbox_RegisterMailbox(void *self, const char *name, uint64_t *handle) {
    KLVR_COUNT(Mailbox, 0);
    if (!name || !handle) return 1;
    int32_t r = 1;
    pthread_mutex_lock(&klvr_lock);
    for (int i = 0; i < KLVR_MAILBOXES; i++)
        if (!g_mail[i].live) {
            memset(&g_mail[i], 0, sizeof g_mail[i]);
            snprintf(g_mail[i].name, sizeof g_mail[i].name, "%s", name);
            g_mail[i].live = 1;
            *handle = (uint64_t)i + 1;
            r = 0;
            break;
        }
    pthread_mutex_unlock(&klvr_lock);
    klvr_log("Mailbox.Register name=%s -> %d handle=%llu", name, r,
             r ? 0ULL : (unsigned long long)*handle);
    return r;
}

static int klvr_mail_ok(uint64_t h) { return h >= 1 && h <= KLVR_MAILBOXES && g_mail[h - 1].live; }

static int32_t klvr_Mailbox_UnregisterMailbox(void *self, uint64_t h) {
    KLVR_COUNT(Mailbox, 1);
    pthread_mutex_lock(&klvr_lock);
    int ok = klvr_mail_ok(h);
    if (ok) {
        for (int i = 0; i < g_mail[h - 1].count; i++)
            free(g_mail[h - 1].msg[(g_mail[h - 1].head + i) % KLVR_MAILQ]);
        memset(&g_mail[h - 1], 0, sizeof g_mail[h - 1]);
    }
    pthread_mutex_unlock(&klvr_lock);
    klvr_log("Mailbox.Unregister handle=%llu -> %d", (unsigned long long)h, ok ? 0 : 1);
    return ok ? 0 : 1;
}

static int32_t klvr_Mailbox_SendMailbox(void *self, uint64_t h, const char *dest, const char *body) {
    KLVR_COUNT(Mailbox, 2);
    int32_t r = 1;
    pthread_mutex_lock(&klvr_lock);
    for (int i = 0; dest && i < KLVR_MAILBOXES; i++)
        if (g_mail[i].live && !strcmp(g_mail[i].name, dest) && g_mail[i].count < KLVR_MAILQ) {
            char *copy = strdup(body ? body : "");
            if (!copy) break;
            g_mail[i].msg[(g_mail[i].head + g_mail[i].count) % KLVR_MAILQ] = copy;
            g_mail[i].count++;
            r = 0;
            break;
        }
    pthread_mutex_unlock(&klvr_lock);
    klvr_log("Mailbox.Send handle=%llu destination=%s body=%.200s -> %d%s",
             (unsigned long long)h, dest ? dest : "(null)", body ? body : "(null)", r,
             r ? " (no mailbox registered under that name)" : "");
    return r;
}

static int32_t klvr_Mailbox_ReadMailbox(void *self, uint64_t h, char *buf, uint32_t cap,
                                        uint32_t *required) {
    KLVR_COUNT(Mailbox, 3);
    int32_t r = 1;
    uint32_t need = 0;
    pthread_mutex_lock(&klvr_lock);
    if (klvr_mail_ok(h) && g_mail[h - 1].count) {
        char *m = g_mail[h - 1].msg[g_mail[h - 1].head];
        need = (uint32_t)strlen(m) + 1;
        if (!buf || cap < need) r = 2;
        else {
            memcpy(buf, m, need);
            free(m);
            g_mail[h - 1].head = (g_mail[h - 1].head + 1) % KLVR_MAILQ;
            g_mail[h - 1].count--;
            r = 0;
        }
    }
    pthread_mutex_unlock(&klvr_lock);
    if (required) *required = need;
    if (r != 1 || klvr_log_this(KLVR_N(Mailbox, 3)))
        klvr_log("Mailbox.Read handle=%llu capacity=%u required=%u -> %d",
                 (unsigned long long)h, cap, need, r);
    return r;
}

// ============================================== the vtables, the registry
KLVR_INTERFACE_TABLE(System, "IVRSystem_026", 51, KLVR_SLOTS_System);
KLVR_INTERFACE_TABLE(Compositor, "IVRCompositor_029", 53, KLVR_SLOTS_Compositor);
KLVR_INTERFACE_TABLE(Chaperone, "IVRChaperone_004", 9, KLVR_SLOTS_Chaperone);
KLVR_INTERFACE_TABLE(Overlay, "IVROverlay_028", 82, KLVR_SLOTS_Overlay);
KLVR_INTERFACE_TABLE(Applications, "IVRApplications_007", 30, KLVR_SLOTS_Applications);
KLVR_INTERFACE_TABLE(OverlayView, "IVROverlayView_003", 4, KLVR_SLOTS_OverlayView);
KLVR_INTERFACE_TABLE(RenderModels, "IVRRenderModels_006", 19, KLVR_SLOTS_RenderModels);
KLVR_INTERFACE_TABLE(HeadsetView, "IVRHeadsetView_001", 9, KLVR_SLOTS_HeadsetView);
KLVR_INTERFACE_TABLE(Mailbox, "IVRMailbox_001", 32, KLVR_SLOTS_Mailbox);
KLVR_INTERFACE_TABLE(XrProto, "IXrProto_001", 32, KLVR_SLOTS_XrProto);
KLVR_INTERFACE_TABLE(InputInternal, "IVRInputInternal_002", 32, KLVR_SLOTS_InputInternal);

// The served objects, in the order libvr requests them (OPENVR-VTABLES.md §2).
// Input's lives in kl_openvr_input.c and is spliced in by klvr_ifaces().
static klvr_iface_def g_ifaces[12];
static int g_nifaces;

static void klvr_ifaces(void) {
    if (g_nifaces) return;
    const klvr_iface_def here[] = {
        KLVR_IFACE_DEF(Applications, 30), KLVR_IFACE_DEF(System, 51),
        KLVR_IFACE_DEF(Overlay, 82),      KLVR_IFACE_DEF(XrProto, 32),
    };
    const klvr_iface_def rest[] = {
        KLVR_IFACE_DEF(OverlayView, 4),   KLVR_IFACE_DEF(InputInternal, 32),
        KLVR_IFACE_DEF(Mailbox, 32),      KLVR_IFACE_DEF(Chaperone, 9),
        KLVR_IFACE_DEF(Compositor, 53),   KLVR_IFACE_DEF(RenderModels, 19),
        KLVR_IFACE_DEF(HeadsetView, 9),
    };
    int n = 0;
    for (size_t i = 0; i < sizeof here / sizeof *here; i++) g_ifaces[n++] = here[i];
    g_ifaces[n++] = *klvr_input_iface();
    for (size_t i = 0; i < sizeof rest / sizeof *rest; i++) g_ifaces[n++] = rest[i];
    g_nifaces = n;
    // The mechanical check, at runtime too: a NULL entry is a slot the list
    // forgot, and would be a jump to 0 the first time the guest called it.
    for (int i = 0; i < n; i++)
        for (unsigned s = 0; s < g_ifaces[i].slots; s++)
            if (!g_ifaces[i].vtable[s])
                klvr_log("BUG: %s slot %u has no entry", g_ifaces[i].version, s);
}

static const klvr_iface_def *klvr_iface(const char *version) {
    klvr_ifaces();
    for (int i = 0; version && i < g_nifaces; i++)
        if (!strcmp(g_ifaces[i].version, version)) return &g_ifaces[i];
    return NULL;
}

int kl_openvr_iface_count(void) { klvr_ifaces(); return g_nifaces; }
const char *kl_openvr_iface_version(int i) {
    klvr_ifaces();
    return i >= 0 && i < g_nifaces ? g_ifaces[i].version : NULL;
}
unsigned kl_openvr_iface_slots(int i) {
    klvr_ifaces();
    return i >= 0 && i < g_nifaces ? g_ifaces[i].slots : 0;
}

int kl_openvr_slot_info(const char *version, unsigned slot, const char **name, int *is_stub,
                        uint64_t *calls) {
    const klvr_iface_def *d = klvr_iface(version);
    if (!d || slot >= d->slots) return -1;
    if (name) *name = d->recs[slot].name;
    if (is_stub) *is_stub = d->recs[slot].kind != KLVR_KIND_IMPL;
    if (calls) *calls = __atomic_load_n(&d->recs[slot].calls, __ATOMIC_RELAXED);
    return 0;
}

void kl_openvr_report(FILE *f) {
    klvr_ifaces();
    fprintf(f, "\n=== OpenVR surface (kl_openvr) ===\n");
    fprintf(f, "  init token %u, session %s, frames ended %llu with layers / %llu empty\n",
            klvr.token, klvr.running ? "running" : klvr.session ? "created" : "none",
            (unsigned long long)g_frame.ended_layers, (unsigned long long)g_frame.ended_empty);
    fprintf(f, "  frame clock: %u frames over %u pose prediction ids (display intervals), "
               "%llu interval(s) beyond one a frame = %.2f ids/frame (1.00 = never missed the display)\n",
            g_frame.seq, g_frame.pred, (unsigned long long)g_frame.pred_skipped,
            g_frame.seq ? (double)g_frame.pred / g_frame.seq : 0.0);
    for (int i = 0; i < g_nifaces; i++) {
        const klvr_iface_def *d = &g_ifaces[i];
        int shown = 0;
        for (unsigned s = 0; s < d->slots; s++) {
            uint64_t n = __atomic_load_n(&d->recs[s].calls, __ATOMIC_RELAXED);
            if (!n) continue;
            if (!shown++) fprintf(f, "  %s (%u slots)\n", d->version, d->slots);
            char where[160] = "";
            if (d->recs[s].first_caller) klvr_where(d->recs[s].first_caller, where, sizeof where);
            fprintf(f, "    slot %2u %-44s %s x%llu%s%s\n", s, d->recs[s].name,
                    d->recs[s].kind == KLVR_KIND_IMPL ? "impl" : "STUB",
                    (unsigned long long)n, where[0] ? " first caller " : "", where);
        }
    }
}

// =========================================================== the exports
//
// The libopenvr_api.so surface. Plain C, `this`-less; error codes are
// EVRInitError.
static const char *const g_null_ifaces[] = {
    "IVRIPCResourceManagerClient_004", "IVRControlPanel_006",
};

static uint32_t klvr_VR_InitInternal2(int32_t *err, uint32_t app_type, const char *startup) {
    pthread_mutex_lock(&klvr_lock);
    int32_t e = 0;
    if (!klvr.inited) {
        klvr_ifaces();
        klvr_input_reset();
        if (klvr_xr_up() != 0) {
            e = 108;                                // VRInitError_Init_HmdNotFound
            klvr_xr_down();
        } else {
            klvr.inited = 1;
            klvr.origin = KLVR_ORIGIN_STANDING;
            if (!klvr.token) klvr.token = 1;
        }
    }
    if (err) *err = e;
    uint32_t token = e ? 0 : klvr.token;
    pthread_mutex_unlock(&klvr_lock);
    klvr_log("VR_InitInternal2(applicationType=%u, startup=%s) -> token %u error %d "
             "(session %s)", app_type, startup ? startup : "(null)", token, e,
             klvr.running ? "running" : klvr.session ? "created" : "none");
    return token;
}

static void klvr_VR_ShutdownInternal(void) {
    pthread_mutex_lock(&klvr_lock);
    if (klvr.inited) {
        klvr_frame_end(NULL, 0);
        klvr_xr_down();
    }
    pthread_mutex_unlock(&klvr_lock);
    klvr_log("VR_ShutdownInternal");
}

static uint32_t klvr_VR_GetInitToken(void) { return klvr.token; }

static void *klvr_VR_GetGenericInterface(const char *version, int32_t *err) {
    const klvr_iface_def *d = klvr_iface(version);
    if (err) *err = d ? 0 : 105;                    // VRInitError_Init_InterfaceNotFound
    const char *why = "";
    for (size_t i = 0; !d && version && i < sizeof g_null_ifaces / sizeof *g_null_ifaces; i++)
        if (!strcmp(version, g_null_ifaces[i])) why = " (not served, as the reference)";
    if (!d && !*why) why = " (UNKNOWN to this runtime)";
    klvr_log("get-interface version=%s -> %p error=%d%s", version ? version : "(null)",
             d ? d->object : NULL, d ? 0 : 105, why);
    return d ? d->object : NULL;
}

static uint8_t klvr_VR_IsInterfaceVersionValid(const char *version) {
    int ok = klvr_iface(version) != NULL;
    klvr_log("version-valid version=%s valid=%d", version ? version : "(null)", ok);
    return (uint8_t)ok;
}

// IVRControlPanel is not served: the reference answered NULL (and libvr's
// own lookup of IVRControlPanel_006 then gets 105 from GetGenericInterface).
static void *klvr_VRControlPanel(void) {
    klvr_log("VRControlPanel -> NULL (not available)");
    return NULL;
}

static const struct { int32_t code; const char *sym, *text; } g_init_errors[] = {
    { 0,   "VRInitError_None",                        "No Error (0)" },
    { 1,   "VRInitError_Unknown",                     "Unknown Error (1)" },
    { 105, "VRInitError_Init_InterfaceNotFound",      "Interface not found (105)" },
    { 108, "VRInitError_Init_HmdNotFound",            "Hmd Not Found (108)" },
    { 109, "VRInitError_Init_NotInitialized",         "Not Initialized (109)" },
};

static const char *klvr_VR_GetVRInitErrorAsSymbol(int32_t e) {
    for (size_t i = 0; i < sizeof g_init_errors / sizeof *g_init_errors; i++)
        if (g_init_errors[i].code == e) return g_init_errors[i].sym;
    return "VRInitError_Unknown";
}

static const char *klvr_VR_GetVRInitErrorAsEnglishDescription(int32_t e) {
    for (size_t i = 0; i < sizeof g_init_errors / sizeof *g_init_errors; i++)
        if (g_init_errors[i].code == e) return g_init_errors[i].text;
    return "Unknown error";
}

static const struct { const char *name; void *fn; } g_exports[] = {
    { "VR_InitInternal2",                      (void *)klvr_VR_InitInternal2 },
    { "VR_ShutdownInternal",                   (void *)klvr_VR_ShutdownInternal },
    { "VR_GetInitToken",                       (void *)klvr_VR_GetInitToken },
    { "VR_GetGenericInterface",                (void *)klvr_VR_GetGenericInterface },
    { "VR_IsInterfaceVersionValid",            (void *)klvr_VR_IsInterfaceVersionValid },
    { "VR_GetVRInitErrorAsSymbol",             (void *)klvr_VR_GetVRInitErrorAsSymbol },
    { "VR_GetVRInitErrorAsEnglishDescription", (void *)klvr_VR_GetVRInitErrorAsEnglishDescription },
    { "VRControlPanel",                        (void *)klvr_VRControlPanel },
};

void *kl_openvr_sym(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_exports / sizeof *g_exports; i++)
        if (!strcmp(g_exports[i].name, name)) return g_exports[i].fn;
    return NULL;
}
