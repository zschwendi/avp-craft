# Install AVPcraft on Apple Vision Pro

AVPcraft runs Minecraft: Java Edition locally on Vision Pro using Vivecraft
and Klepton's ahead-of-time compilation. It offers windowed and immersive
play. **Both modes remain under testing**, with known immersive issues.

## Requirements

- Apple silicon Mac with **Xcode 27 and the visionOS SDK**.
- Apple Vision Pro on **visionOS 26 or newer**, paired with Xcode and with
  Developer Mode enabled.
- Memory for a **28 GB native-image build heap**. The build was written for
  a 36 GB Mac; a 16 GB Mac is insufficient.
- About **40 GB free disk space**, plus about 12 GB more to build ANGLE from source.
- Your own signing team and explicit app identifier with the required
  Increased Memory Limit and Extended Virtual Addressing capabilities.
- A compatible gamepad for windowed play; PSVR2 Sense controllers for VR.
- A Microsoft account that owns **Minecraft: Java Edition**. Bedrock-only
  ownership does not satisfy the Java ownership check.

The complete tools and pinned dependency list is in
[BUILDING.md](BUILDING.md#machine).

## Game files and ownership

You do not import a desktop Minecraft folder or a disc image. The build
downloads the matching Minecraft **26.2** client, libraries, assets, and mods,
then prepares a native client and payload. These generated inputs are not
committed to this repository.

Sign in through the app's Microsoft flow using your existing Java Edition
account. A separate Vision Pro game purchase is not required. Keep account
tokens, worlds, and generated game files private.

## Build from an existing checkout

If the AVP Ports Index cloned a scanned commit for you, keep that checkout.
Do not pull newer source or switch branches during the following steps.

1. Follow **[BUILDING.md](BUILDING.md)** from the repository root. Complete
   the native dependencies, patched Vivecraft jar, matching game downloads,
   classpath bake, native-image compilation, and payload preparation.
   Dependency downloads and their pinned revisions are part of that process.
2. Follow **[AVP_INSTALL.md](AVP_INSTALL.md#your-signing-settings)** to set
   your own signing team, bundle ID, product variables, and build number.
   The underscore-named file is the detailed installation guide; this
   hyphen-named file is the index's entry point.
3. After all matching native inputs exist, use the guide's prepared-checkout
   build/install commands below.

A clone or host-only rebuild does **not** compile Minecraft. The lower-HUD
change requires rebuilding Vivecraft and the native game client as well as
the host. The full build has not been repeated from a clean public checkout;
see [source status](README.md#current-status).

## Install on Apple Vision Pro

After setting the variables in the detailed guide, select the intended
physical headset:

```sh
xcrun devicectl list devices
export KLEPTON_DEVICE="YOUR_VISION_PRO_IDENTIFIER"
```

If updating, save and quit your world and follow the guide's save-backup step
first. Keep your existing signing team and bundle ID.

From the repository root, with all matching build inputs prepared:

```sh
cd visionos
KL_NO_LAUNCH=1 ./run.sh device
```

The script builds and signs the host, installs it, and stages the payload.
It stops before launch. Open **AVPcraft** from Home View, choose **Add account**,
complete the Microsoft sign-in, and follow
[sign-in and first play](README.md#sign-in-and-first-play).

## Controls and known issues

Use a gamepad for windowed play and paired PSVR2 Sense controllers for
Immersive VR. Allow or decline the microphone prompt before pressing Play.
Complete hands-only gameplay is not supported.

Windowed play needs further testing. Immersive presentation, audio after
closing the launcher, and hands/controller presentation remain unfinished.
**Keep the launcher open during VR.** Save, close, and reopen the app to
change play modes; seamless switching is unfinished.

See [controls](README.md#controls),
[troubleshooting](README.md#troubleshooting), and
[saves and sign-in](README.md#saves-and-sign-in). Back up before updating;
do not uninstall the app to troubleshoot it.
