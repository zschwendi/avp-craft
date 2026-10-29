#!/bin/bash
# stage_native.sh [--local] [--meta] [<device-udid>] — a NATIVE target's container data
# stage_native.sh --backup-saves [<device-udid>] — pull the container's saves to this Mac first
# (stage_assets.sh hands over to this for `kind: native`).
#
#   Documents/<tree>/launch.txt   visionos/native/<target>-launch.txt (the
#                                 command line kl_app.c expands)
#   Documents/<tree>/baked.cp     the baked classpath, ROOT-relative, one per line
#   Documents/<tree>/root/        a mirror of <nativeroot> (work/native-image)
#                                 holding only what the image reads at runtime:
#                                   every baked.classpath entry that exists
#                                   (jars: mod resources, the client jar;
#                                   bake/classes is skipped — BakedCodeSource
#                                   never reads it), fgamedir/ minus logs and
#                                   screenshots, assets/ = index 24 + its objects
#
# Built once into build/native-stage/<target>/ (clone-copies, no extra disk on
# APFS), then copied to the booted simulator's container or uploaded to a
# device. --meta re-sends only launch.txt + baked.cp (seconds).
# Env: KL_NATIVE_CP=<classpath file> (default <nativeroot>/t4.cp),
#      KL_NATIVE_ASSET_INDEX (default 24).
# MCV=<minecraft version> (M3, 2026-10-12): unset or 1.21.5 = the shipped instance, byte-identical staging (launch.txt
#   is the template verbatim, build/native-stage/<target>/). Anything else = work/native-image/inst-$MCV/ (scripts/
#   inst.sh's instance): baked.cp from inst-$MCV/t4.cp, asset index from inst-$MCV/asset-index-id.txt, the game dir,
#   assets and remapped client jar under Documents/<tree>/root/inst-$MCV/ (so the device's 1.21.5 root/fgamedir and
#   its saves are left alone: devicectl `copy to` merges), launch.txt = the template rewritten for those paths and
#   --version/--assetIndex, staged in build/native-stage/<target>-$MCV/. The matching image is out/t8-$MCV/<sdk>/
#   libminecraft.dylib (mknative.sh reads MCV too); the app and the stage MUST be the same MCV.
#   1.21.6+ also stages fgamedir/input/profiles/Custom_Bindings.json (SVC push-to-talk on the left stick click; QC
#   Vivecraft 1.3.7 has no XRBindings for Target_SvcPttBinding to patch — work/native-image/scripts/m3a/mk-ptt-profile.py).
# --local: build the stage tree and stop (no simulator, no device) — for diffs and dry runs.
# bundle_payload.sh runs --local and copies the tree into the .app when
# KL_BUNDLE_PAYLOAD=1 (the vivecraft device IPA default). This script's
# devicectl / simulator copy is unchanged.
# --backup-saves [<udid>]: copy Documents/<tree>/root/fgamedir/saves (and root/inst-*/fgamedir/saves when present) off
#   the device (or the booted simulator without a udid) into work/native-image/device-saves/<udid>-<timestamp>/.
#   Run it BEFORE the first install of a new Minecraft version (worlds get a one-way DataFixer upgrade on open).
#   Also copies every root/inst-*/profiles (the per-account game dirs, KleptonMCProfiles.swift: worlds + settings).
# PROFILES: Documents/<tree>/root/inst-*/profiles/ belongs to the APP (one game dir per account). Staging never
#   writes it: the device copy merges and the stage tree has no profiles/; the simulator path, which replaces root/,
#   moves every profiles/ dir aside and back.
# MSA: the Mac's Microsoft refresh token (tools/vivecraft_msa_login.py writes
# it) rides along on every stage as Documents/<tree>/msa-refresh.json, an inbox
# the app ADDS to its account list (Library/) at its next boot (KleptonMSA.swift). --msa sends
# ONLY that file (seconds). KL_MSA_SECRETS=<file> overrides the source. The
# contents are never printed.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
eval "$(python3 targets.py "${KLEPTON_TARGET:-}")"
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-$KLT_BUNDLE}"
NR="$(cd "$ROOT/$KLT_NATIVEROOT" && pwd)"
MCV="${MCV:-26.2}"
if [ "$MCV" = 1.21.5 ]; then
  IREL=""
  CPFILE="${KL_NATIVE_CP:-$NR/t4.cp}"
  IDX="${KL_NATIVE_ASSET_INDEX:-24}"
  STAGE="$PWD/build/native-stage/$KLT_NAME"
