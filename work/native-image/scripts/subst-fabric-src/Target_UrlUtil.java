// Reconstructed from the upstream 1.0.1 launcher.jar used by AVPcraft builds 71/72.
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;
import java.net.URL;
import java.nio.file.Path;
import java.nio.file.Paths;

@TargetClass(className = "net.fabricmc.loader.impl.util.UrlUtil")
final class Target_UrlUtil {
  @Substitute
  public static Path getCodeSource(URL url, String localPath) {
    if (url != null && !"resource".equals(url.getProtocol())) {
      try {
        if ("jar".equals(url.getProtocol())) {
          String value = url.toString();
          int separator = value.indexOf("!/");
          return Paths.get(new URL(value.substring(4, separator)).toURI());
        }
        String value = url.toURI().getPath();
        if (value.endsWith(localPath)) {
          return Paths.get(value.substring(0, value.length() - localPath.length()));
        }
      } catch (Exception failure) {
        throw new RuntimeException(failure);
      }
    }
    return BakedCodeSource.find(localPath);
  }

  @Substitute
  public static Path getCodeSource(Class<?> type) {
    return BakedCodeSource.find(type.getName().replace('.', '/') + ".class");
  }
}
