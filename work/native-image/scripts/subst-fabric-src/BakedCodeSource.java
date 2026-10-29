// Reconstructed from the upstream 1.0.1 launcher.jar used by AVPcraft builds 71/72.
// Finds original classpath roots for native-image resource URLs.
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipFile;

public final class BakedCodeSource {
  private static List<Path> cp;
  private static final Map<Path, ZipFile> zips = new HashMap<>();

  public static synchronized Path find(String name) {
    if (cp == null) {
      cp = new ArrayList<>();
      for (String entry : System.getProperty("baked.classpath", "").split(File.pathSeparator)) {
        if (entry.isEmpty() || entry.endsWith("/bake/classes") || entry.endsWith("/baked/launcher")) continue;
        cp.add(Paths.get(entry));
      }
    }
    for (Path root : cp) {
      try {
        if (Files.isDirectory(root)) {
          if (Files.exists(root.resolve(name))) return root;
          continue;
        }
        ZipFile zip = zips.get(root);
        if (zip == null) {
          zip = new ZipFile(root.toFile());
          zips.put(root, zip);
        }
        if (zip.getEntry(name) != null) return root;
      } catch (Exception ignored) {
      }
    }
    return null;
  }
}
