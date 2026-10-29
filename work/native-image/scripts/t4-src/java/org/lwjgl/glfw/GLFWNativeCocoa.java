package org.lwjgl.glfw;

/** Klepton stand-in: there is no Cocoa window-system object; every getter returns NULL (0). */
public class GLFWNativeCocoa {
    protected GLFWNativeCocoa() { throw new UnsupportedOperationException(); }
    public static long glfwGetCocoaWindow(long window) { return 0L; }
    public static long glfwGetCocoaMonitor(long monitor) { return 0L; }
}