else
  IREL="inst-$MCV/"
  [ -d "$NR/inst-$MCV" ] || { echo "!! MCV=$MCV: $NR/inst-$MCV missing (work/native-image/scripts/m3a/fetch-inst.py)"; exit 1; }
  CPFILE="${KL_NATIVE_CP:-$NR/inst-$MCV/t4.cp}"
  IDX="${KL_NATIVE_ASSET_INDEX:-$(cat "$NR/inst-$MCV/asset-index-id.txt")}"
  STAGE="$PWD/build/native-stage/$KLT_NAME-$MCV"
fi
LAUNCH="native/$KLT_NAME-launch.txt"
LOCAL_ONLY=0
if [ "${1:-}" = "--local" ]; then LOCAL_ONLY=1; shift; fi
if [ "${1:-}" = "--backup-saves" ]; then
  shift; BTARGET="${1:-}"
  # Every saves dir the app can have: the 1.21.5 game dir and any instance's (inst-<mcv>/fgamedir).
  SRCS="root/fgamedir/saves $(cd "$NR" && ls -d inst-*/ 2>/dev/null | sed 's#/$#/fgamedir/saves#' | sed 's#^#root/#' | tr '\n' ' ' || true)"
  # Per-account game dirs: <instance>/profiles, i.e. root/profiles for the 1.21.5 row (instance = root/fgamedir's parent).
  SRCS="$SRCS root/profiles $(cd "$NR" && ls -d inst-*/ 2>/dev/null | sed 's#/$#/profiles#' | sed 's#^#root/#' | tr '\n' ' ' || true)"
  BK="$NR/device-saves/${BTARGET:-sim}-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$BK"
  got=0
  for rel in $SRCS; do
    d="$BK/$(dirname "$rel")"; mkdir -p "$d"
    if [ -n "$BTARGET" ]; then
      b="$(basename "$rel")"
      if xcrun devicectl device copy from --device "$BTARGET" --domain-type appDataContainer \
           --domain-identifier "$BUNDLE_ID" --source "Documents/$KLT_TREE/$rel" --destination "$d/$b" > /dev/null 2>&1; then
        got=$((got + 1)); echo "[backup] $rel -> $d/$b ($(du -sh "$d/$b" | cut -f1))"
      else echo "[backup] $rel: not on the device (skipped)"; fi
    else
      udid="${KLEPTON_SIM_UDID:-$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)}"
      c=$(xcrun simctl get_app_container "$udid" "$BUNDLE_ID" data 2>/dev/null) || { echo "!! $BUNDLE_ID not installed on '$udid'"; exit 1; }
      if [ -d "$c/Documents/$KLT_TREE/$rel" ]; then
        cp -R "$c/Documents/$KLT_TREE/$rel" "$d/"; got=$((got + 1)); echo "[backup] $rel -> $d/$(basename "$rel")"
      else echo "[backup] $rel: not in the container (skipped)"; fi
    fi
  done
  [ "$got" -gt 0 ] || { echo "!! [backup] nothing copied — check the udid / bundle id ($BUNDLE_ID)"; exit 1; }
  echo "[backup] $got saves dir(s) in $BK"
  exit 0
fi
META_ONLY=0
if [ "${1:-}" = "--meta" ]; then META_ONLY=1; shift; fi
MSA_ONLY=0
if [ "${1:-}" = "--msa" ]; then MSA_ONLY=1; shift; fi
TARGET="${1:-}"
MSA_SRC="${KL_MSA_SECRETS:-$HOME/Library/Application Support/Klepton/userdata/$KLT_TREE/files/msa/secrets.json}"
# stage_msa <sim-dest-dir | ""> — copy the refresh token into the container.
stage_msa() {
  if [ ! -s "$MSA_SRC" ] || ! grep -q '"refresh_token": *"[^"]' "$MSA_SRC"; then
    echo "[stage] msa: no refresh token on this Mac — offline profile"; return 0
  fi
  if [ -n "$1" ]; then
    (umask 077; cp "$MSA_SRC" "$1/msa-refresh.json")
  else
    copy "$MSA_SRC" "Documents/$KLT_TREE/msa-refresh.json" > /dev/null
  fi
  echo "[stage] msa: refresh token staged (Documents/$KLT_TREE/msa-refresh.json -> Library at next boot)"
}
copy() {
  xcrun devicectl device copy to --device "$TARGET" \
    --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
    --source "$1" --destination "$2"
}
sim_dest() {
  local udid container
  udid="${KLEPTON_SIM_UDID:-$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)}"
  [ -n "$udid" ] || { echo "!! no booted simulator" >&2; exit 1; }
  container=$(xcrun simctl get_app_container "$udid" "$BUNDLE_ID" data 2>/dev/null) || {
    echo "!! $BUNDLE_ID is not installed on $udid — install the app first" >&2; exit 1; }
  echo "$container/Documents/$KLT_TREE"
}
if [ "$MSA_ONLY" = 1 ]; then
  if [ -z "$TARGET" ]; then d="$(sim_dest)"; mkdir -p "$d"; stage_msa "$d"; else stage_msa ""; fi
  exit 0
