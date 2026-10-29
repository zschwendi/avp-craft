// BakeAgent: javaagent for the Fabric/Knot JVM run. Produces the post-Knot (remap + access widener +
// game-provider entrypoint patch + Mixin) bytes of every game/mod class, so a closed-world build can
// run without Knot or Mixin.
//   -javaagent:bakeagent.jar=<outdir>   -Dbake.fullAt=<seconds> (default 40)
// 1) ClassFileTransformer: every class DEFINED by KnotClassLoader -> <out>/classes/<name>.class
//    (these are the exact bytes passed to defineClass); every class defined by any OTHER non-JDK loader
//    is logged to <out>/other-loaders.txt (runtime class generation detector).
// 2) At fullAt seconds: walk FabricLauncher.getClassPath() and push every not-yet-defined class through
//    KnotClassDelegate.getPostMixinClassByteArray(name,false) WITHOUT defining it -> <out>/classes/.
//    Non-interface @Mixin classes are skipped (Mixin forbids loading them); mixin interfaces (accessors)
//    are kept as raw bytes.
import java.lang.instrument.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.security.ProtectionDomain;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.stream.*;

public class BakeAgent {
  static Path out;
  static final Set<String> defined = ConcurrentHashMap.newKeySet();
  static final Map<String, Integer> otherLoaders = new ConcurrentHashMap<>();

  public static void premain(String args, Instrumentation inst) throws Exception {
    out = Paths.get(args == null ? "bake" : args);
    Files.createDirectories(out.resolve("classes"));
    inst.addTransformer(new ClassFileTransformer() {
      public byte[] transform(ClassLoader l, String name, Class<?> redefined, ProtectionDomain pd, byte[] b) {
        if (l == null || name == null) return null;
        String ln = l.getClass().getName();
        if (ln.contains("KnotClassLoader")) {
          write(name, b); defined.add(name);
        } else if (!ln.startsWith("jdk.internal.loader.") && !ln.equals("jdk.internal.reflect.DelegatingClassLoader")) {
          otherLoaders.merge(ln + " :: " + name, 1, Integer::sum);
        }
        return null;
      }
    });
    long at = Long.getLong("bake.fullAt", 40);
    Thread t = new Thread(() -> {
      try { Thread.sleep(at * 1000); fullPass(); } catch (Throwable e) { e.printStackTrace(); }
    }, "bake-fullpass");
    t.setDaemon(true); t.start();
  }

  static void write(String name, byte[] b) {
    try {
      Path p = out.resolve("classes").resolve(name + ".class");
      Files.createDirectories(p.getParent());
      Files.write(p, b);
    } catch (Exception e) { e.printStackTrace(); }
  }

  static boolean isMixinNonInterface(byte[] raw) {
    String s = new String(raw, java.nio.charset.StandardCharsets.ISO_8859_1);
    if (!s.contains("Lorg/spongepowered/asm/mixin/Mixin;")) return false;
    // access_flags follow the constant pool; cheap check via ASM is safer
    org.objectweb.asm.ClassReader cr = new org.objectweb.asm.ClassReader(raw);
    return (cr.getAccess() & org.objectweb.asm.Opcodes.ACC_INTERFACE) == 0;
  }

