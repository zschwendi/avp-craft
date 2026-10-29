// Which guest a run is for — the small set of facts that differ between them.
//
// There are three consumers and they used to be three tables. `mains/m_boot.c`
// said "beatsaber" in one place and let kl_jni.c's defaults say it in four
// more; `visionos/Sources/kl_app.c` carried its own array of literals; and
// `visionos/targets.py` carried the build's copy. A table in three places is a
// table that disagrees with itself, and the way THAT failure presents is the
// worst kind: the build succeeds, the app installs, and the guest is described
// wrongly to itself — Beat Saber's APK opened as SUPERHOT's zip, or a title
// writing its saves into another's userdata directory.
//
// So targets.py stays the authority (it also carries the bundle id, product and
// display name, which only the build needs) and `make targets` generates
// kl_target_table.h from it. This header is the runtime's view of that table.
//
// What a target does NOT decide is where the files are. The host reads them out
// of the repo, the app reads them out of its container, and each one builds its
// own absolute paths from the relative ones here — kl_target_apply_host() is
// the host's half, and kl_app_configure() is the app's.
#ifndef KL_TARGET_H
#define KL_TARGET_H

// Which boot sequence the guest wants. Not a property of the file layout, which
// is why it lives here rather than being sniffed: libmain's JNI_OnLoad and
// kl_slink's front doors are different entry points into different lifecycles,
// and a guest that is asked the wrong question answers by aborting somewhere
// unrelated.
typedef enum {
    KL_GUEST_UNITY = 0,   // libmain -> NativeLoader.load -> libunity -> lifecycle
    KL_GUEST_STEAMLINK,   // kl_slink's shell / OpenXR doors
    // Unreal Engine 4 — kl_ue4's NativeActivity door. A different ENTRY, not a
    // different lifecycle: UE4's GameActivity is a NativeActivity, so the guest
    // is started through ANativeActivity_onCreate and then driven by the
    // callbacks it fills in, where a Unity guest is started through libmain's
    // JNI_OnLoad and driven by calling natives it registered. Everything after
    // the door — the looper, the Choreographer, EGL, the XR seam — is shared.
    KL_GUEST_UE4,
    // JKXR — kl_jkxr's door. Neither of the two above: the guest's natives are
    // static `Java_*` exports on a plain Activity, so there is no
    // RegisterNatives census to drive and no NativeActivity glue to enter
    // through. The driver stands in for the Java by calling those exports in
    // the Activity's own order, threading the handle onCreate returned through
    // every one after it.
    KL_GUEST_JKXR,
    // SDL2's front door (org.libsdl.app.SDLActivity). The engine behind it
    // varies — the first guest on this kind is nillerusr's srceng port — and
    // the kind names the DOOR, not the engine, because the door is what the
    // boot sequence is a transcription of: System.loadLibrary("SDL2") runs
    // libSDL2's JNI_OnLoad, and nativeRunMain("launcher",
    // "LauncherMainAndroid", args) is SDL's own dlopen+dlsym of the entry
    // library's main function. Named SDL rather than SRCENG because a second
    // SDLActivity guest would take the same door regardless of its engine.
    KL_GUEST_SDL,
    // A NATIVE Mach-O guest (the vivecraft row): no APK, no klepton-ld, no ELF.
    // The entry library is a Darwin dylib in the app's Frameworks/ and the door
    // is one exported C function called on a big-stack pthread
    // (kl_app.c's native_*). Nothing in kl_driver runs for it; the only thing
    // the shared driver answers is that it owns its frame loop.
    KL_GUEST_NATIVE,
} kl_guest_kind;

