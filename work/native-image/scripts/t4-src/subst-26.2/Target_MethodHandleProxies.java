// M3C (26.2, JDK 25): MethodHandleProxies.asInterfaceInstance spins a hidden class per call since JDK 22
// ("jdk.MHProxy1.ModInitializer"), which a native image cannot define at runtime. Fabric Loader's DefaultLanguageAdapter
// uses it for every "Class::method" entrypoint (Fabric API networking: main + client), so the first image run died in
// Minecraft.<init> -> Hooks.startClient. JDK 21 (1.21.x) built a java.lang.reflect.Proxy instead; this does the same.
// The proxy classes must be registered at build time: scripts/t23/filter-config.py adds {"proxy":[ModInitializer]} and
// {"proxy":[ClientModInitializer]} (the only interfaces with method-reference entrypoints in the 26.2 mod set).
import com.oracle.svm.core.annotate.Substitute;
import com.oracle.svm.core.annotate.TargetClass;
import java.lang.invoke.MethodHandle;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.Proxy;

@TargetClass(className = "java.lang.invoke.MethodHandleProxies")
final class Target_MethodHandleProxies {
  @Substitute
  public static <T> T asInterfaceInstance(Class<T> intf, MethodHandle target) {
    if (!intf.isInterface()) throw new IllegalArgumentException("not an interface: " + intf.getName());
    Method sam = null;
    for (Method m : intf.getMethods())
      if (Modifier.isAbstract(m.getModifiers())) {
        if (sam != null && !sam.getName().equals(m.getName()))
          throw new IllegalArgumentException("not a single-method interface: " + intf.getName());
        sam = m;
      }
    if (sam == null) throw new IllegalArgumentException("no abstract method: " + intf.getName());
    final String samName = sam.getName();
    final MethodHandle h = target.asType(java.lang.invoke.MethodType.methodType(sam.getReturnType(), sam.getParameterTypes()));
    Object p = Proxy.newProxyInstance(intf.getClassLoader(), new Class<?>[] {intf}, (proxy, m, args) -> {
      if (m.getDeclaringClass() == Object.class) {
        switch (m.getName()) {
          case "hashCode": return System.identityHashCode(proxy);
          case "equals": return proxy == args[0];
          default: return intf.getName() + "@MHProxy(" + h + ")";
        }
      }
      if (m.getName().equals(samName)) return h.invokeWithArguments(args == null ? new Object[0] : args);
      if (m.isDefault()) return java.lang.reflect.InvocationHandler.invokeDefault(proxy, m, args);
      throw new UnsupportedOperationException(m.toString());
    });
    return intf.cast(p);
  }
}
