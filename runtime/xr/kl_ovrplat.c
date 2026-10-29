// libovrplatformloader.so — see kl_ovrplat.h for why this is a replacement and
// where the DRM line is drawn.
//
// BRANCH NOTE (dlc-test-override): this branch adds a local test override —
// ovr_AssetFile_GetList enumerates the asset files actually present in the
// configured asset dir and reports them with IapStatus "entitled", so a build
// can exercise the game's DLC loading path with files the user already has.
// This asserts ownership based on file presence, which is exactly what main
// refuses to do (see the DRM classifier below); it stays on this branch, is
// not for distribution, and only ever enumerates files that are really there.
//
// BRANCH NOTE (itr): this branch additionally ANSWERS
// ovr_IAP_GetViewerPurchases with an EMPTY purchase array — the viewer owns
// nothing, which is the truthful state on a host with no Oculus Store account.
// It grants no access, unlocks no content and names no SKU; see the carve-out
// comment below. Into the Radius is a LOCAL-ONLY port (never published, never
// distributed, never OTA), like the DLC override above.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>              // the cloud-save directory is created
#include "klepton.h"
#include "kl_jni.h"
#include "kl_env.h"
#include "kl_jni.h"                        // kl_jni_files_dir, for the save directory
#include "kl_ovrplat.h"

#define KL_OVRPLAT_MAX 512
static struct { const char *name; unsigned calls; int drm; } g_plat[KL_OVRPLAT_MAX];
static unsigned g_nplat;
static unsigned g_drm_calls;

static int plat_slot(const char *name) {
    for (unsigned i = 0; i < g_nplat; i++)
        if (strcmp(g_plat[i].name, name) == 0) return (int)i;
    if (g_nplat >= KL_OVRPLAT_MAX) return -1;
    g_plat[g_nplat].name  = strdup(name);
    g_plat[g_nplat].calls = 0;
    g_plat[g_nplat].drm   = 0;
    return (int)g_nplat++;
}

static void plat_hit(const char *name) {
    int s = plat_slot(name);
    if (s >= 0) g_plat[s].calls++;
}

// ---------------------------------------------------------------------------
// The DRM classifier
//
// Matched on substrings of the entry point name rather than an enumerated list,
// deliberately: the export surface is 1335 wide and a list would be the thing
// that goes stale. A new ovr_Entitlement_* or ovr_IAP_* added by a future SDK
// lands on the refusal by default, which is the direction an error should fall.
//
// What is covered and why:
//   Entitle*  — "is this user allowed to run this", the licence check. Note the
//               carve-out below: the APP's own entitlement is answered,
//               everything else in the family still refuses.
//   IAP       — purchases and their ownership records.
//   AssetFile — download and unlock of purchasable DLC; the delivery half of the
//               same question. ovr_AssetFile_* is how paid content arrives.
//               Note the carve-out below: the pure *enumeration* call is
//               answered — "nothing here" on main, the on-disk list on bs-dlc.
//   Purchase  — belt and braces for the SDK's other spelling of IAP.
static const char *const g_drm_markers[] = {
    "Entitle", "Entitlement", "IAP", "Purchase", "AssetFile", "AssetDetails",
};

// The carve-outs: names in the ownership families that we ANSWER rather than
// refuse. Exact names, not substrings, and checked BEFORE the markers below, so
// the marker list keeps its fail-closed default and anything new still lands on
// the refusal.
//
// Upstream (main) answers exactly two:
//
//   ovr_Entitlement_GetIsViewerEntitled — the APPLICATION's own licence check,
//     and the one question in this family that can actually be answered: the
//     user unpacked an APK from a device they own, i.e. from a copy of the app
//     they already bought; answering yes describes that, it does not
//     manufacture it. Upstream keeps DLC ownership refused — paid content is
//     out-of-band asset data plus a licence held by a platform absent here.
//
//   ovr_AssetFile_GetList — enumerates the DLC asset files present on this
//     device; upstream answers "nothing here", which grants no access.
//
// THIS BRANCH (itr) adds a THIRD, and it is a NEGATIVE answer rather than a
// permissive one:
//
//   ovr_IAP_GetViewerPurchases — "what has this viewer bought?" Answered with a
//     ZERO-LENGTH purchase array: nothing. That is the truthful state of a host
//     with no Oculus Store account and no purchase history, it is the same
//     shape as upstream's GetList carve-out ("nothing here"), and it grants
//     exactly nothing — no SKU is named, no entitlement is reported, no content
//     is unlocked. Into the Radius asks it on its way to the first frame (the
//     Supporter Pack check; the Frida gadget injected into this APK forges
//     "MQ-SP-WS" here, which is the thing we are NOT doing) and the refusal
//     aborted the run before any frame could land. Answering "you own nothing"
//     lets the game take its own no-DLC path — or gate itself, which is the
//     honest outcome and the answer to whether this target is viable at all.
//     The accessors over the array come with it, and they can only ever
//     describe an empty array: GetSize is 0, GetElement is NULL, HasNextPage
//     false, GetNextUrl NULL.
//     **ovr_Purchase_GetSKU is deliberately NOT carved out** — it reads a field
//     off a purchase, and there are no purchases. It is unreachable from an
//     empty array, so leaving it on the refusal costs nothing and keeps the
//     fail-closed default exactly where it should be: the moment anything here
//     ever produced a purchase to describe, the run would stop and say so.
//     Every IAP call that could DELIVER or BUY (ovr_IAP_LaunchCheckoutFlow,
//     _ConsumePurchase, _GetProductsBySKU, the next-page fetches) still aborts
//     through the "IAP" marker.
//
// THIS BRANCH (bs-dlc) moves exactly one of them: GetList answers with the
// list of asset files actually present on disk, marked IapStatus "entitled" —
// the DLC test override — plus the pure accessors over the message it builds.
// Everything that could DELIVER content (ovr_AssetFile_Download*) and every
// ownership query beyond the app's own stays refused.
static const char *const g_plat_enumeration[] = {
    "ovr_AssetFile_GetList",
    // The PER-FILE spellings of the same question. Arkham Shadow never
    // enumerates — it asks about its language packs by name, one at a time —
    // so refusing these while answering GetList is answering the question in a
    // shape this guest does not ask it in. Same source (the enumeration root,
    // stat'd), same assertion (ownership from file presence, for files the user
    // supplied from their own copy), and the same nothing for a name that is
    // not there. See klplat_status_msg. ovr_AssetFile_Download* /
    // _DownloadCancel* / _Delete* are deliberately NOT here: those DELIVER or
    // DESTROY content and keep aborting through the "AssetFile" marker.
    "ovr_AssetFile_Status",
    "ovr_AssetFile_StatusById",
    "ovr_AssetFile_StatusByName",
    "ovr_Message_GetAssetDetails",
    "ovr_Message_GetAssetDetailsArray",
    "ovr_AssetDetailsArray_GetSize",
    "ovr_AssetDetailsArray_GetElement",
    "ovr_AssetDetails_GetAssetId",
    "ovr_AssetDetails_GetAssetType",
    "ovr_AssetDetails_GetAssetType_Native",
    "ovr_AssetDetails_GetDownloadStatus",
    "ovr_AssetDetails_GetDownloadStatus_Native",
    "ovr_AssetDetails_GetFilepath",
    "ovr_AssetDetails_GetFilepath_Native",
    "ovr_AssetDetails_GetIapStatus",
    "ovr_AssetDetails_GetIapStatus_Native",
    "ovr_AssetDetails_GetLanguage",
    "ovr_AssetDetails_GetMetadata",
    "ovr_AssetDetails_GetMetadata_Native",

    // The EMPTY purchase answer (itr) and the accessors over it. See the
    // carve-out comment above: the array is always length 0, so none of these
    // can describe a purchase, and ovr_Purchase_* stays refused.
    "ovr_IAP_GetViewerPurchases",
    "ovr_Message_GetPurchaseArray",
    "ovr_PurchaseArray_GetSize",
    "ovr_PurchaseArray_GetElement",
    "ovr_PurchaseArray_GetNextUrl",
    "ovr_PurchaseArray_HasNextPage",
};

// The app-level licence check: answered yes, upstream's move of the line
// (e8089c7), which this branch inherits unchanged.
static const char *const g_plat_app_entitled[] = {
    "ovr_Entitlement_GetIsViewerEntitled",
};

static int plat_is_enumeration(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_enumeration / sizeof g_plat_enumeration[0]; i++)
        if (strcmp(name, g_plat_enumeration[i]) == 0) return 1;
    return 0;
}

static int plat_is_app_entitled(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_app_entitled / sizeof g_plat_app_entitled[0]; i++)
        if (strcmp(name, g_plat_app_entitled[i]) == 0) return 1;
    return 0;
}

static int plat_is_drm(const char *name) {
    if (plat_is_enumeration(name)) return 0;
    if (plat_is_app_entitled(name)) return 0;
    for (size_t i = 0; i < sizeof g_drm_markers / sizeof g_drm_markers[0]; i++)
        if (strstr(name, g_drm_markers[i])) return 1;
    return 0;
}

// Ownership and licence queries — everything the carve-out above does NOT name,
// which after it is the CONTENT half: IAP, purchase records, asset details, and
// the delivery calls. This aborts unconditionally — KL_PERMISSIVE is a scouting
// knob for things we intend to implement, and this is not one of them. Answering
// 0 would be worse than useless here: for a call shaped like ovr_IAP_GetProducts
// or ovr_AssetFile_DownloadById a fabricated answer is the circumvention itself,
// and a permissive run would produce it silently.
static uint64_t klplat_drm(const char *name) {
    int s = plat_slot(name);
    if (s >= 0) { g_plat[s].calls++; g_plat[s].drm = 1; }
    g_drm_calls++;
    fprintf(stderr,
            "\n[klepton] REFUSED: the guest called '%s'.\n"
            "  This is an entitlement / purchase-ownership query. Klepton does not\n"
            "  implement these and does not answer them permissively — inventing a\n"
            "  result is DRM circumvention, which is out of scope.\n"
            "  The Oculus platform is genuinely absent on this host; that is the\n"
            "  true state of affairs, and it is the app's decision what to do with\n"
            "  it. If this call is unavoidable, the target is wrong: pick an APK\n"
            "  without entitlement checks (Steam Link).\n", name);
    kl_ovrplat_report(stderr);
    kl_fatal_prepare();
    abort();
}

// Everything else: named, answered 0, and COUNTED — the terminal tier.
//
// This used to abort, and the abort was costing a launch per name. Paint the
// Town Red stopped on ovr_Leaderboard_GetEntries after a boot that had already
// composited its picture, and the surface behind it is 700 names wide in this
// guest's own metadata: the SDK's whole C binding is compiled into libil2cpp.so
// whether the title calls it or not, so "the next unimplemented name" is a well
// that does not run dry one launch at a time.
//
// Zero is the honest answer for the overwhelming majority of what lands here,
// and it is the SAME answer the classified tiers above give: an ovrRequest of 0
// is "the request could not be made" (and on a Unity guest the SDK completes it
// client-side with a synthesized error — measured, see the note above
// g_plat_request_prefix), an ovrID of 0 is "no such user", a size of 0 is an
// empty array, and NULL is "no string / no sub-object". The classified tiers
// are still worth having: they say WHICH of those a name is, in the log and in
// the census, and this one cannot.
//
// What is deliberately NOT softened: plat_is_drm runs FIRST in kl_ovrplat_sym
// and still aborts unconditionally. The ownership line is a policy, not a work
// list, and nothing here is a way around it.
//
// The cost is that a name whose contract needs a non-NULL handle gets a NULL.
// Every such name in this guest's surface is classified above — the options
// cookie, the launch-details cookie, and the three device creators — so the
// residue is getters and commands. A run that answers any of these says so
// loudly, once per name, and kl_ovrplat_report counts them, which is what makes
// the next round's log a work list instead of a single tombstone.
static unsigned g_plat_unclassified;

