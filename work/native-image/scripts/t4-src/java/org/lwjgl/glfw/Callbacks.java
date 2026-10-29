package org.lwjgl.glfw;

/** Klepton stand-in: the per-window callbacks live in Java (GLFW.Win), so freeing them is plain field clearing. */
public final class Callbacks {
    private Callbacks() { }

    /** Resets all callbacks of the window and frees those that are LWJGL Callback objects (same contract as LWJGL's). */
    public static void glfwFreeCallbacks(long window) { GLFW.freeWindowCallbacks(window); }
}
