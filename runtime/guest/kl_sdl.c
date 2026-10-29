// The SDL2 door. See kl_sdl.h for what it is and why it is its own file.
//
// This body was `sdl_run` inside `mains/m_boot.c` until the device round
// needed it: the visionOS app took the generic libmain.so path for this kind
// and failed at `open .../libmain.so: No such file or directory`, because the
// only transcription of SDLActivity.onCreate lived in the host driver. The
// split is kl_ue4.c's: what the GUEST is lives here, what a RUN is stays with
// the driver (the host's time budget and gap-only mode, the app's guest
// thread).
#include "kl_sdl.h"

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "klepton.h"
#include "kl_jni.h"
#include "kl_egl.h"
#include "kl_opensl.h"
#include "kl_aaudio.h"
#include "kl_ndk.h"
#include "kl_mediandk.h"
#include "kl_openxr.h"
#include "kl_target.h"
#include "kl_glfb.h"

// The Java the door is a transcription of. SDL2's JNI_OnLoad registers its
// natives against this class BY NAME, and the JNI surface has to know which
// class IS the activity when SDL asks for it.
#define SDL_ACTIVITY "org/libsdl/app/SDLActivity"

// The library SDL's own initializers live in — getLibraries()[0] on both
// guests that have come through this door. The ENTRY library is the other
// entry in that array and is per-target (kl_target.h's entry_lib), because
// SDLActivity subclasses override getLibraries()/getMainFunction() and these
// two do it differently: hl2vr's ValveActivity2 answers {"SDL2","launcher"} /
// "LauncherMainAndroid", and Simpsons Hit & Run's SimpsonsActivity overrides
// neither, so it takes stock SDL's {"SDL2","main"} / "SDL_main".
#define SDL_LIB  "libSDL2.so"

// SDL's own default, for a row that states no `sdl_main`.
#define SDL_MAIN_DEFAULT "SDL_main"

// The game's one Java class of its own, whose two natives are the guest's
// exports rather than anything registered.
#define VALVE_ACTIVITY "com/valvesoftware/ValveActivity2"

// SDL's own HID manager class. The activity constructs one in onCreate and its
// constructor is the ONLY caller of the native that hands libSDL2's hidapi
// backend the JavaVM — see kl_sdl_begin.
#define HID_MANAGER "org/libsdl/app/HIDDeviceManager"

// Which of this door's guests is running, for the half of the boot that is
// srceng's alone: the ValveActivity2 environment (VALVE_GAME_PATH and friends),
// the setArgs command line, and the two cfg pins that shape its renderer.
//
// Keyed on the ENTRY LIBRARY rather than on the target name, because that is
// the thing the behaviour actually belongs to — ValveActivity2's natives are
// liblauncher.so's exports, and any future srceng row (a second Source game on
// the same port) would carry the same library and want the same environment,
// while a target name would have to be listed one at a time. The second SDL
// guest (simpsons) has no Java of its own beyond a permission prompt, so all of
// it is skipped there; without this gate a Simpsons run would poke Source's
// environment, write hl2vr's SharedPreferences into ITS userdata directory, and
// report two cfg files missing that were never supposed to be there.
static int sdl_is_srceng(void) {
    const kl_target *t = kl_target_current();
    return t && t->entry_lib && !strcmp(t->entry_lib, "liblauncher");
}

enum { SDL_MAX_LIBS = 64 };

static char      g_libdir[1024];
static char      g_err[512] = "no error";
static char      g_entry_lib[64], g_entry_fn[64];
static kl_image *g_imgs[SDL_MAX_LIBS];
static char      g_names[SDL_MAX_LIBS][64];
static unsigned  g_nimgs;

// me.nillerusr.ExtractAssets, transcribed. The APK's Java copies its bundled
// assets out of the APK into getFilesDir() on first run — the seven font files
// by name (DroidSansFallback.ttf, LiberationMono-Regular.ttf, the four
// dejavusans faces, Itim-Regular.otf) and extras_dir.vpk — and nothing else
// puts them there. We do not run that Java, so the directory the engine then
// reads them out of has always been EMPTY, on the host exactly as on the
// device: launch #5's engine.log carries 1,144 `Failed to load custom font
// file '<filesdir>/files/dejavusans.ttf'` and a host run reproduces the count.
//
// The whole asset directory is copied rather than the smali's seven names: the
// list is a property of one build of one APK, a missing file is the failure
// this exists to end, and the tree is six fonts and a VPK. Existing files are
// left alone — the copy is first-run, and a staged tree a human corrected must
// not be overwritten on every launch.
static int sdl_copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return 0;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[64 * 1024];
    size_t n, ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    if (ferror(in)) ok = 0;
    if (fclose(out) != 0) ok = 0;
    fclose(in);
    if (!ok) remove(to);
    return (int)ok;
}

// ---------- Simpsons Hit & Run: the settings file, not the setter ----------
//
// SharOpenXR's setters are NOT setters. Every one of them -- SetSeatedMode,
// SetSnapTurnEnabled, SetRenderScale, all of them -- writes the byte and then
// tail-calls a common epilogue that reopens
//
//   SDL_GetPrefPath("c4rlox", "simpsons") + "vrsettings.cfg"
//
// in "wb" and fprintf()s the WHOLE settings block back out:
//
//   seated=%d snap=%d smooth=%.1f angle=%.1f csm=%d enhancedMaterials=%d
//   gtao=%d renderScale=%.3f refreshRate=%.0f vrSteeringWheel=%d
//   vehicleLights=%d spatialHud=%d developerMenus=%d
//
// Round 10 called SetRenderScale(2.0) from this door, which runs BEFORE
// SDL_main and therefore before SharOpenXR::Initialize() has installed its
// defaults (smooth 120 deg/s, angle 45 deg, csm/gtao/enhancedMaterials/
// spatialHud all on, refreshRate 72) or read the file. So the block it saved
// was zeroed .bss with one real number in it, and Initialize -- which sets its
// defaults and THEN parses the file over them -- read that back and believed
// it. The device's copy after launch #9, pulled verbatim:
//
//   seated=0 snap=0 smooth=0.0 angle=0.0 csm=0 enhancedMaterials=0 gtao=0
//   renderScale=2.000 refreshRate=72 vrSteeringWheel=0 vehicleLights=0
//   spatialHud=0 developerMenus=0
//
// smooth=0 and snap=0 is exactly a right stick that cannot turn:
// FirstPersonCam::Update reads the stick, clamps it, applies a 0.15 deadzone,
// and then multiplies it by GetSmoothTurnSpeed() (or, with snap off, does not
// take the snap branch at all). Zero times a stick is zero. The third-person
// camera orbits from a different path and kept working, which is precisely
// what Austin reported. csm=0 is why the world lost its shadows in the same
// launch, and it is why the round-10 frame capture -- armed on the first
// depth-only framebuffer -- never fired.
//
// So: never call a SharOpenXR setter from here. Write the file the guest is
// about to read, instead, in its own format, and repair a block this door
// damaged. That is strictly MORE reliable than the setter was, because
// Initialize's own default pass would have overwritten an in-memory scale
// anyway -- launch #9 got 200% from the FILE, not from the call.
static void sdl_shar_vrsettings(FILE *out, float scale) {
    const char *files = kl_jni_files_dir();
    if (!files || !*files) return;
    char dir[1024], path[1200];
    snprintf(dir, sizeof dir, "%s/files", files);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/vrsettings.cfg", dir);

    // SharOpenXR::Initialize's own defaults, read off its prologue: a
    // movi #1 across csm/enhancedMaterials/gtao/spatialHud, a zeroed
    // developerMenus, {0,1} into vrSteeringWheel/vehicleLights, the vector
    // {120.0, 45.0, 1.0, 1.0} into smooth/angle/renderScale, and 72.0 into
    // refreshRate. seated and snap are left as they are (both off).
    int seated = 0, snap = 0, csm = 1, materials = 1, gtao = 1;
    int wheel = 0, lights = 1, hud = 1, devmenus = 0;
    float smooth = 120.0f, angle = 45.0f, refresh = 72.0f, rs = 1.0f;

    int had = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        had = 1;
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char *k = line, *v = eq + 1;
            if      (!strcmp(k, "seated"))            seated    = atoi(v);
            else if (!strcmp(k, "snap"))              snap      = atoi(v);
            else if (!strcmp(k, "smooth"))            smooth    = (float)atof(v);
            else if (!strcmp(k, "angle"))             angle     = (float)atof(v);
            else if (!strcmp(k, "csm"))               csm       = atoi(v);
            else if (!strcmp(k, "enhancedMaterials")) materials = atoi(v);
            else if (!strcmp(k, "gtao"))              gtao      = atoi(v);
            else if (!strcmp(k, "renderScale"))       rs        = (float)atof(v);
            else if (!strcmp(k, "refreshRate"))       refresh   = (float)atof(v);
            else if (!strcmp(k, "vrSteeringWheel"))   wheel     = atoi(v);
            else if (!strcmp(k, "vehicleLights"))     lights    = atoi(v);
            else if (!strcmp(k, "spatialHud"))        hud       = atoi(v);
            else if (!strcmp(k, "developerMenus"))    devmenus  = atoi(v);
        }
        fclose(f);
    }

    // The damage signature, and it cannot be a setting Austin chose: the pause
    // menu's turn rows are clamped well above zero and Initialize's defaults
    // are 120/45, so a file carrying smooth=0 AND angle=0 was written by a
    // caller that ran before Initialize -- which is only ever this door.
    // Everything in such a file is zeroed .bss, so the whole block goes back to
    // the guest's defaults rather than half of it.
    int repaired = 0;
    if (had && smooth <= 0.0f && angle <= 0.0f) {
        seated = 0; snap = 0; csm = 1; materials = 1; gtao = 1;
        wheel = 0; lights = 1; hud = 1; devmenus = 0;
        smooth = 120.0f; angle = 45.0f; refresh = 72.0f;
        repaired = 1;
    }
    if (scale > 0.0f) rs = scale;

    f = fopen(path, "wb");
    if (!f) {
        if (out) fprintf(out, "  [sdl] vrsettings.cfg: cannot write %s\n", path);
        return;
    }
    fprintf(f, "seated=%d\nsnap=%d\nsmooth=%.1f\nangle=%.1f\ncsm=%d\n"
               "enhancedMaterials=%d\ngtao=%d\nrenderScale=%.3f\n"
               "refreshRate=%.0f\nvrSteeringWheel=%d\nvehicleLights=%d\n"
               "spatialHud=%d\ndeveloperMenus=%d\n",
            seated, snap, (double)smooth, (double)angle, csm, materials, gtao,
            (double)rs, (double)refresh, wheel, lights, hud, devmenus);
    fclose(f);
    if (out) {
        fprintf(out, "  [sdl] vrsettings.cfg %s: renderScale=%.3f, smooth=%.1f, "
                     "snap=%d/%.1f, csm=%d, gtao=%d, materials=%d, hud=%d%s\n",
                had ? "updated" : "created", (double)rs, (double)smooth, snap,
                (double)angle, csm, gtao, materials, hud,
                repaired ? " — REPAIRED: the block on disk was the all-zero one "
                           "a pre-Initialize setter wrote, which is what stopped "
                           "the right stick turning" : "");
    }
}

// ---------- Simpsons Hit & Run: the Kwik-E-Mart interior, and who culls it ---
//
// Standing INSIDE the Kwik-E-Mart the interior draws NOTHING: the eye holds
// tView's clear colour (0xFF9AC2DE, the sky blue) and one floating slab of
// exterior Springfield up in the air, while VR PERF reports 632-742 draws and
// 205k-224k triangles a frame. The world is submitted and lost, not culled
// away, so the question is which camera those draws were measured against.
//
// Read out of libmain.so, and it is a two-pass loop nobody outside SHAR would
// guess. `WorldRenderLayer::Render` (0x5b8038) walks its views, and for each
// one runs a pass counter seeded from `this+0x1d8` -- the byte
// `InteriorManager::SwitchToInterior` sets to 1 along with an interior->exterior
// 4x4 at `this+0x1dc`. Indoors that is two passes per view, 1 then 0:
//
//   pass 1 (0x5b8134): V = the layer's OWN tPointCamera for this view
//                      (RenderLayer::SetUpViewCam news one per view and calls
//                      tView::SetCamera with it, 0x5b4c90-0x5b4cbc -- it is
//                      NOT the SuperCam's camera). Its world matrix is
//                      replaced by `Matrix::Mult(V.world, layer[0x1dc])`, the
//                      clear mask is -1 (full clear, hence the sky), and the
//                      EXTERIOR backdrop is drawn from out in the world.
//   pass 0 (0x5b818c): V is restored (0x5b89f4, on `pass-1 == 0`), the clear
//                      mask is 2 (depth only), and the interior is drawn on
//                      top.
//
// The pass number is then handed to `WorldScene::Render(unsigned)`
// (0x5b8678: `mov w1, w23`), and THAT is where the VR port and the interior
// collide. The argument is not a player index, whatever it looks like:
//
//   Render(0)  -> SuperCamManager::GetSCC(0)'s camera [scc+0x140], then
//                 SharOpenXR::GetActiveCullingCamera (0x594ca4) OVERWRITES that
//                 camera's world matrix with the headset matrix (head x the VR
//                 base, GetLatestCullingCamera 0x501da4), MarkCameraVisible(cam,
//                 1), RenderScene(1, cam), and restores the saved matrix.
//   Render(1)  -> SuperCamManager::GetSCC(**1**)'s camera, MarkCameraVisible(
//                 cam, 2), RenderScene(2, cam) -- and NO headset matrix
//                 anywhere. GetSCC is `manager + i*728, i<4` (0x4d264c), so
//                 SCC 1 is a real object rather than a null, which is why this
//                 renders a slab instead of crashing.
//
// So indoors the backdrop pass draws from a head-tracked VIEW camera (V x the
// interior transform) while culling against a camera the headset never
// touched. Culling camera and view camera disagree by exactly the head pose,
// the visible set is whatever SCC 1 happened to be looking at, and what
// survives it is one slab of Springfield hanging in the sky. That is the half
// of the screenshot that IS drawn.
//
// The fix is to make the two agree, in the space the pass renders in: compose
// the headset matrix with `layer[0x1dc]` exactly as the layer composes the view
// camera -- `Mult(head, interior->exterior)` , same operand order as 0x5b8168 --
// and install THAT on SCC 1's camera for the duration of pass 1, restoring it
// afterwards the way pass 0 restores its own.
//
// It is done by INTERPOSING the guest symbol rather than patching its image.
// Every one of these calls is a PLT call bound through a JUMP_SLOT relocation
// (`_ZN10WorldScene6RenderEj` at 0x81f3d0, and there is exactly ONE call site
// in the whole library), so kl_interpose reaches it on the host and on device
// alike, and the host half can call back into the guest by name -- which is
// what makes the LOG possible at all. A kl_gpatch row can rewrite an
// instruction; it cannot print a matrix.
//
// The log is the point as much as the fix, because the other half of the
// screenshot is NOT explained yet: pass 0 is already head-corrected and still
// draws nothing visible. Whether its interior geometry lives in a space the
// headset matrix never reaches is a question four matrices answer, and they
// are printed here per sampled frame: the interior->exterior transform, SCC 0's
// camera, SCC 1's camera, the layer's view camera, and the headset matrix
// itself.
//
// KL_SHAR_INTERIOR=0 removes the whole thing (no interposer installed);
// KL_SHAR_INTERIOR_FIX=0 keeps the measurement and leaves the guest's
// behaviour exactly as it was, which is the A/B for the fix alone.
//
// ---- ROUND 19: launch #18 printed the arm line and then nothing at all ----
//
// Austin walked around inside the Kwik-E-Mart for a whole launch and not one
// `interior ENTERED`, not one frame sample, ever printed. The interposer was
// installed and every symbol resolved, so the only way to read that silence is
// that `layer[0x1d8]` was zero the whole time. Back to the disassembly, and
// this time all the way into `InteriorManager::SwitchToInterior` (0x4f42ac)
// instead of trusting session 02's note about it:
//
//   4f42e8  MakeUID("moe1");  if (this->uid[0x18] == it) {
//   4f4318      RenderManager::GetInstance()->mpLayer(2)
//   4f4334      layer[0x1d8] = 1; layer[0x1dc..0x21c] = <a baked 4x4>
//           }
//   4f436c  MakeUID("bartroom");  if (this->uid[0x18] == it) {
//   4f439c      RenderManager::GetInstance()->mpLayer(2)
//   4f43b8      layer[0x1d8] = 1; layer[0x1dc..0x21c] = <a second baked 4x4>
//           }
//   4f43d0  SetupLightsAndAnims(); this[0x30] = 1; TriggerEvent(116, 1);
//
// The whole two-pass interior path is a SPECIAL CASE FOR EXACTLY TWO ROOMS:
// Moe's Tavern ("moe1") and Bart's bedroom ("bartroom"). Both matrices are
// compiled-in constants, one per room, guarded by `__cxa_guard` statics. Every
// other interior in the game -- the Kwik-E-Mart included -- runs
// SwitchToInterior, matches neither name, never writes 0x1d8, and is therefore
// drawn by the ORDINARY single-pass world path. `SwitchToExterior` (0x4f26ac)
// clears the same byte on the same `mpLayer(2)` unconditionally.
//
// So round 18's premise does not apply to the room Austin is standing in, and
// the missing door wall is not a backdrop-pass culling bug. The fix half stays
// installed because it is free while the byte is zero (it would still be the
// right fix in Moe's), but round 19 spends its launch on MEASUREMENT that does
// not depend on the premise being true.
//
// Three things are hooked, and all three print unconditionally:
//
//   * `InteriorManager::SwitchToInterior` / `SwitchToExterior` / `Enter` --
//     every call, with the interior UID the manager is switching to and the
//     0x1d8 byte read back AFTER the guest has run. If SwitchToInterior fires
//     in the Kwik-E-Mart and comes back with 0x1d8 still 0, the reading above
//     is confirmed outright and the two-pass theory is closed.
//   * a once-a-second heartbeat from inside the WorldScene::Render interposer,
//     which does NOT ask whether we are indoors first: the layer pointer from
//     BOTH accessors (`pWorldRenderLayer()` and `mpLayer(2)`, which the
//     disassembly says are the same object only if RenderManager's index at
//     +0x6c happens to be 2 -- worth one line to prove rather than assume),
//     the byte at +0x1d8 on each, the view count at +0x54, calls a second, and
//     a histogram of the pass argument.
//
// Read it like this. SwitchToInterior fires and 0x1d8 stays 0 -> the room is
// simply not a two-pass interior, and the door wall is an ordinary-path
// problem (H1). SwitchToInterior fires, 0x1d8 goes to 1 on mpLayer(2) but the
// heartbeat's pWorldRenderLayer pointer differs or reads 0 -> round 18 was
// reading the wrong object (H2). SwitchToInterior never fires at all -> the
// VR port never enters interior mode and the entrance path itself is what to
// chase. The pass histogram is the cross-check: `pass{0:N 1:0}` forever means
// the world only ever renders single-pass no matter what the byte says.
typedef struct { float m[16]; } shar_mat;

static void *(*g_shar_rm_get)(void);
static void *(*g_shar_rm_layer)(void *);
static void *(*g_shar_scm_get)(void);
static void *(*g_shar_scm_scc)(void *, int);
static int   (*g_shar_cull)(shar_mat *);
static void  (*g_shar_mult)(shar_mat *, const shar_mat *, const shar_mat *);
static void  (*g_shar_ws_render)(void *, unsigned);

static void *(*g_shar_rm_mplayer)(void *, int);
static void  (*g_shar_switch_in)(void *);
static void  (*g_shar_switch_out)(void *);
static void  (*g_shar_enter)(void *, void *, void *, void *);

static int      g_shar_state;        // 0 unresolved, 1 armed, -1 refused
static int      g_shar_fix = 1;
static unsigned g_shar_samples;      // taken since the last exterior->interior
static int      g_shar_indoor_prev;
static double   g_shar_last_sample;
static int      g_shar_frame_sample;

// The unconditional heartbeat. Nothing here is gated on being indoors, which
// is the whole lesson of launch #18: a probe that only speaks when the flag it
// is trying to validate is already set cannot tell you the flag is never set.
static double   g_shar_hb_last;
static unsigned g_shar_hb_calls;
static unsigned g_shar_hb_pass[4];   // 0, 1, 2, and "3 or more"

// ---------------------------------------------------------------------------
// Round 22: the zone-streaming ledger (KL_SHAR_ZONE=0 removes it).
//
// The symptom this exists to explain is stateful, not per-frame: around Lard
// Lad / Krusty Burger the world is often simply absent, and driving away and
// back makes chunks of it appear. Frustum culling cannot do that -- it would
// restore the instant you look. SHAR streams the map instead: a ZoneEventLocator
// on the road fires event 0x4d, RenderManager::HandleEvent (0x5b6c6c) reads the
// locator's dump list and load list, hands each dump name to
// WorldRenderLayer::DumpDynaLoad and each load name to
// LoadingManager::AddRequest (0x4f7528), and the world only comes back when
// RenderManager::OnProcessRequestsComplete (0x5b66a8) runs. One request is
// outstanding at a time: LoadingManager::ProcessNextRequest (0x4f7918) returns
// immediately while its busy byte (+0x7d10) is set, and only the completion
// callback clears it. RenderManager keeps its own busy byte (+0x98) with the
// same shape: a zone event that arrives while it is set is pushed onto a queue
// (+0x78/+0x7c) instead of being loaded, and the queue is only drained by a
// completion. So ONE request that never completes silently stops all streaming
// for the rest of the session, and a slow one produces exactly the observed
// hysteresis -- the backlog draining late, as you drive back through.
//
// The good news is that the engine already narrates all of it. Every step in
// that path calls rReleasePrintf, and rReleasePrintf (0x6dce1c) formats into a
// 1024-byte buffer and returns without writing it anywhere -- the retail build
// kept the call sites and dropped the output. Interposing it and forwarding the
// text is therefore a pure read: no behaviour changes, and the ledger we would
// otherwise have to invent is already written in the game's own words.
//
// Reaching it is the same JUMP_SLOT trick as the rest of this block
// (0x818b80), with one extra step: rReleasePrintf is variadic, so the guest's
// arguments arrive AAPCS64-style and need the asm thunk in kl_va_thunks.S to
// become something a host function can read.
//
// 107 call sites in the whole library and almost all of them are one-shot, but
// a couple (IntersectManager's overflow warnings) could in principle repeat per
// frame, so the sink collapses consecutive duplicates and caps the rate.
static int      g_zone_on;
static double   g_zone_t0;
static unsigned g_zone_pending;      // a <<START>> with no << END >> yet
static char     g_zone_pending_name[160];
static double   g_zone_pending_at;
static unsigned g_zone_queued;       // "Driving TOO FAST: Adding to queue."
static double   g_zone_stall_said;
static char     g_zone_prev[256];
static unsigned g_zone_rep;
static double   g_zone_win;
static unsigned g_zone_win_n, g_zone_dropped;
static void shar_dsg_arm(const char *why);    // round 27, defined with the walk
static void shar_zone_watchdog(double now);   // defined with the sink, below
static void shar_zone_prune(double now);      // round 25, defined with it too

