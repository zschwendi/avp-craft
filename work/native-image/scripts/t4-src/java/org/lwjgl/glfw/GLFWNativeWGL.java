package org.lwjgl.glfw;

/** Klepton stand-in: there is no WGL window-system object; every getter returns NULL (0). */
public class GLFWNativeWGL {
    protected GLFWNativeWGL() { throw new UnsupportedOperationException(); }
    public static long glfwGetWGLContext(long window) { return 0L; }
}
