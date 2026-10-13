package local.roundedterrain;

import java.util.ArrayList;
import java.util.List;

/** Pure geometric construction, independent of Minecraft, used by the runtime hook and the topology harness. */
public final class BevelMesh {
    // 0/1 west/east, 2/3 down/up, 4/5 north/south. Adjacent faces have different axis IDs.
    public record Point(float x,float y,float z) {
        public float at(int axis) { return axis==0 ? x : axis==1 ? y : z; }
    }
    /** Normals describe the curved surface; the compact Sodium vertex ABI does not itself retain them. */
    public record Patch(Point[] points,int textureFace,String kind,Point[] normals) {
        public Patch(Point[] points,int textureFace,String kind) {
            this(points,textureFace,kind,flatNormals(points));
        }
    }
    public static final int DEFAULT_SEGMENTS=3;
    public final List<Patch> patches;
    private final int exposed;
    private final float radius;
    private final long continuing;
    public final int segments;

    /** Continuing packs two free-axis endpoint bits for each of the twelve perpendicular-face pairs. */
    public BevelMesh(int exposed,long continuing,float radius) {
        this(exposed,continuing,radius,DEFAULT_SEGMENTS);
    }
    public BevelMesh(int exposed,long continuing,float radius,int segments) {
        if (!(radius>=0 && radius<=0.125f)) throw new IllegalArgumentException("radius must be 0..0.125 blocks");
        if (segments<2 || segments>4) throw new IllegalArgumentException("segments must be 2..4");
        this.exposed=exposed;
        this.continuing=continuing;
        this.radius=radius;
        this.segments=segments;
        List<Patch> out=new ArrayList<>(6+12*segments+8*segments*segments);
        for (int face=0;face<6;face++) if (visible(face)) {
            int a=face/2,b=(a+1)%3,c=(a+2)%3;
            Point[] p=new Point[4];
            int[] uv={0,1,3,2};
            for (int i=0;i<4;i++) {
                int bits=((face&1)<<a)|((uv[i]&1)<<b)|(((uv[i]>>1)&1)<<c);
                p[i]=faceVertex(face,bits);
            }
            orient(p,normal(face));
            out.add(new Patch(p,face,"face"));
        }
        for (int a=0;a<6;a++) for (int b=a+1;b<6;b++) {
            if (a/2==b/2 || !visible(a) || !visible(b)) continue;
            for (int step=0;step<segments;step++) {
                Point[] p={edgeVertex(a,b,0,step),edgeVertex(a,b,0,step+1),
                           edgeVertex(a,b,1,step+1),edgeVertex(a,b,1,step)};
                Point[] n={edgeNormal(a,b,0,step),edgeNormal(a,b,0,step+1),
                           edgeNormal(a,b,1,step+1),edgeNormal(a,b,1,step)};
                addPatch(out,p,n,preferred(a,b),"edge",add(normal(a),normal(b)));
            }
        }
        for (int bits=0;bits<8;bits++) {
            int a=bits&1,b=2+((bits>>1)&1),c=4+((bits>>2)&1);
            if (!visible(a) || !visible(b) || !visible(c)) continue;
            // Triangular barycentric grid projected onto the true spherical octant. Boundary samples use the
            // exact quarter-circle helper from the edge strips, avoiding gaps or different arc subdivisions.
            for (int i=0;i<segments;i++) for(int j=0;j<segments-i;j++) {
                addCorner(out,bits,a,b,c,i,j,i+1,j,i,j+1);
                if(i+j<segments-1) addCorner(out,bits,a,b,c,i+1,j,i+1,j+1,i,j+1);
            }
        }
        patches=List.copyOf(out);
    }

    /** Maximum geometric patches before repeated material/tint layers; sphere triangles use degenerate quads. */
    public int patchBudget() { return 6+12*segments+8*segments*segments; }

