package local.roundedterrain.mixin;

import local.roundedterrain.TerrainBevel;
import net.caffeinemc.mods.sodium.client.render.chunk.compile.pipeline.BlockRenderer;
import net.caffeinemc.mods.sodium.client.render.model.MutableQuadViewImpl;
import net.caffeinemc.mods.sodium.client.services.PlatformModelEmitter;
import net.minecraft.client.renderer.block.BlockAndTintGetter;
import net.minecraft.client.renderer.block.dispatch.BlockStateModel;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.state.BlockState;
import java.util.function.Predicate;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

// The Metal fork overwrites renderModel at priority1000 to cache its predicates.
// Mixin requires a higher injector priority to target that merged method; its emitter ABI is unchanged.
@Mixin(value=BlockRenderer.class,remap=false,priority=1100)
public abstract class BlockRendererMixin {
    @Redirect(method="renderModel",at=@At(value="INVOKE",target="Lnet/caffeinemc/mods/sodium/client/services/PlatformModelEmitter;emitModel(Lnet/minecraft/client/renderer/block/dispatch/BlockStateModel;Ljava/util/function/Predicate;Lnet/caffeinemc/mods/sodium/client/render/model/MutableQuadViewImpl;Lnet/minecraft/util/RandomSource;Lnet/minecraft/client/renderer/block/BlockAndTintGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/caffeinemc/mods/sodium/client/services/PlatformModelEmitter$Bufferer;)V"))
    private void roundedterrain$mesh(PlatformModelEmitter emitter,BlockStateModel model,Predicate<Direction> cull,
                                     MutableQuadViewImpl quad,RandomSource random,BlockAndTintGetter view,
                                     BlockPos pos,BlockState state,PlatformModelEmitter.Bufferer buffer) {
        TerrainBevel.emit(emitter,model,cull,quad,random,view,pos,state,buffer);
    }
}
