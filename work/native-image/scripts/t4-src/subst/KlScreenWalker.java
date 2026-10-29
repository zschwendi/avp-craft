// KlScreenWalker: a HOST TEST HARNESS (not a substitution; it lives here because build-t4.sh compiles subst/*.java into
// baked/t4). Off unless -Dklepton.walk=<seconds> is given (SpikeT23 starts it); the device launch.txt never sets it.
// Once the title screen is up (+<seconds>), it walks the menus the tracing runs never reached, pressing every active
// button the way a click would (AbstractButton.onPress on the render thread), depth-first:
//   root 1: Vivecraft's VR Settings (GuiMainVRSettings) -- every sub-screen is opened by GuiVROptionsBase through
//           getConstructor(Screen.class).newInstance (launch 3's "error setting screen"), every option button cycles
//           an OptionEnum (getMethod("values")).  Depth 6.
//   root 2: vanilla Options (depth 2), which also holds Vivecraft's "VR Settings..." and Sodium's video screen.
// Buttons whose label matches SKIP (quit, folders/links, resets, sound device, narrator, done/back/cancel) are not
// pressed. One line per press: "[walk] <depth> <screen> #i "<label>" -> same|<new screen>|EXC ...". Pressing cycles
// real settings: the caller must snapshot/restore fgamedir/options.txt + config/ (scripts/t8/walk-host.sh does).
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.Callable;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;
import net.minecraft.class_310;
import net.minecraft.class_364;
import net.minecraft.class_429;
import net.minecraft.class_437;
import net.minecraft.class_442;
import net.minecraft.class_4264;

public final class KlScreenWalker implements Runnable {
  private static final String[] SKIP = {"quit", "open", "folder", "link", "reset", "default", "device", "narrator",
      "done", "back", "cancel", "disconnect", "realms", "feedback", "report", "guide", "license", "attribution", "website"};
  private final long delayMs;
  private final long gapMs = Long.getLong("klepton.walk.gap", 150L);
  private int presses, errors;
  private class_437 lastNow;

  private KlScreenWalker(long delayMs) { this.delayMs = delayMs; }

  public static void startIfAsked() {
    String p = System.getProperty("klepton.walk");
    if (p == null) return;
    Thread t = new Thread(new KlScreenWalker((long) (Double.parseDouble(p) * 1000)), "kl-walker");
    t.setDaemon(true);
    t.start();
  }

  private static void log(String s) { System.out.println("[walk] " + s); System.out.flush(); }

  private static <T> T onRender(Callable<T> c) throws Exception {
    class_310 mc = class_310.method_1551();
    CompletableFuture<T> f = new CompletableFuture<>();
    mc.execute(() -> { try { f.complete(c.call()); } catch (Throwable t) { f.completeExceptionally(t); } });
    return f.get(20, TimeUnit.SECONDS);
  }

  private static List<class_4264> buttons(class_437 s) {
    List<class_4264> out = new ArrayList<>();
    for (class_364 e : s.method_25396())
      if (e instanceof class_4264 b && b.field_22763 && b.field_22764) out.add(b);
    return out;
  }

  private static boolean skip(String label) {
    String l = label.toLowerCase(Locale.ROOT);
    for (String s : SKIP) if (l.contains(s)) return true;
    return false;
  }

  @Override public void run() {
    try {
      long t0 = System.currentTimeMillis();
      while (true) {
        class_310 mc = class_310.method_1551();
        if (mc != null && mc.field_1755 instanceof class_442) break;
        if (System.currentTimeMillis() - t0 > 600_000) { log("gave up waiting for the title screen"); return; }
        Thread.sleep(500);
      }
      Thread.sleep(delayMs);
      class_310 mc = class_310.method_1551();
      class_437 title = mc.field_1755;
      log("start (title " + title.getClass().getName() + ")");
      walkRoot("VR Settings", () -> new org.vivecraft.client.gui.settings.GuiMainVRSettings(title), 6);
      walkRoot("Options", () -> new class_429(title, mc.field_1690), 2);
      onRender(() -> { mc.method_1507(title); return null; });
      log("done: " + presses + " presses, " + errors + " exceptions");
    } catch (Throwable t) {
      log("walker died: " + t);
      t.printStackTrace(System.out);
    }
  }

  private void walkRoot(String name, Supplier<class_437> make, int depth) throws Exception {
    class_437 s = onRender(make::get);
    log("root " + name + " = " + s.getClass().getName());
    visit(s, depth, new ArrayDeque<>(), new HashSet<>());
  }

  private void visit(class_437 s, int depth, ArrayDeque<class_437> path, Set<String> visited) throws Exception {
    class_310 mc = class_310.method_1551();
    String sn = s.getClass().getName();
    visited.add(sn);
    path.push(s);
    onRender(() -> { mc.method_1507(s); return null; });
    Thread.sleep(gapMs);
    int n = onRender(() -> buttons(s).size());
    log(path.size() + " " + sn + ": " + n + " button(s)");
    for (int i = 0; i < n; i++) {
      final int idx = i;
      String r;
      try {
        r = onRender(() -> {
          if (mc.field_1755 != s) mc.method_1507(s);
          List<class_4264> bs = buttons(s);
          if (idx >= bs.size()) return null;
          class_4264 b = bs.get(idx);
          String label = b.method_25369().getString();
          if (skip(label)) { lastNow = null; return "\"" + label + "\" skipped"; }
          presses++;
          try {
            b.method_25306();
          } catch (Throwable t) {
            errors++;
            lastNow = null;
            System.out.println("[walk] EXC pressing \"" + label + "\" on " + sn + ": " + t);
            t.printStackTrace(System.out);
            return "\"" + label + "\" -> EXC " + t;
          }
          class_437 now = mc.field_1755;
          lastNow = now;
          return "\"" + label + "\" -> " + (now == s ? "same" : now == null ? "null" : now.getClass().getName());
        });
      } catch (Throwable t) {
        errors++;
        log(path.size() + " " + sn + " #" + i + " task failed: " + t);
        continue;
      }
      if (r == null) break;
      log(path.size() + " " + sn + " #" + i + " " + r);
      Thread.sleep(gapMs);
      class_437 now = lastNow;
      if (now != null && now != s && !path.contains(now) && depth > 0 && !visited.contains(now.getClass().getName()))
        visit(now, depth - 1, path, visited);
    }
    path.pop();
  }
}
