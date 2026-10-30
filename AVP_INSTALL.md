# Install AVPcraft on Apple Vision Pro

**Lower-HUD source update:** the current repo places the head-locked health/food/hotbar 15 degrees lower only after rebuilding the patched Vivecraft jar and native game image. A host build with an older prepared `libminecraft.dylib` will retain the old game-side placement. Follow [BUILDING.md](BUILDING.md) and [HUD placement](docs/HUD-PLACEMENT.md) for that rebuild.

A fresh public checkout has produced a signed AVPcraft Build 73 app, including the rebuilt lower-HUD game image and Build 72 host changes. It remains a developer source release: the fresh app still needs physical-headset gameplay acceptance. See [clean-build evidence](docs/CLEAN-BUILD.md) for the exact build, signing and installation boundary, and [build recovery](docs/BUILD-RECOVERY.md) for the original repaired blockers.

## What you need

- An Apple Silicon Mac, Xcode 27 with the visionOS SDK, and the tools/dependencies in [BUILDING.md](BUILDING.md).
- An Apple signing account/team with an explicit app identifier and the memory capabilities required by `visionos/Klepton.entitlements` (Extended Virtual Addressing and Increased Memory Limit).
- A paired, unlocked Vision Pro with Developer Mode enabled. Pair through Xcode's Devices and Simulators window and accept the prompts on the headset. Use a visionOS version compatible with the selected SDK and deployment targets.
- Minecraft: Java Edition ownership. The app's Microsoft sign-in/ownership flow remains enabled; do not put access or refresh tokens in this repository or build commands.

## Build all inputs first

Follow [BUILDING.md](BUILDING.md) to build the patched Vivecraft jar, download the matching Minecraft 26.2 assets/libraries, bake the classpath, compile the native image, and build the native dependencies. Source alone is not enough to run `xcodebuild`: the project embeds generated native libraries and a staged game payload. Minecraft jars/assets and the compiled client are not committed.

Build 72 was a host update that reused Build 71's compiled game client. The command below is the same kind of host reconstruction **only if those matching inputs already exist**. A new native-image build must be tested separately; it is not automatically the installed binary.

## Your signing settings

Run from the repository root. Keep the clone path free of spaces for the inherited shell scripts.

```bash
export ROOT="$PWD"
export MCV=26.2 KLEPTON_TARGET=vivecraft
export KLEPTON_PRODUCT_NAME=AVPcraft KLEPTON_PROJECT_NAME=avp-craft
export KLEPTON_DISPLAY_NAME=AVPcraft
export KLEPTON_MARKETING_VERSION=0.4.0 KLEPTON_BUILD_VERSION=73
export KLEPTON_TEAM="YOUR_APPLE_TEAM_ID"
export KLEPTON_BUNDLE_ID="com.yourname.avpcraft"
export KLEPTON_LOW_LATENCY=0
export KL_BUNDLE_PAYLOAD=1 KL_ANGLE_PREBUILT=1
```

Use your own unique bundle identifier. Do not use the developer's default identifier with a different team. Keep your identifier stable: a different identifier creates another app and another save container. Automatic signing is enabled; do not also force a `CODE_SIGN_IDENTITY` value.

`73` is an example build number for a new build of the current source, not a published release. Use a number higher than your previous installation and keep the same signing team and bundle identifier when updating it.

## Build and install a prepared checkout

List devices, then explicitly select the intended Vision Pro identifier:

```bash
xcrun devicectl list devices
export KLEPTON_DEVICE="YOUR_VISION_PRO_IDENTIFIER"
```

For an existing installation, save and quit your world, then back up its worlds **before updating or changing the Minecraft version**. Skip this backup step on a first installation:

```bash
cd "$ROOT/visionos"
./stage_native.sh --backup-saves "$KLEPTON_DEVICE"
```

Use the bundle id of the install whose saves you are protecting. Do not uninstall the app as a troubleshooting step without preserving its data.

Build and install:

```bash
cd "$ROOT/visionos"
KL_NO_LAUNCH=1 ./run.sh device
```

This builds the native host runtime, wraps the already-built native libraries and ANGLE frameworks, generates `avp-craft.xcodeproj`, builds/signs the `AVPcraft` scheme, installs on the selected headset, and stages the payload. It stops before launch. It does **not** create the Minecraft native image for you. Open AVPcraft from the headset's Home View and follow [sign-in and first play](README.md#sign-in-and-first-play). Verify windowed gameplay before immersive mode and controller input.

To build your own local signed app without looking up or installing on any headset:

```bash
cd "$ROOT/visionos"
KL_NO_INSTALL=1 KL_NO_LAUNCH=1 KL_SKIP_STAGE=1 ./run.sh device
```

The app is written to `visionos/build/dd-device-vivecraft/Build/Products/Release-xros/AVPcraft.app`. The bundled-payload build still prepares the local payload; `KL_SKIP_STAGE` only skips the separate container upload.

## Direct host rebuild with existing frameworks and payload inputs

For developers who already have the matching `build/Klepton.xcframework`, `visionos/Frameworks` and game/classpath inputs, this is the Build 72 host recipe with private signing values removed:

```bash
cd "$ROOT/visionos"
export XROS_PLATFORMS=xros KLEPTON_BUILD_MODE=device
python3 gen_xcodeproj.py
xcodebuild -project avp-craft.xcodeproj -scheme AVPcraft \
  -configuration Release -destination 'generic/platform=visionOS' \
  -derivedDataPath build/dd-device-avpcraft -jobs 2 \
  -allowProvisioningUpdates KL_BUNDLE_PAYLOAD=1 KLEPTON_TARGET=vivecraft \
  MCV=26.2 INFOPLIST_KEY_CFBundleDisplayName=AVPcraft build
```

This bypasses rebuilding/wrapping dependencies. Do not use it with stale frameworks. After a successful build, install the resulting `build/dd-device-avpcraft/Build/Products/Release-xros/AVPcraft.app` with Xcode or `xcrun devicectl device install app --device "$KLEPTON_DEVICE" <app-path>`. The `run.sh` route above handles additional path scrubbing, signature verification and staging.

## Failure checks for a human or coding agent

- Missing `.dylib`, `.xcframework`, classpath or asset-index files: complete the named dependency step in BUILDING.md. Do not relabel macOS libraries as visionOS libraries.
- Signing/profile failure: check your team, unique bundle id and required memory capabilities. `KLEPTON_LOW_LATENCY=0` excludes the optional low-latency entitlement. Dropping memory entitlements can reduce the headroom needed to load the game and is not the tested setup.
- Device locked or unavailable: unlock/wear it, confirm pairing and retry the install. A signed local app does not prove that it installed.
- No audio after closing the launcher: keep the launcher open. Build 72's audio policy changes are now included, but this specific behavior still needs headset verification.
- Report build, signature, install, launch and actual gameplay separately. Verify window/immersive transitions, audio, saves and your input devices on the headset before calling a personal rebuild successful.

An agent may prepare dependencies and inspect errors, but signing-account prompts, headset trust/Developer Mode and gameplay acceptance still require the owner's participation. Do not claim those steps occurred from build logs alone.
