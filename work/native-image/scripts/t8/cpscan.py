# cpscan.py <cpfile> <outjson>: parse every class on the classpath (dirs + jars, first entry wins, as native-image sees it)
# -> name, super, interfaces, access, member refs, class/string constants, declared fields + methods. Used by gen-reflect-extra.py.
import sys, os, zipfile, struct, json
def parse(b):
    if b[:4]!=b'\xca\xfe\xba\xbe': return None
    n=struct.unpack('>H',b[8:10])[0]; i=10; cp=[None]*n; k=1
    while k<n:
        t=b[i]
        if t==1:
            l=struct.unpack('>H',b[i+1:i+3])[0]; cp[k]=('u',b[i+3:i+3+l].decode('utf8','replace')); i+=3+l
        elif t in (3,4): cp[k]=('i',); i+=5
        elif t in (5,6): cp[k]=('l',); i+=9; k+=1
        elif t==7: cp[k]=('c',struct.unpack('>H',b[i+1:i+3])[0]); i+=3
        elif t==8: cp[k]=('s',struct.unpack('>H',b[i+1:i+3])[0]); i+=3
        elif t in (9,10,11): cp[k]=('r',t,)+struct.unpack('>HH',b[i+1:i+5]); i+=5
        elif t==12: cp[k]=('nt',)+struct.unpack('>HH',b[i+1:i+5]); i+=5
        elif t==15: cp[k]=('mh',); i+=4
        elif t==16: cp[k]=('mt',); i+=3
        elif t in (17,18): cp[k]=('dy',); i+=5
        elif t in (19,20): cp[k]=('mod',); i+=3
        else: return None
        k+=1
    U=lambda x: cp[x][1]
    def members(i):
        n=struct.unpack('>H',b[i:i+2])[0]; i+=2; out=[]
        for _ in range(n):
            a,ni_,di,ac=struct.unpack('>HHHH',b[i:i+8]); i+=8
            for _ in range(ac):
                l=struct.unpack('>I',b[i+2:i+6])[0]; i+=6+l
            out.append((U(ni_),U(di),a))
        return i,out
    acc,this,sup=struct.unpack('>HHH',b[i:i+6]); i+=6
    ni=struct.unpack('>H',b[i:i+2])[0]; i+=2
    ifs=[struct.unpack('>H',b[i+2*j:i+2*j+2])[0] for j in range(ni)]; i+=2*ni
    i,flds=members(i); i,mths=members(i)
    C=lambda x: U(cp[x][1]) if x else None
    refs=set(); classes=set(); strings=set()
    for e in cp:
        if not e: continue
        if e[0]=='r':
            owner=C(e[2]); nt=cp[e[3]]; refs.add(owner+'.'+U(nt[1]))
        elif e[0]=='c': classes.add(U(e[1]))
        elif e[0]=='s': strings.add(U(e[1]))
    gson=any(e and e[0]=='u' and 'Lcom/google/gson/annotations/' in e[1] for e in cp)
    return dict(name=C(this),sup=C(sup),ifs=[C(x) for x in ifs],acc=acc,refs=sorted(refs),classes=sorted(classes),strings=sorted(strings),fields=flds,methods=mths,gson=gson)
out={}
def add(src,b):
    d=parse(b)
    if d and d['name'] not in out: d['src']=src; out[d['name']]=d
for p in open(sys.argv[1]).read().strip().split(':'):
    if os.path.isdir(p):
        for r,_,fs in os.walk(p):
            for f in fs:
                if f.endswith('.class'): add(p,open(os.path.join(r,f),'rb').read())
    elif p.endswith('.jar') and os.path.exists(p):
        try: z=zipfile.ZipFile(p)
        except Exception: continue
        for n in z.namelist():
            if n.endswith('.class') and not n.startswith('META-INF/'): add(p,z.read(n))
json.dump(out,open(sys.argv[2],'w'))
print(len(out))
