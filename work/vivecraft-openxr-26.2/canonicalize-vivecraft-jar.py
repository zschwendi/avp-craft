#!/usr/bin/env python3
"""Deterministically repack a locally built (unsigned) Vivecraft JAR.

Every entry name and uncompressed payload byte is preserved, nested JARs
included. Only ZIP container metadata is canonicalized: entry order, timestamps,
extra fields, comments, modes and compression (all entries ZIP_STORED).

Usage: python3 canonicalize-vivecraft-jar.py input.jar output.jar
"""
import hashlib
import io
import os
import sys
import tempfile
import zipfile

MAX_INPUT_BYTES = 64 * 1024 * 1024
MAX_EXPANDED_BYTES = 128 * 1024 * 1024
MAX_ENTRIES = 5000
FIXED_DATE = (1980, 2, 1, 0, 0, 0)
DIR_ATTR = (0o040755 << 16) | 0x10  # drwxr-xr-x + MS-DOS directory flag
FILE_ATTR = 0o100644 << 16  # -rw-r--r--


class NormalizeError(Exception):
    pass


def read_input(path):
    """Return {name: payload} after size, count, duplicate and CRC checks."""
    with open(path, "rb") as f:
        if os.fstat(f.fileno()).st_size > MAX_INPUT_BYTES:
            raise NormalizeError(f"input exceeds {MAX_INPUT_BYTES} bytes")
        try:
            with zipfile.ZipFile(f) as zf:
                infos = zf.infolist()
                names = [i.filename for i in infos]
                if len(infos) > MAX_ENTRIES:
                    raise NormalizeError(f"{len(infos)} entries exceeds {MAX_ENTRIES}")
                if len(set(names)) != len(names):
                    raise NormalizeError("input has duplicate entry names")
                if sum(i.file_size for i in infos) > MAX_EXPANDED_BYTES:
                    raise NormalizeError(f"declared expanded size exceeds {MAX_EXPANDED_BYTES}")
                payloads, total = {}, 0
                for info in infos:
                    with zf.open(info) as member:  # reading to EOF checks the CRC
                        data = member.read(MAX_EXPANDED_BYTES - total + 1)
                    total += len(data)
                    if total > MAX_EXPANDED_BYTES:
                        raise NormalizeError(f"expanded size exceeds {MAX_EXPANDED_BYTES}")
                    if len(data) != info.file_size or (info.is_dir() and data):
                        raise NormalizeError(f"inconsistent size for {info.filename!r}")
                    payloads[info.filename] = data
        except NormalizeError:
            raise
        except Exception as e:
            raise NormalizeError(f"corrupt or unsupported input: {e}") from e
    return payloads


def write_canonical(payloads, fileobj):
    with zipfile.ZipFile(fileobj, "w", zipfile.ZIP_STORED) as zf:
        for name in sorted(payloads):
            info = zipfile.ZipInfo(name, FIXED_DATE)
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 3
            info.external_attr = DIR_ATTR if name.endswith("/") else FILE_ATTR
            zf.writestr(info, payloads[name])


def verify(data, payloads):
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        infos = zf.infolist()
        if [i.filename for i in infos] != sorted(payloads) or zf.comment:
            raise NormalizeError("verification failed: entry names/order or archive comment")
        for i in infos:
            attr = DIR_ATTR if i.filename.endswith("/") else FILE_ATTR
            if (i.date_time != FIXED_DATE or i.compress_type != zipfile.ZIP_STORED
                    or i.extra or i.comment or i.external_attr != attr
                    or zf.read(i) != payloads[i.filename]):
                raise NormalizeError(f"verification failed for {i.filename!r}")


def normalize(input_path, output_path):
    """Write canonical output_path from input_path; return (sha256, bytes)."""
    input_path, output_path = os.path.abspath(input_path), os.path.abspath(output_path)
    if input_path == output_path or os.path.lexists(output_path):
        raise NormalizeError(f"refusing existing or same output path: {output_path}")
    payloads = read_input(input_path)
    fd, tmp = tempfile.mkstemp(prefix=".canonical-", suffix=".jar.tmp",
                               dir=os.path.dirname(output_path))
    try:
        with os.fdopen(fd, "w+b") as f:
            write_canonical(payloads, f)
            f.flush()
            os.fsync(f.fileno())
            f.seek(0)
            data = f.read()
        verify(data, payloads)
        os.chmod(tmp, 0o644)
        try:
            os.link(tmp, output_path)  # exclusive publish: never overwrites
        except FileExistsError:
            raise NormalizeError(f"output appeared during work: {output_path}") from None
    finally:
        os.unlink(tmp)
    return hashlib.sha256(data).hexdigest(), len(data)


def main(argv):
    if len(argv) != 3:
        print("usage: canonicalize-vivecraft-jar.py input.jar output.jar", file=sys.stderr)
        return 2
    try:
        sha, size = normalize(argv[1], argv[2])
    except (NormalizeError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"sha256={sha} bytes={size} output={argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
