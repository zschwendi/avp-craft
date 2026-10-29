# Third-party components

Everything compiled into the vivecraft-visionos IPA, with the license, version, and upstream that apply. Versions are the ones in the Minecraft 26.2 native image. This is a notice file, not legal advice.

The native image is one library, `libminecraft.dylib`, plus the Klepton runtime and the visionOS frameworks. A player cannot remove a mod jar after install.

Core Klepton code retains its MIT notice, Copyright (c) 2026 Max Thomas, in `LICENSE` at the repo root. Derived components and patches retain their applicable upstream licenses, including the Vivecraft and LTW licenses described below; the root license does not replace those terms. AVPcraft additions do not change those component licenses. Upstream Klepton is https://github.com/shinyquagsire23/Klepton. The fork this port builds is https://github.com/rebelancap/Klepton.

LGPL and GPL texts are in this repo: `LICENSES/LGPL-3.0.txt` and `LICENSES/GPL-3.0.txt`. Apache-2.0, MIT, and BSD-3-Clause texts are in the same directory. Several jars below omit a license file; the notice to ship is named in the row.

## Minecraft client

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| Minecraft client | 26.2 | Minecraft EULA and the Microsoft Services Agreement | https://aka.ms/MinecraftEULA and https://www.microsoft.com/msa |

The client jar is downloaded at build time (sha1 `2dc72797acbc1b63fc16a11c4ac393605f453754`). Asset index 32 is the same. Neither is in git. Both are compiled or staged into the app. The ownership gate is what requires an owning account at launch. It does not change these terms.

Mojang libraries with no license file of their own, shipped as part of the client: `authlib` 9.0.75, `blocklist` 1.0.10, `patchy` 2.2.10, `text2speech` 1.19.12.

## Mods

| Mod | Version | License | Upstream |
| --- | --- | --- | --- |
| Vivecraft | 26.2-1.3.15 | LGPLv3 | https://github.com/Vivecraft/VivecraftMod (`LICENSE.md` on branch `Multiloader-26.2`). Modrinth: https://modrinth.com/mod/vivecraft |
| Sodium | 0.9.2+mc26.2 | Polyform Shield 1.0.0 | https://github.com/CaffeineMC/sodium |
| Lithium | 0.25.3+mc26.2 | LGPL-3.0-only | https://github.com/CaffeineMC/lithium-fabric |
| Fabric API | 0.161.0+26.2 | Apache-2.0 | https://github.com/FabricMC/fabric |
| Fabric Loader | 0.19.5 | Apache-2.0 | https://github.com/FabricMC/fabric-loader |
| Simple Voice Chat | 2.6.24+26.2 | All Rights Reserved | https://github.com/henkelmax/simple-voice-chat |
| Cloth Config | 26.2.155 | LGPLv3 | https://github.com/shedaniel/ClothConfig |
| Entity Culling | 1.11.3 | tr7zw Protective License | https://github.com/tr7zw/EntityCulling |
| FerriteCore | 9.0.0 | MIT | https://github.com/malte0811/FerriteCore |
| ImmediatelyFast | 1.16.5+26.2 | LGPL-3.0-or-later | https://github.com/RaphiMC/ImmediatelyFast |
| Krypton | 0.3.1 | LGPL-3.0 | https://github.com/astei/krypton |
| More Culling | 1.8.1 | GPL-3.0-only | https://github.com/fxmorin/moreculling |
| BadOptimizations | 2.4.1 | MIT | https://github.com/imthosea/BadOptimizations |
| Better Biome Blend | 1.4.0 | LGPL-3.0 | https://github.com/FionaTheMortal/better-biome-blend |

Vivecraft's upstream `LICENSE.md` also covers Minecrift (LGPL-3.0-or-later, Pete Tabram and Mark Browning, 2013), and it restates the notices for JNA, NightConfig, LWJGL, JavaOSC, and Fabric API. The jar itself has no license file. The patches in `work/vivecraft-openxr-26.2/patches/` (0001–0005) are the changes this repo makes, on upstream `openxr` at `13eafae1`, tree `8297f0ecc84bb25aecf714c23dae661cf6cdf626`.

