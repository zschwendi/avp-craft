package zach.hands;

import java.util.ArrayList;
import java.util.List;
import net.minecraft.client.model.geom.ModelPart;
import org.joml.Vector3f;

/** True quarter-circle edges and spherical corners; all triangles retain the entity QUADS ABI. */
public final class BevelMesh {
    public static final int SEGMENTS=3;
    public static final int PATCHES=6+12*SEGMENTS+8*SEGMENTS*SEGMENTS;
    public record Vertex(float x, float y, float z, float u, float v, float nx, float ny, float nz) {}
    public final Vertex[] vertices;
    public final float[] low, high;
    private final Face[] faces = new Face[6];
    private final List<Vertex> output = new ArrayList<>(PATCHES*4);
    /** Fixed radius in source model pixels, before positions are divided by16. */
    public final float radius;

    private record Face(ModelPart.Vertex[] q, int axis) {
        float[] uv(float[] p) {
            var a=q[0]; var b=q[1]; var d=q[3];
            float[] av={a.x(),a.y(),a.z()}, bv={b.x(),b.y(),b.z()}, dv={d.x(),d.y(),d.z()};
            float ds=0,dt=0,s=0,t=0;
            for(int k=0;k<3;k++) {
                float e=bv[k]-av[k], f=dv[k]-av[k];
                ds+=e*e; dt+=f*f; s+=(p[k]-av[k])*e; t+=(p[k]-av[k])*f;
            }
            s=Math.max(0,Math.min(1,s/ds)); t=Math.max(0,Math.min(1,t/dt));
            float u=(1-s)*(1-t)*a.u()+s*(1-t)*b.u()+s*t*q[2].u()+(1-s)*t*d.u();
            float v=(1-s)*(1-t)*a.v()+s*(1-t)*b.v()+s*t*q[2].v()+(1-s)*t*d.v();
            return new float[]{u,v};
        }
    }

    /** Null means an unusual/partial cube should retain its exact vanilla mesh. */
    public static BevelMesh build(ModelPart.Cube cube, float requestedRadius) {
        if(cube.polygons.length!=6||!Float.isFinite(requestedRadius)||requestedRadius<=0)return null;
        try { return new BevelMesh(cube,requestedRadius); }
        catch(IllegalArgumentException ex) { return null; }
    }

    private BevelMesh(ModelPart.Cube cube,float requestedRadius) {
        low=new float[]{Float.POSITIVE_INFINITY,Float.POSITIVE_INFINITY,Float.POSITIVE_INFINITY};
        high=new float[]{Float.NEGATIVE_INFINITY,Float.NEGATIVE_INFINITY,Float.NEGATIVE_INFINITY};
        for(var f:cube.polygons)for(var v:f.vertices()) {
            float[] p={v.x(),v.y(),v.z()};
            for(int k=0;k<3;k++){low[k]=Math.min(low[k],p[k]);high[k]=Math.max(high[k],p[k]);}
        }
        float span=Math.min(high[0]-low[0],Math.min(high[1]-low[1],high[2]-low[2]));
        if(!(span>0.05f))throw new IllegalArgumentException("flat/invalid cube");
        radius=Math.min(requestedRadius,span*0.14f);
        for(var f:cube.polygons) {
            var q=f.vertices(); if(q.length!=4)throw new IllegalArgumentException("non-quad cube");
            int axis=-1;
            for(int k=0;k<3;k++) {
                float first=coord(q[0],k); boolean constant=true;
                for(var v:q)constant&=Math.abs(coord(v,k)-first)<0.0001f;
                if(constant)axis=k;
            }
            if(axis<0)throw new IllegalArgumentException("non-axis cube face");
            boolean positive=coord(q[0],axis)>(low[axis]+high[axis])*0.5f;
            faces[axis*2+(positive?1:0)]=new Face(q,axis);
        }
        for(Face f:faces)if(f==null)throw new IllegalArgumentException("missing face");
        // Main faces: all positions retain the original extremal plane.
        for(int axis=0;axis<3;axis++)for(int sign=0;sign<2;sign++) {
            int a=(axis+1)%3,b=(axis+2)%3;
            float[][] q=new float[4][3];
            for(int i=0;i<4;i++) {
                q[i][axis]=outer(axis,sign);
                q[i][a]=inner(a,(i==1||i==2)?1:0);
                q[i][b]=inner(b,i>=2?1:0);
            }
            emit(q,axisNormal(axis,sign),faces[axis*2+sign]);
        }
        // Each edge is between two planes. The free axis runs along the edge.
        for(int free=0;free<3;free++) {
            int a=(free+1)%3,b=(free+2)%3;
            for(int sa=0;sa<2;sa++)for(int sb=0;sb<2;sb++) {
                for(int step=0;step<SEGMENTS;step++) {
                    float[][] n={arcNormal(a,sa,b,sb,step),arcNormal(a,sa,b,sb,step+1),
                                 arcNormal(a,sa,b,sb,step+1),arcNormal(a,sa,b,sb,step)};
                    float[][] q=new float[4][3];
                    for(int i=0;i<4;i++) {
                        q[i][free]=inner(free,i>=2?1:0);
                        q[i][a]=inner(a,sa)+radius*n[i][a];
                        q[i][b]=inner(b,sb)+radius*n[i][b];
                    }
                    float[] outward=new float[3];outward[a]=signed(sa);outward[b]=signed(sb);
                    emit(q,n,outward,faces[a*2+sa]);
                }
            }
        }
        // Spherical octant subdivided into a triangular grid. Boundary samples exactly match edge circles.
        for(int sx=0;sx<2;sx++)for(int sy=0;sy<2;sy++)for(int sz=0;sz<2;sz++) {
            int[] signs={sx,sy,sz};
            for(int i=0;i<SEGMENTS;i++)for(int j=0;j<SEGMENTS-i;j++) {
                corner(signs,i,j,i+1,j,i,j+1);
                if(i+j<SEGMENTS-1)corner(signs,i+1,j,i+1,j+1,i,j+1);
            }
        }
        vertices=output.toArray(Vertex[]::new);
        if(vertices.length!=PATCHES*4)throw new IllegalArgumentException("unexpected face count");
    }

