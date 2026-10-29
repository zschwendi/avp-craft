// IVRInput_011 over kl_openxr's actions — the input half of kl_openvr.
//
// SteamVR Input is a binding engine: the app declares abstract actions in a
// manifest (actions.json), a per-controller binding file maps physical
// sources onto them with modes and parameters (button, trigger, joystick,
// dpad; click thresholds, dead zones), and the app reads action state by
// handle once per UpdateActionState. Nothing of that exists in OpenXR's
// vocabulary, so it is done HERE, in two steps:
//
//   1. The manifest becomes a flat binding table. The reader is the door's
//      (kl_openvr_config.flatten_manifest — kl_openvr_manifest.c's stock one,
//      transcribed from the Alyx jailbreak's OriginalInputManifest), and what it
//      returns is the S/A/P/H/C record text that file documents: action sets,
//      actions with their types, pose and haptic bindings per hand, and one C
//      record per (action, hand, control, mode) with the mode's parameters.
//   2. The controls themselves come from ONE OpenXR action set on the Touch
//      profile (kl_openvr.c's raw surface: trigger, squeeze, stick x/y, the two
//      face buttons, stick click, grip/aim pose, haptic). UpdateActionState
//      syncs it once and evaluates every binding against that sample, so all
//      Get*ActionData calls in a frame read one snapshot, as SteamVR's do.
//
// Handles follow the reference host's scheme so logs line up with its
// telemetry: action sets 0x10000+n, actions 0x20000+n, input sources
// 0x30000+n (/user/hand/left 0x30001, /user/hand/right 0x30002,
// /user/head 0x30003); any other source path is NameNotFound, as there.
// A name is registered the first time it is asked for OR declared by the
// manifest, whichever comes first, so the order of SetActionManifestPath and
// Get*Handle does not matter.
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "kl_openvr_int.h"
#include "kl_openvr_slots.h"
#include "kl_env.h"
#include "klepton.h"

KLVR_INTERFACE_RECS(Input, "IVRInput_011", 34, KLVR_SLOTS_Input);
#define KLVR_N(T, N) (__atomic_load_n(&g_rec_##T[N].calls, __ATOMIC_RELAXED) - 1)

enum { KLVR_SETS_MAX = 64, KLVR_ACTIONS_MAX = 512, KLVR_BINDS_MAX = 1024,
       KLVR_ACTIVE_MAX = 32 };
enum { KLVR_SET_BASE = 0x10000, KLVR_ACTION_BASE = 0x20000, KLVR_SOURCE_BASE = 0x30000 };

// One C record (kl_openvr_manifest.c, flatten_source): the control is `slot` (0
// trigger, 1 grip, 2 stick, 3 lower face, 4 upper face, 5 stick click) and
// `kind` how it is read (0 digital, 1 analog, 2 stick vector, 3 dpad
// direction, 4 digital off a position-forced stick).
typedef struct {
    int   action, hand, slot, kind, source, dir, sticky;
    float on, off, deadzone, maxzone, overlap;
    int   latched;                  // the click hysteresis, per binding
} klvr_binding;

typedef struct { char name[128]; } klvr_set;

typedef struct {
    char  name[192];                // lowercased
    char  type[16];                 // boolean / vector1 / vector2 / pose / vibration / ...
    int   set;                      // index into g_sets, -1 if the name has none
    int   declared;                 // came from the manifest (else only asked for)
    int   pose_hand[2], haptic_hand[2], bound[2];
    // The UpdateActionState snapshot, per hand, and the one before it.
    int   state[2], prev_state[2];
    float x[2], y[2], prev_x[2], prev_y[2];
} klvr_action;

static klvr_set     g_sets[KLVR_SETS_MAX];
static int          g_nsets;
static klvr_action  g_actions[KLVR_ACTIONS_MAX];
static int          g_nactions;
static klvr_binding g_binds[KLVR_BINDS_MAX];
static int          g_nbinds;
static struct { uint64_t set, device; } g_active[KLVR_ACTIVE_MAX];
static int          g_nactive;
static klvr_hand_raw g_raw[2];
static int          g_manifest_loaded;
static uint64_t     g_updates;