**Sodium.** Polyform Shield 1.0.0, not LGPL. The full text from the jar's `LICENSE.md` is [LICENSES/Polyform-Shield-1.0.0.md](LICENSES/Polyform-Shield-1.0.0.md). The same text ships in the IPA. Canonical URL: https://polyformproject.org/licenses/shield/1.0.0. The license requires that anyone who receives the software also receives these terms, or that URL. The jar has no extra `Required Notice:` lines.

**Simple Voice Chat.** `fabric.mod.json` says All Rights Reserved. The upstream `license` file is one line: Copyright (c) Max Henkel - All Rights Reserved. The project FAQ allows use in a modpack with credit: https://modrepo.de/minecraft/voicechat/faq. Homepage: https://modrepo.de/minecraft/voicechat/overview. Modrinth: https://modrinth.com/mod/simple-voice-chat. Codec notices inside the jar are listed under Native codecs below. They license the codecs, not the mod.

**Entity Culling.** The tr7zw Protective License grants use, modify, and compile. It does not grant distribute. It also refuses commercial advantage and monetary compensation. Text: https://github.com/tr7zw/EntityCulling/blob/1.18/LICENSE-EntityCulling. The Modrinth page says the jar may be redistributed in Modrinth and CurseForge modpacks and in YouTube videos, and not elsewhere. The jar also bundles occlusionculling 0.0.8-SNAPSHOT (LogisticsCraft) under MIT. That MIT grant is for occlusionculling.

**More Culling.** GPL-3.0-only. https://www.gnu.org/licenses/gpl-3.0.html. The project page says modpacks are fine with credit, and asks that a client or another mod not merge this mod without permission: https://modrinth.com/mod/moreculling.

**Krypton** is LGPL-3.0. The jar nests `velocity-native` 3.4.0-SNAPSHOT, which is GPL-3.0, including its Java compression and cipher classes. Upstream: https://github.com/PaperMC/Velocity (relicensed to GPL-3.0 on 2021-03-31). Those classes are on the baked classpath.

**BadOptimizations** and **FerriteCore** are MIT upstream. The jars omit the license text. Ship the MIT notice. BadOptimizations' contact URL is https://github.com/imthosea/BadOptimizations (Modrinth `g96Z4WVZ`).

**Cloth Config** shades toml4j 0.7.2 (MIT) and SnakeYAML 1.27 (Apache-2.0). Ship those notices with it.

## Mod libraries

These are their own jars on the classpath, or are shaded inside a mod that is.

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| voicechat_api | 2.6.24 | All Rights Reserved, with Simple Voice Chat | https://github.com/henkelmax/simple-voice-chat |
| configbuilder | 2.1.0 | No license text | Shaded in Simple Voice Chat. The embedded pom has no license field. https://github.com/henkelmax/config-builder has no LICENSE file. |
| velocity-native | 3.4.0-SNAPSHOT | GPL-3.0 | https://github.com/PaperMC/Velocity |
| conditional-mixin | 0.6.3 | LGPL-3.0-only | https://github.com/Fallen-Breath/conditional-mixin |
| MixinExtras | 0.5.5 | MIT | https://github.com/LlamaLad7/MixinExtras |
| MixinSquared | 0.2.0 | MIT | https://github.com/Bawnorton/MixinSquared |
| cloth-basic-math | 0.6.1 | Unlicense | https://github.com/shedaniel/cloth-basic-math |
| night-config core | 3.6.6 | LGPL-3.0 | https://github.com/TheElectronWill/night-config |
| night-config toml | 3.6.6 | LGPL-3.0 | same |
| tact-java | 0.1.4 | Apache-2.0 | https://github.com/bhaptics/tact-java |
| javaosc-core | 0.9 | BSD-3-Clause | https://github.com/hoijui/JavaOSC. Copyright (c) 2002-2014, Chandrasekhar Ramakrishnan / Illposed Software |
| Java-WebSocket | 1.5.1 | MIT | https://github.com/TooTallNate/Java-WebSocket |
| TRansition | 1.0.26 | MIT | https://github.com/tr7zw/TRansition |
| TRender | 1.0.17 | MIT | https://github.com/tr7zw/TRender |
| sponge-mixin | 0.17.4+mixin.0.8.7 | MIT | The jar's `LICENSE.txt` is the MIT text |

