# glfwcover.py <refs.txt> <jar-or-dir> [<lib.jar> ...]
# For every "Owner.name:desc" line of a glfwrefs.py listing, check that the member resolves in the stand-in jar
# (following superclasses into the extra library jars, e.g. lwjgl core for CustomBuffer.position). Then check that every
# org/lwjgl/glfw/* member referenced by the stand-in jar's OWN classes resolves too (catches stock struct/callback
# classes that still point at the removed GLFW$Functions etc.). Prints misses; exit 1 if any.
import sys, zipfile, os, struct
def classes(e):
    if os.path.isdir(e):
        for d, _, fs in os.walk(e):
            for f in fs:
                if f.endswith('.class'): yield os.path.relpath(os.path.join(d, f), e)[:-6], open(os.path.join(d, f), 'rb').read()
    else:
        z = zipfile.ZipFile(e)
        for n in z.namelist():
            if n.endswith('.class') and not n.startswith('META-INF/'): yield n[:-6], z.read(n)
def parse(b):
    n = struct.unpack('>H', b[8:10])[0]; c = [None] * n; i = 10; k = 1
    while k < n:
        t = b[i]
        if t == 1: l = struct.unpack('>H', b[i+1:i+3])[0]; c[k] = ('u', b[i+3:i+3+l].decode('utf8', 'replace')); i += 3 + l
        elif t in (3, 4): i += 5
        elif t in (5, 6): i += 9; k += 1
        elif t == 7: c[k] = ('c', struct.unpack('>H', b[i+1:i+3])[0]); i += 3
        elif t in (8, 16, 19, 20): i += 3
        elif t in (9, 10, 11): c[k] = ('r', struct.unpack('>HH', b[i+1:i+5])); i += 5
        elif t == 12: c[k] = ('nt', struct.unpack('>HH', b[i+1:i+5])); i += 5
        elif t == 15: i += 4
        elif t in (17, 18): i += 5
        else: raise ValueError(t)
        k += 1
    u = lambda x: c[x][1]
    i += 2; this = u(c[struct.unpack('>H', b[i:i+2])[0]][1]); i += 2
    si = struct.unpack('>H', b[i:i+2])[0]; sup = u(c[si][1]) if si else None; i += 2
    ni = struct.unpack('>H', b[i:i+2])[0]; i += 2
    itf = [u(c[struct.unpack('>H', b[i+2*j:i+2*j+2])[0]][1]) for j in range(ni)]; i += 2 * ni
    members = set()
    for _ in range(2):  # fields, methods
        cnt = struct.unpack('>H', b[i:i+2])[0]; i += 2
        for _ in range(cnt):
            _, nm, ds, ac = struct.unpack('>HHHH', b[i:i+8]); i += 8
            members.add(u(nm) + ':' + u(ds))
            for _ in range(ac):
                l = struct.unpack('>I', b[i+2:i+6])[0]; i += 6 + l
    refs = []
    for e in c:
        if e and e[0] == 'r':
            ci, nti = e[1]; nn, dd = c[nti][1]
            refs.append((u(c[ci][1]), u(nn), u(dd)))
    return this, sup, itf, members, refs
db = {}; own = {}
for idx, e in enumerate(sys.argv[2:]):
    for name, b in classes(e):
        if name in db: continue
        db[name] = parse(b)
        if idx == 0: own[name] = db[name]
def resolves(owner, nm, ds, seen=None):
    seen = seen or set()
    if owner in seen: return False
    seen.add(owner)
    if owner.startswith('java/'): return True  # JDK supertypes (Object methods etc.)
    if owner not in db: return False
    _, sup, itf, mem, _ = db[owner]
    if nm + ':' + ds in mem: return True
    return any(resolves(x, nm, ds, seen) for x in ([sup] if sup else []) + itf)
miss = 0; n = 0
for line in open(sys.argv[1]):
    key = line.split('\t')[0].strip()
    if not key: continue
    owner, rest = key.split('.', 1); nm, ds = rest.split(':', 1); n += 1
    if not resolves(owner, nm, ds): print('MISSING (game ref):', key, '  <-', line.split('\t')[2].strip()); miss += 1
m2 = 0
for cname, (_, _, _, _, refs) in own.items():
    for o, nm, ds in refs:
        if o.startswith('org/lwjgl/glfw/') and not resolves(o, nm, ds):
            print('MISSING (inside jar):', o + '.' + nm + ':' + ds, '  <-', cname); m2 += 1
print(f'glfwcover: {n} game refs, {miss} missing; {len(own)} jar classes, {m2} dangling internal glfw refs')
sys.exit(1 if miss or m2 else 0)
