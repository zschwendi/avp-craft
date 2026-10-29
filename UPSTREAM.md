# Upstream pins

Snapshot of the private Vivecraft port at commit `7c4407952bb31ae4b8bb67c7b51a96742ce69df7` (branch `vivecraft`). The earlier plan pin `273eed9f` is behind this commit. This file records the pins. It does not vendor the JDKs, the Minecraft client, or the mod jars.

## ANGLE

- Source: https://chromium.googlesource.com/angle/angle.git at `25e721127e1c6c4c6fa0182b5c234b2c88971175`
- depot_tools: `6afa997717b2c0e1382e1465bedbe1a6855b9388`
- Patch: `angle-patches/klepton.patch`
- Committed frameworks (BSD-3-Clause notice in `vendor/out/LICENSE`):

The committed copies below had their compile-path prefix rewritten in place (`visionos/scrub_paths.py`) and were re-signed ad-hoc. The hashes are of those files. A raw ANGLE build on another machine will not match them byte for byte.

| File | sha256 |
| --- | --- |
| `vendor/out/xros/libEGL.framework/libEGL` | `2c0b97395e75c38a4e0c491f45fb61d265fcdbe7241e8655f835a18e41fb6bd1` |
| `vendor/out/xros/libGLESv2.framework/libGLESv2` | `4dad972e8ec6e034b2af8b9446abc0c73a3e1f6392faf3be77989439a1c6d483` |
| `vendor/out/xrsim/libEGL.framework/libEGL` | `4f0d9a68d33b0d93007db145821e91a5fe0beefbcb5d57f1929155b5da09fdb0` |
| `vendor/out/xrsim/libGLESv2.framework/libGLESv2` | `ca755f46b36d226325d0c9f602335dc3bab9aaadfbd35ec42287044638f5748f` |

## Vivecraft

- Upstream: https://github.com/Vivecraft/VivecraftMod branch `openxr` at `13eafae1`
- Patches: `work/vivecraft-openxr-26.2/patches/` 0001–0005
- Result: `8297f0ecc84bb25aecf714c23dae661cf6cdf626`
- Jar: `vivecraft-26.2-1.3.15-fabric.jar`, sha256 `e241a4c2888bd7158e5177916b6f524b862801c192a08f7d967fc3f21ee5be7d`
- License: LGPLv3 (`LICENSES/LGPL-3.0.txt`)

## LTW

- https://github.com/PojavLauncherTeam/ltw at `7703f1c29a1fd7f151484c38fb7563bb79ca8ccb`
- Darwin port in `work/natives/ltw/`. License: LGPL-3.0.

## GraalVM

- Oracle GraalVM JDK 25.0.4+7.1
- URL: https://download.oracle.com/graalvm/25/latest/graalvm-jdk-25_macos-aarch64_bin.tar.gz
- Tarball: 353,673,229 bytes, sha256 `0b79e23c133facbad2f7aa55a3b76d17bd59d2fa15e2735bb63391ace223fd13` (recorded when it was fetched; the tarball was not on disk for this snapshot, so this hash was not recomputed)
- Installed `Contents/Home/release`: `JAVA_VERSION=25.0.4`, `JAVA_RUNTIME_VERSION=25.0.4+7-LTS-jvmci-b01`, `GRAALVM_VERSION=25.0.4`
- Terms: https://www.oracle.com/downloads/licenses/graal-free-license.html
- JDK 21.0.12 (sha256 `a98948c3a1ad037fe2dde5c80b06b214a5f80c120d8fc2453d155508cf677beb`) is only for a `MCV=1.*` bake. This image uses JDK 25.

## Natives

| What | Pin |
| --- | --- |
| LWJGL 3.4.1 | commit `b800ccffab14396fc529ddb6c931b7c5c5226763` |
| FreeType 2.14.1 | commit `526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b`, tag archive sha256 `44bd69d1f0750603410cfd4f26f1c5523c5a3a087a2dfa8639b757dc7c6677be` |
| libffi 3.5.2 | sha256 `f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc` |
| OpenXR-SDK 1.1.49 | sha256 `74e9260a1876b0540171571a09bad853302ec68a911200321be8b0591ca94111` |
| Phonon SOFA (`runtime/data/phonon_hrtf_cipic_124.sofa`) | sha256 `c28ff4a874ac889ec0c5885ca524762a70d56984232ff7aadcd9c15d32e1cfb6` (Apache-2.0, Valve steam-audio) |

SVC codec pins are in `work/natives/svc/PROVENANCE.md`.

## Native image reused for the proof build

These dylibs were built in the private tree and reused as inputs. They are not in git.

| File | sha256 |
| --- | --- |
| `libminecraft.dylib` (xros) | `9473c5f6db18ee14115bf43fe31a68cd8590034ded76eb5795e34978c30664c9` |
| `libminecraft.dylib` (xrsimulator) | `19f17ce381c0b5770f26238a9f824f2738811da0b1f8504cc1bedbcca340e2f2` |

Image id string on the xros dylib: `klmc t8a 2026-10-14T16:51Z xros ni=3e91b06a7df9`.
