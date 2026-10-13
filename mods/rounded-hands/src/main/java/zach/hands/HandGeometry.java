package zach.hands;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;
import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.VertexConsumer;
import net.minecraft.client.model.geom.ModelPart;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.rendertype.RenderType;
import net.minecraft.client.renderer.texture.UvMapping;
import org.joml.Vector3f;
import zach.hands.mixin.ModelPartAccess;

public final class HandGeometry {
    private static final Map<ModelPart.Cube,BevelMesh> CACHE=new WeakHashMap<>();
    private static boolean announced;
    private static final float RADIUS=radius();
    private record Draw(ModelPart.Cube original,BevelMesh mesh,PoseStack.Pose local) {}
    private static float radius(){
        try{float r=Float.parseFloat(System.getProperty("zach.hands.bevel","0.4"));return Float.isFinite(r)?Math.max(0.05f,Math.min(0.5f,r)):0.4f;}
        catch(NumberFormatException ex){return 0.4f;}
    }

    public static void submit(SubmitNodeCollector collector,ModelPart arm,PoseStack pose,
                              RenderType type,int light,int overlay,UvMapping mapping) {
        if(System.getProperty("zach.hands.enabled","true").equalsIgnoreCase("false")) {
            collector.submitModelPart(arm,pose,type,light,overlay,mapping);return;
        }
        List<Draw> draws=new ArrayList<>(2);
        capture(arm,new PoseStack(),draws);
        if(!announced&&!draws.isEmpty()) {
            announced=true;
            System.out.println("rounded-hands: first-person arm-only curved mesh submitted; radius="+RADIUS+" model units");
        }
        // The collector captures the outer pose as vanilla does. Capture all
        // mutable arm/child poses NOW; callbacks must not reread player state.
        collector.submitCustomGeometry(pose,type,(outer,buffer)-> {
            VertexConsumer target=mapping==null?buffer:mapping.wrap(buffer);
            Vector3f position=new Vector3f(),normal=new Vector3f(),tmp=new Vector3f();
            for(Draw draw:draws) {
                if(draw.mesh==null) {
                    PoseStack.Pose combined=outer.copy();combined.mulPose(draw.local.pose());
                    draw.original.compile(combined,target,light,overlay,-1);continue;
                }
                for(var v:draw.mesh.vertices) {
                    draw.local.pose().transformPosition(v.x(),v.y(),v.z(),position);
                    outer.pose().transformPosition(position,position);
                    draw.local.transformNormal(v.nx(),v.ny(),v.nz(),tmp);
                    outer.transformNormal(tmp,normal);
                    target.addVertex(position.x,position.y,position.z,-1,v.u(),v.v(),overlay,light,normal.x,normal.y,normal.z);
                }
            }
        });
    }

    private static void capture(ModelPart part,PoseStack local,List<Draw> result) {
        if(!part.visible)return;
        local.pushPose();part.translateAndRotate(local);
        var access=(ModelPartAccess)(Object)part;
        if(!part.skipDraw)for(var cube:access.zach$cubes()) {
            // Null is deliberately retained for unusual models; ordinary
            // vanilla arm/sleeve cubes are always six rectangular faces.
            if(!CACHE.containsKey(cube))CACHE.put(cube,BevelMesh.build(cube,RADIUS));
            result.add(new Draw(cube,CACHE.get(cube),local.last().copy()));
        }
        for(ModelPart child:access.zach$children().values())capture(child,local,result);
        local.popPose();
    }
}