static uint64_t klplat_called(const char *name) {
    plat_hit(name);
    int s = plat_slot(name);
    if (s < 0 || g_plat[s].calls == 1) {
        g_plat_unclassified++;
        fprintf(stderr, "  [plat] %s -> 0 (UNCLASSIFIED Oculus Platform entry "
                        "point: no service here, and this name is not yet sorted "
                        "into a request / accessor / command tier. Zero is a "
                        "request that could not be made, an empty array and a "
                        "NULL string all at once; if this guest wanted something "
                        "else, this line is where to start)\n", name);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The completion queue — an asynchronous request has to ARRIVE somewhere
//
// An async request gets a real id and a message that arrives via
// ovr_Message_Get*. Answering `ovrRequest` 0 has the SAME "poll forever" tail
// as a fabricated id — the SDK builds its `Request` object either way and
// awaits it — so 0 buys silence, not an error path.
//
// Beat Saber 1.40 gates a screen on that silence: HealthWarningFlowCoordinator
// hands the epilepsy screen's Continue to WaitForUserAgeCategory, a coroutine
// waiting on a category OculusInit.CheckUserAgeCategoryAsync never sets because
// InitializeOculusAsync is still awaiting platform init. The button does
// nothing, silently, with the frame loop running normally.
//
// What is asserted, and by whose decision. Answering the age category at all
// claims an initialized platform, which is a real change from "genuinely
// absent" — an absent platform cannot answer a demographic question. The user
// asked for it and declared themselves over 18 (2026-08-12), which is the only
// place that answer can legitimately come from here: a self-declaration by the
// person running their own copy, not a discovered fact. THE DRM LINE DOES NOT
// MOVE WITH IT — IAP, purchase records, asset details and every delivery call
// still abort unconditionally, and check_drm_guard still asserts both
// directions. What changes is that the application's own entitlement, already
// answered yes, is answered through the channel the guest actually reads.
//
// The three completions are all this queues, each issued only in response to
// the guest's own request:
//
//   ovr_UnityInitWrapperAsynchronous  -> Platform_InitializeAndroidAsynchronous,
//                                        result Success
//   ovr_Entitlement_GetIsViewerEntitled -> Entitlement_GetIsViewerEntitled, no
//                                        payload; success IS the answer
//   ovr_UserAgeCategory_Get           -> UserAgeCategory_Get, category Ad(ult)
//
// The type numbers are the guest's own, read out of the running IL2CPP runtime
// with KL_PROBE_ENUM (kl_mprobe.c) rather than from an SDK header. Necessary
// rather than tidy: Message.ParseMessageHandle switches on the type and its
// default arm logs "Unrecognized message type" and produces NO message, so a
// guessed number is indistinguishable from the silence above.
#define KLPLAT_MSG_PLATFORM_INIT   450037684u   // Message.MessageType
#define KLPLAT_MSG_ENTITLEMENT     409688241u   //   .Platform_InitializeAndroidAsynchronous
#define KLPLAT_MSG_AGE_CATEGORY    567009472u   //   .Entitlement_GetIsViewerEntitled
#define KLPLAT_MSG_APP_VERSION    1751583246u   //   .UserAgeCategory_Get
                                                //   .Application_GetVersion
#define KLPLAT_MSG_CLOUD_DIR      1990471406u   //   .CloudStorage2_GetUserDirectoryPath
#define KLPLAT_INIT_SUCCESS        0            // PlatformInitializeResult.Success
#define KLPLAT_AGE_ADULT           3            // AccountAgeCategory.Ad

// A malloc'd list, because two of the message shapes own variable-sized
// payloads (the DLC enumeration's asset array, the cloud directory's string)
// and ovr_FreeMessage is where that ownership ends. The mutex is not defensive
// — the requests are issued from managed code on the main thread and the pump
// runs there too, but nothing in the API promises that.
//
// `str` is the STRING payload, for the messages whose managed wrapper is a
// `Message<string>` — it is read through ovr_Message_GetString rather than
// through the int payload above, and a message carries one or the other.
// `items`/`count` are the DLC enumeration's (see the override below); both are
// NULL/0 for every other shape. Everything pointed to is malloc'd and freed by
// ovr_FreeMessage.
typedef struct {
    uint64_t  asset_id;
    char     *filepath;                    // absolute, malloc'd
    // Whether the file is REALLY THERE. GetList only ever builds entries for
    // files it found, so this is 1 for all of them; the per-file status queries
    // (ovr_AssetFile_StatusByName / _StatusById) can be asked about a name that
    // is absent, and that answer has to be "not installed, not entitled"
    // rather than a fabricated one. See klplat_status_msg.
    int       installed;
} klplat_asset;

typedef struct klplat_msg {
    uint32_t            type;
    uint64_t            request_id;
    int32_t             payload;
    size_t              count;
    klplat_asset       *items;
    char               *str;               // ovr_Message_GetString payload, malloc'd
    struct klplat_msg  *next;
} klplat_msg;
static klplat_msg     *g_msg_head, *g_msg_tail;
static uint64_t        g_next_request = 1;
static int             g_platform_up;
static pthread_mutex_t g_msg_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        klplat_enqueue(klplat_msg *m);

// Issue a request and queue its completion. Returns the ovrRequest the guest
// will wait on — non-zero, and matched by the message, which together are the
// whole point.
static uint64_t klplat_request_str(const char *what, uint32_t type,
                                   int32_t payload, const char *str) {
    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type = type;
    m->payload = payload;
    if (str) m->str = strdup(str);
    uint64_t id = klplat_enqueue(m);
    fprintf(stderr, "  [plat] %s -> request %llu, completion queued "
                    "(type 0x%08x, payload %d%s%s)\n",
            what, (unsigned long long)id, type, payload,
            str ? ", string " : "", str ? str : "");
    return id;
}

// ...and the int-payload spelling, which is all but one caller.
static uint64_t klplat_request(const char *what, uint32_t type, int32_t payload) {
    return klplat_request_str(what, type, payload, NULL);
}


// The accessors the SDK's Message base class and the two payload wrappers call
// on that handle. Real functions rather than named trampolines, because these
// take the handle in x0 and a trampoline would put the entry point's NAME there.
//
// ovr_Message_GetError is deliberately absent: nothing here is ever an error, so
// the guest never asks, and the fail-closed default would name it if that
// changed. Same for every other Message accessor — a message shape we do not
// produce should stop the run by name rather than answer.
static uint64_t klplat_Message_GetType(const klplat_msg *m) {
    plat_hit("ovr_Message_GetType");
    return m ? m->type : 0;
}
static uint64_t klplat_Message_IsError(const klplat_msg *m) {
    plat_hit("ovr_Message_IsError");
    (void)m;
    return 0;
}
static uint64_t klplat_Message_GetRequestID(const klplat_msg *m) {
    plat_hit("ovr_Message_GetRequestID");
    return m ? m->request_id : 0;
}
// The payload handle IS the message: there is one payload per message here, so
// a separate allocation would only be a second thing to keep alive.
static uint64_t klplat_Message_GetPayload(const klplat_msg *m) {
    plat_hit("ovr_Message_GetPayload");
    return (uint64_t)(uintptr_t)m;
}
static uint64_t klplat_payload_int(const klplat_msg *m) {
    plat_hit("ovr_Payload_GetInt");
    return m ? (uint64_t)(uint32_t)m->payload : 0;
}
// ...and the string one. `Message<string>` reads its data with this rather than
// through the payload handle, so a message whose managed wrapper is a string
// carries `str` and answers here. NULL for every other message, which is what
// the SDK expects for one that has no string.
static const char *klplat_Message_GetString(const klplat_msg *m) {
    plat_hit("ovr_Message_GetString");
    return m ? m->str : NULL;
}

// ---------------------------------------------------------------------------
// What we do implement: the platform reporting its own absence, honestly.
//
// ovr_IsPlatformInitialized answers false, because it is not initialised and
// never will be. That is the same answer a real headset would give before init,
// so the guest's own "platform unavailable" path is a path it already has — its
// metadata carries "Oculus Platform failed to initialize." and
// "Initialize Error: Oculus platform failed to initialize due to exception."
// ...and it answers TRUE once an init request has been completed, because the
// SDK gates every other request on it — `Entitlements.IsUserEntitledToApplication`
// and `Users.GetUserAgeCategory` both begin `if (Core.IsInitialized())`, and
// Core.IsInitialized() IS this call. Answering false there is not a cautious
// answer, it is a guarantee that no request is ever made and so no completion
// is ever awaited-and-delivered; the two halves have to agree.
static uint64_t klplat_IsPlatformInitialized(void) {
    plat_hit("ovr_IsPlatformInitialized");
    return (uint64_t)g_platform_up;
}

// ...and the SYNCHRONOUS Android wrapper, which is the same decision reached
// through a different door — and a different RETURN TYPE, which is what made it
// worth measuring rather than grouping.
//
// `Oculus.Platform.Core.Initialize(appId)` calls this one and THROWS when it is
// false: `UnityException("Oculus Platform failed to initialize.")`. It is not an
// ovrRequest and not a PlatformInitializeResult — the real export builds a byte
// (`and w8, w8, #1; strb w8, [sp, ...]`), i.e. a plain bool, checked in both
// this tree's loaders (superhot +0x51934, beatsaber +0x8aae4). Grouped with the
// request-returning init family it would answer 0, and 0 in a bool is false.
//
// Why it answers TRUE, given the file's standing objection to inventing success:
// that objection is about a request id nothing completes, and it no longer
// applies. This branch has a message queue, the async init already answers
// success and sets g_platform_up, and the app's own entitlement completes
// through it. Answering false HERE while answering success THERE is the two
// halves disagreeing — and the cost of the disagreement is not an error path,
// it is the whole title: a guest running the stock
// `Oculus.Platform.Samples.EntitlementCheck` behaviour catches the throw, runs
// its failure handler, and calls `UnityEngine.Application.Quit()`. Unity's quit
// gate then makes `nativeRender` a no-op for the rest of the process's life, so
// what a person sees is not an error message but a frozen frame, reprojected
// forever, with every counter healthy. SUPERHOT VR is such a guest.
//
// **The DRM line does not move with this.** Asking to connect to the platform is
// not an ownership question (that is why this family was never among the
// refusals); IAP, purchase records, asset details and every delivery call still
// abort unconditionally and check_drm_guard still asserts both directions.
// What init being answered buys is that the app's own licence check — the one
// carve-out this project deliberately made — becomes REACHABLE. A guest that
// quits before it can ask is not a guest being refused; it is one that never got
// to the question.
//
// Deliberately narrow: only this name. The Asynchronous / Standalone spellings
// have their own answers above, and the `ovr_PlatformInitialize*` family returns
// an `ovrPlatformInitializeResult` (a THIRD convention, whose Success is 0).
static uint64_t klplat_UnityInitWrapper(const char *app_id) {
    plat_hit("ovr_UnityInitWrapper");
    g_platform_up = 1;
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_UnityInitWrapper(\"%s\") -> true (the "
                        "synthetic platform reports it came up, so the app's own "
                        "entitlement check can be asked and answered; every "
                        "ownership query still refuses)\n", app_id ? app_id : "");
    }
    return 1;
}

// The app's own licence check — see the carve-out comment above for why this
// one is answered and the rest of the family is not: the user unpacked their
// own APK, so a licence to the application itself is something we can assert,
// while DLC needs out-of-band asset data and a licence we cannot verify (the
// branch's GetList override asserts it from file presence instead, on the
// user's own call). Non-zero is load-bearing rather than incidental — 0 here
// is the guest's licence-FAILURE path, not a neutral answer — so m_boot's
// check_drm_guard asserts the value, in both directions along with the
// refusals it sits beside.
// It is a REQUEST, not a predicate — `Entitlements.IsUserEntitledToApplication()`
// wraps its return in a `Request` and awaits the completion, so the old bare 1
// was request id 1 with nothing ever arriving for it. The answer is unchanged
// and so is the reasoning above; it is now delivered where the guest reads it,
// as a non-error completion, which is how this API spells "entitled".
static uint64_t klplat_Entitlement_GetIsViewerEntitled(void) {
    plat_hit("ovr_Entitlement_GetIsViewerEntitled");
    return klplat_request("ovr_Entitlement_GetIsViewerEntitled",
                          KLPLAT_MSG_ENTITLEMENT, 0);
}

// Where the user's saves live — and the one member of the CloudStorage family
// that is answered rather than refused.
//
// The rest of that family stays refused, and the split is the same one the DRM
// line is drawn along: `ovr_CloudStorage_Load` / `_Save` move DATA to and from a
// service that is not here, so 0 ("the request could not be made") is the truth
// about them. This one moves no data at all. It asks where on the local
// filesystem the save directory IS — the directory the platform would later
// synchronise — and that is a question this host can answer completely.
//
// Refusing it is what a black launch screen looks like. SUPERHOT's
// `VRSaveManager::LoadAsync` chains its whole load off this request, so a 0
// leaves the SDK waiting on a completion it will never post: no save data, no
// scene, and the launch precache's own `ClearRenderTarget` command buffer parked
// on the camera in the meantime — which renders as black, forever, with every
// frame counter healthy. "A game that cannot reach the cloud writes locally" is
// only true if something tells it where local is.
//
// It is a REQUEST returning `Request<string>` (the SDK's `MessageWithString`),
// so the path arrives as a message payload read through ovr_Message_GetString —
// not as a return value. The type number is the guest's own, read out of the
// running IL2CPP runtime with KL_PROBE_ENUM like the three above it.
//
// The directory is created, and that is not a convenience: the guest calls its
// own IsDirectoryWritable on the answer before it uses it, and a path that does
// not exist fails that test — which is the same stall wearing a different hat.
// It sits beside the guest's files directory, so it is per-target and follows
// KL_FILES_DIR like everything else the guest writes.

// The age category, and the ONE answer in this file that is a declaration
// rather than a measurement. See the completion-queue comment above: the user
// declared themselves over 18 and this reports it; nothing here discovered it.
// `Ad` is Oculus.Platform.AccountAgeCategory's own spelling, read out of the
// guest, and Beat Saber maps it to its `UserAgeCategory.Adult`.
static uint64_t klplat_UserAgeCategory_Get(void) {
    plat_hit("ovr_UserAgeCategory_Get");
    return klplat_request("ovr_UserAgeCategory_Get",
                          KLPLAT_MSG_AGE_CATEGORY, KLPLAT_AGE_ADULT);
}

// The application's own version, which is the one question in this family we can
// answer from something we actually have: the unpacked tree's apktool.yml, i.e.
// exactly what PackageManager already tells the guest. `OculusInit`'s
// GetAppVersionQuestAsync asks it during startup, behind the init completion.
//
// LATEST is answered as CURRENT, and that is a statement rather than a dodge:
// there is no store here to ask what the newest build is, and "the installed
// one is the newest one" is the only self-consistent thing to say — it means
// "no update available", which is true of this host in the only sense available
// to it. Reporting a HIGHER latest would advertise an update that cannot be
// fetched, which is what drives Application_StartAppDownload.
static uint64_t klplat_Application_GetVersion(void) {
    plat_hit("ovr_Application_GetVersion");
    return klplat_request("ovr_Application_GetVersion", KLPLAT_MSG_APP_VERSION, 0);
}

static uint64_t klplat_AppVersion_code(const void *h) {
    plat_hit("ovr_ApplicationVersion_GetCode");
    (void)h;
    long code = 0; kl_jni_guest_version(&code, NULL);
    return (uint64_t)(int64_t)code;
}
// The two fields of the same model we cannot answer from anything here. A
// release date and a download size are properties of a STORE listing, and there
// is no store; 0 is the SDK's own "not published" rather than a stand-in, and
// the model reads every field at construction whether or not it uses them.
static uint64_t klplat_AppVersion_unknown(const void *h) {
    plat_hit("ovr_ApplicationVersion_unknown_field");
    (void)h;
    return 0;
}

static const char *klplat_AppVersion_name(const void *h) {
    plat_hit("ovr_ApplicationVersion_GetName");
    (void)h;
    const char *name = NULL; kl_jni_guest_version(NULL, &name);
    return name ? name : "";
}

// The ASYNCHRONOUS init. Its completion is what everything else waits behind,
// so this is also where the platform becomes "up" for ovr_IsPlatformInitialized.
// Only the async spellings come here: the synchronous ones return a
// PlatformInitializeResult directly and keep their old answer.
static uint64_t klplat_init_async(const char *name) {
    plat_hit(name);
    g_platform_up = 1;
    return klplat_request(name, KLPLAT_MSG_PLATFORM_INIT, KLPLAT_INIT_SUCCESS);
}

static const char *const g_plat_init_async[] = {
    "ovr_UnityInitWrapperAsynchronous",
    "ovr_PlatformInitializeAndroidAsynchronous",
    "ovr_PlatformInitializeAndroidAsynchronousWithOptions",
    "ovr_PlatformInitializeWindowsAsynchronous",
};

static int plat_is_init_async(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_init_async / sizeof g_plat_init_async[0]; i++)
        if (strcmp(g_plat_init_async[i], name) == 0) return 1;
    return 0;
}


