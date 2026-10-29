#!/usr/bin/env python3
"""Install the files a QuestCraft Play needs before it can call Minecraft.

Mirrors pojlib's installer for one already-recorded instance:

  * vanilla version json, client jar, and libraries (names containing
    "lwjgl" are skipped; org.ow2.asm is skipped unless the version id
    itself contains "fabric")
  * the newest stable Fabric loader profile and its libraries
  * the asset index and objects
  * QuestCraft's mods.json mods for this version, when that list has one
  * the LWJGL classes jar, ResConfHack, and the unpacked config files

Classpath is written as paths relative to the files directory, one jar
per line, at files/versions/<id>/classpath.txt. It is not stored in
instances.json: that file's reader drops an instance object bigger than
64KB, and the in-memory classpath field is 1024 bytes. mainClass and
assetIndex are the two short fields the Java installer writes back.

This is not part of make check. It does not talk to the headset.
"""

import hashlib
import json
import os
import shutil
import ssl
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
FABRIC_LOADERS = "https://meta.fabricmc.net/v2/versions/loader"
MODS_JSON = (
    "https://raw.githubusercontent.com/QuestCraftPlusPlus/Pojlib/"
    "refs/heads/QuestCraft-6.0.0/mods.json"
)
ASSET_BASE = "https://resources.download.minecraft.net/"
CTX = ssl.create_default_context()


def die(msg):
    print("FAILED: " + msg, flush=True)
    sys.exit(1)


def files_root():
    env = os.environ.get("KL_FILES_DIR")
    if env:
        return os.path.join(env, "files")
    home = os.environ.get("HOME")
    if not home:
        die("HOME is unset")
    return os.path.join(
        home, "Library/Application Support/Klepton/userdata/vivecraft/files"
    )


def https_json(url):
    if not url.startswith("https://"):
        die("refusing a non-https url")
    req = urllib.request.Request(url, headers={"User-Agent": "klepton-vivecraft"})
    with urllib.request.urlopen(req, context=CTX, timeout=60) as r:
        return json.load(r)


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url, dest, sha1=None, size=None):
    if not url or not url.startswith("https://"):
        return False
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if os.path.isfile(dest) and os.path.getsize(dest) > 0:
        if sha1 is None or sha1_of(dest) == sha1:
            if size is None or os.path.getsize(dest) == size:
                return True
        os.remove(dest)
    tmp = dest + ".partial"
    last = None
    for _ in range(5):
        try:
            req = urllib.request.Request(
                url, headers={"User-Agent": "klepton-vivecraft"}
            )
            with urllib.request.urlopen(req, context=CTX, timeout=120) as r, open(
                tmp, "wb"
            ) as f:
                shutil.copyfileobj(r, f, length=1 << 20)
            if sha1 and sha1_of(tmp) != sha1:
                os.remove(tmp)
                last = "sha1"
                continue
            os.replace(tmp, dest)
            return True
        except Exception as e:
            last = str(e)
            try:
                os.remove(tmp)
            except OSError:
                pass
    print(f"  miss {os.path.basename(dest)}: {last}", flush=True)
    return False


def fetch_sha1(url):
    if not url.startswith("https://"):
        return None
    try:
        req = urllib.request.Request(
            url + ".sha1", headers={"User-Agent": "klepton-vivecraft"}
        )
        with urllib.request.urlopen(req, context=CTX, timeout=30) as r:
            text = r.read(80).decode("ascii", "replace").strip().split()[0]
        return text if len(text) == 40 else None
    except Exception:
        return None


def maven_rel(name):
    parts = name.split(":")
    if len(parts) < 3:
        return None
    group, artifact, version = parts[0], parts[1], parts[2]
    if not group or not artifact or not version:
        return None
    if any(x in (group, artifact, version) for x in ("..", "/", "\\")):
        return None
    jar = f"{artifact}-{version}.jar"
    return f"{group.replace('.', '/')}/{artifact}/{version}/{jar}"


