// The Unreal Engine 4 target. See kl_ue4.h for why it is its own file.
#include "kl_ue4.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_jni.h"
#include "kl_target.h"
#include "kl_ndk.h"
#include "kl_nativeactivity.h"
#include "kl_egl.h"
#include "kl_ovrp.h"
#include "kl_ovrplat.h"
#include "kl_mediandk.h"
#include "kl_aaudio.h"
#include "kl_opensl.h"
#include "kl_env.h"
#include "kl_vulkan.h"

// The library the chain ENDS at. Not a target-table lookup: the table says
// "libUE4" as the entry (which is what proves the right guest was embedded on
// device) and this is the file name, which is the same fact spelled for the
// loader. One string, here, rather than two spellings in two files.
#define UE4_LIB "libUE4.so"

// The chain, DEPENDENCIES FIRST, read off libUE4's own DT_NEEDED rather than
// off the Java. There is no staged NativeLoader-style load here: libUE4 binds
// its imports at RELOCATION time, so anything it needs has to be mapped before
// it is. Loading libUE4 alone stops on `__cxa_guard_acquire` inside its own
// DT_INIT_ARRAY, which is the first line of libc++_shared that a static
// initializer reaches.
//
// libUE4's DT_NEEDED also names libovrplatformloader.so, and it is deliberately
// NOT here: that is one of the three libraries this project REPLACES
// (kl_ovrplat), so loading the guest's copy would map an Oculus forwarder to a
// service that does not exist. kl_shim_lookup answers those names instead.
// Same for libOVRPlugin and libvrapi, which are not in the link at all — UE4
// dlopens them, and kl_ovrp claims both.
//
// libbink2androidarm64 and libbinkpluginandroidarm64 are absent for the other
// reason: they are in nobody's DT_NEEDED. RAD's Bink video is dlopen'd by the
// engine when a movie is first played, and kl_load_auto resolves it then.
//
// THE CHAIN IS A UNION ACROSS TITLES, AND EVERY ENTRY BUT UE4_LIB IS OPTIONAL.
// It was re4's DT_NEEDED written out, and deadpool ships the same set, so the
// list read as "what a UE4 guest links" until tetris (UE4.26.2) shipped no
// libovraudio64.so at all and the load stopped on a file that is not part of
// that APK. Which audio spatializer and which online SDK a UE4 project links
// is a PROJECT setting, not an engine one — so a name missing from a title's
// lib dir is that title's answer, not a broken tree. Absent entries are
// skipped and named in the log; UE4_LIB missing is still fatal (see the
// `!g_ue4` check below). Nothing here is silently invented: the skip line
// appears in every boot log for every title that lacks one.
static const char *const UE4_CHAIN[] = {
    "libc++_shared.so",
    "libEOSSDK.so",         // Epic Online Services — tetris links it, re4 and
                            // deadpool do not ship it
    "libovraudio64.so",     // the Oculus audio spatializer — re4/deadpool ship
                            // it, tetris does not
    "libplaycore.so",       // Google Play core — the OBB downloader's half
    UE4_LIB,
};

// UE4's own Java front door. `com.epicgames.ue4.GameActivity` is a
// NativeActivity subclass, which is why the guest reaches
// ANativeActivity_onCreate at all — and the guest asks for its own class by
// name (GameActivity has a large native surface of its own), so answering
// `android/app/NativeActivity` the way Steam Link's VR door does would be
// wrong here in a way that only shows up as a FindClass several layers in.
#define UE4_ACTIVITY "com/epicgames/ue4/GameActivity"

static char g_libdir[1024];
static char g_err[512] = "no error";
static kl_image *g_ue4;

// A native of the guest's, by exported symbol. Registered with the JNI surface
// so `AndroidThunkJava_*` handlers can make the call GameActivity.java would
// have made; see kl_jni.h's kl_jni_set_guest_native_resolver.
static void *ue4_native_symbol(const char *symbol) {
    return (g_ue4 && symbol) ? kl_sym(g_ue4, symbol) : NULL;
}

static int ue4_fail(const char *why) {
    snprintf(g_err, sizeof g_err, "%s", why);
    return 1;
}

const char *kl_ue4_error(void) { return g_err; }