    private void corner(int[] signs,int i0,int j0,int i1,int j1,int i2,int j2) {
        float[][] n={cornerNormal(signs,i0,j0),cornerNormal(signs,i1,j1),cornerNormal(signs,i2,j2),cornerNormal(signs,i2,j2)};
        float[][] q=new float[4][3];
        for(int i=0;i<4;i++)for(int axis=0;axis<3;axis++)q[i][axis]=inner(axis,signs[axis])+radius*n[i][axis];
        emit(q,n,new float[]{signed(signs[0]),signed(signs[1]),signed(signs[2])},faces[signs[0]]);
    }
    private static float[] cornerNormal(int[] signs,int i,int j) {
        int[] weights={SEGMENTS-i-j,i,j};
        for(int zero=0;zero<3;zero++)if(weights[zero]==0) {
            int lo=-1,hi=-1;
            for(int axis=0;axis<3;axis++)if(axis!=zero){if(lo<0)lo=axis;else hi=axis;}
            return arcNormal(lo,signs[lo],hi,signs[hi],weights[hi]);
        }
        Vector3f n=new Vector3f();
        for(int axis=0;axis<3;axis++)n.setComponent(axis,signed(signs[axis])*(float)Math.sin(Math.PI*0.5*weights[axis]/SEGMENTS));
        n.normalize();return new float[]{n.x,n.y,n.z};
    }
    private static float[] arcNormal(int a,int sa,int b,int sb,int step) {
        // Canonical axis order gives precisely identical samples at every edge/sphere boundary.
        if(a>b)return arcNormal(b,sb,a,sa,SEGMENTS-step);
        if(step==0)return axisNormal(a,sa);
        if(step==SEGMENTS)return axisNormal(b,sb);
        double theta=Math.PI*0.5*step/SEGMENTS;
        float[] n=new float[3];n[a]=signed(sa)*(float)Math.cos(theta);n[b]=signed(sb)*(float)Math.sin(theta);return n;
    }

    private static float coord(ModelPart.Vertex v,int k){return k==0?v.x():k==1?v.y():v.z();}
    private static float signed(int sign){return sign==0?-1:1;}
    private float outer(int axis,int sign){return sign==0?low[axis]:high[axis];}
    private float inner(int axis,int sign){return outer(axis,sign)-signed(sign)*radius;}
    private static float[] axisNormal(int axis,int sign){float[] n=new float[3];n[axis]=signed(sign);return n;}

    private void emit(float[][] q,float[] outward,Face textureFace) {
        float[][] n={outward.clone(),outward.clone(),outward.clone(),outward.clone()};
        emit(q,n,outward,textureFace);
    }
    private void emit(float[][] q,float[][] normals,float[] outward,Face textureFace) {
        Vector3f a=new Vector3f(q[1][0]-q[0][0],q[1][1]-q[0][1],q[1][2]-q[0][2]);
        Vector3f b=new Vector3f(q[2][0]-q[0][0],q[2][1]-q[0][1],q[2][2]-q[0][2]);
        if(a.cross(b).dot(outward[0],outward[1],outward[2])<0) {
            if(java.util.Arrays.equals(q[2],q[3])) {
                float[] t=q[1];q[1]=q[2];q[2]=t;q[3]=q[2].clone();
                t=normals[1];normals[1]=normals[2];normals[2]=t;normals[3]=normals[2].clone();
            } else {
                float[] t=q[1];q[1]=q[3];q[3]=t;t=normals[1];normals[1]=normals[3];normals[3]=t;
            }
        }
        for(int i=0;i<4;i++) {
            float[] p=q[i];Vector3f n=new Vector3f(normals[i]).normalize();float[] uv=textureFace.uv(p);
            output.add(new Vertex(p[0]/16,p[1]/16,p[2]/16,uv[0],uv[1],n.x,n.y,n.z));
        }
    }
}
