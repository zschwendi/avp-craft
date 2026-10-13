#!/usr/bin/env python3
"""Build source on an Apple Silicon Mac with Java25 and Xcode Command Line Tools."""
import argparse, json, os, platform, re, shutil, subprocess, tarfile, zipfile
from pathlib import Path
from lib import ROOT, CACHE, lock, download, safe_name, zip_write, digest

def fetch_dependencies(l):
    meta_path = download(l['minecraft']['metadata_url'], CACHE / 'minecraft-metadata.json', {'sha1': l['minecraft']['metadata_sha1']})
    meta = json.loads(meta_path.read_text())
    files = []
    item = meta['downloads']['client']
    files.append(download(item['url'], CACHE / 'deps/minecraft-client.jar', {'sha1': item['sha1']}, item['size']))
    for library in meta['libraries']:
        item = library.get('downloads', {}).get('artifact')
        if item and 'natives-' not in item['path']:
            files.append(download(item['url'], CACHE / 'deps' / item['path'], {'sha1': item['sha1']}, item['size']))
    # Copied from the official Fabric profile into the reviewed lock; do not trust a later mutable profile.
    for library in l['fabric']['libraries']:
        group, name, version = library['name'].split(':')
        rel = f'{group.replace(".", "/")}/{name}/{version}/{name}-{version}.jar'
        hashes = {'sha256': library['sha256']} if 'sha256' in library else {'sha256': l['fabric']['loader_sha256']}
        if name == 'fabric-loader' and version != l['fabric']['loader']: raise ValueError('Unexpected Fabric loader')
        files.append(download(library['url'] + rel, CACHE / 'deps' / rel, hashes, library.get('size')))
    for name in ['sodium', 'fabric_api']:
        item = l[name]
        jar = download(item['url'], CACHE / 'deps' / item['filename'], {'sha512': item['sha512'], 'sha1': item['sha1']}, item['size'])
        files.append(jar)
    # Fabric Loader also nests MixinExtras. Every nested compiler API is covered by its parent's pinned digest.
    for jar in list(files):
        with zipfile.ZipFile(jar) as z:
            for entry in z.namelist():
                if entry.startswith('META-INF/jars/') and entry.endswith('.jar'):
                    safe_name(entry)
                    target = CACHE / 'deps/nested' / (jar.stem + '-' + digest(jar.read_bytes())[:16]) / Path(entry).name
                    target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(z.read(entry)); files.append(target)
    return files

def renderer_source(l, work):
    archive = download(l['upstream']['url'], CACHE / 'upstream.tar.gz', {'sha256': l['upstream']['sha256']})
    if work.exists(): raise ValueError('Build work directory exists; choose a fresh --output directory')
    work.mkdir(parents=True)
    with tarfile.open(archive, 'r:gz') as t:
        for entry in t.getmembers():
            rel = '/'.join(entry.name.split('/')[1:])
            if not entry.isfile() or not (rel.startswith(('metal/src/main/', 'LICENSES/')) or rel in ['LICENSE', 'NOTICE']): continue
            safe_name(rel)
            target = work / rel; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(t.extractfile(entry).read())
    patch = ROOT / 'patches/renderer/changes.patch'
    if patch.stat().st_size:
        # The reconstructed source is not a Git checkout. In a cloned public repo,
        # discovery of the parent .git would silently filter all patch paths out.
        patch_env = os.environ.copy()
        for name in ['GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_COMMON_DIR']: patch_env.pop(name, None)
        patch_env['GIT_CEILING_DIRECTORIES'] = str(work.parent.resolve())
        subprocess.run(['git', 'apply', '--check', str(patch)], cwd=work, env=patch_env, check=True)
        subprocess.run(['git', 'apply', str(patch)], cwd=work, env=patch_env, check=True)
    for f in sorted((ROOT / 'patches/renderer/added').rglob('*')):
        if f.is_file():
            dest = work / f.relative_to(ROOT / 'patches/renderer/added'); dest.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(f, dest)
    verify_renderer_results(work)
    return work

