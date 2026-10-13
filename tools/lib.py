"""Shared deterministic downloads and archive helpers. Python standard library only."""
from pathlib import Path, PurePosixPath
import hashlib, json, os, re, urllib.request, zipfile

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache'

def lock():
    return json.loads((ROOT / 'dependencies.lock.json').read_text())

def digest(data, algorithm='sha256'):
    return hashlib.new(algorithm, data).hexdigest()

def safe_name(name):
    p = PurePosixPath(name)
    if not name or p.is_absolute() or '..' in p.parts or '\\' in name or re.match(r'^[A-Za-z]:', name):
        raise ValueError('Unsafe archive path: ' + name)
    return p

def download(url, path, hashes, size=None):
    if not url.startswith('https://') or not hashes:
        raise ValueError('HTTPS and a pinned digest are required')
    path = Path(path)
    if path.is_file():
        data = path.read_bytes()
        if all(digest(data, a) == h for a, h in hashes.items()) and (size is None or len(data) == size):
            return path
    req = urllib.request.Request(url, headers={'User-Agent': 'minecraft-metal-mac-build/1.0'})
    with urllib.request.urlopen(req, timeout=90) as response:
        data = response.read(400_000_001)
    if len(data) > 400_000_000 or (size is not None and len(data) != size):
        raise ValueError('Unexpected download size: ' + url)
    for algorithm, expected in hashes.items():
        if digest(data, algorithm) != expected:
            raise ValueError('Download digest mismatch: ' + url)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.partial')
    tmp.write_bytes(data)
    os.replace(tmp, path)
    return path

def zip_write(z, name, data):
    safe_name(name)
    info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)
