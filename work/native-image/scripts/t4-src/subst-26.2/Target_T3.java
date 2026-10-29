// 26.2 copy of scripts/t3-src/Target_T3.java (M3 stage C). 26.x is unobfuscated: every target is its Mojang name.
// T3 closed-world trim (native-image only). Each substitution removes a runtime dependency that cannot exist on visionOS:
//   Narrator.getNarrator                  -> Narrator.EMPTY        (NarratorMac = java-objc-bridge -> JNA -> AppKit)
//   MacosUtil.{exitNativeFullscreen,clearResizableBit}(Window), loadIcon(IoSupplier) -> no-ops (objc bridge)
//   GLX._getCpuInfo                       -> no OSHI               (OSHI SystemInfo -> JNA + IOKit)
//   SystemReport lambda$new$6 = putHardware(new SystemInfo()) and, NEW in 26.x, lambda$new$7 = putSoftware(new
//     SystemInfo()) -> skipped (both build an OSHI SystemInfo; 1.21.x had only the hardware lambda)
//   GlDebug.enableDebugCallback(int, boolean, Set) -> null (no GL debug closure; vanilla's verbosity<=0 result)
//   TinyFileDialogs.tinyfd_messageBox / tinyfd_setGlobalInt -> stderr / 0 (no liblwjgl_tinyfd)
//   NativeModuleLister.listModules        -> empty                 (Windows-only JNA)
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;

@TargetClass(className = "com.mojang.text2speech.Narrator")
final class Target_Narrator {
  @Substitute public static com.mojang.text2speech.Narrator getNarrator() { return com.mojang.text2speech.Narrator.EMPTY; }
}

@TargetClass(className = "com.mojang.blaze3d.platform.MacosUtil")
final class Target_MacosUtil {
  @Substitute public static void exitNativeFullscreen(com.mojang.blaze3d.platform.Window window) { }
  @Substitute public static void clearResizableBit(com.mojang.blaze3d.platform.Window window) { }
  @Substitute public static void loadIcon(net.minecraft.server.packs.resources.IoSupplier<java.io.InputStream> icon) { }
}

@TargetClass(className = "com.mojang.blaze3d.platform.GLX")
final class Target_GLX {
  @Substitute public static String _getCpuInfo() {
    return Runtime.getRuntime().availableProcessors() + "x " + System.getProperty("os.arch") + " (native image, no OSHI)";
  }
}

@TargetClass(className = "net.minecraft.SystemReport")
final class Target_SystemReport {
  @Substitute private void lambda$new$6() { }   // putHardware(new oshi.SystemInfo())
  @Substitute private void lambda$new$7() { }   // putSoftware(new oshi.SystemInfo())
}

@TargetClass(className = "com.mojang.blaze3d.opengl.GlDebug")
final class Target_GlDebug {
  @Substitute public static com.mojang.blaze3d.opengl.GlDebug enableDebugCallback(int verbosity, boolean sync, java.util.Set<String> enabledExtensions) {
    return null;   // never register glDebugMessageCallback (no libffi closure)
  }
}

// tinyfd: Minecraft.<init> shows a message box when no usable GL device is found. No liblwjgl_tinyfd (AppKit) on
// visionOS: print to stderr instead.
@TargetClass(className = "org.lwjgl.util.tinyfd.TinyFileDialogs")
final class Target_TinyFileDialogs {
  // LWJGL 3.4.1: int return + int defaultButton (3.3.x: boolean/boolean)
  @Substitute public static int tinyfd_messageBox(CharSequence title, CharSequence message, CharSequence dialogType, CharSequence iconType, int defaultButton) {
    System.err.println("[tinyfd substituted] " + title + ": " + message);
    return 0;
  }
  @Substitute public static int tinyfd_setGlobalInt(CharSequence name, int value) { return 0; }
}

@TargetClass(className = "net.minecraft.util.NativeModuleLister")
final class Target_NativeModuleLister {
  @Substitute public static java.util.List<?> listModules() { return java.util.List.of(); }
}
