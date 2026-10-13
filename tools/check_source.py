#!/usr/bin/env python3
"""Run CPU regressions against the built public release jars. No downloads or game/GPU launch."""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
import zipfile
from pathlib import Path

from lib import ROOT, CACHE, digest, lock, safe_name

FIXTURES = ROOT / 'tools/checks'
EXPECTED_IDS = {'mcopt-metal', 'roundedterrain', 'zach_rounded_hands'}


def verify_snapshot():
    path = ROOT / 'source-manifest.json'
    manifest = json.loads(path.read_text())
    if manifest['upstream_commit'] != lock()['upstream']['commit']:
        raise ValueError('Source manifest uses a different upstream commit')
    actual = {f.relative_to(ROOT).as_posix() for directory in ['patches', 'mods']
              for f in (ROOT / directory).rglob('*') if f.is_file()}
    if actual != set(manifest['source_files']):
        raise ValueError('Frozen source snapshot file list changed')
    for name, expected in manifest['source_files'].items():
        safe_name(name)
        if digest((ROOT / name).read_bytes()) != expected:
            raise ValueError('Frozen source snapshot changed: ' + name)
    return digest(path.read_bytes())


def release_jars(build):
    receipt_path = build / 'build-receipt.json'
    if not receipt_path.is_file():
        raise ValueError('Run tools/build.py successfully before tools/check_source.py')
    receipt = json.loads(receipt_path.read_text())
    if (receipt.get('minecraft') != lock()['minecraft']['version'] or receipt.get('java') != 25
            or receipt.get('architecture') != 'arm64' or receipt.get('upstream_commit') != lock()['upstream']['commit']):
        raise ValueError('Build receipt targets a different game, runtime or upstream source')
    if len(receipt['artifacts']) != 3:
        raise ValueError('Expected exactly three owned release jars')
    jars, ids = [], set()
    for item in receipt['artifacts']:
        name = item['name']
        safe_name(name)
        if Path(name).name != name or not name.endswith('.jar'):
            raise ValueError('Unsafe release artifact name')
        path = build / name
        data = path.read_bytes()
        if len(data) != item['size'] or any(digest(data, algorithm) != item[algorithm] for algorithm in ['sha1', 'sha256', 'sha512']):
            raise ValueError('Release artifact changed: ' + name)
        with zipfile.ZipFile(path) as jar:
            mod_id = json.loads(jar.read('fabric.mod.json'))['id']
        if mod_id not in EXPECTED_IDS or mod_id in ids:
            raise ValueError('Unexpected or duplicate release mod ID')
        ids.add(mod_id)
        jars.append(path)
    if ids != EXPECTED_IDS:
        raise ValueError('A required owned release jar is missing')
    return jars, receipt, digest(receipt_path.read_bytes())


def find_jdk(requested):
    if requested:
        jdk = requested
    elif os.environ.get('JAVA_HOME'):
        jdk = Path(os.environ['JAVA_HOME'])
    elif Path('/usr/libexec/java_home').is_file():
        jdk = Path(subprocess.check_output(['/usr/libexec/java_home', '-v', '25'], text=True).strip())
    else:
        executable = shutil.which('javac')
        if not executable:
            raise ValueError('Install Java25 or pass --jdk')
        jdk = Path(executable).resolve().parents[1]
    version = subprocess.check_output([str(jdk / 'bin/javac'), '-version'], stderr=subprocess.STDOUT, text=True)
    if not re.search(r'\bjavac\s+25(?:[.\s-]|$)', version):
        raise ValueError('These fixtures require the same Java25 ABI as the release')
    return jdk


def run(command, name):
    start = time.monotonic()
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(name + ' failed:\n' + result.stdout + result.stderr)
    return {'name': name, 'result': 'PASS', 'seconds': round(time.monotonic() - start, 3),
            'stdout': result.stdout.strip()}


