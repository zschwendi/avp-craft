#!/bin/bash
# Stage the guest's data into the app's Documents container.
#
# Assets go here rather than into the .app for one reason: there are 2.2 GB of
# them and they change only when the APK does, while the code changes every few
# minutes. Bundling them would put a multi-minute upload in front of every
# build, which is the difference between the edit/run loop working on device and not.
# Code is the other way round — the guest libraries ride in the bundle, because
# P3 established AMFI's tolerance for a klepton-ld dylib *inside a bundle we
# signed* and established nothing about one pushed into Documents.
#
#   ./stage_assets.sh                 # the booted simulator
#   ./stage_assets.sh <device-udid>   # a physical Vision Pro
#
# Idempotent, but not incremental: it replaces what is there. The APK is
# load-bearing on its own, not just the unpacked tree — getPackageCodePath()
# hands the guest the APK path and it opens it as a zip.
#
# Which guest, and therefore which apk and which asset tree, comes from
# KLEPTON_TARGET (visionos/targets.py). The container layout mirrors the repo's:
# <container>/Documents/<tree>/assets and <container>/Documents/<tree>.apk, which
# is what kl_app_configure builds its paths from.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
eval "$(python3 targets.py "${KLEPTON_TARGET:-}")"
# A native target has no APK: its own staging recipe.
if [ "$KLT_KIND" = native ]; then exec ./stage_native.sh "$@"; fi
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-$KLT_BUNDLE}"
APK="$ROOT/$KLT_APK"
ASSETS="$ROOT/$KLT_ASSETS"
TREE="$KLT_TREE"
# dlc-test-override branch: DLC asset files staged alongside the rest. The
# source is the Quest obb dump; only its regular files go (the `files/` subdir
# is BMBF mod leftovers, and dotfiles are junk). The app enumerates whatever
# lands in Documents/android-files/obb.
#
# WHICH dump is the TARGET's business, not this script's. It used to be a 1.28
# literal here and a `[ "$KLT_NAME" = beatsaber ]` test beside it, which is the
# failure targets.py's own header warns about — the table in a fourth place,
# disagreeing. Beat Saber moving from 1.28 to 1.40 changed the package
# directory under `games/` while the target name stayed the same, so the name
# test would have kept passing and kept staging the wrong game's songs.
DLC_SRC="${KL_DLC_SRC:-${KLT_DLC:+$ROOT/$KLT_DLC}}"
dlc_wanted() { [ -n "$DLC_SRC" ] && [ -d "$DLC_SRC" ]; }

# A /sdcard game tree. Not OBB-shaped: the game data is a directory tree the
# guest reaches under external storage, and kl_guest_extstorage_map points
# /sdcard at the files dir — this container's Documents/android-files on device
# (kl_app.c), the userdata dir on the host. Two targets have one and each
# hardcodes its own directory NAME in the guest: hl2vr's ValveActivity2 says
# "srceng", and Simpsons Hit & Run's libmain says "SimpsonsHitRun" (three
# literals, /storage/emulated/0/SimpsonsHitRun). So the name is a target-table
# field (`gamedest`) beside where the tree lives on THIS machine (`gamedata`) —
# it was the literal "srceng" here until the second one arrived, which is the
# table-in-a-fifth-place failure targets.py's header warns about, and it fails
# SILENTLY: the gigabytes land, under a name the guest never looks up, and the
# game reports missing data.
#
# The guest also WRITES into the tree (hl2/cfg), which the container allows and
# the host's symlink allows too.
GAMEDATA_SRC="${KL_GAMEDATA_SRC:-${KLT_GAMEDATA:+$ROOT/$KLT_GAMEDATA}}"
GAMEDATA_DEST="${KLT_GAMEDEST:-}"
gamedata_wanted() {
  [ -n "$GAMEDATA_SRC" ] && [ -d "$GAMEDATA_SRC" ] && [ -n "$GAMEDATA_DEST" ]
}
# A tree with nowhere to go is a build mistake, not a target without a tree —
# say so rather than staging nothing and letting the game name the symptom.
if [ -n "$GAMEDATA_SRC" ] && [ -d "$GAMEDATA_SRC" ] && [ -z "$GAMEDATA_DEST" ]; then
  echo "!! $KLT_NAME has gamedata but no gamedest in targets.py — nothing staged"
  exit 1
