#!/usr/bin/env python3
"""Import approved text source from working repos. Never imports game data or binaries."""
import argparse, hashlib, json, subprocess
from pathlib import Path
from lib import ROOT, lock, safe_name

EXTENSIONS = {'.java', '.metal', '.m', '.c', '.h', '.json', '.properties'}

def run(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args])

def renderer_path(name):
    return name.startswith(('metal/src/main/java/', 'metal/src/main/native/', 'metal/src/main/resources/')) and Path(name).suffix in EXTENSIONS

def import_renderer(repo):
    base = lock()['upstream']['commit']
    run(repo, 'cat-file', '-e', base + '^{commit}')
    names = run(repo, 'diff', '--name-only', base, '--', 'metal/src/main').decode().splitlines()
    names += run(repo, 'ls-files', '--others', '--exclude-standard', '--', 'metal/src/main').decode().splitlines()
    changed = sorted(set(n for n in names if renderer_path(n)))
    tracked = set(run(repo, 'ls-files').decode().splitlines())
    patch = run(repo, 'diff', '--no-ext-diff', base, '--', *[n for n in changed if n in tracked]) if any(n in tracked for n in changed) else b''
    if b'GIT binary patch' in patch or b'Binary files ' in patch:
        raise ValueError('Renderer import accepts text changes only')
    snapshot = {'patches/renderer/changes.patch': patch}
    for name in changed:
        safe_name(name)
        if name not in tracked:
            source = repo / name
            if source.is_symlink(): raise ValueError('Source symlink is not accepted: ' + name)
            data = source.read_bytes(); data.decode('utf-8')
            snapshot['patches/renderer/added/' + name] = data
    return changed, snapshot

def import_module(source, name):
    snapshot = {}
    for p in sorted((source / 'src/main').rglob('*')):
        if p.is_file() and p.suffix in EXTENSIONS:
            if p.is_symlink(): raise ValueError('Source symlink is not accepted: ' + str(p))
            data = p.read_bytes(); data.decode('utf-8')
            snapshot['mods/' + name + '/' + p.relative_to(source).as_posix()] = data
    if not snapshot: raise ValueError('Module has no approved source files: ' + name)
    snapshot['mods/' + name + '/LICENSE'] = (source / 'LICENSE').read_bytes()
    return snapshot

def renderer_result_files(repo, changed):
    results = {}
    for name in changed:
        safe_name(name)
        source = repo / name
        if source.is_symlink(): raise ValueError('Source symlink is not accepted: ' + name)
        results[name] = hashlib.sha256(source.read_bytes()).hexdigest() if source.is_file() else None
    return results

def write_snapshot(changed, snapshot, results=None):
    # Validate the entire import before creating any public files. Re-import is deliberately explicit.
    if (ROOT / 'source-manifest.json').exists() or any(
            f.is_file() for directory in ['patches', 'mods'] for f in (ROOT / directory).rglob('*')):
        raise ValueError('An imported snapshot is already present; review it before re-importing')
    for name, data in snapshot.items():
        safe_name(name); data.decode('utf-8')
    results = results or {}
    if set(results) != set(changed): raise ValueError('Expected result hashes are required for every changed renderer path')
    manifest = {'schema': 1, 'upstream_commit': lock()['upstream']['commit'],
                'renderer_changed_paths': changed,
                'renderer_result_files': results,
                'source_files': {name: hashlib.sha256(data).hexdigest() for name, data in sorted(snapshot.items())}}
    for name, data in snapshot.items():
        target = ROOT / name; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
    (ROOT / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--renderer', type=Path, required=True)
    p.add_argument('--terrain', type=Path, required=True)
    p.add_argument('--hands', type=Path, required=True)
    a = p.parse_args()
    changed, snapshot = import_renderer(a.renderer)
    snapshot.update(import_module(a.terrain, 'rounded-terrain'))
    snapshot.update(import_module(a.hands, 'rounded-hands'))
    write_snapshot(changed, snapshot, renderer_result_files(a.renderer, changed))
    print('Imported approved source; run tools/audit.py before committing.')

if __name__ == '__main__': main()