  static void fullPass() throws Exception {
    ClassLoader cl = Thread.currentThread().getContextClassLoader();
    Class<?> flb = Class.forName("net.fabricmc.loader.impl.launch.FabricLauncherBase", true, ClassLoader.getSystemClassLoader());
    Object launcher = flb.getMethod("getLauncher").invoke(null);
    ClassLoader knot = (ClassLoader) launcher.getClass().getMethod("getTargetClassLoader").invoke(launcher);
    Method gd = knot.getClass().getDeclaredMethod("getDelegate"); gd.setAccessible(true);
    Object delegate = gd.invoke(knot);
    // Knot's own code sources (remapped game jar, mods, extracted nested jars) -- NOT the launcher's -cp
    Field csf = delegate.getClass().getDeclaredField("codeSources"); csf.setAccessible(true);
    @SuppressWarnings("unchecked")
    List<Path> cp = new ArrayList<>((Set<Path>) csf.get(delegate));
    Method post = delegate.getClass().getDeclaredMethod("getPostMixinClassByteArray", String.class, boolean.class); post.setAccessible(true);
    Method raw = delegate.getClass().getDeclaredMethod("getRawClassByteArray", String.class, boolean.class); raw.setAccessible(true);
    List<String> log = new ArrayList<>();
    int ok = 0, skippedMixin = 0, already = 0, fail = 0, nul = 0;
    List<String> cpLines = new ArrayList<>();
    for (Path p : cp) {
      cpLines.add(p.toString());
      List<String> names = new ArrayList<>();
      if (Files.isDirectory(p)) {
        try (Stream<Path> s = Files.walk(p)) { s.filter(x -> x.toString().endsWith(".class")).forEach(x -> names.add(p.relativize(x).toString())); }
      } else if (p.toString().endsWith(".jar")) {
        try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(p.toFile())) {
          z.stream().map(e -> e.getName()).filter(n -> n.endsWith(".class") && !n.startsWith("META-INF/")).forEach(names::add);
        }
      }
      for (String f : names) {
        String in = f.substring(0, f.length() - 6);
        if (in.equals("module-info") || in.endsWith("package-info")) continue;
        if (defined.contains(in)) { already++; continue; }
        String dn = in.replace('/', '.');
        try {
          byte[] r = (byte[]) raw.invoke(delegate, dn, false);
          if (r == null) { nul++; continue; }
          if (isMixinNonInterface(r)) { skippedMixin++; continue; }
          byte[] b = (byte[]) post.invoke(delegate, dn, false);
          if (b == null) { nul++; continue; }
          write(in, b); ok++;
        } catch (Throwable e) {
          fail++;
          Throwable c = e instanceof InvocationTargetException ? e.getCause() : e;
          log.add(in + " :: " + c);
        }
      }
    }
    // 3) generated classes (Mixin inner-class copies "$Anonymous$<hash>", MixinExtras LocalRef impls,
    //    ModifyArgs Args subclasses): find class refs in everything dumped that exist nowhere yet, ask Knot
    //    for them (Knot falls back to the Mixin class generator), repeat until closure.
    int gen = 0;
    for (int round = 0; round < 5; round++) {
      Set<String> have = new HashSet<>();
      List<Path> files;
      try (Stream<Path> s = Files.walk(out.resolve("classes"))) { files = s.filter(x -> x.toString().endsWith(".class")).collect(Collectors.toList()); }
      for (Path f : files) { String r = out.resolve("classes").relativize(f).toString(); have.add(r.substring(0, r.length() - 6)); }
      Set<String> want = new TreeSet<>();
      for (Path f : files) {
        org.objectweb.asm.ClassReader cr = new org.objectweb.asm.ClassReader(Files.readAllBytes(f));
        char[] buf = new char[cr.getMaxStringLength()];
        for (int i = 1; i < cr.getItemCount(); i++) {
          int off = cr.getItem(i);
          if (off > 0 && cr.readByte(off - 1) == 7) {
            String n = cr.readUTF8(off, buf);
            if (n != null && !n.startsWith("[") && (n.contains("$Anonymous$") || n.contains("/generated/") || n.contains("synthetic/args")) && !have.contains(n)) want.add(n);
          }
        }
      }
      if (want.isEmpty()) break;
      for (String n : want) {
        try {
          byte[] b = (byte[]) post.invoke(delegate, n.replace('/', '.'), false);
          if (b != null) { write(n, b); gen++; } else log.add("GEN-NULL " + n);
        } catch (Throwable e) { Throwable c = e instanceof InvocationTargetException ? e.getCause() : e; log.add("GEN-FAIL " + n + " :: " + c); }
      }
    }
    System.out.println("[BAKE] generated classes recovered: " + gen);
    Files.write(out.resolve("knot-classpath.txt"), cpLines);
    Files.write(out.resolve("fullpass-failures.txt"), log);
    List<String> ol = otherLoaders.entrySet().stream().map(e -> e.getKey() + " x" + e.getValue()).sorted().collect(Collectors.toList());
    Files.write(out.resolve("other-loaders.txt"), ol);
    String summary = "[BAKE] defined-at-runtime=" + defined.size() + " fullpass-transformed=" + ok + " already=" + already
        + " skipped-mixin-classes=" + skippedMixin + " null=" + nul + " failed=" + fail + " other-loader-classes=" + ol.size();
    Files.write(out.resolve("summary.txt"), List.of(summary));
    System.out.println(summary);
  }
}
