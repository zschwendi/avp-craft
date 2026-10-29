#!/usr/bin/env python3
"""Which guest a visionOS build is for — one table, three consumers.

`mknative.sh`, `stage_native.sh`, `run.sh` and `gen_xcodeproj.py` read the same
facts. `make targets` writes `runtime/kl_target_table.h` from this file, so a
build does not need to re-run Python if the header is already committed.

This tree ships one target, vivecraft. The bundle id is `com.rebelancap.vivecraft-visionos`.
`KLEPTON_BUNDLE_ID` overrides it. `MCV` defaults to 26.2.
"""
import os
import sys

TARGETS = {
    "vivecraft": {
        # Vivecraft (Minecraft Java 1.21.5 + Fabric 0.19.5 + Vivecraft/Sodium/
        # Lithium, 82 mods) as a NATIVE Mach-O guest: the first row with no APK,
        # no klepton-ld and no ELF at all. The game is a GraalVM native-image
        # of the baked Fabric classpath (work/native-image, T4/T8a), relinked
        # against the xrOS SDK ("route B"), and it is entered by calling
        # `klmc_main(argc, argv)` — native-image's run_main behind a tiny shim —
        # on an 8 MB-stack pthread. Everything it loads is a plain Darwin dylib
        # in the app's Frameworks/ (LWJGL looks them up by FILE name through
        # -Dorg.lwjgl.librarypath, so they are library xcframeworks, not
        # .framework bundles):
        #
        #   libminecraft      the image (~190 MB, ~111 MB of it image heap)
        #   liblwjgl*         the QuestCraft LWJGL fork's natives (libffi
        #                     trampoline table in __TEXT — no RWX)
        #   libopenal         OpenAL Soft 1.25.2 (CoreAudio)
        #   libltw            Pojav's GL-4.x-on-GLES translator, over ANGLE
        #   libklegl          ltw's "libEGL": eglGetProcAddress -> this app's
        #                     kl_egl gateway, so the game's context lives in
        #                     kl_glfb's ANGLE share group (the one kl_openxr's
        #                     swapchains are made in — M2) and its swaps reach
        #                     the compositor through the same seams every other
        #                     GLES guest uses
        #   libklglfw         the GLFW stand-in's native half
        #   libvloader        Vivecraft's VLoader natives
        #   libopenxr_loader  LWJGL-OpenXR's loader -> kl_openxr_sym
        #
        # Jars, the Fabric game dir and Minecraft's assets (~0.6 GB). Two ways
        # they arrive: stage_native.sh copies them into Documents/vivecraft/root
        # (the devicectl loop), or KL_BUNDLE_PAYLOAD puts the same tree in the
        # .app and the first launch symlinks the read-only half from Documents
        # (a SideStore install has no devicectl). The writable seed is copied.
        #
        # tree/apk/assets/srcdir are empty because there is no APK; the C table
        # keeps the fields and the native door never reads them.
        "libs":    "libminecraft liblwjgl liblwjgl_opengl liblwjgl_stb libopenal "
                   "libltw libklegl libklglfw libvloader libopenxr_loader "
                   "libopus4j librnnoise4j libspeex4j liblame4j",
        "srcdir":  "",
        "tree":    "vivecraft",
        "apk":     "",
        "assets":  "",
        "qtplugins": "",
        "obb":     "obb",
        # Where each embedded dylib comes from, per SDK ({sdk} = xros or
        # xrsimulator; {sim} = xros or xrsim for the T2 fork's directory names).
        # visionos/mknative.sh wraps each into a library xcframework under
        # Frameworks/vivecraft/ — the native kind's mkguest.sh.
        "natives": "libminecraft=work/native-image/out/t8/{sdk}/libminecraft.dylib "
                   "liblwjgl=work/native-image/out/natives-fork-{sim}/liblwjgl.dylib "
                   "liblwjgl_opengl=work/native-image/out/natives-fork-{sim}/liblwjgl_opengl.dylib "
                   "liblwjgl_stb=work/native-image/out/natives-fork-{sim}/liblwjgl_stb.dylib "
                   "libopenal=work/natives/openal/{sdk}/libopenal.dylib "
                   "libltw=work/natives/ltw/build/{sdk}/libltw.dylib "
                   "libklegl=work/native-image/out/t8b/{sdk}/libklegl.dylib "
                   "libklglfw=work/native-image/out/t8b/{sdk}/libklglfw.dylib "
                   "libvloader=work/natives/openxr-shim/{sdk}/libvloader.dylib "
                   "libopenxr_loader=work/natives/openxr-shim/{sdk}/libopenxr_loader.dylib "
                   # Simple Voice Chat's codecs (work/natives/scripts/build-svc.sh); the image's
                   # Target_SvcNatives substitution System.load()s them from java.library.path.
                   "libopus4j=work/natives/svc/{sdk}/libopus4j.dylib "
                   "librnnoise4j=work/natives/svc/{sdk}/librnnoise4j.dylib "
                   "libspeex4j=work/natives/svc/{sdk}/libspeex4j.dylib "
                   "liblame4j=work/natives/svc/{sdk}/liblame4j.dylib",
        # The staging recipe (stage_native.sh): the T4 run's paths under
        # work/native-image, relative, and what the app's launch.txt says.
        "nativeroot": "work/native-image",
        "entry":   "libminecraft",
        "kind":    "native",
        "product": "AVPcraft",
        "project": "avp-craft",
        "display": "AVPcraft",
        "bundle":  "dev.zschwendi.minecraft.visiongame.compiled",
        # AVPcraft wordmark artwork; keep this source for icon regeneration.
        "icon":     "games/icons/avpcraft.png",
        # Austin 10-13: the whole blue picture fills the circle, no black disc
        # (mkicon.py fill_layers). iconback is unused under "fill".
        "iconfit":  "fill",
        "iconback": "0,0,0",
        # M1 is a FLAT window (the Mojang screen / title menu in a visionOS
        # window, via kl_mono). M2 opens the immersive space through
        # kl_openxr; Vivecraft draws its own hands then.
        "immersion": "full",
        "own_hands": True,
        # Background-audio ELIGIBILITY (gen_xcodeproj.py adds UIBackgroundModes
        # "audio" to this row's generated Info.plist; native rows only). Closing
        # the launcher window takes the app phase to background while the
        # immersive world keeps playing, and playback with no foreground window
        # needs the declaration. This is support, not the measured cause: the
        # physical Build71 log showed a non-mixable session interrupted by
        # another app (KleptonAudio.swift, mixWithOthers). The declaration
        # permits playback; it does not keep audio on — Crown/Home still pause
        # and deactivate the session.
        "background_audio": True,
        # ltw must see an honest ES 3.0 (work/natives/ltw/REPORT.md risk 8).
        # KL_XR_EYE_MIRROR=0: the guest's swapchain images are re-pointed at
        # the compositor's MTLTextures instead of copied every frame (the JKXR
        # breakage does not reproduce here — sim-proven 10-12, shots/fove-sim-
        # repoint-*.png). KL_OVRP_EYE_MAX=4032: launch 2 ran 83 fps at 3072 with
        # 2.6 GB of an 8191 MiB budget; launch 3 ran 4032 at gpu 10.8 ms/f, busy
        # 48%, peak 2.8 GB. KL_OVRP_EYE_MAX=4608 (4608x3696, ~+30% pixels, est.
        # ~3.1-3.5 GB peak) is the next rung, paid for by foveation:
        # KL_VRR=1 + KL_VRR_MIN_SAMPLES=1 put the display's map on Vivecraft's
        # single-sampled eye-sized "3D Render" target as well as the swapchain,
        # and kl_glfb rewrites the VR post shader to read its source at the
        # physical position (klfb_rewrite_vr_post) — no unwarp pass. Needs
        # renderScaleFactor 1.0 + useFsaa false (staged). Fallbacks (env file):
        # wrong periphery / magnified centre -> KL_VRR=0; fps collapse ->
        # KL_OVRP_EYE_MAX=4032. KL_PERF=1: the GL GPU clock decides the next rung.
        # Launch 6 (4608, floor 0.50, gpu ~10 ms at 83 fps): a texture-LOD seam
        # above the fovea where the display curve drops 0.85->0.53 in two zone
        # rows. KL_VRR_RAMP=0.92 caps every zone-to-zone fall at 8% (0.12 mip);
        # KL_VRR_FLOOR=0.40 (+1.3 mip at the far edge, where the display itself
        # is at 0.10-0.20) pays for KL_OVRP_EYE_MAX=5088 (+10% linear in the
        # fovea): model ~47% of fragments, est. ~11.3 ms/f, ~4.2 GB peak.
        # A/B: KL_VRR_RAMP=1, KL_VRR_FLOOR=0.5, KL_OVRP_EYE_MAX=4608.
        # Launch 12 (26.2): back to KL_OVRP_EYE_MAX=4608. Launch 11 at 5088 was
        # GPU-bound (busy 85-98%, ~16.4 ms/frame median vs 1.21.5's ~10.1 at
        # the same 5088/0.40/0.92), so it sat on 60.0 or fell to 40-55 with
        # ragged 1x/2x holds = the walking stutter. Exact Metal physical sizes
        # (same rate curve, floor 0.40, ramp 0.92; the Mac's MTLRasterization-
        # RateMap reproduces the device's 3932x2664 exactly): 5088 -> 3932x2664
        # (10.47 Mpx/eye), 4608 -> 3552x2417 (8.59, -18%). The floor is the
        # weak lever (0.30 at 5088 = 9.83 Mpx, -6%, for +0.4 mip more edge blur),
        # so the fovea gives up 9% linear and the periphery keeps its LOD.
        # Numbers: work/perf/LAUNCH12-LEVERS-REPORT.md. A/B: KL_OVRP_EYE_MAX=5088.
        "env":     "KL_GL_ES_VERSION=3.0 KL_XR_EYE_MIRROR=0 KL_OVRP_EYE_MAX=4864 "
                   "KL_VRR=1 KL_VRR_MIN_SAMPLES=1 KL_VRR_FLOOR=0.40 KL_VRR_RAMP=0.92 "
                   # 8x anisotropic on mipmapped textures (the block atlas);
                   # the guest never asks. A/B: KL_GLFB_ANISO=0.
                   "KL_GLFB_ANISO=8 KL_PERF=1 "
                   # 26.x (Vivecraft's ONE array swapchain, reached by COPY):
                   # without this the copy path drops the map and the whole eye
                   # chain renders unfoveated — launch 9: 38 fps in-world, GPU
                   # busy ~18 ms/frame vs launch 7's ~9 (kl_glfb.c,
                   # klfb_vrr_mirror_on). No effect on 1.21.x (re-pointed eyes).
                   # A/B: KL_VRR_MIRROR=0.
                   "KL_VRR_MIRROR=1 "
                   # Minecraft renders in GAMMA space on every version: what it
                   # writes is an sRGB code value, never linear light. The
                   # composite must be told, because the visionOS drawable is
                   # bgra8Unorm_srgb and treats every sample as linear.
                   # 1.21.5 (QC jar): SRGB8 swapchain + glDisable(FRAMEBUFFER_SRGB)
                   #   -> kl_glfb's detection already turned the decode ON.
                   # 1.21.11 (QC mobile list = RGBA8 only): raw code values in a
                   #   unorm texture, no glDisable -> decode stayed off -> launch 8
                   #   "bright, washed".
                   # 26.x (upstream jar): SRGB8 array swapchain, the "L/R Eye"
                   #   RGBA8 targets are glBlitFramebuffer'd into it; ES encodes on
                   #   a blit into an sRGB attachment (desktop GL with the default
                   #   FRAMEBUFFER_SRGB off would not), the sample decodes once ->
                   #   code values read as linear -> launch 9 "bright, washed".
                   # Forcing the decode is a no-op for 1.21.5 and the fix for both
                   # others. A/B: KL_SRGB_DECODE=0 (env file).
                   "KL_SRGB_DECODE=1 "
                   # libltw stages glBufferSubData into big in-flight buffers
                   # (Sodium arenas) through a ring + glCopyBufferSubData —
                   # ANGLE Metal otherwise copies the whole arena per upload
                   # (the 2-10 fps chunk storms of launches 9-11). The fix is
                   # ON by default inside ltw (LTW_DARWIN_SUBDATA=off is the
                   # A/B); this knob only adds the per-second [ltw-buf]
                   # counter line so a launch log shows `staged > 0, big 0.0`.
                   "LTW_DARWIN_BUFSTATS=1 "
                   # 26.x: the ONE array swapchain's (image, slice) pairs are 2D
                   # textures backed by the compositor's eye storage, the game's
                   # layer attach is redirected to them -> no per-eye mirror
                   # copy (launch 11 paid ~3 ms/frame for two full 5088x4081
                   # copies). Inert for 1.21.x (two 2D swapchains, re-pointed).
                   # A/B: KL_XR_ARRAY_DIRECT=0 (env file).
                   "KL_XR_ARRAY_DIRECT=1 "
                   # 26.2 binds away and re-attaches between its back-to-back
                   # clears, which ANGLE turns into a render pass each; holding
                   # the unbind merges them. OFF for launch 12: on the sim
                   # (lane 1, 2026-10-13) every =1 run raises GL_INVALID_OPERATION
                   # from title on (~40/s; Vivecraft "@ pre render setup" in
                   # world), ERRSCAN pins it between bind(READ,fb) and the next
                   # traced call = an op that reaches ANGLE without forwarding
                   # the held unbind; =0 runs are clean. Re-enable once found.
                   # Launch 12 (4608, no mirror copy, ltw staging) measured
                   # GPU 9.4 ms/frame at a steady 90 Hz (11.1 ms budget), 85 fps,
                   # zero stutter — ~15% headroom. 4864 = +11% physical pixels,
                   # est. ~10.3 ms; 5088 (+22%) would spill into held frames
                   # until the post-pass->swapchain jar patch lands. A/B via the
                   # env file: KL_OVRP_EYE_MAX=4608 / 5088.
                   # KL_PERF_PASSES=1 ran for launch 12 only (per-pass table in
                   # vivecraft/logs/launch12-device/, LAUNCH12-GPU-REPORT.md).
                   "KL_GLFB_LAZY_UNBIND=0",
    },

}