// Unity's native plugin interface. Every entry point in it returns void, and the
// real platform loader uses them only to keep hold of the IUnityInterfaces
// registry and the render event queue — there is nothing here to bind either to.
// Taken as a group because it is one fixed interface Unity calls on every plugin,
// and the same group is already answered this way in kl_ovrp.c.
static uint64_t klplat_void(const char *name) {
    plat_hit(name);
    return 0;
}

static const char *const g_plat_unity[] = {
    "UnityPluginLoad", "UnityPluginUnload", "UnitySetEventQueue",
    "UnitySetGraphicsDevice", "UnityShaderCompilerExtEvent", "UnityRenderEvent",
    // The rendering-extension pair. UnityRenderingExtEvent is void; the Query
    // returns int, and 0 is the truthful answer — this plugin wants no
    // render-thread extension events, so Unity will never issue them.
    "UnityRenderingExtEvent", "UnityRenderingExtQuery",
    // Audio-plugin enumeration, dlsym'd speculatively by libunity's
    // AudioPluginManager on every plugin handle. A COUNT of effect definitions,
    // so 0 is "none here" — which is what the real loader says by not exporting
    // it. Same answer, same reasoning, as kl_ovrp.c's copy.
    "UnityGetAudioEffectDefinitions",
    // ovr_FreeMessage is NOT here: it has real work to do (the enumeration
    // messages are heap-allocated), so it routes to a real free above.
};

static int plat_is_unity_hook(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_unity / sizeof g_plat_unity[0]; i++)
        if (strcmp(g_plat_unity[i], name) == 0) return 1;
    return 0;
}

// Platform initialisation. Note this is *not* one of the refused calls: asking to
// connect to the platform is not an ownership question, and it has a truthful
// answer here — it fails, because there is no com.oculus.horizon to connect to.
//
// An ovrRequest of 0 is the SDK's "the request could not be made". That is the
// honest report, and it lands the guest on an error path it already has: its own
// metadata carries "Oculus Platform failed to initialize." and "Initialize Error:
// Oculus platform failed to initialize due to exception."
//
// Returning a plausible request id instead would be a lie with a tail: the caller
// would then poll ovr_PopMessage forever for a completion that cannot come.
// Said once PER NAME rather than once per run: this is the answer for a couple
// of dozen entry points now, and boot #3's log had exactly one of these lines in
// it — the first refusal silenced every later one, which is the opposite of what
// a work-list log is for. One line each, then quiet.
static uint64_t klplat_init_fails(const char *name) {
    plat_hit(name);
    int s = plat_slot(name);
    if (s < 0 || g_plat[s].calls == 1)
        fprintf(stderr, "  [plat] %s -> 0 (no Oculus platform service on this host; "
                        "reporting failure rather than inventing success)\n", name);
    return 0;
}

// ...and the ordinary request calls, which share the return value (0) but NOT
// the claim. klplat_init_fails answers for the ovr_PlatformInitialize* family,
// where 0 is Success and a plat_init target has to report the platform up. A
// leaderboard fetch or a rich-presence set is not an init call: its 0 means
// "the request could not be made", it owes no completion, and it must never be
// the thing that declares the platform up — which it silently was, for any
// request that happened to be asked before the init call on a plat_init target.
//
// Same distinction klplat_drm_request draws for the ownership families' request
// half, for the same reason: the platform's state is not what is in question
// here, the request's is. One line per NAME rather than one for the whole set,
// because the next round reads this log to find out which call came last.
//
// (Ported from the pinballfx branch, d5d0172, unchanged.)
static uint64_t klplat_request_fails(const char *name) {
    plat_hit(name);
    int s = plat_slot(name);
    if (s < 0 || g_plat[s].calls == 1)
        fprintf(stderr, "  [plat] %s -> 0 (request NOT made: no Oculus service on "
                        "this host. Nothing is reported and no completion is "
                        "queued, so the accessors over it are unreachable)\n", name);
    return 0;
}

// ovr_UnityInitWrapper is NOT here — it is a bool, not an ovrRequest, and it has
// its own implementation above. It is the only one of these whose contract can
// report success without owing anybody a completion message.
static const char *const g_plat_init[] = {
    "ovr_UnityInitWrapperAsynchronous",
    "ovr_UnityInitWrapperStandalone", "ovr_UnityInitGlobals",
    "ovr_PlatformInitializeAndroid", "ovr_PlatformInitializeAndroidAsynchronous",
    "ovr_PlatformInitializeAndroidAsynchronousWithOptions",
    "ovr_PlatformInitializeWithAccessToken", "ovr_PlatformInitializeStandaloneOculus",
};

static int plat_is_init(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_init / sizeof g_plat_init[0]; i++)
        if (strcmp(g_plat_init[i], name) == 0) return 1;
    return 0;
}

