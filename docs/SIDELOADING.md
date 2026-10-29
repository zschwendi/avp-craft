# Installing a downloaded IPA

An **IPA is an Apple mobile app package**. Downloading it and signing it are
separate steps: a sideloader signs the package for your Apple account and device,
then installs it. You do not need to compile the source when a suitable prebuilt
IPA is available.

**AVPcraft has not published a Vision Pro or iPad IPA yet.** This page explains
the available installation routes and the work remaining before a download can
replace our [source-build instructions](../AVP_INSTALL.md).

## Do players need a Mac?

Not necessarily. Building AVPcraft currently needs an Apple silicon Mac, but
installing a prebuilt package can use other computers:

| Route | Initial setup | After setup |
| --- | --- | --- |
| **Vision Pro: Apparition** | Its desktop installer supports Mac, Windows and Linux. Pair the headset over Wi-Fi using the code in Settings → General → Remote Devices. | Install a compatible IPA or add a source in Apparition. Its companion or LocalDevVPN provides the connection used for renewal. |
| **iPad: SideStore** | A supported Windows, Mac or Linux computer, a USB cable, an Apple account and device trust. | Install and renew apps on the iPad using SideStore and its documented Wi-Fi/VPN setup. |
| **TestFlight, if a developer offers it** | Install TestFlight from the device's App Store and accept the beta invitation. Players do not need a computer. | TestFlight handles updates; each beta build expires after up to 90 days. |

These are the tools' documented capabilities, **not completed AVPcraft install
tests**. Follow their current guides: [Apparition](https://apparition.illixion.com/),
[SideStore prerequisites](https://docs.sidestore.io/docs/installation/prerequisites),
[SideStore installation](https://docs.sidestore.io/docs/installation/install),
and [TestFlight](https://testflight.apple.com/).

AVPcraft itself does **not** need a sideloader's JIT/debugger option: the game
code is compiled before installation. On Windows, Apparition also needs the
Apple sign-in helper described in its setup documentation.

Vision Pro uses a **visionOS** package; iPad uses an **iPadOS/iOS** package. They
are not interchangeable. The current AVPcraft source-build guide and prepared
app are for Vision Pro. An iPad release needs its own build and verification.

## Accounts, signing and renewal

Two accounts serve different purposes:

- **Apple account:** signs and installs the app through the sideloader.
- **Microsoft account with Minecraft: Java Edition:** signs into the game and
  verifies ownership. Owning Java does not provide an Apple signing certificate.

A free Apple account can be used for development signing, with limits including
three installed development apps per device and profiles that expire after seven
days. Sideloaders renew the signature before it expires. This is not a weekly
Minecraft purchase or a requirement to rebuild the game. See
[Apple's Personal Team limits](https://developer.apple.com/help/account/basics/about-your-developer-account).

AVPcraft currently requests **Increased Memory Limit** and **Extended Virtual
Addressing**. The sideloader and provisioning profile need to preserve the
capabilities the game actually requires. Apparition documents Increased Memory
Limit for free accounts, but Extended Virtual Addressing for paid accounts.
**AVPcraft gameplay with a free-account signature has not been verified.**
See [Apparition's signing notes](https://github.com/illixion/Apparition#what-it-does).

A developer's existing personal build is not automatically installable on
everyone's devices. Development/ad hoc profiles restrict which devices may run
the signed app; see [Apple's distribution guide](https://developer.apple.com/documentation/xcode/distributing-your-app-to-registered-devices).
Do not change signing identity or delete an existing installation without first
backing up its worlds. Use the same identity when renewing/updating it.

## What remains before an AVPcraft download

The existing app can technically be packaged as an IPA. Publication has separate
requirements:

1. **Resolve redistribution of the compiled client.** The current app embeds
   Minecraft game code, client jars and assets. Mojang's published
   [EULA](https://www.minecraft.net/en-us/eula) restricts redistribution of its
   game and modified clients. A sign-in/ownership check does not itself provide
   permission to redistribute them. Removing only the assets would still leave
   the compiled game code. Component licenses also need to be satisfied; see
   [THIRD-PARTY.md](../THIRD-PARTY.md).
2. **Prepare a public package.** Use the matching published source, retain
   notices, remove personal provisioning data and verify that account data and
   saves are absent. Label the exact build and known limitations. An older
   preview must not claim the new lower-HUD native-client change.
3. **Verify installation through the chosen sideloader.** Check signing,
   required memory capabilities, first launch from an empty container, ownership
   sign-in, gameplay and an in-place update that preserves saves.
4. **Publish the package, checksum and tested instructions together.** An
   optional app-source feed can make later updates easier to find.

TestFlight would be a simpler player experience, but external testing requires
[Apple's beta review](https://developer.apple.com/tutorials/develop-in-swift/test-your-beta-app).
It does not resolve the game redistribution question, and AVPcraft has no
TestFlight invitation at present.

Checked against the linked documentation on **October 28, 2026**. No public IPA
or end-to-end AVPcraft sideloading compatibility is claimed by this guide.