# What a target that says nothing gets: the shape every target had before this
# key existed, so Beat Saber and Steam Link are untouched by it.
IMMERSION_DEFAULT = "mixed"

DEFAULT = "vivecraft"


        # `"libs": None` means "whatever this tree would translate for that srcdir",
        # which the Makefile already decides — the .so files that are really ELF, minus
# the ones we REPLACE (libOVRPlugin, libovrplatformloader, libvrapi) and the
# ones that are not part of the application (libfrda, libscript). Asking it
# rather than restating it keeps one answer: a second copy of those rules here
# would drift on the next guest, and the failure mode is a bundle missing a
# library, which on device is a dlopen that cannot fall back to anything.
# 
# It is a hard failure rather than a fallback to a pinned list, for the same
# reason: a quietly incomplete bundle is worse than a build that stops.
def libs_for(srcdir):
    import subprocess, os
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = subprocess.run(["make", "-s", "guestlibs-list", f"LIBS={srcdir}"],
                         cwd=root, capture_output=True, text=True)
    libs = out.stdout.strip()
    if out.returncode != 0 or not libs:
        print(f"!! could not discover the guest libraries in {srcdir!r} "
              f"(make guestlibs-list said: {out.stderr.strip() or 'nothing'})",
              file=sys.stderr)
        sys.exit(1)
    return libs


