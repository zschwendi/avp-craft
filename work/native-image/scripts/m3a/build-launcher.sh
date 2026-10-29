#!/bin/bash
# build-launcher.sh: compile $I/baked/launcher (BakedLauncher + BakedCodeSource + SpikeFabric harness + the Fabric-era
# substitutions in scripts/subst-fabric-src) against this instance's baked classpath. MCV: see scripts/inst.sh.
# (For 1.21.5 the existing baked/launcher was compiled by hand during the spike; this script reproduces it per instance.)
set -e
. "$(dirname "$0")/../inst.sh"
S=$W/scripts
SOURCES=(
  "$S/baked-src/net/fabricmc/loader/impl/launch/BakedLauncher.java"
  "$S/baked-src/klepton/KleptonTeardown.java"
)
# Version-specific files replace shared names AND can introduce their own
# sources. In 26.2 the server substitution exists only in the version directory.
for dir in subst-fabric-src spikef-src; do
  for source in "$S/$dir"/*.java; do
    [ -f "$source" ] || continue
    override="$S/$dir-$MCV/${source##*/}"
    [ ! -f "$override" ] || source="$override"
    SOURCES+=("$source")
  done
  for source in "$S/$dir-$MCV"/*.java; do
    [ -f "$source" ] || continue
    [ -f "$S/$dir/${source##*/}" ] || SOURCES+=("$source")
  done
done
for source in "${SOURCES[@]}"; do
  [ -f "$source" ] || { echo "Missing launcher source: $source" >&2; exit 1; }
done
if [ ! -f "$S/spikef-src/SpikeFabric.java" ] && [ ! -f "$S/spikef-src-$MCV/SpikeFabric.java" ]; then
  echo "Missing SpikeFabric.java; restore the launcher sources before building." >&2
  exit 1
fi
MCV=$MCV "$S/mkcp-baked.sh"
rm -rf "$I/baked/launcher"
mkdir -p "$I/baked/launcher"
"$G/bin/javac" -nowarn -d "$I/baked/launcher" -cp "$(cat "$I/baked.cp")" "${SOURCES[@]}"
find "$I/baked/launcher" -name '*.class' | sed "s#$I/baked/launcher/##"