// Request-returning calls with the same truthful answer as init: 0, "the
// request could not be made", because there is no platform service to make it
// of. Answering with a fabricated request id would have the caller polling
// ovr_PopMessage for a completion that cannot come.
static const char *const g_plat_request[] = {
    "ovr_RichPresence_Clear", "ovr_RichPresence_Set",
    "ovr_RichPresence_SetDestination", "ovr_RichPresence_SetIsJoinable",
    // Asking who is logged in is not an ownership question; with no platform
    // service the request cannot be made, which is what 0 says. Beat Saber
    // 1.28 takes its offline path from there (~swap 34k of the boot).
    "ovr_User_GetLoggedInUser",

    // ---- the rest of the ovr_User_* REQUEST half (batman, boot #3) ----
    //
    // Arkham Shadow's OculusCorePlatformProvider.InitPlatform() walks this
    // family during startup and boot #3 died on the first member of it:
    //
    //   [plat] ovr_Achievements_GetAllDefinitions -> 0 (...)
    //   E/Unity: An unknown error occurred. Request failed.       <- handled
    //   ...
    //   [klepton] fatal: ... unimplemented ... 'ovr_User_GetAccessToken'
    //
    // Those two lines are the whole argument. `ovr_Achievements_*` already
    // answers 0 here, and the guest's own SDK turned that into a completed,
    // FAILED request client-side and carried on for another dozen log lines —
    // which is exactly the outcome an "error completion" is supposed to buy.
    // The Unity SDK special-cases an ovrRequest of 0 in Callback.OnComplete and
    // fires the callback immediately with a synthesized error ("An unknown
    // error occurred. Request failed." is its own string, and it is in this
    // guest's global-metadata.dat); nothing is left outstanding and nothing
    // waits on the pump. See the note below g_plat_request_prefix for why the
    // Unreal branches (pinballfx, deadpool) need a QUEUED error message instead
    // and why queueing one here would be a regression.
    //
    // NOT a blanket ovr_User_ prefix: this family is the mixed one the comment
    // below calls out — ovr_User_GetID / _GetOculusID / _GetDisplayName and the
    // rest of the accessors read fields off a User handle and a blanket 0 would
    // be a wrong ANSWER rather than a refused request. Every name here was
    // cross-checked against the guest's OWN Message.MessageType enum in
    // global-metadata.dat (the same table that yielded the type constants
    // above): a name is a request exactly when the enum has a member for it,
    // and all ten below do. Refusing them reports what a headset with no
    // network and no signed-in session reports; none of them is an ownership
    // question (IAP / Purchase / Entitlement / AssetFile still refuse through
    // plat_is_drm, which runs first), and an access token we do not have is
    // reported as absent rather than invented.
    "ovr_User_Get",
    "ovr_User_GetAccessToken",
    "ovr_User_GetBlockedUsers",
    "ovr_User_GetLinkedAccounts",
    "ovr_User_GetLoggedInUserFriends",
    "ovr_User_GetOrgScopedID",
    "ovr_User_GetSdkAccounts",
    "ovr_User_GetUserCapabilities",
    "ovr_User_GetUserProof",

    // ---- the HTTP fetch half ----
    //
    // The SDK's own transport, not a game feature: `ovr_HTTP_GetWithMessageType`
    // is how every DeserializableList<T> fetches its next page, and HTTP_Get /
    // GetToFile / Post / MultiPartPost are the plain fetches behind avatar
    // images and the like. All five have a MessageType member in this guest's
    // enum, so all five are requests. There is no service and no session to
    // fetch against, so 0 — "the request could not be made" — is the literal
    // truth, and it lands on the same handled-failure path as the achievements
    // line above.
    //
    // The STREAMING trio (ovr_HTTP_StartTransfer / _Write / _WriteEOM) is
    // deliberately absent: it is a different shape (a transfer id plus chunked
    // writes, delivered through ovr_HttpTransferUpdate_*), it has no MessageType
    // member, and nothing has asked for it. It keeps the fail-closed default.
    "ovr_HTTP_Get",
    "ovr_HTTP_GetToFile",
    "ovr_HTTP_GetWithMessageType",
    "ovr_HTTP_MultiPartPost",
    "ovr_HTTP_Post",

    // ---- the rest of the surface, derived rather than discovered (pttr) ----
    //
    // Paint the Town Red VR died on ovr_Leaderboard_GetEntries after a boot that
    // had already composited a frame, and it was the fourth launch in a row lost
    // to "the next name nobody implemented". The surface does not run dry that
    // way: this guest's libil2cpp.so carries the SDK's ENTIRE C binding — 700
    // distinct ovr_* entry points in its rodata — because Unity compiles CApi.cs
    // whether the title calls it or not.
    //
    // So the split was derived in one pass instead of one launch at a time, by
    // the test this file already established and trusts: **a name is a request
    // exactly when the guest's own ovrMessageType enum has a member for it.**
    // The enum members live in global-metadata.dat's identifier heap, which
    // work/pttr-jni-census.md already parses; intersecting the 700 rodata names
    // with the 153,246 identifiers yields 167 requests, and the residue is
    // accessors, options builders and commands. It reproduces the nine
    // ovr_Leaderboard_* the pinballfx branch worked out by hand from the
    // loader's thunks (d5d0172) name for name, which is the check that the test
    // is measuring the right thing.
    //
    // Listed BY NAME, never by prefix, for every family below — the mixed ones
    // are exactly the families the prefix table's comment warned about, and
    // ovr_Challenge_GetID or ovr_Party_GetID answering "0" would be a wrong
    // ANSWER rather than a refused request. Names already covered elsewhere are
    // absent by construction: the DRM families (plat_is_drm runs first), the six
    // request PREFIXES below, the ten ovr_User_* above, the five ovr_HTTP_*, and
    // everything in g_plat_impl / g_plat_init.
    //
    // None of these is an ownership question. Leaderboards and challenges record
    // what a player SCORED, parties and NetSync are who they are playing with,
    // Livestreaming and Media are what they broadcast, and the report/block
    // flows are moderation. The entitlement line does not move with any of it.
    "ovr_Leaderboard_Get", "ovr_Leaderboard_GetEntries",
    "ovr_Leaderboard_GetEntriesAfterRank", "ovr_Leaderboard_GetEntriesByIds",
    "ovr_Leaderboard_GetNextEntries", "ovr_Leaderboard_GetPreviousEntries",
    "ovr_Leaderboard_WriteEntry",
    "ovr_Leaderboard_WriteEntryWithSupplementaryMetric",
    "ovr_Challenges_Create", "ovr_Challenges_DeclineInvite",
    "ovr_Challenges_Delete", "ovr_Challenges_Get", "ovr_Challenges_GetEntries",
    "ovr_Challenges_GetEntriesAfterRank", "ovr_Challenges_GetEntriesByIds",
    "ovr_Challenges_GetList", "ovr_Challenges_GetNextChallenges",
    "ovr_Challenges_GetNextEntries", "ovr_Challenges_GetPreviousChallenges",
    "ovr_Challenges_GetPreviousEntries", "ovr_Challenges_Join",
    "ovr_Challenges_Leave", "ovr_Challenges_UpdateInfo",
    "ovr_Party_Create", "ovr_Party_GatherInApplication", "ovr_Party_Get",
    "ovr_Party_GetCurrent", "ovr_Party_GetCurrentForUser", "ovr_Party_Invite",
    "ovr_Party_Join", "ovr_Party_Leave",
    "ovr_NetSync_Connect", "ovr_NetSync_Disconnect", "ovr_NetSync_GetSessions",
    "ovr_NetSync_GetVoipAttenuation", "ovr_NetSync_GetVoipAttenuationDefault",
    "ovr_NetSync_SetVoipAttenuation", "ovr_NetSync_SetVoipAttenuationModel",
    "ovr_NetSync_SetVoipChannelCfg", "ovr_NetSync_SetVoipGroup",
    "ovr_NetSync_SetVoipListentoChannels", "ovr_NetSync_SetVoipMicSource",
    "ovr_NetSync_SetVoipSessionMuted", "ovr_NetSync_SetVoipSpeaktoChannels",
    "ovr_NetSync_SetVoipStreamMode",
    "ovr_Voip_GetMicrophoneAvailability", "ovr_Voip_ReportAppVoipSessions",
    "ovr_Voip_SetSystemVoipSuppressed",
    "ovr_RichPresence_GetDestinations", "ovr_RichPresence_SetLobbySession",
    "ovr_RichPresence_SetMatchSession",
    "ovr_Livestreaming_IsAllowedForApplication",
    "ovr_Livestreaming_StartPartyStream", "ovr_Livestreaming_StartStream",
    "ovr_Livestreaming_StopPartyStream", "ovr_Livestreaming_StopStream",
    "ovr_Livestreaming_UpdateMicStatus",
    "ovr_Application_CancelAppDownload",
    "ovr_Application_CheckAppDownloadProgress",
    "ovr_Application_GetInstalledApplications",
    "ovr_Application_InstallAppUpdateAndRelaunch",
    "ovr_Application_LaunchOtherApp", "ovr_Application_StartAppDownload",
    "ovr_ApplicationLifecycle_GetRegisteredPIDs",
    "ovr_ApplicationLifecycle_GetSessionKey",
    "ovr_ApplicationLifecycle_RegisterSessionKey",
    "ovr_User_CancelRecordingForReportFlow", "ovr_User_LaunchBlockFlow",
    "ovr_User_LaunchFriendRequestFlow", "ovr_User_LaunchReportFlow",
    "ovr_User_LaunchReportFlow2", "ovr_User_LaunchUnblockFlow",
    "ovr_User_NewEntitledTestUser", "ovr_User_NewTestUser",
    "ovr_User_NewTestUserFriends", "ovr_User_StartRecordingForReportFlow",
    "ovr_User_StopRecordingAndLaunchReportFlow",
    "ovr_User_StopRecordingAndLaunchReportFlow2",
    "ovr_User_TestUserCreateDeviceManifest",
    "ovr_UserAgeCategory_Report",
    "ovr_Colocation_GetCurrentMapUuid", "ovr_Colocation_RequestMap",
    "ovr_Colocation_ShareMap",
    "ovr_Avatar_LaunchAvatarEditor", "ovr_Avatar_UpdateMetaData",
    "ovr_AbuseReport_LaunchAdvancedReportFlow",
    "ovr_AbuseReport_ReportRequestHandled",
    "ovr_Notification_MarkAsRead",
    "ovr_Media_ShareToFacebook",
    "ovr_GraphAPI_Get", "ovr_GraphAPI_Post",
    "ovr_DeviceApplicationIntegrity_GetAttestationToken",
    "ovr_DeviceApplicationIntegrity_GetIntegrityToken",
};

// Whole families where EVERY entry point is request-returning, so the same
// answer is right for all of them and an exact list would only stop the run
// once per name for nothing.
//
//   ovr_Achievements_* — progress and definitions for achievements. All nine
//     calls (AddCount, AddFields, Unlock, GetAllDefinitions, GetAllProgress,
//     GetDefinitionsByName, GetProgressByName and the two ArrayPage walkers)
//     return an ovrRequest. This is not an ownership question: achievements
//     record what the player has done, not what they have bought, and 0 says
//     the request could not be made rather than reporting any progress. The
//     entitlement families are unaffected — plat_is_drm runs first.
//   ovr_GroupPresence_* — "who are you playing with", the multiplayer presence
//     the invite/roster panels read. Ownership does not enter into it, and the
//     trailing underscore keeps this clear of ovr_GroupPresenceOptions_* below,
//     which is a different shape entirely.
//   ovr_CloudStorage_* / ovr_CloudStorage2_* — cloud saves. The user's own save
//     data, not a thing they bought. 0 is right for the transfer calls: with no
//     cloud there is nothing to load from or save to. It was NOT right for
//     ovr_CloudStorage2_GetUserDirectoryPath, which is carved out above and
//     answered — see the comment there for the launch it deadlocked. "A game
//     that cannot reach the cloud writes locally" is only true if something
//     tells it where local IS.
//   ovr_LanguagePack_* — which language pack is current. (The _GetCurrent /
//     _SetCurrent pair only; ovr_LanguagePackInfo_* is an accessor family and is
//     answered separately below.)
//   ovr_UserDataStore_* — the per-user key/value store a title keeps progress
//     in. Again the user's own data. Note the trailing underscore keeps this
//     clear of ovr_UserDataStoreUpdateResponse_*, which is an accessor.
//
// Membership here is not a judgement call: for every name under these prefixes
// the game's own metadata carries a matching ovrMessageType, which is what
// "returns an ovrRequest and completes through a message" means. Cross-checked
// name by name against the MessageType enum in global-metadata.dat (the same
// table that yielded ovrMessageType_Entitlement_GetIsViewerEntitled) —
// Achievements 7/7, CloudStorage 9/9, CloudStorage2 1/1, LanguagePack 2/2,
// UserDataStore 8/8. The mixed families (User_, Room_, Leaderboard_, Voip_,
// Party_, Livestreaming_, Application_) are deliberately absent: they mix
// requests with plain accessors, and a blanket 0 would be a wrong ANSWER rather
// than a refused request.
static const char *const g_plat_request_prefix[] = {
    "ovr_Achievements_", "ovr_GroupPresence_",
    "ovr_CloudStorage_", "ovr_CloudStorage2_",
    "ovr_LanguagePack_", "ovr_UserDataStore_",
};

// ---------------------------------------------------------------------------
// Why this branch does NOT queue an ERROR message for a refused request
//
// The pinballfx branch answers a refused request with a real ovrRequest id and
// a QUEUED error completion (klplat_request_error, c38bfe8/e2351d0), because
// Unreal's FOnlineSubsystemOculus registers a completion delegate under the id
// without checking it against 0 and retires it only when a message carrying
// that id is popped — so a 0 there is a permanent leak, and Zen Studios' Exit
// Table teardown spins forever on the outstanding count. That is a real bug and
// that is a real fix, for a GUEST THAT BEHAVES THAT WAY.
//
// This guest does not. Unity's Oculus SDK special-cases request id 0 in
// Callback.OnComplete and completes the request immediately, client-side, with
// a synthesized error — measured in boot #3 above, where the achievements
// request answered 0 and the very next line was the SDK's own "An unknown error
// occurred. Request failed." with the game carrying on. Nothing is left
// outstanding, so there is nothing for a queued message to retire.
//
// And queueing one would be actively worse here, which is the half worth
// writing down. pinballfx's error message carries type 0 (ovrMessage_Unknown),
// deliberately, because UE dispatches on the request id and never looks at the
// type — and that file's own comment flags the Unity case as needing a revisit
// before it ships. This is that revisit. Unity's Messages.ParseMessageHandle
// switches on ovr_Message_GetType FIRST and builds a typed Message<T> from it;
// its default arm logs "Unrecognized message type" (that string is in this
// guest's global-metadata.dat) and returns null, and Request.RunCallbacks stops
// pumping the moment PopMessage hands it a null. So a type-0 error message
// would not complete the request — it would stall the whole message pump for
// that frame and strand every LATER completion behind it, including the init
// and entitlement ones this file works hard to deliver.
//
// Queueing a CORRECTLY TYPED error would work, but it needs the guest's own
// ovrMessageType constant for each refused call, read out with KL_PROBE_ENUM
// like the five above — a per-name cost with no observed symptom to pay it
// for. If a Unity guest is ever seen waiting on a request that answered 0, that
// is the moment to add klplat_request_error here with a real type, and not
// before.

// ---------------------------------------------------------------------------
// Options objects: ovr_<Thing>Options_Create / _Set<Field> / _Destroy.
//
// Not requests and not queries — a local builder the caller fills in and then
// hands to a request call. Create returns an opaque handle, so 0 is the one
// answer that is NOT safe here: the guest would carry a NULL through to the
// setters, and on a real platform that is a null deref inside the library.
//
// We hand back a non-NULL cookie instead. Nothing reads it: the setters are
// no-ops because the request the options would feed cannot be made anyway
// (ovr_GroupPresence_Set and friends answer 0 above), so there is no state
// worth keeping and one shared cookie serves every live options object. That
// is invented behaviour in the narrow sense, but it invents no ANSWER — the
// guest learns nothing from it that it did not itself put in.
//
// Entitlement spellings never reach here: plat_is_drm runs first, so
// ovr_IAPOptions_* and the rest still refuse.
// Outbound-only calls: the guest hands us something and asks nothing back.
// Accepted and dropped, which is not a stub standing in for an answer — there
// is no answer involved. Same treatment as the haptics commands in kl_ovrp.c.
//
//   ovr_Log_NewEvent(name, ovrKeyValuePair *, count) — analytics. Telemetry
//     with no service behind it goes nowhere, and a headset that is not
//     attached to an Oculus account has nowhere to send it anyway.
//   ovr_Voip_SetMicrophoneMuted(ovrVoipMuteState) — the local mic gate for
//     in-game voice chat. RE4's OculusPlatform module pushes it during startup.
//     Nothing here captures audio at all — kl_aaudio refuses capture streams by
//     design and no microphone is presented — so there is no stream to mute and
//     nothing the guest can learn from the call. It is deliberately NOT filed
//     with the enumeration-answers-none group: those state an absence, and this
//     one is a command with no reply.
//   The rest of the outbound-only surface, added whole for pttr rather than one
//   launch at a time (see the derived-request note above for the method). Every
//   name here is a COMMAND with no reply: the guest hands us a setting, a
//   position, a callback or a buffer to free, and asks nothing back. There is no
//   voice chat here to configure, no NetSync session to place a listener in, no
//   packet we allocated to free, and no test platform to reset.
//
//   ovr_CrashApplication is in this group deliberately even though the enum has
//   a member for it: it is the SDK's deliberate-crash debug hook, and the one
//   thing it must NOT do here is what its name says.
static const char *const g_plat_drop[] = {
    "ovr_Log_NewEvent",
    "ovr_Voip_SetMicrophoneMuted",
    "ovr_Voip_Start", "ovr_Voip_Stop", "ovr_Voip_Accept",
    "ovr_Voip_SetOutputSampleRate", "ovr_Voip_SetNewConnectionOptions",
    "ovr_Voip_SetSystemVoipMicrophoneMuted", "ovr_Voip_SetSystemVoipPassthrough",
    "ovr_Voip_SetMicrophoneFilterCallback",
    "ovr_Voip_SetMicrophoneFilterCallbackWithFixedSizeBuffer",
    "ovr_Voip_DestroyEncoder", "ovr_Voip_DestroyDecoder",
    "ovr_Microphone_Destroy", "ovr_Microphone_Start", "ovr_Microphone_Stop",
    "ovr_Microphone_SetAcceptableRecordingDelayHint",
    "ovr_NetSync_SetListenerPosition", "ovr_Packet_Free",
    "ovr_ApplicationLifecycle_LogDeeplinkResult",
    "ovr_SetDeveloperAccessToken", "ovr_UnityResetTestPlatform",
    "ovr_CrashApplication",
};

