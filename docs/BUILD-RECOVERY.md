# Build 72 source and fresh-checkout repairs

**Follow-up:** the repaired source has now produced a signed Build 73 app from a
fresh public checkout. See [clean-build evidence](CLEAN-BUILD.md) and the
[pinned bake audit](KNOT-BAKE.md). The history below explains the original fixes.

The installed AVPcraft reference is version 0.4.0, build 72. Its saved host source
matches the successful build receipt. This repository now includes its audio
policy, background-audio declaration and Smooth / 45-degree Snap setting. The
newer lower-HUD source remains included and requires rebuilding the game image.
The installed build's existing native game payload did not contain that HUD patch.

## Why the existing app worked but a new build failed

[Issue #1](https://github.com/zschwendi/avp-craft/issues/1) exposed missing build
inputs and assumptions about an existing development checkout. The working app
already contained compiled helpers and a prepared game payload; a source checkout
could not recreate all of them using the published instructions.

- The upstream source snapshot omitted `spikef-src` and `subst-fabric-src`, even
  though its build script required them. The missing helper sources are restored.
- Launcher compilation now includes version-only sources as well as shared files
  and replacements. This includes the 26.2 server substitution.
- OpenAL explicitly disables RTKit so cross-compilation cannot select the host's
  D-Bus / Linux scheduling support.
- Old configuration, options and worlds are optional migration inputs. Missing
  data no longer stops setup; existing destinations are preserved.
- BakeAgent compilation includes ASM through the generated Fabric classpath.
- The bake guide creates a disposable world first and waits for capture completion.
  It no longer uses a timer to terminate the game during setup or saving.
- A small, checksum-verified header fetch supplies the ANGLE and Vulkan headers
  required even when the bundled graphics frameworks are used.

The earlier JAR-hash and commit-versus-tree findings were addressed by the
canonical JAR packaging and explicit tree hash already in [BUILDING.md](../BUILDING.md).
Follow the complete current recipe, including patch 0006; do not disable hash checks.

## Recovered helper provenance

The original helper source files were absent from the pinned upstream Git tree.
Four Java files were reconstructed from the port's own `launcher.jar`, which is
identical in the official
[upstream 1.0.1 release](https://github.com/rebelancap/vivecraft-visionos/releases/tag/1.0.1)
and the preserved AVPcraft builds 71 and 72:

```text
launcher.jar SHA-256:
a616fbc748a9dd017778924b9d106b86b78364ea9d0d9262b4daa55ba0a27004
```

These are launcher and native-image substitution helpers, not Minecraft game
source. The port's existing MIT license and notices remain applicable; Vivecraft's
own patches retain their separate license. The recovered set targets Minecraft
26.2 and does not establish compatibility with historical 1.21.x configurations.

All eight launcher classes compiled against the actual 26.2 dependencies. Their
normalized bytecode matches the reference JAR, retaining instructions, exception
tables, descriptors, annotations and signatures while excluding debug metadata.
Seventeen original-versus-rebuilt behavior comparisons also passed.

## Verification boundary

Build 72 host provenance, combined Swift SDK typechecking, audio/turning/HUD
fixtures, launcher compilation and class equivalence, fresh-instance migration,
and the corrected BakeAgent compile have been checked. See
[BUILD-RECOVERY-CHECKS.json](BUILD-RECOVERY-CHECKS.json) for the recorded results.

At the time of the initial repair audit, a complete fresh bake, native image,
app package and headset run were separate acceptance steps. The later
[clean-build evidence](CLEAN-BUILD.md) records the successful bake, native image,
platform links and signed app; headset gameplay remains unverified. Its exact
49 full-pass failures are classified in the [pinned bake audit](KNOT-BAKE.md).
That classification cannot be applied to another report based on count alone;
compare the exact failures and pinned inputs. Matching historical pruning
counts also does not establish runtime success.
