# usage: python3 -I nbt_strip_packs.py level.dat  -- keep only "vanilla" in Data.DataPacks.Enabled, WasModded=0
import gzip, struct, sys
def rd(b,i,t):
    if t==1: return b[i:i+1],i+1
    if t==2: return b[i:i+2],i+2
    if t==3: return b[i:i+4],i+4
    if t==4: return b[i:i+8],i+8
    if t==5: return b[i:i+4],i+4
    if t==6: return b[i:i+8],i+8
    if t==7: n=struct.unpack('>i',b[i:i+4])[0]; return b[i:i+4+n],i+4+n
    if t==8: n=struct.unpack('>H',b[i:i+2])[0]; return b[i+2:i+2+n].decode('utf-8','surrogatepass'),i+2+n
    if t==9:
        et=b[i]; n=struct.unpack('>i',b[i+1:i+5])[0]; i+=5; L=[]
        for _ in range(n): v,i=rd(b,i,et); L.append(v)
        return (et,L),i
    if t==10:
        D=[]
        while True:
            tt=b[i]; i+=1
            if tt==0: return D,i
            n=struct.unpack('>H',b[i:i+2])[0]; name=b[i+2:i+2+n].decode(); i+=2+n
            v,i=rd(b,i,tt); D.append([tt,name,v])
    if t==11: n=struct.unpack('>i',b[i:i+4])[0]; return b[i:i+4+4*n],i+4+4*n
    if t==12: n=struct.unpack('>i',b[i:i+4])[0]; return b[i:i+4+8*n],i+4+8*n
    raise Exception(t)
def wr(t,v):
    if t in (1,2,3,4,5,6,7,11,12): return v
    if t==8: e=v.encode('utf-8','surrogatepass'); return struct.pack('>H',len(e))+e
    if t==9: et,L=v; return bytes([et])+struct.pack('>i',len(L))+b''.join(wr(et,x) for x in L)
    if t==10: return b''.join(bytes([tt])+wr(8,n)+wr(tt,x) for tt,n,x in v)+b'\0'
b=gzip.decompress(open(sys.argv[1],'rb').read())
assert b[0]==10
n=struct.unpack('>H',b[1:3])[0]; root,_=rd(b,3+n,10)
data=[x for x in root if x[1]=='Data'][0][2]
for e in data:
    if e[1]=='DataPacks':
        for f in e[2]:
            if f[1]=='Enabled': f[2]=(8,[s for s in f[2][1] if s=='vanilla']); print('Enabled ->',f[2][1])
    if e[1]=='WasModded': e[2]=b'\0'
    if e[1]=='ServerBrands': e[2]=(8,['vanilla'])
open(sys.argv[1],'wb').write(gzip.compress(b[:3+n]+wr(10,root)))
