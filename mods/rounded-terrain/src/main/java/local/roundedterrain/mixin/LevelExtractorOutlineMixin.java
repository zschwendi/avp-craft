package local.roundedterrain.mixin;

import local.roundedterrain.BevelMesh;
import local.roundedterrain.OutlineGeometry;
import local.roundedterrain.RoundedOutlineState;
import local.roundedterrain.TerrainBevel;
import net.minecraft.SharedConstants;
import net.minecraft.client.Camera;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.renderer.extract.LevelExtractor;
import net.minecraft.client.renderer.state.level.BlockOutlineRenderState;
import net.minecraft.client.renderer.state.level.LevelRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Preserve vanilla picking and render-state extraction; attach only the eligible selected block's visual mesh. */
@Mixin(LevelExtractor.class)
public abstract class LevelExtractorOutlineMixin {
    @Shadow private ClientLevel level;

    @Inject(method = "extractBlockOutline", at = @At("RETURN"))
    private void roundedterrain$captureOutline(Camera camera, LevelRenderState state, CallbackInfo ci) {
        BlockOutlineRenderState outline = state.blockOutlineRenderState;
        if (level == null || outline == null || SharedConstants.DEBUG_SHAPES) return;
        BevelMesh mesh = TerrainBevel.outlineMesh(level, outline.pos(), level.getBlockState(outline.pos()));
        if (mesh == null) return;
        ((RoundedOutlineState) (Object) outline).roundedterrain$setOutline(OutlineGeometry.fromMesh(mesh));
    }
}
