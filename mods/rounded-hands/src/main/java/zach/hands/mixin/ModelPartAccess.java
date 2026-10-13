package zach.hands.mixin;

import java.util.List;
import java.util.Map;
import net.minecraft.client.model.geom.ModelPart;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(value=ModelPart.class,remap=false)
public interface ModelPartAccess {
    @Accessor("cubes") List<ModelPart.Cube> zach$cubes();
    @Accessor("children") Map<String,ModelPart> zach$children();
}