// tPointCamera's world matrix, through its own vtable: slot 0x68 answers a
// `Matrix *` (0x5b813c-0x5b8148 reads it and copies 64 bytes out of the
// returned pointer), slot 0xc8 takes one (0x5b816c-0x5b817c writes it back).
// Reading the field directly would be a guess about tCamera's layout; calling
// the accessors is what the guest itself does.
static const shar_mat *shar_cam_world(void *cam) {
    if (!cam) return NULL;
    void **vt = *(void ***)cam;
    const shar_mat *(*get)(void *) = (const shar_mat *(*)(void *))vt[0x68 / 8];
    return get(cam);
}

static void shar_cam_set_world(void *cam, const shar_mat *m) {
    void **vt = *(void ***)cam;
    void (*set)(void *, const shar_mat *) =
        (void (*)(void *, const shar_mat *))vt[0xc8 / 8];
    set(cam, m);
}

static void shar_log_mat(const char *tag, const shar_mat *m) {
    if (!m) { fprintf(stderr, "  [shar] %-9s (none)\n", tag); return; }
    fprintf(stderr, "  [shar] %-9s "
            "[% .3f % .3f % .3f % .3f] [% .3f % .3f % .3f % .3f] "
            "[% .3f % .3f % .3f % .3f] [% .3f % .3f % .3f % .3f]\n", tag,
            (double)m->m[0],  (double)m->m[1],  (double)m->m[2],  (double)m->m[3],
            (double)m->m[4],  (double)m->m[5],  (double)m->m[6],  (double)m->m[7],
            (double)m->m[8],  (double)m->m[9],  (double)m->m[10], (double)m->m[11],
            (double)m->m[12], (double)m->m[13], (double)m->m[14], (double)m->m[15]);
}

static double shar_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Every symbol by NAME, none by address: an address measured against this APK
// is a fingerprint we would have to re-measure, while a name that is missing
// says "not this guest" and disarms with one line instead of corrupting a
// stranger's image. kl_guest_sym_global answers out of the loaded image
// registry and does NOT consult the interposer table, so the real
// WorldScene::Render is reachable even though its GOT slot now points here.
static void shar_resolve(void) {
    // The REAL WorldScene::Render first, unconditionally: this slot is the
    // difference between "disarmed" and "the world is never drawn". Whatever
    // else is missing, the fallback below must still have a function to
    // forward to (Sonnet review, round 18).
    g_shar_ws_render = kl_guest_sym_global("_ZN10WorldScene6RenderEj");
    struct { const char *name; void **fn; } want[] = {
        { "_ZN13RenderManager11GetInstanceEv",        (void **)&g_shar_rm_get },
        { "_ZN13RenderManager17pWorldRenderLayerEv",  (void **)&g_shar_rm_layer },
        { "_ZN15SuperCamManager11GetInstanceEv",      (void **)&g_shar_scm_get },
        { "_ZN15SuperCamManager6GetSCCEi",            (void **)&g_shar_scm_scc },
        { "_ZN10SharOpenXR22GetActiveCullingCameraEPN18RadicalMathLibrary6MatrixE",
                                                      (void **)&g_shar_cull },
        { "_ZN18RadicalMathLibrary6Matrix4MultERKS0_S2_", (void **)&g_shar_mult },
    };
    int missing = !g_shar_ws_render;
    if (missing)
        fprintf(stderr, "  [shar] interior: _ZN10WorldScene6RenderEj is not in "
                        "this guest — probe and fix are OFF\n");
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        *want[i].fn = kl_guest_sym_global(want[i].name);
        if (*want[i].fn) continue;
        fprintf(stderr, "  [shar] interior: %s is not in this guest — "
                        "probe and fix are OFF\n", want[i].name);
        missing = 1;
    }
    if (missing) { g_shar_state = -1; return; }

    // Round 19's extras are OPTIONAL on purpose: a missing one costs a log
    // line, not the arm. mpLayer is the accessor SwitchToInterior itself uses
    // (`GetInstance()->mpLayer(2)`), and it is the one we most want to compare
    // against pWorldRenderLayer.
    g_shar_rm_mplayer = kl_guest_sym_global(
        "_ZN13RenderManager7mpLayerEN11RenderEnums9LayerEnumE");
    if (!g_shar_rm_mplayer)
        fprintf(stderr, "  [shar] interior: RenderManager::mpLayer is missing — "
                        "the heartbeat prints one layer pointer, not two\n");

    g_shar_state = 1;
    fprintf(stderr, "  [shar] interior: armed (fix %s). The two-pass path is a "
                    "special case for \"moe1\" and \"bartroom\" ONLY — every "
                    "other room, Kwik-E-Mart included, should read 0x1d8=0.\n",
            g_shar_fix ? "ON" : "off");
}

// mpLayer(RenderEnums::LayerEnum 2) — the object SwitchToInterior pokes. The
// heartbeat prints it beside pWorldRenderLayer()'s answer so we never again
// have to assume the two agree.
static void *shar_layer2(void) {
    if (!g_shar_rm_mplayer || !g_shar_rm_get) return NULL;
    void *rm = g_shar_rm_get();
    return rm ? g_shar_rm_mplayer(rm, 2) : NULL;
}

static unsigned shar_indoor_byte(void *layer) {
    return layer ? *(unsigned char *)((char *)layer + 0x1d8) : 0xFFu;
}

// One line, from wherever the interior machinery was just touched: both layer
// pointers, both 0x1d8 bytes. `--` in the byte column means the pointer was
// null, which is itself an answer.
static void shar_log_layers(const char *tag) {
    void *rm    = g_shar_rm_get ? g_shar_rm_get() : NULL;
    void *wrl   = (rm && g_shar_rm_layer) ? g_shar_rm_layer(rm) : NULL;
    void *l2    = shar_layer2();
    unsigned bw = shar_indoor_byte(wrl), b2 = shar_indoor_byte(l2);
    fprintf(stderr, "  [shar] %-18s wrl %p 0x1d8=%s%u  mpLayer2 %p 0x1d8=%s%u"
                    "  same=%s\n", tag,
            wrl, bw == 0xFFu ? "--" : "", bw == 0xFFu ? 0 : bw,
            l2,  b2 == 0xFFu ? "--" : "", b2 == 0xFFu ? 0 : b2,
            (wrl && wrl == l2) ? "yes" : "NO");
    fflush(stderr);
}

// The interior UID the manager is currently switched to: `this + 0x18`, eight
// bytes, written by `str d0, [x19, #0x18]` on the way in and `str xzr` on the
// way out. We print it raw rather than trying to name it — tName::MakeUID
// returns indirectly through x8 and is not worth calling from here — because
// the byte read back after the call already says whether the guest recognised
// it as one of its two special rooms.
// The three InteriorManager hooks can fire before the first frame, so they
// resolve their own forward pointers rather than leaning on shar_resolve().
static void shar_resolve_interior_mgr(void) {
    static int done;
    if (done) return;
    done = 1;
    g_shar_switch_in  = kl_guest_sym_global("_ZN15InteriorManager16SwitchToInteriorEv");
    g_shar_switch_out = kl_guest_sym_global("_ZN15InteriorManager16SwitchToExteriorEv");
    g_shar_enter      = kl_guest_sym_global(
        "_ZN15InteriorManager5EnterEP23InteriorEntranceLocatorP9CharacterP9Sequencer");
    if (!g_shar_switch_in || !g_shar_switch_out || !g_shar_enter)
        fprintf(stderr, "  [shar] interior: an InteriorManager symbol is missing "
                        "— that hook forwards to nothing (in %p out %p enter %p)\n",
                (void *)g_shar_switch_in, (void *)g_shar_switch_out,
                (void *)g_shar_enter);
}

static void shar_switch_to_interior(void *self) {
    shar_resolve_interior_mgr();
    if (!g_shar_state) shar_resolve();
    unsigned long long before = self ? *(unsigned long long *)((char *)self + 0x10) : 0;
    if (g_shar_switch_in) g_shar_switch_in(self);
    unsigned long long uid = self ? *(unsigned long long *)((char *)self + 0x18) : 0;
    void *l2 = shar_layer2();
    unsigned b = shar_indoor_byte(l2);
    fprintf(stderr, "  [shar] SwitchToInterior this %p  pending %016llx  "
                    "current %016llx  ->  %s\n", self, before, uid,
            (b == 0xFFu) ? "layer unreadable"
                         : (b ? "0x1d8=1 — two-pass interior (moe1/bartroom)"
                              : "0x1d8=0 — ORDINARY single-pass room"));
    shar_log_layers("after SwitchIn");
}

static void shar_switch_to_exterior(void *self) {
    shar_resolve_interior_mgr();
    if (!g_shar_state) shar_resolve();
    if (g_shar_switch_out) g_shar_switch_out(self);
    fprintf(stderr, "  [shar] SwitchToExterior this %p\n", self);
    shar_log_layers("after SwitchOut");
}

// ActionButton::EnterInterior::OnButtonPressed is the only caller: this is the
// player pressing the button on a doorway. It fires BEFORE the switch (it just
// queues sequencer actions 110/111), so it timestamps the entry attempt even
// if SwitchToInterior never follows.
static void shar_interior_enter(void *self, void *locator, void *ch, void *seq) {
    shar_resolve_interior_mgr();
    unsigned long long pending = self ? *(unsigned long long *)((char *)self + 0x10) : 0;
    fprintf(stderr, "  [shar] InteriorManager::Enter this %p  locator %p  "
                    "pending uid %016llx\n", self, locator, pending);
    fflush(stderr);
    // ROUND 26. This is the ONLY moment in the whole run that says "the player
    // is going indoors", and it is the moment both of round 26's instruments
    // want: the PNG of the eye framebuffer and the ordered frame trace are
    // useless taken on the front end or out in Springfield. kl_glfb's own arm
    // (the first shadow pass) fires minutes earlier here, so the door hands it
    // this one instead. Inert unless KL_GLFB_OUT_ARM_INTERIOR is set; the
    // delay/spacing/budget the capture already has start counting from here,
    // which is what puts the picture ~3 s after the doorway rather than inside
    // the entrance sequence.
    kl_glfb_arm_capture();
    shar_dsg_arm("InteriorManager::Enter");
    if (g_shar_enter) g_shar_enter(self, locator, ch, seq);
}

// ---------------------------------------------------------------------------
// ROUND 20 -- the ordinary path's culler, measured.
//
// Launch #19 closed the two-pass theory: the Kwik-E-Mart reads 0x1d8=0 and is
// drawn by `WorldScene::Render(0)` like any other room. What is missing in
// there is the wall the exit door sits in, and the sky. So this round reads
// the ordinary path all the way down, and measures the two places it can drop
// a wall.
//
// `WorldScene::Render(0)` (0x594bc8) does, in order:
//
//   594c70  cam = SuperCamManager::GetSCC(0)->camera        [scc + 0x140]
//   594c8c  save = cam->GetWorldMatrix()                    [vtable + 0x68]
//   594ca4  ok = SharOpenXR::GetActiveCullingCamera(&head)
//   594cc0  if (ok) cam->SetWorldMatrix(head)               [vtable + 0xc8]
//   594cc4  if (this[0xa0]) { mark EVERY section visible }
//           else            MarkCameraVisible(cam, 1)
//   594da0  RenderScene(1, cam)
//   594da8  if (ok) cam->SetWorldMatrix(save)
//
// so culling and drawing share ONE camera object and the overwrite covers
// both -- the "two cameras in two spaces" shape does not apply here. And the
// overwrite really is world space: GetActiveCullingCamera (0x5068ec) refuses
// unless an eye is active (`0x83b4a8` is BeginEye's eye+1) and otherwise tail
// calls GetLatestCullingCamera (0x501da4), which averages the two eye
// positions, rotates the result by the VR base heading and finishes with
// `Matrix::Mult(out, head, base)`. Head pose composed with the VR base is the
// player's world transform. Nothing there is obviously wrong, which is
// exactly why this round measures instead of guessing.
//
// Stage one, `MarkCameraVisible` (0x594de4). It reads TWO rows out of the
// camera's world matrix and nothing else -- row 2 (0x20,0x24,0x28) as the
// forward vector and row 3 (0x30,0x34,0x38) as the position -- then splits on
// SharOpenXR::IsVrModeEnabled:
//
//   VR    (0x594e6c): forward.y is forced to 0 and renormalised, and the cone
//                     is written as sin=1, cos=0 -- a 90-degree half angle,
//                     i.e. the horizontal half space in front of the head.
//   flat  (0x594f18): the half angle is the camera's own FOV / 1.87.
//
// It then marks the spatial tree against a SPHERE, not the cone: centre
// `pos + fwd * (far/2)`, radius `far * 0.5199`, where `far` is this[0x9c] --
// 200.0 from the constructor, or whatever event 0x31 last carried
// (HandleEvent, 0x591c50). That sphere reaches 200 units ahead and only about
// 4 units BEHIND the head. A section more than ~4 units behind the culling
// camera is not in the visible list at all.
//
// Stage two, `IsSphereInCone` (0x591af8), called five times over from
// RenderScene (0x593bc4, 0x593d60, 0x593ef8, 0x594090, 0x594284) on each
// candidate's bounding sphere. With the VR cone (sin=1) the arithmetic
// reduces to `dot(dir, centre - pos) > -radius`: the same half space, per
// object. Small spheres (radius < 0.85) get an extra distance cull at 63.2
// units. this[0xa0] short circuits the whole function to "visible".
//
// Both stages therefore hang on ONE pair of numbers: the culling camera's
// position and its flattened forward. If those track the headset, neither
// stage can drop a wall the player is looking straight at, and the loss is
// downstream in the draw. If they do not, they say so in one line.
//
// The sky gets its own datum. WorldRenderLayer::Render draws the backdrop
// list -- `layer[0x68]`, `layer[0x64]` entries, WorldSphereDSG among them --
// at 0x5b85fc, guarded by the SAME 0x1d8 byte that launch #19 proved is zero,
// so on this path the sky IS being asked for. `layer[0x64]` says whether
// anything is registered to ask for. WorldSphereDSG::Display (0x5a3e1c)
// centres the dome on `tContext::GetView()->camera`'s position (vtable 0xc0),
// which is a different camera from the one above -- but it is reached through
// a vtable, not a JUMP_SLOT, so it cannot be interposed and the count is what
// this round can honestly get.
//
// Read the three new lines like this:
//
//   [shar] cullcam ... camP tracks your head as you walk, camF as you turn
//     -> the culler's inputs are right; stages one and two are exonerated and
//        round 21 goes to the draw (WorldSphereDSG's camera, interior geometry
//        activation). Expect conehist rej to be small and maxrej to be behind
//        you.
//   camP frozen, or nowhere near the room, or camF not following your gaze
//     -> the culler is looking somewhere the player is not, the half space is
//        cutting the wrong half, and the fix is to correct what
//        GetActiveCullingCamera hands back (or to feed MarkCameraVisible the
//        eye camera instead).
//   [shar] conehist rej large AND maxrej r big and IN FRONT of camP
//     -> stage two is eating the wall by itself; the cone fields printed on
//        the cullout line say which term did it.
//   [shar] cullout sect vis << total
//     -> stage one's sphere is the one dropping sections; compare far.
//   [shar] cullout bkdrop 0
//     -> the sky is missing because nothing is registered to draw it, and the
//        WorldSphere lead moves to level load, not to culling.
//
// Nothing here changes behaviour. Every hook forwards, and every printed
// number is the guest's own.
static void  (*g_shar_markvis)(void *, void *, unsigned);
static int   (*g_shar_incone)(void *, const float *, float);

// ROUND 21 -- the marking volume, and why VR needs a bigger one.
//
// Round 20's answer was `far 200.0` in every one of launch #20's 184 samples,
// outdoors and in. This round read out where that number comes from, and the
// chain is short and complete:
//
//   * `WorldScene::WorldScene` (0x59152c) writes 200.0f to this[0x9c]
//     (0x5915dc, `#0x43480000`) and registers the scene as the ONLY listener
//     of event 0x31 (0x5915d0 -> EventManager::AddListener).
//   * `WorldScene::HandleEvent(0x31, p)` (0x591c50) is the only other writer
//     of 0x9c in the whole binary -- every other `str ..., #0x9c]` in
//     libmain.so lands on a SpatialNode or an unrelated object. It reads
//     `p[0x50]` (must be 0), then `far = p[0x4c] ? (float)(int)p[0x24]
//     : 200.0f`.
//   * The only sender is `EventLocator::OnTrigger` (0x50eaa8), which fires
//     `TriggerEvent(this[0x54], this)` -- the locator IS the payload.
//     `LocatorLoader::LoadObject` (0x4f8208) builds those from the level p3d:
//     locator type 0 -> EventLocator, `data[0] -> [0x54]` (the event id) and,
//     when the locator carries exactly two data words, `data[1] -> [0x24]`
//     (0x4f8bcc). TriggerLocator's ctor leaves [0x4c] zero and [0x50] = -1;
//     the trigger system fills them on entry, [0x50] being the player index.
//
// So `far` is LEVEL DATA: an event-49 locator says "while the player is in
// this volume, draw this far", and leaving it restores 200. No such locator
// fired anywhere in launch #20 -- which is not a VR failure of the trigger
// system (InteriorEntranceLocator, a TriggerLocator on the same path, fired
// fine when Austin walked into the Kwik-E-Mart on launch #19). 200.0 is
// simply the value the flat game runs with too, until a locator says
// otherwise. There is no missing event to replay.
//
// What IS VR-specific is the shape the 200 buys. Both branches of
// `MarkCameraVisible` mark against a sphere centred `pos + fwd*(far/2)`; the
// flat branch gives it radius `far*0.5` and the VR branch `far*0.5199`
// (0x3f051eb8). A sphere of radius 0.5*far centred 0.5*far ahead is TANGENT
// to the camera: its lateral half-width at the player's own position is
// exactly zero. That is correct for a chase camera with a ~50-degree FOV
// looking down its own axis, and it is what the flat game has always used.
// The VR port widened stage two to a 90-degree half space (sin=1, cos=0 at
// 0x594e6c) but left stage one that forward-biased sphere, only nudging the
// radius by 4%: at far=200 the volume reaches 204 units ahead, 4 behind, and
// sqrt(104^2 - 100^2) = 28 units to either side of the head. Stage two then
// accepts everything in the forward half space -- but only from the handful
// of things stage one bothered to mark. Anything beside you is not in the
// list to begin with. That is the shape of Austin's report: whole areas
// unrendered, cars on roads that are not there, walls you can walk into.
//
// One lever moves both: `far`. The volume scales with it -- lateral
// half-width at the head is 0.1424*far, so 1000 buys 142 units of side
// vision and 1020 ahead. 1000.0f is not a guess: it is the engine's own
// default far plane, `tCamera::tCamera` loading near 1.0 / far 1000.0 as one
// 8-byte constant from .rodata 0x34c0b8 (0x693e20). It is the game's own
// statement of how far a camera can see, and the WorldScene default is a
// fifth of it.
//
// So: when the scene is still carrying the untouched constructor default,
// replace it with KL_SHAR_FAR (default 1000.0). The `== 200.0f` test is the
// point -- a level that DOES place an event-49 locator keeps its own number,
// entering and leaving the volume, and only the bootstrap default is
// overridden. KL_SHAR_FAR=0 disables the whole thing and the guest is
// untouched. KL_SHAR_FAR=200 is the A/B.
//
// The cost is honest and worth saying: five times the radius is a much
// bigger marked set, and `sect` on the cullout line is the number to read.
// If launch #21 shows the world back but the frame gone, the trade is
// visible there and a smaller KL_SHAR_FAR is the dial.
static float    g_shar_far = 1000.0f;
static int      g_shar_far_said;

static double   g_shar_cull_last;
static unsigned g_shar_cone_calls;
static unsigned g_shar_cone_rej;
static float    g_shar_cone_maxrej_r;
static float    g_shar_cone_maxrej_c[3];
static unsigned g_shar_cone_infacc;

// Round 25's ride-along. Launch #23 saw the infinite-radius drawable tested
// (and NaN-rejected) in every 5 s window of the Kwik-E-Mart visit; launch #24
// saw it not once -- `inf-acc 0` all launch -- with the wall equally missing
// both times. Two readings that far apart cannot both be "the cone rejected
// it", so the question is whether the wall's drawable reaches the cone at all:
// IsSphereInCone is only asked about the contents of sections stage one
// MARKED, so a window with zero tests anywhere near the wall says the wall's
// section was never marked, and no amount of fixing the cone would have helped.
// The probe is the cheapest thing that answers it: the distance from each
// tested centre to the point launch #23 named, counted per window.
static const float g_shar_wall_p[3] = { 505.45f, -19.91f, -303.31f };
static unsigned g_shar_wall_n;       // tests within 12 units of that point
static unsigned g_shar_wall_rej;     // of those, how many the cone refused
static unsigned g_shar_wall_inf;     // of those, how many carried r = inf
static float    g_shar_wall_minr =  1.0e30f;
static float    g_shar_wall_maxr = -1.0e30f;

// Is this centre the wall's neighbourhood? 12 units covers the whole interior
// shell (the camera stands 7.5 units from the point) without reaching the
// street outside.
static int shar_near_wall(const float *c) {
    float dx = c[0] - g_shar_wall_p[0];
    float dy = c[1] - g_shar_wall_p[1];
    float dz = c[2] - g_shar_wall_p[2];
    return dx * dx + dy * dy + dz * dz <= 144.0f;
}

static void shar_resolve_cull(void);

static void shar_log_vec(const char *tag, const float *v) {
    fprintf(stderr, "  [shar] %-9s (% 9.2f % 9.2f % 9.2f)\n", tag,
            (double)v[0], (double)v[1], (double)v[2]);
}