fi

# REPAIRED FILES — a `repaired/` subdirectory of the dump, staged LAST so its
# copies land on top of the dump's own.
#
# Some dumps are repacks that tampered with a file, and the fix is a rebuilt
# copy of that one file. It must NOT overwrite the dump (the original is the
# evidence, and every provenance note in games/PROVENANCE.md depends on it still
# being there), and it must NOT be a hand-push either, or the next full stage
# silently puts the tampered file back. So the repair lives beside the dump, in
# `<dump>/repaired/<exact original filename>`, and this overlay is what makes a
# fresh stage reproduce it.
#
# Batman: Arkham Shadow is the case that grew this — `localization-string-
# tables-english(unitedstates)(en-us)_assets_all.bundle` in the 1.4.0 dump is a
# Russian fan translation written over the en-US column; `repaired/` holds the
# same bundle with English merged back in from a clean 1.0.0 dump by m_Id. See
# `games/Batman - Arkham Shadow …/REPAIR.md`.
#
# `repaired/` is a DIRECTORY, so the `-maxdepth 1 -type f` walks below never
# picked it up as an asset in the first place — the overlay is the only path it
# reaches the device by. Its own `sources/` subdir (the untouched donor files)
# is skipped for the same reason.
REPAIRED_SRC="${KL_REPAIRED_SRC:-${DLC_SRC:+$DLC_SRC/repaired}}"
repaired_wanted() {
  [ "${KL_SKIP_REPAIRED:-0}" != 1 ] && [ -n "$REPAIRED_SRC" ] && [ -d "$REPAIRED_SRC" ] \
    && compgen -G "$REPAIRED_SRC/*" > /dev/null 2>&1
}
# stage_repaired_over <dir> — copy repaired/*.* over an already-populated dir.
# Loud about every file it replaces: a silent overlay is how you end up unable
# to tell which bundle is actually on the device.
stage_repaired_over() {
    repaired_wanted || return 0
    local dest="$1" n=0
    for f in "$REPAIRED_SRC"/*; do
      [ -f "$f" ] || continue
      cp -c "$f" "$dest/" 2>/dev/null || cp "$f" "$dest/"
      echo "[stage] REPAIRED: $(basename "$f") ($(stat -f %z "$f") B) overrides the dump's copy"
      n=$((n + 1))
    done
    [ "$n" = 0 ] || echo "[stage] REPAIRED: $n file(s) from $REPAIRED_SRC"
}

[ -d "$ASSETS" ] || { echo "!! $ASSETS missing"; exit 1; }
[ -f "$APK" ]    || { echo "!! $APK missing"; exit 1; }

# The OBB, for a SPLIT APPLICATION BINARY guest. Beat Saber 1.40 is one: the APK
# is 53 MB of code and the game's data ships beside it in
# main.<versionCode>.<package>.obb, which the guest finds through getObbDirs()
# -> <files>/obb (kl_jni.c, and the version code names the file, so a stale one
# reads as missing game data). 1.28 has no OBB and neither does Steam Link, so
# this is present-or-absent rather than required.
#
# It does NOT live in the repo — it is guest userdata, so it comes from the same
# directory a host run reads it from, and lands in <container>/android-files/obb,
# which is what kl_app.c hands kl_jni_set_files_dir. KL_OBB_DIR points elsewhere;
# KL_SKIP_OBB=1 leaves it alone, which is what you want on the second upload of
# a 1.3 GB file that has not changed.
#
# **The layout is the TARGET's, not a constant.** `KLT_OBB` is the path relative
# to the external-storage root that this guest looks in, and it is not one
# answer: a Unity guest asks Java (Context.getObbDirs() -> <files>/obb) and an
# Unreal guest builds Android's own <external>/Android/obb/<package>/ itself.
# Staging RE4 the Unity way put 8.5 GB somewhere libUE4 never looks, and both
# engines report a missing OBB by carrying on — so it is silent at both ends.
# The same relative path is used for the SOURCE under userdata, so a host run
# and a device run read the file from the same place.
OBB_REL="${KLT_OBB:-obb}"
OBB="${KL_OBB_DIR:-$HOME/Library/Application Support/Klepton/userdata/$KLT_NAME/$OBB_REL}"
# ...and if there is none there, the DLC dump is the OTHER place it can be, for
# a target that has one. Beat Saber 1.40's .obb is not stored apart from its
# songs: the Quest dump is one directory holding the 1.34 GB game data and the
# ~246 per-song bundles as siblings, which is also how they are read back —
# both end up in <files>/obb, one because Unity looks for it by name and the
# rest because kl_ovrplat enumerates the directory. Preferring userdata keeps
# BONELAB and RE4 exactly as they were; falling back to the dump means Beat
# Saber needs no second copy of a gigabyte to be staged from.
if [ "${KL_SKIP_OBB:-0}" != 1 ] && ! compgen -G "$OBB/*.obb" > /dev/null 2>&1 \
   && dlc_wanted && compgen -G "$DLC_SRC/*.obb" > /dev/null 2>&1; then
  OBB="$DLC_SRC"
fi
if [ "${KL_SKIP_OBB:-0}" = 1 ] || ! compgen -G "$OBB/*.obb" > /dev/null 2>&1; then
  # Say WHICH directory was empty. "no OBB" is correct for four of the seven
  # targets and a staging mistake for the other three, and they looked the same.
  [ "${KL_SKIP_OBB:-0}" = 1 ] || echo "[stage] no *.obb in $OBB — staging none"
  OBB=""
fi

# The RETAIL game data, for an engine-port guest. JKXR is one: it is OpenJK, so
# the APK carries the port's own VR pk3s (28 MB, in assets/, which the runtime
# copies into external storage itself) and expects the GAME's assets0.pk3 and up
# — 1.2 GB the user supplies from a copy they own — beside them. They are
# neither in the APK nor an OBB, so nothing above stages them and the engine
# reports them missing the way Quake 3 always has: it refuses to start, without
# naming the directory it looked in.
#
# The source is the host's userdata, which IS the host run's external storage:
# `KL_JKXR_DATA` points a HOST run at an install once, the runtime links the
# pk3s into <userdata>/JKXR/<JK2|JK3>/base, and that directory is what comes
# across. So the knob stays a host-side, once-per-machine thing — pointing it at
# a Mac path on device would name nothing — and this script needs no copy of the
# engine's game-directory table: the host run already made the directory with
# the name the engine reads, and the relative path is preserved verbatim.
#
# Only the `assets*.pk3` set, matching what the runtime links: the port's own
# pk3s are refreshed out of the staged assets on every launch, so uploading them
# too would be 26 MB the guest immediately overwrites.
RETAIL=""
if [ "$KLT_KIND" = jkxr ]; then
  RETAIL="${KL_JKXR_DIR:-$HOME/Library/Application Support/Klepton/userdata/$KLT_NAME/JKXR}"
  if [ "${KL_SKIP_RETAIL:-0}" = 1 ]; then
    RETAIL=""
  elif ! compgen -G "$RETAIL/*/base/assets*.pk3" > /dev/null 2>&1; then
    # Loud, because this is the whole game: an app staged without it installs,
    # launches, and dies inside the engine with a message about assets0.pk3.
    echo "!! [stage] no retail pk3s under $RETAIL"
    echo "!!         Run the target on the host once with KL_JKXR_DATA=<dir holding"
    echo "!!         assets0.pk3 …> to populate it, then stage again."
    RETAIL=""
  fi
fi

# The two files the unpacker left BESIDE assets/, which the guest reads through
# `<assets>/../` and which are not part of the asset tree, so nothing staged
# them. Both are small, both are read, and each one's absence is silent:
#
#   apktool.yml          carries versionCode/versionName. Missing, klj_guest_version
#                        falls back to 1.28's 545 — and a SPLIT APPLICATION BINARY
#                        guest builds its OBB NAME out of that number, so the
#                        1.3 GB of game data staged above is looked for under
#                        main.545... and never opened. That is what left 1.40 on
#                        device with `Unable to start Oculus XR Plugin` (the XR
#                        subsystem descriptors live in the OBB, at
#                        bin/Data/UnitySubsystems/OculusXRPlugin/) and Addressables
#                        reporting `No Location found for Key=AppInit` — a black
#                        immersive space, three layers from the missing file.
#   AndroidManifest.xml  carries the <meta-data> Bundle and the <uses-permission>
#                        list. Missing, every Oculus manifest setting reads as
#                        "not declared" and every permission check as DENIED.
#
# Present-or-absent, like the OBB: a tree unpacked some other way still stages.
META=()
for m in apktool.yml AndroidManifest.xml; do
  [ -f "$ROOT/$TREE/$m" ] && META+=("$ROOT/$TREE/$m")
done

# ...and they are stageable ON THEIR OWN, which is not a convenience.
#
#   ./stage_assets.sh --meta [<device-udid>]
#
# The gate in run.sh exists for a 2.2 GB upload and is skipped by default —
# `KL_SKIP_STAGE=1` is in the device loop as a matter of course. These two files
# are 12 KB, they change with the TREE rather than with the APK's size and
# mtime, and one of them names the OBB. Staged together with the assets they
# would have arrived only on a full re-upload: the run that found this had the
# right OBB on the device, the fix in the build, and still looked for
# main.545 — because nothing had re-staged since the fix. So run.sh calls this
# every run, and the stamp only ever gates the expensive half.
META_ONLY=0
if [ "${1:-}" = "--meta" ]; then META_ONLY=1; shift; fi

TARGET="${1:-}"

# stage_dlc <dest-dir>          — local filesystem destination (simulator)
#
# **`*.obb` is excluded, and that is not tidiness.** For Beat Saber 1.40 the
# game's 1.34 GB application binary sits in this same dump directory, and the
# destination is the same <files>/obb the OBB block stages it to — so without
# the exclusion it would be copied twice, and worse, COUNTED as a song:
# kl_ovrplat's GetList enumerates every regular file in that directory and
# reports each one as an owned asset, so the guest would be told it is entitled
# to a 1.3 GB level pack whose name is the OBB's. The runtime skips the
# extension too (kl_ovrplat.c); both halves are needed, because the file has to
# BE there and must not be read as one of these.
stage_dlc_local() {
    dlc_wanted || { echo "[stage] DLC: not this target (or no DLC dir) — skipping"; return; }
    local dest="$1"
    rm -rf "$dest"
    mkdir -p "$dest"
    (cd "$DLC_SRC" && find . -maxdepth 1 -type f ! -name '.*' ! -name '*.obb' \
        -exec cp -c {} "$dest/" \;)
    stage_repaired_over "$dest"
    echo "[stage] DLC: $(ls "$dest" | wc -l | tr -d ' ') asset files -> $dest"
}

if [ -z "$TARGET" ]; then
  UDID=$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)
  [ -n "$UDID" ] || { echo "!! no booted simulator"; exit 1; }
  CONTAINER=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data 2>/dev/null) || {
    echo "!! $BUNDLE_ID is not installed on $UDID — install the app first"; exit 1; }
  DEST="$CONTAINER/Documents"
  mkdir -p "$DEST/$TREE"
  if [ "$META_ONLY" = 1 ]; then
    [ ${#META[@]} -gt 0 ] && cp "${META[@]}" "$DEST/$TREE/"
    echo "[stage] simulator $UDID: ${#META[@]} metadata file(s) -> $DEST/$TREE"
    exit 0
  fi
  echo "[stage] simulator $UDID -> $DEST ($KLT_NAME)"
  rm -rf "$DEST/$TREE/assets"
  # A copy, not a symlink: the guest resolves paths by concatenation
  # and a link would work here but hide a real failure on device.
  cp -R "$ASSETS" "$DEST/$TREE/assets"
  cp "$APK" "$DEST/$KLT_APK"
  [ ${#META[@]} -gt 0 ] && cp "${META[@]}" "$DEST/$TREE/"
  # The DLC BEFORE the OBB, because it clears its destination directory and for
  # a Unity target that directory is the OBB's too (OBB_REL is "obb"). The old
  # order was safe only while no target had both.
  stage_dlc_local "$DEST/android-files/obb"
  # ...and the OBB file by file, dereferenced, rather than as a directory: the
  # source may BE the DLC dump (see the fallback above), and the ~246 song
  # bundles in it are the DLC's business, not a second copy here.
  if [ -n "$OBB" ]; then
    mkdir -p "$DEST/android-files/$OBB_REL"
    rm -f "$DEST/android-files/$OBB_REL"/*.obb
    for f in "$OBB"/*.obb; do
      [ -e "$f" ] || continue          # a dangling link on THIS machine
      cp -L "$f" "$DEST/android-files/$OBB_REL/"
    done
  fi
  if [ -n "$RETAIL" ]; then
    # -L, because what userdata holds are symlinks into the user's install.
    n=0
    for f in "$RETAIL"/*/base/assets*.pk3; do
      [ -e "$f" ] || continue
      rel="JKXR/${f#"$RETAIL"/}"
      mkdir -p "$DEST/android-files/$(dirname "$rel")"
      cp -L "$f" "$DEST/android-files/$rel"
      n=$((n + 1))
    done
    echo "[stage] $n retail pk3(s) -> $DEST/android-files/JKXR"
  fi
  if [ -n "$KLT_QTPLUGINS" ]; then
    rm -rf "$DEST/$TREE/qtplugins"
    mkdir -p "$DEST/$TREE/qtplugins"
    cp "$ROOT/$KLT_QTPLUGINS"/libplugins_*.so "$DEST/$TREE/qtplugins/"
  fi
  # The /sdcard tree, after the OBB and plugins: it is a destination of its
  # own (android-files/<gamedest>), so nothing above can collide with it. cp -c
  # is clonefile — gigabytes, instant on APFS, same reasoning as the DLC dir.
  if gamedata_wanted; then
    rm -rf "$DEST/android-files/$GAMEDATA_DEST"
    mkdir -p "$DEST/android-files/$GAMEDATA_DEST"
    cp -Rc "$GAMEDATA_SRC/." "$DEST/android-files/$GAMEDATA_DEST/" 2>/dev/null || \
      cp -R "$GAMEDATA_SRC/." "$DEST/android-files/$GAMEDATA_DEST/"
    echo "[stage] gamedata: $(du -sh "$DEST/android-files/$GAMEDATA_DEST" | cut -f1) -> $DEST/android-files/$GAMEDATA_DEST"
  fi
  echo "[stage] done: $(du -sh "$DEST" | cut -f1)"
  exit 0
fi

# Beat Saber's 2.2 GB is the reason any of this exists; Steam Link's is 44 MB
# and takes seconds. Saying which is which up front is the difference between
# waiting and wondering.
copy() {   # <source> <destination-relative-to-container>
  xcrun devicectl device copy to --device "$TARGET" \
    --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
    --source "$1" --destination "$2"
}
# ...and the same thing for an upload big enough that the SOCKET is a risk.
#
# `devicectl device copy to` of one 15 GB directory is one transfer over one
# socket, and Arkham Shadow's dump is the size where that stops arriving:
# round 1 died 80 files in with `NSPOSIXErrorDomain error 54 — the socket was
# closed unexpectedly`, having moved several GB. There is no resume, so the
# retry is ours to do, and it is only useful per FILE — restarting a directory
# copy re-sends everything that already landed.
copy_retry() {   # <source> <destination-relative-to-container>
  local n=0
  until copy "$1" "$2" > /dev/null 2>&1; do
    n=$((n + 1))
    [ "$n" -lt 4 ] || { echo "[stage] !! gave up on $(basename "$1") after $n tries"; return 1; }
    echo "[stage]    retry $n: $(basename "$1")"
    sleep 5
  done
}
# The LEDGER of what is already on the device — read OFF THE DEVICE, not
# remembered locally.
#
# Round 2 is the reason. A local ledger file is a claim about what THIS Mac
# uploaded, and there are three ways for that to be a lie about the device:
#
#   - installing rotates the data container, and everything the last run
#     uploaded is then in a directory nobody will ever read again;
#   - a `copy to` that dies mid-file leaves the destination CREATED and EMPTY,
#     which is exactly what round 1 left behind: 54 of 80 names present and
#     every large one Zero KB. A name-and-size ledger written before the fact
#     would have called those done;
#   - the device is shared with fourteen other ports and can be cleaned up
#     underneath us.
#
# `devicectl device info files --json-output` reports name and size for the
# directory, which is the same key the ledger uses, so asking the device costs
# one call and removes all three. The file under build/ is now a CACHE of that
# answer plus this run's own uploads, not state to be trusted across runs.
device_ledger() {   # <container-subdirectory> <ledger-path>
  local out; out=$(mktemp)
  if xcrun devicectl device info files --device "$TARGET" \
       --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
       --subdirectory "$1" --no-recurse --json-output "$out" > /dev/null 2>&1
  then
    python3 - "$out" > "$2" <<'PY' || : > "$2"
import json, sys
for f in json.load(open(sys.argv[1]))["result"]["files"]:
    if f.get("resources", {}).get("isDirectory"):
        continue
    print(f["name"], f["metadata"]["size"])
PY
  else
    # No listing means no directory (or no app): nothing is on the device, and
    # the caller has to create the directory before it can copy files into it.
    : > "$2"
    rm -f "$out"
    return 1
  fi
  rm -f "$out"
}
copy_meta() {
  for m in "${META[@]:-}"; do
    [ -n "$m" ] && copy "$m" "Documents/$TREE/$(basename "$m")" > /dev/null
  done
}
if [ "$META_ONLY" = 1 ]; then
  copy_meta
  echo "[stage] device $TARGET: ${#META[@]} metadata file(s) -> Documents/$TREE"
  exit 0
fi
# Beat Saber's 2.2 GB is the reason any of this exists; Steam Link's is 44 MB
# and takes seconds. Saying which is which up front is the difference between
# waiting and wondering.
# -L: both the OBB directory and the retail one are deliberately symlinks into
# an install elsewhere on this machine, and what is uploaded is what they point
# at. Without it this line says 44 MB and then spends twenty minutes.
#
# An array, because the OBB source can now be the DLC dump directory and that
# path has spaces in it ("games/Beat Saber v1661+1.40.6_6407/..."). Unquoted
# ${OBB:+$OBB} split it into four arguments and du complained about all four.
SIZED=("$ASSETS" "$APK")
if [ -n "$OBB" ]; then SIZED+=("$OBB"); fi
if [ -n "$RETAIL" ]; then SIZED+=("$RETAIL"); fi
echo "[stage] device $TARGET, $KLT_NAME ($(du -shcL "${SIZED[@]}" 2>/dev/null | tail -1 | cut -f1))"
copy "$ASSETS" "Documents/$TREE/assets"
copy "$APK"    "Documents/$KLT_APK"
copy_meta
# DLC, BEFORE the OBB: it uploads a whole directory to Documents/android-files/
# obb and the OBB lands in that same directory for a Unity target, so the OBB
# goes second or not at all. (The destination is "obb" literally rather than
# $OBB_REL — this one is not the guest's OBB path but the directory
# kl_ovrplat's GetList enumerates, which kl_app.c builds as <files>/obb.)
#
# A filtered staging dir of clonefiles (instant on APFS), then one upload of it.
# Filtered twice: no dotfiles, and no *.obb — see stage_dlc_local's note.
if dlc_wanted; then
  STAGE_DIR="$PWD/$ROOT/build/dlc-stage/obb"
  rm -rf "$PWD/$ROOT/build/dlc-stage"
  mkdir -p "$STAGE_DIR"
  (cd "$DLC_SRC" && find . -maxdepth 1 -type f ! -name '.*' ! -name '*.obb' -exec cp -c {} "$STAGE_DIR/" \;) || \
  (cd "$DLC_SRC" && find . -maxdepth 1 -type f ! -name '.*' ! -name '*.obb' -exec cp {} "$STAGE_DIR/" \;)
  # The repair goes over the top BEFORE the ledger is consulted, so the size in
  # the ledger key is the repaired file's — which is exactly what makes a device
  # still holding the tampered copy re-receive it (different size, key misses).
  stage_repaired_over "$STAGE_DIR"
  echo "[stage] DLC: uploading $(ls "$STAGE_DIR" | wc -l | tr -d ' ') asset files"
  # ONE directory copy while it fits in one socket's lifetime, FILE BY FILE
  # when it does not. The threshold is not a tuning knob so much as the
  # observation that Beat Saber's 2.2 GB has always arrived in one go and
  # Arkham Shadow's 15 GB has never done so.
  #
  # File by file costs one devicectl invocation each (~1 s of setup), which
  # against 80 files is a rounding error next to re-sending 15 GB, and it buys
  # two things a directory copy cannot have: a per-file retry, and a LEDGER —
  # so a run that dies half way up resumes rather than starting over. The
  # ledger is keyed by device, file name and size, because that is what makes
  # "already there" a claim about THIS device rather than about this Mac.
  #
  # The trade against the directory copy is that nothing clears the
  # destination first, so a file dropped from a newer dump would linger. That
  # is what `rm -rf build/dlc-stage` plus a fresh KL_STAGE=1 run does not fix
  # and a note here has to: if the dump's file SET changes, wipe
  # Documents/android-files/obb on the device by hand.
  DLC_BYTES=$(du -skL "$STAGE_DIR" | cut -f1)
  DLC_N=$(ls "$STAGE_DIR" | wc -l | tr -d ' ')
  # An EMPTY staging dir is not "upload nothing", it is a hard failure:
  # `devicectl device copy to` of an empty directory onto a destination that
  # does not exist yet dies with "Failed to retrieve the file node for
  # Documents/android-files/obb (CoreDeviceError 7000)", and with `set -e` that
  # takes the OBB upload below down with it. lastclockwinder is the first
  # target whose DLC dump is JUST the .obb — every asset file is filtered out,
  # so the dir is legitimately empty and the OBB block is the only thing that
  # had anything to do. The OBB block mkdir's its own way in by uploading a
  # file into the path.
  if [ "$DLC_N" = 0 ]; then
    echo "[stage] DLC: nothing but the OBB in $DLC_SRC — no directory upload"
  elif [ "$DLC_BYTES" -lt 4194304 ]; then
    copy "$STAGE_DIR" "Documents/android-files/obb"
  else
    LEDGER="$PWD/$ROOT/build/dlc-uploaded-$TARGET.txt"
    mkdir -p "$(dirname "$LEDGER")"
    # Seed the ledger from the DEVICE (see device_ledger), before anything is
    # written — a name that is there at zero bytes is not a name that is done.
    if device_ledger "Documents/android-files/obb" "$LEDGER"; then
      echo "[stage] DLC: device already holds $(wc -l < "$LEDGER" | tr -d ' ') file(s) in obb/"
    else
      # No listing means no directory, and the only thing here that creates one
      # is a directory copy — so an empty one goes first.
      EMPTY=$(mktemp -d)/obb; mkdir -p "$EMPTY"
      copy "$EMPTY" "Documents/android-files/obb" > /dev/null 2>&1 || true
      rm -rf "$(dirname "$EMPTY")"
    fi
    dlc_n=0; dlc_skip=0; dlc_fail=0
    dlc_total=$(ls "$STAGE_DIR" | wc -l | tr -d ' ')
    for f in "$STAGE_DIR"/*; do
      [ -e "$f" ] || continue
      key="$(basename "$f") $(stat -f %z "$f")"
      dlc_n=$((dlc_n + 1))
      if grep -qxF "$key" "$LEDGER"; then dlc_skip=$((dlc_skip + 1)); continue; fi
      echo "[stage]  ($dlc_n/$dlc_total) $key"
      if copy_retry "$f" "Documents/android-files/obb/$(basename "$f")"; then
        echo "$key" >> "$LEDGER"
      else
        dlc_fail=$((dlc_fail + 1))
      fi
    done
    echo "[stage] DLC: $((dlc_n - dlc_skip - dlc_fail)) uploaded, $dlc_skip already on device, $dlc_fail failed"
    [ "$dlc_fail" = 0 ] || { echo "[stage] !! DLC incomplete — re-run with KL_STAGE=1"; exit 1; }
  fi
fi
# The OBB, FILE BY FILE and through the RESOLVED path — not `copy "$OBB"`.
#
# The obb directory a host run reads is deliberately symlinks into bonelab_obb/
# (6.8 GB is not worth a second copy on this machine), and `devicectl device
# copy to` copies a symlink AS a symlink. The device then holds two ~175-byte
# files with exactly the right names, pointing at a path that does not exist
# there — so every name-based check passes, `getObbDirs()` finds them, and the
# only symptom is Unity saying the OBB's GUID is `''` a thousand log lines
# later. kl_jni.c's obb census now stats them and says so by name; this is the
# other half, which is not staging a link in the first place.
#
# Passing each resolved path dereferences naturally, and it also means the
# destination is the FILE's path rather than the directory's parent, which is
# why this cannot just gain a flag.
if [ -n "$OBB" ]; then
  # ...and with the same device-read ledger as the DLC set, for the same
  # reason: Arkham Shadow's three .obb files are 623 MB, 1.2 GB and 3.5 GB, so
  # a run that dies on the third must not re-send the first two. Read off the
  # device rather than remembered, so a zero-byte survivor of a dead transfer
  # counts as absent (see device_ledger).
  OBB_LEDGER="$PWD/$ROOT/build/obb-uploaded-$TARGET.txt"
  mkdir -p "$(dirname "$OBB_LEDGER")"
  device_ledger "Documents/android-files/$OBB_REL" "$OBB_LEDGER" || : > "$OBB_LEDGER"
  for f in "$OBB"/*.obb; do
    [ -e "$f" ] || continue          # a dangling link on THIS machine too
    real=$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$f")
    obb_key="$(basename "$f") $(stat -Lf %z "$f")"
    if grep -qxF "$obb_key" "$OBB_LEDGER"; then
      echo "[stage] device $TARGET, $KLT_NAME $(basename "$f") — already on device"
      continue
    fi
    echo "[stage] device $TARGET, $KLT_NAME $f"
    # ...with the retry, for the reason copy_retry exists: Arkham Shadow's
    # per-language voice packs are 1.2-1.4 GB each and share the socket's fate.
    copy_retry "$real" "Documents/android-files/$OBB_REL/$(basename "$f")" || exit 1
    echo "$obb_key" >> "$OBB_LEDGER"
    echo "[stage] done $f"
  done
fi
# The retail pk3s, file by file and through the RESOLVED path, for the reason
# the OBB is staged that way: what userdata holds are symlinks into the user's
# own install, `devicectl device copy to` copies a symlink AS a symlink, and the
# device would then hold four correctly named 72-byte files pointing at a Mac
# path. Every name-based check passes and the engine reports the game data
# missing anyway.
if [ -n "$RETAIL" ]; then
  for f in "$RETAIL"/*/base/assets*.pk3; do
    [ -e "$f" ] || continue          # a dangling link on THIS machine too
    real=$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$f")
    echo "[stage] device $TARGET, $KLT_NAME $(basename "$f") ($(du -h "$real" | cut -f1))"
    copy "$real" "Documents/android-files/JKXR/${f#"$RETAIL"/}"
    echo "[stage] done $(basename "$f")"
  done
fi
# The Qt plugin .so files, when the target asks for them. They are DATA, not a
# loader path — see the `qtplugins` note in targets.py: Qt globs this directory
# and parses each candidate's ELF metadata before it will dlopen it, and the
# dlopen itself still resolves to the signed framework in the bundle.
# devicectl copies a directory, so they are staged through one locally first.
if [ -n "$KLT_QTPLUGINS" ]; then
  STAGE=$(mktemp -d)/qtplugins
  mkdir -p "$STAGE"
  cp "$ROOT/$KLT_QTPLUGINS"/libplugins_*.so "$STAGE/"
  copy "$STAGE" "Documents/$TREE/qtplugins"
  rm -rf "$(dirname "$STAGE")"
fi
# The /sdcard tree last: gigabytes, so the size is said before the upload starts
# (the same courtesy as the OBB line). A direct directory copy — the source is
# a real directory on this machine, no symlink trap of the OBB kind.
if gamedata_wanted; then
  echo "[stage] device $TARGET, $KLT_NAME /sdcard tree -> $GAMEDATA_DEST ($(du -sh "$GAMEDATA_SRC" | cut -f1))"
  copy "$GAMEDATA_SRC" "Documents/android-files/$GAMEDATA_DEST"
fi
echo "[stage] done"
