# Fresh public-source build verification

On October 29, 2026 (October 30 UTC), a fresh HTTPS clone of this repository produced **AVPcraft Build 73** as a signed visionOS app. The starting public commit was `80f7833`; this revision includes the bounded compiler-job and strict linker error-handling fixes exercised during that build.

The game, launcher helpers and native libraries were rebuilt. No compiled Minecraft image, private development checkout or existing player world was used as a build input. The build used the public repository's included ANGLE frameworks, a checksum-verified official GraalVM download, installed Xcode/OpenJDK, and a new isolated Gradle cache.

## What passed

- All six Vivecraft patches applied to the pinned upstream source. The canonical JAR exactly matched the published SHA-256.
- Minecraft libraries, all 13 pinned top-level mods and all 5,057 asset objects downloaded and passed their hash checks.
- Fresh OpenAL, LWJGL/libffi/FreeType, klglfw, LTW, voice codecs and OpenXR dependencies built. All 47 resulting dylibs passed architecture/platform/import checks. Actual game/mod coverage resolved 98 GLFW references with no missing references.
- The real Mac JVM client created and rendered a disposable world, loaded a player and surrounding chunks, advanced game ticks, and saved normally. The [Knot bake audit](KNOT-BAKE.md) classifies its exact 49 full-pass failures; that exception is specific to the pinned configuration.
- Pruning removed 30,752 identical library copies, retained 17 transformed library classes, and removed the five documented LWJGL multi-release copies. Launcher and substitution compilation passed; all 137 native-image classpath entries existed.
- Native compilation succeeded using a **12 GiB maximum heap and one worker**. GraalVM reported **10 minutes 29 seconds** to generate the shared library. `/usr/bin/time -l` recorded **7,989,919,744 bytes (7.44 GiB)** maximum resident memory on a 32 GiB M4 Mac mini.
- Both visionOS device and simulator libraries linked against SDK 27.0, with minimum OS 26.0. Their required exports and system-library imports were checked independently.
- Xcode 27.1 built and signed the device app. Its strict nested signature check passed. The separate test identity preserves the existing installation.

The final package audit verified all 5,057 assets, 139 required classpath entries
(the compiled overlay is intentionally omitted), all 13 pinned mods, 97 license
files, the memory entitlements, and all 17 Minecraft Mach-O sections against the
fresh linked input. It found no bundled worlds or account credentials.

The [machine-readable checks](CLEAN-BUILD-CHECKS.json) record versions, library hashes, measurements and the current installation/test boundary.

## Storage

The completed task folder occupied **7.002 GiB**, including the fresh checkout, isolated Gradle cache, builds, diagnostic evidence and a small user-data backup. Its signed app occupied **0.828 GiB**; that is a subset of the task folder. The retained official GraalVM download archive was another **0.329 GiB** outside the folder.

This is a completed working-set measurement, **not a measured whole-build peak or hard minimum**. Installed Xcode/SDKs and OpenJDK, system caches, swap and unrelated projects are excluded. Leave room for temporary files and retries. The previous 40 GB figure was not an established requirement. [BUILDING.md](../BUILDING.md#storage-and-memory) explains the resource settings and scope.

## Remaining acceptance

Physical installation is pending: the reference headset required unlocking during
the install attempt, and the existing Build 72 remained installed. A signed app and the Mac bake's rendered world do not establish fresh headset gameplay, the lower HUD's visual placement, or fixes for audio/window/immersive behavior. Those checks remain separate. A changed mod set, compiler, renderer or feature configuration also needs its own bake and validation.

The upstream voice-codec build still fetches minimp3 from its `master` branch; this run resolved `ea99364f61c14656440e8d77e9c233ccf3124633`. The current build therefore does not claim that every transitive upstream download is immutable.
