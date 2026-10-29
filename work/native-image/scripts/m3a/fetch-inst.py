#!/usr/bin/env python3
# fetch-inst.py <mc-version> [loader]  (M3 stage A, 2026-10-12)
# Fetches a PARALLEL Minecraft+Fabric instance into work/native-image/inst-<v>/ without touching the 1.21.5 tree:
#   inst-<v>/files/versions/<v>/{<v>.json, client.jar, fabric-profile.json}   Mojang + Fabric meta (sha1-verified client)
#   inst-<v>/files/libraries/<maven path>                                      vanilla + fabric libraries (sha1-verified;
#                                                                              lwjgl skipped = we use dl/lwjgl 3.3.3 + the
#                                                                              QC fork, same rule as tools/vivecraft_install_instance.py)
#   inst-<v>/assets/{indexes/<id>.json, objects/}                              asset index + objects; objects are first
#                                                                              APFS-CLONED (cp -c, no extra disk) from the
#                                                                              1.21.5 objects dir, so only new hashes download
#   inst-<v>/files/mods/<dest>                                                 mods pinned in scripts/m3a/mods-<v>.json
#                                                                              (Modrinth sha512 / GitHub sha256 verified)
# Re-runnable: everything already present and hash-valid is skipped.
import hashlib, importlib.util, json, os, subprocess, sys
from pathlib import Path
_HERE = Path(__file__).resolve()
W = str(_HERE.parents[2])          # work/native-image
ROOT = str(_HERE.parents[4])       # repo root
spec = importlib.util.spec_from_file_location('inst', str(Path(ROOT) / 'tools' / 'vivecraft_install_instance.py'))
K = importlib.util.module_from_spec(spec); spec.loader.exec_module(K)
V = sys.argv[1]; LOADER = sys.argv[2] if len(sys.argv) > 2 else '0.19.5'
I = f'{W}/inst-{V}'; F = f'{I}/files'
os.makedirs(f'{F}/versions/{V}', exist_ok=True)
man = K.https_json(K.MANIFEST)
row = [r for r in man['versions'] if r['id'] == V][0]
vj = K.https_json(row['url'])
json.dump(vj, open(f'{F}/versions/{V}/{V}.json', 'w'), indent=1)
c = vj['downloads']['client']
assert K.download(c['url'], f'{F}/versions/{V}/client.jar', c['sha1'], c['size']), 'client'
fp = K.https_json(f'https://meta.fabricmc.net/v2/versions/loader/{V}/{LOADER}/profile/json')
json.dump(fp, open(f'{F}/versions/{V}/fabric-profile.json', 'w'), indent=1)
vjobs, fjobs = K.lib_jobs(vj), K.lib_jobs(fp)
for jobs, lab in ((vjobs, 'vanilla'), (fjobs, 'fabric')):
    ok, bad = K.run_jobs(jobs, f'{F}/libraries', lab)
    assert not bad, bad
with open(f'{F}/versions/{V}/libs-vanilla.txt', 'w') as f:
    for j in vjobs: f.write('libraries/' + j[0] + '\n')
with open(f'{F}/versions/{V}/libs-fabric.txt', 'w') as f:
    for j in fjobs: f.write('libraries/' + j[0] + '\n')
# LWJGL from the version json (26.x only: 3.4.1, the :unsafe core as Mojang ships it). Only for the Knot/bake run on
# stock GLFW + Apple GL; the klglfw/ltw runs and the native image use work/natives/build/lwjgl-<ver>/ (natives agent).
# 1.21.x keeps skipping lwjgl (dl/lwjgl 3.3.3 + the QC fork).
if not V.startswith('1.'):
    ljobs = []
    for lib in vj['libraries']:
        n = lib['name']
        if not n.startswith('org.lwjgl:'): continue
        parts = n.split(':')
        if len(parts) == 4 and parts[3] != 'natives-macos-arm64' and parts[3] != 'unsafe': continue
        a = lib['downloads']['artifact']
        ljobs.append((a['path'], a['url'], a['sha1'], a['size']))
    ok, bad = K.run_jobs(ljobs, f'{F}/libraries', 'lwjgl')
    assert not bad, bad
    with open(f'{F}/versions/{V}/libs-lwjgl.txt', 'w') as f:
        for j in ljobs: f.write('libraries/' + j[0] + '\n')
# assets
ai = vj['assetIndex']; A = f'{I}/assets'
os.makedirs(f'{A}/indexes', exist_ok=True)
idx = K.https_json(ai['url'])
json.dump(idx, open(f"{A}/indexes/{ai['id']}.json", 'w'))
open(f'{I}/asset-index-id.txt', 'w').write(ai['id'] + '\n')
# Optional clone from an older instance if one is already on disk. A 26.2-only
# tree has neither; every object is downloaded below.
cands = [os.path.realpath(f'{W}/inst-1.21.11/assets/objects'), os.path.realpath(f'{W}/assets/objects')]
old = next((o for o in cands if os.path.isdir(o) and o != os.path.realpath(f'{A}/objects')), None)
if old and not os.path.isdir(f'{A}/objects'):
    subprocess.check_call(['cp', '-cR', old, f'{A}/objects'])
ajobs = [(f"{m['hash'][:2]}/{m['hash']}", K.ASSET_BASE + f"{m['hash'][:2]}/{m['hash']}", m['hash'], m['size'])
         for m in idx['objects'].values()]
ok, bad = K.run_jobs(ajobs, f'{A}/objects', 'assets')
assert not bad, bad
# mods
os.makedirs(f'{F}/mods', exist_ok=True)
for m in json.load(open(f'{W}/scripts/m3a/mods-{V}.json')):
    d = f"{F}/mods/{m['dest']}"
    algo = 'sha512' if 'sha512' in m else 'sha256'
    def good():
        return os.path.isfile(d) and hashlib.new(algo, open(d, 'rb').read()).hexdigest() == m[algo]
    if not good():
        if os.path.exists(d): os.remove(d)
        if 'local' in m:
            local = m['local']
            if not os.path.isabs(local):
                local = os.path.join(ROOT, local)
            subprocess.check_call(['cp', local, d])   # our own build (Vivecraft jar)
        else: K.download(m['url'], d)
    assert good(), m['dest']
    print('  mod', m['dest'], m['version'], hashlib.sha256(open(d, 'rb').read()).hexdigest())
print('DONE', V, 'assets', ai['id'], len(idx['objects']), 'objects; libs', len(vjobs), '+', len(fjobs))
