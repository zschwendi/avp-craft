# work/vivecraft-openxr-26.2 — provenance (written BEFORE fetching, 2026-10-12)

Stage B of M3: self-built Vivecraft OpenXR (mobile/GLES path) for Minecraft 26.2.
Everything below is re-fetchable; the clone, toolchain and build outputs are
safe to delete (only REPORT.md, PROVENANCE.md, check-touch-map.py and patches/ are committed).

| Item | Source | Pin | Local path | Re-fetch |
|---|---|---|---|---|
| Vivecraft upstream, branch `openxr` | https://github.com/Vivecraft/VivecraftMod | `13eafae1` ("update to 26.2", 2026-07-27) | `VivecraftMod/` | `git clone -b openxr https://github.com/Vivecraft/VivecraftMod VivecraftMod && git -C VivecraftMod checkout -B kl-mobile 13eafae1` then `git -C VivecraftMod am ../patches/*.patch` |
| QuestCraft Vivecraft fork, branch `OpenXR-1.21.10` (reference only, newest QC OpenXR branch) | https://github.com/QuestCraftPlusPlus/VivecraftMod | `0747838801a9` (2025-11-28, "Fix a couple issues") | `qc-OpenXR-1.21.10/` | `git clone --depth 1 -b OpenXR-1.21.10 https://github.com/QuestCraftPlusPlus/VivecraftMod qc-OpenXR-1.21.10` |
| Oracle GraalVM for JDK 25 (macOS aarch64), used as the JDK 25 for Gradle and later native-image | https://download.oracle.com/graalvm/25/latest/graalvm-jdk-25_macos-aarch64_bin.tar.gz (~354 MB, Last-Modified 2026-07-16) | `graalvm-jdk-25.0.4+7.1` | `../native-image/graalvm/graalvm-jdk-25.0.4+7.1` (sibling of the 21 one; tarball not kept) | `curl -LO <url> && tar xzf graalvm-jdk-25_macos-aarch64_bin.tar.gz -C work/native-image/graalvm/` |
| Gradle 9.3.0 wrapper + Loom 1.15.5 + Minecraft 26.2 (+ asset index) + Fabric deps | downloaded by `./gradlew` from services.gradle.org / maven.fabricmc.net / Mojang | per VivecraftMod `gradle/wrapper` + `gradle.properties` | `gradle-home/` (GRADLE_USER_HOME pinned here, NOT ~/.gradle; 1.2 GB) | rerun the build (REPORT.md) |
| Fabric API for the run test | maven.fabricmc.net `net.fabricmc.fabric-api:fabric-api` | 0.161.0+26.2 (run 3) / 0.152.1+26.2 (upstream pin, runs 1-2) | `gradle-home/` | `-Pfabric_api_version=0.161.0+26.2` |
| Host OpenXR probe + VLoader stub (run 2 only) | copies of `../native-image/out/t23/natives/{libopenxr_loader,libvloader}.dylib`, built by `../native-image/scripts/t3-src/build-t23.sh` from `scripts/t3-src/native/{openxr_probe,vloader_stub}.c` (2026-10-12 00:54) | as copied 2026-10-12 | `host-probe/` | re-copy, or rebuild with build-t23.sh |

Cleanup: `rm -rf VivecraftMod qc-OpenXR-1.21.10 gradle-home host-probe *.log` loses nothing that
REPORT.md + patches/ cannot rebuild (GraalVM 25 sits with the native-image toolchain and is kept).