fi
[ -f "$LAUNCH" ] || { echo "!! $LAUNCH missing"; exit 1; }
[ -f "$CPFILE" ] || { echo "!! $CPFILE missing"; exit 1; }

# clone <src> <dst>: dst's PARENT gets src's basename, merged into an existing
# directory of that name (fgamedir/.fabric is both a classpath parent and a
# game-dir child). APFS clone when it can, a plain copy when it cannot.
clone() {
  local parent; parent="$(dirname "$2")"; mkdir -p "$parent"
  [ "$(basename "$1")" = "$(basename "$2")" ] || { echo "!! clone: name mismatch $1 -> $2"; exit 1; }
  cp -Rc "$1" "$parent/" 2>/dev/null || cp -R "$1" "$parent/"
}

mkdir -p "$STAGE"
if [ -z "$IREL" ]; then
  cp "$LAUNCH" "$STAGE/launch.txt"
else
  # The template is 1.21.5's; rewrite its instance paths, version and asset index. The remapped client jar is the
  # one the classpath names (fabric loader version and all), so launch.txt and baked.cp can never disagree.
  # 26.x is unobfuscated: no remapped jar, the game jar is the (signature-stripped) client jar on the classpath and the
  # namespace is "official"; our Vivecraft 26.2 OpenXR jar selects its mobile GLES path with -Dvivecraft.openxr.mobile.
  case $MCV in
    1.*) GAMEJAR=$(tr ':' '\n' < "$CPFILE" | grep '/fgamedir/\.fabric/remappedJars/.*/client-intermediary\.jar$' | sed -n "s#^$NR/##p" | head -1)
         GAMENS=intermediary ;;
    *)   GAMEJAR=$(tr ':' '\n' < "$CPFILE" | grep "/vanilla/client-$MCV\.jar\$" | sed -n "s#^$NR/##p" | head -1)
         GAMENS=official ;;
  esac
  [ -n "$GAMEJAR" ] && [ -f "$NR/$GAMEJAR" ] || { echo "!! MCV=$MCV: no game jar on $CPFILE"; exit 1; }
  awk -v ir="$IREL" -v mcv="$MCV" -v idx="$IDX" -v gj="$GAMEJAR" -v ns="$GAMENS" '
    /^#/ { print; next }
    /^-Dfabric\.gameMappingNamespace=/ { print "-Dfabric.gameMappingNamespace=" ns
      if (ns == "official") { print "-Dvivecraft.openxr.mobile=true"; print "-Dorg.lwjgl.util.NoHashChecks=true" }
      prev = $0; next }
    prev == "--version"    { print mcv; prev = $0; next }
    prev == "--assetIndex" { print idx; prev = $0; next }
    /^-Dfabric\.gameJarPath=/ { print "-Dfabric.gameJarPath=@ROOT@/" gj; prev = $0; next }
    { line = $0
      if (line ~ /@ROOT@\/fgamedir$/) sub(/@ROOT@\/fgamedir$/, "@ROOT@/" ir "fgamedir", line)
      if (line == "@ROOT@/assets") line = "@ROOT@/" ir "assets"
      print line; prev = $0 }' "$LAUNCH" > "$STAGE/launch.txt"
  # No 1.21.5 path, version or index may survive outside the comments.
  if grep -v '^#' "$STAGE/launch.txt" | grep -nE '1\.21\.5|^24$|@ROOT@/(fgamedir|assets)' ; then
    echo "!! MCV=$MCV: launch.txt still carries 1.21.5 lines (above)"; exit 1; fi
  echo "[stage] launch.txt for $MCV: --version $MCV --assetIndex $IDX, game dir @ROOT@/${IREL}fgamedir"