def verify_renderer_results(work):
    manifest = json.loads((ROOT / 'source-manifest.json').read_text())
    results = manifest.get('renderer_result_files', {})
    if set(results) != set(manifest['renderer_changed_paths']):
        raise ValueError('Expected renderer result hashes are missing from the source manifest; re-import reviewed source')
    for name, expected in results.items():
        safe_name(name)
        path = work / name
        if expected is None:
            if path.exists(): raise ValueError('Deleted renderer file remains after reconstruction: ' + name)
        elif not path.is_file() or digest(path.read_bytes()) != expected:
            raise ValueError('Renderer reconstruction differs from approved source: ' + name)

def verify_snapshot():
    manifest = json.loads((ROOT / 'source-manifest.json').read_text())
    if manifest['upstream_commit'] != lock()['upstream']['commit']:
        raise ValueError('Source manifest uses a different upstream commit')
    actual = {f.relative_to(ROOT).as_posix() for directory in ['patches', 'mods']
              for f in (ROOT / directory).rglob('*') if f.is_file()}
    if actual != set(manifest['source_files']):
        raise ValueError('Source snapshot file list changed; review and regenerate the manifest')
    for name, expected in manifest['source_files'].items():
        safe_name(name)
        if digest((ROOT / name).read_bytes()) != expected:
            raise ValueError('Source snapshot changed: ' + name)

def javac(jdk, source, classes, cp):
    classes.mkdir(parents=True)
    args = ['-proc:none', '--release', '25', '--add-modules', 'jdk.incubator.vector', '-classpath', cp, '-d', str(classes)] + [str(p) for p in sorted(source.rglob('*.java'))]
    argfile = classes.parent / (classes.name + '.args')
    argfile.write_text('\n'.join('"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"' for s in args) + '\n')
    # javac's argument-file parser cannot pass -J VM flags; provide those to the launcher directly.
    subprocess.run([str(jdk / 'bin/javac'), '-J-Xmx768m', '@' + str(argfile)], check=True)

def package(classes, resources, licenses, output, version, native=None):
    with zipfile.ZipFile(output, 'w') as z:
        manifest = 'Manifest-Version: 1.0\r\nImplementation-Version: ' + version + '\r\n'
        if native: manifest += 'Main-Class: mcopt.metal.InstallHelp\r\n'
        zip_write(z, 'META-INF/MANIFEST.MF', (manifest + '\r\n').encode())
        for f in sorted(classes.rglob('*.class')): zip_write(z, f.relative_to(classes).as_posix(), f.read_bytes())
        for f in sorted(resources.rglob('*')):
            if not f.is_file(): continue
            data = f.read_bytes(); name = f.relative_to(resources).as_posix()
            if name == 'META-INF/MANIFEST.MF': continue
            if name == 'fabric.mod.json':
                meta = json.loads(data.decode().replace('${version}', version)); meta['version'] = version
                meta.setdefault('contact', {})['sources'] = 'https://github.com/' + lock()['repository']
                if meta['id'] == 'mcopt-metal': meta['name'] = 'Minecraft Metal Mac'
                elif meta['id'] == 'roundedterrain': meta['name'] = 'Curved Terrain'
                elif meta['id'] == 'zach_rounded_hands':
                    meta['name'] = 'Curved First-Person Arms'
                    meta['description'] = 'Curved arm and sleeve cuboids retaining vanilla skin UVs and animations.'
                data = (json.dumps(meta, indent=2) + '\n').encode()
            zip_write(z, name, data)
        for name, f in licenses: zip_write(z, name, f.read_bytes())
        if native: zip_write(z, 'natives/libmcmetal.dylib', native.read_bytes())

def native_toolchain():
    """Frame interpolation headers require SDK26; its runtime calls remain availability guarded."""
    try:
        version = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-version'], text=True).strip()
        sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
        clang = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--find', 'clang'], text=True).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit('Select Xcode26+ or Command Line Tools with macOS SDK26+; xcrun could not find the native compiler SDK.') from error
    if not re.fullmatch(r'\d+(?:\.\d+)*', version) or int(version.split('.')[0]) < 26:
        raise SystemExit(f'Native source compilation needs macOS SDK26+ (selected: {version}). Select Xcode26+ or current Command Line Tools. Runtime deployment remains macOS15; optional frame generation needs macOS26.')
    if not (sdk / 'usr/include/simd/simd.h').is_file() or not (sdk / 'System/Library/Frameworks/MetalFX.framework/Headers/MTLFXFrameInterpolator.h').is_file():
        raise SystemExit('Selected SDK lacks required SIMD/MetalFX frame-interpolation headers; use a complete macOS SDK26+ installation.')
    return {'version': version, 'sdk': sdk, 'clang': clang}

