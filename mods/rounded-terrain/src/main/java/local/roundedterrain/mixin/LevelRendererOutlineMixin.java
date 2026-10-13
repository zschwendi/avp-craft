package local.roundedterrain.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import java.util.List;
import local.roundedterrain.OutlineGeometry;
import local.roundedterrain.RoundedOutlineState;
import net.minecraft.SharedConstants;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.SubmitNodeCollection;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.feature.CustomFeatureRenderer;
import net.minecraft.client.renderer.rendertype.RenderType;
import net.minecraft.client.renderer.state.level.BlockOutlineRenderState;
import net.minecraft.util.ARGB;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Replace the selected block's cube with its captured mesh in the original outline pass. */
@Mixin(LevelRenderer.class)
public abstract class LevelRendererOutlineMixin {
    @Inject(method = "submitHitOutline", at = @At("HEAD"), cancellable = true)
    private void roundedterrain$submitOutline(PoseStack poses, SubmitNodeCollector collector, RenderType renderType,
                                              BlockOutlineRenderState outline, int color, float width,
                                              boolean translucent, CallbackInfo ci) {
        if (SharedConstants.DEBUG_SHAPES) return;
        List<OutlineGeometry.Segment> segments = ((RoundedOutlineState) (Object) outline).roundedterrain$getOutline();
        if (segments.isEmpty()) return;
        // Actual SubmitNodeStorage.submitShapeOutline delegates to order(0). A foreign collector retains vanilla.
        if (!(collector.order(0) instanceof SubmitNodeCollection collection)) return;
        var submit = new CustomFeatureRenderer.Submit(poses.last().copy(), renderType,
                (pose, vertices) -> OutlineGeometry.render(segments, pose, vertices, color, width));
        // Generic submitCustomGeometry would send blended lines to a different phase; copy vanilla shape routing.
        if (ARGB.alpha(color) == 255) collection.solid.submit(submit);
        else if (translucent) collection.afterTerrain.submit(submit);
        else collection.shapeOutlines.submit(submit);
        ci.cancel();
    }
}