fi
tr ':' '\n' < "$CPFILE" | sed -n "s#^$NR/##p" > "$STAGE/baked.cp"
outside=$(tr ':' '\n' < "$CPFILE" | grep -vc "^$NR/" || true)
[ "$outside" = 0 ] || echo "!! [stage] $outside classpath entries are outside $NR — not staged"

if [ "$META_ONLY" = 0 ]; then
  rm -rf "$STAGE/root"; mkdir -p "$STAGE/root"
  n=0; skipped=0
  while IFS= read -r rel; do
    case "$rel" in bake/classes|*/bake/classes) skipped=$((skipped + 1)); continue ;; esac
    [ -e "$NR/$rel" ] || { echo "!! [stage] classpath entry missing on this Mac: $rel"; continue; }
    clone "$NR/$rel" "$STAGE/root/$rel"; n=$((n + 1))
  done < "$STAGE/baked.cp"
  echo "[stage] classpath: $n entries staged, $skipped skipped (bake/classes)"
  # fgamedir minus what a run writes for itself
  (cd "$NR/${IREL}fgamedir" && find . -mindepth 1 -maxdepth 1 \
      ! -name logs ! -name crash-reports ! -name screenshots ! -name downloads \
      ! -name fabricloader.log -print0) | while IFS= read -r -d '' e; do
    clone "$NR/${IREL}fgamedir/$e" "$STAGE/root/${IREL}fgamedir/$e"
  done
  # Vivecraft hands-only preset (vivecraft/notes/hands-map.md). Austin has no
  # controllers; kl_openxr's KL_XR_HANDS_TOUCH makes a Touch controller out of
  # pinches, and these settings fit that controller: free-move follows the
  # head (the hand-drag stick is head-relative; a tracked hand's aim is noisy).
  # Teleport (left ring pinch), snap turn, physical jump/crouch and the touch
  # hotbar are Vivecraft defaults and stay. Written into the STAGED copy only
  # (the clone is replaced by a real file), so work/native-image is untouched.
  # physicalKeyboard=false: Vivecraft's default keyboard is the "physical"
  # one, typed by poking keys with the controller/hand POSITION (KeyboardHandler
  # .processGui -> PhysicalKeyboard.findTouchedKey); the laser pointer never
  # hits it. The pointer keyboard (GuiKeyboard.processCursor + keyboardclick)
  # is what pointer-and-trigger play expects (launch 1, 10-12: Austin could
  # not type a server address with the Sense controllers).
  # disableGarbageCollectorMessage=true: Vivecraft's GarbageCollectorScreen ("use ZGC", Don't Show Again / OK / Open
  # Guide) opens on the first world join because the image runs SerialGC (BySpaceAndTime is deliberate, launch 4); it
  # is a pause screen, so the integrated server paused before sending chunks (launch 8 sim: black void + HUD).
  # KL_VIVECRAFT_HANDS_PRESET=0 stages the config exactly as it is.
  CFG="$STAGE/root/${IREL}fgamedir/config/vivecraft-client-config.json"
  if [ "$KLT_NAME" = vivecraft ] && [ "${KL_VIVECRAFT_HANDS_PRESET:-1}" != 0 ] && [ -f "$CFG" ]; then
    python3 -I - "$CFG" <<'PY2'