static int plat_is_drop(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_drop / sizeof g_plat_drop[0]; i++)
        if (strcmp(g_plat_drop[i], name) == 0) return 1;
    return 0;
}

static const char g_options_cookie[] = "klepton-ovr-options";

static uint64_t klplat_options_create(const char *name) {
    plat_hit(name);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] %s -> opaque cookie (options objects are local "
                        "state; the request they feed cannot be made)\n", name);
    }
    return (uint64_t)(uintptr_t)g_options_cookie;
}

static int plat_is_options_create(const char *name) {
    return strstr(name, "Options_Create") != NULL;
}

// ---------------------------------------------------------------------------
// Launch details: HOW the app was started.
//
// Not a request — a plain accessor returning an opaque handle that the managed
// SDK immediately picks apart, reading a dozen ovr_LaunchDetails_* fields off it
// in `Models.LaunchDetails`'s constructor. SUPERHOT asks while building its main
// scene.
//
// The truthful answer is "normally, by the user, with nothing attached": no
// deeplink message, no destination, no room, no invited users, LaunchType 0
// (Unknown). That is precisely what every accessor answers below, because 0 is
// simultaneously the zero enum, the zero integer and the NULL pointer — and the
// SDK null-checks each optional list before wrapping it, so NULL is how it
// spells "there was none", not a hole.
//
// The handle itself must be NON-NULL for the same reason the options cookie is:
// it is carried straight into those accessors, and on a real platform a NULL
// there is a null dereference inside the library rather than an empty answer.
static const char g_launch_cookie[] = "klepton-launch-details";

static uint64_t klplat_launch_details(const char *name) {
    plat_hit(name);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] %s -> opaque handle: launched normally, by the "
                        "user, with nothing attached (no deeplink, no destination, "
                        "no invited users)\n", name);
    }
    return (uint64_t)(uintptr_t)g_launch_cookie;
}

static uint64_t klplat_launch_field(const char *name) {
    plat_hit(name);
    return 0;                 // the zero enum, the zero int and NULL, all at once
}

static int plat_is_launch_details(const char *name) {
    return strcmp(name, "ovr_ApplicationLifecycle_GetLaunchDetails") == 0;
}

static int plat_is_launch_field(const char *name) {
    return strncmp(name, "ovr_LaunchDetails_", 18) == 0;
}

// ...and the empty ARRAY that a NULL list handle turns into one call later.
//
// The managed `DeserializableList<T>` reads three things off a list handle —
// GetSize, GetElement and GetNextUrl — and this SDK version wraps the pointer
// WITHOUT null-checking it first, so answering ovr_LaunchDetails_GetUsers with
// NULL (correctly: nobody was invited) still ends in ovr_UserArray_GetSize(NULL)
// one call later. Size 0 is the same answer the NULL was: there is nobody in it.
//
// GetElement is deliberately NOT here. It can only be reached on an array with
// elements, which is an array we did not produce, so it keeps the fail-closed
// default and would stop the run by name rather than hand back a User the guest
// would then read fields off.
static int plat_is_empty_array(const char *name) {
    const char *p = strstr(name, "Array_Get");
    if (!p) return 0;
    return strcmp(p, "Array_GetSize") == 0 || strcmp(p, "Array_GetNextUrl") == 0;
}

static uint64_t klplat_empty_array(const char *name) {
    plat_hit(name);
    return 0;                 // no elements, and no next page to fetch them from
}

// The setters and the destructor. Both are genuinely nothing to do, not stubs
// standing in for something: there is no options state to keep.
static int plat_is_options_sink(const char *name) {
    return strstr(name, "Options_Destroy") != NULL ||
           strstr(name, "Options_Set") != NULL;
}

// ---------------------------------------------------------------------------
// Launch details: how the app was started. Not a request — a plain accessor
// returning an opaque handle the managed SDK immediately picks apart
// (Models.LaunchDetails reads a dozen ovr_LaunchDetails_* fields off it in its
// constructor). SUPERHOT asks while building its main scene.
//
// The truthful answer is "normally, by the user, with nothing attached": no
// deeplink message, no destination, no room, no invited users, LaunchType 0
// (Unknown). That is what every ovr_LaunchDetails_* accessor reports below —
// 0 for the integers and enums, NULL for the strings — and it is the same
// answer the real platform gives an app nobody deeplinked into.
//
// The handle itself must be non-NULL for the same reason the options cookie
// is: a NULL would be carried into those accessors, and on a real platform
// that is a null deref inside the library rather than an empty answer.
static uint64_t klplat_GetLaunchDetails(void) {
    plat_hit("ovr_ApplicationLifecycle_GetLaunchDetails");
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_ApplicationLifecycle_GetLaunchDetails -> empty "
                        "(launched normally: no deeplink, no destination, no party)\n");
    }
    return (uint64_t)(uintptr_t)g_launch_cookie;
}

// The accessors over it, plus the user array one of them hands back. Reporting
// "nothing" is the answer, not a stand-in for one: 0 is an empty string / a
// zero id / LaunchType_Unknown / an array of length 0, and the managed models
// are built to be told exactly that.
static uint64_t klplat_nothing(const char *name) {
    plat_hit(name);
    return 0;
}

// ovrLaunchType_ToString — the enum-to-string helper beside those accessors, and
// UE4's OculusPlatform module calls it on whatever GetLaunchType answered so it
// can log the launch. In this title's own libovrplatformloader.so (+0x86898) it
// is a loader THUNK — `str w0,[sp,#0xc]; ldr x8,[got+0x120]; ldr w0,[sp,#0xc];
// blr x8` — so the binary states the ABI (one 32-bit enum in w0, a pointer out)
// and nothing else; the table it forwards to lives in com.oculus.horizon, which
// is not here. The strings are the platform SDK's own OVR_LaunchType.h spelling.
//
// We answer 0 / ovrLaunchType_Unknown from every launch accessor (see above), so
// "Unknown" is the string this actually produces on this host. The rest of the
// table is there so the answer stays right if that ever changes, and the default
// is the SDK's own "UNKNOWN_ERROR" rather than NULL — a caller that logs the
// result with %s must not be handed one.
static const char *klplat_LaunchType_ToString(int v) {
    plat_hit("ovrLaunchType_ToString");
    switch (v) {
    case 0: return "Unknown";
    case 1: return "Normal";
    case 2: return "Invite";
    case 3: return "Coordinated";
    case 4: return "Deeplink";
    }
    return "UNKNOWN_ERROR";
}

static int plat_is_launch_accessor(const char *name) {
    return strncmp(name, "ovr_LaunchDetails_", 18) == 0 ||
           strncmp(name, "ovr_UserArray_", 14) == 0;
}

// ---------------------------------------------------------------------------
// Accessors over a message we did not produce, and the arrays hanging off one.
//
// Every request above answers 0, and a request that answered 0 queues no
// message — so on a Unity guest the SDK synthesizes the failure client-side and
// none of these is reachable. They are answered anyway, because "not reachable"
// is a property of the guest's SDK version rather than of this file, and the
// price of being wrong about it has been one launch per name.
//
// Zero is the right answer for all of them at once, and it is the answer the
// real SDK gives for a field that is not on an error message: an empty array, a
// zero id, a zero rank or score, and NULL for every string and sub-handle. The
// managed models are built to be told exactly that — DeserializableList<T>
// reads GetSize and stops, and every optional sub-object is null-checked.
//
// Narrow on purpose: the message / array / error / leaderboard-entry /
// challenge-entry families only. Everything else still falls through to the
// terminal tier, which now NAMES what it answered instead of stopping the run,
// so the next log is still a work list.
//
// It runs AFTER plat_is_request in kl_ovrplat_sym, so a request whose name
// happens to contain one of these fragments is still classified as a request.
static int plat_is_model_accessor(const char *name) {
    return strstr(name, "Array_Get") != NULL              ||
           strncmp(name, "ovr_Error_", 10)            == 0 ||
           strncmp(name, "ovr_Message_Get", 15)       == 0 ||
           strncmp(name, "ovr_LeaderboardEntry_", 21) == 0 ||
           strncmp(name, "ovr_LeaderboardUpdateStatus_", 28) == 0 ||
           strncmp(name, "ovr_ChallengeEntry_", 19)    == 0 ||
           strncmp(name, "ovr_Challenge_", 14)         == 0;
}

static uint64_t klplat_model_accessor(const char *name) {
    plat_hit(name);
    int s = plat_slot(name);
    if (s < 0 || g_plat[s].calls == 1)
        fprintf(stderr, "  [plat] %s -> 0 (accessor over a message this host never "
                        "produced: an empty array, a zero id and a NULL string, "
                        "which is what the SDK reads off an error message)\n", name);
    return 0;
}

// The three handle-returning CREATORS outside the options family, which are the
// only names in this guest's 700 where a NULL is a contract violation rather
// than an empty answer — and the only ones where NULL is nonetheless the honest
// report, because the device really is absent.
//
// kl_aaudio refuses capture streams by design and no microphone is presented on
// this host, so ovr_Microphone_Create has nothing to open; the two Voip codec
// creators belong to a voice session that cannot exist without it. A real
// headset with no microphone permission answers exactly this way, and the Unity
// SDK's Voip.CreateEncoder / Microphone wrappers null-check what they get back.
//
// ovr_GetLoggedInUserLocale rides along because it is the same shape — a
// synchronous accessor returning a const char * we do not have. NULL is the
// SDK's "unknown", and Marshal.PtrToStringAnsi(NULL) is null on the managed
// side rather than a fault.
static const char *const g_plat_absent[] = {
    "ovr_Microphone_Create", "ovr_Voip_CreateEncoder", "ovr_Voip_CreateDecoder",
    "ovr_GetLoggedInUserLocale",
};

static int plat_is_absent(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_absent / sizeof g_plat_absent[0]; i++)
        if (strcmp(g_plat_absent[i], name) == 0) return 1;
    return 0;
}

static uint64_t klplat_absent(const char *name) {
    plat_hit(name);
    int s = plat_slot(name);
    if (s < 0 || g_plat[s].calls == 1)
        fprintf(stderr, "  [plat] %s -> NULL (the device or datum it would hand "
                        "back is genuinely absent on this host, which is what a "
                        "headset with no microphone and no session reports)\n", name);
    return 0;
}

static int plat_is_request(const char *name) {
    for (size_t i = 0; i < sizeof g_plat_request / sizeof g_plat_request[0]; i++)
        if (strcmp(g_plat_request[i], name) == 0) return 1;
    for (size_t i = 0; i < sizeof g_plat_request_prefix / sizeof g_plat_request_prefix[0]; i++)
        if (strncmp(name, g_plat_request_prefix[i],
                    strlen(g_plat_request_prefix[i])) == 0) return 1;
    return 0;
}

