// See kl_target.h.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "klepton.h"
#include "kl_env.h"
#include "kl_jni.h"
#include "kl_target.h"

#define KL_TARGET_ROW(name, tree, apk, assets, libdir, entry, userdata, obb, kind, hands, world, sdkint, sdlmain, env) \
    { name, tree, apk, assets, libdir, entry, userdata, obb, kind, hands, world, sdkint, sdlmain, env },
static const kl_target TARGETS[] = {
#include "kl_target_table.h"
};
#undef KL_TARGET_ROW

// The row's baked environment (kl_target.h says why it exists). Applied once,
// at the moment the run commits to a target, which is before anything that
// reads one of these has run — kl_target_apply_host() on the host and
// kl_app_configure() in the app both come through here.
//
// Two rules, and both are the difference between this and a hardcoded default:
// the caller's environment ALWAYS wins, and everything this does is said out
// loud. A baked value that quietly replaced a value someone set is a knob that
// stops answering its own A/B.
static void kl_target_env_apply(const kl_target *t) {
    if (!t->env || !*t->env) return;
    char buf[1024];
    if (snprintf(buf, sizeof buf, "%s", t->env) >= (int)sizeof buf) {
        fprintf(stderr, "  [target] %s: baked env is longer than %zu bytes — "
                        "NOTHING was applied; split the row or widen this\n",
                t->name, sizeof buf);
        return;
    }
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok;
         tok = strtok_r(NULL, " \t", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok) {
            // By name, not skipped: the whole point of the field is that the
            // device has no other way to be told, so a typo that silently bakes
            // nothing is a launch that fails exactly as it did before.
            fprintf(stderr, "  [target] %s: baked env entry \"%s\" is not "
                            "NAME=VALUE — REFUSED, that variable is NOT set\n",
                    t->name, tok);
            continue;
        }
        *eq = 0;
        const char *name = tok, *val = eq + 1;
        const char *have = getenv(name);
        if (have) {
            fprintf(stderr, "  [target] %s: %s is already %s in the environment "
                            "— the caller wins, the row's \"%s\" is NOT applied\n",
                    t->name, name, *have ? have : "(empty)", val);
            continue;
        }
        setenv(name, val, 0);
        fprintf(stderr, "  [target] %s: %s=%s, from the target row — a device "
                        "launch has no command line to set it on\n",
                t->name, name, val);
    }
}

// See the header: the row the run is for.
static const kl_target *s_current;
void kl_target_use(const kl_target *t) {
    if (!t || t == s_current) return;
    s_current = t;
    kl_target_env_apply(t);
}
const kl_target *kl_target_current(void) { return s_current ? s_current : kl_target_default(); }

const kl_target *kl_target_lookup(const char *name) {
    if (!name || !*name) return NULL;
    for (unsigned i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++)
        if (!strcmp(TARGETS[i].name, name)) return &TARGETS[i];
    return NULL;
}

const kl_target *kl_target_default(void) {
    const kl_target *t = kl_target_lookup(KL_TARGET_DEFAULT_NAME);
    return t ? t : &TARGETS[0];
}

const char *kl_target_names(void) {
    static char buf[256];
    if (!*buf) {
        size_t n = 0;
        for (unsigned i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++)
            n += (size_t)snprintf(buf + n, n < sizeof buf ? sizeof buf - n : 0,
                                  "%s%s", i ? ", " : "", TARGETS[i].name);
    }
    return buf;
}