void klvr_input_reset(void) {
    g_nsets = g_nactions = g_nbinds = g_nactive = 0;
    g_manifest_loaded = 0;
    g_updates = 0;
    memset(g_raw, 0, sizeof g_raw);
}

static void klvr_lower(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

static int klvr_set_index(const char *name, int create) {
    char low[128];
    klvr_lower(low, sizeof low, name);
    for (int i = 0; i < g_nsets; i++) if (!strcmp(g_sets[i].name, low)) return i;
    if (!create || g_nsets >= KLVR_SETS_MAX) return -1;
    snprintf(g_sets[g_nsets].name, sizeof g_sets[0].name, "%s", low);
    return g_nsets++;
}

// "/actions/<set>/in/<x>" belongs to "/actions/<set>".
static int klvr_set_of_action(const char *low) {
    const char *p = strncmp(low, "/actions/", 9) ? NULL : strchr(low + 9, '/');
    if (!p) return -1;
    char set[128];
    size_t n = (size_t)(p - low);
    if (n >= sizeof set) return -1;
    memcpy(set, low, n);
    set[n] = 0;
    return klvr_set_index(set, 1);
}

static int klvr_action_index(const char *name, int create) {
    char low[192];
    klvr_lower(low, sizeof low, name);
    for (int i = 0; i < g_nactions; i++) if (!strcmp(g_actions[i].name, low)) return i;
    if (!create || g_nactions >= KLVR_ACTIONS_MAX) return -1;
    klvr_action *a = &g_actions[g_nactions];
    memset(a, 0, sizeof *a);
    snprintf(a->name, sizeof a->name, "%s", low);
    a->set = klvr_set_of_action(low);
    return g_nactions++;
}

static klvr_action *klvr_action_of(uint64_t h) {
    uint64_t i = h - KLVR_ACTION_BASE - 1;
    return h > KLVR_ACTION_BASE && i < (uint64_t)g_nactions ? &g_actions[i] : NULL;
}

static uint64_t klvr_source_handle(int hand) { return KLVR_SOURCE_BASE + 1 + (uint64_t)hand; }

// Which hands a restrict-to-device handle allows: bit 0 left, bit 1 right.
// 0 (k_ulInvalidInputValueHandle) is "any device". -1 for a handle we never
// gave out.
static int klvr_hands_for(uint64_t device) {
    if (device == 0) return 3;
    if (device == klvr_source_handle(0)) return 1;
    if (device == klvr_source_handle(1)) return 2;
    if (device == KLVR_SOURCE_BASE + 3) return 0;          // /user/head: no hand
    return -1;
}

// Is the action's set in the list UpdateActionState was last given (for this
// hand, when the activation was restricted to one)?
static int klvr_set_active(int set, int hand) {
    for (int i = 0; set >= 0 && i < g_nactive; i++) {
        if (g_active[i].set != KLVR_SET_BASE + 1 + (uint64_t)set) continue;
        int hands = klvr_hands_for(g_active[i].device);
        if (hands < 0 || (hands & (1 << hand))) return 1;
    }
    return 0;
}

// ---- the manifest -----------------------------------------------------------
static void klvr_parse_records(const char *text) {
    int sets = 0, actions = 0, binds = 0, bad = 0;
    for (const char *line = text; line && *line; ) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char buf[512];
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, line, len);
        buf[len] = 0;
        line = nl ? nl + 1 : NULL;
        char name[192], type[32];
        int hand, slot, kind, source, dir, sticky;
        float on, off, dz, mz, ov;
        if (buf[0] == 'S' && sscanf(buf, "S %127s", name) == 1) {
            if (klvr_set_index(name, 1) >= 0) sets++;
        } else if (buf[0] == 'A' && sscanf(buf, "A %191s %31s", name, type) == 2) {
            int i = klvr_action_index(name, 1);
            if (i < 0) { bad++; continue; }
            snprintf(g_actions[i].type, sizeof g_actions[i].type, "%s", type);
            g_actions[i].declared = 1;
            actions++;
        } else if ((buf[0] == 'P' || buf[0] == 'H') &&
                   sscanf(buf + 1, " %191s %d", name, &hand) == 2 && (hand == 0 || hand == 1)) {
            int i = klvr_action_index(name, 1);
            if (i < 0) { bad++; continue; }
            if (buf[0] == 'P') g_actions[i].pose_hand[hand] = 1;
            else               g_actions[i].haptic_hand[hand] = 1;
            g_actions[i].bound[hand] = 1;
            binds++;
        } else if (buf[0] == 'C' &&
                   sscanf(buf, "C %191s %d %d %d %f %f %d %d %f %f %f %d", name, &hand, &slot,
                          &kind, &on, &off, &source, &dir, &dz, &mz, &ov, &sticky) == 12 &&
                   (hand == 0 || hand == 1) && g_nbinds < KLVR_BINDS_MAX) {
            int i = klvr_action_index(name, 1);
            if (i < 0) { bad++; continue; }
            g_binds[g_nbinds++] = (klvr_binding){ i, hand, slot, kind, source, dir, sticky,
                                                  on, off, dz, mz, ov, 0 };
            g_actions[i].bound[hand] = 1;
            binds++;
        } else if (buf[0]) {
            bad++;
        }
    }
    klvr_log("Input: manifest table sets=%d actions=%d bindings=%d unparsed=%d", sets, actions,
             binds, bad);
}