// ROUND 24 -- the infinite bounding sphere, and the one multiply that turns it
// into a NaN.
//
// Launch #23 printed the same line at every 5 s sample while Austin stood in
// the Kwik-E-Mart: `conehist ... biggest reject r inf at (505.45 -19.91
// -303.31)` -- an object seven and a half units in front of him, with an
// INFINITE bounding radius, rejected by stage two on every frame. An infinite
// radius is not corruption: it is the engine's own "always visible" idiom, and
// every drawable on this path hands its sphere over by value out of storage
// (`tGeometry::GetBoundingSphere` 0x6a0f48 and
// `tCompositeDrawable::GetBoundingSphere` 0x67edc4 are each a 16-byte copy,
// from +0x2c and +0x48; `StaticEntityDSG` 0x5a2b64 tail calls its drawable's).
// Nothing on the way computes it, and libmain.so contains no +inf constant at
// all -- so the inf is level data, written into the p3d's bounding sphere by
// whatever built l1i01, exactly as a "never cull this" marker would be.
//
// The retail culler honours that marker. Ours does not, and the disassembly of
// `IsSphereInCone` (0x591af8) says where, to the instruction, with launch
// #23's own numbers (dir (-0.056, 0, 0.998), pos (497.96,-18.34,-304.61),
// centre (505.45,-19.91,-303.31), r = +inf, 1/sin = 1, sin2 = 1):
//
//   591b44  fcmp s8, #0.85            r = inf -> PL taken, the small-sphere
//   591b4c  b.pl                      63.2-unit distance cull is skipped
//   591b80  fmul s17, s6, s8          s17 = (1/sin) * r      = +inf
//   591b88  fmul s16, s6, s17         s6 = dir.y = 0.0f  ->  0 * inf = NaN
//   591b90  fsub s19, s3, s16         apex.y = pos.y - NaN   = NaN
//   591ba0  fsub s17, s5, s19         d.y    = centre.y - NaN = NaN
//   591bac  fmul s21, s6, s17         dir.y * NaN            = NaN
//   591bb4  fmadd s19, s7, s18, s21   dot poisoned           = NaN
//   591bbc  fcmp s19, #0.0            unordered: N=0, V=1
//   591bc0  b.le -> 591be4            LE is Z==1 || N!=V, so an unordered
//                                     compare TAKES it -> return 0, rejected.
//
// The multiply at 0x591b88 is the whole bug, and it is ours. `dir.y` is
// exactly +0.0f only because `MarkCameraVisible`'s VR branch flattens the
// forward vector (`str wzr, [sp, #0xc]` at 0x594e7c, before the renormalise);
// the flat game's forward has a real y, so its apex is (-inf,-inf,-inf)-ish
// with every component's sign following dir, `d = centre - apex` comes back
// +inf along each axis, the dot is +inf, and an inf-radius sphere sails
// through as intended. Round 20's algebra ("reject iff dot <= -radius") is
// right for finite spheres and says nothing about this one -- IEEE does.
//
// (The second trap that could have fired does not: `0x591bd8 fmul s17, s17,
// cos2` with |d|^2 = inf would be 0*inf if cos2 were truly zero, but the VR
// branch writes 0x2709aded = 1.9e-15 at 0x594eec, not 0 -- the census's
// `cos2 0.000000000` is just %.9f rounding. And stage one is clean: it hands
// section boxes to `SpatialTreeIter::MarkAllSphere` (0x59022c), which never
// looks at a drawable's radius, so no guard is needed there.)
//
// The fix is the rule the data is stating: a sphere whose radius is not a
// finite number is always visible. Answer that before the guest's math ever
// sees it, count the force-accepts, and print them on the census line as
// `inf-acc`. `KL_SHAR_CULL=0` removes the wrapper, and with it the fix -- that
// is the backout.
static int shar_radius_unbounded(float r) {
    // False for NaN as well as for +/-inf and absurd magnitudes: every
    // comparison against a NaN is false, so the negation catches it.
    return !(r > -1.0e30f && r < 1.0e30f);
}

// ---------------------------------------------------------------------------
// ROUND 27 -- the draw list itself, and the silent overflow that can empty it.
//
// Launch #26 closed the drawn-and-lost theory: the interior-armed captures of
// fb1 show the void exactly as the wearer sees it (floor, ceiling, rack, signs
// all present; the SHELL never drawn), and the ordered ftrace shows no wiper --
// clear-to-sky, then ~250 same-state runs into fb1, and nothing after. So the
// shell's drawable is never SUBMITTED, and the question moved upstream of the
// draw entirely.
//
// The disassembly of the walk answers what "upstream" contains, and the answer
// is: almost nothing. `WorldScene::RenderScene` (0x5939c0) is the whole of it.
// It iterates TWO section lists on the scene -- `this+0x08` (count `this+0x0c`,
// the `sect vis` number the census already prints; array `this+0x10`) and
// `this+0x20` (count `this+0x24`, array `this+0x28`), switching from the first
// to the second at 0x594390 -- and for each section it walks FIVE drawable
// arrays hanging off the section node:
//
//     (count, array) = (0x0c,0x10) (0x84,0x88) (0x9c,0xa0) (0x54,0x58)
//                      (0x24,0x28)
//
// For every drawable in every one of those arrays it calls `vt[0x38]`
// (GetBoundingSphere -> centre at +0, radius at +0xc) and then
// `IsSphereInCone` (0x593bc4, 0x593d60, 0x593ef8, 0x594090, 0x594284 -- the
// five sites, one per array). A `false` there is a `continue`; a `true` falls
// into the display path, where `[drawable+0x24]` bit 0 and `vt[0x60]` only
// choose between drawing NOW and deferring onto the z-sort vector at
// `this+0xf0`. There is no activation bit, no LOD gate, no type filter, no
// per-drawable early-out of any kind between the section list and the cone.
//
// That is the round's first finding and it is a negative one: if the shell
// never reaches `IsSphereInCone` -- and on launches #24 and #26 it never did,
// `inf 0` for the whole visit -- then the shell is NOT IN ANY OF THOSE FIVE
// ARRAYS. Nothing else can produce that reading.
//
// The second finding says how a drawable gets into the first array, and it has
// a silent failure mode sitting right in the middle of it.
// `WorldScene::Add(StaticEntityDSG*)` (0x5921b8) tail-calls
// `WorldScene::Place(StaticEntityDSG*)` (0x5921fc), which is short enough to
// read whole:
//
//     5922a4  bl SpatialTreeIter::rSeekNode(proxy, 0)   -> x0 = leaf node
//     5922a8  ldp w9, w8, [x0, #0x8]                    w9 = cap, w8 = count
//     5922ac  cmp w8, w9
//     5922b0  b.lt 5922c4                               room here -> insert
//     5922b4  ldr x0, [x20, #0x40]                      else the scene's
//     5922b8  ldp w9, w8, [x0, #0x8]                    fallback node
//     5922bc  cmp w8, w9
//     5922c0  b.ge 5922d8                               ALSO FULL -> return,
//                                                       having placed nothing
//     5922c4  ldr x9, [x0, #0x10]                       array
//     5922cc  str w10, [x0, #0xc]                       count = count + 1
//     5922d0  str x19, [x9, w8, sxtw #3]                array[count] = entity
//     5922d4  str x0, [x19, #0x30]                      entity->node = node
//
// The branch at 0x5922c0 drops the entity on the floor. No log, no error, no
// retry: a StaticEntityDSG whose leaf and whose fallback node are both at
// capacity is simply never placed, `[entity+0x30]` stays whatever it was, and
// the drawable exists, is loaded, is resident -- and is in no draw list, so
// nothing ever asks the cone about it. That is a per-visit, order-dependent
// outcome that depends on what else already occupies that leaf, which is
// exactly the shape of the #23-vs-#24/#26 discrepancy the round is chasing:
// same code, same zone, different arrival, because the arrival depends on how
// full the node was at the moment the shell was placed.
//
// This round proves or kills that with two read-only probes. Neither changes a
// pointer, a flag, or a draw.
//
//   KL_SHAR_DSGDUMP=N (default 3) -- N armed walks of the very lists
//   RenderScene walks, from `InteriorManager::Enter` (or the indoor
//   transition, whichever comes first): the first at +KL_SHAR_DSGDELAY
//   seconds, then every KL_SHAR_DSGEVERY. Each walk prints one line per
//   drawable within KL_SHAR_DSGR units of the launch-#23 wall point, every
//   drawable with a non-finite radius wherever it is, and a summary with the
//   totals per array. If the shell is in the lists, its inf-radius line is
//   there and the drop is somewhere else; if the summary says the interior's
//   sections hold no such drawable, `Place` is where it went.
//
//   KL_SHAR_ACTLOG=1 (default on) -- `Place(StaticEntityDSG*)` and
//   `Remove(IEntityDSG*)` interposed. Place is checked AFTER it runs, by the
//   only test that cannot lie: is the entity actually in `node->array[0..n)`
//   of the node it now claims? A miss, or a null node, is the 0x5922c0 drop,
//   and it prints with the node's count and capacity. Remove prints when it
//   takes away something near the wall or something with an infinite radius.
//   Counters ride the 5 s census line.
//
// Both walk guest heap structures, so every pointer is range- and
// alignment-checked and every count is sanity-bounded before it is believed;
// a structure that does not look like a structure is skipped and counted, not
// dereferenced. `vt[0x38]` is only ever called on an object the guest itself
// has just made a virtual call on, or one that is sitting in a list the guest
// calls `vt[0x38]` on every frame.
// ---------------------------------------------------------------------------

typedef struct { float c[3]; float r; } shar_sphere;

static int      g_shar_dsg_left;       // armed walks still owed
static double   g_shar_dsg_due;        // when the next one is due
static double   g_shar_dsg_arm_t;      // when the arm happened
static int      g_shar_dsg_n;          // which walk this is
static int      g_shar_dsg_want = 3;   // KL_SHAR_DSGDUMP
static float    g_shar_dsg_delay = 2.0f;
static float    g_shar_dsg_every = 5.0f;
static float    g_shar_dsgr = 25.0f;
static int      g_shar_dsgmax = 400;

// A pointer we are willing to dereference: non-null, plausibly in a 48-bit
// user address space, and 8-aligned like every object on this heap.
static int shar_ptr_ok(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000u && v < (1ull << 47) && (v & 7u) == 0;
}

// A count we are willing to loop on. The biggest marked set seen so far is
// 1572 sections; a section holding a quarter of a million drawables is a
// corrupt read, not a section.
static int shar_count_ok(int n) { return n >= 0 && n <= 262144; }

// The five drawable arrays a section node carries, in RenderScene's own order.
static const struct { unsigned cnt, arr; const char *name; } g_shar_dsg_l[5] = {
    { 0x0c, 0x10, "stat" },   // WorldScene::Place(StaticEntityDSG*) lands here
    { 0x84, 0x88, "l84"  },
    { 0x9c, 0xa0, "l9c"  },
    { 0x54, 0x58, "l54"  },
    { 0x24, 0x28, "l24"  },
};

// GetBoundingSphere through the drawable's own vtable -- the same slot, on the
// same objects, that RenderScene uses once per drawable per frame.
//
// The calling convention is not assumed; it is read off the guest's own call
// sites, which are all identical. Site one (array `stat`, 0x593b8c):
//
//     593b94  sub  x1, x29, #0x20      x1 = &sphere, a 16-byte stack slot
//     593ba8  ldur x0, [x8, #-0x8]     x0 = the drawable (this)
//     593bac  ldr  x8, [x0]            x8 = vtable
//     593bb0  ldr  x8, [x8, #0x38]     x8 = the function POINTER
//     593bb4  blr  x8
//     593bb8  ldur s0, [x29, #-0x14]   radius read back from &sphere + 0xc
//
// and site two (array `l84`, 0x593d28) is the same instruction for instruction
// with only the array offset changed. Three things follow, and they settle the
// question the ABI raises:
//
//   * The out-pointer travels in **x1**, the second integer argument -- this is
//     `void GetBoundingSphere(Sphere&)`, a reference parameter, not a return
//     value. So there is no HFA in V0-V3 to miss.
//   * **x8 holds the callee address at `blr` time**, so x8 is emphatically NOT
//     a hidden indirect-result pointer here. A function returning a 16-byte
//     aggregate by sret would need x8 live across the call; this one cannot,
//     because the caller just overwrote it with the branch target.
//   * The result is read back **from memory** at `&sphere + 0xc`, which is only
//     meaningful if the callee wrote it there.
//
// `void f(void *this, shar_sphere *out)` in C lowers to exactly x0 = this,
// x1 = out, nothing in x8 -- byte for byte the sequence above. So the plain
// call is the correct one and an asm shim would only re-spell it.
//
// The raw-storage alternative was considered and rejected on the same evidence:
// round 24's +0x2c / +0x48 offsets belong to `tGeometry` and
// `tCompositeDrawable`, but the objects in these five arrays are entity nodes
// (`StaticEntityDSG::GetBoundingSphere` at 0x5a2b64 TAIL CALLS its drawable's),
// so a fixed offset would be reading an entity as if it were a drawable. The
// virtual call is what dereferences that indirection correctly.
//
// Belt anyway, because a probe that quietly reports zeros is worse than one
// that reports nothing: the slot is stamped with a sentinel first, and if the
// callee did not overwrite it the caller is told (`r ?`) rather than handed a
// zero it would believe.
#define SHAR_SPHERE_SENTINEL (-1.2345678e30f)

static int shar_dsg_sphere(void *d, shar_sphere *out) {
    if (!shar_ptr_ok(d)) return 0;
    void **vt = *(void ***)d;
    if (!shar_ptr_ok(vt)) return 0;
    void *fn = vt[0x38 / 8];
    if (!shar_ptr_ok(fn)) return 0;
    out->c[0] = out->c[1] = out->c[2] = SHAR_SPHERE_SENTINEL;
    out->r    = SHAR_SPHERE_SENTINEL;
    ((void (*)(void *, shar_sphere *))fn)(d, out);
    // Untouched in all four words: the callee wrote nothing, so the convention
    // is not what the call sites say it is and every number here is a lie.
    if (out->c[0] == SHAR_SPHERE_SENTINEL && out->c[1] == SHAR_SPHERE_SENTINEL &&
        out->c[2] == SHAR_SPHERE_SENTINEL && out->r == SHAR_SPHERE_SENTINEL)
        return 0;
    return 1;
}

static unsigned long long shar_dsg_uid(void *d) {
    unsigned long long uid = 0;
    memcpy(&uid, (const char *)d + 0x28, sizeof uid);
    return uid;
}

static int shar_dsg_near(const float *c) {
    float dx = c[0] - g_shar_wall_p[0];
    float dy = c[1] - g_shar_wall_p[1];
    float dz = c[2] - g_shar_wall_p[2];
    return dx * dx + dy * dy + dz * dz <= g_shar_dsgr * g_shar_dsgr;
}

// The whole walk's item budget. `g_shar_dsgmax` caps LINES; this caps WORK.
// The walk runs on the render thread, and visionOS kills an immersive app that
// misses 2.0 s of drawables -- so a corrupt-but-plausible count (anything up to
// shar_count_ok's 262144 ceiling), multiplied by sections and by five arrays,
// is a silent SIGKILL rather than a bad log line. 65536 visited drawables is
// far more than the ~1572 sections have ever held and is a small fraction of a
// frame's budget; crossing it stops the walk and says so.
static int g_shar_dsgbudget = 65536;

typedef struct {
    unsigned sect, sect_bad;
    unsigned draw[5];
    unsigned near_n, inf_n, bad_n, nosphere;
    unsigned printed, elided;
    unsigned visited;
    int      truncated;
} shar_dsg_tot;

// One section list. `base` is the {?, count@+4, array@+8} header RenderScene
// loads through `this+0x38`.
static void shar_dsg_walk(void *base, const char *tag, shar_dsg_tot *t) {
    if (!shar_ptr_ok(base)) return;
    int    n   = *(const int *)((const char *)base + 4);
    void **arr = *(void ***)((const char *)base + 8);
    if (!shar_count_ok(n) || (n > 0 && !shar_ptr_ok(arr))) {
        fprintf(stderr, "  [shar] dsg  list %s unreadable (n %d arr %p) — "
                        "skipped\n", tag, n, (void *)arr);
        return;
    }
    for (int s = 0; s < n; s++) {
        if (t->truncated) return;
        void *sect = arr[s];
        if (!shar_ptr_ok(sect)) { t->sect_bad++; continue; }
        t->sect++;
        for (int l = 0; l < 5; l++) {
            int    dn = *(const int *)((const char *)sect + g_shar_dsg_l[l].cnt);
            void **da = *(void ***)((const char *)sect + g_shar_dsg_l[l].arr);
            if (!shar_count_ok(dn) || (dn > 0 && !shar_ptr_ok(da))) {
                t->bad_n++;
                continue;
            }
            for (int i = 0; i < dn; i++) {
                if ((int)t->visited >= g_shar_dsgbudget) {
                    t->truncated = 1;
                    return;
                }
                t->visited++;
                void *d = da[i];
                if (!shar_ptr_ok(d)) { t->bad_n++; continue; }
                t->draw[l]++;
                shar_sphere sp;
                // A sphere the callee never wrote is reported, never believed.
                if (!shar_dsg_sphere(d, &sp)) { t->nosphere++; continue; }
                int inf  = shar_radius_unbounded(sp.r);
                int near = shar_dsg_near(sp.c);
                if (inf)  t->inf_n++;
                if (near) t->near_n++;
                if (!inf && !near) continue;
                if ((int)t->printed >= g_shar_dsgmax) { t->elided++; continue; }
                t->printed++;
                fprintf(stderr, "  [shar] dsg  %s s%04d %s[%4d] %p vt %p "
                                "uid %016llx flag %02x  c (% 9.2f % 9.2f "
                                "% 9.2f)  r %.2f%s%s\n",
                        tag, s, g_shar_dsg_l[l].name, i, d, *(void **)d,
                        shar_dsg_uid(d),
                        (unsigned)*(const unsigned char *)((const char *)d + 0x24),
                        (double)sp.c[0], (double)sp.c[1], (double)sp.c[2],
                        (double)sp.r, inf ? "  INF" : "", near ? "  NEAR" : "");
            }
        }
    }
}

// The walk itself, once per arm. Read-only from first line to last.
static void shar_dsg_dump(void *scene, unsigned pass, int indoor) {
    shar_dsg_tot t;
    memset(&t, 0, sizeof t);
    g_shar_dsg_n++;
    fprintf(stderr, "  [shar] dsg  ===== walk %d  t+%.1f s  scene %p  pass %u  "
                    "indoor %d  A n %d  B n %d  (near = within %.1f u of the "
                    "launch-#23 wall point)\n",
            g_shar_dsg_n, shar_now() - g_shar_dsg_arm_t, scene, pass, indoor,
            shar_ptr_ok(scene) ? *(const int *)((const char *)scene + 0x0c) : -1,
            shar_ptr_ok(scene) ? *(const int *)((const char *)scene + 0x24) : -1,
            (double)g_shar_dsgr);
    if (shar_ptr_ok(scene)) {
        shar_dsg_walk((char *)scene + 0x08, "A", &t);
        shar_dsg_walk((char *)scene + 0x20, "B", &t);
    }
    fprintf(stderr, "  [shar] dsg  ===== walk %d done: sections %u (bad %u)  "
                    "drawables stat %u l84 %u l9c %u l54 %u l24 %u  = %u  "
                    "near %u  inf %u  unreadable %u  no-sphere %u  printed %u  "
                    "elided %u  visited %u%s\n",
            g_shar_dsg_n, t.sect, t.sect_bad,
            t.draw[0], t.draw[1], t.draw[2], t.draw[3], t.draw[4],
            t.draw[0] + t.draw[1] + t.draw[2] + t.draw[3] + t.draw[4],
            t.near_n, t.inf_n, t.bad_n, t.nosphere, t.printed, t.elided,
            t.visited,
            t.truncated ? "  *** WALK TRUNCATED AT BUDGET — the totals above "
                          "are a floor, not a census ***" : "");
    fflush(stderr);
}

// Arm: from InteriorManager::Enter, and from the indoor transition, whichever
// reaches here first. Re-arming while walks are still owed is ignored so a
// doorway that fires twice does not double the log.
static void shar_dsg_arm(const char *why) {
    if (g_shar_dsg_want <= 0 || g_shar_dsg_left > 0) return;
    g_shar_dsg_left  = g_shar_dsg_want;
    g_shar_dsg_arm_t = shar_now();
    g_shar_dsg_due   = g_shar_dsg_arm_t + (double)g_shar_dsg_delay;
    g_shar_dsg_n     = 0;
    fprintf(stderr, "  [shar] dsg  armed by %s: %d walk(s), first at +%.1f s, "
                    "then every %.1f s\n",
            why, g_shar_dsg_want, (double)g_shar_dsg_delay,
            (double)g_shar_dsg_every);
    fflush(stderr);
}

static void shar_dsg_maybe_dump(void *scene, unsigned pass, int indoor) {
    if (g_shar_dsg_left <= 0) return;
    double now = shar_now();
    if (now < g_shar_dsg_due) return;
    g_shar_dsg_left--;
    g_shar_dsg_due = now + (double)g_shar_dsg_every;
    shar_dsg_dump(scene, pass, indoor);
}

// --- the activation half -------------------------------------------------

static void (*g_shar_place_se)(void *, void *);
static void (*g_shar_remove_dsg)(void *, void *);
static int      g_shar_act_on;
static int      g_shar_actmax = 200;
static unsigned g_shar_act_place, g_shar_act_drop, g_shar_act_miss;
static unsigned g_shar_act_remove, g_shar_act_lines;

// Is `ent` actually in the node's static-entity array? This is the only
// question that matters and the only one that cannot be answered by a flag.
static int shar_node_holds(void *node, void *ent, int *cnt, int *cap) {
    *cnt = *cap = -1;
    if (!shar_ptr_ok(node)) return 0;
    int c   = *(const int *)((const char *)node + 0x0c);
    int cp  = *(const int *)((const char *)node + 0x08);
    void **a = *(void ***)((const char *)node + 0x10);
    *cnt = c; *cap = cp;
    if (!shar_count_ok(c) || (c > 0 && !shar_ptr_ok(a))) return 0;
    for (int i = 0; i < c; i++) if (a[i] == ent) return 1;
    return 0;
}

static void shar_resolve_act(void);
static int  shar_act_forwardable(void);

static void shar_place_static(void *scene, void *ent) {
    if (!shar_act_forwardable()) return;
    g_shar_place_se(scene, ent);
    if (!g_shar_act_on || !shar_ptr_ok(ent)) return;
    g_shar_act_place++;
    void *node = *(void **)((char *)ent + 0x30);
    int cnt, cap;
    int held = shar_node_holds(node, ent, &cnt, &cap);
    int drop = !shar_ptr_ok(node);
    if (!drop && held) return;            // the ordinary, silent case
    if (drop) g_shar_act_drop++; else g_shar_act_miss++;
    if ((int)g_shar_act_lines >= g_shar_actmax) return;
    g_shar_act_lines++;
    // Place has just made a virtual call on this object, so its vtable is
    // live and the sphere is safe to ask for.
    shar_sphere sp;
    char where[96];
    if (shar_dsg_sphere(ent, &sp))
        snprintf(where, sizeof where, "c (% 9.2f % 9.2f % 9.2f) r %.2f%s",
                 (double)sp.c[0], (double)sp.c[1], (double)sp.c[2],
                 (double)sp.r, shar_radius_unbounded(sp.r) ? "  INF" : "");
    else
        snprintf(where, sizeof where, "c ? r ?  (no sphere)");
    fprintf(stderr, "  [shar] dsgact PLACE %s ent %p uid %016llx node %p "
                    "%d/%d  %s  [0x5922c0 drops silently when leaf AND "
                    "fallback are full]\n",
            drop ? "DROPPED (no node)" : "NOT IN NODE LIST",
            ent, shar_dsg_uid(ent), node, cnt, cap, where);
    fflush(stderr);
}

