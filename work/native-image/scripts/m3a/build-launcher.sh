#!/bin/bash
# build-launcher.sh: compile $I/baked/launcher (BakedLauncher + BakedCodeSource + SpikeFabric harness + the Fabric-era
# substitutions in scripts/subst-fabric-src) against this instance's baked classpath. MCV: see scripts/inst.sh.
# (For 1.21.5 the existing baked/launcher was compiled by hand during the spike; this script reproduces it per instance.)
set -e
. "$(dirname "$0")/../inst.sh"
S=$W/scripts
MCV=$MCV $S/mkcp-baked.sh
rm -rf $I/baked/launcher; mkdir -p $I/baked/launcher
# Per-version overrides (26.x = Mojang names): scripts/{subst-fabric-src,spikef-src}-$MCV/X.java replaces the shared X.java.
pick() { local d=$1 f; for f in $S/$d/*.java; do [ -f $S/$d-$MCV/$(basename $f) ] && echo $S/$d-$MCV/$(basename $f) || echo $f; done; }
$G/bin/javac -nowarn -d $I/baked/launcher -cp "$(cat $I/baked.cp)" \
  $S/baked-src/net/fabricmc/loader/impl/launch/BakedLauncher.java \
  $S/baked-src/klepton/KleptonTeardown.java \
  $(pick subst-fabric-src) $(pick spikef-src)
find $I/baked/launcher -name '*.class' | sed "s#$I/baked/launcher/##"