static int32_t klvr_Input_SetActionManifestPath(void *self, const char *path) {
    KLVR_COUNT(Input, 0);
    char host[1024];
    const char *hp = path ? kl_guest_path(path, host, sizeof host) : NULL;
    int (*flatten)(const char *, char **, FILE *) = klvr_config_flattener();
    int32_t r = KLVR_INPUT_OK;
    char *text = NULL;
    if (!hp) r = KLVR_INPUT_INVALID_PARAM;
    else if (!flatten)
        klvr_log("Input.SetActionManifestPath(\"%s\"): no manifest reader configured — "
                 "every action reads inactive", path);
    else if (flatten(hp, &text, stderr) != 0 || !text)
        r = KLVR_INPUT_NO_DATA;     // what the reference answered when nothing was installed
    pthread_mutex_lock(&klvr_lock);
    if (text) {
        klvr_parse_records(text);
        g_manifest_loaded = 1;
    }
    pthread_mutex_unlock(&klvr_lock);
    free(text);
    klvr_log("Input.SetActionManifestPath(\"%s\") -> host \"%s\" -> %d",
             path ? path : "(null)", hp ? hp : "(null)", r);
    return r;
}

static int32_t klvr_Input_GetActionSetHandle(void *self, const char *name, uint64_t *h) {
    KLVR_COUNT(Input, 1);
    if (!name || !h) return KLVR_INPUT_INVALID_PARAM;
    pthread_mutex_lock(&klvr_lock);
    int i = klvr_set_index(name, 1);
    pthread_mutex_unlock(&klvr_lock);
    *h = i < 0 ? 0 : KLVR_SET_BASE + 1 + (uint64_t)i;
    if (klvr_log_this(KLVR_N(Input, 1)))
        klvr_log("Input.GetActionSetHandle %s -> 0x%llx", name, (unsigned long long)*h);
    return i < 0 ? 6 /* MaxCapacityReached */ : KLVR_INPUT_OK;
}

static int32_t klvr_Input_GetActionHandle(void *self, const char *name, uint64_t *h) {
    KLVR_COUNT(Input, 2);
    if (!name || !h) return KLVR_INPUT_INVALID_PARAM;
    pthread_mutex_lock(&klvr_lock);
    int i = klvr_action_index(name, 1);
    int declared = i >= 0 && g_actions[i].declared;
    pthread_mutex_unlock(&klvr_lock);
    *h = i < 0 ? 0 : KLVR_ACTION_BASE + 1 + (uint64_t)i;
    if (klvr_log_this(KLVR_N(Input, 2)) || (g_manifest_loaded && !declared))
        klvr_log("Input.GetActionHandle %s -> 0x%llx%s", name, (unsigned long long)*h,
                 g_manifest_loaded && !declared ? " (not in the manifest)" : "");
    return i < 0 ? 6 : KLVR_INPUT_OK;
}

