# AVPcraft: downloads and sideloading status

**There is no public AVPcraft IPA yet.** The older `v0.1.0` `.mrpack` release
belongs to the previous Mac project and cannot install AVPcraft on Vision Pro.
The currently supported route remains [building from source](../BUILDING.md)
and [installing with your own signing team](../AVP_INSTALL.md).

## Why this port is different

AVPcraft's build compiles Minecraft and its mods into `libminecraft.dylib` and
bundles prepared client libraries and assets. The app is not a small launcher
that downloads the normal game after installation. Removing an assets folder
would still leave Minecraft code in that compiled library; removing the library
would leave an incomplete app.

The [Minecraft EULA](https://www.minecraft.net/en-us/eula) distinguishes
original mods from modified game clients and restricts distributing the latter.
The Microsoft ownership check confirms that a player owns Java Edition; it
does not establish permission to redistribute the compiled client. A public
IPA needs a distribution approach that addresses this, rather than a renamed
or incomplete copy of the existing development app.

## Does sideloading itself require a Mac?

No. [Apparition](https://apparition.illixion.com/) documents Vision Pro IPA
installation from Windows, Linux and macOS. It pairs wirelessly and signs with
the recipient's Apple account. That solves signing/installing an existing
compatible IPA; it does not build AVPcraft's native Minecraft client.

AVPcraft also requires memory capabilities, including **Extended Virtual
Addressing**. Apparition's [signing documentation](https://github.com/illixion/Apparition)
states that this capability requires a paid developer account. A free-account
AVPcraft installation and gameplay have not been validated. Do not remove the
memory entitlements to make an installation error disappear.

## Export your own build for private installation

After completing the full build and setting your own signing variables in
[AVP_INSTALL.md](../AVP_INSTALL.md), the existing script can build a local IPA
without installing or launching on a device:

```sh
cd visionos
KL_NO_INSTALL=1 KL_BUNDLE_PAYLOAD=1 ./run.sh device
```

This still runs on the build Mac and requires the prepared native inputs.
The script prints the resulting app and IPA paths. Keep that private package,
its signing material, accounts and worlds out of public releases. Re-signing
must preserve the memory capabilities and use a profile for the intended
headset. This export path has not been validated with a fresh third-party
signing account.

## Updates and troubleshooting

Keep your existing signing team and bundle identifier to preserve app data.
Back up worlds before updates; do not uninstall to fix a signing error. A
missing native client is a build/package issue, while a rejected capability is
a signing issue. Include the build number and exact redacted error in a bug
report, never account tokens, profiles or private game files.

This guide records what is currently available. It is not a promise that
Windows-only users can install and play AVPcraft today.
