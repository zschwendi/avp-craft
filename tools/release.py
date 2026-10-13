#!/usr/bin/env python3
"""Prepare owned release jars and a Prism-importable MRPACK. Does not upload anything."""
import argparse, json, re, urllib.parse, zipfile
from pathlib import Path
from lib import ROOT, lock, digest, safe_name, zip_write

IDS = {'mcopt-metal', 'roundedterrain', 'zach_rounded_hands'}

def check_jar(path):
    with zipfile.ZipFile(path) as z:
        if len(z.namelist()) != len(set(z.namelist())): raise ValueError('Duplicate archive entries')
        meta = json.loads(z.read('fabric.mod.json'))
        if meta['id'] not in IDS or meta.get('environment') != 'client': raise ValueError('Unexpected release mod')
        if 'LICENSE' not in z.namelist(): raise ValueError('Release mod lacks LICENSE')
        if meta['id'] == 'mcopt-metal' and ('NOTICE' not in z.namelist() or 'LICENSES/PolyForm-Shield-1.0.0.md' not in z.namelist()): raise ValueError('Renderer notices are missing')
        for name in z.namelist():
            safe_name(name)
            if name.startswith(('net/minecraft/', 'net/caffeinemc/', 'net/fabricmc/', 'com/mojang/',
                                'org/lwjgl/', 'org/joml/', 'org/objectweb/asm/', 'org/spongepowered/',
                                'saves/', 'resourcepacks/')) or name.endswith('.jar'): raise ValueError('Forbidden bundled dependency/content: ' + name)
    return meta['id']

def prepare(build, tag, repository, output):
    l = lock()
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository) or not re.fullmatch(r'[A-Za-z0-9_.-]+', tag): raise ValueError('Invalid repository/tag')
    receipt = json.loads((build / 'build-receipt.json').read_text())
    if receipt.get('minecraft') != l['minecraft']['version'] or receipt.get('java') != 25 or receipt.get('architecture') != 'arm64': raise ValueError('Unexpected build target')
    if len(receipt['artifacts']) != 3: raise ValueError('Exactly three owned client mod artifacts are required')
    # Review every input before copying any output or producing an importable manifest.
    files = []; found = set(); artifacts = []
    for item in receipt['artifacts']:
        name = item['name']; safe_name(name)
        if Path(name).name != name or not name.endswith('.jar'): raise ValueError('Invalid artifact filename')
        path = build / name; data = path.read_bytes()
        if len(data) != item['size'] or any(digest(data, algo) != item[algo] for algo in ['sha256', 'sha512', 'sha1']): raise ValueError('Build artifact changed: ' + name)
        mod_id = check_jar(path)
        if mod_id in found: raise ValueError('Duplicate release mod ID')
        found.add(mod_id); artifacts.append((name, data))
        files.append({'path': 'mods/' + name, 'hashes': {'sha512': item['sha512'], 'sha1': item['sha1']},
                      'env': {'client': 'required', 'server': 'unsupported'}, 'fileSize': item['size'],
                      'downloads': [f'https://github.com/{repository}/releases/download/{tag}/{urllib.parse.quote(name)}']})
    if found != IDS: raise ValueError('Exactly the three owned client mods must be present')
    if output.exists() and any(output.iterdir()): raise ValueError('Release output is not empty; use a fresh directory')
    output.mkdir(parents=True, exist_ok=True)
    for name, data in artifacts: (output / name).write_bytes(data)
    for name in ['sodium', 'fabric_api']:
        d = l[name]; files.append({'path': 'mods/' + d['filename'], 'hashes': {'sha512': d['sha512'], 'sha1': d['sha1']},
                                  'env': {'client': 'required', 'server': 'unsupported'}, 'fileSize': d['size'], 'downloads': [d['url']]})
    index = {'formatVersion': 1, 'game': 'minecraft', 'versionId': receipt['version'], 'name': 'Minecraft Metal Mac — M4 Balanced',
             'summary': 'Native Metal lighting and curved terrain/arms. Apple Silicon, Java25. Vanilla assets; legitimate Minecraft account required for online play.',
             'files': files, 'dependencies': {'minecraft': l['minecraft']['version'], 'fabric-loader': l['fabric']['loader']}}
    pack = output / f'minecraft-metal-mac-{tag}.mrpack'
    with zipfile.ZipFile(pack, 'w') as z:
        zip_write(z, 'modrinth.index.json', (json.dumps(index, indent=2) + '\n').encode())
        for f in sorted((ROOT / 'presets/m4-balanced').rglob('*')):
            if f.is_file(): zip_write(z, 'overrides/' + f.relative_to(ROOT / 'presets/m4-balanced').as_posix(), f.read_bytes())
    (output / 'SHA256SUMS').write_text('\n'.join(digest(f.read_bytes()) + '  ' + f.name for f in sorted(output.iterdir()) if f.suffix in ['.jar', '.mrpack']) + '\n')
    return pack

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=ROOT / 'build/release'); p.add_argument('--tag', required=True)
    p.add_argument('--repository', default=lock()['repository']); p.add_argument('--output', type=Path, default=ROOT / 'dist')
    a = p.parse_args()
    prepare(a.build, a.tag, a.repository, a.output)
    print('Prepared owned jars, dependency-download MRPACK and SHA256SUMS. No upload performed.')

if __name__ == '__main__': main()