static void shar_remove_entity(void *scene, void *ent) {
    if (!shar_act_forwardable()) return;
    if (g_shar_act_on && shar_ptr_ok(ent)) {
        g_shar_act_remove++;
        shar_sphere sp;
        if (shar_dsg_sphere(ent, &sp) &&
            (shar_radius_unbounded(sp.r) || shar_dsg_near(sp.c)) &&
            (int)g_shar_act_lines < g_shar_actmax) {
            g_shar_act_lines++;
            fprintf(stderr, "  [shar] dsgact REMOVE ent %p uid %016llx "
                            "node %p  c (% 9.2f % 9.2f % 9.2f) r %.2f%s\n",
                    ent, shar_dsg_uid(ent), *(void **)((char *)ent + 0x30),
                    (double)sp.c[0], (double)sp.c[1], (double)sp.c[2],
                    (double)sp.r, shar_radius_unbounded(sp.r) ? "  INF" : "");
            fflush(stderr);
        }
    }
    g_shar_remove_dsg(scene, ent);
}

static void shar_resolve_act(void) {
    static int done;
    if (done) return;
    done = 1;
    g_shar_place_se   = kl_guest_sym_global("_ZN10WorldScene5PlaceEP15StaticEntityDSG");
    g_shar_remove_dsg = kl_guest_sym_global("_ZN10WorldScene6RemoveEP10IEntityDSG");
    if (!g_shar_place_se)
        fprintf(stderr, "  [shar] dsgact: WorldScene::Place(StaticEntityDSG*) "
                        "is not in this guest — NOTHING WILL BE PLACED; the "
                        "hook forwards to nothing\n");
    if (!g_shar_remove_dsg)
        fprintf(stderr, "  [shar] dsgact: WorldScene::Remove(IEntityDSG*) is "
                        "not in this guest — the hook forwards to nothing\n");
}

// Both of these hooks sit on calls the GAME needs to complete: a Place that
// does not forward loses a static entity for the whole run, and a Remove that
// does not forward leaves a freed entity in a draw list. Neither may ever
// swallow a call silently.
//
// The null case should be unreachable, and the reason is worth stating because
// it is what makes the hooks safe to install at all. kl_image.c's `sym_value`
// substitutes our function ONLY when the relocation's symbol NAME matches the
// row we registered — which means the guest's symbol table contains that exact
// name, which is precisely what `kl_guest_sym_global` searches. If our hook is
// running, the symbol exists; if the symbol did not exist, the relocation would
// never have been rewritten and the hook would never run. The two conditions
// are the same condition.
//
// It is still checked, because "should be unreachable" is how instruments eat
// launches. If it ever is null there is no real function to forward to and
// nothing here can synthesise one, so the only useful behaviour is to say so
// as loudly as the log allows, and to say it on every call rather than once —
// a game quietly losing its world deserves a log that is impossible to miss.
static int shar_act_forwardable(void) {
    shar_resolve_act();
    if (g_shar_place_se && g_shar_remove_dsg) return 1;
    static unsigned said;
    if (said < 64) {
        said++;
        fprintf(stderr, "  [shar] dsgact FATAL: Place %p / Remove %p — one of "
                        "the interposed WorldScene entry points did not "
                        "resolve, so this call CANNOT be forwarded and the "
                        "entity is lost. Relaunch with KL_SHAR_ACTLOG=0; the "
                        "round-27 activation probe is unusable on this "
                        "guest.\n",
                (void *)g_shar_place_se, (void *)g_shar_remove_dsg);
        fflush(stderr);
    }
    return 0;
}

// The per-object verdict. Hot -- five call sites, once per candidate drawable,
// low thousands a frame -- so the body is three counters and a forward.
static int shar_is_sphere_in_cone(void *scene, const float *centre, float radius) {
    shar_resolve_cull();
    int wall = shar_near_wall(centre);
    if (wall) {
        g_shar_wall_n++;
        if (radius < g_shar_wall_minr) g_shar_wall_minr = radius;
        if (radius > g_shar_wall_maxr) g_shar_wall_maxr = radius;
    }
    if (shar_radius_unbounded(radius)) {
        g_shar_cone_calls++;
        g_shar_cone_infacc++;
        if (wall) g_shar_wall_inf++;
        return 1;
    }
    int vis = g_shar_incone ? g_shar_incone(scene, centre, radius) : 1;
    g_shar_cone_calls++;
    if (wall && !vis) g_shar_wall_rej++;
    if (!vis) {
        g_shar_cone_rej++;
        if (radius > g_shar_cone_maxrej_r) {
            g_shar_cone_maxrej_r = radius;
            g_shar_cone_maxrej_c[0] = centre[0];
            g_shar_cone_maxrej_c[1] = centre[1];
            g_shar_cone_maxrej_c[2] = centre[2];
        }
    }
    return vis;
}

// Once a frame, on the ordinary path, and the only place where the culling
// camera is readable AFTER the headset overwrite and BEFORE the restore.
static void shar_mark_camera_visible(void *scene, void *cam, unsigned mask) {
    shar_resolve_cull();
    double now = shar_now();
    // Round 20's question is answered; one line every 5 s is enough to watch
    // the marked-set size move.
    int    say = (g_shar_cull_last == 0.0) || (now - g_shar_cull_last >= 5.0);

    // What stage one is about to read: rows 2 and 3 of the camera's world
    // matrix, through the accessor the guest itself uses.
    const shar_mat *cw = say ? shar_cam_world(cam) : NULL;

    if (g_shar_markvis) g_shar_markvis(scene, cam, mask);

    if (!say) return;
    g_shar_cull_last = now;

    shar_mat head;
    int have_head = g_shar_cull ? g_shar_cull(&head) : 0;

    void *rm    = g_shar_rm_get ? g_shar_rm_get() : NULL;
    void *layer = (rm && g_shar_rm_layer) ? g_shar_rm_layer(rm) : NULL;

    fprintf(stderr, "  [shar] cullcam scene %p  cam %p  mask %u  "
                    "GetActiveCullingCamera %s\n",
            scene, cam, mask, have_head ? "yes" : "NO");
    if (cw) {
        float f[3] = { cw->m[8],  cw->m[9],  cw->m[10] };
        float p[3] = { cw->m[12], cw->m[13], cw->m[14] };
        shar_log_vec("camF", f);          // forward, before the y is flattened
        shar_log_vec("camP", p);          // position -- the cone's apex
    } else {
        fprintf(stderr, "  [shar] camF/camP  (camera has no world matrix)\n");
    }
    if (have_head) {
        float hp[3] = { head.m[12], head.m[13], head.m[14] };
        shar_log_vec("headP", hp);        // must equal camP after the overwrite
    }

    // What stage one wrote, and what stage two will read.
    {
        char *s     = (char *)scene;
        float *dir  = (float *)(s + 0x138);
        float *pos  = (float *)(s + 0x144);
        float far_  = *(float *)(s + 0x9c);
        unsigned by = *(unsigned char *)(s + 0xa0);
        int vis     = *(int *)(s + 0x0c);
        int tot     = *(int *)(s + 0x58);
        int views   = layer ? *(int *)((char *)layer + 0x54) : -1;
        int bkdrop  = layer ? *(int *)((char *)layer + 0x64) : -1;
        void *lws   = layer ? *(void **)((char *)layer + 0x58) : NULL;
        fprintf(stderr, "  [shar] cullout dir (% .3f % .3f % .3f)  "
                        "pos (% 9.2f % 9.2f % 9.2f)  far %.1f  bypass %u  "
                        "sect %d/%d  views %d  bkdrop %d  layerWS %p%s\n",
                (double)dir[0], (double)dir[1], (double)dir[2],
                (double)pos[0], (double)pos[1], (double)pos[2],
                (double)far_, by, vis, tot, views, bkdrop, lws,
                (lws == scene) ? "" : " (NOT this scene)");
        fprintf(stderr, "  [shar] cone    1/sin %.4f  sin2 %.6f  cos2 %.9f\n",
                (double)*(float *)(s + 0x150), (double)*(float *)(s + 0x154),
                (double)*(float *)(s + 0x158));
    }

    fprintf(stderr, "  [shar] conehist calls %u  rejected %u  inf-acc %u  "
                    "biggest reject r %.2f at (% 9.2f % 9.2f % 9.2f)\n",
            g_shar_cone_calls, g_shar_cone_rej, g_shar_cone_infacc,
            (double)g_shar_cone_maxrej_r,
            (double)g_shar_cone_maxrej_c[0], (double)g_shar_cone_maxrej_c[1],
            (double)g_shar_cone_maxrej_c[2]);
    // The wall's own line. `n 0` means the wall's drawable was never offered
    // to the cone in this window -- stage one did not mark its section, and
    // the missing wall is a MARKING problem, not a cone problem. `n` high with
    // `inf` high is launch #23's reading (offered, infinite, now accepted);
    // `n` high with `rej` high and a finite `r` is a third answer again.
    fprintf(stderr, "  [shar] wall     n %u  rej %u  inf %u  r [%.2f .. %.2f]"
                    "  (within 12 u of the launch-#23 point)\n",
            g_shar_wall_n, g_shar_wall_rej, g_shar_wall_inf,
            g_shar_wall_n ? (double)g_shar_wall_minr : 0.0,
            g_shar_wall_n ? (double)g_shar_wall_maxr : 0.0);
    if (g_shar_act_on)
        fprintf(stderr, "  [shar] dsgact place %u  DROPPED %u  not-in-list %u  "
                        "remove %u  (this window; a non-zero DROPPED is the "
                        "0x5922c0 silent overflow)\n",
                g_shar_act_place, g_shar_act_drop, g_shar_act_miss,
                g_shar_act_remove);
    fflush(stderr);
    g_shar_act_place = g_shar_act_drop = g_shar_act_miss = 0;
    g_shar_act_remove = 0;
    g_shar_act_lines = 0;
    g_shar_cone_calls = g_shar_cone_rej = g_shar_cone_infacc = 0;
    g_shar_cone_maxrej_r = 0.0f;
    g_shar_cone_maxrej_c[0] = g_shar_cone_maxrej_c[1] = g_shar_cone_maxrej_c[2] = 0.0f;
    g_shar_wall_n = g_shar_wall_rej = g_shar_wall_inf = 0;
    g_shar_wall_minr =  1.0e30f;
    g_shar_wall_maxr = -1.0e30f;
}

static void shar_resolve_cull(void) {
    static int done;
    if (done) return;
    done = 1;
    g_shar_markvis = kl_guest_sym_global(
        "_ZN10WorldScene17MarkCameraVisibleEP12tPointCameraj");
    g_shar_incone = kl_guest_sym_global(
        "_ZN10WorldScene14IsSphereInConeERN18RadicalMathLibrary6VectorEf");
    if (!g_shar_markvis)
        fprintf(stderr, "  [shar] cull: WorldScene::MarkCameraVisible is not in "
                        "this guest — NOTHING WILL BE MARKED VISIBLE; the hook "
                        "forwards to nothing\n");
    if (!g_shar_incone)
        fprintf(stderr, "  [shar] cull: WorldScene::IsSphereInCone is not in "
                        "this guest — the hook answers \"visible\" for every "
                        "object\n");
}

// The interposer. Called once per pass per view: twice a frame indoors, once
// outdoors.
static void shar_worldscene_render(void *scene, unsigned pass) {
    if (!g_shar_state) shar_resolve();

    // Round 21. Before anything reads it: if this scene is still carrying the
    // constructor's 200.0f, hand it the VR draw distance. Level data wins --
    // an event-49 locator's value is never 200.0f exactly, so it survives.
    if (scene && g_shar_far > 0.0f) {
        float *far_ = (float *)((char *)scene + 0x9c);
        if (*far_ == 200.0f) {
            if (!g_shar_far_said) {
                g_shar_far_said = 1;
                fprintf(stderr, "  [shar] far: WorldScene %p carrying the "
                                "constructor default 200.0 -> %.1f "
                                "(KL_SHAR_FAR; 0 disables, 200 is the A/B)\n",
                        scene, (double)g_shar_far);
                fflush(stderr);
            }
            *far_ = g_shar_far;
        }
    }
    if (g_shar_state != 1) {
        // Nothing resolved: there is no real function to forward to either, so
        // the only honest thing is to do nothing rather than to crash.
        if (g_shar_ws_render) g_shar_ws_render(scene, pass);
        return;
    }

    void *rm    = g_shar_rm_get();
    void *layer = rm ? g_shar_rm_layer(rm) : NULL;

    // Round 25, once a frame and before anything else looks at the world: drop
    // the deferred zone events that have gone cold, so the drain can only ever
    // apply one the player is still standing in.
    shar_zone_prune(shar_now());

    // The heartbeat, before any indoor test. One line a second, whatever the
    // world is doing.
    {
        double now = shar_now();
        g_shar_hb_calls++;
        g_shar_hb_pass[pass < 3 ? pass : 3]++;
        if (g_shar_hb_last == 0.0) g_shar_hb_last = now;
        else if (now - g_shar_hb_last >= 1.0) {
            void *l2 = shar_layer2();
            unsigned bw = shar_indoor_byte(layer), b2 = shar_indoor_byte(l2);
            int views = layer ? *(int *)((char *)layer + 0x54) : -1;
            int bkd   = layer ? *(int *)((char *)layer + 0x64) : -1;
            fprintf(stderr, "  [shar] hb  wrl %p 0x1d8=%s%u  mpLayer2 %p "
                            "0x1d8=%s%u  views %d  bkdrop %d  calls/s %u  "
                            "pass{0:%u 1:%u 2:%u 3+:%u}\n",
                    layer, bw == 0xFFu ? "--" : "", bw == 0xFFu ? 0 : bw,
                    l2,    b2 == 0xFFu ? "--" : "", b2 == 0xFFu ? 0 : b2,
                    views, bkd, g_shar_hb_calls,
                    g_shar_hb_pass[0], g_shar_hb_pass[1],
                    g_shar_hb_pass[2], g_shar_hb_pass[3]);
            fflush(stderr);
            shar_zone_watchdog(now);
            g_shar_hb_last = now;
            g_shar_hb_calls = 0;
            g_shar_hb_pass[0] = g_shar_hb_pass[1] = 0;
            g_shar_hb_pass[2] = g_shar_hb_pass[3] = 0;
        }
    }

    int   indoor = 0;
    const shar_mat *interior = NULL;
    if (layer) {
        indoor   = *(unsigned char *)((char *)layer + 0x1d8) != 0;
        interior = (const shar_mat *)((char *)layer + 0x1dc);
    }

    // Throttle. A sample is a FRAME, not a pass, so both passes of a sampled
    // frame print and the two can be compared side by side. Six frames on
    // entering an interior, then one a second: enough to read, never enough to
    // cost a drawable.
    if (pass >= 1 || !indoor) {
        double now = shar_now();
        if (indoor && !g_shar_indoor_prev) {
            g_shar_samples = 0;
            fprintf(stderr, "  [shar] interior ENTERED (layer %p)\n", layer);
            shar_dsg_arm("the indoor transition");
        }
        if (!indoor && g_shar_indoor_prev)
            fprintf(stderr, "  [shar] interior LEFT\n");
        g_shar_indoor_prev = indoor;
        g_shar_frame_sample = 0;
        if (indoor && (g_shar_samples < 6 || now - g_shar_last_sample >= 1.0)) {
            g_shar_frame_sample = 1;
            g_shar_samples++;
            g_shar_last_sample = now;
        }
    }

    void *scm  = g_shar_scm_get();
    void *scc0 = scm ? g_shar_scm_scc(scm, 0) : NULL;
    void *scc1 = scm ? g_shar_scm_scc(scm, 1) : NULL;
    void *cam0 = scc0 ? *(void **)((char *)scc0 + 0x140) : NULL;
    void *cam1 = scc1 ? *(void **)((char *)scc1 + 0x140) : NULL;

    shar_mat head;
    int have_head = g_shar_cull ? g_shar_cull(&head) : 0;

    if (g_shar_frame_sample) {
        void  *view = (layer && *(int *)((char *)layer + 0x54) > 0)
                    ? *(void **)((char *)layer + 0x10) : NULL;
        void  *vcam = view ? *(void **)((char *)view + 0x20) : NULL;
        fprintf(stderr, "  [shar] frame sample %u  pass %u  indoor %d  "
                        "head %s\n",
                g_shar_samples, pass, indoor, have_head ? "yes" : "NO");
        shar_log_mat("interior", interior);   // layer[0x1dc], interior->exterior
        shar_log_mat("head",     have_head ? &head : NULL);   // the VR camera
        shar_log_mat("scc0",     shar_cam_world(cam0));       // pass 0's culler
        shar_log_mat("scc1",     shar_cam_world(cam1));       // pass 1's culler
        shar_log_mat("view",     shar_cam_world(vcam));       // what is drawn
        fflush(stderr);
    }

    // The fix, and only for the backdrop pass: give the pass its own view's
    // camera to cull against, in the exterior space it draws in.
    if (g_shar_fix && indoor && pass == 1 && have_head && cam1 && interior) {
        const shar_mat *cur = shar_cam_world(cam1);
        shar_mat save, want;
        if (cur) {
            save = *cur;
            g_shar_mult(&want, &head, interior);
            shar_cam_set_world(cam1, &want);
            g_shar_ws_render(scene, pass);
            shar_cam_set_world(cam1, &save);
            shar_dsg_maybe_dump(scene, pass, indoor);
            return;
        }
    }
    g_shar_ws_render(scene, pass);
    shar_dsg_maybe_dump(scene, pass, indoor);
}

// The sink behind the interposed rReleasePrintf. One line per guest message,
// tagged and timestamped from the first one, with the two counters that matter
// carried on every line so the ledger reads without cross-referencing:
//
//   [zone] +12.345 p0 q0 | Encountered Dynamic Zone:
//
// `p` is how many load requests are outstanding (a <<START>> Async Loading with
// no << END >> yet); `q` is how many zone events have been deferred onto
// RenderManager's queue by "Driving TOO FAST". A healthy drive keeps both at 0
// between zones and shows START/END pairs a few hundred ms apart. The signature
// of the bug, if it is this: a `p1` that never returns to 0, `q` climbing, and
// no further ZoneLoadingStart lines for the rest of the drive.
//
// The lines to look for, all the engine's own wording:
//   Encountered Dynamic Zone:            a ZoneEventLocator fired
//   Dump: %s / Load: %s                  what that locator asked for
//   Driving TOO FAST: Adding to queue.   deferred; RenderManager was busy
//   ZoneLoadingStart--===--%s--===--     the request went to LoadingManager
//   <<START>> / << END >> Async Loading  the file layer, with elapsed msecs
//   Zone Loading Ended: %s               the completion callback ran
//   ZoneLoadingEnded Verified            the whole chain is idle again
//   Nothing to load in that Zone...      the zone was already resident
//   ARGH! You're driving TOO FAST!       WorldRenderLayer refused a load slot
//   CurLoadIndex was/is %d               which dynaload slot it used
// and, because the same door catches them, the engine's own out-of-memory and
// heap complaints -- which is the other candidate for a load that never lands.
extern void (*kl_guest_trace_line)(const char *text);

// Round 23: which THREAD said it.
//
// Launch #22's ledger could not answer the one question round 23 turns on.
// "P3D Loading Starved for 3 frames, 1958 ms" is printed by
// tFileFTT::WaitForCompletion (0x6ab090), which spins on
// tLoadManager::SwitchTask; whether that spin is the game thread (in which case
// rendering stops while it waits, and the fix is a budget) or radLoadManager's
// own load thread (in which case rendering does NOT stop, and the fix is
// scheduling) is the difference between two entirely different rounds. The
// launch-#22 log argues for the load thread -- the [xr] layer pairs kept
// arriving 16.7 ms apart straight through a 1958 ms starve -- but that is an
// inference across two log streams, and a `t<n>` on the line is the fact.
//
// First-come indices, not thread ids: t0 is whichever guest thread reaches the
// ledger first (in practice the game thread, which does the boot loads), and
// any t1+ is a thread that is loading concurrently with it. Eight is more than
// this engine has; a ninth prints t? rather than growing a lock.
static pthread_t g_zone_thr[8];
static unsigned  g_zone_thr_n;

static int shar_zone_thread_tag(void) {
    pthread_t self = pthread_self();
    for (unsigned i = 0; i < g_zone_thr_n; i++)
        if (pthread_equal(g_zone_thr[i], self)) return (int)i;
    if (g_zone_thr_n >= 8) return -1;
    g_zone_thr[g_zone_thr_n] = self;
    return (int)g_zone_thr_n++;
}

static void shar_zone_sink(const char *text) {
    if (!text || !*text) return;
    char line[256];
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof line; p++)
        if (*p != '\n' && *p != '\r') line[n++] = *p;
    line[n] = 0;
    while (n && line[n - 1] == ' ') line[--n] = 0;
    if (!n) return;

    double now = shar_now();
    if (g_zone_t0 == 0.0) g_zone_t0 = now;

    // State first, so a message that gets rate-limited still counts.
    if (strstr(line, "<<START>> Async Loading")) {
        g_zone_pending++;
        g_zone_pending_at = now;
        g_zone_stall_said = 0.0;
        snprintf(g_zone_pending_name, sizeof g_zone_pending_name, "%s", line);
    } else if (strstr(line, "<< END >> Async Loading")) {
        if (g_zone_pending) g_zone_pending--;
        if (!g_zone_pending) g_zone_pending_name[0] = 0;
    } else if (strstr(line, "Adding to queue")) {
        g_zone_queued++;
    } else if (strstr(line, "ZoneLoadingEnded Verified")) {
        g_zone_queued = 0;
    }

    // Consecutive duplicates collapse; a burst is capped at 40 lines a second
    // and the drop is reported rather than hidden.
    if (strcmp(line, g_zone_prev) == 0) {
        g_zone_rep++;
        if (g_zone_rep != 1 && g_zone_rep != 10 && g_zone_rep != 100 &&
            g_zone_rep % 1000 != 0)
            return;
    } else {
        snprintf(g_zone_prev, sizeof g_zone_prev, "%s", line);
        g_zone_rep = 0;
    }
    if (now - g_zone_win >= 1.0) {
        if (g_zone_dropped)
            fprintf(stderr, "  [zone] %+8.3f (%u lines dropped by the rate "
                            "cap)\n", now - g_zone_t0, g_zone_dropped);
        g_zone_win = now;
        g_zone_win_n = 0;
        g_zone_dropped = 0;
    }
    if (++g_zone_win_n > 40) { g_zone_dropped++; return; }

    int tag = shar_zone_thread_tag();
    char thr[4];
    if (tag < 0) snprintf(thr, sizeof thr, "t?");
    else         snprintf(thr, sizeof thr, "t%d", tag);

    if (g_zone_rep)
        fprintf(stderr, "  [zone] %+8.3f %s p%u q%u | %s  (x%u)\n",
                now - g_zone_t0, thr, g_zone_pending, g_zone_queued, line,
                g_zone_rep + 1);
    else
        fprintf(stderr, "  [zone] %+8.3f %s p%u q%u | %s\n",
                now - g_zone_t0, thr, g_zone_pending, g_zone_queued, line);
    fflush(stderr);
}

