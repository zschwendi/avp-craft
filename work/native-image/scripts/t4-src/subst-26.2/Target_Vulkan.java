// 26.2 Vulkan backend kept out of the image's runtime paths (M3 stage C; native-image only). Klepton is GL-only
// (klglfw: glfwVulkanSupported = false; libltw = GLES over ANGLE), and the staged options pin
// preferredGraphicsBackend:"opengl" (scripts/m3a/pin-gl-options.py). Belt and braces for a reset options.txt:
//   PreferredGraphicsApi.getBackendsToTry()  -> [GlBackend] only (vanilla: [GL, Vulkan], VULKAN: [Vulkan, GL])
//   VulkanBackend.checkBackendAvailable()    -> null (Minecraft.<init> probes it for DEFAULT only to fill the crash
//                                               report / telemetry "Vulkan unavailable" detail; no loader probe)
//   Vivecraft GraphicsHelper.getHelper()     -> OpenGLHelper (upstream picks Vulkan/GL by the device backend class;
//                                               keeps VulkanHelper/VK10 code from being constructed)
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;

@TargetClass(className = "net.minecraft.client.PreferredGraphicsApi")
final class Target_PreferredGraphicsApi {
  @Substitute public com.mojang.blaze3d.systems.GpuBackend[] getBackendsToTry() {
    return new com.mojang.blaze3d.systems.GpuBackend[] { new com.mojang.blaze3d.opengl.GlBackend() };
  }
}

@TargetClass(className = "com.mojang.blaze3d.vulkan.VulkanBackend")
final class Target_VulkanBackend {
  @Substitute public static com.mojang.blaze3d.systems.BackendCreationException checkBackendAvailable() { return null; }
}

@TargetClass(className = "org.vivecraft.client_vr.render.helpers.graphics.GraphicsHelper")
final class Target_VivecraftGraphicsHelper {
  @Substitute private static org.vivecraft.client_vr.render.helpers.graphics.GraphicsHelper getHelper() {
    return new org.vivecraft.client_vr.render.helpers.graphics.opengl.OpenGLHelper();
  }
}