# The public id is on the vivecraft row. KLEPTON_BUNDLE_ID still overrides it
# in gen_xcodeproj.py and stage_native.sh. This function does not prefix $USER:
# a login name is not an App ID, and this tree ships one bundle id.
def bundle_for(name):
    row = TARGETS.get(name) or {}
    ident = row.get("bundle")
    if not ident:
        raise SystemExit(f"!! {name}: set \"bundle\" on the row or export KLEPTON_BUNDLE_ID")
    return ident


def resolve(name):
    if name not in TARGETS:
        print(f"!! unknown target {name!r} — one of: {', '.join(sorted(TARGETS))}",
              file=sys.stderr)
        sys.exit(1)
    t = dict(TARGETS[name])
    t["name"] = name
    # Minecraft 26.x on the native row (M3C): the same libraries by name, but LWJGL 3.4.1's natives (core/opengl/stb,
    # plus tinyfd and FreeType, which 26.2's NativeLibrariesBootstrap loads eagerly and fatally) and the 3.4.1 klglfw
    # come from work/natives/build/lwjgl-3.4.1/{sdk}/ (USAGE.md there). Keyed on the MCV env var that run.sh,
    # mknative.sh and stage_native.sh already share; unset / 1.x leaves the row exactly as written. libs/natives are
    # build-only, so `make targets` (the C table) is unaffected.
    _mcv = os.environ.get("MCV") or "26.2"
    if t.get("kind") == "native" and _mcv.startswith("26.") and t.get("natives"):
        _l341 = "work/natives/build/lwjgl-3.4.1/{sdk}/"
        _swap = {n: _l341 + n + ".dylib" for n in
                 ("liblwjgl", "liblwjgl_opengl", "liblwjgl_stb", "libopenal", "libklglfw",
                  "liblwjgl_tinyfd", "libfreetype")}
        _pairs = [p.split("=", 1) for p in t["natives"].split()]
        _names = [n for n, _ in _pairs]
        _pairs = [(n, _swap.get(n, src)) for n, src in _pairs]
        _extra = [n for n in ("liblwjgl_tinyfd", "libfreetype") if n not in _names]
        _pairs += [(n, _swap[n]) for n in _extra]
        t["natives"] = " ".join(f"{n}={src}" for n, src in _pairs)
        t["libs"] = " ".join(t["libs"].split() + _extra)
    # Derived from the target name, EXCEPT where a row states its own — and a
    # row that states one means it. An id is not a naming preference: it is the
    # identity iOS installs against, so changing it does not rename an app, it
    # installs a SECOND one beside the first. Beat Saber has been on Austin's
    # headset as `dev.klepton.bs` since the 1.28 bring-up; letting the derived
    # id win here would have left him with two Beat Sabers and no way to tell
    # which icon was which — which is exactly what happened once already, on
    # 2026-08-16, when the 1.40 build installed under a derived id and the
    # tester spent two rounds reporting bugs from the stale app.
    t["bundle"] = TARGETS[name].get("bundle") or bundle_for(name)
    if t["libs"] is None:
        t["libs"] = libs_for(t["srcdir"])
    t.setdefault("immersion", IMMERSION_DEFAULT)
    t.setdefault("own_hands", False)
    # Off unless a row asks; gen_xcodeproj.py refuses it on a non-native row
    # rather than silently dropping it.
    t.setdefault("background_audio", False)
    # Empty rather than absent, so every consumer sees the same keys whether or
    # not the row bothered to state them — a shell that evals this table cannot
    # tell a missing assignment from an empty one, and `set -u` can.
    t.setdefault("dlc", "")
    # Same treatment: absent and empty are indistinguishable to a shell that
    # evals this table under `set -u`.
    t.setdefault("gamedata", "")
    # ...and WHERE that tree lands on the guest's side of /sdcard. It used to be
    # the literal "srceng" inside stage_assets.sh, which was the table in a
    # fifth place: Simpsons Hit & Run hardcodes /storage/emulated/0/SimpsonsHitRun
    # (libmain's own strings) and would have been staged into hl2vr's directory
    # name and reported "missing game files" with the 1.9 GB sitting right there.
    t.setdefault("gamedest", "")
    # SDL door only — see kl_target.h. Absent on every other kind.
    t.setdefault("sdl_main", "")
    # ...and the environment the row bakes in — see `env` below and in
    # kl_target.h. Empty for almost every row.
    t.setdefault("env", "")
    # The device bundle's guest set, when it is narrower than `libs` (a row
    # whose device build keeps fewer libraries than its host/simulator one;
    # gen_xcodeproj.py applies it). Empty = the device embeds `libs`.
    t.setdefault("device_libs", "")
    # The native kind's dylib map and staging root (empty on every APK row).
    t.setdefault("natives", "")
    t.setdefault("nativeroot", "")
    # The scene manifest's immersion style and the app's runtime half of the
    # same decision (opaque_world in kl_target.h) cannot see each other —
    # one is read by the build, the other by the app. Deriving the runtime
    # half here is what keeps them in step.
    t["opaque_world"] = t["immersion"] == "full"
    return t


