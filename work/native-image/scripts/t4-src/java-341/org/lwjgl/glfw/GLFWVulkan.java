package org.lwjgl.glfw;

import java.nio.*;

import javax.annotation.*;

import org.lwjgl.*;
import org.lwjgl.system.*;
import org.lwjgl.vulkan.*;

/**
 * Klepton stand-in for LWJGL 3.4.1's GLFWVulkan (Minecraft 26.x only; compiled by scripts/t2/build-klglfw341.sh).
 * klglfw has no Vulkan surface: Vulkan is reported unsupported so 26.2 stays on its OpenGL backend
 * (VulkanBackend checks glfwVulkanSupported(); VulkanInstance / VulkanGpuSurface / VulkanPhysicalDevice reference the
 * rest). Every entry point is an honest failure, none a silent success.
 */
public class GLFWVulkan {
    protected GLFWVulkan() { throw new UnsupportedOperationException(); }

    /** VK_ERROR_INITIALIZATION_FAILED, what GLFW returns from glfwCreateWindowSurface without a Vulkan loader. */
    private static final int VK_ERROR_INITIALIZATION_FAILED = -3;

    public static void glfwInitVulkanLoader(@NativeType("PFN_vkGetInstanceProcAddr") long loader) { }
    public static boolean glfwVulkanSupported() { return false; }
    public static @Nullable PointerBuffer glfwGetRequiredInstanceExtensions() {
        GLFW.klError(GLFW.GLFW_API_UNAVAILABLE, "klglfw: Vulkan: no Vulkan support (glfwGetRequiredInstanceExtensions)");
        return null;
    }
    public static long glfwGetInstanceProcAddress(@Nullable VkInstance instance, ByteBuffer procname) { return 0L; }
    public static long glfwGetInstanceProcAddress(@Nullable VkInstance instance, CharSequence procname) { return 0L; }
    public static boolean glfwGetPhysicalDevicePresentationSupport(VkInstance instance, VkPhysicalDevice device, int queuefamily) { return false; }
    public static int glfwCreateWindowSurface(VkInstance instance, long window, @Nullable VkAllocationCallbacks allocator, LongBuffer surface) {
        surface.put(surface.position(), 0L);
        GLFW.klError(GLFW.GLFW_API_UNAVAILABLE, "klglfw: Vulkan: no Vulkan support (glfwCreateWindowSurface)");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    public static int glfwCreateWindowSurface(VkInstance instance, long window, @Nullable VkAllocationCallbacks allocator, long[] surface) {
        surface[0] = 0L;
        GLFW.klError(GLFW.GLFW_API_UNAVAILABLE, "klglfw: Vulkan: no Vulkan support (glfwCreateWindowSurface)");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    public static void setPath(FunctionProvider sharedLibrary) { }
    public static void setPath(@Nullable String path) { }
}
