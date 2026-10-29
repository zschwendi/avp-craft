// BakedLauncher: replaces Knot for a classpath whose game/mod classes were pre-transformed by BakeAgent
// (remap + access wideners + entrypoint patch + Mixin all baked). No custom classloader, no Mixin bootstrap,
// no class transformation at runtime. Mirrors Knot.init() minus the transformer/Mixin steps.
// Props: -Dbaked.classpath=<path list> (used when java.class.path is empty, i.e. in a native image)
//        -Dfabric.gameMappingNamespace=intermediary (game jar is already the intermediary-remapped jar)
package net.fabricmc.loader.impl.launch;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.jar.Manifest;

import net.fabricmc.api.EnvType;
import net.fabricmc.loader.api.entrypoint.PreLaunchEntrypoint;
import net.fabricmc.loader.impl.FabricLoaderImpl;
import net.fabricmc.loader.impl.game.GameProvider;
import net.fabricmc.loader.impl.game.minecraft.MinecraftGameProvider;
import net.fabricmc.loader.impl.util.log.Log;
import net.fabricmc.loader.impl.util.log.LogCategory;

public final class BakedLauncher extends FabricLauncherBase {
  private final List<Path> classPath = new ArrayList<>();
  private GameProvider provider;
  private ClassLoader cl;

  public static void main(String[] args) {
    setupUncaughtExceptionHandler();
    BakedLauncher l = new BakedLauncher();
    ClassLoader cl = l.init(args);
    try {
      l.provider.launch(cl);
    } finally {
      // After Main.main returns (or throws). Shuts the daemon threads DestroyJavaVM
      // otherwise waits on forever. No-op when -Dklepton.teardown=false.
      klepton.KleptonTeardown.afterMain();
    }
  }

  private ClassLoader init(String[] args) {
    setProperties(new HashMap<>());
    String cpProp = System.getProperty("baked.classpath");
    if (cpProp == null || cpProp.isEmpty()) cpProp = System.getProperty("java.class.path", "");
    for (String e : cpProp.split(File.pathSeparator)) {
      if (e.isEmpty()) continue;
      Path p = Paths.get(e);
      if (Files.exists(p)) classPath.add(p.toAbsolutePath().normalize());
    }
    provider = new MinecraftGameProvider();
    if (!provider.locateGame(this, args)) throw new RuntimeException("BakedLauncher: MinecraftGameProvider could not locate the game");
    Log.finishBuiltinConfig();
    Log.info(LogCategory.GAME_PROVIDER, "[BAKED] Loading %s %s with Fabric Loader %s (no Knot, no Mixin)", provider.getGameName(), provider.getRawGameVersion(), FabricLoaderImpl.VERSION);
    cl = BakedLauncher.class.getClassLoader();
    Thread.currentThread().setContextClassLoader(cl);
    FabricLoaderImpl loader = FabricLoaderImpl.INSTANCE;
    loader.setGameProvider(provider);
    provider.initialize(this);
    loader.load();
    loader.freeze();
    loader.loadClassTweakers();
    runMixinPluginOnLoad(loader);
    provider.unlockClassPath(this);
    loader.invokeEntrypoints("preLaunch", PreLaunchEntrypoint.class, PreLaunchEntrypoint::onPreLaunch);
    return cl;
  }

  // Mixin config plugins often have side effects in onLoad() (config loading, early init). Mixin is not
  // bootstrapped at runtime, so call onLoad() ourselves. Mixin-machinery plugins are skipped.
  private void runMixinPluginOnLoad(FabricLoaderImpl loader) {
    for (net.fabricmc.loader.impl.ModContainerImpl mod : loader.getModsInternal()) {
      for (String cfg : mod.getMetadata().getMixinConfigs(EnvType.CLIENT)) {
        try {
          Path p = null;
          for (Path root : mod.getRootPaths()) { Path c = root.resolve(cfg); if (Files.exists(c)) { p = c; break; } }
          if (p == null) continue;
          com.google.gson.JsonObject o = com.google.gson.JsonParser.parseString(Files.readString(p)).getAsJsonObject();
          if (!o.has("plugin")) continue;
          String plugin = o.get("plugin").getAsString();
          if (plugin.startsWith("com.llamalad7.mixinextras") || plugin.startsWith("com.bawnorton.mixinsquared")) continue;
          if (!seenPlugins.add(plugin + "|" + cfg)) continue;
          Object inst = Class.forName(plugin, true, cl).getDeclaredConstructor().newInstance();
          String pkg = o.has("package") ? o.get("package").getAsString() : "";
          inst.getClass().getMethod("onLoad", String.class).invoke(inst, pkg);
          Log.info(LogCategory.KNOT, "[BAKED] mixin plugin onLoad: %s (%s)", plugin, cfg);
        } catch (Throwable t) {
          Log.warn(LogCategory.KNOT, "[BAKED] mixin plugin onLoad failed for %s/%s: %s", mod.getMetadata().getId(), cfg, t);
        }
      }
    }
  }
  private final Set<String> seenPlugins = new HashSet<>();

  @Override public List<Path> getClassPath() { return classPath; }
  @Override public void addToClassPath(Path path, String... allowedPrefixes) { Log.debug(LogCategory.KNOT, "[BAKED] addToClassPath %s (no-op)", path); }
  @Override public void setAllowedPrefixes(Path path, String... prefixes) { }
  @Override public void setValidParentClassPath(Collection<Path> paths) { }
  @Override public EnvType getEnvironmentType() { return EnvType.CLIENT; }
  @Override public boolean isClassLoaded(String name) {
    try { Class.forName(name, false, cl); return true; } catch (Throwable t) { return false; }
  }
  @Override public Class<?> loadIntoTarget(String name) throws ClassNotFoundException { return Class.forName(name, true, cl); }
  @Override public InputStream getResourceAsStream(String name) { return cl.getResourceAsStream(name); }
  @Override public ClassLoader getTargetClassLoader() { return cl; }
  @Override public byte[] getClassByteArray(String name, boolean runTransformers) throws IOException {
    try (InputStream is = cl.getResourceAsStream(name.replace('.', '/') + ".class")) {
      return is == null ? null : is.readAllBytes();
    }
  }
  @Override public Manifest getManifest(Path originPath) { return null; }
  @Override public String getEntrypoint() { return provider.getEntrypoint(); }
}
