// T3 signature-only stub (native-image build classpath only): lets substituted Minecraft classes load without the real
// jar, so JNA / OSHI / java-objc-bridge leave the closed world entirely. Never instantiated: the code using it is substituted.
package com.sun.jna.platform.win32;
public interface Tlhelp32 { class MODULEENTRY32W { private MODULEENTRY32W() { } } }