static int32_t klvr_Input_GetInputSourceHandle(void *self, const char *path, uint64_t *h) {
    KLVR_COUNT(Input, 3);
    if (!path || !h) return KLVR_INPUT_INVALID_PARAM;
    static const char *const known[] = { "/user/hand/left", "/user/hand/right", "/user/head" };
    *h = 0;
    for (int i = 0; i < 3; i++)
        if (!strcasecmp(path, known[i])) *h = KLVR_SOURCE_BASE + 1 + (uint64_t)i;
    klvr_log("Input.GetInputSourceHandle %s -> 0x%llx", path, (unsigned long long)*h);
    return *h ? KLVR_INPUT_OK : KLVR_INPUT_NAME_NOT_FOUND;
}

// ---- the sample and the evaluation -------------------------------------------
static float klvr_xr_float(int raw, int hand) {
    klxr_ActionStateGetInfo gi = { KLXR_T_ACTION_STATE_GET_INFO, NULL, klvr.raw[raw],
                                   klvr.hand_path[hand] };
    klxr_ActionStateFloat st;
    memset(&st, 0, sizeof st);
    st.type = KLXR_T_ACTION_STATE_FLOAT;
    return klvr_xr.xrGetActionStateFloat(klvr.session, &gi, &st) == 0 && st.isActive
               ? st.currentState : 0.0f;
}

static int klvr_xr_bool(int raw, int hand) {
    klxr_ActionStateGetInfo gi = { KLXR_T_ACTION_STATE_GET_INFO, NULL, klvr.raw[raw],
                                   klvr.hand_path[hand] };
    klxr_ActionStateBoolean st;
    memset(&st, 0, sizeof st);
    st.type = KLXR_T_ACTION_STATE_BOOLEAN;
    return klvr_xr.xrGetActionStateBoolean(klvr.session, &gi, &st) == 0 && st.isActive &&
           st.currentState;
}

static void klvr_sample(void) {
    memset(g_raw, 0, sizeof g_raw);
    if (!klvr.session || !klvr.raw_set) return;
    klxr_ActiveActionSet as = { klvr.raw_set, 0 };
    klxr_ActionsSyncInfo si = { KLXR_T_ACTIONS_SYNC_INFO, NULL, 1, &as };
    if (klvr_xr.xrSyncActions(klvr.session, &si) != 0) return;
    for (int h = 0; h < 2; h++) {
        klvr_hand_raw *r = &g_raw[h];
        r->trigger     = klvr_xr_float(KLVR_RAW_TRIGGER, h);
        r->squeeze     = klvr_xr_float(KLVR_RAW_SQUEEZE, h);
        r->sx          = klvr_xr_float(KLVR_RAW_STICK_X, h);
        r->sy          = klvr_xr_float(KLVR_RAW_STICK_Y, h);
        r->lower       = klvr_xr_bool(KLVR_RAW_LOWER, h);
        r->upper       = klvr_xr_bool(KLVR_RAW_UPPER, h);
        r->stick_click = klvr_xr_bool(KLVR_RAW_STICK_CLICK, h);
        r->menu        = h == 0 && klvr_xr_bool(KLVR_RAW_MENU, h);
        r->present     = klvr_hand_present(h);
    }
}

static float klvr_hyst(int *latched, float v, float on, float off) {
    *latched = *latched ? v > off : v >= on;
    return (float)*latched;
}

// A dpad direction: the stick past the dead zone and inside the direction's
// sector, which is 90 degrees widened by the overlap fraction.
static int klvr_dpad(const klvr_hand_raw *r, const klvr_binding *b) {
    float mag = hypotf(r->sx, r->sy);
    if (mag < b->deadzone || mag <= 0) return 0;
    float ang = atan2f(r->sy, r->sx) * (180.0f / (float)M_PI);   // east 0, north 90
    float centre = 90.0f * (float)b->dir;
    float d = fabsf(fmodf(ang - centre + 540.0f, 360.0f) - 180.0f);
    return d <= 45.0f * (1.0f + b->overlap);
}

