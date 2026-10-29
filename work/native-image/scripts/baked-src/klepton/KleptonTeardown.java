// After Minecraft's main returns, two daemon threads keep the Graal isolate alive:
//   * EventLoopGroupHolder.LOCAL's MultiThreadIoEventLoopGroup ("Local IO #N")
//   * SoundEngineExecutor ("Sound engine") — SoundEngine.destroy() never calls shutDown()
// SVM's DestroyJavaVM interrupts every Java thread and waits for them to exit. An
// interrupt only makes their park() return, and with the shutdown flag still clear
// they spin. Shut them down here, on the way out of BakedLauncher.main, so the
// host's KL_MC_TEARDOWN=1 path can unmap the heap.
//
// Every step is bounded and swallowed. A failure leaves the parked-isolate path
// exactly as it was. -Dklepton.teardown=false skips the whole thing.
package klepton;

import java.lang.reflect.Field;
import java.util.concurrent.TimeUnit;

import io.netty.channel.EventLoopGroup;
import io.netty.util.concurrent.Future;
import net.minecraft.client.Minecraft;
import net.minecraft.client.sounds.SoundEngine;
import net.minecraft.client.sounds.SoundEngineExecutor;
import net.minecraft.client.sounds.SoundManager;
import net.minecraft.server.network.EventLoopGroupHolder;

public final class KleptonTeardown {
  private KleptonTeardown() {}

  public static void afterMain() {
    if ("false".equalsIgnoreCase(System.getProperty("klepton.teardown", "true"))) {
      System.err.println("[kl-teardown] skipped (-Dklepton.teardown=false)");
      return;
    }
    long t0 = System.nanoTime();
    String sound = shutdownSound();
    String loops = shutdownEventLoops();
    long ms = (System.nanoTime() - t0) / 1_000_000L;
    System.err.println("[kl-teardown] " + sound + "; " + loops + "; " + ms + " ms");
    System.err.flush();
  }

  private static String shutdownSound() {
    try {
      Minecraft mc = Minecraft.getInstance();
      if (mc == null) return "sound: no Minecraft";
      SoundManager sm = mc.getSoundManager();
      if (sm == null) return "sound: no SoundManager";
      SoundEngine engine = (SoundEngine) field(SoundManager.class, "soundEngine").get(sm);
      if (engine == null) return "sound: no SoundEngine";
      SoundEngineExecutor exec = (SoundEngineExecutor) field(SoundEngine.class, "executor").get(engine);
      if (exec == null) return "sound: no executor";
      Thread t = new Thread(exec::shutDown, "kl-teardown-sound");
      t.setDaemon(true);
      t.start();
      t.join(3000);
      return t.isAlive() ? "sound: shutDown still joining after 3 s" : "sound: executor shut down";
    } catch (Throwable t) {
      return "sound: " + t.getClass().getSimpleName() + ": " + t.getMessage();
    }
  }

  private static String shutdownEventLoops() {
    StringBuilder sb = new StringBuilder();
    for (String name : new String[] {"LOCAL", "NIO", "EPOLL", "KQUEUE"}) {
      if (sb.length() > 0) sb.append(", ");
      sb.append(name).append(": ").append(shutdownOne(name));
    }
    return sb.toString();
  }

  private static String shutdownOne(String staticName) {
    try {
      EventLoopGroupHolder holder =
          (EventLoopGroupHolder) field(EventLoopGroupHolder.class, staticName).get(null);
      if (holder == null) return "no holder";
      EventLoopGroup group = (EventLoopGroup) field(EventLoopGroupHolder.class, "group").get(holder);
      if (group == null) return "not started";
      Future<?> f = group.shutdownGracefully(0, 200, TimeUnit.MILLISECONDS);
      boolean done = f.await(3, TimeUnit.SECONDS);
      return done ? "shutdownGracefully" : "await timed out";
    } catch (Throwable t) {
      return t.getClass().getSimpleName() + ": " + t.getMessage();
    }
  }

  private static Field field(Class<?> c, String name) throws ReflectiveOperationException {
    Field f = c.getDeclaredField(name);
    f.setAccessible(true);
    return f;
  }
}