Fabric API modules. All Apache-2.0, same grant as Fabric API, upstream https://github.com/FabricMC/fabric.

| Id | Version |
| --- | --- |
| fabric-advancement-api-v1 | 1.0.0+58719a3b9e |
| fabric-api-base | 2.0.4+ece063239e |
| fabric-api-lookup-api-v1 | 2.0.18+11e1c9a39e |
| fabric-biome-api-v1 | 18.0.6+c7bd5b8e9e |
| fabric-block-api-v1 | 3.1.0+53515aab9e |
| fabric-block-getter-api-v2 | 2.0.7+ec56b6019e |
| fabric-command-api-v2 | 3.1.0+00cb03469e |
| fabric-content-registries-v0 | 11.3.2+37b1aa249e |
| fabric-convention-tags-v2 | 4.7.1+1f023a9e9e |
| fabric-crash-report-info-v1 | 1.0.5+086d547a9e |
| fabric-creative-tab-api-v1 | 5.0.15+6468574f9e |
| fabric-data-attachment-api-v1 | 2.2.19+515ac5339e |
| fabric-data-generation-api-v1 | 25.6.0+c0da0b829e |
| fabric-debug-api-v1 | 1.0.2+c792624d9e |
| fabric-dimensions-v1 | 5.1.12+515ac5339e |
| fabric-entity-events-v1 | 5.0.5+06488ac19e |
| fabric-events-interaction-v0 | 5.2.8+515ac5339e |
| fabric-game-rule-api-v1 | 4.0.8+46a6d00c9e |
| fabric-item-api-v1 | 14.5.0+c68f6cbe9e |
| fabric-key-mapping-api-v1 | 2.0.5+e2bdee789e |
| fabric-lifecycle-events-v1 | 4.1.4+29b6eb019e |
| fabric-loot-api-v3 | 3.0.17+06488ac19e |
| fabric-menu-api-v1 | 2.0.17+086d547a9e |
| fabric-message-api-v1 | 7.0.8+bb2ed3f79e |
| fabric-model-loading-api-v1 | 8.0.17+c80601bb9e |
| fabric-networking-api-v1 | 6.3.4+2989c6a09e |
| fabric-object-builder-api-v1 | 24.1.1+6fcd5f039e |
| fabric-particles-v1 | 5.0.19+d1756aa99e |
| fabric-permission-api-v1 | 1.0.5+e442d8d09e |
| fabric-recipe-api-v1 | 9.0.22+11e1c9a39e |
| fabric-registry-sync-v0 | 7.1.1+c7bd5b8e9e |
| fabric-renderer-api-v1 | 14.1.4+2b0d8a229e |
| fabric-renderer-indigo | 9.1.4+2b0d8a229e |
| fabric-rendering-fluids-v1 | 6.0.4+06488ac19e |
| fabric-rendering-v1 | 25.3.3+515ac5339e |
| fabric-resource-conditions-api-v1 | 6.1.0+10e9a28b9e |
| fabric-resource-loader-v0 | 3.3.20+4fc5413f9e |
| fabric-resource-loader-v1 | 2.0.13+9edec1269e |
| fabric-screen-api-v1 | 5.2.1+5087f8249e |
| fabric-serialization-api-v1 | 2.0.4+11a26f319e |
| fabric-sound-api-v1 | 2.0.5+11a26f319e |
| fabric-tag-api-v1 | 2.1.4+515ac5339e |
| fabric-transfer-api-v1 | 8.0.13+11dbb7d79e |
| fabric-transitive-access-wideners-v1 | 8.1.4+67c847259e |

## Mojang libraries that are openly licensed

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| brigadier | 1.3.10 | MIT | https://github.com/Mojang/brigadier |
| datafixerupper | 10.0.21 | MIT | https://github.com/Mojang/DataFixerUpper |
| logging (`com.mojang:logging`) | 1.7.12 | No license text published with this version | The jar, the Minecraft-libraries POM, and the Gradle module metadata contain no license field. |
| jtracy | 1.0.37 | BSD-3-Clause | https://github.com/wolfpld/tracy. Copyright (c) 2017-2025, Bartosz Taudul. The native jars contain `Tracy_LICENSE` |

