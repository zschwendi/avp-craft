package oshi.util.tuples;

// Not a signature stub: the real class (oshi-core 6.9.0's Pair is exactly this). QC Vivecraft 1.3.7 (1.21.11)
// MCOpenXR.loadBindings builds Pair<String,String> keys; oshi-core is off the native-image classpath, so without this
// VR init died with NoClassDefFoundError: oshi.util.tuples.Pair (launch 8 sim round, 2026-10-12).
public class Pair<A, B> {
    private final A a;
    private final B b;

    public Pair(A a, B b) { this.a = a; this.b = b; }
    public final A getA() { return a; }
    public final B getB() { return b; }
}
