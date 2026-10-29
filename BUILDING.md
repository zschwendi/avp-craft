# Building AVPcraft (Build 72 host with lower HUD)

The host source matches AVPcraft Build 72's audio and turning changes, with the newer lower-HUD patch and independent profile migration preserved. Build 72 is verified installed on a physical Vision Pro and uses the same native game payload as Build 71. The patched Vivecraft Fabric jar has been built from the pinned source and checked locally. The complete native-image/relink/host installation sequence has not been repeated for the newer HUD or the clean-build repairs. See [build recovery](docs/BUILD-RECOVERY.md) for the verified scope, and [AVP_INSTALL.md](AVP_INSTALL.md) for signing and installation.

Build the visionOS app from this repo plus the downloads below. The Minecraft client, the mod jars, and `libminecraft.dylib` are build outputs. They are not in git. GitHub rejects files over 100 MB, and `libminecraft.dylib` is about 202 MB of compiled client.

Script roots come from the script path. `KLEPTON_TEAM` is an environment variable; supply your own signing team.

Set the signing team with the environment. The repo has no team id in it.

```
export ROOT="$(pwd)"          # the clone
export KLEPTON_TEAM="<your Apple Development Team ID>"
```

A simulator build does not need `KLEPTON_TEAM`. `run.sh` sets `CODE_SIGNING_ALLOWED=NO` for the simulator.

The target is `vivecraft`, the product/display name is `AVPcraft`, and the project is `avp-craft.xcodeproj`. Use a unique `KLEPTON_BUNDLE_ID` owned by your signing team; do not register the developer's default id. Changing the id creates a separate app/data container. Keep it stable for later updates.

Export these settings before the app-generation/build steps:

```bash
export MCV=26.2 KLEPTON_TARGET=vivecraft
export KLEPTON_PRODUCT_NAME=AVPcraft KLEPTON_PROJECT_NAME=avp-craft
export KLEPTON_DISPLAY_NAME=AVPcraft KLEPTON_MARKETING_VERSION=0.4.0
export KLEPTON_BUILD_VERSION=73 KLEPTON_LOW_LATENCY=0
export KLEPTON_BUNDLE_ID="com.yourname.avpcraft"
```

The source retains the Build 72 host plus the HUD changes listed in SOURCE-SNAPSHOT.json. Use a new build number for this rebuild; 73 above is an example. Rebuild the game client: the old compiled image cannot consume the new HUD pitch setting. Keep build receipts and test the result on your headset.

## Machine

- Apple Silicon Mac.
- Xcode 27 with the visionOS SDK. Deployment target 26.0.
- CMake (with visionOS support), Ninja, Git, curl, make, zip/unzip and Xcode command-line tools.
- A clone path without spaces: some inherited shell scripts do not quote every path.
- Python 3 with Pillow. `visionos/mkicon.py` imports it. `run.sh` runs that step with `python3.13` when `python3.13 -c "import PIL"` works, and otherwise with `python3`.
- About 40 GB free. An ANGLE-from-source build needs about 12 GB more.
- Enough RAM for `native-image -J-Xmx28g`. The bake was written for a 36 GB machine. It will not start on a 16 GB Mac.
- A Microsoft account that owns Minecraft: Java Edition, used when you **launch**. The download step does not check ownership.

The simulator build targets the **Apple Vision Pro** simulator on visionOS 27.0. `KL_XR_STAGE_DROP=1.6` is the simulator head offset.

## 1. GraalVM

Install Oracle GraalVM JDK 25.0.4 into:

```
work/native-image/graalvm/graalvm-jdk-25.0.4+7.1
```

Download:

```
https://download.oracle.com/graalvm/25/archive/graalvm-jdk-25.0.4_macos-aarch64_bin.tar.gz
```

