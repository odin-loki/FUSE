#include <fuse/platform/window.hpp>
#include <fuse/platform/window_wsi.hpp>

#include <cstdlib>

#if defined(FUSE_PLATFORM_WINDOW_WIN32)
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan.h>
#endif

#if defined(FUSE_PLATFORM_WINDOW_X11)
#include "x11_window.hpp"
#endif

namespace fuse::platform {

namespace {

#if !defined(FUSE_PLATFORM_WINDOW_WIN32)
bool hasDisplayServerEnv() {
#if defined(__linux__)
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    return (display != nullptr && display[0] != '\0') ||
           (wayland != nullptr && wayland[0] != '\0');
#else
    return true;
#endif
}
#endif

} // namespace

WindowWsiKind activeWindowWsiKind() {
#if defined(FUSE_PLATFORM_WINDOW_WIN32)
    return WindowWsiKind::Win32;
#elif defined(FUSE_PLATFORM_WINDOW_X11)
    return WindowWsiKind::X11;
#else
    return WindowWsiKind::Null;
#endif
}

const char* windowWsiBackendName() {
#if defined(FUSE_PLATFORM_WINDOW_WIN32)
    return "win32-wsi";
#elif defined(FUSE_PLATFORM_WINDOW_X11)
    return "x11-xlib";
#else
    return "null-wsi";
#endif
}

bool windowWsiAvailable() {
#if defined(FUSE_PLATFORM_WINDOW_WIN32)
    return true;
#elif defined(FUSE_PLATFORM_WINDOW_X11)
    return x11::ensureDisplay();
#else
    return false;
#endif
}

bool displayServerAvailable() {
#if defined(FUSE_PLATFORM_WINDOW_WIN32)
    return true;
#else
    return hasDisplayServerEnv();
#endif
}

void requiredVulkanInstanceExtensions(std::vector<const char*>& out) {
    out.clear();
#if defined(FUSE_PLATFORM_WINDOW_WIN32)
    out.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    out.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(FUSE_PLATFORM_WINDOW_X11)
    // Mirrors the GLFW backend: publish WSI extensions only when a display is reachable so
    // headless CI instances stay extension-free.
    if (x11::ensureDisplay()) {
        x11::vulkanInstanceExtensions(out);
    }
#endif
}

void* nativeDisplayHandle() {
#if defined(FUSE_PLATFORM_WINDOW_X11)
    return x11::display();
#else
    return nullptr;
#endif
}

bool rawMouseInputAvailable() {
#if defined(FUSE_PLATFORM_WINDOW_X11)
    return x11::rawMotionAvailable();
#elif defined(_WIN32)
    return true;
#else
    return false;
#endif
}

bool createVulkanSurface(void* vkInstance, const Window& window, void** outSurface) {
    if (outSurface != nullptr) {
        *outSurface = nullptr;
    }

#if defined(FUSE_PLATFORM_WINDOW_WIN32)
    void* nativeWindow = window.nativeHandle().value;
    if (vkInstance == nullptr || nativeWindow == nullptr) {
        return false;
    }

    VkWin32SurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.pNext = nullptr;
    createInfo.flags = 0;
    createInfo.hinstance = GetModuleHandleW(nullptr);
    createInfo.hwnd = static_cast<HWND>(nativeWindow);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result = vkCreateWin32SurfaceKHR(static_cast<VkInstance>(vkInstance),
                                                    &createInfo, nullptr, &surface);
    if (result != VK_SUCCESS || surface == VK_NULL_HANDLE) {
        return false;
    }

    if (outSurface != nullptr) {
        *outSurface = (void*)surface;
    }
    return true;
#elif defined(FUSE_PLATFORM_WINDOW_X11)
    void* nativeWindow = window.nativeHandle().value;
    if (vkInstance == nullptr || nativeWindow == nullptr) {
        return false;
    }
    return x11::createVulkanSurface(vkInstance, nativeWindow, outSurface);
#else
    (void)vkInstance;
    (void)window;
    return false;
#endif
}

} // namespace fuse::platform