// The engine's command-line OVERRIDE file, named and printed.
//
// UE4's `InitCommandLine` reads `<external>/UE4Game/<project>/UE4CommandLine.txt`
// before anything else, and whatever is in it configures the whole engine —
// threading model, RHI, log verbosity. It is a file in the guest's USERDATA
// directory, which outlives `make clean`, outlives a rebuild, and is not part
// of the tree, so a line left in it by one debugging session silently
// configures every run after it.
//
// That is not hypothetical: `-onethread`, added to test whether a stall was
// threading (it was not — it was the condvar table), stayed behind and made
// every later host run single-threaded. Under it this title's main menu renders
// an all-zero eye, so the run reads as "the guest goes black after the title
// screen" — a rendering bug with no rendering cause, and the file that caused
// it appears nowhere in any log.
//
// Both places are looked at because the base the engine builds this path from
// is not ours to restate; naming what is THERE is honest where reimplementing
// the search would be a second, drifting copy of it.
static void ue4_report_command_line(FILE *out) {
    if (!out) return;
    const char *files = kl_jni_files_dir();
    if (!files || !*files) return;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/UE4Game", files);

    const char *names[8];
    char        held[8][1024];
    size_t      n = 0;
    snprintf(held[n], sizeof held[n], "%s/UE4CommandLine.txt", dir);
    names[n] = held[n]; n++;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < sizeof names / sizeof *names) {
            if (e->d_name[0] == '.') continue;
            snprintf(held[n], sizeof held[n], "%s/%s/UE4CommandLine.txt",
                     dir, e->d_name);
            names[n] = held[n]; n++;
        }
        closedir(d);
    }

    for (size_t i = 0; i < n; i++) {
        FILE *f = fopen(names[i], "rb");
        if (!f) continue;
        char buf[512];
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[got] = 0;
        for (char *p = buf; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        fprintf(out, "  [ue4] command line: %s\n", buf);
        fprintf(out, "  [ue4]   from %s — the ENGINE reads this, and it is in "
                     "userdata rather than in the tree\n", names[i]);
    }
    fflush(out);
}

int kl_ue4_configure(const char *libdir, FILE *out) {
    if (!libdir || !*libdir) return ue4_fail("no library directory");
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);
    // Everything else — assets, apk, files, native lib dir — has already been
    // set from the target row by kl_target_apply_host(). Restating any of it
    // here is how two descriptions of one guest start to drift.
    kl_jni_set_activity_class(UE4_ACTIVITY);
    // ...and the way back. UE4's Java front end calls natives on itself, so a
    // driver standing in for that Java has to be able to make those calls; the
    // JNI surface does not know which image is the guest, and this file does.
    kl_jni_set_guest_native_resolver(ue4_native_symbol);
    if (out) {
        fprintf(out, "  [ue4] activity: %s\n", UE4_ACTIVITY);
        fprintf(out, "  [ue4] userdata: %s\n", kl_jni_files_dir());
        // Named and CENSUSED here, because nothing else on this door will.
        // A Unity guest asks Java where its OBB is and the census rides along
        // on the answer; UE4 builds Android's own path itself and asks nobody,
        // so a completely unstaged 8.5 GB produced no line anywhere — and the
        // engine reports a missing OBB the way it reports a missing .uproject,
        // by carrying on.
        fprintf(out, "  [ue4] obb:      %s\n", kl_jni_obb_dir());
        fflush(out);
        ue4_report_command_line(out);
    }
    return 0;
}

