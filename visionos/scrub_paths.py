#!/usr/bin/env python3
"""Rewrite a build-machine home prefix inside Mach-O copies.

Compile paths are absolute. A same-length replacement keeps every file
offset valid, so a signed binary can be re-signed afterwards. The
replacement is /vendor/angle (13 bytes). This is a no-op when HOME is
any other length. It edits only the tree it is pointed at. The private
build inputs stay as they are.
"""
import os
import sys

REPL = b"/vendor/angle"


def main() -> None:
    home = os.environ.get("HOME", "")
    old = home.encode()
    if not old or len(old) != len(REPL):
        print(f"scrub_paths: skipped (home length {len(old)})")
        return
    root = sys.argv[1]
    nfiles = 0
    nhits = 0
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for name in filenames:
            path = os.path.join(dirpath, name)
            if os.path.islink(path):
                continue
            try:
                data = open(path, "rb").read()
            except OSError:
                continue
            count = data.count(old)
            if not count:
                continue
            with open(path, "wb") as out:
                out.write(data.replace(old, REPL))
            nfiles += 1
            nhits += count
    print(f"scrub_paths: {nhits} paths in {nfiles} files")


if __name__ == "__main__":
    main()
