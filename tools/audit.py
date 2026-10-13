#!/usr/bin/env python3
"""Check the public source tree and release ZIP contents before publication."""
import argparse, json, re, urllib.parse, zipfile
from pathlib import Path
from lib import ROOT, lock, safe_name

EXCLUDED = {'.git', '.cache', 'build', 'dist', '__pycache__'}
FORBIDDEN_SUFFIXES = {'.jar', '.class', '.dylib', '.mp4', '.mov', '.png', '.jpg', '.jpeg', '.zip', '.mrpack'}
PERSONAL_PATH = re.compile(r'/Users/[^/\s]+/|[A-Za-z]:\\Users\\|/home/[^/\s]+/')
SECRET = re.compile(r'(?i)(?:accessToken|refreshToken|clientToken|xstsToken)\s*[=:]\s*[\"\x27]?[^\s\"\x27]+')

def audit(root):
    count = 0
    for f in sorted(root.rglob('*')):
        if not f.is_file() or any(p in EXCLUDED for p in f.relative_to(root).parts): continue
        rel = f.relative_to(root).as_posix()
        if f.suffix.lower() in FORBIDDEN_SUFFIXES or f.name == 'accounts.json' or any(p in ['saves', 'resourcepacks', 'screenshots', 'logs'] for p in f.relative_to(root).parts): raise ValueError('Forbidden public file: ' + rel)
        text = f.read_text('utf-8')
        # This script's detector patterns are intentionally not interpreted as private content.
        if f.name != 'audit.py' and (PERSONAL_PATH.search(text) or SECRET.search(text)): raise ValueError('Personal path/credential-shaped content: ' + rel)
        if rel.startswith('presets/') and re.search(r'(?i)mcopt\.(?:rec|probe)|(?:drsLog|shadowTimes|MTL_HUD|DYLD_)', text): raise ValueError('Diagnostic flags in public preset: ' + rel)
        count += 1
    return count

def audit_pack(path):
    with zipfile.ZipFile(path) as z:
        if len(z.namelist()) != len(set(z.namelist())): raise ValueError('Duplicate pack entries')
        for name in z.namelist():
            safe_name(name)
            if name.endswith('.jar') or any(x in name.split('/') for x in ['saves', 'resourcepacks', 'accounts.json']): raise ValueError('Game/mod bytes or private data embedded in MRPACK')
            if name != 'modrinth.index.json' and name not in ['overrides/config/mcopt.properties', 'overrides/options.txt']: raise ValueError('Unexpected override content')
            text = z.read(name).decode('utf-8')
            if PERSONAL_PATH.search(text) or SECRET.search(text): raise ValueError('Private content in import pack')
            if name.startswith('overrides/') and re.search(r'(?i)mcopt\.(?:rec|probe)|(?:drsLog|shadowTimes|MTL_HUD|DYLD_)', text): raise ValueError('Diagnostic flags in import pack')
        index = json.loads(z.read('modrinth.index.json'))
        if index.get('formatVersion') != 1 or index.get('game') != 'minecraft': raise ValueError('Unexpected pack format')
        if index['dependencies'] != {'minecraft': '26.3', 'fabric-loader': '0.19.5'}: raise ValueError('Unexpected pinned game/loader')
        if len(index['files']) != 5: raise ValueError('Unexpected mod dependency count')
        if len({item['path'] for item in index['files']}) != 5: raise ValueError('Duplicate dependency paths')
        pinned = {lock()[name]['filename']: lock()[name] for name in ['sodium', 'fabric_api']}
        seen_pinned = set()
        for item in index['files']:
            safe_name(item['path'])
            filename = Path(item['path']).name
            if item['path'] != 'mods/' + filename or not filename.endswith('.jar'): raise ValueError('Invalid download target')
            if item.get('env') != {'client': 'required', 'server': 'unsupported'} or item.get('fileSize', 0) <= 0: raise ValueError('Unexpected mod environment/size')
            for algo in ['sha1', 'sha512']:
                if not re.fullmatch('[0-9a-f]{' + str({'sha1': 40, 'sha512': 128}[algo]) + '}', item['hashes'][algo]): raise ValueError('Missing dependency hash')
            if filename in pinned:
                d = pinned[filename]; seen_pinned.add(filename)
                if item['downloads'] != [d['url']] or item['fileSize'] != d['size'] or any(item['hashes'][algo] != d[algo] for algo in ['sha1', 'sha512']): raise ValueError('Pinned dependency changed')
            else:
                if len(item['downloads']) != 1: raise ValueError('Unexpected owned-mod download count')
                url = urllib.parse.urlsplit(item['downloads'][0])
                if url.scheme != 'https' or url.netloc != 'github.com' or '/releases/download/' not in url.path or urllib.parse.unquote(url.path.split('/')[-1]) != filename: raise ValueError('Unexpected owned-mod release URL')
        if seen_pinned != set(pinned): raise ValueError('Pinned dependencies missing')

def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--pack', type=Path); a = p.parse_args()
    count = audit(ROOT)
    if a.pack: audit_pack(a.pack)
    print(f'PASS: {count} public text files; source/data boundaries checked' + ('; MRPACK checked' if a.pack else ''))

if __name__ == '__main__': main()