// Who is signed in — the C SDK's synchronous accessor, not a request. It
// returns an `ovrID`, and 0 is that type's own "no such user": the managed side
// spells the same thing `User == null`, and every caller of it null-checks,
// because a user really can be absent on a real headset (offline, or before the
// platform has initialized).
//
// Not an ownership question and not a request, so neither classifier is right
// for it: `ovr_User_GetLoggedInUser` beside them is the ASYNC form and correctly
// answers "the request could not be made", while this one is asked and answered
// on the spot. RE4 asks during engine init.
//
// `KL_PLAT_USER=<ovrID>` answers an id instead, and it exists because 0 is not
// free for every guest: UE4's OnlineSubsystemOculus REFUSES to Init without a
// logged-in user, and a subsystem that did not Init never Ticks — so it never
// calls ovr_PopMessage, and every completion this file queues (the entitlement
// answer included) sits in the queue forever. Measured on Tetris Effect r2:
// 0 gives a platform surface of 9 calls and no pump at all; a non-zero id gives
// 15, `ovr_PopMessage` x8697, and the entitlement completion actually popped,
// checked with ovr_Message_IsError and freed.
//
// It is OFF by default and stays off. Answering an id is a claim about a person
// where the truthful answer is "nobody", so it has to be asked for out loud —
// a target that needs one bakes it, the same shape as PORTING.md's KL_PLAT_INIT
// bake, and the log says so on every run that uses it.
static uint64_t klplat_GetLoggedInUserID(void) {
    plat_hit("ovr_GetLoggedInUserID");
    const char *forced = kl_env_str("KL_PLAT_USER", NULL);
    unsigned long long id = forced && *forced ? strtoull(forced, NULL, 0) : 0;
    static int said;
    if (!said) {
        said = 1;
        if (id)
            fprintf(stderr, "  [plat] ovr_GetLoggedInUserID -> %llu (KL_PLAT_USER: "
                            "there is no Oculus account here, this id is INVENTED "
                            "— set so the guest's platform subsystem will "
                            "initialize and pump its messages)\n", id);
        else
            fprintf(stderr, "  [plat] ovr_GetLoggedInUserID -> 0 (no platform user; "
                            "there is no service here to be signed in to)\n");
    }
    return id;
}

// The real libovrplatformloader exports JNI_OnLoad and caches the JavaVM out of
// it. Ours has no JNI surface to set up, so the whole body is the version
// number Android checks for.
//
// Reached only because System.load() now honours the Android contract and calls
// JNI_OnLoad on what it loaded (kl_jni.c). Before that, this library was
// dlopen'd and never initialized, and nothing noticed — which is exactly the
// bug that change fixes, so this is the cost of fixing it rather than a
// workaround. Answering the version is not a stub: a library that returns an
// unrecognised version is one that REFUSED to initialize, and Android unloads it.
//
// Below plat_is_drm in kl_ovrplat_sym, like everything in this table, so the
// entitlement classifier still runs first and this cannot be a way around it.
static uint64_t klplat_JNI_OnLoad(void *vm, void *reserved) {
    (void)vm; (void)reserved;
    plat_hit("JNI_OnLoad");
    return 0x00010006;                 // JNI_VERSION_1_6
}

// ---------------------------------------------------------------------------
// DLC TEST OVERRIDE — the enumeration machinery (branch dlc-test-override only).
//
// ovr_AssetFile_GetList builds a real message describing the asset files found
// in the configured asset dir, queues it, and returns a request id; the SDK's
// own pump (ovr_PopMessage, already called every frame) delivers it on a later
// call. The accessors hand the managed SDK exactly what GetList put in. Every
// string points into the message and is freed by ovr_FreeMessage, after the SDK
// has copied what it needs — that is the real platform's lifetime contract.
//
// The type constant is the SDK's ovrMessageType_AssetFile_GetList, and the
// status strings are the SDK's own vocabulary ("default" / "installed" /
// "entitled"). "entitled" is the override: it asserts ownership from file
// presence. Only files physically present in the asset dir are ever listed.
#define KL_MSG_ASSETFILE_GETLIST 0x4afc6f74u

// ...and the three PER-FILE status spellings. Read out of THIS GUEST's own
// Oculus.Platform.Message/MessageType enum in global-metadata.dat, by the same
// walk that produced the constants at the top of this file, with
// AssetFile_GetList (0x4afc6f74) and Entitlement_GetIsViewerEntitled
// (0x186b58b1) reproduced exactly as the calibration. Necessary rather than
// tidy: Message.ParseMessageHandle switches on the type FIRST and its default
// arm logs "Unrecognized message type" and produces NO message, so a guessed
// number is indistinguishable from a completion that never arrives.
#define KL_MSG_ASSETFILE_STATUS       0x02d32f60u
#define KL_MSG_ASSETFILE_STATUSBYID   0x5d955d38u
#define KL_MSG_ASSETFILE_STATUSBYNAME 0x41cfda50u

static char            g_asset_dir[1024];
static int             g_asset_dir_set;

void kl_ovrplat_set_asset_dir(const char *dir) {
    if (!dir || !*dir) { g_asset_dir_set = 0; return; }
    snprintf(g_asset_dir, sizeof g_asset_dir, "%s", dir);
    g_asset_dir_set = 1;
}

// One queue, two message shapes. Publishing is its own function because the
// moment a message is on the queue it belongs to whoever pops it next — reading
// anything back out of `m` after this returns is a use-after-free waiting for a
// second thread — so the request id comes back by value and the caller keeps
// nothing.
static uint64_t klplat_enqueue(klplat_msg *m) {
    pthread_mutex_lock(&g_msg_mu);
    uint64_t id = m->request_id = g_next_request++;
    if (g_msg_tail) g_msg_tail->next = m;
    else            g_msg_head = m;
    g_msg_tail = m;
    pthread_mutex_unlock(&g_msg_mu);
    return id;
}

// A completion with no payload: type, request id, and "not an error", which is
// every field the SDK reads off a message it has no accessor for. The
// entitlement answer is exactly this shape.
static uint64_t klplat_queue_bare(uint32_t type) {
    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type = type;
    return klplat_enqueue(m);
}

// ---------------------------------------------------------------------------
// ovr_CloudStorage2_GetUserDirectoryPath — the one cloud-save call that has to
// be ANSWERED rather than refused.
//
// It was in the request-returning families below, answering 0 ("the request
// could not be made"), on the reasoning that a game which cannot reach the
// cloud writes locally. For SUPERHOT that reasoning is wrong in a way that
// cost this port a week: 0 does not put the game on a local path, it puts it
// on NO path. `VRSaveManager::LoadAsync` (libil2cpp+0xd2174c) asks for the
// directory and hangs its whole continuation off the answer —
//
//     req = CloudStorage2.GetUserDirectoryPath();
//     if (req == null) FailedToLoad(...);            // quits
//     req.OnComplete(LoadAsyncContinued);            // the ONLY way forward
//
// — and the SDK's OnComplete drops a request whose id is 0 on the floor with
// "An unknown error occurred. Request failed.", so `LoadAsyncContinued` never
// runs, `VRLaunch::OnLoadCompleted` never runs, and the `LoadSceneAsync` at
// the end of it never happens. The launch scene stays on screen forever. That
// is the same shape as the entitlement bug above: a request id nothing would
// ever complete.
//
// So: queue a real completion carrying a real directory. This asserts nothing
// about ownership — a per-user save directory is the user's own data, not a
// purchase — and the honest answer on a machine with no Oculus cloud is a
// local directory that is really there. `LoadAsyncContinued` stores it as the
// save folder and `PerformOneTimeSaveMerging` calls IsDirectoryWritable on it
// before anything else (and quits if it is not), so it is created here.
//
// 0x76a42eee is ovrMessageType_CloudStorage2_GetUserDirectoryPath, read out of
// the guest's own metadata with KL_PROBE_ENUM=Oculus.Platform.Message/
// MessageType (kl_mprobe.c) — the same table, cross-checked in the same run
// against the two constants already baked here: Entitlement_GetIsViewerEntitled
// 0x186b58b1 and AssetFile_GetList 0x4afc6f74, both exact.
#define KL_MSG_CLOUDSTORAGE2_GETUSERDIRECTORYPATH 0x76a42eeeu

static uint64_t klplat_CloudStorage2_GetUserDirectoryPath(void) {
    plat_hit("ovr_CloudStorage2_GetUserDirectoryPath");

    // KL_PLAT_CLOUD_DIR overrides the directory; setting it EMPTY restores the
    // old request-id-0 refusal, which is the A/B for "is this what unblocked
    // the launch".
    const char *over = kl_env_str("KL_PLAT_CLOUD_DIR", NULL);
    if (over && !*over) return 0;

    static char dir[1100];
    if (!dir[0]) {
        if (over) snprintf(dir, sizeof dir, "%s", over);
        // A sibling of getFilesDir()'s `files`, not a child: the game merges
        // this directory against Application.persistentDataPath (which IS
        // `files`), and a save folder nested inside the folder it is being
        // merged with invites the game to find its own copies.
        else snprintf(dir, sizeof dir, "%s/cloudstorage", kl_jni_files_dir());
    }
    struct stat st;
    if (stat(dir, &st) != 0 && mkdir(dir, 0755) != 0) {
        fprintf(stderr, "  [plat] ovr_CloudStorage2_GetUserDirectoryPath: cannot "
                        "create %s — answering 0 (the request could not be made)\n", dir);
        return 0;
    }

    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type = KL_MSG_CLOUDSTORAGE2_GETUSERDIRECTORYPATH;
    m->str  = strdup(dir);
    if (!m->str) { free(m); return 0; }
    uint64_t req = klplat_enqueue(m);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_CloudStorage2_GetUserDirectoryPath -> \"%s\" "
                        "(no Oculus cloud here; the user's save directory is local)\n", dir);
    }
    return req;
}

// The asset id. Any unique nonzero value does: the game binds content to files
// by FILENAME (the enumeration hands back a Filepath and that is what it maps),
// and the id only feeds the Download path, which the "installed" status keeps
// it off and the refusals keep it off again. FNV-1a over the basename, so the
// SAME file gets the SAME id however it was reached — which is what makes
// ovr_AssetFile_StatusById answerable at all: the id the game holds came from
// an AssetDetails we built, so hashing the directory back is a lookup rather
// than a guess.
static uint64_t klplat_asset_id(const char *basename) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)basename; *p; p++)
        h = (h ^ *p) * 1099511628211ull;
    return h ? h : 1;
}

static uint64_t klplat_GetList(void) {
    plat_hit("ovr_AssetFile_GetList");
    // No asset dir configured (the other mains, the guard's forked child) or
    // the override switched off: the legacy answer, request id 0 — "the
    // request could not be made".
    if (!g_asset_dir_set || !kl_env_on("KL_DLC_TEST", 1)) return 0;

    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type = KL_MSG_ASSETFILE_GETLIST;

    size_t cap = 0;
    DIR *d = opendir(g_asset_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;             // dotfiles, .DS_Store
            // ...and the guest's own APPLICATION BINARY, which for a split
            // build lives in this very directory: Beat Saber 1.40 is
            // main.1661.com.beatgames.beatpaber.obb, 1.34 GB, a sibling of the
            // ~246 song bundles because that is how the Quest dump stores it
            // and where Unity looks for it. Without this the game is told it
            // owns a DLC pack whose name is the OBB's — a 1.3 GB "song" with an
            // entitled IapStatus, reported to the very code that is about to
            // open the same file as its data. Nothing else here filters by
            // extension because nothing else had two kinds of file to tell
            // apart.
            {
                size_t n = strlen(de->d_name);
                if (n > 4 && strcmp(de->d_name + n - 4, ".obb") == 0) continue;
            }
            char path[1200];
            snprintf(path, sizeof path, "%s/%s", g_asset_dir, de->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (m->count == cap) {
                cap = cap ? cap * 2 : 16;
                klplat_asset *ni = realloc(m->items, cap * sizeof *m->items);
                if (!ni) { free(m->items); free(m); closedir(d); return 0; }
                m->items = ni;
            }
            m->items[m->count].asset_id  = klplat_asset_id(de->d_name);
            m->items[m->count].filepath  = strdup(path);
            m->items[m->count].installed = 1;      // it was just stat'd; it is there
            m->count++;
        }
        closedir(d);
    }

    size_t   found = m->count;
    uint64_t req   = klplat_enqueue(m);      // `m` is the queue's from here on

    fprintf(stderr, "  [plat] ovr_AssetFile_GetList -> %zu asset file(s) in %s "
                    "(DLC TEST OVERRIDE: IapStatus \"entitled\" asserted from "
                    "file presence)\n", found, g_asset_dir);
    return req;
}

