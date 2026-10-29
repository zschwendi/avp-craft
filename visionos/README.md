# AVPcraft visionOS host

This directory contains the AVPcraft launcher and windowed/immersive host, based on Klepton and Vivecraft for visionOS.

Start with [the app overview](../README.md), [installation guide](../AVP_INSTALL.md), and [full build recipe](../BUILDING.md). Use the `vivecraft` build target and your own signing team and bundle identifier. The Xcode project is generated; it is not committed.

`Sources/` contains the Swift/C host, `targets.py` describes dependency locations, and `gen_xcodeproj.py` generates the project. The compiled Minecraft client, downloaded game assets, generated frameworks and signed app are local build inputs/outputs excluded from Git.

See [SOURCE-SNAPSHOT.json](../SOURCE-SNAPSHOT.json) for the build 71 source provenance. Existing worlds and accounts belong to the installed app container; keep your bundle identifier stable when updating.