int kl_ue4_load(FILE *out) {
    if (out) {
        fprintf(out, "=== the chain (%zu libraries, dependencies first) ===\n",
                sizeof UE4_CHAIN / sizeof *UE4_CHAIN);
        fflush(out);
    }
    // Mapped and relocated in order, then the initializers are run in the same
    // order — NOT interleaved. libUE4's static initializers reach straight into
    // libc++_shared, so a chain that ran each library's DT_INIT_ARRAY as it
    // loaded would be running libUE4's against a libc++ whose own had not
    // happened yet.
    for (size_t i = 0; i < sizeof UE4_CHAIN / sizeof *UE4_CHAIN; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, UE4_CHAIN[i]);
        // Optional-by-absence — see the UE4_CHAIN note. The test is whether the
        // library can be LOADED AT ALL, not whether an ELF sits at that path: on
        // device the .so tree is deliberately not in the bundle and every guest
        // library is a translated framework, so an access(path) here answered
        // "absent" for libc++_shared, libEOSSDK and libplaycore on a device that
        // was carrying all three. The chain then ran libUE4's DT_INIT_ARRAY with
        // no libc++ under it and died instantly on the first function-local
        // static — `fatal: called unresolved import '__cxa_guard_acquire'`,
        // which is exactly the failure the top-of-file note describes. kl_can_load
        // is the one place that resolves a guest library name; asking anything
        // else on the guest's behalf is the documented mistake (klepton.h).
        //
        // Still an existence test and not a load: a library that is present and
        // fails to map is a failure, and says so.
        if (strcmp(UE4_CHAIN[i], UE4_LIB) && !kl_can_load(path)) {
            if (out) {
                fprintf(out, "  skip   %-22s (not shipped with this title)\n",
                        UE4_CHAIN[i]);
                fflush(out);
            }
            continue;
        }
        kl_image *img = kl_load_auto(path);
        if (!img) {
            snprintf(g_err, sizeof g_err, "%s: %s", UE4_CHAIN[i], kl_error());
            return 1;
        }
        kl_register_image(UE4_CHAIN[i], img);
        if (!strcmp(UE4_CHAIN[i], UE4_LIB)) g_ue4 = img;
        // The LOAD ADDRESS is printed, in the shape tools/symbolize_sample.py
        // parses (`<soname> @0x... N.NN MB`). `sample <pid>` is the diagnostic
        // that keeps paying off here — a UE4 guest owns its own frame loop, so
        // "what is the game thread doing" is a question no return value answers
        // — and it reports every guest frame as `??? (in <unknown binary>)`
        // until something joins the two. Without this line the sampler is
        // useless on the one target that needs it most.
        if (out) {
            fprintf(out, "  mapped %-22s @%p %7.2f MB\n", UE4_CHAIN[i],
                    kl_base(img), kl_span(img) / 1048576.0);
            fflush(out);
        }
    }
    if (!g_ue4) return ue4_fail(UE4_LIB " is not in the chain");

    for (size_t i = 0; i < sizeof UE4_CHAIN / sizeof *UE4_CHAIN; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, UE4_CHAIN[i]);
        kl_image *img = kl_find_image(path);
        if (!img) continue;
        if (out) { fprintf(out, "  init %s\n", UE4_CHAIN[i]); fflush(out); }
        kl_run_init(img);
    }

    // JNI_OnLoad — for EVERY library in the chain that exports one, not just
    // libUE4.
    //
    // UE4 exports all three doors — JNI_OnLoad, ANativeActivity_onCreate and
    // android_main — and Android runs them in that order: System.loadLibrary
    // first (which is what calls JNI_OnLoad), then the activity, then the
    // engine's own thread inside android_main.
    //
    // The part that was missing is that GameActivity.java calls
    // System.loadLibrary for each packaged library, so Android's linker calls
    // JNI_OnLoad on each of them — that is the ONLY moment a library gets
    // handed a JavaVM*, and a library loaded as somebody's DT_NEEDED never
    // gets one. Running it for libUE4 alone left every other library in the
    // chain without a VM, and the cost of that was invisible until it was
    // enormous: libEOSSDK.so caches the VM in its JNI_OnLoad, and without it
    // `EOS_Initialize` returns **17000, EOS_Android_JavaVMNotStored**. Epic
    // Online Services then never starts, the Redpoint subsystem factory
    // produces nothing, and Tetris Effect dereferences the resulting null
    // `GetOnlineSubsystem(FName("RedpointEOS"))` at level BeginPlay — a
    // SIGSEGV an hour of rendering work away from the missing call.
    //
    // Chain order, dependencies first, with libUE4 last exactly as it is in
    // UE4_CHAIN: a library's JNI_OnLoad may call into what it links against.
    //
    // NOT fatal if a library has no JNI_OnLoad, and not fatal if libUE4's is
    // absent either, unlike the Unity path where JNI_OnLoad IS the entry
    // point. Here it is the engine registering its GameActivity natives, and a
    // build that registers them statically instead would be a different shape
    // rather than a broken one — so this reports and carries on, and the
    // NativeActivity door is what the run actually turns on.
    typedef int (*jni_onload_fn)(void *vm, void *reserved);
    int ue4_saw_onload = 0;
    for (size_t i = 0; i < sizeof UE4_CHAIN / sizeof *UE4_CHAIN; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", g_libdir, UE4_CHAIN[i]);
        kl_image *img = kl_find_image(path);
        if (!img) continue;
        jni_onload_fn onload = (jni_onload_fn)kl_sym(img, "JNI_OnLoad");
        if (!onload) continue;
        kl_jni_local_frame_push();
        int version = onload(kl_jni_vm(), NULL);
        kl_jni_local_frame_pop();
        if (img == g_ue4) ue4_saw_onload = 1;
        if (out) {
            fprintf(out, "  JNI_OnLoad %-22s returned 0x%08x\n",
                    UE4_CHAIN[i], version);
            fflush(out);
        }
    }
    if (!ue4_saw_onload && out)
        fprintf(out, "  no JNI_OnLoad in " UE4_LIB
                     " — the activity door is the entry\n");
    return 0;
}

