package org.lwjgl.glfw;

/** The klglfw window's EGL objects (ANGLE, created through libltw). Vivecraft's VLoader.getEGL* must return these (T8). */
public class GLFWNativeEGL {
    protected GLFWNativeEGL() { throw new UnsupportedOperationException(); }
    public static long glfwGetEGLDisplay() { return KlGlfwNative.nGetEGLHandle(0L, 0); }
    public static long glfwGetEGLContext(long window) { return KlGlfwNative.nGetEGLHandle(window, 1); }
    public static long glfwGetEGLSurface(long window) { return KlGlfwNative.nGetEGLHandle(window, 2); }
    public static long glfwGetEGLConfig(long window) { return KlGlfwNative.nGetEGLHandle(window, 3); }
}
