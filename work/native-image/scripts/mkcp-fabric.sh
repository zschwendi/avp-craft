#!/bin/bash
# Fabric instance on macOS: libs + macOS LWJGL -> $I/fabric.cp, gamedir $I/fgamedir   (MCV: see scripts/inst.sh)
#   1.21.5: libs/mods/config/saves from the Klepton userdata instance (original behaviour).
#   Other versions: libs + mods from $I/files; config copied from the 1.21.5 fgamedir (mod configs are version-
#   independent JSON/TOML, unknown keys ignored); options.txt from the 1.21.5 fgamedir; saves/world copied from the
#   1.21.5 fgamedir (one-way DFU upgrade of the COPY only).
#   Libraries and mods are required. The prior config/options/world copies are optional: skipped when the source is
#   missing or a top-level symlink, and never written over or merged into anything already at the destination.
set -e
. "$(dirname "$0")/inst.sh"
mkdir -p "$I/fabric/lib" "$I/fgamedir/saves"
# copy_lib <src>: required library; an existing destination is kept, any copy failure aborts.
copy_lib() {
  local d="$I/fabric/lib/${1##*/}"
  [ -e "$d" ] || [ -L "$d" ] || cp "$1" "$d"
}
# copy_prior <src> <dst>: optional migration of a previous instance's file/dir.
copy_prior() {
  if [ -L "$1" ]; then echo "mkcp-fabric: skip symlink source $1" >&2; return 0; fi
  if [ ! -e "$1" ]; then echo "mkcp-fabric: no prior $1" >&2; return 0; fi
  if [ -e "$2" ] || [ -L "$2" ]; then return 0; fi
  cp -R "$1" "$2"
}
# copy_mods <src>: required; an existing mods directory is kept.
copy_mods() {
  local d="$I/fgamedir/mods"
  [ -d "$d" ] && return 0
  if [ -e "$d" ] || [ -L "$d" ]; then echo "mkcp-fabric: $d exists and is not a directory" >&2; return 1; fi
  cp -R "$1" "$d"
}
if [ "$MCV" = 1.21.5 ]; then
  V="$HOME/Library/Application Support/Klepton/userdata/vivecraft/files"
  grep '^libraries/net/fabricmc/' "$V/versions/1.21.5/classpath.txt" | while IFS= read -r l; do copy_lib "$V/$l"; done
  copy_mods "$V/instances/1.21.5/mods"
  copy_prior "$V/instances/1.21.5/config" "$I/fgamedir/config"
  copy_prior "$V/instances/1.21.5/saves/world" "$I/fgamedir/saves/world"
  copy_prior "$W/gamedir/options.txt" "$I/fgamedir/options.txt"
else
  V="$I/files"
  grep '^libraries/net/fabricmc/' "$V/versions/$MCV/libs-fabric.txt" | while IFS= read -r l; do copy_lib "$V/$l"; done
  # fabric's asm 9.x / mixin: the fabric profile's ow2 + spongepowered libs (vanilla has none on 1.21.11)
  grep -e '^libraries/org/ow2/' -e '^libraries/net/fabricmc/sponge-mixin' "$V/versions/$MCV/libs-fabric.txt" | while IFS= read -r l; do copy_lib "$V/$l"; done
  # config/options/world source: 1.21.x <- the 1.21.5 fgamedir; 26.x <- the 1.21.11 instance (its voicechat config already
  # has macos_check_microphone_permission=false; its world copy is already DFU'd to 1.21.11 -> one more one-way hop).
  SRCG="$W/fgamedir"; case $MCV in 1.*) ;; *) [ -d "$W/inst-1.21.11/fgamedir" ] && SRCG="$W/inst-1.21.11/fgamedir";; esac
  copy_mods "$V/mods"
  copy_prior "$SRCG/config" "$I/fgamedir/config"
  copy_prior "$SRCG/saves/world" "$I/fgamedir/saves/world"
  copy_prior "$SRCG/options.txt" "$I/fgamedir/options.txt"
fi
# The fabric-loader jar is SIGNED; BakedLauncher lives in its package (net.fabricmc.loader.impl.launch) -> strip the
# signature (the 1.21.5 and 1.21.11 copies were stripped by hand; SecurityException "signer information does not match").
for j in "$I"/fabric/lib/fabric-loader-*.jar; do
  [ -e "$j" ] || continue
  zip -q -d "$j" 'META-INF/*.SF' 'META-INF/*.RSA' 'META-INF/*.EC' >/dev/null 2>&1 || true; done
{ printf '%s' "$I/vanilla/client-$MCV.jar"
  for j in "$I"/vanilla/lib/*.jar "$I"/fabric/lib/*.jar; do [ -e "$j" ] || continue; printf ':%s' "$j"; done; } > "$I/fabric.cp"