## Other Java libraries

Apache-2.0, license file in the jar: `commons-codec` 1.22.0, `commons-compress` 1.28.0, `commons-io` 2.20.0, `commons-lang3` 3.20.0 (each also has `NOTICE.txt`), `failureaccess` 1.0.3, `guava` 33.6.0-jre, `log4j-api` 2.26.0, `log4j-core` 2.26.0, `log4j-slf4j2-impl` 2.26.0.

Apache-2.0, manifest `Bundle-License` only: `gson` 2.14.0, `jspecify` 1.0.0, and the Netty 4.2.15.Final jars: `netty-buffer`, `netty-codec-base`, `netty-codec-compression`, `netty-codec-http`, `netty-common`, `netty-handler`, `netty-resolver`, `netty-transport`, `netty-transport-classes-epoll`, `netty-transport-classes-kqueue`, `netty-transport-native-kqueue` (osx-aarch_64 and osx-x86_64), `netty-transport-native-unix-common`.

Apache-2.0, upstream SPDX, no file in the jar: `fastutil` 8.5.18 (https://github.com/vigna/fastutil), `lz4-java` 1.10.1 (`at.yawk.lz4`).

MIT, evidence in the jar: `jopt-simple` 5.0.4, `slf4j-api` 2.0.17, `msal4j` 1.24.1.

MIT, upstream SPDX, no file in the jar: `joml` 1.10.8 (https://github.com/JOML-CI/JOML), `oshi-core` 6.9.0 (https://github.com/oshi/oshi), `azure-json` 1.4.0 (Azure SDK for Java).

`java-objc-bridge` 1.1 (`ca.weblite`): the jar's pom names Apache License, Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0.txt). The jar has no license file. The notice text is `LICENSES/Apache-2.0.txt`.

BSD-3-Clause, manifest only: `asm` 9.10.1, `asm-analysis`, `asm-commons`, `asm-tree`, `asm-util`. https://asm.ow2.io/LICENSE.txt.

`icu4j` 78.3. Unicode terms, http://www.unicode.org/copyright.html. The manifest reads "© 2016 and later: Unicode, Inc. and others."

`jorbis` 0.0.17 (`org.jcraft`). Upstream JCraft JOrbis is LGPL. The jar does not name the LGPL version. Ship the LGPL text.

JNA 5.17.0 and jna-platform 5.17.0. Dual license: Apache-2.0 OR LGPL-2.1-or-later. This project elects Apache-2.0. https://github.com/java-native-access/jna.

## LWJGL, OpenVR, OpenXR

LWJGL 3 is BSD-3-Clause. Copyright (c) 2012-present Lightweight Java Game Library. https://github.com/LWJGL/lwjgl3. Vivecraft's `LICENSE.md` quotes the BSD-3 text. Natives LWJGL vendors (FreeType, stb, and the others) keep the notices LWJGL documents.

On this classpath:

- LWJGL 3.4.1 (`lwjgl-3.4.1-unsafe` and the macOS arm64 native) and the modules freetype, glfw, jemalloc, openal, opengl, shaderc, spvc, stb, tinyfd, vma, vulkan, each with a macOS arm64 native jar. Source tag `3.4.1`, commit `b800ccffab14396fc529ddb6c931b7c5c5226763`.
- `lwjgl-openvr` 3.3.6 and its natives.
- `lwjgl-openxr` 3.4.1 and its natives. No macOS OpenXR native jar is on the classpath. The visionOS loader is the shim below.

The visionOS build swaps stock `lwjgl-glfw` for klglfw, which is project code on the same LWJGL BSD-3 API (`work/native-image/scripts/t4-src/`).

FreeType 2.14.1, FreeType License (BSD-style), commit `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b`. https://github.com/freetype/freetype.

libffi 3.5.2, MIT, sha256 `f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc`. https://github.com/libffi/libffi. The LWJGL 3.4.1 link uses this version.

## Native code and data in the app

| Component | Version / pin | License | Upstream |
| --- | --- | --- | --- |
| ANGLE `libEGL` and `libGLESv2` (xros and xrsimulator, after `angle_retarget.sh`) | `25e721127e1c6c4c6fa0182b5c234b2c88971175` | BSD-3-Clause | https://chromium.googlesource.com/angle/angle. depot_tools `6afa997717b2c0e1382e1465bedbe1a6855b9388`. The delta is `angle-patches/klepton.patch` |
| GraalVM native-image runtime | JDK 25.0.4 | GraalVM Free Terms and Conditions | https://www.oracle.com/downloads/licenses/graal-free-license.html. The JDK tarball is not redistributed. The image contains the runtime |
| libltw | Pojav LTW `7703f1c29a1fd7f151484c38fb7563bb79ca8ccb` | LGPL-3.0, plus the Darwin port in this repo | https://github.com/PojavLauncherTeam/ltw. Commit `work/natives/ltw/darwin/` and the Makefile |
| libopenxr_loader, libvloader | this repo | MIT (the shims) | `work/natives/openxr-shim/` |
| libklegl | this repo | MIT | `work/native-image/scripts/t8b/` and `klegl_bridge.c` |
| opus4j | `362db14553d39c4dc080e4edd50630b17dbf9682` | JNI glue from the henkelmax repo; Opus is BSD | https://github.com/henkelmax/opus4j |
| rnnoise4j | `5f2097d504fc67f31d0d8900e9df0a5bdf4ec9ae` | JNI glue from the henkelmax repo; RNNoise is BSD-style | https://github.com/henkelmax/rnnoise4j |
| speex4j | `9428cb14b8ec373fcd36f5b5792899611042ece9` | JNI glue from the henkelmax repo; Speex is BSD-style | https://github.com/henkelmax/speex4j |
| lame4j | `b09599989878cbc7349075eca0b3aa4f105e5957` | JNI glue is LGPL-3.0; LAME is LGPL-2.0 | https://github.com/henkelmax/lame4j |
| Phonon HRTF | CIPIC subject 124 as shipped by Valve | Apache-2.0 | Valve steam-audio. File `runtime/data/phonon_hrtf_cipic_124.sofa`, sha256 `c28ff4a874ac889ec0c5885ca524762a70d56984232ff7aadcd9c15d32e1cfb6`. `make phonon-hrtf` refetches it |

Simple Voice Chat's jar also carries these codec notices. Credits: https://modrepo.de/minecraft/voicechat/credits.

| Notice in the jar | What it covers |
| --- | --- |
| `OPUS_LICENSE` | Opus. BSD (Xiph.Org, Skype, and others) |
| `CONCENTUS_LICENSE` | Concentus 1.0.1. BSD-style |
| `RNNOISE_LICENSE` | RNNoise. BSD-style (Jean-Marc Valin, Amazon, Mozilla, Xiph.Org, Mark Borgerding) |
| `SPEEX_LICENSE` | Speex. BSD-style |
| `LAME_LICENSE` | LAME. GNU Library General Public License, version 2 (June 1991) |
| `LAME4J_LICENSE` | LAME4J. LGPL-3.0 |
| `MINIMP3_LICENSE` | minimp3. CC0 1.0 |
| `NATIVE_UTILS_LICENSE` | native-utils 1.0.2. MIT. Copyright (c) 2025 Max Henkel. This grant is only for native-utils |

## LGPL in a native image

LGPL-3.0 section 4 covers a combined work. Shipping one of these libraries inside the static image means the recipient gets the LGPL text, the copyright notices, and a way to relink against a modified copy of that library. The way this repo does that is to publish the patches and the build scripts, and to have the builder supply their own Minecraft client. The client source is not published.

On that path: Vivecraft, Lithium, ImmediatelyFast, Better Biome Blend, Cloth Config, night-config, conditional-mixin, jorbis, Krypton's own code, lame4j, and Pojav LTW.

GPL-3.0 has no equivalent linking exception. More Culling and velocity-native are GPL-3.0 and are in this image. Polyform Shield is not LGPL. Publishing patches does not change Sodium's license. The Sodium text is in this repo because that license asks for the terms to travel with the binary.
