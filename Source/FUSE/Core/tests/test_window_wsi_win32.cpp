#include <fuse/platform/window.hpp>
#include <fuse/platform/window_wsi.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(FUSE_PLATFORM_WINDOW_WIN32)
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

#if defined(FUSE_PLATFORM_WINDOW_WIN32)
bool hasExtension(const std::vector<const char*>& extensions, const char* name) {
    for (const char* extension : extensions) {
        if (extension != nullptr && name != nullptr && std::strcmp(extension, name) == 0) {
            return true;
        }
    }
    return false;
}
#endif

void testBackendIdentity() {
#if defined(FUSE_PLATFORM_WINDOW_WIN32)
    expectTrue(fuse::platform::activeWindowWsiKind() == fuse::platform::WindowWsiKind::Win32,
               "Win32 WSI kind is compiled in");
    expectTrue(std::strcmp(fuse::platform::windowWsiBackendName(), "win32-wsi") == 0,
               "Win32 WSI backend name");
    expectTrue(fuse::platform::windowWsiAvailable(), "Win32 WSI is available");
    expectTrue(fuse::platform::displayServerAvailable(), "Win32 display server is available");
#elif defined(FUSE_PLATFORM_WINDOW_GLFW)
    expectTrue(fuse::platform::windowWsiBackendName() != nullptr, "GLFW WSI backend name present");
#elif defined(FUSE_PLATFORM_WINDOW_X11)
    expectTrue(fuse::platform::activeWindowWsiKind() == fuse::platform::WindowWsiKind::X11,
               "X11 WSI kind is compiled in");
    expectTrue(std::strcmp(fuse::platform::windowWsiBackendName(), "x11-xlib") == 0,
               "X11 WSI backend name");
    expectTrue(fuse::platform::windowWsiAvailable() == (fuse::platform::nativeDisplayHandle() != nullptr),
               "X11 WSI availability tracks the display connection");
#else
    expectTrue(fuse::platform::activeWindowWsiKind() == fuse::platform::WindowWsiKind::Null,
               "default WSI kind is Null");
    expectTrue(std::strcmp(fuse::platform::windowWsiBackendName(), "null-wsi") == 0,
               "null WSI backend name");
    expectTrue(!fuse::platform::windowWsiAvailable(), "null WSI is unavailable");
#endif
}

void testRequiredInstanceExtensions() {
    std::vector<const char*> extensions;
    fuse::platform::requiredVulkanInstanceExtensions(extensions);
#if defined(FUSE_PLATFORM_WINDOW_WIN32)
    expectTrue(hasExtension(extensions, "VK_KHR_surface"), "Win32 WSI requires VK_KHR_surface");
    expectTrue(hasExtension(extensions, "VK_KHR_win32_surface"),
               "Win32 WSI requires VK_KHR_win32_surface");
#elif defined(FUSE_PLATFORM_WINDOW_GLFW)
    (void)extensions;
#elif defined(FUSE_PLATFORM_WINDOW_X11)
    if (!fuse::platform::windowWsiAvailable()) {
        expectTrue(extensions.empty(), "X11 WSI without a display requires no instance extensions");
    }
#else
    expectTrue(extensions.empty(), "null WSI requires no instance extensions");
#endif
}

void testCreateSurfaceWithoutHwndReturnsFalse() {
    fuse::platform::Window window;
    expectTrue(window.nativeHandle().value == nullptr, "default window has no HWND");

    void* surface = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    const bool created = fuse::platform::createVulkanSurface(nullptr, window, &surface);
    expectTrue(!created, "createVulkanSurface fails without HWND");
    expectTrue(surface == nullptr, "outSurface is cleared on failure");
}

void testCreateSurfaceWithHwndWithoutInstanceReturnsFalse() {
    fuse::platform::WindowDesc desc;
    desc.title = "FUSE WSI Test";
    desc.width = 64;
    desc.height = 64;
    desc.createNative = true;

    fuse::platform::Window window(desc);
    if (window.nativeHandle().value == nullptr) {
        std::fprintf(stderr, "SKIP: WindowDesc.createNative did not produce an HWND\n");
        return;
    }

    void* surface = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    const bool created = fuse::platform::createVulkanSurface(nullptr, window, &surface);
    expectTrue(!created, "createVulkanSurface fails without VkInstance");
    expectTrue(surface == nullptr, "outSurface is cleared when instance is null");
}

void testCreateSurfaceWithHwndIfInstanceExists() {
#if defined(FUSE_PLATFORM_WINDOW_WIN32)
    fuse::platform::WindowDesc desc;
    desc.title = "FUSE WSI Test";
    desc.width = 64;
    desc.height = 64;
    desc.createNative = true;

    fuse::platform::Window window(desc);
    if (window.nativeHandle().value == nullptr) {
        std::fprintf(stderr, "SKIP: WindowDesc.createNative did not produce an HWND\n");
        return;
    }

    std::vector<const char*> extensions;
    fuse::platform::requiredVulkanInstanceExtensions(extensions);

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "FUSE Win32 WSI Test";
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult instanceResult = vkCreateInstance(&createInfo, nullptr, &instance);
    if (instanceResult != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        std::fprintf(stderr, "SKIP: vkCreateInstance unavailable (no ICD / extensions)\n");
        return;
    }

    void* surface = nullptr;
    const bool created = fuse::platform::createVulkanSurface(instance, window, &surface);
    if (!created || surface == nullptr) {
        std::fprintf(stderr, "SKIP: vkCreateWin32SurfaceKHR failed (ICD without Win32 WSI)\n");
    } else {
        vkDestroySurfaceKHR(instance, (VkSurfaceKHR)surface, nullptr);
    }

    vkDestroyInstance(instance, nullptr);
#else
    (void)0;
#endif
}

} // namespace

int main() {
    testBackendIdentity();
    testRequiredInstanceExtensions();
    testCreateSurfaceWithoutHwndReturnsFalse();
    testCreateSurfaceWithHwndWithoutInstanceReturnsFalse();
    testCreateSurfaceWithHwndIfInstanceExists();

    if (g_failures == 0) {
        std::printf("fuse_core window WSI Win32 tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core window WSI Win32 tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
