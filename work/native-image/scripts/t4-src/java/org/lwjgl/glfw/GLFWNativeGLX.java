package org.lwjgl.glfw;

/** Klepton stand-in: there is no GLX window-system object; every getter returns NULL (0). */
public class GLFWNativeGLX {
    protected GLFWNativeGLX() { throw new UnsupportedOperationException(); }
    public static long glfwGetGLXContext(long window) { return 0L; }
    public static long glfwGetGLXWindow(long window) { return 0L; }
}