// One binding's value for its hand: digital in *x as 0/1, analog in *x (and *y).
static void klvr_eval(klvr_binding *b, float *x, float *y) {
    const klvr_hand_raw *r = &g_raw[b->hand];
    *x = *y = 0;
    float v = b->slot == 0 ? r->trigger : b->slot == 1 ? r->squeeze
            : b->slot == 3 ? (float)r->lower : b->slot == 4 ? (float)r->upper
            : b->slot == 5 ? (float)r->stick_click : 0.0f;
    switch (b->kind) {
    case 0:                                         // digital
        *x = b->slot <= 1 ? klvr_hyst(&b->latched, v, b->on, b->off) : (v != 0);
        break;
    case 1:                                         // analog (a face button reads 0/1)
        *x = v;
        break;
    case 2: {                                       // the stick, dead/max-zoned radially
        float mag = hypotf(r->sx, r->sy);
        float span = b->maxzone - b->deadzone;
        if (mag > b->deadzone && mag > 0 && span > 0) {
            float s = (mag - b->deadzone) / span;
            if (s > 1) s = 1;
            *x = r->sx / mag * s;
            *y = r->sy / mag * s;
        }
        break;
    }
    case 3:                                         // dpad direction
        *x = (float)klvr_dpad(r, b);
        break;
    case 4:                                         // a position-forced stick as a button
        *x = klvr_hyst(&b->latched, hypotf(r->sx, r->sy), b->on, b->off);
        break;
    }
}

static int32_t klvr_Input_UpdateActionState(void *self, const klvr_VRActiveActionSet_t *sets,
                                            uint32_t size, uint32_t count) {
    KLVR_COUNT(Input, 4);
    if (size < sizeof(klvr_VRActiveActionSet_t) || (count && !sets))
        return KLVR_INPUT_INVALID_PARAM;
    pthread_mutex_lock(&klvr_lock);
    g_nactive = 0;
    for (uint32_t i = 0; i < count && g_nactive < KLVR_ACTIVE_MAX; i++) {
        const klvr_VRActiveActionSet_t *s =
            (const klvr_VRActiveActionSet_t *)((const char *)sets + (size_t)i * size);
        g_active[g_nactive].set = s->ulActionSet;
        g_active[g_nactive].device = s->ulRestrictedToDevice;
        g_nactive++;
    }
    klvr_sample();
    for (int i = 0; i < g_nactions; i++)
        for (int h = 0; h < 2; h++) {
            klvr_action *a = &g_actions[i];
            a->prev_state[h] = a->state[h];
            a->prev_x[h] = a->x[h];
            a->prev_y[h] = a->y[h];
            a->state[h] = 0;
            a->x[h] = a->y[h] = 0;
        }
    for (int i = 0; i < g_nbinds; i++) {
        klvr_binding *b = &g_binds[i];
        klvr_action *a = &g_actions[b->action];
        if (!klvr_set_active(a->set, b->hand)) { b->latched = 0; continue; }
        float x, y;
        klvr_eval(b, &x, &y);
        if (b->kind == 1 || b->kind == 2) {
            // Several bindings on one analog action: the largest wins.
            if (fabsf(x) + fabsf(y) > fabsf(a->x[b->hand]) + fabsf(a->y[b->hand])) {
                a->x[b->hand] = x;
                a->y[b->hand] = y;
            }
        } else if (x != 0) {
            a->state[b->hand] = 1;
        }
    }
    uint64_t n = g_updates++;
    int present = g_raw[0].present | g_raw[1].present << 1;
    pthread_mutex_unlock(&klvr_lock);
    if (klvr_log_this(n))
        klvr_log("Input.UpdateActionState call#=%llu size=%u count=%u hands=%d",
                 (unsigned long long)n, size, count, present);
    return KLVR_INPUT_OK;
}

// The hands an action answers for under a restrict-to-device handle.
static int klvr_action_hands(const klvr_action *a, uint64_t device, int32_t *err) {
    int hands = klvr_hands_for(device);
    if (hands < 0) { *err = KLVR_INPUT_INVALID_DEVICE; return 0; }
    *err = KLVR_INPUT_OK;
    return hands & ((a->bound[0] ? 1 : 0) | (a->bound[1] ? 2 : 0));
}

static int klvr_action_active(const klvr_action *a, int hands) {
    for (int h = 0; h < 2; h++)
        if ((hands & (1 << h)) && klvr_set_active(a->set, h)) return 1;
    return 0;
}

