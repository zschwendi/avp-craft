package org.lwjgl.glfw;

import org.lwjgl.system.Library;
import org.lwjgl.system.Platform;

/**
 * JNI surface of libklglfw (scripts/t4-src/native/klglfw.c, which documents each native). Plain downcalls only:
 * the C side never calls back into Java (no FindClass/GetMethodID, no upcalls, no closures), so the native image needs
 * no JNI configuration for it. Loaded from -Dklglfw.libpath=<file> if set, else via LWJGL's library path
 * (-Dorg.lwjgl.librarypath) as libklglfw.dylib.
 */
public final class KlGlfwNative {
    private KlGlfwNative() { }

    private static boolean loaded;

    static synchronized void load() {
        if (loaded) return;
        String p = System.getProperty("klglfw.libpath");
        if (p != null) System.load(p);
        else Library.loadSystem(System::load, System::loadLibrary, KlGlfwNative.class, "org.lwjgl.glfw", Platform.mapLibraryNameBundled("klglfw"));
        loaded = true;
    }

    static native boolean nInit(String glLibrary);
    static native String nLastError();
    static native String nDescribe();
    static native void nTerminate();
    static native long nGetMonitorSize();
    static native long nCreateWindow(int width, int height, long share, int glMajor, int glMinor, int samples);
    static native void nDestroyWindow(long window);
    static native long nGetFramebufferSize(long window);
    static native boolean nMakeContextCurrent(long window);
    static native long nGetCurrentContext();
    static native void nSwapBuffers(long window);
    static native void nSwapInterval(int interval);
    static native long nGetProcAddress(String name);
    static native boolean nExtensionSupported(String name);
    static native int nPollEvents(long buffer, int capacity);
    static native void nWaitEvents(double timeout);
    static native void nPostEmptyEvent();
    static native void nSetCursorPos(long window, double x, double y);
    static native void nSetInputMode(long window, int mode, int value);
    static native double nGetTime();
    static native void nSetTime(double time);
    static native long nGetTimerValue();
    /** which: 0 EGLDisplay, 1 EGLContext, 2 EGLSurface, 3 EGLConfig (the handles Vivecraft's VLoader must return in T8). */
    static native long nGetEGLHandle(long window, int which);
}
