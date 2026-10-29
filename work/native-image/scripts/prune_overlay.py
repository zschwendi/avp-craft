# usage: python3 -I prune_overlay.py <overlay-dir> <lib.jar>...  -- delete overlay classes byte-identical to a
# library jar's copy (Knot pass-through); keep library classes that Mixin/AW actually changed. Drops $Proxy/jdk.
import sys, os, zipfile
ov = sys.argv[1]; same = changed = 0; kept = []
lib = {}
for j in sys.argv[2:]:
    with zipfile.ZipFile(j) as z:
        for n in z.namelist():
            if n.endswith('.class') and n not in lib: lib[n] = (j, n)
cache = {}
for root, _, files in os.walk(ov):
    for f in files:
        p = os.path.join(root, f); rel = os.path.relpath(p, ov)
        if rel.startswith('jdk/') or '$Proxy' in f: os.remove(p); continue
        if rel in lib:
            j, n = lib[rel]
            z = cache.get(j) or cache.setdefault(j, zipfile.ZipFile(j))
            if z.read(n) == open(p, 'rb').read(): os.remove(p); same += 1
            else: changed += 1; kept.append(rel)
for root, dirs, _ in os.walk(ov, topdown=False):
    for d in dirs:
        dp = os.path.join(root, d)
        if not os.listdir(dp): os.rmdir(dp)
print("removed identical lib copies:", same, " kept transformed lib classes:", changed)
open(os.path.join(os.path.dirname(ov.rstrip('/')), 'transformed-lib-classes.txt'), 'w').write('\n'.join(sorted(kept)))