static int32_t klvr_Input_GetDigitalActionData(void *self, uint64_t action,
                                               klvr_InputDigitalActionData_t *d, uint32_t size,
                                               uint64_t device) {
    KLVR_COUNT(Input, 5);
    if (!d || size < sizeof *d) return KLVR_INPUT_INVALID_PARAM;
    memset(d, 0, sizeof *d);
    pthread_mutex_lock(&klvr_lock);
    const klvr_action *a = klvr_action_of(action);
    int32_t err = KLVR_INPUT_INVALID_HANDLE;
    if (a) {
        int hands = klvr_action_hands(a, device, &err);
        d->bActive = (uint8_t)klvr_action_active(a, hands);
        int prev = 0;
        for (int h = 0; h < 2; h++) {
            if (!(hands & (1 << h))) continue;
            prev |= a->prev_state[h];
            if (a->state[h] && !d->bState) { d->bState = 1; d->activeOrigin = klvr_source_handle(h); }
            if (!d->activeOrigin && d->bActive) d->activeOrigin = klvr_source_handle(h);
        }
        d->bChanged = d->bState != prev;
    }
    pthread_mutex_unlock(&klvr_lock);
    if (klvr_log_this(KLVR_N(Input, 5)) || (d->bChanged && d->bState))
        klvr_log("Input.GetDigitalActionData %s device=0x%llx -> %d active=%d pressed=%d changed=%d",
                 a ? a->name : "?", (unsigned long long)device, err, d->bActive, d->bState,
                 d->bChanged);
    return err;
}

static int32_t klvr_Input_GetAnalogActionData(void *self, uint64_t action,
                                              klvr_InputAnalogActionData_t *d, uint32_t size,
                                              uint64_t device) {
    KLVR_COUNT(Input, 6);
    if (!d || size < sizeof *d) return KLVR_INPUT_INVALID_PARAM;
    memset(d, 0, sizeof *d);
    pthread_mutex_lock(&klvr_lock);
    const klvr_action *a = klvr_action_of(action);
    int32_t err = KLVR_INPUT_INVALID_HANDLE;
    if (a) {
        int hands = klvr_action_hands(a, device, &err);
        d->bActive = (uint8_t)klvr_action_active(a, hands);
        float best = -1;
        for (int h = 0; h < 2; h++) {
            if (!(hands & (1 << h))) continue;
            float m = fabsf(a->x[h]) + fabsf(a->y[h]);
            if (m > best) {
                best = m;
                d->x = a->x[h]; d->y = a->y[h];
                d->deltaX = a->x[h] - a->prev_x[h];
                d->deltaY = a->y[h] - a->prev_y[h];
                d->activeOrigin = klvr_source_handle(h);
            }
        }
    }
    pthread_mutex_unlock(&klvr_lock);
    if (klvr_log_this(KLVR_N(Input, 6)))
        klvr_log("Input.GetAnalogActionData %s -> %d active=%d x=%f y=%f", a ? a->name : "?",
                 err, d->bActive, d->x, d->y);
    return err;
}

// A pose action's hand (its P record), narrowed by the device restriction.
static int klvr_pose_hand(const klvr_action *a, uint64_t device) {
    int hands = klvr_hands_for(device);
    for (int h = 0; h < 2; h++)
        if (hands > 0 && (hands & (1 << h)) && a->pose_hand[h]) return h;
    return -1;
}

static int32_t klvr_pose_data(uint64_t action, int origin, int64_t t,
                              klvr_InputPoseActionData_t *d, uint32_t size, uint64_t device,
                              const char *what, uint64_t call) {
    if (!d || size < sizeof *d) return KLVR_INPUT_INVALID_PARAM;
    memset(d, 0, sizeof *d);
    pthread_mutex_lock(&klvr_lock);
    const klvr_action *a = klvr_action_of(action);
    int32_t err = a ? KLVR_INPUT_OK : KLVR_INPUT_INVALID_HANDLE;
    int hand = a ? klvr_pose_hand(a, device) : -1;
    if (a && hand >= 0 && klvr_set_active(a->set, hand)) {
        d->bActive = 1;
        d->activeOrigin = klvr_source_handle(hand);
        klvr_locate_device(KLVR_DEVICE_LEFT + (uint32_t)hand, origin, t, &d->pose);
    }
    pthread_mutex_unlock(&klvr_lock);
    if (klvr_log_this(call))
        klvr_log("Input.%s %s hand=%d origin=%d -> %d active=%d valid=%d", what,
                 a ? a->name : "?", hand, origin, err, d->bActive, d->pose.bPoseIsValid);
    return err;
}

