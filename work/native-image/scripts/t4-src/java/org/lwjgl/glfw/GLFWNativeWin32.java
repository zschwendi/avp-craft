package org.lwjgl.glfw;

/** Klepton stand-in: there is no Win32 window-system object; every getter returns NULL (0). */
public class GLFWNativeWin32 {
    protected GLFWNativeWin32() { throw new UnsupportedOperationException(); }
    public static long glfwGetWin32Window(long window) { return 0L; }
    public static long glfwGetWin32Adapter(long monitor) { return 0L; }
}
