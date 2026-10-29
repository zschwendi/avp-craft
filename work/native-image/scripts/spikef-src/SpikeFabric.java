// Reconstructed from the upstream 1.0.1 launcher.jar used by AVPcraft builds 71/72.
// Preserves the original entry point, daemon monitor and screenshot scheduling.
import java.util.Iterator;
import java.util.Set;
import java.util.TreeSet;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Screenshot;
import net.fabricmc.loader.impl.launch.BakedLauncher;

public class SpikeFabric {
  public static void main(String[] args) throws Exception {
    String shots = System.getProperty("spike.shots", "25");
    Thread monitor = new Thread(() -> {
      long started = System.currentTimeMillis();
      Set<Long> pending = new TreeSet<>();
      for (String shot : shots.split(",")) {
        if (!shot.isEmpty()) pending.add(Long.parseLong(shot.trim()));
      }
      for (;;) {
        try {
          Thread.sleep(1000L);
        } catch (InterruptedException interrupted) {
          return;
        }
        long seconds = (System.currentTimeMillis() - started) / 1000L;
        Minecraft minecraft;
        try {
          minecraft = Minecraft.getInstance();
        } catch (Throwable unavailable) {
          continue;
        }
        if (minecraft == null) continue;
        System.out.println("[SPIKE] t=" + seconds + "s fps=" + minecraft.getFps());
        Iterator<Long> iterator = pending.iterator();
        if (iterator.hasNext() && seconds >= iterator.next()) {
          iterator.remove();
          long capturedSeconds = seconds;
          Minecraft capturedMinecraft = minecraft;
          minecraft.execute(() -> {
            Screenshot.grab(capturedMinecraft, false);
            System.out.println("[SPIKE] screenshot@" + capturedSeconds + "s");
          });
        }
      }
    }, "spike-monitor");
    monitor.setDaemon(true);
    monitor.start();
    BakedLauncher.main(args);
  }
}
