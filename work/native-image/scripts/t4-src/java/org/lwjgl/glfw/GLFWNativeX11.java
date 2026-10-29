package org.lwjgl.glfw;

/** Klepton stand-in: there is no X11 window-system object; every getter returns NULL (0). */
public class GLFWNativeX11 {
    protected GLFWNativeX11() { throw new UnsupportedOperationException(); }
    public static long glfwGetX11Display() { return 0L; }
    public static long glfwGetX11Window(long window) { return 0L; }
}
