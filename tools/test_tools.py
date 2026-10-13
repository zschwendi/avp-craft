#!/usr/bin/env python3
"""Exercise the actual import/release tools with isolated source and ZIP fixtures."""
import io, json, subprocess, tempfile, unittest, zipfile
from pathlib import Path
from unittest import mock
import audit, build, import_source, lib, release

class PackagingChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build_dir = self.root / 'compiled'; self.build_dir.mkdir()
        self.real_root = lib.ROOT
        self.preset = self.root / 'presets/m4-balanced/config'
        self.preset.mkdir(parents=True)
        (self.preset / 'mcopt.properties').write_text('mcopt.shade.drsFps=60\n')
        self.receipt = {'minecraft': '26.3', 'java': 25, 'architecture': 'arm64', 'version': 'fixture.1', 'artifacts': []}
        for mod_id in sorted(release.IDS):
            name = mod_id + '.jar'; data = self.jar(mod_id)
            (self.build_dir / name).write_bytes(data)
            self.receipt['artifacts'].append({'name': name, 'size': len(data), **{a: lib.digest(data, a) for a in ['sha1', 'sha256', 'sha512']}})
        self.write_receipt()

    def jar(self, mod_id, extra=None, license=True):
        target = io.BytesIO()
        with zipfile.ZipFile(target, 'w') as z:
            lib.zip_write(z, 'fabric.mod.json', json.dumps({'id': mod_id, 'environment': 'client'}).encode())
            if license: lib.zip_write(z, 'LICENSE', b'Fixture license')
            if mod_id == 'mcopt-metal':
                lib.zip_write(z, 'NOTICE', b'Fixture notices')
                lib.zip_write(z, 'LICENSES/PolyForm-Shield-1.0.0.md', b'Fixture license')
            if extra: lib.zip_write(z, extra, b'Fixture bytes')
        return target.getvalue()

    def write_receipt(self):
        (self.build_dir / 'build-receipt.json').write_text(json.dumps(self.receipt))

    def prepare(self, output='release'):
        with mock.patch.object(release, 'ROOT', self.root):
            return release.prepare(self.build_dir, 'vfixture.1', 'example/minecraft-metal-mac', self.root / output)

    def test_pack_downloads_only_pinned_mods(self):
        pack = self.prepare()
        audit.audit_pack(pack)
        with zipfile.ZipFile(pack) as z:
            self.assertEqual(set(z.namelist()), {'modrinth.index.json', 'overrides/config/mcopt.properties'})
            index = json.loads(z.read('modrinth.index.json'))
        self.assertEqual(len(index['files']), 5)
        self.assertEqual(index['dependencies'], {'minecraft': '26.3', 'fabric-loader': '0.19.5'})
        sodium = next(x for x in index['files'] if x['path'].startswith('mods/sodium-'))
        self.assertEqual(sodium['downloads'], [lib.lock()['sodium']['url']])
        self.assertEqual(sodium['hashes']['sha512'], lib.lock()['sodium']['sha512'])

    def test_release_is_deterministic(self):
        self.assertEqual(self.prepare('one').read_bytes(), self.prepare('two').read_bytes())

    def test_pack_audit_rejects_protocol_hash_and_domain_mutations(self):
        pack = self.prepare()
        with zipfile.ZipFile(pack) as z: original = json.loads(z.read('modrinth.index.json'))
        def wrong_pin(index):
            next(x for x in index['files'] if x['path'].startswith('mods/sodium-'))['hashes']['sha512'] = '0' * 128
        mutations = [lambda x: x.update(formatVersion=999), lambda x: x.update(game='other'),
                     lambda x: x['files'][1].update(path=x['files'][0]['path']),
                     lambda x: x['files'][0].update(downloads=['https://example.invalid/unrelated.jar']),
                     lambda x: x['files'][0].pop('fileSize'),
                     lambda x: x['files'][0].update(env={'client': 'unsupported', 'server': 'required'}), wrong_pin]
        for i, mutate in enumerate(mutations):
            with self.subTest(mutation=i):
                index = json.loads(json.dumps(original)); mutate(index)
                broken = self.root / f'mutated-{i}.mrpack'
                with zipfile.ZipFile(broken, 'w') as z: lib.zip_write(z, 'modrinth.index.json', json.dumps(index).encode())
                with self.assertRaises(ValueError): audit.audit_pack(broken)

    def test_every_receipt_digest_is_verified(self):
        self.receipt['artifacts'][0]['sha512'] = '0' * 128; self.write_receipt()
        with self.assertRaisesRegex(ValueError, 'changed'): self.prepare()
        self.assertFalse((self.root / 'release').exists())

    def test_receipt_size_is_verified(self):
        self.receipt['artifacts'][0]['size'] += 1; self.write_receipt()
        with self.assertRaisesRegex(ValueError, 'changed'): self.prepare()

    def test_artifact_path_cannot_escape(self):
        self.receipt['artifacts'][0]['name'] = '../other.jar'; self.write_receipt()
        with self.assertRaisesRegex(ValueError, 'Unsafe'): self.prepare()

    def test_game_dependency_bytes_are_rejected(self):
        target = self.root / 'bad.jar'
        for name in ['net/minecraft/Client.class', 'net/caffeinemc/mods/sodium/Quad.class', 'net/fabricmc/api/ModInitializer.class']:
            with self.subTest(name=name):
                target.write_bytes(self.jar('roundedterrain', name))
                with self.assertRaisesRegex(ValueError, 'Forbidden'): release.check_jar(target)

    def test_missing_license_is_rejected(self):
        target = self.root / 'bad.jar'; target.write_bytes(self.jar('roundedterrain', license=False))
        with self.assertRaisesRegex(ValueError, 'LICENSE'): release.check_jar(target)

    def test_package_preserves_notices_and_upstream_authors(self):
        classes = self.root / 'classes'; classes.mkdir()
        resources = self.root / 'resources'; resources.mkdir()
        (resources / 'fabric.mod.json').write_text(json.dumps({'id': 'mcopt-metal', 'version': '${version}', 'authors': ['Upstream author'], 'environment': 'client'}))
        native = self.root / 'owned.dylib'; native.write_bytes(b'Owned native fixture')
        licenses = [('LICENSE', self.real_root / 'LICENSES/upstream/LICENSE'), ('NOTICE', self.real_root / 'LICENSES/upstream/NOTICE')]
        licenses += [('LICENSES/' + p.name, p) for p in (self.real_root / 'LICENSES/upstream/LICENSES').iterdir() if p.is_file()]
        target = self.root / 'owned.jar'
        build.package(classes, resources, licenses, target, 'fixture.2', native)
        self.assertEqual(release.check_jar(target), 'mcopt-metal')
        with zipfile.ZipFile(target) as z:
            meta = json.loads(z.read('fabric.mod.json'))
            self.assertEqual(meta['authors'], ['Upstream author'])
            self.assertEqual(meta['version'], 'fixture.2')
            self.assertEqual(meta['contact']['sources'], 'https://github.com/' + lib.lock()['repository'])
            self.assertEqual(z.read('NOTICE'), (self.real_root / 'LICENSES/upstream/NOTICE').read_bytes())
            self.assertIn('LICENSES/MIT-MJP.txt', z.namelist())
            self.assertIn('LICENSES/MIT-SE-Natural-Bloom.txt', z.namelist())
            self.assertIn(b'Main-Class: mcopt.metal.InstallHelp', z.read('META-INF/MANIFEST.MF'))

    def test_snapshot_import_includes_untracked_sources_only(self):
        repo = self.root / 'upstream'; repo.mkdir()
        def git(*args): return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.DEVNULL)
        git('init', '-q'); git('config', 'user.name', 'Fixture'); git('config', 'user.email', 'fixture@example.invalid')
        base_java = repo / 'metal/src/main/java/Example.java'; base_java.parent.mkdir(parents=True)
        base_java.write_text('class Example {}\n'); git('add', '.'); git('commit', '-qm', 'fixture baseline')
        commit = git('rev-parse', 'HEAD').decode().strip()
        base_java.write_text('class Example { int changed; }\n')
        header = repo / 'metal/src/main/native/mcpresent_slots.h'; header.parent.mkdir(parents=True); header.write_text('int available(void);\n')
        material = repo / 'metal/src/main/java/ShadeMaterials.java'; material.write_text('class ShadeMaterials {}\n')
        (repo / 'metal/src/main/native/private.png').write_bytes(b'Private excluded fixture')
        tool = repo / 'tools/check.c'; tool.parent.mkdir(); tool.write_text('int main(void) {return 0;}\n')
        pinned = {'upstream': {'commit': commit}}
        with mock.patch.object(import_source, 'lock', return_value=pinned):
            changed, snapshot = import_source.import_renderer(repo)
            with mock.patch.object(import_source, 'ROOT', self.root / 'public'):
                import_source.write_snapshot(changed, snapshot)
        self.assertEqual(len(changed), 3)
        self.assertIn('patches/renderer/added/metal/src/main/native/mcpresent_slots.h', snapshot)
        self.assertTrue(snapshot['patches/renderer/changes.patch'])
        self.assertFalse(any('private.png' in p or 'tools/check' in p for p in snapshot))
        public = self.root / 'public'
        with mock.patch.object(build, 'ROOT', public), mock.patch.object(build, 'lock', return_value=pinned):
            build.verify_snapshot()
            (public / 'patches/renderer/changes.patch').write_text('tampered')
            with self.assertRaisesRegex(ValueError, 'changed'): build.verify_snapshot()

    def test_reimport_cannot_partially_overwrite(self):
        target = self.root / 'public'; existing = target / 'mods/rounded-terrain/Keep.java'; existing.parent.mkdir(parents=True); existing.write_text('keep')
        with mock.patch.object(import_source, 'ROOT', target):
            with self.assertRaisesRegex(ValueError, 'already present'):
                import_source.write_snapshot([], {'patches/renderer/changes.patch': b'new'})
        self.assertFalse((target / 'patches').exists())
        self.assertEqual(existing.read_text(), 'keep')

    def test_bad_download_keeps_prior_verified_cache(self):
        target = self.root / 'cache.file'; target.write_bytes(b'prior')
        with mock.patch.object(lib.urllib.request, 'urlopen', return_value=io.BytesIO(b'wrong')):
            with self.assertRaisesRegex(ValueError, 'digest mismatch'):
                lib.download('https://example.invalid/file', target, {'sha256': lib.digest(b'expected')})
        self.assertEqual(target.read_bytes(), b'prior')

    def test_archive_paths(self):
        for path in ['../escape', '/absolute', 'C:/absolute', 'dir\\escape', '']:
            with self.subTest(path=path), self.assertRaises(ValueError): lib.safe_name(path)

    def test_private_and_diagnostic_content_is_rejected(self):
        public = self.root / 'public'; public.mkdir()
        (public / 'notes.txt').write_text('/Users' + '/' + 'example/private')
        with self.assertRaisesRegex(ValueError, 'Personal'): audit.audit(public)
        (public / 'notes.txt').unlink()
        preset = public / 'presets/x.properties'; preset.parent.mkdir(); preset.write_text('mcopt.rec=true')
        with self.assertRaisesRegex(ValueError, 'Diagnostic'): audit.audit(public)

if __name__ == '__main__': unittest.main(verbosity=2)
