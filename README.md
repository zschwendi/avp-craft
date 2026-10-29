# AVPcraft — Minecraft on Apple Vision Pro

**Minecraft: Java Edition 26.2, running entirely on your Vision Pro.**

AVPcraft compiles Minecraft and its mods ahead of time into a native app, so
the game runs locally on the headset. You don't need a JIT, a debugger or a PC
streaming to you. Play in a window, or choose **Immersive VR** to stand inside
your world with stereoscopic rendering, head tracking and PSVR2 Sense controllers.

AVPcraft is an unofficial, work-in-progress fork of
[rebelancap/vivecraft-visionos](https://github.com/rebelancap/vivecraft-visionos),
built on [Vivecraft](https://github.com/Vivecraft/VivecraftMod) and
[Klepton](https://github.com/shinyquagsire23/Klepton). This fork adds the
AVPcraft launcher and changes to presentation, input and account handling.

[Requirements](#requirements) · [Install](#install) · [First play](#sign-in-and-first-play) · [Controls](#controls) · [Saves](#saves-and-sign-in) · [Status](#current-status) · [Troubleshooting](#troubleshooting)

**Device support:** the current installation guide is for **Apple Vision Pro**.
iPhone, iPad and Mac are not supported installation targets in this guide.

**There's no ready-to-install AVPcraft IPA yet.** For now, you build and sign
the app yourself. The old **v0.1.0** `.mrpack` is from an earlier Mac project
and doesn't install anything on a headset.

## Requirements

| To play | To build |
| --- | --- |
| Apple Vision Pro on **visionOS 26 or later** | Apple silicon Mac with **Xcode 27 and the visionOS SDK** |
| A Microsoft account that **already owns Minecraft: Java Edition**. A Bedrock-only account doesn't pass the Java check. | Memory for a **28 GB build heap** (`native-image -J-Xmx28g`). The build was written for a 36 GB Mac; a 16 GB Mac isn't enough. |
| A compatible gamepad, such as DualSense, for windowed play; **PSVR2 Sense controllers** for immersive play | About **40 GB free**, plus about 12 GB more if you build ANGLE from source |
| | Your own signing team, able to grant the app's required memory capabilities |
| | A headset paired with Xcode, with **Developer Mode** turned on |

Your existing Java Edition purchase is the game license used here; you don't
need to buy a separate Vision Pro edition. Internet access is needed for build
downloads, initial sign-in and online servers.

You don't manually import a desktop Minecraft installation. The build downloads
the matching client, libraries, assets and mods, then embeds the prepared payload
in the app. The complete tool list, including CMake, Ninja, Python and the pinned
Java toolchain, is in [BUILDING.md](BUILDING.md#machine).

## Install

This is a real source build, not a one-command install. Cloning the repository,
or running `run.sh` on its own, doesn't compile the game.

```sh
git clone https://github.com/zschwendi/avp-craft.git
cd avp-craft
```

1. Follow **[BUILDING.md](BUILDING.md)** to build the patched Vivecraft jar and
   the native Minecraft client. It's a long build, and part of it is interactive.
2. Follow **[AVP_INSTALL.md](AVP_INSTALL.md)** to set your signing team and
   bundle identifier, pair the headset, then build the app and install it.
   Use a new build number for your rebuild; **73 is an example**, not a
   downloadable release. Back up an existing install before updating it.
3. Open **AVPcraft** from your Vision Pro's Home View.

## Sign in and first play

1. Choose **Add account**. On a phone or computer, open the Microsoft page
   shown in the launcher and enter the code. Sign in with the account that owns
   Java Edition. If the launcher says you need a Java profile, set one up at
   [minecraft.net/msaprofile](https://www.minecraft.net/msaprofile), then choose
   **Check again**.
2. Under **Play**, choose one of the following:

   - **Create world**, which opens Minecraft's title screen when you have no
     saves; from there, choose **Singleplayer** and create your world
   - **Continue** with your newest world
   - **Title screen**, to choose a different saved world through Singleplayer
   - **Join server**, then type a compatible Java server address
3. Under **Play mode**, choose **Windowed** or **Immersive VR**. If you choose
   Immersive VR, you also choose a **Resolution**. Sharp is the tested default.
4. Answer the microphone prompt, which is used for voice chat. **Allow or
   decline**; either answer lets you continue. Play waits while it is unanswered.
5. Press **Play in a window** or **Play in VR**.

Pair your controller with the headset in visionOS Bluetooth settings before
playing. For VR, pair both PSVR2 Sense controllers and allow tracking permissions
when asked. Press the Digital Crown to see your room. Hand tracking provides
limited input; complete play with hands alone is not supported by this guide.

**Keep the launcher window open during immersive play.** Closing it can stop
the audio. To change play mode, save your game, then close and reopen the app.

## Controls

Windowed play uses a compatible gamepad. The mapping below uses DualSense button
names. Sense controllers are handled by the immersive input path, not the
windowed gamepad path. Use the normal visionOS look-and-pinch gesture to select
launcher menu items.

| Action | DualSense |
| --- | --- |
| Move · Look | Left stick · Right stick |
| Jump · Inventory | Cross · Triangle |
| Attack/select · Use | R2 · L2 |
| Previous · next hotbar slot | L1 · R1 |
| Pause | Circle or Options |

In Immersive VR, Vivecraft provides motion controls for the Sense controllers.

## Saves and sign-in

Worlds and accounts are stored on the headset. Sign-in is remembered and refreshed
automatically, although Microsoft may require you to sign in again. Choose the
same account to return to its worlds; each account has a separate game profile.

An in-place update with the same signing team and bundle identifier is meant to
keep your data. A new bundle identifier creates a separate app with its own data.
**Back up before updating**
(see [AVP_INSTALL.md](AVP_INSTALL.md)), and don't uninstall to fix problems.
There is no iCloud save sync in this version.

## Mods and multiplayer

Vivecraft and the bundled performance mods are compiled into the native client.
Adding or changing Java code mods requires a compatible mod set, another bake
and a rebuilt native client. Dropping extra `.jar` files onto the headset does
not add their code to the installed app.

Graphics use OpenGL ES through **ANGLE on Metal**. The earlier Mac graphics
modpack and custom renderer are not included in this Vision Pro source release;
arbitrary desktop shaders and mods are not a tested feature.

You can join compatible **Java 26.2** servers while signed in with an account
that owns Java Edition. The **Offline** account can't join servers that require
Microsoft authentication.
There's no built-in Bedrock crossplay.

## Current status

The source is based on **AVPcraft 0.4.0 / build 71**, which is the baseline
tested on a physical Vision Pro. The source also moves the head-locked HUD
(health, food and hotbar) **15 degrees lower**. To get that change, you must
rebuild Vivecraft, the native game client *and* the host app. The updated jar
and its migration checks pass. A new native image and the HUD's look on a
headset haven't been verified. The separate build 72 audio changes aren't in this
source. See [HUD placement](docs/HUD-PLACEMENT.md) and
[SOURCE-SNAPSHOT.json](SOURCE-SNAPSHOT.json).

The complete source-to-headset build has not yet been repeated from a clean
public checkout. These are developer build instructions, not a verified
one-click installer. Seamless switching between windowed and immersive play,
audio after closing the launcher, and hands/controller presentation remain
unfinished.

## Troubleshooting

- **Build fails with missing `.dylib`, `.xcframework` or asset files:** complete
  the matching step in [BUILDING.md](BUILDING.md).
- **The launcher asks you to sign in with an account that owns Java Edition:**
  use the account that owns Java Edition, not Bedrock. Check that its Java
  profile is set up, then choose **Check again**.
- **No sound in immersive mode:** keep the launcher window open. If you already
  closed it, save your game and reopen the app.
- **Immersive mode is sluggish:** choose **Balanced** resolution before
  launching. **Max** may judder.
- **Play is disabled:** finish account sign-in and answer the microphone prompt.
- **The lower HUD isn't visible after an update:** rebuild the native game
  client as well as the host app; an old compiled client keeps the old placement.

## Contributors and agents

Start with [AVP_INSTALL.md](AVP_INSTALL.md), [BUILDING.md](BUILDING.md) and
[SOURCE-SNAPSHOT.json](SOURCE-SNAPSHOT.json). Preserve the pinned dependencies,
account data and saves. Keep source/build results separate from actual headset
testing when reporting a change.

| Location | Purpose |
| --- | --- |
| `visionos/` | Launcher, account handling, input and windowed/immersive host |
| `runtime/` | Klepton runtime and XR integration |
| `work/native-image/` | Dependency pins and native compilation scripts |
| `work/vivecraft-openxr-26.2/` | Vivecraft patches, HUD checks and jar packaging |
| `work/natives/` | Native dependency builds |

Do not commit downloaded game files, generated native clients, saves, account
tokens or signing material. Fixes and pull requests are welcome.

## Credits and licenses

- [rebelancap/vivecraft-visionos](https://github.com/rebelancap/vivecraft-visionos):
  the Vision Pro port this fork builds on.
- [Klepton](https://github.com/shinyquagsire23/Klepton) by
  [shinyquagsire23](https://github.com/shinyquagsire23), via
  [rebelancap/Klepton](https://github.com/rebelancap/Klepton): the app and
  runtime foundation.
- [Vivecraft](https://github.com/Vivecraft/VivecraftMod), Minecrift and
  [QuestCraft](https://github.com/QuestCraftPlusPlus/QuestCraft): VR support
  and controller work.
- Fabric, Sodium, ANGLE, LWJGL, GraalVM and more: see
  [THIRD-PARTY.md](THIRD-PARTY.md).

Each component keeps its own license. See [LICENSE](LICENSE),
[LICENSES/](LICENSES/) and [UPSTREAM.md](UPSTREAM.md). This repository doesn't
include Minecraft or its assets. AVPcraft isn't affiliated with Mojang,
Microsoft or Apple.

## Issues

Check [existing issues](https://github.com/zschwendi/avp-craft/issues) first.
When you report a bug, include:

- the build number, shown at the bottom of the launcher
- your visionOS version
- your play mode and controller
- steps to reproduce
- what you expected to happen

Remove account names, tokens and other private details from logs or recordings
before you share them.
