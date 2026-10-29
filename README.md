# AVPcraft

**Minecraft: Java Edition on Apple Vision Pro, with windowed play, immersive VR, and PSVR2 Sense motion controls.**

The game runs locally on the headset. No PC streaming, no JIT. Minecraft and its mods are compiled ahead of time into native code.

AVPcraft is a work-in-progress fork of [rebelancap/vivecraft-visionos](https://github.com/rebelancap/vivecraft-visionos), built on [Vivecraft](https://github.com/Vivecraft/VivecraftMod) and [Klepton](https://github.com/shinyquagsire23/Klepton). Their work makes this possible. This fork adds the AVPcraft launcher and changes to presentation, input, audio, and account handling.

## What you can do

- Play Minecraft Java Edition **26.2** in a window on Vision Pro.
- Choose **Immersive VR** to play inside the world with stereoscopic rendering and head tracking.
- Use **PSVR2 Sense controllers** for immersive movement and interactions.
- Sign in with a Microsoft account that owns Minecraft: Java Edition, choose a world, or join a server.

Choose **Windowed** or **Immersive VR** in the launcher before starting the game. Windowed is the default. Changing modes during a running session is not supported in this version.

## Current version

This source tree is based on **AVPcraft 0.4.0 / build 71**, with a new **15-degree lower head-locked HUD** source update. Health, food and item slots use the lower placement; inventory and menus keep their placement. [SOURCE-SNAPSHOT.json](SOURCE-SNAPSHOT.json) distinguishes the tested build 71 baseline from the new source changes. The separate build 72 audio changes are not included.

**The HUD update requires rebuilding Vivecraft and the compiled native game client, as well as the host.** Reusing an old `libminecraft.dylib` will not apply the new placement. The updated game jar and migration checks pass; a new native image and headset appearance have not been verified. See [HUD placement](docs/HUD-PLACEMENT.md).

This repository now contains the Vision Pro app source and build tools. The old **v0.1.0** release and its `.mrpack` belong to the earlier **Mac Metal renderer project**. They are not an AVPcraft installer.

**There is no ready-to-install AVPcraft IPA published here yet. Installation currently means building and signing the app yourself.** The compiled Minecraft client, game assets, account data, and saves are not included in this repository.

## Install

Start with **[AVP_INSTALL.md](AVP_INSTALL.md)**. It explains the installation route for a person or an agent helping them. **[BUILDING.md](BUILDING.md)** contains the full dependency and native-client build recipe.

You will need:

- An Apple Vision Pro running visionOS 26 or later.
- A Microsoft account that owns **Minecraft: Java Edition**.
- An Apple Silicon Mac with **Xcode 27 and the visionOS SDK**.
- Your own Apple signing team and a paired headset with Developer Mode enabled.
- The build tools, memory, and disk space listed in the build guide. Compiling the native Minecraft client is a substantial build.

The process is:

1. Clone this repository.
2. Follow the build guide to fetch the pinned dependencies and build the native Minecraft client. Game files and dependency downloads stay outside Git.
3. Build the AVPcraft host with your own signing settings and install it on your paired Vision Pro.
4. Open **AVPcraft**, choose **Add account**, and sign in with the Microsoft account that owns Java Edition.
5. Select a world and play mode. For immersive play, pair your Sense controllers and choose **Play in VR**.

```sh
git clone https://github.com/zschwendi/avp-craft.git
cd avp-craft
```

The account check happens on the headset. Building the app does not replace owning Minecraft. Do not use an upstream SideStore feed expecting it to install this fork; that would install a different app.

### If an agent is helping you

Have it read [AVP_INSTALL.md](AVP_INSTALL.md), [BUILDING.md](BUILDING.md), and [SOURCE-SNAPSHOT.json](SOURCE-SNAPSHOT.json) before running commands. It should check the required SDKs and dependencies, use your signing identity, and preserve existing worlds and account data. A successful compile is not proof that the game installed or plays correctly: verify the installed app and test it on the headset.

## Known issues

This is still early. It is not a polished release, and bug reports are welcome.

- **Audio can stop when you close the launcher window while playing in immersive mode.** Keep the launcher window open for now.
- **Windowed and immersive modes are selected before launch.** Seamless switching while keeping the same running world is still being worked on.
- **Immersive UI placement and controller behavior need more polish.** Some interactions can feel rough, and behavior may differ between worlds or setups.

Found a reproducible bug or have a concrete feature request? Check [existing issues](https://github.com/zschwendi/avp-craft/issues) first, then open one. For bugs, include the app/build version, headset and visionOS version, play mode, controller type, steps to reproduce, and what you expected versus what happened. For features, explain the use case. A short recording or relevant log excerpt helps; remove account details, tokens, and other private information before sharing. Please keep Issues focused on bugs and feature requests.

Fixes and pull requests are welcome too.

## What's in this repository

| Path | Purpose |
| --- | --- |
| `visionos/` | AVPcraft launcher, windowed/immersive host, input, account handling, and Xcode project generator |
| `runtime/` | Klepton runtime and XR integration |
| `work/native-image/` | Minecraft/mod dependency pins, patches, and ahead-of-time compilation scripts |
| `work/natives/` | Native dependency build recipes and integration code |
| `vendor/out/` | Prebuilt ANGLE frameworks and their license notice |
| `LICENSES/`, `THIRD-PARTY.md` | Component licenses, attribution, and source references |

## Credits and licenses

- [rebelancap/vivecraft-visionos](https://github.com/rebelancap/vivecraft-visionos) — the Vision Pro port this fork builds on.
- [Klepton](https://github.com/shinyquagsire23/Klepton) by [shinyquagsire23](https://github.com/shinyquagsire23), via [rebelancap/Klepton](https://github.com/rebelancap/Klepton) — the app/runtime foundation.
- [Vivecraft](https://github.com/Vivecraft/VivecraftMod), Minecrift, and [QuestCraft](https://github.com/QuestCraftPlusPlus/QuestCraft) — VR support and related controller work.
- Fabric, Sodium, ANGLE, LWJGL, GraalVM, and the other projects listed in [THIRD-PARTY.md](THIRD-PARTY.md).

Components retain their respective licenses; see [LICENSE](LICENSE), [LICENSES/](LICENSES/), [THIRD-PARTY.md](THIRD-PARTY.md), and [UPSTREAM.md](UPSTREAM.md). Minecraft and its assets remain subject to Mojang/Microsoft's terms and are not distributed in this source tree.

AVPcraft is an unofficial project, not affiliated with Mojang, Microsoft, or Apple.
