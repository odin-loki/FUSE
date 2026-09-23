#include <fuse/platform/window.hpp>
#include <fuse/platform/window_wsi.hpp>

#if defined(FUSE_PLATFORM_WINDOW_GLFW)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

#include <cstdlib>
#endif

namespace fuse::platform {

namespace {

#if defined(FUSE_PLATFORM_WINDOW_GLFW)
bool g_glfwInitialized = false;
bool g_glfwUsable = false;

bool ensureGlfwInitialized() {
    if (g_glfwInitialized) {
        return g_glfwUsable;
    }

    g_glfwInitialized = true;
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
        g_glfwUsable = false;
        return false;
    }

    if (glfwInit() != GLFW_TRUE) {
        g_glfwUsable = false;
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    g_glfwUsable = true;
    return true;
}
#endif

} // namespace

WindowWsiKind activeWindowWsiKind() {
#if defined(FUSE_PLATFORM_WINDOW_GLFW)
    return WindowWsiKind::Glfw;
#else
    return WindowWsiKind::Null;
#endif
}

const char* windowWsiBackendName() {
#if defined(FUSE_PLATFORM_WINDOW_GLFW)
    return "glfw-hidden-window";
#else
    return "null-wsi";
#endif
}

bool windowWsiAvailable() {
#if defined(FUSE_PLATFORM_WINDOW_GLFW)
    return ensureGlfwInitialized();
#else
    return false;
#endif
}

bool displayServerAvailable() {
#if defined(FUSE_PLATFORM_WINDOW_GLFW)
    return std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
#else
    return true;
#endif
}

void requiredVulkanInstanceExtensions(std::vector<const char*>& out) {
    out.clear();
#if defined(FUSE_PLATFORM_WINDOW_GLFW)
    if (!ensureGlfwInitialized()) {
        return;
    }

    u32 extensionCount = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
    if (extensions == nullptr || extensionCount == 0) {
        return;
    }

    out.reserve(extensionCount);
    for (u32 i = 0; i < extensionCount; ++i) {
        out.push_back(extensions[i]);
    }
#endif
}

void* nativeDisplayHandle() {
    return nullptr;
}

bool createVulkanSurface(void* vkInstance, const Window& window, void** outSurface) {
    if (outSurface != nullptr) {
        *outSurface = nullptr;
    }

#if defined(FUSE_PLATFORM_WINDOW_GLFW)
    if (vkInstance == nullptr || !window.isValid() || !ensureGlfwInitialized()) {
        return false;
    }

    void* nativeWindow = window.nativeHandle().value;
    if (nativeWindow == nullptr) {
        return false;
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(static_cast<VkInstance>(vkInstance),
                                static_cast<GLFWwindow*>(nativeWindow), nullptr,
                                &surface) != VK_SUCCESS) {
        return false;
    }

    if (outSurface != nullptr) {
        *outSurface = surface;
    }
    return true;
#else
    (void)vkInstance;
    (void)window;
    return false;
#endif
}

} // namespace fuse::platform