def json_result(stdout, prefix=''):
    lines = [line[len(prefix):] for line in stdout.splitlines() if line.startswith(prefix + '{')]
    if len(lines) != 1:
        raise ValueError('Fixture did not produce one JSON result')
    return json.loads(lines[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--jdk', type=Path, help='Java25 JDK home; defaults to JAVA_HOME or system detection')
    parser.add_argument('--cc', default='clang', help='C11 compiler, defaults to clang')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/source-checks')
    args = parser.parse_args()
    manifest_hash = verify_snapshot()
    jars, receipt, receipt_hash = release_jars(args.build)
    jdk = find_jdk(args.jdk)
    dependencies = sorted((CACHE / 'deps').rglob('*.jar'))
    if not dependencies:
        raise ValueError('The ignored compiler dependency cache is missing; run tools/build.py first')
    header = ROOT / 'patches/renderer/added/metal/src/main/native/mcpresent_slots.h'
    built_header = args.build / 'work/metal/src/main/native/mcpresent_slots.h'
    if header.read_bytes() != built_header.read_bytes():
        raise ValueError('Public presentation header differs from the actual native build input')
    fixtures = sorted(FIXTURES.glob('*.java'))
    args.output.mkdir(parents=True, exist_ok=True)
    checks = []
    # Temporary classes/executable are freshly generated and removed after checking.
    # The classpath loads production fixes from the final release jars, never worker classes.
    with tempfile.TemporaryDirectory(prefix='regression-', dir=args.output) as scratch_name:
        scratch = Path(scratch_name)
        native_test = scratch / 'present-slots'
        checks.append(run([args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-UNDEBUG',
                           '-I', str(header.parent), str(FIXTURES / 'present_slots.c'), '-o', str(native_test)],
                          'compile presentation-slot fixture'))
        checks.append(run([str(native_test)], 'presentation-slot states and mailbox ownership'))
        classes = scratch / 'classes'
        classes.mkdir()
        classpath = os.pathsep.join(map(str, [*jars, *dependencies]))
        checks.append(run([str(jdk / 'bin/javac'), '-J-Xmx256m', '-proc:none', '--release', '25',
                           '-classpath', classpath, '-d', str(classes), *map(str, fixtures)],
                          'compile fixtures against actual release jars and Minecraft26.3 ABI'))
        runtime_classpath = os.pathsep.join(map(str, [classes, *jars, *dependencies]))
        for main_class, label in [('mcopt.metal.shade.NeutralMaterialCheck', 'neutral material provenance and compact mapping'),
                                  ('local.roundedterrain.OutlineGeometryCheck', 'outline geometry and actual vertex/pose/cache contract'),
                                  ('MeshTopology', 'fillet curvature, normals and exhaustive neighboring topology')]:
            # Keep CPU/heap use bounded while other development work is active.
            checks.append(run([str(jdk / 'bin/java'), '-Xmx256m', '-XX:ActiveProcessorCount=2',
                               '-cp', runtime_classpath, main_class], label))
    if verify_snapshot() != manifest_hash:
        raise ValueError('Source snapshot changed while regressions were running')
    # Recheck every artifact after execution to bind results to stable released bytes.
    if release_jars(args.build)[2] != receipt_hash:
        raise ValueError('Release receipt changed while regressions were running')
    report = {'schema': 1, 'result': 'PASS', 'minecraft': receipt['minecraft'], 'java': 25,
              'source_manifest_sha256': manifest_hash, 'build_receipt_sha256': receipt_hash,
              'artifacts': [{key: item[key] for key in ['name', 'sha256', 'size']} for item in receipt['artifacts']],
              'runner_sha256': digest(Path(__file__).read_bytes()),
              'fixtures': {f.name: digest(f.read_bytes()) for f in [*fixtures, FIXTURES / 'present_slots.c']},
              'presentation_header_sha256': digest(header.read_bytes()), 'checks': checks,
              'neutral_material': json_result(checks[3]['stdout']),
              'terrain_topology': json_result(checks[5]['stdout'], 'PASS '),
              'scope': 'CPU source/header behavior and built-JAR Minecraft26.3 ABI; no native GPU, game launch, frame pacing or visual acceptance'}
    (args.output / 'source-check-receipt.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': 'PASS', 'checks': len(checks), 'neutral_material': report['neutral_material'],
                      'terrain_topology': report['terrain_topology'], 'scope': report['scope']}, indent=2))


if __name__ == '__main__':
    main()
