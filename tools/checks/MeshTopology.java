/* Copyright 2026 mcopt-shade contributors. Licensed under Apache-2.0. */
import local.roundedterrain.BevelMesh;
import local.roundedterrain.BevelMesh.Point;
import java.util.*;

/** Independent topology audit of the emitted mesh; tests actual shared world-space edges, not generator internals. */
public class MeshTopology {
    record Cell(int x,int y,int z) {}
    record Triangle(Point a,Point b,Point c) {}
    static int assertions, cases;
    static void check(boolean ok,String detail) { assertions++; if (!ok) throw new AssertionError(detail); }
    static Cell neighbor(Cell c,int face) { int s=(face&1)==0?-1:1; return new Cell(c.x+(face/2==0?s:0),c.y+(face/2==1?s:0),c.z+(face/2==2?s:0)); }
    static int visible(Set<Cell> cells,Cell c) { int m=0; for(int f=0;f<6;f++) if(!cells.contains(neighbor(c,f))) m|=1<<f; return m; }
    static long continuing(Set<Cell> cells,Set<Cell> selected,Cell c,int mask) {
        long result=0;
        for(int a=0;a<6;a++) for(int b=a+1;b<6;b++) if(a/2!=b/2 && (mask&(1<<a))!=0 && (mask&(1<<b))!=0) {
            int free=3-a/2-b/2;
            for(int end=0;end<2;end++) { Cell n=neighbor(c,2*free+end); int nm=visible(cells,n);
                if(selected.contains(n) && (nm&(1<<a))!=0 && (nm&(1<<b))!=0) result|=1L<<(BevelMesh.pair(a,b)*2+end);
            }
        }
        return result;
    }
    static Point world(Point p,Cell c) { return new Point(p.x()+c.x,p.y()+c.y,p.z()+c.z); }
    static String key(Point p) { return Math.round(p.x()*1e6)+","+Math.round(p.y()*1e6)+","+Math.round(p.z()*1e6); }
    static List<Triangle> mesh(Set<Cell> cells,float radius) {
        return mesh(cells,cells,radius);
    }
    static List<Triangle> mesh(Set<Cell> cells,Set<Cell> selected,float radius) {
        return mesh(cells,selected,radius,BevelMesh.DEFAULT_SEGMENTS);
    }
    static List<Triangle> mesh(Set<Cell> cells,Set<Cell> selected,float radius,int segments) {
        List<Triangle> triangles=new ArrayList<>();
        for(Cell c:cells) {
            int mask=visible(cells,c);
            BevelMesh m=new BevelMesh(mask,continuing(cells,selected,c,mask),selected.contains(c)?radius:0f,segments);
            check(m.patches.size()<=m.patchBudget(),"quad budget exceeded");
            for(var q:m.patches) {
                Point[] p=q.points();
                for(Point v:p) check(v.x()>=0&&v.x()<=1&&v.y()>=0&&v.y()<=1&&v.z()>=0&&v.z()<=1,"geometry outside original collision cube");
                Point n=BevelMesh.cross(BevelMesh.subtract(p[2],p[0]),BevelMesh.subtract(p[3],p[1]));
                check(BevelMesh.dot(n,n)>1e-18,"degenerate patch");
                check(q.normals().length==4,"missing per-vertex normal contract");
                for(Point normal:q.normals()) {
                    check(Math.abs(BevelMesh.dot(normal,normal)-1)<1e-5,"non-unit/non-finite normal");
                    check(BevelMesh.dot(n,normal)>0,"normal points into its patch");
                }
                float plane=BevelMesh.dot(n,BevelMesh.subtract(p[3],p[0]));
                check(Math.abs(plane)<1e-6,"nonplanar quad");
                for(int[] index:new int[][]{{0,1,2},{2,3,0}}) {
                    Point a=world(p[index[0]],c),b=world(p[index[1]],c),d=world(p[index[2]],c);
                    Point tn=BevelMesh.cross(BevelMesh.subtract(b,a),BevelMesh.subtract(d,a));
                    if(BevelMesh.dot(tn,tn)>1e-18) triangles.add(new Triangle(a,b,d));
                }
            }
        }
        return triangles;
    }
    static void audit(Set<Cell> cells,float radius) {
        audit(cells,cells,radius);
    }
    static void audit(Set<Cell> cells,Set<Cell> selected,float radius) {
        audit(cells,selected,radius,BevelMesh.DEFAULT_SEGMENTS);
    }
    static void audit(Set<Cell> cells,Set<Cell> selected,float radius,int segments) {
        List<Triangle> triangles=mesh(cells,selected,radius,segments);
        Map<String,Integer> count=new HashMap<>(), winding=new HashMap<>();
        double volume=0;
        for(Triangle t:triangles) {
            Point[] p={t.a,t.b,t.c};
            volume+=BevelMesh.dot(t.a,BevelMesh.cross(t.b,t.c))/6.0;
            for(int i=0;i<3;i++) {
                String a=key(p[i]),b=key(p[(i+1)%3]); int order=a.compareTo(b);
                String edge=order<0?a+"|"+b:b+"|"+a;
                count.merge(edge,1,Integer::sum); winding.merge(edge,order<0?1:-1,Integer::sum);
            }
        }
        for(var e:count.entrySet()) {
            boolean ordinary=e.getValue()==2;
            // Vanilla occupancy can have two diagonal solids touching along one grid edge:
            // four balanced faces are legitimate there, not an open boundary or a bevel gap.
            boolean diagonalContact=e.getValue()==4 && inputDiagonalContact(cells,e.getKey());
            check((ordinary||diagonalContact)&&winding.get(e.getKey())==0,"open or inconsistent edge in "+cells+": "+e+" winding "+winding.get(e.getKey()));
        }
        check(volume>0&&volume<=cells.size()+1e-5,"negative/outward wrong or enlarged volume "+volume);
        cases++;
    }
    static boolean inputDiagonalContact(Set<Cell> cells,String edge) {
        String[] ends=edge.split("\\|"); float[][] p=new float[2][3];
        for(int i=0;i<2;i++) { String[] xyz=ends[i].split(","); for(int a=0;a<3;a++) p[i][a]=Integer.parseInt(xyz[a])/1e6f; }
        int along=-1;
        for(int a=0;a<3;a++) if(Math.abs(p[0][a]-p[1][a])>1e-5) { if(along!=-1) return false; along=a; }
        if(along<0) return false;
        int b=(along+1)%3,c=(along+2)%3;
        if(Math.abs(p[0][b]-Math.round(p[0][b]))>1e-5||Math.abs(p[0][c]-Math.round(p[0][c]))>1e-5) return false;
        int[] xyz=new int[3]; xyz[along]=(int)Math.floor((p[0][along]+p[1][along])*0.5);
        int pattern=0;
        for(int i=0;i<4;i++) { xyz[b]=Math.round(p[0][b])-1+(i&1); xyz[c]=Math.round(p[0][c])-1+((i>>1)&1);
            if(cells.contains(new Cell(xyz[0],xyz[1],xyz[2]))) pattern|=1<<i; }
        return pattern==6||pattern==9;
    }
    static void curvature(float radius,int segments) {
        BevelMesh m=new BevelMesh(63,0,radius,segments);
        Map<String,Point> sharedNormals=new HashMap<>();
        int changingEdges=0,corners=0;
        for(var q:m.patches) {
            if(q.kind().equals("corner")) corners++;
            if(q.kind().equals("edge")) {
                boolean curved=false;
                for(Point n:q.normals()) if(BevelMesh.dot(q.normals()[0],n)<0.9999) curved=true;
                if(curved) changingEdges++;
            }
            for(int i=0;i<4;i++) {
                Point p=q.points()[i],n=q.normals()[i];
                Point center=new Point(Math.clamp(p.x(),radius,1-radius),Math.clamp(p.y(),radius,1-radius),Math.clamp(p.z(),radius,1-radius));
                Point radial=BevelMesh.subtract(p,center);
                check(Math.abs(Math.sqrt(BevelMesh.dot(radial,radial))-radius)<1e-6,"vertex is not on true rounded-box surface");
                check(BevelMesh.dot(BevelMesh.unit(radial),n)>0.99999,"curved vertex normal is not its radius direction");
                Point previous=sharedNormals.putIfAbsent(key(p),n);
                if(previous!=null) check(BevelMesh.dot(previous,n)>0.99999,"normal seam at shared curved-surface vertex");
            }
        }
        check(changingEdges==12*segments,"edge bands are flat chamfer normals rather than a curved arc");
        check(corners==8*segments*segments,"spherical corner coverage missing");
        check(m.patches.size()==m.patchBudget(),"isolated cube does not exhaust known surface budget");
    }
    public static void main(String[] args) throws Exception {
        for(int segments:new int[]{3,4}) for(float radius:new float[]{0.025f,0.04f,0.06f,0.08f}) curvature(radius,segments);
        for(float radius:new float[]{0.025f,0.04f,0.06f,0.08f}) for(int mask=1;mask<256;mask++) {
            Set<Cell> cells=new HashSet<>(); for(int i=0;i<8;i++) if((mask&(1<<i))!=0) cells.add(new Cell(i&1,(i>>1)&1,(i>>2)&1));
            audit(cells,radius);
        }
        Random r=new Random(1761);
        for(int n=0;n<100;n++) { Set<Cell> cells=new HashSet<>(); for(int i=0;i<27;i++) if(r.nextBoolean()) cells.add(new Cell(i%3,(i/3)%3,i/9)); if(!cells.isEmpty()) audit(cells,0.04f); }
        // Exhaust every empty/vanilla/beveled assignment in2x2x2, verifying seams against unselected full cubes too.
        for(int assignment=1;assignment<6561;assignment++) {
            int n=assignment; Set<Cell> cells=new HashSet<>(),selected=new HashSet<>();
            for(int i=0;i<8;i++) { int type=n%3; n/=3; Cell c=new Cell(i&1,(i>>1)&1,(i>>2)&1);
                if(type!=0) cells.add(c); if(type==2) selected.add(c); }
            audit(cells,selected,0.04f);
        }
        // Exercise the optional four-segment mode against every 2x2x2 occupancy too.
        for(int mask=1;mask<256;mask++) {
            Set<Cell> cells=new HashSet<>(); for(int i=0;i<8;i++) if((mask&(1<<i))!=0) cells.add(new Cell(i&1,(i>>1)&1,(i>>2)&1));
            audit(cells,cells,0.06f,4);
        }
        String report="{\"cases\":"+cases+",\"assertions\":"+assertions+",\"closed_world_edges\":true,\"planar_quads\":true,\"quarter_circle_edges\":true,\"spherical_corners\":true,\"continuous_surface_normals\":true,\"default_segments\":3,\"optional_segments\":4,\"max_patches_3_segments\":114,\"max_patches_4_segments\":182,\"radii\":[0.025,0.04,0.06,0.08],\"scope\":\"pure actual-source CPU mesh; excludes GPU normal interpolation, renderer hook and gameplay\"}";
        System.out.println("PASS "+report);
    }
}