// A tree with no table entry — the version-swap directories. Everything is
// derived from the tree name, so the libraries, the assets and the APK all come
// from the same place; the alternative (which is what the hardcoded defaults
// used to do) is to pair one version's libraries with another's assets, and
// Unity reports that three layers up as corrupt game data.
//
// The strings are strdup'd because the caller's argv outlives nothing in
// particular and this is read for the whole process.
static const kl_target *synthesize(const char *tree, const char *libdir) {
    static kl_target t;
    static char s_tree[512], s_apk[544], s_assets[544], s_libdir[1024], s_key[512];
    snprintf(s_tree,   sizeof s_tree,   "%s", tree);
    snprintf(s_apk,    sizeof s_apk,    "%s.apk", tree);
    snprintf(s_assets, sizeof s_assets, "%s/assets", tree);
    snprintf(s_libdir, sizeof s_libdir, "%s", libdir);

    // The userdata key: the tree with a trailing "-<digits>" removed, so
    // beatsaber-128 and beatsaber share one save/pairing profile. See the
    // header; KL_FILES_DIR overrides the whole directory.
    snprintf(s_key, sizeof s_key, "%s", tree);
    char *dash = strrchr(s_key, '-');
    if (dash && dash[1]) {
        int digits = 1;
        for (const char *p = dash + 1; *p; p++) if (!isdigit((unsigned char)*p)) digits = 0;
        if (digits) *dash = 0;
    }

    t = (kl_target){ .name = s_tree, .tree = s_tree, .apk = s_apk,
                     .assets = s_assets, .libdir = s_libdir,
                     .entry_lib = "libmain", .userdata = s_key,
                     // The version-swap trees are all Beat Saber, i.e. Unity, so
                     // the OBB layout is the one getObbDirs() answers. Stated
                     // rather than left NULL: every reader of this field builds
                     // a path out of it.
                     .obb = "obb",
                     // The version-swap trees are Unity; the UE4 dialect flag
                     // is stated rather than left implicit for the same reason
                     // .obb is.
                     .ue4_sdk_int = 0,
                     // A synthesized tree has no row, so it bakes nothing.
                     // Stated rather than left to the initializer for the same
                     // reason .obb is: every reader of this field walks it.
                     .env = "",
                     .kind = KL_GUEST_UNITY };
    return &t;
}

const kl_target *kl_target_resolve(const char *arg) {
    if (!arg || !*arg) {
        const char *env = kl_env_str("KL_TARGET", NULL);
        if (env && *env) {
            const kl_target *t = kl_target_lookup(env);
            return t ? t : kl_target_default();   // a bad KL_TARGET is the
                                                  // caller's to report; do not
                                                  // silently run another guest
        }
        return kl_target_default();
    }
    if (!strchr(arg, '/')) return kl_target_lookup(arg);

    // A path. Its leading component is the tree; a table entry whose libdir or
    // tree matches wins, so `beatsaber-140/lib/arm64-v8a` — which is what every
    // gate in the Makefile passes — resolves to the real beatsaber target
    // rather than to a synthesized twin of it.
    //
    // `beatsaber/lib/arm64-v8a` no longer matches anything, because the
    // beatsaber target moved to the 1.40 tree and 1.28 is retired. It
    // synthesizes instead, which is the right answer and not a regression: the
    // 1.28 tree is still on disk, still runnable by path on the host, and still
    // lands on the `beatsaber` userdata profile — exactly what beatsaber-128
    // and the other version-swap trees have always done.
    for (unsigned i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++)
        if (!strcmp(TARGETS[i].libdir, arg)) return &TARGETS[i];

    char tree[512];
    snprintf(tree, sizeof tree, "%s", arg);
    char *slash = strchr(tree, '/');
    if (slash) *slash = 0;
    for (unsigned i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++)
        if (!strcmp(TARGETS[i].tree, tree)) return &TARGETS[i];

    return synthesize(tree, arg);
}

void kl_target_apply_host(const kl_target *t, const char *libdir_override) {
    if (!t) return;
    kl_target_use(t);
    const char *libdir = libdir_override && *libdir_override ? libdir_override
                                                             : t->libdir;
    kl_set_library_path(libdir);
    kl_jni_set_native_lib_dir(libdir);
    kl_jni_set_assets_dir(t->assets);
    kl_jni_set_apk_path(t->apk);
    // Keyed on the guest rather than the APK — see kl_userdata_dir(), and the
    // synthesize() note above. KL_FILES_DIR still overrides it outright, which
    // is how a run gets a scratch profile.
    kl_jni_set_files_dir(kl_userdata_dir(t->userdata));
    // The raw "<apk>/assets/..." path map is derived from the assets
    // directory inside kl_jni's init, so nothing to do here.
}
