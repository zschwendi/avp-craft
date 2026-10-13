package zach.hands.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import net.minecraft.client.model.geom.ModelPart;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.entity.player.AvatarRenderer;
import net.minecraft.client.renderer.rendertype.RenderType;
import net.minecraft.client.renderer.texture.UvMapping;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;
import zach.hands.HandGeometry;

@Mixin(value=AvatarRenderer.class,remap=false)
public abstract class AvatarHandMixin {
    @Redirect(method="renderHand",require=1,at=@At(value="INVOKE",
        target="Lnet/minecraft/client/renderer/SubmitNodeCollector;submitModelPart(Lnet/minecraft/client/model/geom/ModelPart;Lcom/mojang/blaze3d/vertex/PoseStack;Lnet/minecraft/client/renderer/rendertype/RenderType;IILnet/minecraft/client/renderer/texture/UvMapping;)V"))
    private void zach$roundedArm(SubmitNodeCollector collector,ModelPart arm,PoseStack pose,
                                 RenderType type,int light,int overlay,UvMapping mapping) {
        HandGeometry.submit(collector,arm,pose,type,light,overlay,mapping);
    }
}
