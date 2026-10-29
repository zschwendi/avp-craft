#!/bin/bash
# Copy the native stage tree into the .app when KL_BUNDLE_PAYLOAD=1.
#
# visionOS has no Contents/ folder. The tree lands at
# KleptonVivecraft.app/klepton-payload/ and is inside the IPA. A fresh data
# container has none of it: KleptonPayload.swift copies the writable defaults
# (options, config) into Documents and symlinks the jars and assets back at
# this directory. Saves, command history, usercache, and server lists are
# stripped here and never enter the IPA. stage_native.sh is not modified, so
# a devicectl stage still has its worlds. An already-staged container is left
# alone.
#
# Off unless KL_BUNDLE_PAYLOAD=1. run.sh sets that for a vivecraft device
# build. A simulator build stays off so the edit loop does not clone the
# payload into every .app. KL_BUNDLE_PAYLOAD=0 on device keeps the devicectl
# stage (stage_native.sh) as the only copy.
#
# MCV is the same knob stage_native.sh uses. Unset, on vivecraft, means 26.2,
# which is the image this IPA is built to launch.
set -euo pipefail

DEST="${TARGET_BUILD_DIR:-}/${UNLOCALIZED_RESOURCES_FOLDER_PATH:-}/klepton-payload"

if [ "${KL_BUNDLE_PAYLOAD:-0}" != 1 ]; then
  if [ -n "${TARGET_BUILD_DIR:-}" ] && [ -n "${UNLOCALIZED_RESOURCES_FOLDER_PATH:-}" ]; then
    rm -rf "$DEST"
  fi
  echo "bundle_payload: off (KL_BUNDLE_PAYLOAD!=1)"
  exit 0
fi

cd "${PROJECT_DIR:?}"
if [ -z "${MCV:-}" ] && [ "${KLEPTON_TARGET:-vivecraft}" = vivecraft ]; then
  MCV=26.2
  echo "bundle_payload: MCV unset, using 26.2"
fi
export MCV
export KLEPTON_TARGET="${KLEPTON_TARGET:-vivecraft}"

mkdir -p "$PWD/build"
if ! ./stage_native.sh --local > "$PWD/build/bundle-payload-stage.log"; then
  echo "error: bundle_payload: stage_native.sh failed (visionos/build/bundle-payload-stage.log)"
  tail -30 "$PWD/build/bundle-payload-stage.log" >&2 || true
  exit 1
fi

# Same stage directory stage_native.sh just wrote. Kept here so a change to
# that script's MCV layout fails this copy instead of bundling the wrong tree.
eval "$(python3 targets.py "$KLEPTON_TARGET")"
NR="$(cd "$PWD/../$KLT_NATIVEROOT" && pwd)"
if [ "$MCV" = 1.21.5 ]; then
  STAGE="$PWD/build/native-stage/$KLT_NAME"
else
  STAGE="$PWD/build/native-stage/$KLT_NAME-$MCV"
fi
[ -f "$STAGE/launch.txt" ] && [ -f "$STAGE/baked.cp" ] && [ -d "$STAGE/root" ] \
  || { echo "error: bundle_payload: $STAGE is not a stage tree"; exit 1; }

TREE="$(python3 targets.py "$KLEPTON_TARGET" tree)"
rm -rf "$DEST"
mkdir -p "$DEST"
cp "$STAGE/launch.txt" "$STAGE/baked.cp" "$DEST/"
if ! cp -Rc "$STAGE/root" "$DEST/root" 2>/dev/null; then
  rm -rf "$DEST/root"
  cp -R "$STAGE/root" "$DEST/root"
fi
find "$DEST" -name '.DS_Store' -delete

