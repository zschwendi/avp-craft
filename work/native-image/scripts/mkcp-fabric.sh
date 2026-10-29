#!/bin/bash
# Fabric instance on macOS: libs + macOS LWJGL -> $I/fabric.cp, gamedir $I/fgamedir   (MCV: see scripts/inst.sh)
#   1.21.5: libs/mods/config/saves from the Klepton userdata instance (original behaviour).
#   Other versions: libs + mods from $I/files; config copied from the 1.21.5 fgamedir (mod configs are version-
#   independent JSON/TOML, unknown keys ignored); options.txt from the 1.21.5 fgamedir; saves/world copied from the
#   1.21.5 fgamedir (one-way DFU upgrade of the COPY only).
set -e
. "$(dirname "$0")/inst.sh"
mkdir -p $I/fabric/lib $I/fgamedir/saves
if [ "$MCV" = 1.21.5 ]; then
  V="$HOME/Library/Application Support/Klepton/userdata/vivecraft/files"
  grep '^libraries/net/fabricmc/' "$V/versions/1.21.5/classpath.txt" | while read l; do cp -n "$V/$l" $I/fabric/lib/ || true; done
  [ -d $I/fgamedir/mods ] || cp -R "$V/instances/1.21.5/mods" $I/fgamedir/mods
  [ -d $I/fgamedir/config ] || cp -R "$V/instances/1.21.5/config" $I/fgamedir/config
  [ -d $I/fgamedir/saves/world ] || cp -R "$V/instances/1.21.5/saves/world" $I/fgamedir/saves/world
  cp -n $W/gamedir/options.txt $I/fgamedir/options.txt || true
else
  V=$I/files
  grep '^libraries/net/fabricmc/' $V/versions/$MCV/libs-fabric.txt | while read l; do cp -n "$V/$l" $I/fabric/lib/ || true; done
  # fabric's asm 9.x / mixin: the fabric profile's ow2 + spongepowered libs (vanilla has none on 1.21.11)
  grep -e '^libraries/org/ow2/' -e '^libraries/net/fabricmc/sponge-mixin' $V/versions/$MCV/libs-fabric.txt | while read l; do cp -n "$V/$l" $I/fabric/lib/ || true; done
  # config/options/world source: 1.21.x <- the 1.21.5 fgamedir; 26.x <- the 1.21.11 instance (its voicechat config already
  # has macos_check_microphone_permission=false; its world copy is already DFU'd to 1.21.11 -> one more one-way hop).
  SRCG=$W/fgamedir; case $MCV in 1.*) ;; *) [ -d $W/inst-1.21.11/fgamedir ] && SRCG=$W/inst-1.21.11/fgamedir;; esac
  [ -d $I/fgamedir/mods ] || cp -R $V/mods $I/fgamedir/mods
  [ -d $I/fgamedir/config ] || cp -R $SRCG/config $I/fgamedir/config
  [ -d $I/fgamedir/saves/world ] || cp -R $SRCG/saves/world $I/fgamedir/saves/world
  cp -n $SRCG/options.txt $I/fgamedir/options.txt || true
fi
# The fabric-loader jar is SIGNED; BakedLauncher lives in its package (net.fabricmc.loader.impl.launch) -> strip the
# signature (the 1.21.5 and 1.21.11 copies were stripped by hand; SecurityException "signer information does not match").
for j in $I/fabric/lib/fabric-loader-*.jar; do
  zip -q -d $j 'META-INF/*.SF' 'META-INF/*.RSA' 'META-INF/*.EC' >/dev/null 2>&1 || true; done
{ echo -n $I/vanilla/client-$MCV.jar; for j in $I/vanilla/lib/*.jar $I/fabric/lib/*.jar; do echo -n ":$j"; done; } > $I/fabric.cp