    private void addCorner(List<Patch> out,int bits,int a,int b,int c,int i0,int j0,int i1,int j1,int i2,int j2) {
        Point[] n={cornerNormal(bits,i0,j0),cornerNormal(bits,i1,j1),cornerNormal(bits,i2,j2),cornerNormal(bits,i2,j2)};
        Point[] p=new Point[4];
        for(int i=0;i<4;i++) p[i]=cornerVertex(bits,n[i]);
        addPatch(out,p,n,preferred(preferred(a,b),c),"corner",add(add(normal(a),normal(b)),normal(c)));
    }
    private Point cornerNormal(int bits,int i,int j) {
        // Weights on X/Y/Z. Sine weights give uniform-angle samples along every boundary arc.
        int[] weight={segments-i-j,i,j};
        float[] n=new float[3];
        for(int axis=0;axis<3;axis++) n[axis]=(float)Math.sin(Math.PI*0.5*weight[axis]/segments);
        for(int zero=0;zero<3;zero++) if(weight[zero]==0) {
            int lo=-1,hi=-1;
            for(int axis=0;axis<3;axis++) if(axis!=zero) { if(lo<0) lo=axis; else hi=axis; }
            Point arc=arcNormal(lo*2+((bits>>lo)&1),hi*2+((bits>>hi)&1),weight[hi]);
            return arc;
        }
        for(int axis=0;axis<3;axis++) if(((bits>>axis)&1)==0) n[axis]=-n[axis];
        return unit(new Point(n[0],n[1],n[2]));
    }
    private Point cornerVertex(int bits,Point n) {
        float[] p=new float[3];
        for(int axis=0;axis<3;axis++) {
            int end=(bits>>axis)&1; float s=end==0?-1:1;
            p[axis]=end-s*radius+radius*n.at(axis);
        }
        return new Point(p[0],p[1],p[2]);
    }
    private Point arcNormal(int a,int b,int step) {
        // The endpoints are exact axes, including at r=0; no trigonometric near-zero coordinate drift.
        if(step==0) return normal(a);
        if(step==segments) return normal(b);
        double theta=Math.PI*0.5*step/segments;
        return add(scale(normal(a),(float)Math.cos(theta)),scale(normal(b),(float)Math.sin(theta)));
    }
    private float edgeRadius(int a,int b,int end) {
        int free=3-a/2-b/2,endFace=free*2+end;
        return visible(endFace)||((continuing>>(pair(a,b)*2+end))&1)!=0 ? radius : 0f;
    }
    private Point edgeVertex(int a,int b,int end,int step) {
        int free=3-a/2-b/2;
        float r=edgeRadius(a,b,end);
        Point n=arcNormal(a,b,step);
        float[] p=new float[3];
        for(int face:new int[]{a,b}) {
            float s=(face&1)==0?-1:1;
            p[face/2]=(face&1)-s*r+r*n.at(face/2);
        }
        p[free]=end;
        if(visible(free*2+end)) p[free]+=end==0?radius:-radius;
        return new Point(p[0],p[1],p[2]);
    }
    private Point edgeNormal(int a,int b,int end,int step) {
        Point radial=arcNormal(a,b,step);
        float r0=edgeRadius(a,b,0),r1=edgeRadius(a,b,1);
        if(r0==r1) return radial;
        // A noncontinuing joint tapers to the original grid corner. Its longitudinal slope makes a cone,
        // not the fixed-radius cylinder; give it a true cone normal rather than a fabricated radial normal.
        int free=3-a/2-b/2;
        float span=1-(visible(free*2)?radius:0)-(visible(free*2+1)?radius:0);
        float slope=(r1-r0)/span;
        float axial=-slope*dot(radial,subtract(radial,add(normal(a),normal(b))));
        float[] n={radial.x,radial.y,radial.z};n[free]=axial;
        return unit(new Point(n[0],n[1],n[2]));
    }
    private static void addPatch(List<Patch> out,Point[] p,Point[] n,int texture,String kind,Point outward) {
        if(areaSquared(p)<1e-18f) return;
        Point geometric=cross(subtract(p[2],p[0]),subtract(p[3],p[1]));
        if(dot(geometric,outward)<0) {
            Point t=p[1];p[1]=p[3];p[3]=t;t=n[1];n[1]=n[3];n[3]=t;
        }
        out.add(new Patch(p,texture,kind,n));
    }
    private static Point[] flatNormals(Point[] p) {
        Point n=unit(cross(subtract(p[2],p[0]),subtract(p[3],p[1])));
        return new Point[]{n,n,n,n};
    }
    public static Point scale(Point p,float s) { return new Point(p.x*s,p.y*s,p.z*s); }
    public static Point unit(Point p) { return scale(p,(float)(1/Math.sqrt(dot(p,p)))); }

    public Point faceVertex(int face,int bits) {
        float[] p={(bits&1),((bits>>1)&1),((bits>>2)&1)};
        int axis=face/2;
        p[axis]=face&1;
        for (int other=0;other<3;other++) if (other!=axis) {
            int adjacent=other*2+((bits>>other)&1);
            if (!visible(adjacent)) continue;
            int free=3-axis-other;
            int end=(bits>>free)&1;
            int endFace=free*2+end;
            boolean fullRadius=visible(endFace) || ((continuing>>(pair(face,adjacent)*2+end))&1)!=0;
            if (fullRadius) p[other]+=(p[other]==0 ? radius : -radius);
        }
        return new Point(p[0],p[1],p[2]);
    }

    public boolean visible(int face) { return (exposed&(1<<face))!=0; }
    public static int pair(int a,int b) {
        int index=0;
        for (int i=0;i<6;i++) for (int j=i+1;j<6;j++) if (i/2!=j/2) {
            if ((i==a && j==b)||(i==b && j==a)) return index;
            index++;
        }
        throw new IllegalArgumentException("parallel faces");
    }
    private static int preferred(int a,int b) { return a==3 || b==3 ? 3 : Math.min(a,b); }
    public static Point normal(int face) {
        float s=(face&1)==0 ? -1 : 1;
        return new Point(face/2==0?s:0,face/2==1?s:0,face/2==2?s:0);
    }
    public static Point subtract(Point a,Point b) { return new Point(a.x-b.x,a.y-b.y,a.z-b.z); }
    public static Point add(Point a,Point b) { return new Point(a.x+b.x,a.y+b.y,a.z+b.z); }
    public static Point cross(Point a,Point b) { return new Point(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x); }
    public static float dot(Point a,Point b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
    public static float areaSquared(Point[] p) { Point n=cross(subtract(p[2],p[0]),subtract(p[3],p[1])); return dot(n,n); }
    public static void orient(Point[] p,Point outward) {
        Point n=cross(subtract(p[2],p[0]),subtract(p[3],p[1]));
        if (dot(n,outward)<0) { Point t=p[1]; p[1]=p[3]; p[3]=t; }
    }
}
