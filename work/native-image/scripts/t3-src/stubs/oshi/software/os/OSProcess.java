// T3 signature-only stub (M3C, 26.2): SystemReport.putProcessDetails(OSProcess) reads these; unreachable in the image.
package oshi.software.os;
public interface OSProcess {
  long getResidentSetSize(); long getVirtualSize(); long getUserTime(); long getKernelTime(); long getUpTime();
}
