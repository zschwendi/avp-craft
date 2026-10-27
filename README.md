# AVPcraft

Native Apple Metal rendering for Minecraft: Java Edition 26.3, with shader lighting and real curved edges on selected terrain blocks and first-person arms. This is an experimental community fork, not a Mojang product.

[Download the Prism import pack](https://github.com/zschwendi/avp-craft/releases/download/v0.1.0/minecraft-metal-mac-v0.1.0.mrpack) · [Release notes](https://github.com/zschwendi/avp-craft/releases/tag/v0.1.0)

AVPcraft was previously named Minecraft Metal Mac. The published Mac pack keeps its existing `minecraft-metal-mac-v0.1.0.mrpack` filename; existing Prism instances continue to work.

## Install

1. Install the Apple Silicon build of [Prism Launcher](https://prismlauncher.org/download/).
2. Download the `.mrpack` from [Releases](https://github.com/zschwendi/avp-craft/releases). In Prism, choose **Add Instance → Import** and select it. Review the three project mods when Prism prompts about GitHub downloads.
3. In **Settings → Java**, enable **Auto-detect Java version** and **Auto-download Mojang Java**. Prism then obtains ARM64 Java 25 and the pinned Minecraft/Fabric/mod dependencies. In the instance's **Edit → Settings → Java**, choose about 4 GB of maximum memory if your machine has enough available RAM. See [Prism's Java setup guide](https://prismlauncher.org/wiki/getting-started/installing-java/).
4. For the full game, sign into the Microsoft account that owns Minecraft Java and launch. Prism's normal **Play Demo** option can preview graphics without an entitlement; the demo's normal restrictions apply.

On macOS, you can also open the import directly using Prism's [documented command-line interface](https://prismlauncher.org/wiki/getting-started/command-line-interface/):

```sh
open -n -a "/Applications/Prism Launcher.app" --args --import "https://github.com/zschwendi/avp-craft/releases/download/v0.1.0/minecraft-metal-mac-v0.1.0.mrpack"
```

Use a fresh imported instance. This renderer replaces other `mcopt-metal` variants; installing several variants together produces duplicate mod IDs. The pack starts with vanilla textures, Balanced lighting, 1080p output, 12-chunk render distance, 6-chunk simulation distance and a 60 FPS cap. You can tune these in Minecraft or `config/mcopt.properties`.

Minecraft's trial can demonstrate local graphics, but Mojang disables online-server joining in [demo mode](https://www.minecraft.net/en-us/free-trial). Prism's Microsoft-account flow does not grant game ownership. Check an existing account's entitlement first. An authenticated Java 26.3 client can join ordinary compatible Java servers; other players do not need these visual mods. Bedrock crossplay requires a separate compatible Java-server bridge. The [Prism importer](https://github.com/PrismLauncher/PrismLauncher/blob/7d2d3c1ec6fcf8255ebacd02d9f792688575dfb4/launcher/modplatform/modrinth/ModrinthInstanceCreationTask.cpp) downloads release files and verifies their hashes using the [MRPACK format](https://support.modrinth.com/en/articles/8802351-modrinth-modpack-format-mrpack).

## What this adds

- Direct Metal lighting, shadows, volumetric clouds and reflective water from the upstream renderer.
- Neighbor-aware quarter-circle terrain edges and spherical corners, with selection outlines generated from the same mesh.
- Curved first-person arm and sleeve cuboids that retain the existing skin UVs and animations.
- Optional static LabPBR normal/AO/height-map support when a separately obtained compatible resource pack supplies the data. Vanilla assets use neutral material fallbacks.

The fork skips faded-out material shading, reuses cached terrain meshes, keeps per-block diagnostics off during normal play, and fixes a presentation-ring case that discarded a fresh frame despite an available slot. These changes have focused regression checks; their effect on gameplay still needs controlled measurements.

These are raster effects and geometry. This project does not claim hardware ray tracing, path tracing, fingers, changed terrain collision or exact mesh picking. Curved terrain remains logically Minecraft blocks. No paid or private resource pack is included.

## Compatibility and evidence

Requires Apple Silicon, macOS 26+, ARM64 Java 25, Minecraft 26.3, Fabric Loader 0.19.5 and Sodium 0.9.3-alpha.1. Intel/Rosetta, Windows and Linux are outside this renderer build's supported scope. Runtime work has focused on an M4 Mac mini; older Apple Silicon models and OS versions need separate testing. The mod's native library is not an independently notarized application. Use Apple's normal trusted-application approval process; this project does not disable Gatekeeper or remove quarantine flags.

The geometry has CPU topology and UV checks. Build checks and native shader/pipeline compilation establish narrower facts than gameplay smoothness. There is no public controlled FPS gain, frame-latency benchmark or 60 FPS guarantee yet. World complexity, output resolution, warm-up and competing apps affect performance. Do not treat upstream benchmarks on other Macs as measurements of this fork.

The v0.1.0 client passed a startup check on an M4 Mac mini running macOS 27: the official demo world loaded through Metal with vanilla textures, a 2×2 neutral-material atlas, curved terrain and arm mesh submission, clouds, shadows and the water scene copy. This verifies startup and initialization; movement quality and older Mac/OS combinations need further testing.

The published pack also imported successfully in Prism 11.1.1 through its normal source-review dialog; all five downloaded mods, version pins and preset files matched the release. The first client session logged a chunk-loading timeout. That remains an unresolved runtime warning, and visual correctness and frame pacing are not established by the startup log.

## Build from source

Install an ARM64 Java 25 JDK, Python 3 and Xcode 26 or newer (macOS SDK 26+). The native library uses a macOS 15 deployment target, but the current mod startup check requires macOS 26 or newer. The newer SDK supplies optional MetalFX APIs. Then:

```sh
python3 tools/audit.py
python3 tools/test_tools.py
python3 tools/build.py --version 0.1.0
python3 tools/check_source.py
python3 tools/release.py --tag v0.1.0
python3 tools/audit.py --pack dist/minecraft-metal-mac-v0.1.0.mrpack
```

The build fetches the verified upstream source commit and pinned compiler dependencies into ignored `.cache/` files. It builds the ARM64 native library and three owned mod jars. Minecraft, LWJGL, Sodium and Fabric dependencies are not embedded in those jars or the import pack. Prism obtains runtime dependencies from their original hosts. The release preparation script creates files locally and does not upload them.

Maintainers: see [source integration](docs/MAINTAINERS.md). Builds require the reviewed source snapshot and its hash manifest. Release preparation does not publish files or establish gameplay performance.

## Credits and licenses

Built on [Noah Dunnagan's mcopt](https://github.com/noahdunnagan/mcopt) and [Gael Quintanilla's Metal Shaders fork](https://github.com/GaelQuinta/minecraft-metal-shaders), pinned at commit `e555c0334e0e061d3a84366ef135af2b13bebf51`. Sodium hooks and adapted terrain shader portions retain the PolyForm Shield 1.0.0 license. Other upstream renderer code is Apache 2.0, with the MIT portions identified in its NOTICE. Original terrain code is Apache 2.0; arm code is MIT. See [license inventory](docs/LICENSES.md) and the preserved upstream [NOTICE](LICENSES/upstream/NOTICE).

Minecraft belongs to Mojang/Microsoft. Sodium, Fabric and Prism are separately maintained projects. Their dependency downloads and licenses remain separate from this fork.
