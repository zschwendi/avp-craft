// 26.2 copy of scripts/t4-src/subst/KlScreenWalker.java (M3 stage C): Mojang names (26.x is unobfuscated); onPress takes an
// InputWithModifiers (KeyEvent ENTER, as 1.21.11); the current screen moved from Minecraft.screen/setScreen to
// Minecraft.gui.screen()/gui.setScreen(); OptionsScreen takes (Screen, Options, boolean).
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
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.components.AbstractButton;
import net.minecraft.client.gui.components.events.GuiEventListener;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.gui.screens.TitleScreen;
import net.minecraft.client.gui.screens.options.OptionsScreen;

public final class KlScreenWalker implements Runnable {
  private static final String[] SKIP = {"quit", "open", "folder", "link", "reset", "default", "device", "narrator",
      "done", "back", "cancel", "disconnect", "realms", "feedback", "report", "guide", "license", "attribution", "website",
      "play mode", "load defaults"};   // "Play Mode" flips GuiMainVRSettings into its seated-confirm layout (2 buttons)
  // Every upstream (1.3.15) settings screen with a (Screen) constructor -- opened directly as root 3, because upstream
  // builds many as list entries / custom handlers the button walk cannot reach (the M3C host walk saw 2 of 14).
  private static final String[] VR_SCREENS = {"GuiAllSettings", "GuiBackpackSwitchingSettings", "GuiBindings",
      "GuiBlacklistEditor", "GuiChatNotificationSelection", "GuiClimbingSettings", "GuiDebugSettings",
      "GuiFreeMoveSettings", "GuiHUDSettings", "GuiKeyboardSettings", "GuiMenuWorldSettings", "GuiMixedRealitySettings",
      "GuiOtherHUDSettings", "GuiPlayerModelSettings", "GuiPostEffectsSettings", "GuiQuickCommandEditor",
      "GuiRadialConfiguration", "GuiRadialSettings", "GuiRenderingSettings", "GuiRenderOpticsSettings",
      "GuiRoomscaleSettings", "GuiSeatedOptions", "GuiServerSettings", "GuiShaderCompatSettings",
      "GuiStandingSettings", "GuiTeleportSettings", "GuiVRControls", "GuiWeaponCollisionSettings",
      "GuiActiveKeyboardLayoutSelector", "GuiKeyboardLayoutEditor", "GuiKeyboardThemeEditor", "VivecraftMainSettings"};
  private final long delayMs;
  private final long gapMs = Long.getLong("klepton.walk.gap", 150L);
  // per-screen press cap: GuiAllSettings lists every VR option (~435) but GuiChatNotificationSelection lists every
  // sound event (~3900); the cap keeps one walk inside a few minutes
  private final int maxPerScreen = Integer.getInteger("klepton.walk.max", 500);
  private int presses, errors;
  private Screen lastNow;

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
    Minecraft mc = Minecraft.getInstance();
    CompletableFuture<T> f = new CompletableFuture<>();
    mc.execute(() -> { try { f.complete(c.call()); } catch (Throwable t) { f.completeExceptionally(t); } });
    return f.get(20, TimeUnit.SECONDS);
  }

  private static List<AbstractButton> buttons(Screen s) {
    List<AbstractButton> out = new ArrayList<>();
    collect(s.children(), out, 0);
    return out;
  }

  // descend into lists / container entries (vanilla OptionsList, Sodium, upstream Vivecraft's settings lists)
  private static void collect(List<? extends GuiEventListener> kids, List<AbstractButton> out, int depth) {
    for (GuiEventListener e : kids) {
      if (e instanceof AbstractButton b) { if (b.active && b.visible) out.add(b); }
      else if (depth < 4 && e instanceof net.minecraft.client.gui.components.events.ContainerEventHandler c)
        collect(c.children(), out, depth + 1);
    }
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
        Minecraft mc = Minecraft.getInstance();
        if (mc != null && mc.gui != null && mc.gui.screen() instanceof TitleScreen) break;   // gui is null until Minecraft.<init> builds it
        if (System.currentTimeMillis() - t0 > 600_000) { log("gave up waiting for the title screen"); return; }
        Thread.sleep(500);
      }
      Thread.sleep(delayMs);
      Minecraft mc = Minecraft.getInstance();
      Screen title = mc.gui.screen();
      log("start (title " + title.getClass().getName() + ")");
      walkRoot("VR Settings", () -> new org.vivecraft.client.gui.settings.GuiMainVRSettings(title), 6);
      walkRoot("Options", () -> new OptionsScreen(title, mc.options, false), 2);
      for (String n : VR_SCREENS) {
        Class<?> c;
        try { c = Class.forName("org.vivecraft.client.gui.settings." + n); } catch (Throwable t) { log("no " + n); continue; }
        try {
          walkRoot("vr " + n, () -> {
            try { return (Screen) c.getConstructor(Screen.class).newInstance(title); }
            catch (ReflectiveOperationException e) { throw new RuntimeException(e); }
          }, 2);
        } catch (Throwable t) { errors++; log("root vr " + n + " failed: " + t); }
      }
      onRender(() -> { mc.gui.setScreen(title); return null; });
      log("done: " + presses + " presses, " + errors + " exceptions");
    } catch (Throwable t) {
      log("walker died: " + t);
      t.printStackTrace(System.out);
    }
  }

  private void walkRoot(String name, Supplier<Screen> make, int depth) throws Exception {
    Screen s = onRender(make::get);
    log("root " + name + " = " + s.getClass().getName());
    visit(s, depth, new ArrayDeque<>(), new HashSet<>());
  }

  private void visit(Screen s, int depth, ArrayDeque<Screen> path, Set<String> visited) throws Exception {
    Minecraft mc = Minecraft.getInstance();
    String sn = s.getClass().getName();
    visited.add(sn);
    path.push(s);
    onRender(() -> { mc.gui.setScreen(s); return null; });
    Thread.sleep(gapMs);
    int n = Math.min(maxPerScreen, onRender(() -> buttons(s).size()));
    log(path.size() + " " + sn + ": " + n + " button(s)");
    for (int i = 0; i < n; i++) {
      final int idx = i;
      String r;
      try {
        r = onRender(() -> {
          if (mc.gui.screen() != s) mc.gui.setScreen(s);
          List<AbstractButton> bs = buttons(s);
          if (idx >= bs.size()) return null;
          AbstractButton b = bs.get(idx);
          String label = b.getMessage().getString();
          if (skip(label)) { lastNow = null; return "\"" + label + "\" skipped"; }
          presses++;
          try {
            b.onPress(new net.minecraft.client.input.KeyEvent(257, 0, 0));   // onPress(InputWithModifiers) = a KeyEvent(ENTER)
          } catch (Throwable t) {
            errors++;
            lastNow = null;
            System.out.println("[walk] EXC pressing \"" + label + "\" on " + sn + ": " + t);
            t.printStackTrace(System.out);
            return "\"" + label + "\" -> EXC " + t;
          }
          Screen now = mc.gui.screen();
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
      Screen now = lastNow;
      if (now != null && now != s && !path.contains(now) && depth > 0 && !visited.contains(now.getClass().getName()))
        visit(now, depth - 1, path, visited);
    }
    path.pop();
  }
}