def skip_lib(name, version_id):
    if "lwjgl" in name:
        return True
    if "org.ow2.asm" in name and "fabric" not in version_id:
        return True
    return False


def lib_jobs(version_json):
    vid = version_json.get("id") or ""
    jobs = []
    for lib in version_json.get("libraries") or []:
        name = lib.get("name") or ""
        if not name or skip_lib(name, vid):
            continue
        art = (lib.get("downloads") or {}).get("artifact") or None
        if art and art.get("path") and art.get("url"):
            rel = art["path"]
            if rel.startswith("/") or ".." in rel.split("/"):
                continue
            jobs.append((rel, art["url"], art.get("sha1"), art.get("size")))
            continue
        rel = maven_rel(name)
        base = lib.get("url") or "https://libraries.minecraft.net/"
        if not rel or not base.startswith("https://"):
            continue
        if not base.endswith("/"):
            base += "/"
        url = base + rel
        jobs.append((rel, url, fetch_sha1(url), None))
    return jobs


def run_jobs(jobs, dest_root, label):
    ok = 0
    bad = []

    def one(job):
        rel, url, sha, size = job
        dest = os.path.join(dest_root, rel)
        return rel, download(url, dest, sha, size)

    workers = 8 if len(jobs) > 8 else max(1, len(jobs))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futs = [pool.submit(one, j) for j in jobs]
        done = 0
        for fut in as_completed(futs):
            rel, good = fut.result()
            done += 1
            if good:
                ok += 1
            else:
                bad.append(rel)
            if done % 25 == 0 or done == len(jobs):
                print(f"  {label} {done}/{len(jobs)}", flush=True)
    return ok, bad


def latest_fabric_loader():
    loaders = https_json(FABRIC_LOADERS)
    for row in loaders:
        if row.get("stable") and row.get("version") and "+" not in row["version"]:
            return row["version"]
    for row in loaders:
        ver = row.get("version") or ""
        if ver and "+" not in ver:
            return ver
    die("fabric loader list was empty")


def instance_slug(name):
    out = []
    for ch in name:
        if "A" <= ch <= "Z":
            ch = chr(ord(ch) + 32)
        elif ch == " ":
            ch = "_"
        out.append(ch)
    return "".join(out)


def local_game_dir(inst, files):
    stored = inst.get("gameDir") or ""
    if stored and os.path.isdir(stored):
        return stored, True
    slug = instance_slug(inst.get("instanceName") or "")
    if not slug or "/" in slug or slug.startswith("."):
        die("instance name does not make a directory slug")
    path = os.path.join(files, "instances", slug)
    os.makedirs(path, exist_ok=True)
    return path, False


def copy_tree_files(pairs):
    for src, dest in pairs:
        if not os.path.isfile(src):
            print(f"  local file absent: {os.path.basename(src)}", flush=True)
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if not os.path.isfile(dest):
            shutil.copy2(src, dest)


def install_mods(files, game, version, default_mods):
    raw = os.path.join(files, "new_mods.json")
    if not download(MODS_JSON, raw):
        print("  mods.json was not fetched", flush=True)
        return 0
    with open(raw) as f:
        doc = json.load(f)
    names = [v.get("name") for v in doc.get("versions") or []]
    print("  mods.json versions: " + ", ".join(n or "?" for n in names), flush=True)
    chosen = None
    for v in doc.get("versions") or []:
        if v.get("name") == version:
            chosen = v
            break
    if not chosen:
        print(f"  mods.json has no list for {version}", flush=True)
        return 0
    mods = list(chosen.get("coreMods") or [])
    if default_mods:
        mods.extend(chosen.get("defaultMods") or [])
    n = 0
    for mod in mods:
        link = mod.get("download_link") or ""
        if not link.startswith("https://"):
            continue
        kind = mod.get("type") or "mod"
        folder = "resourcepacks" if kind == "resourcepack" else "mods"
        ext = ".zip" if kind == "resourcepack" else ".jar"
        base = mod.get("fileName") or mod.get("slug") or "mod"
        if "/" in base or ".." in base:
            continue
        dest = os.path.join(game, folder, base + ext)
        if download(link, dest):
            n += 1
    print(f"  mods placed: {n}", flush=True)
    return n