// Called from the once-a-second heartbeat. A request that never completes is
// the failure mode with no line of its own -- the ledger simply stops -- so it
// gets one here.
static void shar_zone_watchdog(double now) {
    if (!g_zone_on || !g_zone_pending) return;
    double age = now - g_zone_pending_at;
    if (age < 3.0) return;
    if (g_zone_stall_said != 0.0 && now - g_zone_stall_said < 10.0) return;
    g_zone_stall_said = now;
    fprintf(stderr, "  [zone] STALL %.1f s: %u outstanding, queue %u, last "
                    "start was '%s' -- LoadingManager's busy byte is stuck and "
                    "nothing further will stream\n",
            age, g_zone_pending, g_zone_queued, g_zone_pending_name);
    fflush(stderr);
}

// ---------------------------------------------------------------------------
// Round 25: the deferred zone queue is applied where the player WAS.
//
// Launch #24 ended parked in a white void at (189.3, 5.0, -256.9) with cars
// driving on invisible roads, and the ledger's tail says why. At +345.465 two
// queued zone events drained in the same instant, each DUMPING l1i01 and
// l1z3/z4/z6/z7 and l1r3/r4a/r4b/r6/r7 and answering "Nothing to load in that
// Zone" for the four the player was actually standing in. The
// "Driving TOO FAST: Adding to queue." lines that put them there fired at
// +310.9, +317.3 and +325.7 -- twenty to thirty-one seconds and most of a
// suburb earlier.
//
// The disassembly makes it exact. A deferred entry is nothing but the
// `ZoneEventLocator *` the event carried: HandleEvent's busy branch (0x5b7098)
// prints the TOO-FAST line and then does `str x20, [x8, x9, lsl #3]` into the
// array at RenderManager+0x80 with the count at +0x7c -- no dedupe, no
// capacity test, no timestamp, no copy of anything about the moment. The
// locator is STATIC LEVEL DATA: GetNumDumpZones/GetDumpZone/GetNumLoadZones/
// GetLoadZone read lists that were baked into the p3d, so a queued entry is a
// standing instruction to make the world look the way it should look AT THAT
// TRIGGER VOLUME. Drain it late and it does exactly that -- it dumps the
// district you have since driven into.
//
// The drain (OnProcessRequestsComplete, 0x5b6968-0x5b69f0) is
// `SwapArray<ZoneEventLocator*>::RemoveKeepOrder(0)` inlined: FIFO, oldest
// first, one per completion, the popped pointer copied into RenderManager+0x90
// as "the current locator" and re-dispatched through the vtable as event 0x4d
// with NO staleness check of any kind. Two identical events can and do queue --
// the same locator pointer appended twice -- which is precisely the double
// drain in #24's tail.
//
// And the engine already owns the predicate it is skipping. Locator+0x4c is
// the per-player "inside this trigger volume" flag: `TriggerVolume::Trigger`
// (0x510778) writes it on entry and clears it on exit before calling
// OnTrigger, and TriggerVolumeTracker clears the player's bit when the volume
// is left (0x510f3c, 0x5110a4). HandleEvent's first act on event 0x4d
// (0x5b6f30) is `ldrb w8,[x20,#0x4c]; ldrb w9,[x19,#0x9a]; orr; cbz -> return`
// -- a LIVE event whose player is no longer inside is dropped on the floor.
// The replay path sets that +0x9a byte precisely to bypass the test, which is
// the right call when a completion lands a few hundred milliseconds later and
// the player has just clipped through a thin gate. It is the wrong call when
// the completion lands half a minute later, which is the world this port
// lives in.
//
// So the fix is the engine's own rule, applied to entries that have gone cold:
// once a frame, walk the deferred array and remove any entry whose locator the
// player is no longer inside AND that has been sitting there longer than
// KL_SHAR_ZONESTALE seconds (5.0 by default), plus any exact duplicate of a
// later entry, which is redundant whatever its age. Removal is the guest's own
// exported `RemoveKeepOrder`, so the array keeps the shape the drain expects,
// and an emptied queue simply takes the drain's own empty branch (event 0x82,
// "ZoneLoadingEnded Verified"). Nothing is invented: the last volume the
// player is genuinely standing in survives the prune and still applies.
// KL_SHAR_ZONEFIX=0 backs the whole thing out.
//
// Both halves run on the game thread -- every [zone] line in #24 that touches
// the queue is t0, and so is WorldScene::Render -- so this adds no sharing the
// engine did not already have.
#define SHAR_ZQ_MAX 64
static int      g_zfix_on = 1;
static float    g_zfix_stale = 5.0f;
static int      g_zfix_state;        // 0 unresolved, 1 armed, -1 refused
static void   (*g_shar_zq_remove)(void *, int);
static void    *g_zq_seen[SHAR_ZQ_MAX];   // locator -> when we first saw it
static double   g_zq_seen_t[SHAR_ZQ_MAX];
static unsigned g_zq_seen_n;
static unsigned g_zfix_stale_n, g_zfix_dup_n;

// When did this pointer first appear in the array? The queue carries no time
// of its own, so we keep the arrival stamps beside it. A pointer we have never
// seen is stamped now, which is the conservative answer: it starts fresh.
static double shar_zq_stamp(void *p, double now) {
    for (unsigned i = 0; i < g_zq_seen_n; i++)
        if (g_zq_seen[i] == p) return g_zq_seen_t[i];
    if (g_zq_seen_n < SHAR_ZQ_MAX) {
        g_zq_seen[g_zq_seen_n]   = p;
        g_zq_seen_t[g_zq_seen_n] = now;
        g_zq_seen_n++;
    }
    return now;
}

static void shar_zq_forget_absent(void **data, int n) {
    unsigned w = 0;
    for (unsigned i = 0; i < g_zq_seen_n; i++) {
        int live = 0;
        for (int j = 0; j < n; j++) if (data[j] == g_zq_seen[i]) { live = 1; break; }
        if (!live) continue;
        g_zq_seen[w]   = g_zq_seen[i];
        g_zq_seen_t[w] = g_zq_seen_t[i];
        w++;
    }
    g_zq_seen_n = w;
}

static void shar_zq_resolve(void) {
    if (g_zfix_state) return;
    g_shar_zq_remove = kl_guest_sym_global(
        "_ZN9SwapArrayIP16ZoneEventLocatorE15RemoveKeepOrderEi");
    if (!g_shar_zq_remove || !g_shar_rm_get) {
        fprintf(stderr, "  [zone] zonefix: SwapArray<ZoneEventLocator*>::"
                        "RemoveKeepOrder or RenderManager::GetInstance is not "
                        "in this guest — the deferred queue is left alone\n");
        g_zfix_state = -1;
        return;
    }
    g_zfix_state = 1;
    fprintf(stderr, "  [zone] zonefix: ARMED — a deferred zone event whose "
                    "trigger volume the player has left, and which has waited "
                    "longer than %.1f s, is dropped instead of being applied "
                    "to wherever he is now (KL_SHAR_ZONEFIX=0 backs it out)\n",
            (double)g_zfix_stale);
    fflush(stderr);
}

// Once a frame, from the WorldScene::Render interposer. Empty queue is the
// normal case and costs one load.
static void shar_zone_prune(double now) {
    if (!g_zfix_on) return;
    shar_zq_resolve();
    if (g_zfix_state != 1) return;

    void *rm = g_shar_rm_get();
    if (!rm) return;
    char  *sa   = (char *)rm + 0x78;          // the SwapArray itself
    int    n    = *(int *)(sa + 4);           // RenderManager+0x7c
    void **data = *(void ***)(sa + 8);        // RenderManager+0x80
    if (n <= 0 || n > 4096 || !data) {
        g_zq_seen_n = 0;
        return;
    }
    for (int i = 0; i < n; i++)
        if (data[i]) shar_zq_stamp(data[i], now);

    // One removal per pass of the loop, because RemoveKeepOrder shifts every
    // index above the victim down. The bound is paranoia, not arithmetic.
    for (int guard = 0; guard < SHAR_ZQ_MAX; guard++) {
        n    = *(int *)(sa + 4);
        data = *(void ***)(sa + 8);
        if (n <= 1 || !data) break;

        int victim = -1;
        for (int i = 0; i < n - 1 && victim < 0; i++)
            for (int j = i + 1; j < n; j++)
                if (data[j] == data[i]) { victim = i; break; }
        if (victim < 0) break;

        void *loc = data[victim];
        g_zfix_dup_n++;
        fprintf(stderr, "  [zone] %+8.3f SKIPPED duplicate queued event "
                        "(locator %p, age %.1f s, inside %u) — a later entry "
                        "carries the same locator and the same lists; q %d -> "
                        "%d  [dropped so far: %u stale, %u duplicate]\n",
                now - (g_zone_t0 != 0.0 ? g_zone_t0 : now), loc,
                now - shar_zq_stamp(loc, now),
                loc ? *(unsigned char *)((char *)loc + 0x4c) : 0u, n, n - 1,
                g_zfix_stale_n, g_zfix_dup_n);
        fflush(stderr);
        g_shar_zq_remove(sa, victim);
        if (g_zone_queued) g_zone_queued--;
    }

    // Now the age test, on whatever survived the dedupe.
    for (int guard = 0; guard < SHAR_ZQ_MAX; guard++) {
        n    = *(int *)(sa + 4);
        data = *(void ***)(sa + 8);
        if (n <= 0 || !data) break;

        int victim = -1;
        for (int i = 0; i < n; i++) {
            void *loc = data[i];
            if (!loc) continue;
            if (*(unsigned char *)((char *)loc + 0x4c)) continue;  // still in it
            if (now - shar_zq_stamp(loc, now) < (double)g_zfix_stale) continue;
            victim = i;
            break;
        }
        if (victim < 0) break;
        void *loc = data[victim];
        g_zfix_stale_n++;
        fprintf(stderr, "  [zone] %+8.3f SKIPPED stale queued event (locator "
                        "%p, age %.1f s, player left its volume) — its dump "
                        "list describes where he WAS; q %d -> %d  [dropped so "
                        "far: %u stale, %u duplicate]\n",
                now - (g_zone_t0 != 0.0 ? g_zone_t0 : now), loc,
                now - shar_zq_stamp(loc, now), n, n - 1,
                g_zfix_stale_n, g_zfix_dup_n);
        fflush(stderr);
        g_shar_zq_remove(sa, victim);
        if (g_zone_queued) g_zone_queued--;
    }

    n    = *(int *)(sa + 4);
    data = *(void ***)(sa + 8);
    if (data && n >= 0 && n <= 4096) shar_zq_forget_absent(data, n);
}

// ---------------------------------------------- round 36: the radar-HUD probe
//
// The minimap is a black square on device, and the host is where the GL state
// behind it can be read. But the host never reached the capture at all: no
// 1440x1080 framebuffer in the draw census and no `VR radar capture:` line,
// while the level itself streamed and drew. This probe says which of the two
// halves is missing, and nothing else — both hooks forward unconditionally.
//
//   ScroobySetVrRadarGroup  (guiscreenhud.cpp registers HudMap0 with Scrooby)
//   SharOpenXR::BeginRadarCapture  (FeGroup::Display calls it for that group)
//
// Both are cross-TU calls to exported symbols and so go through JUMP_SLOT
// relocations, which is what kl_interpose can reach. FeGroup::Display itself is
// virtual and dispatched through the vtable — deliberately NOT hooked here,
// because that relocation is not one the interposer sees.
//
// KL_SHAR_RADARPROBE=1 turns it on; it is off by default and read-only.
static void (*g_shar_set_radar_group)(void *);
static int  (*g_shar_begin_radar)(int, int, int, int);
static void (*g_shar_end_radar)(void);
static unsigned g_radar_group_sets, g_radar_begin_calls, g_radar_begin_ok;

// Round 37. What was bound when BeginRadarCapture was ENTERED — which is the
// one thing the guest fails to record for itself. Its own
// `glGetIntegerv(GL_FRAMEBUFFER_BINDING, &g.radarSavedFramebuffer)` runs AFTER
// the first-call creation block has already bound the radar framebuffer, so on
// call 1 the capture saves itself and EndRadarCapture restores the radar target
// over the eye target. See the "round 37: EndRadarCapture" block in kl_glfb.c.
// KL_SHAR_RADARFIX=0 takes the restore out and leaves the counting behind.
static int32_t g_radar_saved_fb = -1;
static int     g_radar_capturing;

static int shar_radarfix(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_SHAR_RADARFIX", 0);
    return on;
}

static void shar_set_radar_group(void *group) {
    if (!g_shar_set_radar_group)
        g_shar_set_radar_group = kl_guest_sym_global(
            "_Z22ScroobySetVrRadarGroupPN7Scrooby5GroupE");
    g_radar_group_sets++;
    // An interposer REPLACES the call site, so a failed resolve does not
    // degrade to a tap — it drops the registration and HudMap0 never reaches
    // Scrooby at all, which is the very thing this probe exists to observe.
    // Scream once rather than logging "registered" over a call that never
    // happened. (The name is verified against this build's libmain.so:
    // `nm -D` finds it at 0x7284c4, and the host run forwards it.)
    if (!g_shar_set_radar_group) {
        static int screamed;
        if (!screamed) { screamed = 1;
            fprintf(stderr, "  [radar] ScroobySetVrRadarGroup did not resolve — "
                            "the hook is DROPPING the registration; take the "
                            "probe out with KL_SHAR_RADARPROBE=0\n"); }
        return;
    }
    if (g_radar_group_sets <= 4)
        fprintf(stderr, "  [radar] ScroobySetVrRadarGroup(%p) — HudMap0 #%u "
                        "registered\n", group, g_radar_group_sets);
    g_shar_set_radar_group(group);
}

static int shar_begin_radar(int x0, int y0, int x1, int y1) {
    if (!g_shar_begin_radar) {
        g_shar_begin_radar = kl_guest_sym_global(
            "_ZN10SharOpenXR17BeginRadarCaptureEiiii");
        if (!g_shar_begin_radar) {
            static int screamed;
            if (!screamed) { screamed = 1;
                fprintf(stderr, "  [radar] BeginRadarCapture did not resolve — "
                                "the hook answers `no capture` to every call; "
                                "take the probe out with "
                                "KL_SHAR_RADARPROBE=0\n"); }
        }
    }
    // `false` is a value the guest already handles (FeGroup::Display simply
    // does not capture), so a failed resolve costs the minimap and nothing else.
    // BEFORE the forward, because the forward is what corrupts it.
    g_radar_saved_fb = kl_glfb_shar_radar_binding();
    int r = g_shar_begin_radar ? g_shar_begin_radar(x0, y0, x1, y1) : 0;
    g_radar_capturing = r ? 1 : 0;
    g_radar_begin_calls++;
    if (r) g_radar_begin_ok++;
    if (g_radar_begin_calls <= 4 || (r && g_radar_begin_ok <= 2))
        fprintf(stderr, "  [radar] BeginRadarCapture(%d,%d..%d,%d) -> %s "
                        "(call %u, %u accepted)\n", x0, y0, x1, y1,
                r ? "TRUE" : "false", g_radar_begin_calls, g_radar_begin_ok);
    return r;
}

// EndRadarCapture closes the pass. Two things happen around the forward and
// they are independent: the trace takes its own readback of the radar texture
// while that framebuffer is still bound (the guest's own readback fires ONCE in
// the whole run, so this is the only way to see a steady-state minimap), and
// the framebuffer the capture was entered with is put back afterwards.
static void shar_end_radar(void) {
    if (!g_shar_end_radar) {
        g_shar_end_radar = kl_guest_sym_global("_ZN10SharOpenXR15EndRadarCaptureEv");
        if (!g_shar_end_radar) {
            static int screamed;
            if (!screamed) { screamed = 1;
                fprintf(stderr, "  [radar] EndRadarCapture did not resolve — the "
                                "hook is DROPPING the call, which leaves the "
                                "capture framebuffer bound for the rest of the "
                                "frame; take the probe out with "
                                "KL_SHAR_RADARPROBE=0\n"); }
        }
    }
    if (g_radar_capturing) kl_glfb_shar_radar_endpass();
    if (g_shar_end_radar) g_shar_end_radar();
    if (g_radar_capturing && shar_radarfix())
        kl_glfb_shar_radar_restore(g_radar_saved_fb);
    g_radar_capturing = 0;
}

static void sdl_shar_radar_probe(FILE *out) {
    if (!kl_env_on("KL_SHAR_RADARPROBE", 0)) return;
    // The forwards resolve on the FIRST CALL, not here: an interposer has to be
    // registered before the guest is mapped, and kl_guest_sym_global has
    // nothing to answer with until it is.
    kl_interpose("_Z22ScroobySetVrRadarGroupPN7Scrooby5GroupE",
                 (void *)shar_set_radar_group);
    kl_interpose("_ZN10SharOpenXR17BeginRadarCaptureEiiii",
                 (void *)shar_begin_radar);
    kl_interpose("_ZN10SharOpenXR15EndRadarCaptureEv", (void *)shar_end_radar);
    if (out) fprintf(out, "  [radar] KL_SHAR_RADARPROBE=1 — "
                          "ScroobySetVrRadarGroup, BeginRadarCapture and "
                          "EndRadarCapture are counted and forwarded; "
                          "KL_SHAR_RADARFIX=%d\n", shar_radarfix());
}

// -------------------------------------------- round 38: the widescreen marker
//
// THE BLACK SQUARE. The radar capture's draw 0 is the minimap's OWN Pure3D mesh
// — world-space positions, a camera modelview — submitted through the radar's
// 2D orthographic canvas. On the host it clips away and costs nothing; on the
// device it lands and paints two opaque-black full-width bands, and the guest's
// crop scan then latches uv 0,0..1,1 on them. Both halves of what Austin sees.
//
// WHY it goes through the ortho. In FePure3dObject::Display
// (libs/scrooby/src/FePure3dObject.cpp:543) the marker that routes a widget
// through its OWN map camera and keeps Hole0's colour mask off is
//
//     const bool vrEmbeddedHud = m_isWideScreenCorrectionEnabled;
//
// and that flag is only ever set, on Map0 and Hole0, by
// code/presentation/gui/utility/hudmap.cpp:239 —
//
//     if( CGuiScreen::IsWideScreenDisplay() ) { ...SetWideScreenCorrectionEnabled(true) }
//
// which is `p3d::display->IsWidescreen()`, i.e. `GetWidth()*3 > GetHeight()*4`.
// Our SDL door reports the EYE size as the window (4128x4416 — portrait), so it
// answers false, the marker never gets set, Map0 takes
// `SetupHardwareProjection`'s `!IsEmbeddedHudRendering()` branch through
// GetActiveRadarProjection's ortho, and PrepareRadarDraw forces
// glColorMask(TRUE,...) for the non-embedded case — so Hole0, the disc mask
// that is supposed to write depth only, paints opaque black instead. On the
// author's Quest the SDL surface is landscape and the flag is true; his later
// HEAD hard-codes `|| this==gVrRadarMapPure3dObject || this==gVrRadarHolePure3dObject`,
// which is the same conclusion by another route.
//
// So: answer the question the way the author's device answers it. The symbol is
// exported (`nm -D libmain.so` -> 000000000055476c T), takes no arguments and
// returns bool, so the interposer needs no forward at all.
//
// NOT interposed: pddiDisplay::IsWidescreen itself. It is a weak inline (`W`)
// and its other readers — camera aspect, screen transitions — are right to keep
// seeing a 4:3 display. This row moves ONE decision: the HUD-map marker. Its
// other call sites (letterbox, the galleries, the pause-coin X at
// openxrmanager.cpp:2669, guiscreen.cpp's SLIDE_X assert) shift to their
// widescreen constants, which is what the author's build already does.
//
// The width correction at FePure3dObject.cpp:526 is gated on
// `m_isWideScreenCorrectionEnabled && p3d::display->IsWidescreen()` — the weak
// inline, still false — so the map is NOT squashed by turning this on.
//
// KL_SHAR_WIDESCREEN=0 backs it out.
// AAPCS64 returns a C++ bool in w0 as 0 or 1; an int 1 is byte-identical there,
// and this file does not pull in <stdbool.h>.
//
// The counter is the only proof that the row BOUND. A pre-flight
// kl_guest_sym_global() here would be a lie — the interposer is registered
// before the guest is mapped, so nothing resolves yet and the check would
// scream on every run including the ones that work. So say it from the call.
static unsigned g_shar_widescreen_calls;
static int shar_is_widescreen_display(void) {
    if (++g_shar_widescreen_calls == 1)
        fprintf(stderr, "  [radar] CGuiScreen::IsWideScreenDisplay -> TRUE "
                        "(the widescreen marker row is BOUND)\n");
    return 1;
}

static void sdl_shar_widescreen_install(FILE *out) {
    if (!kl_env_on("KL_SHAR_WIDESCREEN", 0)) {
        if (out) fprintf(out, "  [radar] KL_SHAR_WIDESCREEN=0 — the HUD-map "
                              "embedded-HUD marker stays off (black square)\n");
        return;
    }
    kl_interpose("_ZN10CGuiScreen19IsWideScreenDisplayEv",
                 (void *)shar_is_widescreen_display);
    if (out) fprintf(out, "  [radar] KL_SHAR_WIDESCREEN=1 — "
                          "CGuiScreen::IsWideScreenDisplay answers TRUE, so "
                          "HudMap0/Hole0 carry the embedded-HUD marker "
                          "(0 backs it out)\n");
}