Confirm `Contents/Home/release` reads `25.0.4`. The JDK is under the [GraalVM Free Terms and Conditions](https://www.oracle.com/downloads/licenses/graal-free-license.html). Do not commit it.

The tarball that produced this image is 353,673,229 bytes, sha256 `0b79e23c133facbad2f7aa55a3b76d17bd59d2fa15e2735bb63391ace223fd13`. Oracle serves that file as `latest` (Last-Modified Thu, 16 Jul 2026 13:18:02 GMT). Check the sha256 before you trust a new download. `Contents/Home/release` reads `JAVA_VERSION=25.0.4` and `JAVA_RUNTIME_VERSION=25.0.4+7-LTS-jvmci-b01`.

Point `JAVA_HOME` at `.../graalvm-jdk-25.0.4+7.1/Contents/Home` for the Vivecraft Gradle build.

## 2. ANGLE

The repo contains the retargeted frameworks at `vendor/out/xros` and `vendor/out/xrsim` (about 12 MB each). Fetch the matching headers needed by the native dependency builds, even when using those frameworks:

```sh
python3 work/natives/scripts/fetch-graphics-headers.py
```

This verifies small, pinned ANGLE and Vulkan header archives and populates `vendor/include` and `vendor/third_party/vulkan-headers/src/include`. Existing differing files are left untouched and reported as conflicts. No graphics framework is rebuilt by this step.

To build the frameworks themselves instead:

```
make angle-fetch
make angle-xros angle-xrsim
```

`angle-fetch` clones https://chromium.googlesource.com/angle/angle.git at `25e721127e1c6c4c6fa0182b5c234b2c88971175`, syncs depot_tools at `6afa997717b2c0e1382e1465bedbe1a6855b9388`, and applies `angle-patches/klepton.patch`. The build is about 12 GB and 30–60 minutes. The app command below uses `KL_ANGLE_PREBUILT=1`, which reads those two framework trees.

This repo commits those two framework trees and the BSD notice at `vendor/out/LICENSE`. `KL_ANGLE_PREBUILT=1` uses them. Run the `make` lines above only when you want to replace them.

## 3. Vivecraft jar

From the repository root:

```bash
mkdir -p work/vivecraft-openxr-26.2
cd "$ROOT/work/vivecraft-openxr-26.2"
```

```
git clone -b openxr https://github.com/Vivecraft/VivecraftMod VivecraftMod
git -C VivecraftMod checkout -B kl-mobile 13eafae1
git -C VivecraftMod am "$ROOT/work/vivecraft-openxr-26.2/patches/"*.patch
```

The original patches 0001–0005 form the build 71 baseline. Patch 0006 adds the real head-HUD pitch consumer and new-profile defaults. The resulting Git **tree** is `bc542a09e32f6f9b74f54ddb4a37373d2b32c902` (commit IDs created by `git am` can differ). License: LGPLv3.

```
cd VivecraftMod
printf 'project.build.excludeForge=true\nproject.build.excludeNeoforge=true\n' > local.properties
export JAVA_HOME="/path/to/jdk-25.0.4.1/Contents/Home"
./gradlew --no-daemon --max-workers=2 :fabric:build -x test -Pfabric_api_version=0.161.0+26.2
mkdir -p build/canonical
python3 "$ROOT/work/vivecraft-openxr-26.2/canonicalize-vivecraft-jar.py" \
  build/libs/vivecraft-26.2-1.3.15-fabric.jar \
  build/canonical/vivecraft-26.2-1.3.15-fabric.jar
```

For this jar build, the verified compiler is OpenJDK 25.0.4.1, Gradle 9.3.0 and Fabric Loom 1.15.5. Set `JAVA_HOME` to your matching JDK installation in the command above. The GraalVM installation in section 1 is still used for the later bake/native-image steps.

The normalized jar is `VivecraftMod/build/canonical/vivecraft-26.2-1.3.15-fabric.jar`. The normalizer is required: Loom inserts changing ZIP metadata for nested dependency jars even when every payload byte is identical. It produces a deterministic stored ZIP while preserving all entry names and uncompressed bytes. It never changes the input jar. The output path must be new; move or remove only the previous generated canonical jar before intentionally repeating this step. Python 3.9.6 and 3.14.6 produced identical bytes in verification.

Expected normalized sha256 (patches 0001–0006):

```
f317964c49f8e1dace17dcc4ea4a8d4f9d29c7e5a2888c61b73259f4355deac4
```

`work/native-image/scripts/m3a/mods-26.2.json` sets `local` to `work/vivecraft-openxr-26.2/VivecraftMod/build/canonical/vivecraft-26.2-1.3.15-fabric.jar` (relative to the repo root) and `sha256` to the hash above. The jar is a build output. Do not commit it.

## 4. Client, libraries, assets, mods

From `work/native-image` (run `cd "$ROOT/work/native-image"` first):

```
python3 -I scripts/m3a/fetch-inst.py 26.2
```

This downloads the Minecraft 26.2 client, vanilla and Fabric libraries, LWJGL 3.4.1 jars, asset index 32, and the Modrinth mods named in `mods-26.2.json`. It checks hashes. It does not check that you own the game.

`fetch-inst.py`, `inst.sh`, and `jdk.sh` take the repo root from their own path. `MCV` defaults to 26.2. Unset `MCV` selects the JDK 25 tree above. `MCV=1.*` selects GraalVM 21.0.12.

## 5. Natives

Run this section from the repository root (`cd "$ROOT"`). Download the source archives that the native scripts expect, then verify their pinned checksums:

```bash
mkdir -p work/natives/downloads work/native-image/dl/libffi
curl -fL https://github.com/libffi/libffi/releases/download/v3.5.2/libffi-3.5.2.tar.gz -o work/native-image/dl/libffi/libffi-3.5.2.tar.gz
printf '%s  %s\n' f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc work/native-image/dl/libffi/libffi-3.5.2.tar.gz | shasum -a 256 -c -
curl -fL https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-1.tar.gz -o work/natives/downloads/freetype-VER-2-14-1.tar.gz
printf '%s  %s\n' 44bd69d1f0750603410cfd4f26f1c5523c5a3a087a2dfa8639b757dc7c6677be work/natives/downloads/freetype-VER-2-14-1.tar.gz | shasum -a 256 -c -
work/natives/scripts/build-openal.sh xros xrsimulator
work/natives/scripts/fetch-lwjgl341.sh
work/native-image/scripts/t2/build-lwjgl341-natives.sh macosx
work/native-image/scripts/t2/build-lwjgl341-natives-xros.sh
work/native-image/scripts/t2/build-klglfw341.sh
```

OpenAL Soft 1.25.2 is required **before** the visionOS LWJGL build: that build copies `work/natives/openal/<sdk>/libopenal.dylib` into `work/natives/build/lwjgl-3.4.1/<sdk>/`. The helper downloads a hash-pinned upstream archive and builds CoreAudio dylibs with RTKit disabled. Both device and simulator builds passed against SDK 27 during the build-recovery check. This does not establish byte-for-byte identity with the installed app's older OpenAL library.

`fetch-lwjgl341.sh` checks source commit `b800ccffab14396fc529ddb6c931b7c5c5226763`. The LWJGL macOS binaries support the host JVM bake. The headset needs the separately built xros libraries. FreeType is invoked by the visionOS LWJGL builder.

PojavLauncherTeam LTW, LGPL-3.0. The Makefile compiles this checkout in place and does not edit it. `work/natives/ltw/darwin/` and the Makefile are the patch set, and they are in git.

```
mkdir -p work/src
git clone https://github.com/PojavLauncherTeam/ltw work/src/PojavLauncherTeam_ltw
git -C work/src/PojavLauncherTeam_ltw checkout 7703f1c29a1fd7f151484c38fb7563bb79ca8ccb
make -C work/natives/ltw SDK=xros ANGLE_INC="$ROOT/vendor/include"
make -C work/natives/ltw SDK=xrsimulator ANGLE_INC="$ROOT/vendor/include"
```

`work/natives/ltw/Makefile` defaults `VK_INC` to `$(ROOT)/vendor/third_party/vulkan-headers/src/include`. Those headers exist after `make angle-fetch`. They are header-only. An already-built `libltw` does not need them to link the app. The default is the repo root, not a home directory.

Simple Voice Chat codecs, and the OpenXR loader shims:

```
work/natives/scripts/build-svc.sh
work/natives/scripts/build-openxr-shim.sh
mkdir -p work/native-image/lwjgl-fork-t4/natives
work/native-image/scripts/t8b/build-natives.sh
```

`build-svc.sh` clones the henkelmax JNI repos and checks out the pins in `work/natives/svc/PROVENANCE.md` (opus4j `362db14553d39c4dc080e4edd50630b17dbf9682`, rnnoise4j `5f2097d504fc67f31d0d8900e9df0a5bdf4ec9ae`, speex4j `9428cb14b8ec373fcd36f5b5792899611042ece9`, lame4j `b09599989878cbc7349075eca0b3aa4f105e5957`).

`build-natives.sh` builds `libklegl`.

`build-openxr-shim.sh` includes `jni.h` from `work/native-image/graalvm/graalvm-jdk-25.0.4+7.1/Contents/Home`. `$G` defaults to that tree.

`ANGLE_INC` in `scripts/t2/` and `scripts/t8b/build-natives.sh` defaults to `$ROOT/vendor/include`.

## 6. Classpath and Knot bake

Run `cd "$ROOT/work/native-image"` before this section; use `MCV=26.2` on every step. Set `JAVA_HOME` to the pinned GraalVM and prepend its `bin` to `PATH` so `javac`/`jar` use the same JDK:

```
export JAVA_HOME="$ROOT/work/native-image/graalvm/graalvm-jdk-25.0.4+7.1/Contents/Home"
export PATH="$JAVA_HOME/bin:$PATH"
MCV=26.2 scripts/mkcp.sh
MCV=26.2 scripts/mkcp-fabric.sh
```

Build the bake agent (default package, `Premain-Class: BakeAgent`). ASM is already
in the generated Fabric classpath; it is required when compiling this source:

```bash
mkdir -p bakeagent
AVP_BAKE_CLASSES=$(mktemp -d "${TMPDIR:-/tmp}/avpcraft-bakeagent.XXXXXX")
javac -cp "$(cat inst-26.2/fabric.cp)" -d "$AVP_BAKE_CLASSES" scripts/bake-src/BakeAgent.java
jar cfm bakeagent/bakeagent.jar scripts/bake-src/MANIFEST.MF -C "$AVP_BAKE_CLASSES" .
```

The fresh temporary directory prevents classes from an earlier build entering the
agent jar. Keep it until the jar has been checked; it contains only this compile's
output.

A fresh checkout has no world. The optional copies in `mkcp-fabric.sh` do not
create one. First inspect `inst-26.2/fgamedir/saves/world`: if it already contains a
world you care about, back it up and use a separate clean checkout for this bake.
Do not overwrite, delete or bake against your only copy of a player world.

Use the following foreground helper for both setup and capture. Run these commands
in Bash, from `work/native-image`, in the same terminal as the Java setup above.
Unlike `runfabric.sh`, this helper has no timed process kill. That older script's
watchdog starts at JVM launch and can terminate the client during world creation
or while saving; its example timeout is not a completion check.

```bash
AVP_BAKE_ROOT="$PWD"
AVP_BAKE_GAMEARGS=()
run_bake_client() (
  cd "$AVP_BAKE_ROOT/inst-26.2/fgamedir" || exit
  "$JAVA_HOME/bin/java" -XstartOnFirstThread -Xmx3G "$@" \
    -cp "$(cat "$AVP_BAKE_ROOT/inst-26.2/fabric.cp")" \
    net.fabricmc.loader.impl.launch.knot.KnotClient \
    --accessToken 0 --username spike \
    --uuid 00000000-0000-0000-0000-000000000000 --version 26.2 \
    --gameDir "$AVP_BAKE_ROOT/inst-26.2/fgamedir" \
    --assetsDir "$AVP_BAKE_ROOT/inst-26.2/assets" \
    --assetIndex "$(cat "$AVP_BAKE_ROOT/inst-26.2/asset-index-id.txt")" \
    "${AVP_BAKE_GAMEARGS[@]}"
)
run_bake_client -Dvivecraft.openxr.mobile=true
```

At the title screen choose **Singleplayer**, create a disposable world named
`world`, and wait until terrain has loaded. Use **Save and Quit**, then quit the
client normally. Confirm that the actual save folder is
`inst-26.2/fgamedir/saves/world` and contains `level.dat`; a display name alone is
not sufficient if Minecraft chose a different folder name.

Now capture the prepared world. The command refuses to reuse an existing bake
output. Preserve any previous `inst-26.2/bake` under a separate, unused backup
name before deliberately making a new capture.

```bash
if [ ! -f inst-26.2/fgamedir/saves/world/level.dat ]; then
  echo 'Create and save the disposable world first.' >&2
elif [ -e inst-26.2/bake ] || [ -L inst-26.2/bake ]; then
  echo 'Preserve the existing inst-26.2/bake before a fresh capture.' >&2
else
  AVP_BAKE_GAMEARGS=(--quickPlaySingleplayer world)
  run_bake_client \
    "-javaagent:$AVP_BAKE_ROOT/bakeagent/bakeagent.jar=$AVP_BAKE_ROOT/inst-26.2/bake" \
    -Dbake.fullAt=300 \
    -Dvivecraft.openxr.mobile=true
fi
```

`bake.fullAt=300` starts the full pass five minutes after JVM launch; it is a
starting allowance, not proof that loading is complete. Keep the world open until
the terminal prints the final `[BAKE] defined-at-runtime=...` summary and
`inst-26.2/bake/summary.txt` exists. If the world has not loaded before the full pass
starts, preserve that output and repeat with a larger delay. Read
`fullpass-failures.txt` and investigate failures; elapsed time or a copied class
count alone does not establish a usable capture. After the pass completes, use
**Save and Quit**, then quit normally before pruning. No timer closes the game for
you. `-Dvivecraft.openxr.mobile=true` selects the mobile OpenGL ES path.

Prune classes the bake copied unchanged from library jars. Keep classes a mixin actually changed.

```
python3 -I scripts/prune_overlay.py inst-26.2/bake/classes \
  inst-26.2/vanilla/lib/*.jar inst-26.2/fabric/lib/*.jar
```

Then delete these five LWJGL 3.4.1 multi-release classes if they are still in the overlay. They would shadow the natives built in step 5: `org/lwjgl/VersionImpl`, `org/lwjgl/system/CheckIntrinsics`, `org/lwjgl/system/MemoryUtilTunables`, `org/lwjgl/system/MultiReleaseTextDecoding`, `org/lwjgl/system/StackWalkUtil`.

The original baseline bake recorded the following in `inst-26.2/bake/PRUNE-NOTE.txt` (a build product, not committed). These counts are historical evidence, not acceptance criteria for your new capture:

```
prune_overlay.py (30752 identical lib copies removed, 17 kept) + the 5 LWJGL 3.4.1 multi-release copies removed by hand
(org/lwjgl/VersionImpl, org/lwjgl/system/CheckIntrinsics, org/lwjgl/system/MemoryUtilTunables, org/lwjgl/system/MultiReleaseTextDecoding, org/lwjgl/system/StackWalkUtil): they are
the JDK-25 META-INF/versions variants Knot picked, not transformed classes, and would shadow the natives agent's LWJGL.
```

`scripts/prune_overlay.py` is the deleter. `scripts/prune-overlay.sh` is the old one. Do not use it.

```
MCV=26.2 scripts/mkcp-baked.sh
MCV=26.2 scripts/m3a/build-launcher.sh
python3 -I scripts/m3a/pin-gl-options.py inst-26.2/fgamedir/options.txt
MCV=26.2 bash scripts/t4/build-t4.sh
```

`build-launcher.sh` calls `mkcp-baked.sh` again. `pin-gl-options.py` writes `preferredGraphicsBackend:"opengl"`. Run it after the Knot bake, because that run rewrites `options.txt`. `build-t4.sh` writes `inst-26.2/t4.cp` and `inst-26.2/t4-ni.cp`.

The committed reachability JSON is the trace for this classpath:

- `inst-26.2/config/t23-trim/reachability-metadata.json`
- `inst-26.2/config/t23-extra/reflect-config.json`
- `inst-26.2/config/t23-extra/resource-config.json`

Re-run `scripts/t8/gen-reflect-extra.py` only when the classpath changes, and commit the new JSON with the image that used it.

Those three JSON files are in this tree. `*.cp` files are generated and contain absolute paths. `.gitignore` excludes them. Do not commit them.

## 7. Native image

From `work/native-image`. `-O1` is the default when `CPFILE` points at an `inst-*` classpath. `-O2` does not link at this size.

```
MCV=26.2 \
  CPFILE="$PWD/inst-26.2/t4-ni.cp" \
  CONFIG_DIRS="$PWD/inst-26.2/config/t23-trim,$PWD/inst-26.2/config/t23-extra" \
  T8OUT="$PWD/out/t8-26.2" \
  scripts/t8/build-shared.sh

MCV=26.2 T8OUT="$PWD/out/t8-26.2" scripts/t8/relink-xros.sh xros
MCV=26.2 T8OUT="$PWD/out/t8-26.2" scripts/t8/relink-xros.sh xrsimulator
```

`relink-xros.sh` runs `work/xros-probe/scripts/patch_buildver.py`. The device dylib is `out/t8-26.2/xros/libminecraft.dylib`.

`scripts/t8/host-run.sh` is an optional host smoke test of the dylib before you build the app.

`build-shared.sh` and `relink-xros.sh` set their root from the script path (`work/native-image`). `relink-xros.sh` calls `work/xros-probe/scripts/patch_buildver.py`.

## 8. The app

From `visionos/`:

```
MCV=26.2 KLEPTON_TARGET=vivecraft KL_ANGLE_PREBUILT=1 KL_NO_LAUNCH=1 \
  KLEPTON_TEAM="$KLEPTON_TEAM" \
  ./run.sh device
```

`run.sh`, `stage_native.sh`, and `mknative.sh` default `MCV` to 26.2. The launch template in `visionos/native/` is the 1.21.5 text. `stage_native.sh` rewrites version, asset index, game jar, and game-dir paths when `MCV` is not 1.21.5.

A device build defaults to Release (`KL_XCODE_CONFIG=Debug` selects Debug). `KL_BUNDLE_PAYLOAD` defaults on for a vivecraft device build and off for the simulator. Set `KL_BUNDLE_PAYLOAD=1` when the simulator app should carry its own payload. `KL_NO_INSTALL=1` builds locally without looking up or installing on a paired headset.

`make xros` compiles `runtime/xr/kl_openvr.c`. When `vendor-moltenvk/out/include/vulkan/vulkan.h` is present (`make mvk` installs the headers and the MoltenVK frameworks), the OpenVR eye copy is included. Without that header the same file builds and the eye copy is omitted. Vivecraft's guest is OpenGL ES through ANGLE. `visionos/mkmvk.sh` embeds MoltenVK when the frameworks are there, and continues when they are not.

The first install that does not bundle the payload omits `KL_SKIP_STAGE`, so `stage_native.sh` fills the container (jars and assets, on the order of 0.6 GB). A payload build (`KL_BUNDLE_PAYLOAD=1`) runs `stage_native.sh --local` during `xcodebuild` and copies the scrubbed tree into the app. Later container installs can set `KL_SKIP_STAGE=1`.

`visionos/scrub_paths.py` runs on the built app before packaging, and before the simulator's ad-hoc sign. It rewrites the build machine's home prefix inside Mach-O copies when that prefix is 13 bytes, which keeps file offsets valid. The app is signed again after that rewrite. The private build inputs are not modified.

A simulator build is the same command without `device`, on the Apple Vision Pro simulator, visionOS 27.0. Set `KL_XR_STAGE_DROP=1.6` for that run. Set `KL_BUNDLE_PAYLOAD=1` if the simulator app should materialize from the bundle.

`run.sh` generates the Xcode project. Do not commit the generated project. It would capture the team id and the bundle id of the machine that wrote it.

The commands above create your own signed app after all native and game inputs have been built. Follow the paired-headset Xcode/devicectl instructions in [AVP_INSTALL.md](AVP_INSTALL.md). `KL_NO_LAUNCH=1` prevents automatic launch. A successful compile/signature check is not a gameplay test.

## Reproducibility limits

The complete client build includes an interactive host-JVM Knot bake and pruning step. It is not a one-command unattended build. Do not skip it, substitute an unrelated classpath, or present the old Mac/Prism instructions as a Vision Pro install. The jar manifest pins the patched Vivecraft jar hash; a local jar whose hash differs needs investigation before the manifest is changed.

The publication audit checked source paths and script syntax; it did not download dependencies, compile a fresh native image, install a rebuilt app, or validate runtime behavior. Native libraries, game downloads/assets, generated classpaths, caches and signing profiles remain build inputs/outputs outside Git. The OpenAL helper and clean-build bootstrap fixes added for publication require end-to-end validation. Historical notes in `visionos/README.md` describe earlier Klepton targets; use this guide and `AVP_INSTALL.md` for AVPcraft.
