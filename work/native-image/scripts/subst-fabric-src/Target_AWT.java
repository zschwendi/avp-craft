// Reconstructed from the upstream 1.0.1 launcher.jar used by AVPcraft builds 71/72.
// Preserve the native-image headless AWT substitutions, including annotations.
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;

@TargetClass(className = "java.awt.GraphicsEnvironment")
final class Target_java_awt_GraphicsEnvironment {
  @Substitute
  public static boolean isHeadless() { return true; }
  @Substitute
  static String getHeadlessMessage() { return "headless (native image)"; }
}

@TargetClass(className = "java.awt.Toolkit")
final class Target_java_awt_Toolkit {
  @Substitute
  static void loadLibraries() { }
  @Substitute
  private static void initIDs() { }
}