// Round 28's fourth wall, which lives in kl_glfb and is named here because the
// default is a property of the TARGET, not of the GL layer.
//
// SHAR's interiors are stage sets: art/l1i01.p3d gives the Kwik-E-Mart three
// walls and leaves the z -307.13 side out entirely, because the retail cameras
// film the room in through the hole from outside it. In VR the player stands
// inside and sees the clear colour. kl_glfb redraws the wall mesh translated
// -17.40 on z to close it; see the "SHAR fourth wall" block there.
//
// kl_glfb defaults KL_SHAR_FOURTHWALL OFF, so every other guest is untouched
// whether or not it comes through this door. overwrite=0: KL_SHAR_FOURTHWALL=0
// in the environment is the backout, and KL_SHAR_FOURTHWALL_DZ moves the copy.
static void sdl_shar_fourth_wall(FILE *out) {
    const kl_target *t = kl_target_current();
    if (!t || !t->name || strcmp(t->name, "simpsons") != 0) return;
    setenv("KL_SHAR_FOURTHWALL", "1", 0);
    // Round 32: radLoad's loader/game-thread baton mutex needs fair handoff
    // (see mtx_policy_fairshare in runtime/libc/kl_pthread.c). Set here,
    // before the guest is mapped, so every guest mutex is born with it.
    setenv("KL_MUTEX_POLICY", "fairshare", 0);
    // Round 36: the radar trace, ON for this launch. The minimap is a black
    // square and the HOST — which now reaches the same capture pass — draws it
    // correctly (maxRGB 245), so the difference is device-side and the device
    // is where the pass has to be read. See the "SHAR radar-HUD trace" block in
    // kl_glfb.c. Read-only; KL_SHAR_RADARTRACE=0 takes it out, and the PNG
    // directory is resolved against the app container by kl_app.c.
    // (the PNG directory is baked in kl_app.c instead — it has to be set
    // BEFORE the container resolver runs, and this door runs after it.)
    setenv("KL_SHAR_RADARTRACE", "1", 0);
    setenv("KL_SHAR_RADARPROBE", "1", 0);
    // Round 37: the first-capture self-save fix, ON. It is the one thing about
    // the minimap that is certain — the guest saves the radar framebuffer as
    // the target to restore to and then restores it, so the rest of frame 1's
    // eye lands in the minimap texture. KL_SHAR_RADARFIX=0 backs it out.
    setenv("KL_SHAR_RADARFIX", "1", 0);
    // ...and the trace now looks at a STEADY-STATE pass as well as the first
    // one. Pass 0/1 are the captures taken while Pure3D is still preparing the
    // map drawable — the least representative frames of the run, and the only
    // ones launch #35 saw. Two passes here, two at ~200, four PNGs each.
    setenv("KL_SHAR_RADARTRACE_FROM", "0,240", 0);
    setenv("KL_SHAR_RADARTRACE_PASSES", "2", 0);
    setenv("KL_SHAR_RADARTRACE_DUMPS", "16", 0);
    // Round 38: THE FIX. CGuiScreen::IsWideScreenDisplay answers TRUE so the
    // HUD map's Pure3D widgets carry the embedded-HUD marker and stop being
    // squeezed through the radar's 2D orthographic. KL_SHAR_WIDESCREEN=0 backs
    // it out; see the "round 38: the widescreen marker" block above.
    setenv("KL_SHAR_WIDESCREEN", "1", 0);
    if (out)
        fprintf(out, "  [sdl] SHAR streaming: KL_MUTEX_POLICY=%s (firstfit is the A/B)\n",
                getenv("KL_MUTEX_POLICY"));
    if (out)
        fprintf(out, "  [sdl] SHAR radar trace: KL_SHAR_RADARTRACE=%s dir=%s "
                     "passes %s from %s; KL_SHAR_RADARFIX=%s "
                     "(0 takes each out)\n",
                getenv("KL_SHAR_RADARTRACE"),
                getenv("KL_SHAR_RADARTRACE_DIR"),
                getenv("KL_SHAR_RADARTRACE_PASSES"),
                getenv("KL_SHAR_RADARTRACE_FROM"),
                getenv("KL_SHAR_RADARFIX"));
    if (out)
        fprintf(out, "  [sdl] SHAR widescreen marker: KL_SHAR_WIDESCREEN=%s "
                     "(0 backs it out)\n", getenv("KL_SHAR_WIDESCREEN"));
    if (out)
        fprintf(out, "  [sdl] SHAR fourth wall: KL_SHAR_FOURTHWALL=%s "
                     "(0 backs it out; KL_SHAR_FOURTHWALL_DZ moves the copy)\n",
                getenv("KL_SHAR_FOURTHWALL"));
}

// Installed BEFORE the guest is mapped, because an interposer is consulted at
// relocation time and a GOT slot bound to the real function stays bound.
// Nothing else in the tree exports `_ZN10WorldScene6RenderEj`, so this row is
// inert for every other target whether or not the knob is on.
static void sdl_shar_interior_install(FILE *out) {
    if (!kl_env_on("KL_SHAR_INTERIOR", 1)) {
        if (out) fprintf(out, "  [sdl] KL_SHAR_INTERIOR=0 — the interior probe "
                              "and fix are out of this build\n");
        return;
    }
    g_shar_fix = kl_env_on("KL_SHAR_INTERIOR_FIX", 1);
    g_shar_far = kl_env_float("KL_SHAR_FAR", 1000.0f);
    kl_interpose("_ZN10WorldScene6RenderEj", (void *)shar_worldscene_render);
    sdl_shar_radar_probe(out);
    // All three go through JUMP_SLOT relocations in libmain.so, so the
    // interposer table reaches them: SwitchToInterior from
    // InteriorManager::AttemptEntry (0x4f315c) and LoadLevelGags (0x4f3ca8),
    // SwitchToExterior from 0x4f2540 and 0x4f3cbc, Enter from
    // ActionButton::EnterInterior::OnButtonPressed (0x49bf5c). None of them is
    // hot — a handful of calls a session, not a frame.
    kl_interpose("_ZN15InteriorManager16SwitchToInteriorEv",
                 (void *)shar_switch_to_interior);
    kl_interpose("_ZN15InteriorManager16SwitchToExteriorEv",
                 (void *)shar_switch_to_exterior);
    kl_interpose("_ZN15InteriorManager5EnterEP23InteriorEntranceLocatorP9Character"
                 "P9Sequencer", (void *)shar_interior_enter);
    // Round 20's two, both reached through JUMP_SLOT relocations: from
    // WorldScene::Render (0x594d90) and from WorldScene::RenderScene (five
    // sites, 0x593bc4 onward). MarkCameraVisible runs once a frame;
    // IsSphereInCone is hot, so its hook is three counters and a forward.
    // Round 22. rReleasePrintf's output is discarded by the guest, so this is a
    // read-only tap: forwarding it changes nothing the game does. The thunk is
    // needed because the symbol is variadic.
    g_zfix_on    = kl_env_on("KL_SHAR_ZONEFIX", 1);
    g_zfix_stale = kl_env_float("KL_SHAR_ZONESTALE", 5.0f);
    g_zone_on = kl_env_on("KL_SHAR_ZONE", 1);
    if (g_zone_on) {
        extern void klv_guest_trace_printf(void);
        kl_guest_trace_line = shar_zone_sink;
        kl_interpose("_Z14rReleasePrintfPKcz", (void *)klv_guest_trace_printf);
    }
    // Round 27's two read-only probes. KL_SHAR_DSGDUMP=0 takes the armed walk
    // out; KL_SHAR_ACTLOG=0 leaves Place/Remove untouched. Neither changes a
    // thing the guest does when it is on.
    g_shar_dsg_want  = kl_env_int("KL_SHAR_DSGDUMP", 3);
    g_shar_dsg_delay = kl_env_float("KL_SHAR_DSGDELAY", 2.0f);
    g_shar_dsg_every = kl_env_float("KL_SHAR_DSGEVERY", 5.0f);
    g_shar_dsgr      = kl_env_float("KL_SHAR_DSGR", 25.0f);
    g_shar_dsgmax    = kl_env_int("KL_SHAR_DSGMAX", 400);
    g_shar_dsgbudget = kl_env_int("KL_SHAR_DSGBUDGET", 65536);
    g_shar_actmax    = kl_env_int("KL_SHAR_ACTMAX", 200);
    g_shar_act_on    = kl_env_on("KL_SHAR_ACTLOG", 1);
    if (g_shar_act_on) {
        kl_interpose("_ZN10WorldScene5PlaceEP15StaticEntityDSG",
                     (void *)shar_place_static);
        kl_interpose("_ZN10WorldScene6RemoveEP10IEntityDSG",
                     (void *)shar_remove_entity);
    }
    int cull = kl_env_on("KL_SHAR_CULL", 1);
    if (cull) {
        kl_interpose("_ZN10WorldScene17MarkCameraVisibleEP12tPointCameraj",
                     (void *)shar_mark_camera_visible);
        kl_interpose("_ZN10WorldScene14IsSphereInConeERN18RadicalMathLibrary6VectorEf",
                     (void *)shar_is_sphere_in_cone);
    }
    if (out) fprintf(out, "  [sdl] SHAR interior: WorldScene::Render + "
                          "InteriorManager Enter/SwitchToInterior/"
                          "SwitchToExterior interposed (measure on, fix %s); "
                          "cull probe (MarkCameraVisible + IsSphereInCone) %s; "
                          "KL_SHAR_FAR %.1f; zone ledger %s; stale-zone "
                          "prune %s (KL_SHAR_ZONESTALE %.1f s); DSG walk %d "
                          "(delay %.1f s, every %.1f s, near %.1f u, max %d "
                          "lines); Place/Remove log %s\n",
                     g_shar_fix ? "on" : "OFF", cull ? "on" : "OFF",
                     (double)g_shar_far, g_zone_on ? "on" : "OFF",
                     g_zfix_on ? "on" : "OFF", (double)g_zfix_stale,
                     g_shar_dsg_want, (double)g_shar_dsg_delay,
                     (double)g_shar_dsg_every, (double)g_shar_dsgr,
                     g_shar_dsgmax, g_shar_act_on ? "on" : "OFF");
}

static void sdl_extract_assets(FILE *out) {
    const char *assets = kl_jni_assets_dir();
    const char *files  = kl_jni_files_dir();
    if (!assets || !*assets || !files || !*files) return;
    char dest[1024];
    snprintf(dest, sizeof dest, "%s/files", files);
    mkdir(dest, 0755);
    DIR *d = opendir(assets);
    if (!d) return;
    int copied = 0, failed = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char src[1200], dst[1200];
        snprintf(src, sizeof src, "%s/%s", assets, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", dest, e->d_name);
        struct stat st;
        if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (stat(dst, &st) == 0) continue;            // first run only
        if (sdl_copy_file(src, dst)) copied++; else failed++;
    }
    closedir(d);
    if (out && (copied || failed))
        fprintf(out, "  [sdl] assets -> %s: %d copied, %d failed "
                     "(ExtractAssets, the Java half — the fonts live here)\n",
                dest, copied, failed);
}

// The world went MONO the moment the Quest multiview path engaged, and it is
// this ConVar that engages it.
//
// `hl2quest_vr_single_pass_opaque` is HL2Q3VR's "Experimental Quest two-eye
// single-pass rendering for the opaque main view" — it draws the opaque world
// ONCE into a two-layer array target through `GL_OVR_multiview2`, and the
// port's own `cfg/sourcevr_hl2.cfg` ships it at 1 with the note "Set this to 0
// only when comparing against the stable two-pass renderer."
//
// ANGLE-on-Metal has no multiview at all, so on this runtime the path cannot
// start. Launch #7's log says so three ways, in order, on consecutive lines:
//
//   HL2Q3VR multiview: framebuffer incomplete 0x8cd7
//   HL2Q3VR multiview: no usable vertex shader variant for lightmappedgeneric_vs20
//   ERROR: 0:2: 'GL_OVR_multiview2' : extension is not supported
//   HL2Q3VR multiview: begin failed; using normal stereo.      x1,599, once a frame
//
// **"Using normal stereo" is not what it then does.** The eye dump is the
// proof and it is exact: the two dumps taken BEFORE that first `begin failed`
// differ per eye (the loading screen, each eye's text on its own optical
// axis), and the one taken AFTER it — the first real world frame — is
// BYTE-IDENTICAL between eye 0 and eye 1, same md5, same 4,542,691 lit texels.
// The game's own log agrees: `Eye 0 ... texture=86 ... framebuffer=41/42` and
// `Eye 1 ... texture=86 ... framebuffer=41/42`, both eyes copied out of ONE
// source target.
//
// And a mono picture is exactly the doubling Austin reports. The compositor
// places each eye's texture through that eye's OWN asymmetric frustum — 60/45
// for the left, 45/60 for the right, measured and 1:1 (round 8's `[cp] ...
// edges land at NDC x -1.000 .. +1.000`). Hand both eyes the SAME pixels and
// the same feature lands ~15 degrees apart in the two eyes: too far to fuse,
// so the brain reports two pictures, one to the left and one to the right.
// That is the report, and nothing in the composite is wrong.
//
// So the ConVar goes to 0 and the game takes its own stable two-pass renderer.
//
// **Why the file and not the command line.** `+hl2quest_vr_single_pass_opaque 0`
// in setArgs would be overwritten: libclient `exec sourcevr_%s.cfg` at VR
// init, which is AFTER the startup commands run, and the cfg says 1. The cfg
// IS the setting, so the cfg is what gets edited.
//
// The edit is in the game tree, which both drivers reach at the same relative
// name — `<files>/srceng/hl2/cfg/sourcevr_hl2.cfg`, the staged copy under
// Documents/android-files on device and the symlink into `games/` on the host
// — so host and device get one behaviour rather than two. It is idempotent
// (a second launch finds 0 and says so), it rewrites only the value on that
// one line, and `KL_SRCENG_SINGLE_PASS=1` leaves the file alone, which is the
// A/B back to the doubling.
// The generic half: set `key` to `want` in one of the game tree's cfg files,
// in place, idempotently. Two syntaxes, because srceng uses both — a console
// cfg is `key value` and a KeyValues cfg is `"key"\t"value"` — and `quoted`
// says which.
static void sdl_set_cfg(FILE *out, const char *rel, const char *key,
                        const char *want, int quoted) {
    const char *files = kl_jni_files_dir();
    if (!files || !*files) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", files, rel);

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (out) fprintf(out, "  [sdl] no %s — %s not pinned\n", path, key);
        return;
    }
    static char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    int overlong = !feof(f);
    fclose(f);
    buf[n] = 0;
    if (overlong) {
        if (out) fprintf(out, "  [sdl] %s is larger than the cfg pin's buffer "
                              "— left alone rather than truncated\n", path);
        return;
    }

    // The FIRST occurrence outside a comment. A `//` line mentioning the name
    // is exactly what sourcevr_hl2.cfg has above its setting, and rewriting
    // the comment would leave the setting untouched and say it had worked.
    size_t klen = strlen(key);
    char *p = buf, *hit = NULL;
    while ((p = strstr(p, key)) != NULL) {
        char *bol = p;
        while (bol > buf && bol[-1] != '\n') bol--;
        int commented = 0;
        for (char *q = bol; q + 1 < p; q++)
            if (q[0] == '/' && q[1] == '/') { commented = 1; break; }
        if (!commented) { hit = p; break; }
        p += klen;
    }
    if (!hit) {
        if (out) fprintf(out, "  [sdl] %s carries no %s setting — nothing "
                              "pinned\n", path, key);
        return;
    }

    // Step past the key, its closing quote in a KeyValues file, the gap, and
    // the opening quote of the value.
    char *v = hit + klen;
    if (quoted && *v == '"') v++;
    while (*v == ' ' || *v == '\t') v++;
    if (quoted && *v == '"') v++;

    // Only a single-character value is rewritten in place: every setting this
    // pins is one digit, and widening the file would mean moving the tail.
    size_t wlen = strlen(want);
    char end = quoted ? '"' : '\n';
    if (wlen != 1 || (v[1] != end && v[1] != '\r' && v[1] != ' ' && v[1] != '\t')) {
        if (out) fprintf(out, "  [sdl] %s in %s reads '%.8s', which is not the "
                              "one-digit value this pin can rewrite — left "
                              "alone\n", key, path, v);
        return;
    }
    if (*v == *want) {
        if (out) fprintf(out, "  [sdl] %s already %s\n", key, want);
        return;
    }
    char was = *v;
    *v = *want;

    FILE *w = fopen(path, "wb");
    if (!w || fwrite(buf, 1, n, w) != n || fclose(w) != 0) {
        if (w) fclose(w);
        if (out) fprintf(out, "  [sdl] could not rewrite %s — %s stays %c\n",
                         path, key, was);
        return;
    }
    if (out) fprintf(out, "  [sdl] %s %c -> %s in %s\n", key, was, want, path);
}


// The OTHER kind of pin: a whole console line, added when the cfg does not
// carry the setting at all.
//
// `sdl_set_cfg` above rewrites one digit of a setting that is ALREADY in the
// file — which is the right tool for `mat_picmip` and friends, and useless for
// a cvar the pack never wrote down. `r_flex` is that: nothing in the shipped
// tree mentions it, so there is no digit to overwrite.
//
// This one rewrites the file: every existing uncommented line whose first
// token is `key` is dropped, and `key value` is appended at the end. Appending
// is deliberate — a console cfg is executed top to bottom and the LAST
// assignment wins, so the pin cannot be undone by a line below it.
//
// Console cfgs only (`key value`). KeyValues files keep `sdl_set_cfg`.
static void sdl_set_cvar_line(FILE *out, const char *rel, const char *key,
                              const char *want) {
    const char *files = kl_jni_files_dir();
    if (!files || !*files) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", files, rel);

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (out) fprintf(out, "  [sdl] no %s — %s not pinned\n", path, key);
        return;
    }
    static char in[64 * 1024];
    size_t n = fread(in, 1, sizeof in - 1, f);
    int overlong = !feof(f);
    fclose(f);
    in[n] = 0;
    if (overlong) {
        if (out) fprintf(out, "  [sdl] %s is larger than the cvar pin's buffer "
                              "— left alone rather than truncated\n", path);
        return;
    }

    static char outbuf[64 * 1024 + 256];
    size_t o = 0, klen = strlen(key);
    int dropped = 0, already = 0;

    // Walk the file a line at a time, copying everything that is not a
    // previous pin of this same key.
    for (char *line = in; *line; ) {
        char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) + 1 : strlen(line);
        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        int mine = !strncmp(t, key, klen) &&
                   (t[klen] == ' ' || t[klen] == '\t');
        if (mine) {
            // Is it already exactly what we want? Only then can this be a
            // no-op; a DIFFERENT value on that line still has to go.
            const char *v = t + klen;
            while (*v == ' ' || *v == '\t') v++;
            size_t wlen = strlen(want);
            if (!strncmp(v, want, wlen) &&
                (v[wlen] == 0 || v[wlen] == '\n' || v[wlen] == '\r' ||
                 v[wlen] == ' ' || v[wlen] == '\t'))
                already = 1;
            dropped++;
        } else if (o + len < sizeof outbuf) {
            memcpy(outbuf + o, line, len);
            o += len;
        }
        line += len;
    }

    // Exactly one line, carrying the value asked for, and it is the last
    // assignment in the file: nothing to write.
    if (already && dropped == 1) {
        if (out) fprintf(out, "  [sdl] %s already %s in %s\n", key, want, path);
        return;
    }

    if (o && outbuf[o - 1] != '\n' && o + 1 < sizeof outbuf) outbuf[o++] = '\n';
    int add = snprintf(outbuf + o, sizeof outbuf - o, "%s %s\n", key, want);
    if (add < 0 || (size_t)add >= sizeof outbuf - o) return;
    o += (size_t)add;

    FILE *w = fopen(path, "wb");
    if (!w || fwrite(outbuf, 1, o, w) != o || fclose(w) != 0) {
        if (w) fclose(w);
        if (out) fprintf(out, "  [sdl] could not rewrite %s — %s not pinned\n",
                         path, key);
        return;
    }
    if (out) fprintf(out, "  [sdl] %s %s pinned in %s (%d old line%s replaced)\n",
                     key, want, path, dropped, dropped == 1 ? "" : "s");
}

// THE FACE PROBE (round 11): is the missing face the flex path?
//
// The defect, Austin's since day one and unchanged by every round so far: a
// citizen's HEAD has no FACE inside ~20 yards. Far away the face is there;
// close up the head is a hollow shell — hair and the back of the skull, and
// nothing where the face should be. Bodies also read as unlit.
//
// What changes at close range is the LOD, and what LOD0 has that no other LOD
// has is FLEX. Source's studiorender takes a different path entirely for a
// model with flexes: instead of drawing the static vertex buffer, it builds a
// flexed copy of the mesh (`R_StudioFlexVerts` and the software-skinned draw
// behind it) and draws that. The lower LODs have their flexes stripped by
// studiomdl, so they never enter it — which is exactly the shape of "far is
// fine, near is not".
//
// The evidence that it is NOT the other candidates, from launch #11's logs:
//   * no shader compile or link failure in the whole run (kl_glfb prints
//     every one of both, with the source; the count is zero),
//   * no `[gl] unhandled pname` and no GL error line,
//   * DXLevel_V1 is 90, so `HasFastVertexTextures()` is false and the
//     HARDWARE morph path — the vertex-texture one, `_rt_MorphAccumulator`
//     and friends — is never entered at all. There is no vertex-texture fetch
//     to be broken; the caps say 32 vertex texture units anyway.
//   * studiorender's own two flex complaints ("flex verts have not been
//     converted", "model verts have been compressed, cannot render") do not
//     appear in engine.log, so the vertex data itself converts.
//
// So the cheapest decisive question is whether the flexed path is drawn at
// all, and `r_flex 0` asks it in one launch: the config the client hands
// studiorender carries `bFlex`, and with it false a flexed model is drawn
// down the same ordinary path the far LOD uses.
//
//   faces come back  -> the flexed-mesh draw is the defect, and the next round
//                       is about WHY (winding, the dynamic-mesh build, the
//                       software skin), not about WHETHER.
//   faces still gone -> flex is exonerated and the defect is LOD0 geometry
//                       itself; the next A/B is `r_rootlod 1` in
//                       videoconfig_android.cfg, which forces every model to
//                       the LOD that already works.
//
// The cost of the probe if it works is deadpan faces — no lip sync, no
// blinking. That is a face, which is more than the port has now.
//
// `KL_SRCENG_FLEX=1` writes the line back to 1 and is the A/B in the other
// direction; the pin is idempotent and self-reversing, so an env flip on the
// next launch is enough (the cfg lives in the game tree and persists).
// The eye size is NOT the size we recommend. libsourcevr logs
// `Eye resolution: 100% of Quest 3 native 2064x2208 -> 2064x2208 per eye
// (runtime max 10240x8214)` — it sizes its swapchain as `vr_eye_scale`
// percent of a hardcoded Quest 3 eye and only CLAMPS to the runtime's max.
// So launches #11 and #12 rendered 2064x2208 whatever KL_OVRP_EYE_MAX said,
// and "don't notice sharper" was correct. 200% is 4128x4416 per eye, near
// the panel's own 5087x4081; KL_SRCENG_EYE_SCALE=100..300 is the A/B.
static const char *sdl_eye_scale(void) {
    static char buf[4];
    const char *e = getenv("KL_SRCENG_EYE_SCALE");
    int v = e ? atoi(e) : 200;
    if (v < 50 || v > 300) v = 200;
    snprintf(buf, sizeof buf, "%d", v);
    return buf;
}

