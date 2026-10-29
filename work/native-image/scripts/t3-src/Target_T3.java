// T3 closed-world trim (native-image only; intermediary names, see T2T3-REPORT.md for the mojmap -> intermediary table).
// Each substitution removes a runtime dependency that cannot exist on visionOS:
//   Narrator.getNarrator        -> Narrator.EMPTY            (NarratorMac = java-objc-bridge -> JNA -> AppKit)
//   MacosUtil (class_6417)      -> no-ops                     (exitNativeFullscreen / clearResizableBit / loadIcon via objc bridge)
//   GLX._getCpuInfo             -> no OSHI                    (OSHI SystemInfo -> JNA + IOKit)
//   SystemReport hardware group (class_6396.method_68673 = lambda$new$2 'putHardware(new SystemInfo())') -> skipped
//   GlDebug.enableDebugCallback (class_1008.method_4227)  -> null (no GL debug closure; vanilla's verbosity<=0 result)
//   TinyFileDialogs.tinyfd_messageBox / tinyfd_setGlobalInt -> stderr / 0 (no liblwjgl_tinyfd)
//   NativeModuleLister.listModules (class_6498.method_37961) -> empty (Windows-only JNA)
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;

@TargetClass(className = "com.mojang.text2speech.Narrator")
final class Target_Narrator {
  @Substitute public static com.mojang.text2speech.Narrator getNarrator() { return com.mojang.text2speech.Narrator.EMPTY; }
}

@TargetClass(className = "net.minecraft.class_6417") // com.mojang.blaze3d.platform.MacosUtil
final class Target_MacosUtil {
  @Substitute public static void method_37461(long window) { }   // exitNativeFullscreen
  @Substitute public static void method_54101(long window) { }   // clearResizableBit
  @Substitute public static void method_41718(net.minecraft.class_7367<java.io.InputStream> icon) { } // loadIcon
}

@TargetClass(className = "com.mojang.blaze3d.platform.GLX")
final class Target_GLX {
  @Substitute public static String _getCpuInfo() {
    return Runtime.getRuntime().availableProcessors() + "x " + System.getProperty("os.arch") + " (native image, no OSHI)";
  }
}

@TargetClass(className = "net.minecraft.class_6396") // net.minecraft.SystemReport
final class Target_SystemReport {
  @Substitute private void method_68673() { }   // lambda$new$2: putHardware(new oshi.SystemInfo())
}

@TargetClass(className = "net.minecraft.class_1008") // com.mojang.blaze3d.opengl.GlDebug
final class Target_GlDebug {
  @Substitute public static net.minecraft.class_1008 method_4227(int verbosity, boolean sync, java.util.Set<String> enabledExtensions) {
    return null;   // enableDebugCallback: never register glDebugMessageCallback (no libffi closure)
  }
}

// tinyfd: Minecraft.<init> shows a message box when no usable GL device is found. No liblwjgl_tinyfd (AppKit) on
// visionOS: print to stderr instead.
@TargetClass(className = "org.lwjgl.util.tinyfd.TinyFileDialogs")
final class Target_TinyFileDialogs {
  @Substitute public static boolean tinyfd_messageBox(CharSequence title, CharSequence message, CharSequence dialogType, CharSequence iconType, boolean defaultButton) {
    System.err.println("[tinyfd substituted] " + title + ": " + message);
    return false;
  }
  @Substitute public static int tinyfd_setGlobalInt(CharSequence name, int value) { return 0; }
}

// NativeModuleLister (class_6498): Main.main adds its crash-report section at startup; listModules is Windows-only
// JNA code (Kernel32/Version). Empty list, no JNA.
@TargetClass(className = "net.minecraft.class_6498") // net.minecraft.util.NativeModuleLister
final class Target_NativeModuleLister {
  @Substitute public static java.util.List<?> method_37961() { return java.util.List.of(); }   // listModules
}