// ---------------------------------------------------------------------------
// The PER-FILE half of the same question: ovr_AssetFile_StatusByName / _StatusById
//
// Arkham Shadow does not enumerate. It asks about its language packs by name,
// one at a time — boot #5 died on the first of them, immediately after it had
// asked UnityPlayerActivity for getObbDir() (i.e. after working out where the
// files would be). That is the same question ovr_AssetFile_GetList asks in
// bulk, asked one file at a time, and it gets the same answer from the same
// place: the enumeration root, stat'd.
//
// **The DRM line does not move.** This asserts nothing GetList did not already
// assert on this branch — ownership from file presence, for a file the user
// supplied from their own copy of the game — and it asserts NOTHING AT ALL
// about a name that is not on disk: that answers "available / not-entitled",
// which is the platform saying "not yours and not here". Every delivery call
// (ovr_AssetFile_Download*, _DownloadCancel*, _Delete*) and every IAP /
// Purchase / Entitlement query is untouched and still aborts by name, and
// m_boot's guard asserts both directions.
//
// Shape: the SDK's `Message<AssetDetails>` — a single AssetDetails read through
// ovr_Message_GetAssetDetails, not the array GetList produces. One message
// carrying exactly one item serves that: klplat_msg_asset_details hands back
// &items[0], and the ovr_AssetDetails_* accessors above are already the ones
// the managed model reads.
//
// MEASURED, not assumed, in this guest's own ParseMessageHandle (libil2cpp
// +0x3296760..+0x3297700, the message-type switch): all three status spellings
// — AssetFile_Status 0x02d32f60, AssetFile_StatusById 0x5d955d38 and
// AssetFile_StatusByName 0x41cfda50 — branch to ONE arm at +0x3297694, which
// builds the wrapper class held at [statics+0xc78]. LanguagePack_GetCurrent
// (0x1f90f0d5) lands on that same arm, and `LanguagePack.GetCurrent()` is
// declared `Request<AssetDetails>` in the SDK, which names the class. The
// DELETE spellings (AssetFile_Delete 0x6d5d7886, _DeleteByName 0x420ac1cf) go
// to a DIFFERENT arm at +0x3297678 / [statics+0xc88] — the
// AssetFileDeleteResult wrapper — so the two shapes really are distinct in this
// build and this is the right one of them. `ovr_Message_GetAssetDetails` is in
// the binary's import strings and nothing else here answers it, so if the arm
// were the other shape after all the guest would ask for a name we do not
// serve and the fail-closed default would abort naming it, rather than read a
// wrong payload.
//
// WHAT THE GAME READS, which is what the three strings above have to satisfy.
// `Manta.Platform.CorePlatformSubsystem/DownloadRequest::OnVerifyComplete`
// (libil2cpp+0x1dde81c) is the completion handler, reached from
// `DownloadRequest.Verify` -> `AssetFile.StatusByName(DownloadRequest.FileName)`.
// It:
//   - fails the whole verify if `msg.Data` is null, so the message MUST carry an
//     AssetDetails (klplat_msg_asset_details never hands back NULL for one of
//     these — count is 1 by construction);
//   - stores `AssetId` as `DownloadRequest.ID` and uses it as the handle for
//     every later operation (DownloadById / DeleteById / DownloadCancelById),
//     which is why the id has to be STABLE for a given file — it is, being a
//     hash of the basename rather than a counter;
//   - String.Equals `DownloadStatus` against exactly "installed",
//     "in-progress" and "available", and ANYTHING ELSE is an error
//     ("Unable to verify asset <name>, is the file name correct?"). So those
//     two words are not a choice of vocabulary, they are the vocabulary;
//   - for "installed", calls File.Exists(Filepath) and downgrades to
//     in-progress if the file is not there — hence the real path, stat'd;
//   - NEVER READS IapStatus. Batman asserts nothing about entitlement at all;
//     it asks whether the file is present. The "entitled" this branch reports
//     is inherited from the GetList override and is dead weight here, which
//     makes the per-file carve-out strictly narrower than the one it extends.
// The names it asks about come from `LanguageData._assetNames`;
// `Manta.Audio.GlobalAudioManager::OnSetSoundBanksPath` builds them as
// "Audio_NonVO.obb" and "Audio_" + AkSoundEngine.GetCurrentLanguage() + ".obb",
// i.e. the Audio_en-US.obb / Audio_NonVO.obb staged into the enumeration root.
// Note those are `.obb` files: GetList's *.obb skip does NOT apply here, and
// must not — there the guest names the file itself.
static uint64_t klplat_status_msg(const char *what, uint32_t type,
                                  const char *fname, uint64_t id) {
    // Only ever inside the enumeration root: the name comes from the guest, and
    // a status query is not a path query. Anything with a separator in it is
    // reduced to its last component, so "../../x" asks about "x".
    const char *base = fname;
    if (base) {
        const char *s = strrchr(base, '/');
        if (s) base = s + 1;
    }

    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type  = type;
    m->items = calloc(1, sizeof *m->items);
    if (!m->items) { free(m); return 0; }
    m->count = 1;

    char path[1200];
    struct stat st;
    if (base && *base) {
        snprintf(path, sizeof path, "%s/%s", g_asset_dir, base);
        m->items[0].installed = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
        m->items[0].asset_id  = id ? id : klplat_asset_id(base);
        // The path is reported whether or not the file is there: it is where
        // the file WOULD be, which is what the SDK's AssetDetails.Filepath
        // means for a not-yet-installed asset. Nothing follows it unless the
        // status says installed.
        m->items[0].filepath  = strdup(path);
    } else {
        // Asked by id about something we could not find in the directory (see
        // klplat_StatusById). Not installed, not entitled, no path.
        m->items[0].asset_id = id;
    }

    int      inst  = m->items[0].installed;
    uint64_t req   = klplat_enqueue(m);       // `m` is the queue's from here on
    fprintf(stderr, "  [plat] %s(\"%s\") -> %s (DLC TEST OVERRIDE: answered from "
                    "file presence in %s)\n",
            what, base && *base ? base : "<by id>",
            inst ? "installed / entitled" : "available / not-entitled", g_asset_dir);
    return req;
}

// The gate every per-file status query goes through. KL_DLC_TEST=0 does NOT
// mean "answer nothing" here the way it does for GetList — GetList is answered
// on main too (with an empty list), while these are refusals on main, so
// switching the override off has to put them back on the refusal. That is what
// makes KL_DLC_TEST=0 a real A/B rather than a different third behaviour.
static int klplat_status_available(void) {
    return g_asset_dir_set && kl_env_on("KL_DLC_TEST", 1);
}

static uint64_t klplat_StatusByName(const char *name) {
    // The refusal counts the call itself (klplat_drm does its own book-keeping),
    // so the hit goes on the answered path only — otherwise a KL_DLC_TEST=0 run
    // reports every refused query twice.
    if (!klplat_status_available()) return klplat_drm("ovr_AssetFile_StatusByName");
    plat_hit("ovr_AssetFile_StatusByName");
    return klplat_status_msg("ovr_AssetFile_StatusByName",
                             KL_MSG_ASSETFILE_STATUSBYNAME, name, 0);
}

// ...and the by-id spelling. The id can only have come from an AssetDetails we
// built, so the directory is scanned and the hashes compared — a lookup of our
// own answer, not a new assertion. A miss is reported as not installed rather
// than aborting, because "no such asset" is a thing the real platform says.
static uint64_t klplat_status_by_id(const char *what, uint32_t type, uint64_t id) {
    if (!klplat_status_available()) return klplat_drm(what);
    plat_hit(what);

    char found[512] = "";
    DIR *d = opendir(g_asset_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            if (klplat_asset_id(de->d_name) == id) {
                snprintf(found, sizeof found, "%s", de->d_name);
                break;
            }
        }
        closedir(d);
    }
    return klplat_status_msg(what, type, found[0] ? found : NULL, id);
}

static uint64_t klplat_StatusById(uint64_t id) {
    return klplat_status_by_id("ovr_AssetFile_StatusById",
                               KL_MSG_ASSETFILE_STATUSBYID, id);
}

// The deprecated spelling of the same query — same argument, same answer, its
// own message type because the managed SDK switches on the type before it looks
// at anything else and each spelling has its own enum member.
static uint64_t klplat_Status(uint64_t id) {
    return klplat_status_by_id("ovr_AssetFile_Status",
                               KL_MSG_ASSETFILE_STATUS, id);
}

// The message pump. NULL still means "no messages queued" — the documented
// steady state; now it can also hand out a queued enumeration message.
static void *klplat_PopMessage(void) {
    plat_hit("ovr_PopMessage");
    pthread_mutex_lock(&g_msg_mu);
    klplat_msg *m = g_msg_head;
    if (m) {
        g_msg_head = m->next;
        if (!g_msg_head) g_msg_tail = NULL;
        m->next = NULL;
    }
    pthread_mutex_unlock(&g_msg_mu);
    return m;
}

static uint64_t klplat_FreeMessage(klplat_msg *m) {
    plat_hit("ovr_FreeMessage");
    if (!m) return 0;
    for (size_t i = 0; i < m->count; i++) free(m->items[i].filepath);
    free(m->items);
    free(m->str);
    free(m);
    return 0;
}

// Accessors over the message and its contents. The handles are our own
// structs; every read goes through one of these, so the layout is ours.
static uint64_t    klplat_msg_type(klplat_msg *m)        { return m ? m->type : 0; }
static uint64_t    klplat_msg_is_error(klplat_msg *m)    { (void)m; return 0; }
static uint64_t    klplat_msg_request_id(klplat_msg *m)  { return m ? m->request_id : 0; }
static void       *klplat_msg_error(klplat_msg *m)       { (void)m; return NULL; }
static void       *klplat_msg_string(klplat_msg *m)      { return m ? m->str : NULL; }
static void       *klplat_msg_native(klplat_msg *m)      { return m; }
static void       *klplat_msg_asset_array(klplat_msg *m) { return m; }
// ...and the SINGLE-AssetDetails payload the per-file status queries carry.
// One item, so the handle is that item rather than the message — the
// ovr_AssetDetails_* accessors below take a klplat_asset*, and this is the only
// place a message hands one out directly.
static void       *klplat_msg_asset_details(klplat_msg *m) {
    return (m && m->count) ? &m->items[0] : NULL;
}
static uint64_t    klplat_arr_size(klplat_msg *m)        { return m ? m->count : 0; }
static void       *klplat_arr_element(klplat_msg *m, uint64_t i) {
    return (m && i < m->count) ? &m->items[i] : NULL;
}
static uint64_t    klplat_ad_id(klplat_asset *a)       { return a ? a->asset_id : 0; }
static const char *klplat_ad_type(klplat_asset *a)     { (void)a; return "default"; }
// The two status strings, and the one place the override's assertion is made.
//
// A file that is on disk is reported "installed" / "entitled"; a NAME that is
// not on disk is reported "available" / "not-entitled" — i.e. the platform has
// it in its catalogue but this device does not have it and this user has no
// claim on it. Both are the SDK's own vocabulary. Nothing here invents a
// download: "available" is the status a real platform reports for content the
// user could buy, and every ovr_AssetFile_Download* spelling still aborts, so
// the absent branch is a dead end by construction rather than by hope.
static const char *klplat_ad_status(klplat_asset *a) {
    return (a && a->installed) ? "installed" : "available";
}
static const char *klplat_ad_path(klplat_asset *a)     { return a ? a->filepath : NULL; }
static const char *klplat_ad_iap(klplat_asset *a) {
    return (a && a->installed) ? "entitled" : "not-entitled";
}
static void       *klplat_ad_language(klplat_asset *a) { (void)a; return NULL; }
static const char *klplat_ad_meta(klplat_asset *a)     { (void)a; return ""; }

// The language-pack sub-object: our asset files carry none, so the handle is
// NULL and every accessor says "nothing here" (null string to the managed
// model). The game reads these unconditionally when parsing an AssetDetails.
static const char *klplat_lang_string(void *h)         { (void)h; return NULL; }

// ---------------------------------------------------------------------------
// ovr_IAP_GetViewerPurchases — answered EMPTY (branch itr).
//
// The one ownership question in this file that is answered with a NEGATIVE
// rather than refused. See the carve-out comment at the top for the policy; the
// mechanics are: queue a completion carrying a purchase array of length 0, hand
// the guest a real request id, and let its own pump deliver it. count is 0 and
// items is NULL, so every accessor over it already says "nothing" — this reuses
// the array shape GetList built rather than adding a second one.
//
// THE MESSAGE TYPE IS 0 (ovrMessage_Unknown), on purpose, and it is safe here
// for the same reason it is on the pinballfx branch (see the note under
// g_plat_request_prefix): UNREAL dispatches a completion on the REQUEST ID.
// FOnlineSubsystemOculus::OnReceiveMessage looks the id up in its
// RequestDelegates map and only falls back to ovr_Message_GetType for
// notifications, which have no id. Into the Radius is a UE 4.27 guest and
// libUE4 imports ovr_Message_GetRequestID / _IsError / _GetPurchaseArray for
// exactly that path. A UNITY guest would need the real ovrMessageType constant
// read out of its own metadata with KL_PROBE_ENUM (Messages.ParseMessageHandle
// switches on the type FIRST), so if this is ever wanted on a Unity title, that
// is the thing to add before it is used there — not a guessed number.
static uint64_t klplat_IAP_GetViewerPurchases(void) {
    plat_hit("ovr_IAP_GetViewerPurchases");

    klplat_msg *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->type  = 0;                 // ovrMessage_Unknown; UE dispatches on the id
    m->count = 0;                 // ...and the array is EMPTY. That is the answer.
    m->items = NULL;
    uint64_t req = klplat_enqueue(m);
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "  [plat] ovr_IAP_GetViewerPurchases -> request %llu, "
                        "completion queued with an EMPTY purchase array (this "
                        "viewer owns nothing: there is no Oculus Store account "
                        "here and no purchase history. Nothing is claimed, no "
                        "SKU is named, and every purchasing / delivery call "
                        "still refuses)\n", (unsigned long long)req);
    }
    return req;
}