// Round 11 pinned r_flex 0 and launch #12 answered it: the faces came back, so
// the flexed-mesh path WAS the defect — and the eyes and the neck stayed
// missing, which named the defect. Both live on the same GL call: studiorender
// draws flexed faces, eyeballs and software-skinned groups out of a DYNAMIC
// vertex buffer, which reaches togl as glDrawRangeElementsBaseVertex with
// basevertex != 0, and kl_glfb was answering that with ANGLE's core ES 3.2
// entry point — a validation error on our ES 3.0 context, i.e. no draw at all.
// See the long note at klfb_DrawRangeElementsBaseVertex in runtime/gfx/kl_glfb.c.
//
// So the pin goes back to the shipped 1: with the GL fix in, faces AND eyes AND
// neck should all be there, animated. KL_SRCENG_FLEX=0 re-pins 0 (faces static
// but present) and KL_GLFB_BASEVERTEX_CORE=1 puts the GL bug back — the two
// halves of the A/B.
static void sdl_pin_face_probe(FILE *out) {
    const char *fx = getenv("KL_SRCENG_FLEX");
    const char *want = (fx && fx[0] == '0' && !fx[1]) ? "0" : "1";
    sdl_set_cvar_line(out, "srceng/hl2/cfg/sourcevr_hl2.cfg", "r_flex", want);
    sdl_set_cvar_line(out, "srceng/hl2/cfg/sourcevr_hl2.cfg", "vr_eye_scale",
                      sdl_eye_scale());
    // ...which launch #13 showed is NOT where libsourcevr reads it: it still
    // logged `100% of Quest 3 native` with `+vr_eye_scale 200` on the command
    // line AND in the cfg. The launcher (VRLauncherTabs.selectedEyeScale)
    // stores the choice in SharedPreferences "mod" as int `vr_eye_scale`, and
    // libsourcevr reads THAT through JNI (getSharedPreferences("mod", 0)
    // .getInt("vr_eye_scale", 100)) before the swapchain is sized. Our
    // SharedPreferences are kl_jni_prefs.c's shared_prefs/<name>.xml, and
    // launch #13's mod.xml had 0 entries — so write the one entry the game
    // wants. The launcher's own menu offers only 50/75/100/114; whether the
    // native side clamps is what the `Eye resolution:` line answers.
    {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/shared_prefs", kl_jni_files_dir());
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/shared_prefs/mod.xml", kl_jni_files_dir());
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n"
                       "<map>\n    <int name=\"vr_eye_scale\" value=\"%s\" />\n</map>\n",
                    sdl_eye_scale());
            fclose(f);
            if (out) fprintf(out, "  [sdl] vr_eye_scale %s written to %s (the "
                                  "SharedPreferences libsourcevr sizes the eye from)\n",
                             sdl_eye_scale(), path);
        } else if (out) {
            fprintf(out, "  [sdl] cannot write %s\n", path);
        }
    }
    if (out)
        fprintf(out, "  [sdl] face probe: r_flex %s — %s\n", want,
                want[0] == '1'
                    ? "flexes ON, the shipped behaviour: with the basevertex "
                      "fix the faces should be present AND animated, and the "
                      "eyes and neck back with them (KL_SRCENG_FLEX=0 re-pins 0)"
                    : "flexes OFF, launch #12's arm — faces static, eyes and "
                      "neck are the basevertex tell");
}

// The two settings this port pins, and why each one.
static void sdl_pin_two_pass(FILE *out) {
    if (kl_env_on("KL_SRCENG_SINGLE_PASS", 0)) {
        if (out) fprintf(out, "  [sdl] KL_SRCENG_SINGLE_PASS=1 — leaving "
                              "hl2quest_vr_single_pass_opaque alone (the A/B "
                              "back to the mono world)\n");
    } else {
        sdl_set_cfg(out, "srceng/hl2/cfg/sourcevr_hl2.cfg",
                    "hl2quest_vr_single_pass_opaque", "0", 0);
    }

    // ...and the texture budget, which is the OTHER thing the game tree gets
    // wrong here.
    //
    // This port ships a 4.9 GB Alyx-remix HD pack in `hl2/custom`
    // (alyx_remix_q3_mobile_000..004.vpk) and `videoconfig_android.cfg` asks
    // for it at `mat_picmip 0` — full resolution, every mip resident. Launch
    // #8's own headless run measured what that costs on this device:
    //
    //   [hb] 15:38:17  foot=5452M avail=2739M     the menu, before any map
    //   [hb] 15:38:25  foot=7487M avail=704M      d1_trainstation_01 loading
    //
    // 7,487 MB against an 8,191 MB ceiling, with 704 MB of headroom, on a run
    // with NOBODY WEARING THE HEADSET — `[cp] pinch: L no skeleton (0 frames)
    // | R no skeleton (0 frames)` for its whole length. A worn session pays
    // ARKit hand tracking and a live compositor on top of that, and "new game
    // crashes with no .ips" is what running out of that headroom looks like.
    //
    // `mat_picmip 1` drops the top mip of every texture: a quarter of the
    // texels, so most of the HD pack's cost, for a pack whose whole point is
    // resolution nobody can resolve at HL2 map scale on this panel anyway. It
    // is read from `videoconfig_android.cfg` at startup, BEFORE any texture
    // loads — which is why it goes there and not into the console cfg, where
    // it would arrive after the menu's textures were already resident.
    //
    // Launch #10 (2026-08-26) changed the arithmetic: with the runtime
    // advertising DXT (d170808) the pack uploads as compressed blocks instead
    // of being decompressed to RGBA8888 on the CPU, and the SAME picmip-1 run
    // that peaked at 3,275 MB now peaks at 2,401 MB — with the whole 4x
    // texel cost of picmip 0 being paid in 1/4-to-1/6-size DXT blocks. So the
    // default is picmip 0 again: the HD pack at the resolution it was made
    // for. `KL_SRCENG_PICMIP=1` is the step back if the worn peak says so.
    const char *pm = getenv("KL_SRCENG_PICMIP");
    char want[2] = { (pm && pm[0] >= '0' && pm[0] <= '4') ? pm[0] : '0', 0 };
    sdl_set_cfg(out, "srceng/hl2/videoconfig_android.cfg", "mat_picmip",
                want, 1);

    // ...and the two sampler settings, which are the OTHER half of "grainy"
    // and cost nothing at all.
    //
    // Launch #9 got NEW GAME working at mat_picmip 1 and Austin's report was
    // "grainy, disappointing quality". Half of that is the picmip above — half
    // the texels — and the round cannot spend the other half back yet (the
    // measurement is in STATUS; picmip 0 peaked at 7,487 MB of 8,191). But the
    // pack's own cfg also ships two settings that make a HALVED texture look
    // worse than it is, and neither is a byte of memory:
    //
    //   "mat_forceaniso"  "1"    anisotropy OFF
    //   "mat_trilinear"   "0"    bilinear, with a hard jump between mips
    //
    // With anisotropy at 1, a surface seen at a grazing angle is sampled from
    // the mip its WORST axis needs — and a first-person shooter's frame is
    // mostly floor and wall seen edge-on, so most of the picture is being read
    // from a mip two or three levels below what it deserves. That is a bigger
    // sharpness loss on this content than the picmip is, and it is a sampler
    // parameter: no extra texels, no extra bytes. With trilinear off, the mip
    // the sampler lands on switches abruptly across the floor, which is the
    // shimmer half of the same complaint.
    //
    // 8x rather than 16x because 8 is where the returns flatten on a
    // 2064x2208 eye and ANGLE-on-Metal charges per sample. Both are clamped
    // by shaderapidx9 to what its caps allow, so if the driver has no
    // anisotropy these are silently a no-op rather than an error —
    // GL_EXT_texture_filter_anisotropic is one of the extensions the runtime
    // now adopts from the backend when it is really there (kl_egl.c).
    const char *an = getenv("KL_SRCENG_ANISO");
    char aniso[2] = { (an && an[0] >= '1' && an[0] <= '9' && !an[1]) ? an[0] : '8', 0 };
    sdl_set_cfg(out, "srceng/hl2/videoconfig_android.cfg", "mat_forceaniso",
                aniso, 1);
    sdl_set_cfg(out, "srceng/hl2/videoconfig_android.cfg", "mat_trilinear",
                "1", 1);

    // MSAA is a knob, not a default — the eye is 5087x4081 now and the
    // srceng MSAA path (ScreenMSAA + mat_antialias, resolved through
    // glBlitFramebuffer) is untested under this GL layer. KL_SRCENG_MSAA=2|4
    // pins both; unset leaves whatever the cfg has (the pack ships 0).
    // The face probe's SECOND arm, wired but not armed: if `r_flex 0` does not
    // bring the faces back, flex is exonerated and the question becomes LOD0
    // geometry itself. `KL_SRCENG_ROOTLOD=1` forces every model to the LOD
    // that already renders correctly at distance — one env var, no rebuild.
    const char *rl = getenv("KL_SRCENG_ROOTLOD");
    if (rl && rl[0] >= '0' && rl[0] <= '9' && !rl[1]) {
        char v[2] = { rl[0], 0 };
        sdl_set_cfg(out, "srceng/hl2/videoconfig_android.cfg", "r_rootlod", v, 1);
    }

    // Round 16: 4x by default. Launch #16 ran the eye at a true 4128x4416
    // with 4.3 GB free and 0-10 repeats per 300 presents in play, and edge
    // aliasing is what is left to see. KL_SRCENG_MSAA=0 is the step back.
    const char *ms = getenv("KL_SRCENG_MSAA");
    if (!(ms && (ms[0] == '0' || ms[0] == '2' || ms[0] == '4') && !ms[1])) ms = "4";
    {
        char v[2] = { ms[0], 0 };
        sdl_set_cfg(out, "srceng/hl2/videoconfig_android.cfg", "ScreenMSAA", v, 1);
        sdl_set_cfg(out, "srceng/hl2/videoconfig_android.cfg", "mat_antialias", v, 1);
        // ...and the VR path's own sample count (libclient's
        // hl2quest_vr_opaque_msaa), which is the one that matters in-headset.
        sdl_set_cvar_line(out, "srceng/hl2/cfg/sourcevr_hl2.cfg",
                          "hl2quest_vr_opaque_msaa", v);
    }
}

static int sdl_fail(const char *why) {
    snprintf(g_err, sizeof g_err, "%s", why);
    return 1;
}

const char *kl_sdl_error(void) { return g_err; }

int kl_sdl_configure(const char *libdir, FILE *out) {
    if (!libdir || !*libdir) return sdl_fail("no guest libdir");
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);

    // The guest's threads need x18 and the TLS veneer's slot before any guest
    // code runs, and srceng spawns threads with stack protectors the way UE4
    // does. Idempotent, so a driver that already did it on this thread pays
    // nothing.
    kl_thread_init();
    kl_jni_set_activity_class(SDL_ACTIVITY);

    // What SDLActivity.java passes to nativeRunMain, from each APK's own smali
    // and carried in the target row: getMainSharedObject() (the library name as
    // Java spells it, which is "lib" + getLibraries()[last]) and
    // getMainFunction(). Constants here until the second SDL guest arrived —
    // Simpsons Hit & Run comes through libmain.so/SDL_main, and a door that
    // knew only liblauncher.so would have failed it at "the entry library is
    // not in the chain" with the right library sitting in the roster.
    {
        const kl_target *t = kl_target_current();
        const char *lib  = (t && t->entry_lib) ? t->entry_lib : "libmain";
        const char *main_fn = (t && t->sdl_main && *t->sdl_main)
                            ? t->sdl_main : SDL_MAIN_DEFAULT;
        snprintf(g_entry_lib, sizeof g_entry_lib, "%s.so", lib);
        snprintf(g_entry_fn, sizeof g_entry_fn, "%s", main_fn);
        if (out) fprintf(out, "  [sdl] entry: %s / %s\n", g_entry_lib, g_entry_fn);
    }

    // ValveActivity2.initNatives, the env half, transcribed with the paths
    // pointed at this driver's equivalents:
    //   VALVE_GAME_PATH  the game root. The on-device value is CONFIRMED as
    //                    /sdcard/srceng (game_cache/hl2q3vr-cache.json), so the
    //                    GUEST spelling is kept in both drivers and
    //                    kl_guest_path maps it — to <userdata>/hl2vr/srceng on
    //                    the host, to <container>/android-files/srceng in the
    //                    app.
    //   APP_DATA_PATH    ApplicationInfo.dataDir. In the GUEST's spelling
    //                    (/sdcard), for the reason VALVE_GAME_PATH is — see
    //                    the paragraph below.
    //   APP_LIB_PATH     nativeLibraryDir, ABSOLUTE: the launcher
    //                    Sys_LoadModule()s its modules by concatenating this
    //                    with the module name, and a relative value only
    //                    resolves by accident of the cwd ("Can't find module -
    //                    hl2vr/lib/..." was exactly that). Android's is always
    //                    absolute.
    //   EXTRAS_VPK_PATH  getFilesDir()/extras_dir.vpk, per the smali — that is
    //                    <dataDir>/files/, which sdl_extract_assets fills.
    //                    Guest spelling for the same reason.
    //   LANG             Locale.getDefault() on the guest's behalf. The
    //                    launcher's own default is russian; the args half
    //                    below says english outright.
    // APP_MOD_LIB is skipped: the smali sets it only when the Intent carries a
    // gamelibdir extra, which the stock launcher path does not.
    //
    // overwrite=0 throughout, which is what makes this work on BOTH drivers:
    // a shell can override any of them for an A/B, and a Home-View launch with
    // no environment at all gets the baked answer.
    // Before any env is published: EXTRAS_VPK_PATH and the font directory
    // both name files this copy is what puts there.
    sdl_extract_assets(out);
    const char *files = kl_jni_files_dir();
    char abslib[1200];
    if (!realpath(g_libdir, abslib)) snprintf(abslib, sizeof abslib, "%s", g_libdir);

    // Everything from here to the end of this function is srceng's Java, and
    // only srceng has it — see sdl_is_srceng(). A guest whose SDLActivity
    // subclass overrides nothing (simpsons) is started by SDL alone: no
    // environment, no setArgs, no cfg pins. It finds its game tree itself, from
    // a path its own libmain hardcodes.
    if (!sdl_is_srceng()) {
        if (out) {
            fprintf(out, "  [sdl] no ValveActivity2 on this guest — SDL's own "
                         "boot only (no env, no setArgs, no cfg pins)\n");
            fflush(out);
        }
        return 0;
    }

    // ...and the game tree's own cfg, which is where the stereo mode lives.
    sdl_pin_two_pass(out);
    sdl_pin_face_probe(out);

    // **The data paths go over in the GUEST's spelling, not ours, and launch
    // #6 is the second time this has been the whole bug.**
    //
    // Source LOWERCASES a path it takes in — round 5 found it doing that to
    // the mod root and paid a launch for it (klb_getcwd now answers in the
    // guest's spelling for exactly this). The fonts were the same defect on a
    // second path and it survived round 6's fix because that fix only put the
    // FILES there. `sdl_extract_assets` copied all nine, `devicectl` lists all
    // nine, and the engine still could not open one:
    //
    //   [fs] fopen(".../Containers/data/application/a4c7744e-.../documents/
    //              android-files/files/dejavusans.ttf") FAILED: No such file
    //
    // — every component lowercased, against a container whose real name is
    // `Containers/Data/Application/A4C7744E-…/Documents`. macOS answers that
    // anyway; the visionOS data volume is case-SENSITIVE and does not, so
    // 1,348 font opens failed on device while the host saw none. The same
    // lowercasing takes out `extras_dir.vpk` with them.
    //
    // A path that is already lowercase cannot be damaged by being lowercased,
    // and the guest's own spelling is exactly that: `/sdcard` is one of the
    // three external-storage prefixes kl_guest_path maps (kl_libc.c), and it
    // resolves to the very directory `kl_jni_files_dir()` names — to
    // <userdata>/hl2vr on the host and to <container>/Documents/android-files
    // in the app, which is where the nine assets were copied on both. So the
    // fix is not a case-insensitive open or a repair pass over an incoming
    // string: it is to stop handing the guest a name only we can spell.
    //
    // This is the trade VALVE_GAME_PATH already made and the reason it reads
    // /sdcard/srceng rather than a container path.
    setenv("VALVE_GAME_PATH", "/sdcard/srceng", 0);
    setenv("APP_DATA_PATH", "/sdcard", 0);
    setenv("APP_LIB_PATH", abslib, 0);
    setenv("EXTRAS_VPK_PATH", "/sdcard/files/extras_dir.vpk", 0);
    setenv("LANG", "en_US", 0);
    if (out) {
        fprintf(out, "  [sdl] VALVE_GAME_PATH=%s (guest spelling; maps under %s)\n",
                getenv("VALVE_GAME_PATH"), files && *files ? files : "(no files dir)");
        fprintf(out, "  [sdl] APP_DATA_PATH=%s  EXTRAS_VPK_PATH=%s "
                     "(guest spelling — Source lowercases these)\n",
                getenv("APP_DATA_PATH"), getenv("EXTRAS_VPK_PATH"));
        fprintf(out, "  [sdl] APP_LIB_PATH=%s\n", abslib);
        fflush(out);
    }
    return 0;
}

int kl_sdl_load(FILE *out) {
    if (!*g_libdir) return sdl_fail("kl_sdl_configure was not called");
    g_nimgs = 0;

    // Before the first image is mapped: an interposer is consulted while
    // relocations are applied, so one registered afterwards would find every
    // GOT slot already bound to the guest's own function.
    sdl_shar_fourth_wall(out);
    sdl_shar_widescreen_install(out);
    sdl_shar_interior_install(out);

    // The roster: every .so in the libdir but the replaced one. A directory
    // scan, not a table — the library set is a property of the APK, and a
    // hand-written list drifts on the next build of the port. It is also
    // exactly what `libs: None` translates for the device bundle.
    //
    //   libopenxr_loader.so — REPLACED, the same call §3.1 made for
    //     libOVRPlugin and Steam Link made for this same file: the guest
    //     dlopens it and kl_openxr_dlopen claims the dlopen by name, so
    //     mapping the guest's copy would run a Khronos loader looking for an
    //     Android broker that is not here.
    // Two directories can answer that, and which one is not a choice: the app
    // bundle carries the TRANSLATIONS and deliberately not the ELF tree (80 MB
    // that would buy only an A/B against the loader they replace), so on device
    // g_libdir is a name the loader resolves rather than a directory that
    // exists. A scan of it there returns nothing, and a chain of nothing is how
    // this door would fail on device while passing on the host — the same shape
    // of split that cost launch #1. So: the ELF tree when it is there, the
    // translations otherwise, and the same roster afterwards either way.
    static char roster[SDL_MAX_LIBS][64];
    unsigned nlibs = 0;
    unsigned dropped = 0;
    {
        DIR *d = opendir(g_libdir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && nlibs < SDL_MAX_LIBS) {
                size_t l = strlen(e->d_name);
                if (l < 4 || strcmp(e->d_name + l - 3, ".so")) continue;
                snprintf(roster[nlibs++], sizeof roster[0], "%s", e->d_name);
            }
            closedir(d);
            if (out) fprintf(out, "  [sdl] roster: %u libraries in %s\n", nlibs, g_libdir);
        }
        if (!nlibs) {
            nlibs = kl_list_translations(roster, SDL_MAX_LIBS);
            if (out) fprintf(out, "  [sdl] roster: %u translated libraries "
                                  "(no ELF tree — this is the bundle)\n", nlibs);
        }
        // libopenxr_loader.so is dropped from whichever list answered: it is
        // REPLACED, the same call §3.1 made for libOVRPlugin and Steam Link
        // made for this same file. The guest dlopens it and kl_openxr_dlopen
        // claims the dlopen by name, so mapping the guest's copy would run a
        // Khronos loader looking for an Android broker that is not here.
        for (unsigned i = 0; i < nlibs; i++) {
            if (strcmp(roster[i], "libopenxr_loader.so")) continue;
            memmove(roster[i], roster[nlibs - 1], sizeof roster[0]);
            nlibs--;
            dropped = 1;
            break;
        }
        if (out && !dropped)
            fprintf(out, "  [sdl] no libopenxr_loader.so in the roster "
                         "(nothing to replace)\n");
    }
    if (!nlibs) return sdl_fail("no guest libraries: neither an ELF tree nor translations");

    char path[1200];
    int  loaded[SDL_MAX_LIBS] = {0};

    // libc++_shared FIRST, before the sort and before libSDL2's own init, for
    // exactly the reason kl_ue4's chain makes the same call: Android's
    // weak-symbol resolution binds the FIRST definition in load order, and the
    // C++ runtime's weak symbols — the __cxxabiv1 typeinfo vtables and
    // __dynamic_cast — must come from the one libc++_shared, not from
    // libtier0's STATICALLY-LINKED libc++abi. Roster order in the first Kahn
    // pass put libtier0 first, every typeinfo in libserver ended up with
    // libtier0's vtables, and CHL2_Player's dynamic_cast to CBasePlayer
    // returned NULL — a null-deref at campaign-map load (the menu map has no
    // player entity, which is why only NEW GAME hit it).
    {
        snprintf(path, sizeof path, "%s/libc++_shared.so", g_libdir);
        kl_image *cxx = kl_load_auto(path);
        if (!cxx) { snprintf(g_err, sizeof g_err, "libc++_shared.so: %s", kl_error()); return 1; }
        kl_register_image("libc++_shared.so", cxx);
        g_imgs[g_nimgs] = cxx;
        snprintf(g_names[g_nimgs++], sizeof g_names[0], "%s", "libc++_shared.so");
        for (unsigned i = 0; i < nlibs; i++)
            if (!strcmp(roster[i], "libc++_shared.so")) { loaded[i] = 1; break; }
    }

    // Kahn's algorithm over the DT_NEEDED graph, restricted to the roster: a
    // needed name that is NOT in the roster is an Android system library the
    // shim answers (libc, libm, liblog, libEGL, ...), not an ordering
    // constraint. Order is load-BEARING here — imports bind at relocation
    // time, so libfilesystem_stdio mapped before libtier0 keeps g_pMemAlloc a
    // recorded-but-unresolved import FOREVER and its CreateInterface then
    // follows a GOT slot that was never bound (SIGSEGV, no name attached).
    // Directory order put filesystem before tier0; DT_NEEDED order cannot.
    // A cycle — none is expected here — degrades to loading what is left in
    // roster order, which is no worse than where this started.
    unsigned progress = 1;
    while (g_nimgs < nlibs && progress) {
        progress = 0;
        for (unsigned i = 0; i < nlibs; i++) {
            if (loaded[i]) continue;
            snprintf(path, sizeof path, "%s/%s", g_libdir, roster[i]);
            char need[32][128];
            int nn = kl_list_needed(path, need, 32);
            for (int j = 0; j < nn; j++) {
                for (unsigned k = 0; k < nlibs; k++) {
                    if (k == i || strcmp(roster[k], need[j])) continue;
                    if (!loaded[k]) goto not_ready;   // a guest dep not yet mapped
                    break;
                }
            }
            {
                kl_image *img = kl_load_auto(path);
                if (!img) {
                    if (out) fprintf(out, "  %-28s LOAD FAILED: %s\n", roster[i], kl_error());
                } else {
                    kl_register_image(roster[i], img);
                    g_imgs[g_nimgs] = img;
                    snprintf(g_names[g_nimgs++], sizeof g_names[0], "%s", roster[i]);
                    // The LOAD ADDRESS, in the shape tools/symbolize_sample.py
                    // parses: this guest owns its frame loop, so `sample <pid>`
                    // is the one instrument that says what it is doing, and it
                    // reports every frame as `??? (in <unknown binary>)` until
                    // something joins the two.
                    if (out)
                        fprintf(out, "  mapped %-28s @%p %7.2f MB\n", roster[i],
                                kl_base(img), kl_span(img) / 1048576.0);
                }
                loaded[i] = 1;
                progress++;
            }
            not_ready:;
        }
    }
    for (unsigned i = 0; i < nlibs; i++) {
        if (loaded[i]) continue;
        if (out) fprintf(out, "  %-28s in a DT_NEEDED cycle — loading unordered\n", roster[i]);
        snprintf(path, sizeof path, "%s/%s", g_libdir, roster[i]);
        kl_image *img = kl_load_auto(path);
        if (!img) continue;
        kl_register_image(roster[i], img);
        g_imgs[g_nimgs] = img;
        snprintf(g_names[g_nimgs++], sizeof g_names[0], "%s", roster[i]);
    }
    if (out) {
        fprintf(out, "=== the chain: mapped %u of %u libraries, dependencies first ===\n",
                g_nimgs, nlibs);
        fflush(out);
    }
    if (!kl_find_image(SDL_LIB)) return sdl_fail(SDL_LIB " is not in the chain");
    if (!kl_find_image(g_entry_lib)) return sdl_fail("the entry library is not in the chain");
    return 0;
}

