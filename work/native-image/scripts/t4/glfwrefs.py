# glfwrefs.py <classpath-file> [owner-regex] : every Methodref/Fieldref whose owner matches (default org/lwjgl/glfw/)
# made by classes OUTSIDE org/lwjgl/glfw, as "Owner.name:desc<TAB>count<TAB>first-user". Earlier cp entries shadow later ones.
import re, sys, zipfile, os, struct
cp = open(sys.argv[1]).read().strip().split(':')
rx = re.compile(sys.argv[2] if len(sys.argv) > 2 else r'^org/lwjgl/glfw/')
def classes(e):
    if os.path.isdir(e):
        for d, _, fs in os.walk(e):
            for f in fs:
                if f.endswith('.class'): yield os.path.relpath(os.path.join(d, f), e), open(os.path.join(d, f), 'rb').read()
    elif e.endswith('.jar') and os.path.exists(e):
        z = zipfile.ZipFile(e)
        for n in z.namelist():
            if n.endswith('.class') and not n.startswith('META-INF/'): yield n, z.read(n)
def refs(b):
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
    for e in c:
        if e and e[0] == 'r':
            ci, nti = e[1]; owner = c[c[ci][1]][1]; nn, dd = c[nti][1]
            yield owner, c[nn][1], c[dd][1]
seen = set(); out = {}
for e in cp:
    for name, b in classes(e):
        if name in seen: continue
        seen.add(name)
        if name.startswith('org/lwjgl/glfw/'): continue
        try:
            for o, nm, d in refs(b):
                if rx.search(o):
                    key = o + '.' + nm + ':' + d
                    if key not in out: out[key] = [0, name]
                    out[key][0] += 1
        except Exception as ex: print('ERR', e, name, ex, file=sys.stderr)
for k in sorted(out): print(k + '\t' + str(out[k][0]) + '\t' + out[k][1])
