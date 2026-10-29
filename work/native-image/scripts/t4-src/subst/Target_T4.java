// T4 substitution (native-image only), on top of scripts/t3-src/Target_T3.java.
//   MCOpenXR.destroy -> the same teardown with null guards (T2T3-REPORT open issue 4). After a failed OpenXR init
//   (no instance/session/swapchains yet) Vivecraft's VRState.destroyVR calls destroy(), which iterated the null
//   `swapchain` array (MCOpenXR.java:99) and NPE'd; the retries then turned into a Minecraft crash instead of the
//   "VR init failed" error screen. Body = QuestCraftPlusPlus_VivecraftMod OpenXR-1.21.5 612a11ca MCOpenXR.destroy(),
//   with every handle/buffer null-checked.
import com.oracle.svm.core.annotate.Alias;
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;
import org.lwjgl.openxr.XR10;
import org.lwjgl.openxr.XrActionSet;
import org.lwjgl.openxr.XrSwapchain;
import org.lwjgl.opengl.GL11;
import org.vivecraft.client_vr.provider.openxr.MCOpenXR;

@TargetClass(className = "org.vivecraft.client_vr.provider.openxr.MCOpenXR")
final class Target_MCOpenXR {
  @Alias private java.util.Map<org.vivecraft.client_vr.provider.control.VRInputActionSet, Long> actionSetHandles;
  @Alias protected native void logError(int error, String method, String... extra);

  @Substitute
  public void destroy() {
    MCOpenXR self = (MCOpenXR) (Object) this;
    int error;
    if (self.instance != null && actionSetHandles != null) {
      for (Long set : actionSetHandles.values()) {
        error = XR10.xrDestroyActionSet(new XrActionSet(set, self.instance));
        logError(error, "xrDestroyActionSet", "");
      }
    }
    if (self.swapchain != null) {
      for (XrSwapchain sc : self.swapchain) {
        if (sc == null) continue;
        error = XR10.xrDestroySwapchain(sc);
        logError(error, "xrDestroySwapchain", "");
      }
    }
    if (self.viewBuffer != null) self.viewBuffer.close();
    if (self.xrAppSpace != null) { error = XR10.xrDestroySpace(self.xrAppSpace); logError(error, "xrDestroySpace", "xrAppSpace"); }
    if (self.xrViewSpace != null) { error = XR10.xrDestroySpace(self.xrViewSpace); logError(error, "xrDestroySpace", "xrViewSpace"); }
    if (self.session != null) { error = XR10.xrDestroySession(self.session); logError(error, "xrDestroySession", ""); }
    if (self.instance != null) { error = XR10.xrDestroyInstance(self.instance); logError(error, "xrDestroyInstance", ""); }
    GL11.glEnable(0x8DB9 /* GL_FRAMEBUFFER_SRGB */);
    if (self.eventDataBuffer != null) self.eventDataBuffer.close();
    System.err.println("[T4] MCOpenXR.destroy (null-guarded) done; instance=" + self.instance + " swapchain=" + (self.swapchain == null ? "null" : String.valueOf(self.swapchain.length)));
  }
}
