#!/usr/bin/env python3
"""In-place rewrite of LC_BUILD_VERSION (platform/minos/sdk) in thin arm64 Mach-O object files.
vtool refuses on these objects ("not enough space to hold load commands": it wants to append a
tool entry), but the platform field is a fixed 4-byte slot, so patching it in place is exact.
Objects with no LC_BUILD_VERSION / with LC_VERSION_MIN_* are reported, not touched.
Usage: patch_buildver.py <platform-int> <minos e.g. 26.0> <sdk e.g. 27.0> files..."""
import struct, sys
LC_BUILD_VERSION = 0x32
VERMIN = {0x24, 0x25, 0x2F, 0x30}
def ver(s):
    p = [int(x) for x in s.split('.')] + [0, 0]
    return (p[0] << 16) | (p[1] << 8) | p[2]
plat, minos, sdk = int(sys.argv[1]), ver(sys.argv[2]), ver(sys.argv[3])
stats = {"patched": 0, "none": 0, "vermin": 0}
for f in sys.argv[4:]:
    b = bytearray(open(f, 'rb').read())
    magic, cpu, sub, ft, ncmds, sz, flags, res = struct.unpack_from('<IiiIIIII', b, 0)
    assert magic == 0xfeedfacf, f
    off, hit = 32, False
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', b, off)
        if cmd == LC_BUILD_VERSION:
            struct.pack_into('<III', b, off + 8, plat, minos, sdk); hit = True
        elif cmd in VERMIN:
            stats["vermin"] += 1; print("VERSION_MIN in", f)
        off += cs
    if hit:
        open(f, 'wb').write(b); stats["patched"] += 1
    else:
        stats["none"] += 1
print(stats)
