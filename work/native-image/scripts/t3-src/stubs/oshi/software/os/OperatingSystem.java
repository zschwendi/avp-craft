// T3 signature-only stub (M3C, 26.2): SystemReport.putSoftware(SystemInfo) -> getOperatingSystem() -> getCurrentProcess().
// Target_T3 (subst-26.2) substitutes the lambda that calls it; this only lets SystemReport load without oshi-core.
package oshi.software.os;
public interface OperatingSystem { OSProcess getCurrentProcess(); }
