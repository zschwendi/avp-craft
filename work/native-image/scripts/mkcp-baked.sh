#!/bin/bash
# baked Fabric classpath: launcher + bake overlay + intermediary game jar + mods (resources) + libs -> $I/baked.cp
# MCV=<version> picks the instance (scripts/inst.sh); baked/launcher is per instance ($I/baked/launcher).
. "$(dirname "$0")/inst.sh"
F=$I/fgamedir
{ echo -n "$I/baked/launcher:$I/bake/classes:$REMAPPED"
  for j in $F/mods/*.jar $F/.fabric/processedMods/*.jar $I/vanilla/lib/*.jar $I/fabric/lib/*.jar; do echo -n ":$j"; done; } > $I/baked.cp
