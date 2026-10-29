#!/usr/bin/env python3
"""Fetch pinned ANGLE/Vulkan headers needed alongside the prebuilt frameworks.

Python 3.9+, standard library only. Existing identical headers are retained;
conflicts and links are refused. No archive permissions or ownership are applied.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import tarfile
import tempfile
import zlib
from urllib.request import urlopen


MAX_ARCHIVE_BYTES = 16 * 1024 * 1024
MAX_TAR_BYTES = 64 * 1024 * 1024
MAX_HEADER_BYTES = 32 * 1024 * 1024

# Gitiles tar/PAX mtimes change on every request. Pin the full sorted inventory
# of [normalized path, byte length, SHA256(file bytes)], encoded as compact ASCII
# JSON. Adding/removing/renaming a file or changing any byte fails verification.
SOURCES = (
    {
        "name": "ANGLE",
        "archive": "angle-include-25e721127e1c6c4c6fa0182b5c234b2c88971175.tar.gz",
        "url": "https://chromium.googlesource.com/angle/angle/+archive/25e721127e1c6c4c6fa0182b5c234b2c88971175/include.tar.gz",
        "content_sha256": "a06fb0b74c48f275a9705a10fdc78ad30b91a92a0ad85f36750e64b517c0018c",
        "output": "vendor/include",
    },
    {
        "name": "Vulkan-Headers",
        "archive": "vulkan-headers-include-11d6898377797e07dbd543aaaa367e4465074597.tar.gz",
        "url": "https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Headers/+archive/11d6898377797e07dbd543aaaa367e4465074597/include.tar.gz",
        "content_sha256": "4e2a7e2e74420f6a8923a6ae21dedddfb3106a483e423208117790ff13b9d62d",
        "output": "vendor/third_party/vulkan-headers/src/include",
    },
)


class HeaderError(Exception):
    pass


def lstat_or_none(path):
    try:
        return path.lstat()
    except FileNotFoundError:
        return None


def check_directory(path):
    info = lstat_or_none(path)
    if info is not None and not stat.S_ISDIR(info.st_mode):
        raise HeaderError("directory conflict or symlink: {}".format(path))


def checked_directories(root, path):
    """Yield every directory from the supplied root, refusing existing links."""
    path.relative_to(root)
    current = root
    check_directory(current)
    yield current
    for part in path.relative_to(root).parts:
        current = current / part
        check_directory(current)
        yield current


def ensure_directories(root, path):
    for directory in checked_directories(root, path):
        try:
            directory.mkdir()
        except FileExistsError:
            check_directory(directory)


def regular_bytes(path, limit=MAX_HEADER_BYTES):
    info = lstat_or_none(path)
    if info is None:
        return None
    if not stat.S_ISREG(info.st_mode):
        raise HeaderError("file conflict or symlink: {}".format(path))
    # Refuse a symlink introduced between lstat and open as well.
    fd = os.open(str(path), os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise HeaderError("not a regular file: {}".format(path))
        if info.st_size > limit:
            raise HeaderError("existing file exceeds size limit: {}".format(path))
        data = stream.read(limit + 1)
        if len(data) > limit:
            raise HeaderError("existing file exceeds size limit: {}".format(path))
        return data


def publish_new(path, data):
    """Publish a fully written file without replacing an existing destination."""
    fd, temporary = tempfile.mkstemp(prefix=".avpcraft-headers-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        try:
            os.link(temporary, path)
            return True
        except FileExistsError:
            if regular_bytes(path) != data:
                raise HeaderError("existing file differs; left unchanged: {}".format(path))
            return False
    finally:
        os.unlink(temporary)


def content_sha256(files):
    entries = sorted((str(name), len(data), hashlib.sha256(data).hexdigest())
                     for name, data in files.items())
    canonical = json.dumps(entries, ensure_ascii=True, separators=(",", ":")).encode("ascii")
    return hashlib.sha256(canonical).hexdigest()


def fetch_archive(root, cache, source):
    ensure_directories(root, cache)
    path = cache / source["archive"]
    data = regular_bytes(path, MAX_ARCHIVE_BYTES)
    if data is None:
        print("Downloading {} headers".format(source["name"]), flush=True)
        with urlopen(source["url"], timeout=60) as response:
            data = response.read(MAX_ARCHIVE_BYTES + 1)
        if len(data) > MAX_ARCHIVE_BYTES:
            raise HeaderError("archive exceeds download limit: {}".format(source["name"]))
    files, directories = read_headers(data)
    actual = content_sha256(files)
    if actual != source["content_sha256"]:
        raise HeaderError("header content SHA-256 mismatch for {}; cache left unchanged".format(path))
    publish_new(path, data)
    return files, directories


def read_headers(data):
    """Bound decompression before tar parsing, including PAX/longname metadata."""
    if len(data) > MAX_ARCHIVE_BYTES:
        raise HeaderError("archive exceeds size limit")
    with gzip.GzipFile(fileobj=io.BytesIO(data)) as compressed:
        unpacked = compressed.read(MAX_TAR_BYTES + 1)
    if len(unpacked) > MAX_TAR_BYTES:
        raise HeaderError("decompressed archive exceeds size limit")
    files = {}
    directories = set()
    total = 0
    # Never call extract/extractall or apply archive permissions/ownership.
    with tarfile.open(fileobj=io.BytesIO(unpacked), mode="r:") as bundle:
        for member in bundle:
            raw = member.name
            name = PurePosixPath(raw)
            if "\\" in raw or "\x00" in raw or name.is_absolute() or ".." in name.parts:
                raise HeaderError("unsafe archive path: {!r}".format(raw))
            if not name.parts:
                if member.isdir():
                    continue
                raise HeaderError("invalid archive root file")
            if member.isdir():
                if name in files:
                    raise HeaderError("archive file/directory collision: {}".format(name))
                directories.add(name)
                continue
            if member.type not in (tarfile.REGTYPE, tarfile.AREGTYPE):
                raise HeaderError("archive links/special entries are forbidden: {}".format(name))
            if name in files or name in directories:
                raise HeaderError("duplicate archive file: {}".format(name))
            total += member.size
            if member.size < 0 or member.size > MAX_HEADER_BYTES or total > MAX_TAR_BYTES:
                raise HeaderError("header archive exceeds extraction limit")
            with bundle.extractfile(member) as stream:
                data = stream.read()
            if len(data) != member.size:
                raise HeaderError("truncated archive member: {}".format(name))
            files[name] = data
            directories.update(parent for parent in name.parents if parent.parts)
    if any(name in files for name in directories):
        raise HeaderError("archive file/directory collision")
    if not files:
        raise HeaderError("header archive contains no files")
    required_dirs = {parent for name in files for parent in name.parents if parent.parts}
    if directories - required_dirs:
        raise HeaderError("archive contains directories outside the file inventory")
    return files, required_dirs


def install(root, sources=SOURCES):
    cache = root / "work/natives/downloads"
    plans = []
    # Validate both archives and every existing destination before writing headers.
    for source in sources:
        files, directories = fetch_archive(root, cache, source)
        output = root / source["output"]
        for directory in [output] + [output / str(d) for d in directories]:
            list(checked_directories(root, directory))
        for name, data in files.items():
            destination = output / str(name)
            list(checked_directories(root, destination.parent))
            existing = regular_bytes(destination)
            if existing is not None and existing != data:
                raise HeaderError("existing header differs; left unchanged: {}".format(destination))
        plans.append((source, output, files, directories))
    for source, output, files, directories in plans:
        for directory in sorted([output] + [output / str(d) for d in directories], key=lambda p: len(p.parts)):
            ensure_directories(root, directory)
        added = 0
        for name, data in files.items():
            destination = output / str(name)
            list(checked_directories(root, destination.parent))
            added += publish_new(destination, data)
        print("{}: {} headers verified, {} added -> {}".format(source["name"], len(files), added, output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3],
                        help="repository root (default: inferred from this script)")
    args = parser.parse_args()
    root = args.root.absolute()
    try:
        if not root.is_dir() or root.is_symlink():
            raise HeaderError("repository root must be an existing ordinary directory")
        install(root)
    except (HeaderError, OSError, EOFError, tarfile.TarError, zlib.error) as error:
        print("graphics headers: {}".format(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
