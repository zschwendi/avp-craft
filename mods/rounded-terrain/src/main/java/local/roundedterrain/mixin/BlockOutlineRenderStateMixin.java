package local.roundedterrain.mixin;

import java.util.List;
import local.roundedterrain.OutlineGeometry;
import local.roundedterrain.RoundedOutlineState;
import net.minecraft.client.renderer.state.level.BlockOutlineRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;

@Mixin(BlockOutlineRenderState.class)
public abstract class BlockOutlineRenderStateMixin implements RoundedOutlineState {
    @Unique
    private List<OutlineGeometry.Segment> roundedterrain$outline = List.of();

    @Override
    public List<OutlineGeometry.Segment> roundedterrain$getOutline() {
        return roundedterrain$outline;
    }

    @Override
    public void roundedterrain$setOutline(List<OutlineGeometry.Segment> segments) {
        roundedterrain$outline = List.copyOf(segments);
    }
}
