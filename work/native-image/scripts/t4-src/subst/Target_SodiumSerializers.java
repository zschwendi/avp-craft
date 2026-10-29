// Sodium vertex serializers without runtime class generation (native-image only; M2 terrain round).
//   VertexSerializerRegistryImpl.createSerializer generates a class per (src, dst) VertexFormat pair with ASM and
//   defines it through MethodHandles.Lookup.defineClass. A native image cannot define new bytecode at run time
//   (PredefinedClassesSupport: "Defining a class from new bytecodes at run time is not supported"), so this
//   substitution returns an interpreted serializer built from the SAME transfer list the generator would emit:
//   for every element id 0..31 that the destination format contains, copy byteSize() bytes from the source
//   vertex at src.getOffset(e) to the destination vertex at dst.getOffset(e) (adjacent copies merged), per vertex.
//   Slower than the generated code (a loop instead of unrolled puts), but it only runs on BufferBuilder.copySlow
//   (format-mismatched pushes: Vivecraft's menu world, some block models).
//   The registry itself also needs the impl class reflectively constructible (config/t23-extra/reflect-config.json:
//   DependencyInjection.load = Class.forName + getConstructor().newInstance()).
import com.mojang.blaze3d.vertex.VertexFormat;
import com.mojang.blaze3d.vertex.VertexFormatElement;
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;
import net.caffeinemc.mods.sodium.api.vertex.serializer.VertexSerializer;
import org.lwjgl.system.MemoryUtil;

@TargetClass(className = "net.caffeinemc.mods.sodium.client.render.vertex.serializers.VertexSerializerRegistryImpl")
final class Target_VertexSerializerRegistryImpl {
  @Substitute
  private static VertexSerializer createSerializer(VertexFormat src, VertexFormat dst) {
    return KlInterpretedVertexSerializer.create(src, dst);
  }
}

final class KlInterpretedVertexSerializer implements VertexSerializer {
  private final int srcStride, dstStride;
  private final int[] srcOff, dstOff, len;

  private KlInterpretedVertexSerializer(int srcStride, int dstStride, int[] srcOff, int[] dstOff, int[] len) {
    this.srcStride = srcStride; this.dstStride = dstStride;
    this.srcOff = srcOff; this.dstOff = dstOff; this.len = len;
  }

  static VertexSerializer create(VertexFormat src, VertexFormat dst) {
    int[] so = new int[32], doff = new int[32], ln = new int[32];
    int n = 0;
    for (int id = 0; id < 32; id++) {                       // = VertexSerializerFactory.createMemoryTransferList
      VertexFormatElement e = VertexFormatElement.byId(id);
      if (e == null || !dst.contains(e)) continue;
      if (!src.contains(e))
        throw new RuntimeException("Source format is missing element %s as required by destination format".formatted(e));
      int s = src.getOffset(e), d = dst.getOffset(e), l = e.byteSize();
      if (n > 0 && so[n - 1] + ln[n - 1] == s && doff[n - 1] + ln[n - 1] == d) { ln[n - 1] += l; continue; } // merge
      so[n] = s; doff[n] = d; ln[n] = l; n++;
    }
    System.err.println("[kl-sodium] interpreted vertex serializer " + src + " -> " + dst + ": " + n + " transfer(s)");
    return new KlInterpretedVertexSerializer(src.getVertexSize(), dst.getVertexSize(),
        java.util.Arrays.copyOf(so, n), java.util.Arrays.copyOf(doff, n), java.util.Arrays.copyOf(ln, n));
  }

  @Override
  public void serialize(long src, long dst, int count) {
    final int n = len.length;
    for (int v = 0; v < count; v++) {
      for (int i = 0; i < n; i++) MemoryUtil.memCopy(src + srcOff[i], dst + dstOff[i], len[i]);
      src += srcStride;
      dst += dstStride;
    }
  }
}
