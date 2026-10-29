// 26.2 copy of scripts/subst-fabric-src/Target_MinecraftServer_Int.java (Mojang names; the file keeps its name so it
// replaces the shared one). MinecraftServer.loadStatusIcon() uses javax.imageio -> AWT, which cannot load in a macOS
// native image. Return no icon.
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;
@TargetClass(className = "net.minecraft.server.MinecraftServer")
final class Target_MinecraftServer_Int {
  @Substitute
  private java.util.Optional<?> loadStatusIcon() { return java.util.Optional.empty(); }
}