# The copy above is the dev stage, which holds a person's worlds and names.
# This is the only place those are removed. A name that is not on the fgamedir
# allowlist does not ship, so a new personal file cannot ride along by accident.
python3 - "$DEST" <<'PY'
import os, re, shutil, sys
dest = sys.argv[1]
UUID_NAME = re.compile(
    r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")

# Top of every fgamedir. Anything else (saves, caches, history) is dropped.
ALLOW = {
    "mods", ".fabric", "resourcepacks",
    "config", "options.txt", "keyboardtheme.txt", "data",
}
# Anywhere under the payload, as a directory or a file. Jars are not unpacked.
DENY_DIRS = {"saves", "logs", "crash-reports", "screenshots"}
DENY_FILES = {
    "command_history.txt", "usercache.json", "username-cache.json", "servers.dat",
}
# options.txt keys that name a server or an account. Resolution and the
# sound/key rows stay. soundCategory_player is a volume, not an account.
def drop_option(key):
    k = key.strip().lower()
    if k in {"lastserver", "username", "playername", "account", "accountname"}:
        return True
    if "username" in k or "gamertag" in k or k.endswith("uuid"):
        return True
    return False

removed = []
for dirpath, dirnames, filenames in os.walk(dest, topdown=True):
    base = os.path.basename(dirpath)
    if base == "fgamedir":
        for name in list(dirnames) + list(filenames):
            if name in ALLOW or name == ".DS_Store":
                continue
            path = os.path.join(dirpath, name)
            if not os.path.lexists(path):
                continue
            removed.append(os.path.relpath(path, dest))
            if os.path.islink(path) or not os.path.isdir(path):
                os.remove(path)
            else:
                shutil.rmtree(path)
            if name in dirnames:
                dirnames.remove(name)
    # Denied names inside an allowed directory (config/voicechat/username-cache.json).
    for name in list(dirnames):
        if name not in DENY_DIRS:
            continue
        path = os.path.join(dirpath, name)
        if not os.path.lexists(path):
            continue
        removed.append(os.path.relpath(path, dest))
        shutil.rmtree(path)
        dirnames.remove(name)
    for name in list(filenames):
        if name not in DENY_FILES:
            continue
        path = os.path.join(dirpath, name)
        if not os.path.lexists(path):
            continue
        removed.append(os.path.relpath(path, dest))
        os.remove(path)

for dirpath, dirnames, filenames in os.walk(dest):
    if os.path.basename(dirpath) != "fgamedir":
        continue
    for name in filenames:
        if name != "options.txt":
            continue
        path = os.path.join(dirpath, name)
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        kept = []
        dropped = 0
        for line in lines:
            key = line.split(":", 1)[0] if ":" in line else line
            if drop_option(key):
                dropped += 1
                continue
            kept.append(line)
        text = "\n".join(kept) + ("\n" if kept else "")
        open(path, "w", encoding="utf-8").write(text)
        if dropped:
            removed.append(os.path.relpath(path, dest) + f" (-{dropped} option keys)")

# Regression lock. A later edit that copies the stage tree and forgets this
# pass fails the build instead of shipping a world.
bad = []
for dirpath, dirnames, filenames in os.walk(dest):
    rel_dir = os.path.relpath(dirpath, dest)
    if os.path.basename(dirpath) == "fgamedir":
        for name in dirnames + filenames:
            if name not in ALLOW and name != ".DS_Store":
                bad.append(os.path.join(rel_dir, name))
    for name in dirnames:
        if name in DENY_DIRS:
            bad.append(os.path.join(rel_dir, name))
        if UUID_NAME.search(name):
            bad.append(UUID_NAME.sub("<uuid>", os.path.join(rel_dir, name)))
    for name in filenames:
        if name in DENY_FILES:
            bad.append(os.path.join(rel_dir, name))
        if name == "options.txt":
            body = open(os.path.join(dirpath, name), encoding="utf-8", errors="replace").read()
            for line in body.splitlines():
                key = line.split(":", 1)[0] if ":" in line else ""
                if drop_option(key) or key.strip().lower() == "lastserver":
                    bad.append(os.path.join(rel_dir, name) + ":" + key)
        # Player data is named with an account UUID (uuid.json, uuid.dat).
        # Redact before the build log can echo it.
        if UUID_NAME.search(name) or UUID_NAME.search(os.path.basename(dirpath)):
            bad.append(UUID_NAME.sub("<uuid>", os.path.join(rel_dir, name)))

# launch.txt stays the offline template (@USER@, nil uuid, token 0). A baked
# account name or token fails the build. The value is not printed.
launch_path = os.path.join(dest, "launch.txt")
if os.path.isfile(launch_path):
    lines = open(launch_path, encoding="utf-8", errors="replace").read().splitlines()
    nxt = {}
    for i, line in enumerate(lines):
        if i + 1 < len(lines) and line.strip() in ("--username", "--uuid", "--accessToken"):
            nxt[line.strip()] = lines[i + 1].strip()
    if nxt.get("--username") not in ("@USER@", "Player"):
        bad.append("launch.txt --username is not the offline placeholder")
    if nxt.get("--uuid") not in ("00000000-0000-0000-0000-000000000000",):
        bad.append("launch.txt --uuid is not the offline placeholder")
    if nxt.get("--accessToken") not in ("0",):
        bad.append("launch.txt --accessToken is not the offline placeholder")
if bad:
    sys.stderr.write("error: bundle_payload: personal data still in the payload:\n")
    for b in bad[:40]:
        sys.stderr.write("  " + b + "\n")
    sys.exit(1)
print("bundle_payload: scrub removed %d (%s)" % (
    len(removed), ", ".join(removed[:12]) or "nothing"))
PY

# A Java .class file starts with 0xCAFEBABE, which is also the fat Mach-O
# magic. installd and SideStore parse every file in the bundle by its magic,
# so a loose class file fails the install ("FAT slice bounds exceed file
# size"). Each baked classpath directory that holds class files is packed into
# a jar beside it, and baked.cp names the jar. Fabric reads both the same way.
# A Mach-O here would be unsigned and fail the install too. Either one left
# anywhere in the payload fails the build.
python3 - "$DEST" <<'PY'
import os, shutil, sys, zipfile
dest = sys.argv[1]
root = os.path.join(dest, "root")
MAGIC = {b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
         b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce"}

def magic(path):
    with open(path, "rb") as f:
        return f.read(4) in MAGIC

cp_path = os.path.join(dest, "baked.cp")
lines = open(cp_path, encoding="utf-8").read().splitlines()
out, packed = [], []
for line in lines:
    entry = line.strip()
    d = os.path.join(root, entry)
    if entry and not entry.startswith("#") and os.path.isdir(d) and not os.path.islink(d):
        files = sorted(os.path.join(r, f) for r, _, fs in os.walk(d) for f in fs if f != ".DS_Store")
        if any(magic(p) for p in files):
            jar = d.rstrip("/") + ".jar"
            with zipfile.ZipFile(jar, "w", zipfile.ZIP_DEFLATED) as z:
                for p in files:
                    info = zipfile.ZipInfo(os.path.relpath(p, d), date_time=(1980, 1, 1, 0, 0, 0))
                    info.compress_type = zipfile.ZIP_DEFLATED
                    z.writestr(info, open(p, "rb").read())
            shutil.rmtree(d)
            out.append(line.replace(entry, entry.rstrip("/") + ".jar"))
            packed.append("%s (%d files)" % (entry, len(files)))
            continue
    out.append(line)
open(cp_path, "w", encoding="utf-8").write("\n".join(out) + "\n")

bad = [os.path.relpath(os.path.join(r, f), dest)
       for r, _, fs in os.walk(dest) for f in fs
       if not os.path.islink(os.path.join(r, f)) and magic(os.path.join(r, f))]
if bad:
    sys.stderr.write("error: bundle_payload: class or Mach-O files would fail the install:\n")
    for b in bad[:40]:
        sys.stderr.write("  " + b + "\n")
    sys.exit(1)
print("bundle_payload: packed %s" % (", ".join(packed) or "nothing"))
PY

# Identity of the bytes we ship, after the scrub. A later app update whose
# hash differs refreshes the symlinks and does not replace options or worlds
# the player has created.
VER="$(find "$DEST/launch.txt" "$DEST/baked.cp" "$DEST/root" -type f ! -name '.DS_Store' -print0 \
  | sort -z | xargs -0 shasum -a 256 | shasum -a 256 | awk '{print $1}')"
[ -n "$VER" ] || { echo "error: bundle_payload: empty version hash"; exit 1; }
printf '%s\n' "$VER" > "$DEST/payload-version"
printf '%s\n' "$TREE" > "$DEST/tree"
# NR is unused except as a sanity that the instance we staged is the one on disk.
[ -d "$NR" ]
echo "bundle_payload: $TREE MCV=$MCV $(du -sh "$DEST" | awk '{print $1}') version ${VER:0:12} -> $DEST"
