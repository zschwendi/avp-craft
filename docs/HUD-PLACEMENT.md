# Lower in-world HUD

The head-locked health, food and hotbar/item HUD defaults to **15 degrees below its previous position**. Its center and orientation rotate together around the head-local right axis. The HUD remains 2 m away at scale 0.8; world projection and scale stay unchanged. The offset follows physical head orientation, including yaw, pitch and roll.

This is a game-side source change, not a compositor shift. Build **all six Vivecraft patches**, normalize the resulting jar as BUILDING.md describes, then repeat the bake, native-image and visionOS relink steps before building the host. **An older compiled client reused with a new host will not acquire the fix.** Compiled native game binaries are not included in this commit.

## Settings and upgrades

New profiles default to `vrHudLockMode: "HEAD"`, `hudDistance: "2.0"`, `headHudScale: "0.8"`, and `hudPitchOffset: "-15.0"`. These are the actual Java defaults, so they also apply when a config is missing or contains only `vrEnabled`.

Existing profiles migrate only the old exact stock string `"hudPitchOffset": "-2.0"` to `"-15.0"`. The pitch migration uses its own `.avpcraft-hud-pitch-v1` marker, so the earlier distance/scale migration cannot suppress it. The original config is kept as `<config>.avpcraft-hud-pitch-v1.orig`. Custom values and other settings, worlds, options and account data are preserved. A later adjustment survives subsequent launches.

To tune a head-locked HUD, close the game and edit `hudPitchOffset` in the active profile's `config/vivecraft-client-config.json`: negative degrees lower it, zero centers it, positive raises it; the consumer clamps to ±45 degrees. Keep the value as a JSON string, as Vivecraft writes it. Custom wrist/hand placement remains available and is not pitched by this helper.

Inventory, pause/chat screens, room menus, forced-on-HUD screens and seated alternate controller-aim placement retain their original placement.

## Checks and limits

The updated Fabric jar built from the pinned upstream source plus patches 0001–0006. The actual GuiHandler/helper/VRSettings sources compile against the Minecraft 26.2 API. Real JOML/Vec3 geometry checks cover direction, head yaw/pitch/roll, radial distance, quad orientation, exclusions, input ownership, clamping and invalid inputs. Swift migration/file-preservation fixtures pass; its production code compiles for arm64 visionOS with SDK 27 and Swift 6. An independent Opus 5.5 source review found no blocking issue.

The build/check summary is recorded in [HUD-CHECKS.json](HUD-CHECKS.json).

These checks do not establish the appearance of a newly compiled native image on a headset. AOT compilation, visionOS linkage, installation and physical UI review remain required. Other known audio and windowed/VR lifetime issues are not addressed by this change.

After applying the patches, the source contract can run before the expensive client build:

```sh
python3 -I work/vivecraft-openxr-26.2/tests/hud_pitch_source_contract.py \
  work/vivecraft-openxr-26.2/VivecraftMod
```

Host fixture checks:

```sh
swiftc -parse-as-library -D MC_HUD_PITCH_TEST visionos/Sources/MCHUDPitch.swift -o /tmp/avpcraft-hud-pitch
/tmp/avpcraft-hud-pitch
swiftc -parse-as-library -D MC_UI_COMFORT_TEST visionos/Sources/MCUIComfort.swift -o /tmp/avpcraft-hud-comfort
/tmp/avpcraft-hud-comfort
```

After the baked classpath exists, the geometry test uses the actual game value classes:

```sh
CP="$(cat work/native-image/inst-26.2/baked.cp)"
mkdir -p build/hud-check
javac -proc:none -cp "$CP" -d build/hud-check \
  work/vivecraft-openxr-26.2/VivecraftMod/common/src/main/java/org/vivecraft/client_vr/gameplay/screenhandlers/HeadHudPitch.java \
  work/vivecraft-openxr-26.2/tests/HudPitchTransformTest.java
java -Xverify:all -cp "build/hud-check:$CP" HudPitchTransformTest
```

Packaging fixture checks:

```sh
python3 -m unittest discover -s work/vivecraft-openxr-26.2 -p 'test_canonicalize_vivecraft_jar.py' -v
```