unsigned kl_sdl_gap(FILE *out) {
    unsigned libs = 0;
    for (unsigned i = 0; i < g_nimgs; i++) {
        unsigned n = 0;
        const char *const *miss = kl_missing_imports(g_imgs[i], &n);
        if (!n) continue;
        libs++;
        if (!out) continue;
        fprintf(out, "  %s: %u unique unresolved import%s\n", g_names[i], n, n == 1 ? "" : "s");
        for (unsigned j = 0; j < n; j++) {
            if (j % 4 == 0) fprintf(out, "      ");
            fprintf(out, "%-28s", miss[j]);
            if (j % 4 == 3 || j + 1 == n) fprintf(out, "\n");
        }
    }
    if (out) fflush(out);
    return libs;
}

int kl_sdl_begin(FILE *out) {
    if (!g_nimgs) return sdl_fail("kl_sdl_load was not called");

    // Initializers in the same dependencies-first order the mapping used —
    // Android runs DT_INIT_ARRAYs that way too, and srceng's libraries register
    // their interfaces into tier0's registry from theirs, so tier0 must have
    // run before anything that links it. The topo order guarantees it; no
    // priority list does.
    for (unsigned i = 0; i < g_nimgs; i++)
        kl_run_init(g_imgs[i]);

    // The door's own entry: libSDL2's JNI_OnLoad, which registers the
    // SDLActivity natives SDL's half of the boot is about to be driven through.
    // On Android it runs inside System.loadLibrary("SDL2"), after the library's
    // own initializers — which is where it sits here.
    kl_image *sdl = kl_find_image(SDL_LIB);
    if (!sdl) return sdl_fail(SDL_LIB " is not in the chain");
    if (out) { fprintf(out, "\n=== libSDL2 JNI_OnLoad (the SDL door) ===\n"); fflush(out); }
    typedef int (*jni_onload_fn)(void *vm, void *reserved);
    jni_onload_fn onload = (jni_onload_fn)kl_sym(sdl, "JNI_OnLoad");
    if (!onload) return sdl_fail(SDL_LIB " exports no JNI_OnLoad");
    kl_jni_local_frame_push();
    int version = onload(kl_jni_vm(), NULL);
    kl_jni_local_frame_pop();
    if (out) fprintf(out, "  JNI_OnLoad returned 0x%08x\n", version);
    // NOT pinned to JNI_VERSION_1_6 the way the Unity gate is: SDL2 asks for
    // JNI_VERSION_1_4 (its whole registered surface needs nothing newer), and a
    // run that fails here has confused SDL's choice with a defect.
    if (version < 0x00010004)
        return sdl_fail("libSDL2's JNI_OnLoad returned a version older than 1.4");

    // SDLActivity.onCreate's JNI half, in its order: SDL.setupJNI() runs the
    // three nativeSetupJNI natives (they double as a version check), and
    // SDL_SetMainReady() is what SDL_main-wrapped entry points set on the way
    // through — without it SDL_Init refuses with the "did you include
    // SDL_main.h" complaint no matter who called nativeRunMain.
    {
        typedef int (*setupjni_fn)(void *, void *);
        const char *cls[] = { SDL_ACTIVITY,
                              "org/libsdl/app/SDLAudioManager",
                              "org/libsdl/app/SDLControllerManager" };
        for (unsigned i = 0; i < sizeof cls / sizeof *cls; i++) {
            setupjni_fn fn = (setupjni_fn)kl_jni_native(cls[i], "nativeSetupJNI", NULL);
            if (!fn) {
                if (out) fprintf(out, "  [sdl] %s.nativeSetupJNI not registered\n", cls[i]);
                continue;
            }
            kl_jni_local_frame_push();
            // The jclass is the second argument, and it is LOAD-BEARING: SDL
            // takes a global ref to it and resolves every static callback it
            // will ever make (getContext, getNativeSurface, initTouch, ...)
            // against it. NULL here is exactly the "<unknown-jclass>" this
            // door's first run stopped on — see kl_jni.h's kl_jni_class note.
            int v = fn(kl_jni_env(), kl_jni_class(cls[i]));
            kl_jni_local_frame_pop();
            if (out) fprintf(out, "  [sdl] %s.nativeSetupJNI -> 0x%08x\n", cls[i], v);
        }
        typedef void (*mainready_fn)(void);
        mainready_fn mr = (mainready_fn)kl_sym(sdl, "SDL_SetMainReady");
        if (mr) mr();
    }

    // ...and SDLActivity.onCreate line 412, `HIDDeviceManager.acquire(this)`,
    // which is NOT optional and is not about HID devices.
    //
    // acquire() constructs the manager, whose constructor calls the private
    // native HIDDeviceRegisterCallback(). That native's whole job on the C side
    // is to cache three things libSDL2's Android hidapi backend has no other
    // way to get: the JavaVM (env->GetJavaVM), a pthread key for the per-thread
    // JNIEnv, and a global ref to the manager object. Nothing else in SDL sets
    // them.
    //
    // Skip it and the crash is nowhere near HID. `SDL_Init(SDL_INIT_JOYSTICK)`
    // -> HIDAPI_JoystickInit -> SDL_hid_init -> PLATFORM_hid_init, which after
    // an `SDL_GetAndroidSDKVersion() >= 18` check does
    // `g_JVM->AttachCurrentThread(...)` on a NULL g_JVM — the first thing
    // Simpsons Hit & Run's SDL_main does, and a SIGSEGV at pc=0 with the
    // engine nowhere on the stack (host run 1: `fault: signal 11 at 0x0,
    // libSDL2.so+0xf995c [PLATFORM_hid_init+0x50]`, called from SDL_main+0x128).
    //
    // NOT run for the srceng guest. It is the same Java there, but hl2vr is a
    // finished port whose SDL never reaches hid_init, and a door change that
    // hands its libSDL2 a new global ref is a behaviour change to a confirmed
    // build that no one has launched — the charter's "a crossing needs its own
    // launch". Drop the gate when hl2vr next gets a round of its own.
    if (!sdl_is_srceng()) {
        typedef void (*hidreg_fn)(void *, void *);
        hidreg_fn reg = (hidreg_fn)kl_sym(sdl,
            "Java_org_libsdl_app_HIDDeviceManager_HIDDeviceRegisterCallback");
        if (!reg) {
            if (out) fprintf(out, "  [sdl] no HIDDeviceRegisterCallback export "
                                  "— this libSDL2 has no Android hidapi backend\n");
        } else {
            kl_jni_local_frame_push();
            reg(kl_jni_env(), kl_jni_new_object(HID_MANAGER));
            kl_jni_local_frame_pop();
            if (out) fprintf(out, "  [sdl] HIDDeviceManager.acquire() — the JavaVM "
                                  "and the manager ref hidapi caches (SDL_hid_init "
                                  "dereferences them unconditionally)\n");
        }
    }

    // ---- the screen resolution, which NOBODY HAS EVER TOLD SDL on this door --
    //
    // SDL learns how big its window is from exactly one place on Android:
    // SDLSurface.surfaceChanged calls nativeSetScreenResolution and then
    // onNativeResize. This door has never called either. The Steam Link door
    // does (kl_slink_sdl_start_main); the generic SDL door skipped it because
    // hl2vr never asked.
    //
    // Simpsons Hit & Run asks. `SDL_GL_GetDrawableSize` is the ONLY size
    // function it imports out of libSDL2, and its Pure3D renderer turns the
    // answer into a scissor rectangle: `pglContext::SetScissor(pddiRect*)`,
    // reached through pddiBaseContext's virtual, is the only caller of
    // glScissor in the whole guest -- and this runtime measured the two calls
    // it makes in a launch as `glScissor(0, 0, 0, 0)`, from
    // libmain.so+0x6c1178, which is +0x208 into that function.
    //
    // A zero scissor box with GL_SCISSOR_TEST enabled discards every fragment,
    // glClear included, with no GL error and a complete framebuffer. That is
    // the shape of every measurement this port has taken since round 4.
    //
    // So the fix belongs HERE, at the size answer, and not in a rewrite of the
    // box: SDL is told what its surface is, once, before the guest's main
    // runs, exactly as the Java would have.
    //
    // NOT run for the srceng guest, on the same reasoning as the HID acquire
    // above: hl2vr is a finished, Austin-confirmed port on this door, and
    // giving its SDL a screen resolution it has never had is a behaviour change
    // to a build nobody has launched. KL_SDL_SCREEN_RES=0 turns it off here,
    // =1 forces it on for srceng too when hl2vr next gets a round.
    {
        const char *res_env = kl_env_str("KL_SDL_SCREEN_RES", NULL);
        // "0" is off, "1" is on, "WxH" is on AND states the size. A bare unset
        // follows the srceng gate.
        int res_on = res_env ? (strcmp(res_env, "0") != 0) : !sdl_is_srceng();
        if (res_on) {
        void *setres = kl_jni_native(SDL_ACTIVITY, "nativeSetScreenResolution", NULL);
        void *resize = kl_jni_native(SDL_ACTIVITY, "onNativeResize", NULL);
        int32_t w = 0, h = 0;
        kl_ndk_window_size(NULL, &w, &h);
        // ...and the size can be STATED, because the placeholder window is not
        // necessarily the drawable the guest renders into. Measured on the
        // host: told 1832x1920, this guest answers glScissor(0,0,1832,1920) --
        // exactly what it was told, which is the proof that
        // SDL_GL_GetDrawableSize is where its rectangle comes from. But its eye
        // buffers are 2064x2208, so a box of 1832x1920 clips the top and right
        // of every eye. On a Quest the Android surface and the eye are the same
        // numbers and the box covers the eye exactly; KL_SDL_SCREEN_RES=WxH is
        // how a target says so here.
        const char *spec = kl_env_str("KL_SDL_SCREEN_RES", NULL);
        if (spec && strchr(spec, 'x')) {
            int sw = 0, sh = 0;
            if (sscanf(spec, "%dx%d", &sw, &sh) == 2 && sw > 0 && sh > 0) {
                w = sw; h = sh;
            }
        }
        if (w > 0 && h > 0 && setres) {
            kl_jni_local_frame_push();
            // (IIIIF): surface w/h, device w/h, refresh rate. Read off this
            // guest's own registration, not assumed -- SDL3 spells it (IIIIFF)
            // and calling one through the other's prototype is a silent
            // argument shift.
            ((void (*)(void *, void *, int32_t, int32_t, int32_t, int32_t, float))
             setres)(kl_jni_env(), kl_jni_class(SDL_ACTIVITY), w, h, w, h, 90.0f);
            kl_jni_local_frame_pop();
            if (out) fprintf(out, "  [sdl] nativeSetScreenResolution(%d,%d, %d,%d, "
                                  "90.0) — SDL had no window size at all before "
                                  "this, and this guest scissors to it\n",
                             w, h, w, h);
            if (resize) {
                kl_jni_local_frame_push();
                ((void (*)(void *, void *))resize)(kl_jni_env(),
                                                   kl_jni_class(SDL_ACTIVITY));
                kl_jni_local_frame_pop();
                if (out) fprintf(out, "  [sdl] onNativeResize()\n");
            }
            // ...and the guest's OWN render-resolution native, if it has one.
            //
            // Simpsons Hit & Run's port author added
            // SDLSurface.nativeSetSHARRenderResolution(int,int) and calls it
            // from surfaceChanged. Nothing here has ever called it either, so
            // the engine takes its FALLBACK -- which the guest names in its own
            // log:
            //
            //   SHAR Android PDDI display resolution [init-display-sdl-fallback]:
            //   2064x2208
            //
            // That fallback is why five launches ran at Quest 3's native per-eye
            // size against a device recommending 5120x4107, and it is why the
            // picture is soft. Told a size, the guest logs "SHAR Android Java
            // render resolution received: WxH" instead and builds its eye
            // buffers from it.
            //
            // It gets THE SAME NUMBER as SDL, and that is the whole point of
            // sending it from here rather than from a second knob: the render
            // resolution and the scissor rectangle must agree, and this guest
            // derives the scissor from SDL_GL_GetDrawableSize. Two sources for
            // one size is how the world came to be scissored to nothing.
            {
                typedef void (*shar_res_fn)(void *, void *, int32_t, int32_t);
                kl_image *lib = kl_find_image(g_entry_lib);
                shar_res_fn shar = lib ? (shar_res_fn)kl_sym(lib,
                    "Java_org_libsdl_app_SDLSurface_nativeSetSHARRenderResolution")
                    : NULL;
                if (shar) {
                    kl_jni_local_frame_push();
                    shar(kl_jni_env(), kl_jni_class("org/libsdl/app/SDLSurface"),
                         w, h);
                    kl_jni_local_frame_pop();
                    if (out) fprintf(out, "  [sdl] nativeSetSHARRenderResolution"
                                          "(%d, %d) — the guest's own render "
                                          "resolution, which it was taking from "
                                          "an init-display-sdl-fallback\n", w, h);
                }
                // ...and the OTHER half of the size, which is a different
                // number from a different place: the eye SWAPCHAIN the guest
                // asks the XR runtime for. The PDDI display resolution above
                // does not move it -- told 4128x4416 the guest still requested
                // 2064x2208 -- and it is computed with a render scale the port
                // keeps in vrsettings.cfg beside every other VR setting.
                //
                // Setting it through the guest's OWN state rather than
                // substituting a size in xrCreateSwapchain is the difference
                // between the guest KNOWING its eye is bigger and being handed
                // a bigger one behind its back. Its projection, its viewport,
                // its subImage rect and its scissor all follow from its own
                // number; a swapchain we silently enlarge leaves every one of
                // those describing the old size, which is a picture drawn into
                // one corner.
                //
                // But NOT through SharOpenXR::SetRenderScale(), which round 10
                // called and which cost launch #9 its right-stick turn --
                // sdl_shar_vrsettings has the whole account. The setter saves
                // the entire settings block, and from here that block is still
                // zeroed .bss. Write the file the guest is about to read
                // instead: Initialize() parses it AFTER installing its own
                // defaults, so the file is where the number has to be anyway.
                // Unconditional for this guest, not gated on the scale being
                // asked for: the REPAIR has to run on a launch that states no
                // scale at all, or a tree damaged once stays damaged. A scale
                // of 0 means "leave renderScale as it is and fix the rest".
                const char *rs = kl_env_str("KL_SHAR_RENDER_SCALE", NULL);
                int vrcfg = kl_env_int("KL_SHAR_VRCFG", 1);
                int is_shar = lib && kl_sym(lib,
                    "_ZN10SharOpenXR14SetRenderScaleEf") != NULL;
                if (is_shar && vrcfg) {
                    float sc = (rs && *rs) ? (float)atof(rs) : 0.0f;
                    sdl_shar_vrsettings(out, sc);
                } else if (is_shar && out) {
                    fprintf(out, "  [sdl] KL_SHAR_VRCFG=0 — vrsettings.cfg "
                                 "left exactly as the guest last wrote it\n");
                }
            }
        } else if (out) {
            fprintf(out, "  [sdl] nativeSetScreenResolution %s — SDL keeps "
                         "whatever size it inferred\n",
                    setres ? "has no window geometry to send" : "NOT REGISTERED");
        }
        }
    }

    // ValveActivity2.initNatives, the args half: setArgs("-game hl2 -vr
    // -language <pref>"). Called through the guest's OWN native, the way the
    // Java does, because where the launcher keeps that string is its own
    // business. The language pref defaults to "russian" on a fresh install;
    // english here, and the difference is a string table, not a code path.
    kl_image *launcher = kl_find_image(g_entry_lib);
    if (!launcher) return sdl_fail("the entry library is not in the chain");
    typedef void (*setargs_fn)(void *, void *, void *);
    setargs_fn setargs = (setargs_fn)kl_sym(launcher,
        "Java_com_valvesoftware_ValveActivity2_setArgs");
    if (setargs) {
        kl_jni_local_frame_push();
        char args[128];
        snprintf(args, sizeof args,
                 "-game hl2 -vr -language english +vr_eye_scale %s",
                 sdl_eye_scale());
        setargs(kl_jni_env(), kl_jni_class(VALVE_ACTIVITY),
                kl_jni_new_string(args));
        kl_jni_local_frame_pop();
        if (out) fprintf(out, "  [sdl] setArgs(\"%s\")\n", args);
    } else if (out) {
        fprintf(out, "  [sdl] no setArgs export — the launcher takes SDL's argv only\n");
    }
    if (out) fflush(out);
    return 0;
}

int kl_sdl_run_main(FILE *out) {
    // SDL's nativeRunMain itself — the REGISTERED native, not a dlsym of the
    // entry function. Calling LauncherMainAndroid directly was tried first and
    // it is NOT the same door: without kl_sdl_begin's setup half (the
    // nativeSetupJNI trio and SDL_SetMainReady), SDL_Init(SDL_INIT_VIDEO) fails
    // with SDL's "did you include SDL_main.h" complaint and the launcher never
    // reaches the engine. SDLActivity.java passes (getMainSharedObject(),
    // getMainFunction(), getArguments()): the library NAME as Java spells it,
    // the function, and an empty args array.
    if (out) {
        fprintf(out, "\n=== SDLActivity.nativeRunMain(\"%s\", \"%s\") ===\n",
                g_entry_lib, g_entry_fn);
        fflush(out);
    }
    typedef int (*runmain_fn)(void *, void *, void *, void *, void *);
    runmain_fn runmain = (runmain_fn)kl_jni_native(SDL_ACTIVITY, "nativeRunMain", NULL);
    if (!runmain) { sdl_fail("SDLActivity.nativeRunMain was not registered"); return -1; }
    // ...and the args array is the guest's ARGV, which is not nothing.
    //
    // SDLActivity passes getArguments() straight into nativeRunMain, and SDL
    // hands it to the entry point as argc/argv. An empty array is right for a
    // launcher that takes no options; it is a LOST LEVER for one that does.
    // Simpsons Hit & Run's Pure3D engine parses argv through
    // CommandLineOptions::HandleOption, and its front end -- the language
    // screen the host has never been driven past, because the host presents no
    // controller and the port is driven entirely through OpenXR actions -- is
    // exactly what its own SKIPFE option exists to bypass.
    //
    // KL_SDL_ARGS is a whitespace-separated argv, delivered verbatim. It is a
    // DEVELOPMENT lever, off by default, and it belongs on this path rather
    // than in a target row because every SDL guest gets an argv and only the
    // person debugging one knows what to put in it.
    const char *argstr = kl_env_str("KL_SDL_ARGS", NULL);
    char        argbuf[512];
    const char *argv[32];
    int         argn = 0;
    if (argstr && *argstr) {
        snprintf(argbuf, sizeof argbuf, "%s", argstr);
        for (char *tok = strtok(argbuf, " \t");
             tok && argn < (int)(sizeof argv / sizeof *argv);
             tok = strtok(NULL, " \t"))
            argv[argn++] = tok;
        if (out) {
            fprintf(out, "  [sdl] KL_SDL_ARGS -> %d argv entr%s:",
                    argn, argn == 1 ? "y" : "ies");
            for (int i = 0; i < argn; i++) fprintf(out, " \"%s\"", argv[i]);
            fprintf(out, "\n");
        }
    }
    kl_jni_local_frame_push();
    void *jargs = argn ? kl_jni_new_string_array(argv, argn)
                       : kl_jni_new_string_array(NULL, 0);
    int rc = runmain(kl_jni_env(), kl_jni_class(SDL_ACTIVITY),
                     kl_jni_new_string(g_entry_lib),
                     kl_jni_new_string(g_entry_fn), jargs);
    kl_jni_local_frame_pop();
    if (out) { fprintf(out, "  nativeRunMain returned %d\n", rc); fflush(out); }
    return rc;
}

// NOT for a signal handler, unlike everything kl_fault.c prints. Two of these
// take a lock a running guest thread can be holding — the mutex-owner map
// symbolizes live backtraces, and the AAudio report walks a stream list the
// audio thread rewrites on every restart — so calling this from `m_boot`'s
// budget-expiry handler wedged the run instead of ending it, with the numbers
// it exists to print never reaching the log. The host's handler prints the
// narrow set itself; this is what a NORMAL return path calls.
void kl_sdl_report(FILE *out) {
    if (!out) return;
    // Who is blocked on what, first: this guest owns its frame loop inside
    // nativeRunMain, so there is no return value that says "the engine
    // stopped", and a run that produced no frames looks exactly like one that
    // produced them until the reports are read.
    kl_pthread_report(out);
    kl_egl_report(out);
    kl_opensl_report(out);
    kl_aaudio_report(out);
    kl_mediandk_report(out);
    kl_openxr_report(out);
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}