unsigned kl_ue4_gap(FILE *out) {
    if (!g_ue4) return 0;
    unsigned n = 0;
    const char *const *miss = kl_missing_imports(g_ue4, &n);
    if (!out) return n;
    // Uncategorised, deliberately: t_load already sorts a library's gap into
    // families and this is the RUNTIME's view of the same question after the
    // whole chain is up, where the interesting property is which names survived
    // rather than what kind they are. Four to a line, as t_load prints them, so
    // the two lists can be diffed by eye.
    fprintf(out, "  %s: %u unique unresolved import%s\n",
            UE4_LIB, n, n == 1 ? "" : "s");
    for (unsigned i = 0; i < n; i++) {
        if (i % 4 == 0) fprintf(out, "      ");
        fprintf(out, "%-28s", miss[i]);
        if (i % 4 == 3 || i + 1 == n) fprintf(out, "\n");
    }
    fflush(out);
    return n;
}

// ---- GameActivity.java's own half of the boot ----
//
// The NativeActivity door is only half of how a UE4 guest starts, and the half
// that is missing is invisible: `ANativeActivity_onCreate` spawns the game
// thread through the NDK's app glue, and that thread's FIRST act inside
// `android_main` is to spin waiting for `GResumeMainInit`, which nothing native
// ever sets. It is set by `nativeResumeMainInit`, one of twenty-eight natives
// GameActivity.java calls on itself — so on Android the engine is configured
// from JAVA, by the activity, and released by the activity.
//
// That is a genuinely different shape from every guest here so far. A Unity
// guest's Java front end hands libunity a few objects and then libunity drives
// itself; UE4's hands the engine its file paths, its device strings, its window
// geometry and its OBB identity as ARGUMENTS, and until they arrive the engine
// has not started. Nothing about it is optional and nothing about it fails by
// name: the guest simply sits, forever, with every counter healthy — which is
// exactly what the first runs of this target looked like.
//
// So this is a transcription of `GameActivity.onCreate` and `onResume` from
// THIS APK's smali, in their order, calling the natives libUE4 exports by
// symbol. The values are read from the same places the Java reads them — the
// manifest <meta-data> block, the files directory, the package name and version
// — rather than being restated here, because a constant transcribed twice is a
// constant that goes stale on a guest swap.
//
// The natives are static exports (`Java_com_epicgames_ue4_GameActivity_native*`)
// rather than `RegisterNatives` bindings, which is why the JNI surface reports
// zero natives registered on this target and is not a gap.
typedef unsigned char ue4_jboolean;

// One typedef per shape, named after the Java signature it is a transcription
// of. A function-pointer type cannot be a macro argument (its commas are the
// macro's), and naming them after the descriptor keeps the call sites checkable
// against the smali line above each one.
typedef void (*ue4_fn_v)(void *, void *);
typedef void (*ue4_fn_ZI)(void *, void *, ue4_jboolean, int);
typedef void (*ue4_fn_Z)(void *, void *, ue4_jboolean);
typedef void (*ue4_fn_ZZssZsZ)(void *, void *, ue4_jboolean, ue4_jboolean,
                               void *, void *, ue4_jboolean, void *, ue4_jboolean);
