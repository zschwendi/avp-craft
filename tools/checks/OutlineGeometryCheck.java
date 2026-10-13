/* Copyright 2026 mcopt-shade contributors. Licensed under Apache-2.0. */
package local.roundedterrain;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import local.roundedterrain.BevelMesh.Patch;
import local.roundedterrain.BevelMesh.Point;

/** CPU-only checks for geometric selection lines and the actual26.3 VertexConsumer/PoseStack contract. */
public final class OutlineGeometryCheck {
    private static int checks;
    private static void require(boolean result, String message) {
        checks++;
        if (!result) throw new AssertionError(message);
    }
    private static Point p(float x, float y, float z) { return new Point(x, y, z); }
    private static Patch triangle(Point a, Point b, Point c) {
        return new Patch(new Point[] {a, b, c, c}, 0, "fixture");
    }

    public static void main(String[] args) throws Exception {
        Point a=p(0,0,0), b=p(1,0,0), c=p(1,1,0), d=p(0,1,0);
        List<OutlineGeometry.Segment> square=OutlineGeometry.fromPatches(List.of(triangle(a,b,c),triangle(a,c,d)));
        require(square.size()==4,"coplanar triangulation keeps square boundary and removes diagonal");
        var folded=OutlineGeometry.fromPatches(List.of(triangle(a,b,c),triangle(a,c,p(0,0,1))));
        require(folded.size()==5,"noncoplanar shared edge remains once");
        require(OutlineGeometry.fromPatches(List.of(triangle(a,a,a))).isEmpty(),"degenerate corners produce no line");
        Point[] mutable={a,b,c,c};
        var captured=OutlineGeometry.fromPatches(List.of(new Patch(mutable,0,"fixture")));
        mutable[0]=p(99,99,99);
        require(captured.stream().noneMatch(s -> s.start().x()==99 || s.end().x()==99),"deferred segments own copied coordinates");
        try { captured.clear(); throw new AssertionError("mutable capture list"); }
        catch (UnsupportedOperationException expected) { checks++; }

        int cases=0;
        for (int segments=2;segments<=4;segments++) for (int exposed=1;exposed<64;exposed++) {
            BevelMesh mesh=new BevelMesh(exposed,0,.06f,segments);
            var lines=OutlineGeometry.fromMesh(mesh);
            require(lines==OutlineGeometry.fromMesh(mesh),"same mesh identity reuses immutable line list");
            var keys=new HashSet<String>();
            for (var line:lines) {
                Point start=line.start(),end=line.end(),n=line.direction();
                require(Float.isFinite(n.x()) && Float.isFinite(n.y()) && Float.isFinite(n.z()),"finite direction");
                require(Math.abs(n.x()*n.x()+n.y()*n.y()+n.z()*n.z()-1)<1e-5,"normalized line direction");
                require(!start.equals(end),"nonzero segment");
                String s=start.toString(),e=end.toString();
                require(keys.add(s.compareTo(e)<=0?s+e:e+s),"unique undirected segment");
            }
            cases++;
        }

        PoseStack poses=new PoseStack();
        poses.translate(2,3,4);
        PoseStack.Pose pose=poses.last().copy();
        poses.translate(100,100,100);
        List<float[]> positions=new ArrayList<>(),normals=new ArrayList<>();
        List<Integer> colors=new ArrayList<>();
        List<Float> widths=new ArrayList<>();
        VertexConsumer consumer=(VertexConsumer)Proxy.newProxyInstance(VertexConsumer.class.getClassLoader(),
                new Class<?>[]{VertexConsumer.class},(proxy,method,values)->{
                    if(method.isDefault()) return InvocationHandler.invokeDefault(proxy,method,values);
                    switch(method.getName()) {
                        case "addVertex" -> positions.add(new float[]{(float)values[0],(float)values[1],(float)values[2]});
                        case "setNormal" -> normals.add(new float[]{(float)values[0],(float)values[1],(float)values[2]});
                        case "setColor" -> colors.add((int)values[0]);
                        case "setLineWidth" -> widths.add((float)values[0]);
                        default -> throw new AssertionError("unexpected line attribute: "+method);
                    }
                    return proxy;
                });
        OutlineGeometry.render(List.of(new OutlineGeometry.Segment(a,b,p(1,0,0))),pose,consumer,0x66234567,3.5f);
        require(positions.size()==2 && normals.size()==2 && colors.size()==2 && widths.size()==2,"complete endpoint attributes");
        require(positions.get(0)[0]==2 && positions.get(0)[1]==3 && positions.get(0)[2]==4,"copied pose transforms first endpoint");
        require(positions.get(1)[0]==3 && positions.get(1)[1]==3 && positions.get(1)[2]==4,"copied pose transforms second endpoint");
        require(colors.stream().allMatch(v->v==0x66234567),"original packed color retained");
        require(widths.stream().allMatch(v->v==3.5f),"original per-vertex line width retained");
        require(normals.stream().allMatch(v->v[0]==1 && v[1]==0 && v[2]==0),"segment direction passed through pose normal transform");

        for(int i=0;i<2100;i++) OutlineGeometry.fromMesh(new BevelMesh(1,0,.06f,2));
        var field=OutlineGeometry.class.getDeclaredField("MESH_CACHE");
        field.setAccessible(true);
        require(((java.util.Map<?,?>)field.get(null)).size()==2048,"mesh identity cache covers valid contexts and remains bounded at2048");
        System.out.println("OutlineGeometryCheck PASS: "+cases+" mesh cases; "+checks+" assertions; no game/GPU initialized");
    }
}