// The accessors over that empty array. The message IS the array handle, as it
// is for the asset enumeration; size and element route to the shared array
// accessors, which answer 0 and NULL for count 0.
static uint64_t    klplat_purch_has_next(klplat_msg *m) { (void)m; return 0; }
static const char *klplat_purch_next_url(klplat_msg *m) { (void)m; return NULL; }

static const struct { const char *name; void *fn; } g_plat_enum_impl[] = {
    {"ovr_AssetFile_GetList",                       klplat_GetList},
    {"ovr_AssetFile_Status",                        klplat_Status},
    {"ovr_AssetFile_StatusById",                    klplat_StatusById},
    {"ovr_AssetFile_StatusByName",                  klplat_StatusByName},
    {"ovr_Message_GetAssetDetails",                 klplat_msg_asset_details},
    {"ovr_FreeMessage",                             klplat_FreeMessage},
    {"ovr_Message_GetType",                         klplat_msg_type},
    {"ovr_Message_IsError",                         klplat_msg_is_error},
    {"ovr_Message_GetRequestID",                    klplat_msg_request_id},
    {"ovr_Message_GetError",                        klplat_msg_error},
    {"ovr_Message_GetString",                       klplat_msg_string},
    {"ovr_Message_GetString_Native",                klplat_msg_string},
    {"ovr_Message_GetNativeMessage",                klplat_msg_native},
    {"ovr_Message_GetAssetDetailsArray",            klplat_msg_asset_array},
    {"ovr_AssetDetailsArray_GetSize",               klplat_arr_size},
    {"ovr_AssetDetailsArray_GetElement",            klplat_arr_element},
    {"ovr_AssetDetails_GetAssetId",                 klplat_ad_id},
    {"ovr_AssetDetails_GetAssetType",               klplat_ad_type},
    {"ovr_AssetDetails_GetAssetType_Native",        klplat_ad_type},
    {"ovr_AssetDetails_GetDownloadStatus",          klplat_ad_status},
    {"ovr_AssetDetails_GetDownloadStatus_Native",   klplat_ad_status},
    {"ovr_AssetDetails_GetFilepath",                klplat_ad_path},
    {"ovr_AssetDetails_GetFilepath_Native",         klplat_ad_path},
    {"ovr_AssetDetails_GetIapStatus",               klplat_ad_iap},
    {"ovr_AssetDetails_GetIapStatus_Native",        klplat_ad_iap},
    {"ovr_AssetDetails_GetLanguage",                klplat_ad_language},
    {"ovr_AssetDetails_GetMetadata",                klplat_ad_meta},
    {"ovr_AssetDetails_GetMetadata_Native",         klplat_ad_meta},
    {"ovr_LanguagePackInfo_GetEnglishName",         klplat_lang_string},
    {"ovr_LanguagePackInfo_GetEnglishName_Native",  klplat_lang_string},
    {"ovr_LanguagePackInfo_GetNativeName",          klplat_lang_string},
    {"ovr_LanguagePackInfo_GetNativeName_Native",   klplat_lang_string},
    {"ovr_LanguagePackInfo_GetTag",                 klplat_lang_string},
    {"ovr_LanguagePackInfo_GetTag_Native",          klplat_lang_string},
    // The empty purchase answer (itr). ovr_Purchase_* is NOT here on purpose:
    // an empty array has no element to read a SKU off, so those names keep the
    // refusal.
    {"ovr_IAP_GetViewerPurchases",                  klplat_IAP_GetViewerPurchases},
    {"ovr_Message_GetPurchaseArray",                klplat_msg_asset_array},
    {"ovr_PurchaseArray_GetSize",                   klplat_arr_size},
    {"ovr_PurchaseArray_GetElement",                klplat_arr_element},
    {"ovr_PurchaseArray_HasNextPage",               klplat_purch_has_next},
    {"ovr_PurchaseArray_GetNextUrl",                klplat_purch_next_url},
};

static const struct { const char *name; void *fn; } g_plat_impl[] = {
    {"JNI_OnLoad",                (void *)klplat_JNI_OnLoad},
    {"ovr_IsPlatformInitialized", (void *)klplat_IsPlatformInitialized},
    {"ovr_GetLoggedInUserID",     (void *)klplat_GetLoggedInUserID},
    {"ovr_UnityInitWrapper",      (void *)klplat_UnityInitWrapper},
    {"ovr_PopMessage",            (void *)klplat_PopMessage},
    {"ovr_FreeMessage",           (void *)klplat_FreeMessage},
    {"ovr_Entitlement_GetIsViewerEntitled", (void *)klplat_Entitlement_GetIsViewerEntitled},
    {"ovr_UserAgeCategory_Get",   (void *)klplat_UserAgeCategory_Get},
    {"ovr_Application_GetVersion",(void *)klplat_Application_GetVersion},
    {"ovr_Message_GetApplicationVersion",      (void *)klplat_Message_GetPayload},
    {"ovr_ApplicationVersion_GetCurrentCode",  (void *)klplat_AppVersion_code},
    {"ovr_ApplicationVersion_GetLatestCode",   (void *)klplat_AppVersion_code},
    {"ovr_ApplicationVersion_GetCurrentName",  (void *)klplat_AppVersion_name},
    {"ovr_ApplicationVersion_GetLatestName",   (void *)klplat_AppVersion_name},
    {"ovr_ApplicationVersion_GetReleaseDate",  (void *)klplat_AppVersion_unknown},
    {"ovr_ApplicationVersion_GetSize",         (void *)klplat_AppVersion_unknown},
    // The message accessors. Each takes the handle in x0, so they cannot go
    // through a named trampoline — see the note above klplat_Message_GetType.
    {"ovr_Message_GetType",       (void *)klplat_Message_GetType},
    // "The native message" is this message — the SDK keeps the handle around to
    // hand back to callers that want the raw pointer, and there is nothing
    // behind ours that it is not already holding.
    {"ovr_Message_GetNativeMessage", (void *)klplat_Message_GetPayload},
    {"ovr_Message_IsError",       (void *)klplat_Message_IsError},
    {"ovr_Message_GetRequestID",  (void *)klplat_Message_GetRequestID},
    {"ovr_Message_GetPlatformInitialize",      (void *)klplat_Message_GetPayload},
    {"ovr_PlatformInitialize_GetResult",       (void *)klplat_payload_int},
    {"ovr_Message_GetUserAccountAgeCategory",  (void *)klplat_Message_GetPayload},
    {"ovr_UserAccountAgeCategory_GetAgeCategory", (void *)klplat_payload_int},
    // The string payload, and the one request that carries one.
    {"ovr_Message_GetString",     (void *)klplat_Message_GetString},
    {"ovr_CloudStorage2_GetUserDirectoryPath",
                                  (void *)klplat_CloudStorage2_GetUserDirectoryPath},
    {"ovr_ApplicationLifecycle_GetLaunchDetails", (void *)klplat_GetLaunchDetails},
    // Takes the enum value in w0, so it cannot go through a named trampoline
    // either — see klplat_LaunchType_ToString.
    {"ovrLaunchType_ToString",    (void *)klplat_LaunchType_ToString},
};
static const char g_plat_handle[] = "klepton-ovrplatformloader";

int kl_ovrplat_claims(const char *soname) {
    if (!soname) return 0;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    return strcmp(b, "libovrplatformloader.so") == 0 ||
           strcmp(b, "ovrplatformloader") == 0;
}

void *kl_ovrplat_dlopen(const char *soname) {
    if (!kl_ovrplat_claims(soname)) return NULL;
    const char *b = strrchr(soname, '/');
    b = b ? b + 1 : soname;
    fprintf(stderr, "  [plat] guest dlopen(\"%s\") -> synthetic Oculus Platform "
                    "(the real one forwards to com.oculus.horizon, which is not "
                    "here; see kl_ovrplat.h)\n", b);
    return (void *)g_plat_handle;
}

int kl_ovrplat_is_handle(const void *h) { return h == (const void *)g_plat_handle; }

void *kl_ovrplat_sym(const char *name) {
    if (!name) return NULL;
    int s = plat_slot(name);                    // resolved, whatever happens next

    // Classified at *resolve* time as well as call time, so kl_ovrplat_report can
    // show what the guest went looking for even on a run where nothing aborted.
    // Resolving is still only a measurement — the refusal is at the call.
    if (plat_is_drm(name)) {
        if (s >= 0) g_plat[s].drm = 1;
        return kl_named_stub(name, (void *)klplat_drm);
    }
    for (size_t i = 0; i < sizeof g_plat_impl / sizeof g_plat_impl[0]; i++)
        if (strcmp(g_plat_impl[i].name, name) == 0) return g_plat_impl[i].fn;
    for (size_t i = 0; i < sizeof g_plat_enum_impl / sizeof g_plat_enum_impl[0]; i++)
        if (strcmp(g_plat_enum_impl[i].name, name) == 0) return g_plat_enum_impl[i].fn;
    if (plat_is_unity_hook(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_drop(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_options_create(name))
        return kl_named_stub(name, (void *)klplat_options_create);
    if (plat_is_launch_details(name))
        return kl_named_stub(name, (void *)klplat_launch_details);
    if (plat_is_launch_field(name))
        return kl_named_stub(name, (void *)klplat_launch_field);
    if (plat_is_empty_array(name))
        return kl_named_stub(name, (void *)klplat_empty_array);
    if (plat_is_options_sink(name))
        return kl_named_stub(name, (void *)klplat_void);
    if (plat_is_launch_accessor(name))
        return kl_named_stub(name, (void *)klplat_nothing);
    if (plat_is_init_async(name))
        return kl_named_stub(name, (void *)klplat_init_async);
    if (plat_is_init(name))
        return kl_named_stub(name, (void *)klplat_init_fails);
    if (plat_is_absent(name))
        return kl_named_stub(name, (void *)klplat_absent);
    // Requests before accessors: a request name that happens to contain one of
    // the accessor fragments is still a request.
    if (plat_is_request(name))
        return kl_named_stub(name, (void *)klplat_request_fails);
    if (plat_is_model_accessor(name))
        return kl_named_stub(name, (void *)klplat_model_accessor);
    return kl_named_stub(name, (void *)klplat_called);
}

void kl_ovrplat_report(FILE *f) {
    static int done;
    if (done || !g_nplat) return;
    done = 1;
    unsigned called = 0, drm = 0;
    for (unsigned i = 0; i < g_nplat; i++) {
        if (g_plat[i].calls) called++;
        if (g_plat[i].drm)   drm++;
    }
    fprintf(f, "\n=== Oculus Platform surface ===\n");
    fprintf(f, "  resolved: %u, of which called: %u\n", g_nplat, called);
    if (g_plat_unclassified)
        fprintf(f, "  %u name%s answered 0 from the UNCLASSIFIED tier — each one is "
                   "a line above saying so, and the next round's work list\n",
                g_plat_unclassified, g_plat_unclassified == 1 ? "" : "s");
    if (drm) {
        fprintf(f, "  --- entitlement / ownership (REFUSED by policy) ---\n");
        for (unsigned i = 0; i < g_nplat; i++)
            if (g_plat[i].drm)
                fprintf(f, "    %-52s %s\n", g_plat[i].name,
                        g_plat[i].calls ? "CALLED" : "resolved only");
        fprintf(f, "  (%u DRM call%s refused)\n", g_drm_calls, g_drm_calls == 1 ? "" : "s");
    }
    fprintf(f, "  --- called ---\n");
    for (unsigned i = 0; i < g_nplat; i++)
        if (g_plat[i].calls && !g_plat[i].drm)
            fprintf(f, "    %-52s x%u\n", g_plat[i].name, g_plat[i].calls);
    fprintf(f, "  --- resolved but never called ---\n");
    for (unsigned i = 0; i < g_nplat; i++)
        if (!g_plat[i].calls && !g_plat[i].drm) fprintf(f, "    %s\n", g_plat[i].name);
}