typedef void (*ue4_fn_sssss)(void *, void *, void *, void *, void *, void *, void *);
// ...and 4.26/4.27's spelling of the same native, with Build.VERSION.SDK_INT
// second. The target row picks between them (kl_target.h, ue4_sdk_int).
typedef void (*ue4_fn_sIssss)(void *, void *, void *, int, void *, void *,
                              void *, void *);
typedef void (*ue4_fn_ssIIs)(void *, void *, void *, void *, int, int, void *);

#define UE4_NATIVE_PREFIX "Java_com_epicgames_ue4_GameActivity_"

static void *ue4_native(const char *name) {
    if (!g_ue4) return NULL;
    char sym[256];
    snprintf(sym, sizeof sym, UE4_NATIVE_PREFIX "%s", name);
    return kl_sym(g_ue4, sym);
}

// A missing native is reported and skipped rather than fatal: this table is a
// transcription of one build's Java, and a build that dropped a setter is a
// different shape rather than a broken one. What must not happen silently is
// the opposite — a setter present and never called — so every one of these is
// named either way.
#define UE4_CALL(out, name, type, ...)                                          \
    do {                                                                         \
        type fn__ = (type)ue4_native(name);                                       \
        if (!fn__) {                                                              \
            if (out) fprintf(out, "  [ue4] %s — not exported, skipped\n", name);   \
        } else {                                                                  \
            if (out) { fprintf(out, "  [ue4] %s\n", name); fflush(out); }          \
            fn__(env, thiz, ##__VA_ARGS__);                                        \
        }                                                                          \
    } while (0)

// `<meta-data android:value="true"/>` as the Java reads it: Bundle.getBoolean
// answers false for anything that is not the literal "true", including an
// absent key.
static ue4_jboolean ue4_meta_bool(const char *key) {
    const char *v = kl_jni_manifest_meta(key);
    return (ue4_jboolean)(v && strcmp(v, "true") == 0);
}

static int ue4_meta_int(const char *key, int dflt) {
    const char *v = kl_jni_manifest_meta(key);
    return v ? (int)strtol(v, NULL, 10) : dflt;
}

static const char *ue4_meta_str(const char *key) {
    const char *v = kl_jni_manifest_meta(key);
    return v ? v : "";
}

// Portrait or landscape, as `Configuration.orientation == ORIENTATION_PORTRAIT`.
// A headset is landscape, and it is the same answer the display panel gives
// everywhere else in this project.
#define UE4_PORTRAIT 0

static void kl_ue4_java_create(FILE *out) {
    void *env  = kl_jni_env();
    void *thiz = kl_jni_activity();
    if (!env || !thiz) return;

    if (out) { fprintf(out, "=== GameActivity.onCreate (the Java half) ===\n"); fflush(out); }

    // nativeSetGlobalActivity(bUseExternalFilesDir, bPublicLogFiles,
    //                         internalFilePath, externalFilePath,
    //                         bOBBinAPK, APKPath, bIsNotDebuggable)
    //
    // Both file paths are the guest's userdata directory here. On Android they
    // are `getFilesDir()` and `getExternalFilesDir(null)`, two directories the
    // app owns; here there is one, and handing over two names for it is what
    // makes `bUseExternalFilesDir` a choice with no consequence rather than a
    // fork into a tree that does not exist: a resource with two doors has to
    // resolve to one place by construction.
    const char *files = kl_jni_files_dir();
    void *jinternal = kl_jni_new_string(files);
    void *jexternal = kl_jni_new_string(files);
    void *japk      = kl_jni_new_string(kl_jni_apk_path());
    UE4_CALL(out, "nativeSetGlobalActivity", ue4_fn_ZZssZsZ,
             ue4_meta_bool("com.epicgames.ue4.GameActivity.bUseExternalFilesDir"),
             ue4_meta_bool("com.epicgames.ue4.GameActivity.bPublicLogFiles"),
             jinternal, jexternal,
             ue4_meta_bool("com.epicgames.ue4.GameActivity.bPackageDataInsideApk"),
             japk,
             // The Java's last argument is `(ApplicationInfo.flags &
             // FLAG_DEBUGGABLE) == 0`. A release APK unpacked from a device is
             // not debuggable.
             (ue4_jboolean)1);

    // nativeSetWindowInfo(bIsPortrait, DepthBufferPreference)
    UE4_CALL(out, "nativeSetWindowInfo", ue4_fn_ZI,
             (ue4_jboolean)UE4_PORTRAIT,
             ue4_meta_int("com.epicgames.ue4.GameActivity.DepthBufferPreference", 0));

    // nativeSetAndroidStartupState(bDebuggerAttached)
    UE4_CALL(out, "nativeSetAndroidStartupState", ue4_fn_Z,
             (ue4_jboolean)0);

    // nativeSetAndroidVersionInformation(Build.VERSION.RELEASE, [Build.VERSION.SDK_INT,]
    //                                    Build.MANUFACTURER, Build.MODEL,
    //                                    Build.DISPLAY, locale)
    //
    // The SDK_INT argument is 4.26's and 4.27's; 4.25 (re4) passes five bare
    // strings. The TARGET ROW says which, because the two are not
    // distinguishable from the native's address and getting it wrong is not a
    // wrong answer but a fault: tetris's first host boot called the
    // six-argument native with the five-argument signature and died in
    // klj_str reading an int as a jstring. SDK_INT is 29, the same answer
    // kl_nativeactivity's sdkVersion gives.
    // Read through kl_jni's Build table, so this guest is told the same device
    // the JNI surface describes to every other one.
    char lang[16] = "en", country[16] = "US";
    kl_jni_locale_parts(lang, sizeof lang, country, sizeof country);
    char locale[40];
    snprintf(locale, sizeof locale, "%s_%s", lang, country);
    void *jrel   = kl_jni_new_string(kl_jni_build_string("VERSION.RELEASE"));
    void *jmake  = kl_jni_new_string(kl_jni_build_string("MANUFACTURER"));
    void *jmodel = kl_jni_new_string(kl_jni_build_string("MODEL"));
    void *jbuild = kl_jni_new_string(kl_jni_build_string("DISPLAY"));
    void *jloc   = kl_jni_new_string(locale);
    if (kl_target_current()->ue4_sdk_int)
        UE4_CALL(out, "nativeSetAndroidVersionInformation", ue4_fn_sIssss,
                 jrel, 29, jmake, jmodel, jbuild, jloc);
    else
        UE4_CALL(out, "nativeSetAndroidVersionInformation", ue4_fn_sssss,
                 jrel, jmake, jmodel, jbuild, jloc);

    // nativeSetObbInfo(ProjectName, PackageName, Version, PatchVersion, AppType)
    // The Java passes `PackageInfo.versionCode` for Version and a literal 0 for
    // PatchVersion — this is what the engine builds `main.<version>.<package>.obb`
    // out of, so it is the same fact klj_obb_census checks the staged files
    // against and it comes from the same place (apktool.yml, via kl_jni).
    long version = 0;
    kl_jni_guest_version(&version, NULL);
    void *jproj = kl_jni_new_string(ue4_meta_str("com.epicgames.ue4.GameActivity.ProjectName"));
    void *jpkg  = kl_jni_new_string(kl_jni_guest_package());
    void *jtype = kl_jni_new_string(ue4_meta_str("com.epicgames.ue4.GameActivity.AppType"));
    UE4_CALL(out, "nativeSetObbInfo", ue4_fn_ssIIs,
             jproj, jpkg, (int)version, 0, jtype);
}

// ...and onResume's half, which is the one that starts the engine: the game
// thread has been spinning on GResumeMainInit since ANativeActivity_onCreate
// returned. GameActivity.onResume re-states the window geometry first (the
// orientation can have changed while the activity was away) and then releases
// it, in that order, so the engine's first look at the window is at a value
// somebody set.
static void kl_ue4_java_resume(FILE *out) {
    void *env  = kl_jni_env();
    void *thiz = kl_jni_activity();
    if (!env || !thiz) return;

    if (out) { fprintf(out, "=== GameActivity.onResume (the Java half) ===\n"); fflush(out); }

    UE4_CALL(out, "nativeSetWindowInfo", ue4_fn_ZI,
             (ue4_jboolean)UE4_PORTRAIT,
             ue4_meta_int("com.epicgames.ue4.GameActivity.DepthBufferPreference", 0));

    // The other arm of the Java's `if` is nativeOnInitialDownloadStarted, i.e.
    // "the OBB is not here, go and fetch it". There is no downloader here and
    // the OBB is staged, so this is the arm a device with its data present
    // takes.
    UE4_CALL(out, "nativeResumeMainInit", ue4_fn_v);
}

int kl_ue4_create(FILE *out) {
    if (!g_ue4) return ue4_fail(UE4_LIB " was never loaded");
    if (kl_na_create(g_ue4, "ANativeActivity_onCreate", out) != 0)
        return ue4_fail(UE4_LIB " exports no ANativeActivity_onCreate");
    // After the super call, exactly as GameActivity.onCreate does it: the
    // activity's own body runs on the UI thread while the game thread it just
    // spawned is already spinning.
    kl_ue4_java_create(out);
    return 0;
}

void kl_ue4_start(FILE *out) {
    kl_na_start(out);
    kl_ue4_java_resume(out);
}
void kl_ue4_stop(FILE *out)  { kl_na_stop(out); }

// ------------------------------------------------------- mobile multiview
//
// UE4's OpenGL RHI decides mobile multiview ONCE, into two exported globals,
// and every later decision reads them:
//
//   FOpenGLES::bSupportsMobileMultiView   set in FOpenGLES::ProcessExtensions,
//                                         true only when the GL_EXTENSIONS
//                                         string names GL_OVR_multiview,
//                                         GL_OVR_multiview2 AND
//                                         GL_OVR_multiview_multisampled_-
//                                         render_to_texture, and both
//                                         glFramebufferTexture*MultiviewOVR
//                                         resolve through eglGetProcAddress
//   GSupportsMobileMultiView              copied from it during RHI init
//
// GSupportsMobileMultiView is what GLSLToDeviceCompatibleGLSL branches on when
// it rewrites every cooked shader (read at libUE4+0x9f0a620 in Tetris Effect):
//
//   true  -> it PREPENDS "#extension GL_OVR_multiview2 : enable" and
//            "layout(num_views = 2) in;" to the shader
//   false -> it prepends "#define gl_ViewID_OVR 0" instead
//
// That false branch is UE4's OWN fallback for a device without the extension,
// and it turns the identical cooked shader into ordinary single-view ES 3.0.
// So the multiview wall this gateway hits — ANGLE's Metal backend implements
// no multiview at all, so every such shader fails to compile, the link fails,
// and UE4 turns the Fatal into a ForceQuit — is not a wall that has to be
// climbed. It only has to be told the truth.
//
// This gateway does NOT name any of the three extensions and does NOT serve
// the two entry points, so the honest value of both flags is 0. Writing them
// is belt-and-braces against a path that sets them from something other than
// the extension string; when they are already 0 this costs one byte read per
// pump tick and says nothing.
//
// EVERY WORD ABOVE IS ABOUT OPENGL ES, so the clear is gated on the guest
// actually using the GL gateway (kl_vulkan_guest_active(), inside the
// function). A Vulkan RHI rewrites no cooked GLSL and never reads an ANGLE
// extension string; clearing there only forces UE4's two-sequential-view
// DoubleWide mobile stereo path, which cost Into the Radius eleven launches of
// vanishing static meshes (docs/r25-multiview.md).
//
// KL_UE4_MULTIVIEW is a TRI-STATE override of that decision: unset = decide
// from the RHI, 1 = never clear (the A/B, and the way back if a future ANGLE
// grows GL_OVR_multiview2), 0 = always clear (the pre-r25 behaviour, and the
// way back if a Vulkan guest is ever found that needs the clear).
static void ue4_no_multiview(void) {
    static uint8_t *gsupports, *bsupports;
    static int looked, said, said_vk, ever_vk;
    static uint8_t saved_g, saved_b;
    if (!g_ue4) return;
    // TRI-STATE, and the unset state is the interesting one:
    //   unset -> decide from the RHI the guest actually brought up (below);
    //   1     -> always leave the guest's answer alone;
    //   0     -> always clear, which is the pre-r25 behaviour and the way back
    //            if a Vulkan guest is ever found that needs the clear.
    const char *knob = kl_env_str("KL_UE4_MULTIVIEW", NULL);
    int force_keep  = knob && kl_env_on("KL_UE4_MULTIVIEW", 0);
    int force_clear = knob && !force_keep;
    if (force_keep) return;
    if (!looked) {
        looked = 1;
        gsupports = ue4_native_symbol("GSupportsMobileMultiView");
        bsupports = ue4_native_symbol("_ZN9FOpenGLES24bSupportsMobileMultiViewE");
    }
    // THE CLEAR IS A STATEMENT ABOUT THE GL GATEWAY, so it may only be made
    // about a guest that is using it. ANGLE names no GL_OVR_multiview
    // extension, UE4 prepends `layout(num_views = 2) in;` to every cooked
    // shader when the flag is true, and the compile failure becomes a
    // ForceQuit — that is a real wall for a GL guest and the reason this
    // function exists. A VULKAN RHI rewrites no cooked GLSL and ANGLE is not in
    // the picture: clearing there buys nothing and forces UE4 onto the
    // two-sequential-view DoubleWide mobile stereo path the title never shipped
    // with. Measured on Into the Radius over eleven launches of vanishing
    // static meshes — docs/r25-multiview.md.
    //
    // kl_vulkan_guest_active() is the honest test rather than a renderer enum:
    // it is a measurement of what the guest DID (resolved vk* symbols AND
    // created a VkDevice through us, i.e. it is inside FVulkanDynamicRHI::Init)
    // rather than a guess. It goes true before the first shader compile reads
    // the flag, but AFTER some number of pump ticks — so an early tick can
    // clear a flag the Vulkan RHI set moments earlier. Hence save-and-restore
    // rather than merely declining future clears.
    // Sticky: once a Vulkan device has been seen, the guest is a Vulkan guest
    // for good — at quit vkDestroyDevice leaves none, and a tick after that
    // would otherwise clear (and announce clearing) a flag nothing reads any
    // more (Deadpool launch 2, 2026-10-09).
    if (kl_vulkan_guest_active()) ever_vk = 1;
    if (!force_clear && ever_vk) {
        if (saved_g && gsupports) *gsupports = saved_g;
        if (saved_b && bsupports) *bsupports = saved_b;
        saved_g = saved_b = 0;
        if (!said_vk) {
            said_vk = 1;
            fprintf(stderr, "  [ue4] mobile multiview left alone — this guest "
                            "brought up a VULKAN device, so no cooked GLSL is "
                            "rewritten and the GL gateway's extension string "
                            "decides nothing. KL_UE4_MULTIVIEW=0 forces the "
                            "clear back on.\n");
        }
        return;
    }
    int was = (gsupports && *gsupports) || (bsupports && *bsupports);
    if (gsupports && *gsupports) saved_g = *gsupports;
    if (bsupports && *bsupports) saved_b = *bsupports;
    if (gsupports) *gsupports = 0;
    if (bsupports) *bsupports = 0;
    if (was && !said) {
        said = 1;
        fprintf(stderr, "  [ue4] mobile multiview was ON — cleared "
                        "(GSupportsMobileMultiView / FOpenGLES::"
                        "bSupportsMobileMultiView). The GL gateway names no "
                        "GL_OVR_multiview extension, so UE4's own non-multiview "
                        "shader path is the correct one; KL_UE4_MULTIVIEW=1 "
                        "leaves the guest's answer alone.\n");
    }
}

double kl_ue4_pump(double seconds, const volatile int *quit) {
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds); t++) {
        kl_ndk_pump_looper(100);
        // Cheap and idempotent: the RHI decides multiview during its own init,
        // which is somewhere inside this pump, and the answer is read much
        // later at the first shader compile.
        ue4_no_multiview();
        // The UI thread's task queue and the frame clock, on the same thread
        // that turns the looper — which is what makes it the UI thread by the
        // only definition that matters: "am I the UI thread" is answered by
        // WHO DRAINS THE QUEUE.
        if ((t + 1) % 10 == 0) kl_jni_drain_ui_tasks();
        kl_jni_tick_choreographer();
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec)
                + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

void kl_ue4_report(FILE *out) {
    if (!out) return;
    // Who is blocked on what, first — this guest owns its own threads and its
    // own frame loop, so unlike the Unity path there is no return value that
    // says "the engine stopped". A UE4 run that produced no frames looks
    // exactly like one that produced frames until the reports are read, and
    // the mutex owner map is the one instrument that separates "still working"
    // from "two of its threads are waiting on each other".
    kl_pthread_report(out);
    kl_egl_report(out);
    kl_opensl_report(out);
    kl_aaudio_report(out);
    kl_mediandk_report(out);
    kl_ovrp_report(out);
    kl_ovrplat_report(out);
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}
