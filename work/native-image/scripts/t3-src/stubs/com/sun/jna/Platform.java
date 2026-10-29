// T3 stub of JNA's com.sun.jna.Platform (native-image build classpath only; JNA itself is not in the image).
// Simple Voice Chat (VoicechatClient, ClientConfig, NativeValidator), Sodium (ModuleScanner) and Vivecraft
// (DeviceCompat.Desktop) only use the pure-Java OS queries below; JNA's real class also touches Native.POINTER_SIZE,
// which would load libjnidispatch. Same answers as JNA for the os.name values we run with.
package com.sun.jna;
public final class Platform {
  public static final int UNSPECIFIED = -1, MAC = 0, LINUX = 1, WINDOWS = 2, SOLARIS = 3, FREEBSD = 4, OPENBSD = 5,
      WINDOWSCE = 6, AIX = 7, ANDROID = 8, GNU = 9, KFREEBSD = 10, NETBSD = 11, DRAGONFLYBSD = 12;
  private Platform() { }
  private static String os() { return System.getProperty("os.name", ""); }
  public static int getOSType() {
    String o = os();
    if (o.startsWith("Mac") || o.startsWith("Darwin")) return MAC;
    if (o.startsWith("Windows")) return WINDOWS;
    if (o.startsWith("Linux")) return "dalvik".equalsIgnoreCase(System.getProperty("java.vm.name")) ? ANDROID : LINUX;
    return UNSPECIFIED;
  }
  public static boolean isMac() { return getOSType() == MAC; }
  public static boolean isWindows() { return getOSType() == WINDOWS; }
  public static boolean isWindowsCE() { return false; }
  public static boolean isLinux() { return getOSType() == LINUX; }
  public static boolean isAndroid() { return getOSType() == ANDROID; }
  public static boolean is64Bit() { return true; }
  public static boolean isARM() { return System.getProperty("os.arch", "").startsWith("a"); }
  public static boolean isX11() { return false; }
}