import json, sys
p = sys.argv[1]
cfg = json.load(open(p))
preset = {"vrFreeMoveMode": "HMD", "physicalKeyboard": "false", "disableGarbageCollectorMessage": "true"}
changed = {k: v for k, v in preset.items() if cfg.get(k) != v}
cfg.update(preset)
tmp = p + ".tmp"
json.dump(cfg, open(tmp, "w"), indent=2)
import os; os.replace(tmp, p)
print("[stage] vivecraft hands preset: " + (", ".join(f"{k}={v}" for k, v in changed.items()) or "already set"))
PY2
  fi
  # Simple Voice Chat presets. The microphone backend is NOT a config choice here: SVC 2.6.11 rewrites
  # java_microphone_implementation=true at every boot on macOS (os.name is "Mac OS X" in this image), so the
  # image's Target_SvcMicrophone substitution forces OpenAL capture; the line below is cosmetic. Kept:
  # macos_check_microphone_permission=false (that check is JNA into AVFoundation, which this image does not
  # carry; KleptonAudio.swift asks for record permission). Push-to-talk: options.txt binds key.push_to_talk to
  # F24 (a key nothing else uses) so the left-thumbstick-click VR action (Target_SvcPttBinding) reaches SVC's
  # raw-key PTT handler through Vivecraft's InputSimulator; an unbound PTT key would only set the KeyMapping,
  # which SVC never reads. KL_VIVECRAFT_SVC_PRESET=0 stages both files exactly as they are.
  SVC="$STAGE/root/${IREL}fgamedir/config/voicechat/voicechat-client.properties"
  OPT="$STAGE/root/${IREL}fgamedir/options.txt"
  if [ "$KLT_NAME" = vivecraft ] && [ "${KL_VIVECRAFT_SVC_PRESET:-1}" != 0 ] && [ -f "$SVC" ]; then
    rm -f "$SVC.tmp"
    sed -e 's/^java_microphone_implementation=.*/java_microphone_implementation=false/' \
        -e 's/^macos_check_microphone_permission=.*/macos_check_microphone_permission=false/' "$SVC" > "$SVC.tmp"
    mv "$SVC.tmp" "$SVC"
    echo "[stage] voicechat preset: $(grep -E '^(java_microphone_implementation|macos_check_microphone_permission)=' "$SVC" | tr '\n' ' ')"
  fi
  if [ "$KLT_NAME" = vivecraft ] && [ "${KL_VIVECRAFT_SVC_PRESET:-1}" != 0 ] && [ -f "$OPT" ]; then
    rm -f "$OPT.tmp"
    if grep -q '^key_key.push_to_talk:' "$OPT"; then
      sed -e 's/^key_key.push_to_talk:.*/key_key.push_to_talk:key.keyboard.f24/' "$OPT" > "$OPT.tmp"
    else
      { cat "$OPT"; echo 'key_key.push_to_talk:key.keyboard.f24'; } > "$OPT.tmp"
    fi
    mv "$OPT.tmp" "$OPT"
    echo "[stage] voicechat PTT: $(grep '^key_key.push_to_talk:' "$OPT") (VR: left thumbstick click)"
  fi
  # 1.21.6+ (QC Vivecraft 1.3.7): the left-stick-click binding is data, not a substitution. Vivecraft loads
  # input/profiles/<currentBindingProfile>.json from the cwd (= the game dir) INSTEAD of its defaults when it exists;
  # mk-ptt-profile.py writes the Touch defaults + mod/key.push_to_talk -> /user/hand/left/input/thumbstick/click as
  # "Custom Bindings" (the setting's default value, enforced below). Replaces an in-game edited profile on the device.
  # 26.x: NOT staged — our Vivecraft 26.2 OpenXR jar has PTT in its built-in Touch map (patch 0003) and no
  # BindingProfile JSON loader; a stale QC profile would be ignored at best.
  if [ -n "$IREL" ] && [ "${MCV#1.}" != "$MCV" ] && [ "$KLT_NAME" = vivecraft ] && [ "${KL_VIVECRAFT_SVC_PRESET:-1}" != 0 ]; then
    GD="$STAGE/root/${IREL}fgamedir"
    mkdir -p "$GD/input/profiles"
    python3 -I "$NR/scripts/m3a/mk-ptt-profile.py" "$GD/mods/Vivecraft.jar" "$GD/input/profiles/Custom_Bindings.json.tmp"
    mv "$GD/input/profiles/Custom_Bindings.json.tmp" "$GD/input/profiles/Custom_Bindings.json"
    if [ -f "$CFG" ]; then
      python3 -I - "$CFG" <<'PY3'
import json, os, sys
p = sys.argv[1]; cfg = json.load(open(p))
if cfg.get("currentBindingProfile", "Custom Bindings") != "Custom Bindings":
    print(f"[stage] vivecraft currentBindingProfile {cfg['currentBindingProfile']!r} -> 'Custom Bindings'")
    cfg["currentBindingProfile"] = "Custom Bindings"
    json.dump(cfg, open(p + ".tmp", "w"), indent=2); os.replace(p + ".tmp", p)
