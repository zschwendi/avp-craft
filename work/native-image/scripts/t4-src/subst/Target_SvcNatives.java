// Simple Voice Chat natives without the jar-extract-to-tmp loader (native-image only; vivecraft SVC round).
//   opus4j / rnnoise4j / speex4j / lame4j each carry a LibraryLoader that picks natives/<os>-<arch>/lib<name>.dylib
//   out of the jar, writes it to java.io.tmpdir and System.load()s it. In this image the jar resources are not
//   embedded, the mac-aarch64 slice is the wrong platform for visionOS, and a dylib written to tmp would not pass
//   code signing on device. The substitutions load lib<name>.dylib from java.library.path instead (the app's
//   Frameworks dir, where mknative.sh wraps work/natives/svc/<sdk>/lib<name>.dylib and the app build signs it).
//   JNI access the C glue needs (exception classes, "[S", OpusEncoder$Application.value) is in
//   config/extra/svc-jni.json; rnnoise's weights resource is in config/extra/svc-resources.json.
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;
import java.io.File;
import java.io.IOException;

final class KlSvcNatives {
  static void load(String libName) throws IOException {
    String name = System.mapLibraryName(libName.startsWith("lib") ? libName.substring(3) : libName);
    String path = System.getProperty("java.library.path", "");
    for (String dir : path.split(File.pathSeparator)) {
      if (dir.isEmpty()) continue;
      File f = new File(dir, name);
      if (!f.isFile()) continue;
      try {
        System.load(f.getAbsolutePath());
      } catch (UnsatisfiedLinkError e) {
        System.err.println("[kl-svc] " + libName + ": System.load(" + f + ") failed: " + e.getMessage());
        throw new IOException("Could not load " + f + ": " + e.getMessage(), e);
      }
      System.err.println("[kl-svc] " + libName + " loaded from " + f);
      return;
    }
    System.err.println("[kl-svc] " + libName + ": " + name + " not in java.library.path=" + path);
    throw new IOException(name + " not found in java.library.path (" + path + ")");
  }
}

@TargetClass(className = "de.maxhenkel.opus4j.LibraryLoader")
final class Target_Opus4jLibraryLoader {
  @Substitute
  public static void load(String libName) throws IOException { KlSvcNatives.load(libName); }
}

@TargetClass(className = "de.maxhenkel.rnnoise4j.LibraryLoader")
final class Target_RNNoise4jLibraryLoader {
  @Substitute
  public static void load(String libName) throws IOException { KlSvcNatives.load(libName); }
}

@TargetClass(className = "de.maxhenkel.speex4j.LibraryLoader")
final class Target_Speex4jLibraryLoader {
  @Substitute
  public static void load(String libName) throws IOException { KlSvcNatives.load(libName); }
}

@TargetClass(className = "de.maxhenkel.lame4j.LibraryLoader")
final class Target_Lame4jLibraryLoader {
  @Substitute
  public static void load(String libName) throws IOException { KlSvcNatives.load(libName); }
}
