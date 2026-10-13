/* Copyright 2026 mcopt-shade contributors. Licensed under Apache-2.0. */
package mcopt.metal.mixin.shade;

import mcopt.metal.shade.ShadeMaterials;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.texture.SpriteLoader;
import net.minecraft.client.renderer.texture.TextureAtlas;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(TextureAtlas.class)
public abstract class ShadeMaterialAtlasMixin {
    @Inject(method="upload",at=@At("RETURN"))
    private void mcopt$materials(SpriteLoader.Preparations preparations,CallbackInfo ci) {
        TextureAtlas atlas=(TextureAtlas)(Object)this;
        if (TextureAtlas.LOCATION_BLOCKS.equals(atlas.location()))
            ShadeMaterials.reload(preparations,Minecraft.getInstance().getResourceManager());
    }
}