PY3
    fi
    echo "[stage] voicechat PTT profile: ${IREL}fgamedir/input/profiles/Custom_Bindings.json"
  fi
  # 26.x: pin the OpenGL backend (options.txt preferredGraphicsBackend; "default" would also probe Vulkan, and Vulkan is
  # the fallback after GL in both). Values: default|opengl|vulkan (net.minecraft.client.PreferredGraphicsApi).
  if [ -n "$IREL" ] && [ "${MCV#1.}" = "$MCV" ] && [ -f "$OPT" ]; then
    python3 -I "$NR/scripts/m3a/pin-gl-options.py" "$OPT"
  fi
  # Minecraft's assets: index $IDX and exactly the objects it names.
  mkdir -p "$STAGE/root/${IREL}assets/indexes" "$STAGE/root/${IREL}assets/objects"
  cp "$NR/${IREL}assets/indexes/$IDX.json" "$STAGE/root/${IREL}assets/indexes/"
  python3 -I - "$NR/${IREL}assets" "$STAGE/root/${IREL}assets" "$IDX" <<'PY'
import json, os, shutil, subprocess, sys
src, dst, idx = sys.argv[1], sys.argv[2], sys.argv[3]
objs = json.load(open(f"{src}/indexes/{idx}.json"))["objects"]
missing = 0; size = 0
for o in objs.values():
    h = o["hash"]; rel = f"objects/{h[:2]}/{h}"
    s = os.path.join(src, rel); d = os.path.join(dst, rel)
    if not os.path.exists(s): missing += 1; continue
    os.makedirs(os.path.dirname(d), exist_ok=True)
    if subprocess.run(["cp", "-c", s, d], capture_output=True).returncode: shutil.copy(s, d)
    size += o["size"]
print(f"[stage] assets: index {idx}, {len(objs) - missing}/{len(objs)} objects, {size/1e6:.0f} MB"
      + (f", {missing} MISSING on this Mac" if missing else ""))
PY
fi
echo "[stage] $KLT_NAME tree: $(du -sh "$STAGE" | cut -f1) in $STAGE"
[ "$LOCAL_ONLY" = 0 ] || { echo "[stage] --local: not copied anywhere"; exit 0; }

if [ -z "$TARGET" ]; then
  UDID="${KLEPTON_SIM_UDID:-$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)}"
  [ -n "$UDID" ] || { echo "!! no booted simulator"; exit 1; }
  CONTAINER=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data 2>/dev/null) || {
    echo "!! $BUNDLE_ID is not installed on $UDID — install the app first"; exit 1; }
  DEST="$CONTAINER/Documents/$KLT_TREE"
  mkdir -p "$DEST"
  cp "$STAGE/launch.txt" "$STAGE/baked.cp" "$DEST/"
  stage_msa "$DEST"
  if [ "$META_ONLY" = 0 ]; then
    # The per-account game dirs (the app's own) survive the root/ replacement: root/inst-*/profiles, and
    # root/profiles for the 1.21.5 row (its instance is root/ itself; kept as "_root_").
    KEEP="$DEST/.profiles-keep"; rm -rf "$KEEP"; mkdir -p "$KEEP"
    if [ -d "$DEST/root/profiles" ]; then
      mv "$DEST/root/profiles" "$KEEP/_root_"; echo "[stage] keeping root/profiles"
    fi
    for p in "$DEST"/root/inst-*/profiles; do
      [ -d "$p" ] || continue
      i="$(basename "$(dirname "$p")")"; mv "$p" "$KEEP/$i"; echo "[stage] keeping root/$i/profiles"
    done
    rm -rf "$DEST/root"
    clone "$STAGE/root" "$DEST/root"
    for k in "$KEEP"/*; do
      [ -d "$k" ] || continue
      i="$(basename "$k")"
      if [ "$i" = "_root_" ]; then mv "$k" "$DEST/root/profiles"; continue; fi
      mkdir -p "$DEST/root/$i"; mv "$k" "$DEST/root/$i/profiles"
    done
    rmdir "$KEEP"
  fi
  echo "[stage] simulator $UDID -> $DEST ($(du -sh "$DEST" | cut -f1))"
  exit 0
fi

copy "$STAGE/launch.txt" "Documents/$KLT_TREE/launch.txt" > /dev/null
copy "$STAGE/baked.cp" "Documents/$KLT_TREE/baked.cp" > /dev/null
stage_msa ""
if [ "$META_ONLY" = 0 ]; then
  echo "[stage] device $TARGET: uploading $(du -sh "$STAGE/root" | cut -f1) (one directory copy)"
  copy "$STAGE/root" "Documents/$KLT_TREE/root"
fi
echo "[stage] device $TARGET -> Documents/$KLT_TREE"
