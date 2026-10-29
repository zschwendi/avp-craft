import contextlib
import hashlib
import importlib.util
import io
import os
import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path
from unittest import mock

_spec = importlib.util.spec_from_file_location(
    "canon", Path(__file__).with_name("canonicalize-vivecraft-jar.py"))
canon = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(canon)


def nested_jar():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(zipfile.ZipInfo("inner.class", (2024, 5, 6, 7, 8, 10)), b"\xca\xfe\xba\xbe")
    return buf.getvalue()


ENTRIES = {
    "META-INF/": b"",
    "META-INF/jars/": b"",
    "META-INF/jars/lib-1.0.jar": nested_jar(),
    "META-INF/LICENSE": b"license text\n",
    "META-INF/FAKE.SF": b"Signature-Version: 1.0\n",
    "org/vivecraft/A.class": b"\xca\xfe\xba\xbe" + bytes(range(256)) * 4,
}


def make_zip(path, names, date, compression, extra=b"", comment=b"", mode=0o644):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")  # duplicate-name fixtures warn
        with zipfile.ZipFile(path, "w") as zf:
            zf.comment = comment
            for name in names:
                info = zipfile.ZipInfo(name, date)
                info.compress_type = compression
                info.extra, info.comment = extra, comment
                info.external_attr = ((0o40000 | 0o777) if name.endswith("/") else (0o100000 | mode)) << 16
                zf.writestr(info, ENTRIES.get(name, b"dup"))


def read_all(path):
    with zipfile.ZipFile(path) as zf:
        return {i.filename: zf.read(i) for i in zf.infolist()}


class CanonicalizeTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.a, self.b = self.dir / "a.jar", self.dir / "b.jar"
        make_zip(self.a, list(ENTRIES), (2026, 10, 1, 12, 0, 0), zipfile.ZIP_DEFLATED,
                 extra=b"\xfe\xca\x02\x00hi", comment=b"run2", mode=0o755)
        make_zip(self.b, list(reversed(ENTRIES)), (2026, 10, 2, 9, 30, 0), zipfile.ZIP_STORED,
                 comment=b"run3")

    def tearDown(self):
        self._tmp.cleanup()

    def assertRefused(self, src, out):
        before = src.read_bytes()
        with self.assertRaises(canon.NormalizeError):
            canon.normalize(src, out)
        self.assertEqual(src.read_bytes(), before)
        self.assertEqual([p for p in os.listdir(self.dir) if p.endswith(".tmp")], [])

    def test_differing_metadata_gives_identical_output(self):
        self.assertNotEqual(self.a.read_bytes(), self.b.read_bytes())
        sha_a, size_a = canon.normalize(self.a, self.dir / "out-a.jar")
        sha_b, _ = canon.normalize(self.b, self.dir / "out-b.jar")
        out = (self.dir / "out-a.jar").read_bytes()
        self.assertEqual(out, (self.dir / "out-b.jar").read_bytes())
        self.assertEqual((sha_a, size_a), (hashlib.sha256(out).hexdigest(), len(out)))
        self.assertEqual(sha_a, sha_b)
        self.assertEqual(read_all(self.dir / "out-a.jar"), ENTRIES)
        self.assertEqual(sorted(os.listdir(self.dir)), ["a.jar", "b.jar", "out-a.jar", "out-b.jar"])

    def test_renormalizing_is_byte_identical(self):
        canon.normalize(self.a, self.dir / "once.jar")
        canon.normalize(self.dir / "once.jar", self.dir / "twice.jar")
        self.assertEqual((self.dir / "once.jar").read_bytes(), (self.dir / "twice.jar").read_bytes())

    def test_duplicate_names_refused(self):
        dup = self.dir / "dup.jar"
        make_zip(dup, ["x.txt", "x.txt"], (2026, 1, 1, 0, 0, 0), zipfile.ZIP_STORED)
        self.assertRefused(dup, self.dir / "out.jar")
        self.assertFalse((self.dir / "out.jar").exists())

    def test_crc_corruption_refused(self):
        data = bytearray(self.b.read_bytes())  # stored, so payload bytes are visible
        data[data.index(b"license text")] ^= 0xFF
        bad = self.dir / "bad.jar"
        bad.write_bytes(bytes(data))
        self.assertRefused(bad, self.dir / "out.jar")
        self.assertFalse((self.dir / "out.jar").exists())

    def test_existing_or_same_output_rejected(self):
        existing = self.dir / "existing.jar"
        existing.write_bytes(b"keep")
        self.assertRefused(self.a, existing)
        self.assertEqual(existing.read_bytes(), b"keep")
        self.assertRefused(self.a, self.a)

    def test_bounds_enforced(self):
        for name, value in [("MAX_INPUT_BYTES", 100), ("MAX_EXPANDED_BYTES", 100), ("MAX_ENTRIES", 3)]:
            with self.subTest(name), mock.patch.object(canon, name, value):
                self.assertRefused(self.a, self.dir / "out.jar")
                self.assertFalse((self.dir / "out.jar").exists())

    def test_cli_prints_pin_and_fails_nonzero(self):
        out = self.dir / "cli.jar"
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            self.assertEqual(canon.main(["prog", str(self.a), str(out)]), 0)
        self.assertIn(hashlib.sha256(out.read_bytes()).hexdigest(), stdout.getvalue())
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(canon.main(["prog", str(self.a), str(out)]), 1)
            self.assertEqual(canon.main(["prog"]), 2)


if __name__ == "__main__":
    unittest.main()
