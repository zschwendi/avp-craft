package org.lwjgl.glfw;

/** Klepton stand-in: there is no NSGL window-system object; every getter returns NULL (0). */
public class GLFWNativeNSGL {
    protected GLFWNativeNSGL() { throw new UnsupportedOperationException(); }
    public static long glfwGetNSGLContext(long window) { return 0L; }
}
