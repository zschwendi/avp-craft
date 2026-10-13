/* Copyright 2026 mcopt-shade contributors. Licensed under Apache-2.0. */
package mcopt.metal.mixin.shade;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import mcopt.metal.shade.ShadeMaterials;
import net.minecraft.client.renderer.texture.SpriteContents;
import net.minecraft.client.renderer.texture.SpriteLoader;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.resources.Identifier;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyVariable;

/** Keep paired LabPBR data out of vanilla's diffuse-only block atlas. Source files remain available to the compact loader. */
@Mixin(SpriteLoader.class)
public abstract class ShadeMaterialStitchMixin {
    @Shadow @Final private Identifier location;

    @ModifyVariable(method="stitch(Ljava/util/List;ILjava/util/concurrent/Executor;)Lnet/minecraft/client/renderer/texture/SpriteLoader$Preparations;",
                    at=@At("HEAD"),argsOnly=true,index=1)
    private List<SpriteContents> mcopt$materialData(List<SpriteContents> input) {
        if (!ShadeMaterials.ON || !TextureAtlas.LOCATION_BLOCKS.equals(location)) return input;
        Set<Identifier> names=new HashSet<>();
        for (SpriteContents sprite:input) names.add(sprite.name());
        List<SpriteContents> kept=new ArrayList<>(input.size());
        int dropped=0;
        long texels=0;
        for (SpriteContents sprite:input) {
            Identifier name=sprite.name();
            String path=name.getPath();
            boolean suffix=path.endsWith("_n") || path.endsWith("_s");
            String base=suffix ? path.substring(0,path.length()-2) : "";
            // Suffix alone is insufficient: standalone custom textures retain their original semantics.
            boolean paired=suffix && !base.isEmpty() && !base.endsWith("/") && names.contains(name.withPath(base));
            if (paired) {
                dropped++;
                texels+=(long)sprite.width()*sprite.height();
                sprite.close();
            } else kept.add(sprite);
        }
        if (dropped==0) return input;
        System.out.println("[mcopt] materials: excluded "+dropped+" paired _n/_s diffuse-atlas sprites ("+(texels*4L/1048576L)+" MiB decoded); base textures preserved");
        return kept;
    }
}