def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--version', required=True); p.add_argument('--jdk', type=Path); p.add_argument('--output', type=Path, default=ROOT / 'build/release')
    a = p.parse_args(); l = lock()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.+-]*', a.version): raise ValueError('Invalid version')
    if platform.system() != 'Darwin' or platform.machine() != 'arm64': raise SystemExit('Build on macOS ARM64; Rosetta/Intel builds are unsupported.')
    toolchain = native_toolchain()
    jdk = a.jdk or Path(subprocess.check_output(['/usr/libexec/java_home', '-v', '25'], text=True).strip())
    props = subprocess.run([str(jdk / 'bin/java'), '-XshowSettings:properties', '-version'], capture_output=True, text=True, check=True).stderr
    if 'java.specification.version = 25' not in props or 'os.arch = aarch64' not in props: raise SystemExit('Use an ARM64 Java25 JDK.')
    if not (ROOT / 'source-manifest.json').exists(): raise SystemExit('Import final approved source first: tools/import_source.py')
    verify_snapshot()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    cp = os.pathsep.join(map(str, fetch_dependencies(l))); work = renderer_source(l, out / 'work')
    classes = out / 'renderer-classes'; javac(jdk, work / 'metal/src/main/java', classes, cp)
    native = out / 'libmcmetal.dylib'
    native_root = work / 'metal/src/main/native'
    # Import SIMD explicitly instead of depending on newer framework headers importing it transitively.
    cmd = [toolchain['clang'], '-isysroot', str(toolchain['sdk']), '-include', 'simd/simd.h', '-fno-objc-arc', '-O2', '-Wall', '-arch', 'arm64', '-mmacosx-version-min=15.0', '-dynamiclib', '-install_name', '@rpath/libmcmetal.dylib']
    for framework in ['AppKit', 'CoreGraphics', 'CoreVideo', 'Foundation', 'Metal', 'MetalFX', 'QuartzCore']: cmd += ['-framework', framework]
    subprocess.run(cmd + ['-o', str(native)] + [str(native_root / f) for f in l['native_sources']], check=True)
    subprocess.run(['codesign', '--sign', '-', '--force', str(native)], check=True)
    licenses = [('LICENSE', work / 'LICENSE'), ('NOTICE', work / 'NOTICE')] + [('LICENSES/' + f.name, f) for f in sorted((ROOT / 'LICENSES/upstream/LICENSES').iterdir()) if f.is_file()]
    outputs = []
    renderer = out / f'minecraft-metal-mac-{a.version}.jar'; package(classes, work / 'metal/src/main/resources', licenses, renderer, a.version, native); outputs.append(renderer)
    for module in ['rounded-terrain', 'rounded-hands']:
        src = ROOT / 'mods' / module; cls = out / (module + '-classes'); javac(jdk, src / 'src/main/java', cls, cp)
        jar = out / f'{module}-{a.version}.jar'; package(cls, src / 'src/main/resources', [('LICENSE', src / 'LICENSE')], jar, a.version); outputs.append(jar)
    receipt = {'schema': 1, 'version': a.version, 'minecraft': l['minecraft']['version'], 'java': 25, 'architecture': 'arm64', 'native_sdk': toolchain['version'], 'native_deployment_target': '15.0', 'native_force_include': 'simd/simd.h', 'upstream_commit': l['upstream']['commit'], 'source_manifest_sha256': digest((ROOT / 'source-manifest.json').read_bytes()), 'renderer_result_hashes_verified': True, 'runtime_tested': False, 'artifacts': []}
    for f in outputs: receipt['artifacts'].append({'name': f.name, 'sha256': digest(f.read_bytes()), 'sha512': digest(f.read_bytes(), 'sha512'), 'sha1': digest(f.read_bytes(), 'sha1'), 'size': f.stat().st_size})
    (out / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('Built three owned mod jars; game and dependency jars remain only in .cache.')

if __name__ == '__main__': main()