typedef struct {
    const char *name;       // KLEPTON_TARGET / KL_TARGET / m_boot's argv[1]
    const char *tree;       // the unpacked APK's directory
    const char *apk;        // ...and the APK itself, which is load-bearing
    const char *assets;     // <tree>/assets, the AssetManager root
    const char *libdir;     // <tree>/lib/arm64-v8a, relative to the repo root
    const char *entry_lib;  // the library the chain starts at ("libmain")
    // The userdata KEY — the name of the profile directory saves, PlayerPrefs
    // and pairing credentials live in. It is the target's own name unless the
    // table says otherwise, which one row does: Steam Link's credential was
    // written under "steamlink" before the target row existed, and a key that
    // disagrees with an installed profile reads as a deauthorized host rather
    // than as a moved directory. See kl_target_resolve() for the synthesized
    // trees, which drop a trailing "-<digits>" so version swaps share a profile.
    const char *userdata;
    // Where this guest looks for its OBB, RELATIVE to the external-storage
    // root — <userdata>/<obb> on the host, <container>/android-files/<obb> in
    // the app. It is in the table because it is not one layout: a Unity guest
    // asks Java (Context.getObbDirs() -> <files>/obb) and an Unreal guest
    // builds Android's own path itself (Android/obb/<package>/), so staging a
    // UE4 target the Unity way puts gigabytes somewhere the engine never looks
    // — and both engines report a missing OBB by carrying on.
    const char *obb;
    kl_guest_kind kind;
    // Scene policy, read by the SwiftUI scene body (kl_app_target_own_hands /
    // kl_app_target_opaque_world). own_hands: the guest models the wearer's
    // hands itself, so the system must not draw the real ones over the scene
    // (upperLimbVisibility). opaque_world: the guest draws an opaque world and
    // wants the room gone — .full immersion rather than .mixed.
    int own_hands;
    int opaque_world;
    // Engine dialect, not scene policy: UE4.26 and 4.27's GameActivity pass
    // Build.VERSION.SDK_INT as the SECOND argument of
    // nativeSetAndroidVersionInformation where 4.25's (re4) passes five bare
    // strings. Verified against each APK's own GameActivity.smali. kl_ue4.c
    // transcribes that call per target, so the row has to say which spelling
    // the guest's native expects — calling the six-argument native with the
    // five-argument signature faults inside the guest's first string read
    // (tetris, r1: SIGSEGV in klj_str with an int in a jstring's place).
    int ue4_sdk_int;
    // SDL door only: the function SDLActivity.getMainFunction() answers, which
    // libSDL2's nativeRunMain dlsym's out of `entry_lib`. It is in the table
    // for the reason `entry_lib` is — it is a property of the GUEST's Java,
    // read off each APK's own smali, and the two SDL guests here disagree:
    // hl2vr's ValveActivity2 overrides it to "LauncherMainAndroid" out of
    // liblauncher.so, and Simpsons Hit & Run's SimpsonsActivity overrides
    // nothing, so it takes stock SDLActivity's {"SDL2","main"} /
    // "SDL_main" out of libmain.so. NULL on every non-SDL row, and the door
    // treats NULL as SDL's own default.
    const char *sdl_main;
    // Environment this target needs BAKED IN, as a space-separated list of
    // NAME=VALUE (empty for almost every row).
    //
    // It exists because **a device launch has no command line**. Every other
    // knob in this project is an environment variable set in front of
    // `build/m_boot`, and that works right up until the guest is an installed
    // .app the wearer starts from the Home View: there is nowhere to put an env
    // var, so a target that cannot boot without one cannot boot on the device at
    // all. ITR is the first — OnlineSubsystemOculus aborts on "Unable to get a
    // valid UserID" unless KL_PLAT_USER answers one.
    //
    // A value here NEVER overrides one the caller already set: the real
    // environment wins, so a host A/B (`KL_PLAT_USER=0 ./build/m_boot itr`)
    // still does what it says. Each entry is announced when it is applied, and a
    // malformed one is refused BY NAME rather than skipped — a row that silently
    // bakes nothing is a device build that dies the way it did before, with no
    // line in the log saying why.
    const char *env;
} kl_target;

// NULL if the name is not in the table. `kl_target_names()` is for the error
// message that follows.
const kl_target *kl_target_lookup(const char *name);
const kl_target *kl_target_default(void);
const char      *kl_target_names(void);

// What a driver actually calls: turn one argument into a target.
//
//   NULL / ""                     KL_TARGET, else the default (beatsaber)
//   "superhot"                    that target
//   "superhot/lib/arm64-v8a"      ...the same one, named by its libdir
//   "beatsaber-128/lib/arm64-v8a" a tree with no table entry: SYNTHESIZED
//
// The third form is what every documented run command and every Makefile gate
// passes, so it has to keep working exactly as it does. The fourth is the
// version-swap trees (beatsaber-113, -128, -16): a path is taken at its word,
// because refusing it would break the one workflow those directories exist for,
// and a synthesized target reads its assets and APK from BESIDE the libdir
// rather than from whatever the default target happens to be — which is what
// the old hardcoded defaults did, silently pairing 1.28's libraries with
// 1.40's assets.
//
// A synthesized target's `userdata` key drops a trailing "-<digits>", so
// beatsaber-128 and beatsaber share one profile. That is not tidiness: it is
// the documented rule that swapping Beat Saber versions
// must not repeat first-run setup.
//
// Never returns NULL for a path; returns NULL only for an unknown NAME, which
// is a typo the caller should report with kl_target_names().
const kl_target *kl_target_resolve(const char *arg);

// The host's paths, applied: library path, native lib dir, assets, APK and the
// userdata directory. The visionOS app does NOT call this — its files live in
// the app container and kl_app_configure builds those paths itself.
//
// `libdir_override` is for a run that was handed a path outside the tree
// (nothing in-tree does this today); NULL uses the target's own.
void kl_target_apply_host(const kl_target *t, const char *libdir_override);

// The row THIS run is for, for runtime code that is not handed the target and
// must not re-derive it (kl_target_resolve(NULL) reads KL_TARGET, which a host
// run started from argv never sets — that is how one run reported another
// target's OBB path). kl_target_apply_host records it; until something does,
// the table default stands in.
void             kl_target_use(const kl_target *t);
const kl_target *kl_target_current(void);

#endif