static int32_t klvr_Input_GetPoseActionDataRelativeToNow(void *self, uint64_t action,
                                                         uint32_t origin, float seconds,
                                                         klvr_InputPoseActionData_t *d,
                                                         uint32_t size, uint64_t device) {
    KLVR_COUNT(Input, 7);
    return klvr_pose_data(action, (int)origin, klvr_now_ns() + (int64_t)(seconds * 1e9), d, size,
                          device, "GetPoseActionDataRelativeToNow", KLVR_N(Input, 7));
}

static int32_t klvr_Input_GetPoseActionDataForNextFrame(void *self, uint64_t action,
                                                        uint32_t origin,
                                                        klvr_InputPoseActionData_t *d,
                                                        uint32_t size, uint64_t device) {
    KLVR_COUNT(Input, 8);
    return klvr_pose_data(action, (int)origin, klvr.display_time, d, size, device,
                          "GetPoseActionDataForNextFrame", KLVR_N(Input, 8));
}

// No hand tracking: a skeletal action is never active.
static int32_t klvr_Input_GetSkeletalActionData(void *self, uint64_t action,
                                                klvr_InputSkeletalActionData_t *d, uint32_t size) {
    KLVR_COUNT(Input, 9);
    if (!d || size < sizeof *d) return KLVR_INPUT_INVALID_PARAM;
    memset(d, 0, sizeof *d);
    return klvr_action_of(action) ? KLVR_INPUT_OK : KLVR_INPUT_INVALID_HANDLE;
}

static int32_t klvr_Input_GetDominantHand(void *self, uint32_t *role) {
    KLVR_COUNT(Input, 10);
    if (role) *role = 2;                            // TrackedControllerRole_RightHand
    return KLVR_INPUT_OK;
}

// The finger curls a summary would carry, from the Touch controls: index from
// the trigger, middle/ring/pinky from the grip, thumb from either face button
// or the stick click. The reference answered NoData (13), so that is the
// default; KL_OPENVR_SKELETAL_SUMMARY=1 fills it.
static int32_t klvr_Input_GetSkeletalSummaryData(void *self, uint64_t action, uint32_t type,
                                                 klvr_VRSkeletalSummaryData_t *d) {
    KLVR_COUNT(Input, 20);
    if (!kl_env_on("KL_OPENVR_SKELETAL_SUMMARY", 0)) return KLVR_INPUT_NO_DATA;
    if (!d) return KLVR_INPUT_INVALID_PARAM;
    memset(d, 0, sizeof *d);
    pthread_mutex_lock(&klvr_lock);
    const klvr_action *a = klvr_action_of(action);
    int hand = a && a->bound[0] && !a->bound[1] ? 0 : 1;
    if (a && strstr(a->name, "left")) hand = 0;
    const klvr_hand_raw *r = &g_raw[hand];
    d->flFingerCurl[0] = (r->lower || r->upper || r->stick_click) ? 1.0f : 0.0f;
    d->flFingerCurl[1] = r->trigger;
    d->flFingerCurl[2] = d->flFingerCurl[3] = d->flFingerCurl[4] = r->squeeze;
    pthread_mutex_unlock(&klvr_lock);
    return a ? KLVR_INPUT_OK : KLVR_INPUT_INVALID_HANDLE;
}

