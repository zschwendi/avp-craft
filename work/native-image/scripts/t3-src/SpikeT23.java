// T2/T3 harness entry: pins the platform properties, then runs the spike harness (SpikeFabric: FPS log + screenshots).
//   os.name           -> "Mac OS X" unless already Mac/Darwin (LWJGL Platform.get() and Minecraft Util.getPlatform()
//                        both key on it; the visionOS build must keep taking the MACOSX branches)
//   os.version        -> "Android-14 (Darwin <real>)" when -Dklepton.vr.android=true: selects Vivecraft's
//                        DeviceCompat.Mobile (OpenXR + XR_KHR_opengl_es_enable + VLoader EGL handles).
//                        -Dos.version=Android-... directly also works (DeviceCompat only does contains("Android")).
//   org.lwjgl.system.allocator -> "system" (no jemalloc) unless set
public class SpikeT23 {
  public static void main(String[] args) throws Exception {
    if (System.getProperty("org.lwjgl.system.allocator") == null) System.setProperty("org.lwjgl.system.allocator", "system");
    String os = System.getProperty("os.name", "");
    if (!(os.startsWith("Mac OS X") || os.startsWith("Darwin"))) System.setProperty("os.name", "Mac OS X");
    if (Boolean.getBoolean("klepton.vr.android")) {
      String v = System.getProperty("os.version", "");
      if (!v.contains("Android")) System.setProperty("os.version", "Android-14 (Darwin " + v + ")");
    }
    System.out.println("[T23] os.name=" + System.getProperty("os.name") + " os.version=" + System.getProperty("os.version")
        + " allocator=" + System.getProperty("org.lwjgl.system.allocator")
        + " lwjgl.librarypath=" + System.getProperty("org.lwjgl.librarypath")
        + " openxr.libname=" + System.getProperty("org.lwjgl.openxr.libname")
        + " java.library.path=" + System.getProperty("java.library.path"));
    KlScreenWalker.startIfAsked();   // host test harness, off unless -Dklepton.walk=<s> (scripts/t4-src/subst)
    SpikeFabric.main(args);
  }
}
