# inst.sh — sourced by the bake/classpath/run scripts (M3 stage A, 2026-10-12). Picks the Minecraft instance.
#   MCV unset            -> 26.2 (this tree). I=$W/inst-26.2
#   MCV=1.21.5           -> I=$W (the old layout: vanilla/ fabric/ fgamedir/ bake/ baked/ *.cp)
#   MCV=<other>          -> I=$W/inst-$MCV (same sub-layout; fetched by scripts/m3a/fetch-inst.py)
# Shared by every instance: graalvm/, dl/lwjgl, lwjgl-fork/ (QC LWJGL 3.3.3 fork + natives), lwjgl-fork-t4/ (klglfw),
# bakeagent/, baked/launcher sources, baked/t23-stubs, out/t23/natives.
# Exports: W G GJDK MCV I AIDX ASSETS FLV REMAPPED (the game jar Knot/BakedLauncher run) GAMENS SUBST_SKIP.
# <repo>/work/native-image/scripts/inst.sh
_INST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
W="$(cd "$_INST_DIR/.." && pwd)"
ROOT="$(cd "$_INST_DIR/../../.." && pwd)"
MCV=${MCV:-26.2}
. $W/scripts/jdk.sh   # G: GraalVM 21.0.12 for 1.21.x, 25.0.4 for 26.x (one resolver for every script)
FLV=${FLV:-0.19.5}
if [ "$MCV" = 1.21.5 ]; then
  I=$W; AIDX=24; ASSETS=$W/assets
else
  I=$W/inst-$MCV; AIDX=$(cat $I/asset-index-id.txt); ASSETS=$I/assets
fi
# Substitution sources (scripts/t4-src/subst) that do not apply to this instance (target class gone):
#   1.21.11: QC Vivecraft 1.3.7 dropped provider.openxr.XRBindings (bindings are JSON BindingProfiles now) -> the PTT
#   binding is data instead: visionos/stage_native.sh (MCV=1.21.11) stages fgamedir/input/profiles/Custom_Bindings.json
#   from scripts/m3a/mk-ptt-profile.py (M3A-1.21.11-HOST-REPORT.md "Stage A follow-ups").
#   Per-version replacements live in scripts/t4-src/subst-$MCV/ (compiled instead of the same-named shared file).
#   26.x: our Vivecraft 26.2 OpenXR jar (work/vivecraft-openxr-26.2) has PTT inside its Touch map (patch 0003) -> no
#   Target_SvcPttBinding and no Custom_Bindings.json; upstream MCOpenXR.destroy is null-safe and has no `swapchain`
#   field -> no Target_T4 (it would not compile).
case $MCV in 1.21.5) SUBST_SKIP="";; 1.*) SUBST_SKIP="Target_SvcPttBinding.java";;
  *) SUBST_SKIP="Target_SvcPttBinding.java
Target_T4.java";; esac
[ -d $W/scripts/t4-src/subst-$MCV ] && for f in $W/scripts/t4-src/subst-$MCV/*.java; do SUBST_SKIP="$SUBST_SKIP
$(basename $f)"; done
# Game jar + its namespace. 1.21.x: Knot's intermediary-remapped jar. 26.x is unobfuscated (Fabric intermediary 26.2 =
# 0.0.0, no mappings on the classpath -> runtime namespace "official" = Mojang names): Knot remaps nothing and the game
# jar is the client jar itself (signature stripped by mkcp.sh so the bake overlay can share its packages).
# GAMENS = the value for -Dfabric.gameMappingNamespace (BakedLauncher / run scripts / native image).
case $MCV in
  1.*) REMAPPED=$I/fgamedir/.fabric/remappedJars/minecraft-$MCV-$FLV/client-intermediary.jar; GAMENS=intermediary;;
  *)   REMAPPED=$I/vanilla/client-$MCV.jar; GAMENS=official;;
esac