static int32_t klvr_Input_TriggerHapticVibrationAction(void *self, uint64_t action, float start,
                                                       float duration, float frequency,
                                                       float amplitude, uint64_t device) {
    KLVR_COUNT(Input, 23);
    pthread_mutex_lock(&klvr_lock);
    const klvr_action *a = klvr_action_of(action);
    int32_t err = a ? KLVR_INPUT_OK : KLVR_INPUT_INVALID_HANDLE;
    int hands = a ? klvr_hands_for(device) : 0, sent = 0;
    for (int h = 0; a && hands > 0 && h < 2; h++) {
        if (!(hands & (1 << h)) || !a->haptic_hand[h] || !klvr.session) continue;
        klxr_HapticActionInfo hi = { KLXR_T_HAPTIC_ACTION_INFO, NULL, klvr.raw[KLVR_RAW_HAPTIC],
                                     klvr.hand_path[h] };
        // XR_MIN_HAPTIC_DURATION (-1) for a zero-length request: a click, not silence.
        klxr_HapticVibration v = { KLXR_T_HAPTIC_VIBRATION, NULL,
                                   duration > 0 ? (int64_t)(duration * 1e9) : -1,
                                   frequency, amplitude };
        if (klvr_xr.xrApplyHapticFeedback(klvr.session, &hi, &v) == 0) sent |= 1 << h;
    }
    pthread_mutex_unlock(&klvr_lock);
    if (klvr_log_this(KLVR_N(Input, 23)))
        klvr_log("Input.TriggerHapticVibrationAction %s device=0x%llx delay=%g duration=%g "
                 "frequency=%g amplitude=%g -> %d hands=%d", a ? a->name : "?",
                 (unsigned long long)device, start, duration, frequency, amplitude, err, sent);
    return err;
}

static int32_t klvr_Input_GetActionOrigins(void *self, uint64_t set, uint64_t action,
                                           uint64_t *out, uint32_t count) {
    KLVR_COUNT(Input, 24);
    if (!out && count) return KLVR_INPUT_INVALID_PARAM;
    for (uint32_t i = 0; i < count; i++) out[i] = 0;
    pthread_mutex_lock(&klvr_lock);
    const klvr_action *a = klvr_action_of(action);
    uint32_t n = 0;
    for (int h = 0; a && h < 2; h++)
        if (a->bound[h] && n < count) out[n++] = klvr_source_handle(h);
    pthread_mutex_unlock(&klvr_lock);
    return a ? KLVR_INPUT_OK : KLVR_INPUT_INVALID_HANDLE;
}

static int32_t klvr_Input_GetOriginLocalizedName(void *self, uint64_t origin, char *buf,
                                                 uint32_t cap, int32_t sections) {
    KLVR_COUNT(Input, 25);
    int hands = klvr_hands_for(origin);
    const char *name = hands == 1 ? "Left Hand" : hands == 2 ? "Right Hand"
                     : origin == KLVR_SOURCE_BASE + 3 ? "Headset" : NULL;
    if (!name) return KLVR_INPUT_INVALID_HANDLE;
    size_t need = strlen(name) + 1;
    if (!buf || cap < need) return KLVR_INPUT_BUFFER_TOO_SMALL;
    memcpy(buf, name, need);
    return KLVR_INPUT_OK;
}

static int32_t klvr_Input_GetOriginTrackedDeviceInfo(void *self, uint64_t origin,
                                                     klvr_InputOriginInfo_t *info, uint32_t size) {
    KLVR_COUNT(Input, 26);
    if (!info || size < sizeof *info) return KLVR_INPUT_INVALID_PARAM;
    int hands = klvr_hands_for(origin);
    memset(info, 0, sizeof *info);
    if (hands != 1 && hands != 2 && origin != KLVR_SOURCE_BASE + 3)
        return KLVR_INPUT_INVALID_HANDLE;
    info->devicePath = origin;
    info->trackedDeviceIndex = hands == 1 ? KLVR_DEVICE_LEFT : hands == 2 ? KLVR_DEVICE_RIGHT
                                                                        : KLVR_DEVICE_HMD;
    return KLVR_INPUT_OK;
}

static uint8_t klvr_Input_IsUsingLegacyInput(void *self) {
    KLVR_COUNT(Input, 31);
    return 0;
}

static int32_t klvr_Input_GetBindingVariant(void *self, uint64_t device, char *buf, uint32_t cap) {
    KLVR_COUNT(Input, 33);
    if (buf && cap) buf[0] = 0;
    return KLVR_INPUT_OK;
}

KLVR_INTERFACE_TABLE(Input, "IVRInput_011", 34, KLVR_SLOTS_Input);

const klvr_iface_def *klvr_input_iface(void) {
    static const klvr_iface_def d = KLVR_IFACE_DEF(Input, 34);
    return &d;
}