def main():
    files = files_root()
    inst_path = os.path.join(files, "instances.json")
    if not os.path.isfile(inst_path):
        die("instances.json is absent")
    with open(inst_path) as f:
        doc = json.load(f)
    instances = doc.get("instances") or []
    want = os.environ.get("KL_MC_VERSION") or ""
    chosen = None
    if want:
        hits = [i for i in instances if i.get("versionName") == want]
        if len(hits) != 1:
            die(f"{len(hits)} instances match the requested version")
        chosen = hits[0]
    elif len(instances) == 1:
        chosen = instances[0]
    else:
        die(f"{len(instances)} instances; set KL_MC_VERSION")
    version = chosen.get("versionName") or ""
    if not version or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-" for c in version):
        die("version id is not usable")
    print(f"installing {version}", flush=True)

    manifest = https_json(MANIFEST)
    meta_url = None
    for row in manifest.get("versions") or []:
        if row.get("id") == version:
            meta_url = row.get("url")
            break
    if not meta_url:
        die(f"{version} is not in the version manifest")
    vanilla = https_json(meta_url)
    ver_dir = os.path.join(files, "versions", version)
    os.makedirs(ver_dir, exist_ok=True)
    with open(os.path.join(files, "versions", f"{version}.json"), "w") as f:
        json.dump(vanilla, f)
        f.write("\n")

    client = ((vanilla.get("downloads") or {}).get("client")) or {}
    client_url = client.get("url") or ""
    client_sha = client.get("sha1")
    client_size = client.get("size")
    client_dest = os.path.join(ver_dir, "client.jar")
    print(f"  client {client_size} bytes", flush=True)
    if not download(client_url, client_dest, client_sha, client_size):
        die("client jar was not fetched")

    loader = latest_fabric_loader()
    print(f"  fabric loader {loader}", flush=True)
    profile_url = (
        f"https://meta.fabricmc.net/v2/versions/loader/{version}/{loader}/profile/json"
    )
    fabric = https_json(profile_url)
    with open(os.path.join(ver_dir, "fabric-profile.json"), "w") as f:
        json.dump(fabric, f)
        f.write("\n")
    main_class = fabric.get("mainClass") or ""
    if not main_class or len(main_class) > 190:
        die("fabric profile has no usable mainClass")
    print(f"  mainClass {main_class}", flush=True)

    lib_root = os.path.join(files, "libraries")
    vjobs = lib_jobs(vanilla)
    fjobs = lib_jobs(fabric)
    print(f"  vanilla libraries {len(vjobs)}, fabric libraries {len(fjobs)}", flush=True)
    vok, vbad = run_jobs(vjobs, lib_root, "vanilla")
    fok, fbad = run_jobs(fjobs, lib_root, "fabric")
    if vbad or fbad:
        die(f"library downloads failed: {len(vbad)} vanilla, {len(fbad)} fabric")
    print(f"  libraries ok {vok + fok}", flush=True)

    asset_index = vanilla.get("assetIndex") or {}
    asset_id = asset_index.get("id") or ""
    assets_name = vanilla.get("assets") or asset_id
    if not asset_id or len(asset_id) > 60:
        die("version json has no asset index id")
    index_url = asset_index.get("url") or ""
    index = https_json(index_url)
    idx_dir = os.path.join(files, "assets", "indexes")
    os.makedirs(idx_dir, exist_ok=True)
    for name in {asset_id, assets_name}:
        with open(os.path.join(idx_dir, name + ".json"), "w") as f:
            json.dump(index, f)
            f.write("\n")
    with open(os.path.join(ver_dir, "asset-index-id.txt"), "w") as f:
        f.write(asset_id + "\n")
    objects = index.get("objects") or {}
    print(f"  assets {len(objects)} objects, index {asset_id}", flush=True)
    obj_root = os.path.join(files, "assets", "objects")
    ajobs = []
    for _name, meta in objects.items():
        digest = meta.get("hash") or ""
        if len(digest) != 40:
            continue
        rel = f"{digest[:2]}/{digest}"
        url = ASSET_BASE + rel
        ajobs.append((rel, url, digest, meta.get("size")))
    aok, abad = run_jobs(ajobs, obj_root, "assets")
    if abad:
        die(f"asset downloads failed: {len(abad)}")
    print(f"  assets ok {aok}", flush=True)

    apk_assets = os.path.join(ROOT, "vivecraft", "assets")
    copy_tree_files([
        (os.path.join(apk_assets, "lwjgl", "lwjgl-glfw-classes.jar"),
         os.path.join(files, "lwjgl3", "lwjgl-glfw-classes.jar")),
        (os.path.join(apk_assets, "hacks", "ResConfHack.jar"),
         os.path.join(files, "hacks", "ResConfHack.jar")),
        (os.path.join(apk_assets, "hacks", "resolv.conf"),
         os.path.join(files, "hacks", "resolv.conf")),
    ])
    game, stored_ok = local_game_dir(chosen, files)
    print(f"  game dir is the stored path: {stored_ok}", flush=True)
    copy_tree_files([
        (os.path.join(apk_assets, "sodium-options.json"),
         os.path.join(game, "config", "sodium-options.json")),
        (os.path.join(apk_assets, "smoothboot.json"),
         os.path.join(game, "config", "smoothboot.json")),
        (os.path.join(apk_assets, "immediatelyfast.json"),
         os.path.join(game, "config", "immediatelyfast.json")),
        (os.path.join(apk_assets, "moreculling.toml"),
         os.path.join(game, "config", "moreculling.toml")),
        (os.path.join(apk_assets, "modernfix-mixins.properties"),
         os.path.join(game, "config", "modernfix-mixins.properties")),
        (os.path.join(apk_assets, "vivecraft-client-config.json"),
         os.path.join(game, "config", "vivecraft-client-config.json")),
        (os.path.join(apk_assets, "options.txt"),
         os.path.join(game, "options.txt")),
        (os.path.join(apk_assets, "servers.dat"),
         os.path.join(game, "servers.dat")),
    ])
    install_mods(files, game, version, bool(chosen.get("defaultMods")))

    rels = [f"versions/{version}/client.jar"]
    for rel, _url, _sha, _size in vjobs:
        rels.append("libraries/" + rel)
    rels.append("lwjgl3/lwjgl-glfw-classes.jar")
    rels.append("hacks/ResConfHack.jar")
    for rel, _url, _sha, _size in fjobs:
        rels.append("libraries/" + rel)
    rels.append("lwjgl3/lwjgl-glfw-classes.jar")
    rels.append("hacks/ResConfHack.jar")
    # installLWJGL appends the same classes jar once more.
    rels.append("lwjgl3/lwjgl-glfw-classes.jar")
    missing = [r for r in rels if not os.path.isfile(os.path.join(files, r))]
    if missing:
        die(f"classpath is missing {len(missing)} jars")
    cp_path = os.path.join(ver_dir, "classpath.txt")
    with open(cp_path, "w") as f:
        for rel in rels:
            f.write(rel + "\n")
    print(f"  classpath {len(rels)} entries", flush=True)

    changed = False
    if chosen.get("mainClass") != main_class:
        chosen["mainClass"] = main_class
        changed = True
    if chosen.get("assetIndex") != asset_id:
        chosen["assetIndex"] = asset_id
        changed = True
    if changed:
        tmp = inst_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
        os.replace(tmp, inst_path)
        print("  instance mainClass and assetIndex updated", flush=True)
    else:
        print("  instance mainClass and assetIndex already set", flush=True)
    print("DONE", flush=True)


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as e:
        die(f"{type(e).__name__}: {e}")