# The runtime's copy of the table, as an X-macro list. Only the fields C needs
# are here: the visionOS product/bundle/display are the BUILD's business and the
# app already knows which one it is by the time it runs, and `libs` is discovered
# per build rather than being a property anything at runtime should believe.
# 
# The DEFAULT is emitted too, so `build/m_boot` with no argument and an app built
# with no -DKL_TARGET_DEFAULT agree about which guest that means.
def c_table():
    out = [
        "// GENERATED by visionos/targets.py — do not edit. Regenerate: make targets",
        "//",
        "// The target table, as the runtime sees it. visionos/targets.py is the",
        "// authority (it also carries the bundle id, product and display name, which",
        "// only the build needs); this is its C half, generated so that the two",
        "// cannot disagree about which tree, which APK or which boot sequence a",
        "// target means. See runtime/kl_target.h for the struct these expand into.",
        "",
        "// KL_TARGET_ROW(name, tree, apk, assets, libdir, entry_lib, userdata, obb,",
        "//                kind, own_hands, opaque_world, ue4_sdk_int, sdl_main, env)",
        "#ifndef KL_TARGET_ROW",
        "#error \"include this through runtime/kl_target.c\"",
        "#endif",
        "",
    ]
    for name in sorted(TARGETS):
        t = dict(TARGETS[name])
        t.setdefault("immersion", IMMERSION_DEFAULT)
        t.setdefault("own_hands", False)
        t.setdefault("ue4_sdk_int", False)
        t.setdefault("env", "")
        kind = "KL_GUEST_" + t["kind"].upper()
        # `userdata` defaults to the row name — one profile per target is the
        # rule, and a row only states it to KEEP a directory a previous naming
        # created (steamlink-vr).
        # NULL rather than "" for a row with no SDL door: the field is a
        # pointer the door dereferences only on its own kind, and an empty
        # string there would read as "the guest exports a function called
        # nothing" rather than as "this row has no opinion".
        sdlmain = ('"%s"' % t["sdl_main"]) if t.get("sdl_main") else "NULL"
        if any(c in t["env"] for c in '"\\'):
            raise SystemExit(f"!! {name}: env {t['env']!r} has a quote or a "
                             "backslash in it — it is emitted as a C string "
                             "literal, so that would not survive the trip")
        out.append('KL_TARGET_ROW("%s", "%s", "%s", "%s", "%s", "%s", "%s", "%s", %s, %d, %d, %d, %s, "%s")'
                   % (name, t["tree"], t["apk"], t["assets"], t["srcdir"],
                      t["entry"], t.get("userdata", name), t["obb"], kind,
                      1 if t["own_hands"] else 0,
                      1 if t["immersion"] == "full" else 0,
                      1 if t["ue4_sdk_int"] else 0,
                      sdlmain,
                      t["env"]))
    out += ["", '#define KL_TARGET_DEFAULT_NAME "%s"' % DEFAULT, ""]
    return "\n".join(out)


def main(argv):
    if len(argv) > 1 and argv[1] == "--list":
        for k in sorted(TARGETS):
            print(k)
        return 0
    if len(argv) > 1 and argv[1] == "--c-table":
        print(c_table())
        return 0
    name = argv[1] if len(argv) > 1 and argv[1] else DEFAULT
    t = resolve(name)
    if len(argv) > 2:
        if argv[2] not in t:
            print(f"!! no field {argv[2]!r}", file=sys.stderr)
            return 1
        print(t[argv[2]])
        return 0
# Shell-sourceable. Quoted because `libs` has spaces in it and an unquoted
# eval would turn one assignment into a command.
    for k, v in t.items():
        print(f"KLT_{k.upper()}='{v}'")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
