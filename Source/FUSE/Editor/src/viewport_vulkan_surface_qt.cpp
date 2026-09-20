#include <fuse/editor/viewport_vulkan_surface.hpp>

#if defined(FUSE_HAS_QT_VULKAN)

#include <QByteArrayList>
#include <QGuiApplication>
#include <QVulkanInstance>
#include <QVulkanWindow>
#include <QWindow>

#include <cstdlib>

#include <vulkan/vulkan.h>

namespace fuse::editor {

namespace {

QVulkanInstance* sharedQtVulkanInstance() {
    static QVulkanInstance* instance = nullptr;
    if (instance != nullptr) {
        return instance;
    }

    instance = new QVulkanInstance();
    if (!instance->create()) {
        delete instance;
        instance = nullptr;
    }
    return instance;
}

ViewportVulkanSurfaceResult makeWinIdStub(u64 winId, const char* message) {
    ViewportVulkanSurfaceResult result{};
    result.vkSurface = reinterpret_cast<void*>(static_cast<uintptr_t>(winId));
    result.valid = true;
    result.stubPath = true;
    result.message = message;
    return result;
}

} // namespace

ViewportVulkanSurfaceResult createViewportVulkanSurfaceFromWinIdQt(u64 winId, u32 /*width*/,
                                                                 u32 /*height*/) {
    if (winId == 0u) {
        ViewportVulkanSurfaceResult invalid{};
        invalid.message = "invalid Qt winId";
        return invalid;
    }

    QVulkanInstance* instance = sharedQtVulkanInstance();
    if (instance == nullptr) {
        return makeWinIdStub(winId, "QVulkanInstance::create failed — winId stub handoff");
    }

    QWindow* window = QWindow::fromWinId(static_cast<WId>(winId));
    if (window == nullptr) {
        return makeWinIdStub(winId, "QWindow::fromWinId failed — winId stub handoff");
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!instance->createSurface(window, &surface) || surface == VK_NULL_HANDLE) {
        return makeWinIdStub(winId, "QVulkanInstance::createSurface failed — winId stub handoff");
    }

    ViewportVulkanSurfaceResult result{};
    result.vkInstance = reinterpret_cast<void*>(instance->vkInstance());
    result.vkSurface = reinterpret_cast<void*>(surface);
    result.valid = true;
    result.stubPath = false;
    result.message = "QVulkanInstance surface created";
    return result;
}

bool hasDisplayServer() {
#if defined(__linux__)
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    return (display != nullptr && display[0] != '\0') ||
           (wayland != nullptr && wayland[0] != '\0');
#else
    return true;
#endif
}

QVulkanWindowWsiProbeResult probeQVulkanWindowWsiQt() {
    QVulkanWindowWsiProbeResult result{};
    result.attempted = true;

    if (!hasDisplayServer()) {
        result.headlessSkipped = true;
        result.note = "headless_no_display_server";
        return result;
    }

    QVulkanInstance* instance = sharedQtVulkanInstance();
    if (instance == nullptr) {
        result.note = "qvulkan_instance_create_failed";
        return result;
    }

    result.instanceReady = true;
    const QByteArrayList extensions = QVulkanInstance::supportedSurfaceExtensions();
    result.extensionsProbed = true;
    result.supportedExtensionCount = static_cast<u32>(extensions.size());
    if (extensions.isEmpty()) {
        result.note = "qvulkan_no_surface_extensions";
        return result;
    }

    QVulkanWindow window;
    window.setVulkanInstance(instance);
    window.setWidth(64);
    window.setHeight(64);
    window.create();

    const VkSurfaceKHR surface = window.vulkanSurface();
    if (surface == VK_NULL_HANDLE) {
        result.note = "qvulkan_window_surface_failed";
        window.destroy();
        return result;
    }

    result.surfaceReady = true;
    result.note = "qvulkan_window_surface";
    window.destroy();
    return result;
}

void destroyViewportVulkanSurfaceQt(ViewportVulkanSurfaceResult& result) {
    if (result.valid && !result.stubPath && result.vkSurface != nullptr) {
        // Surface lifetime remains tied to QWindow; explicit destroy deferred to U6 embed teardown.
        result.vkSurface = nullptr;
    }
    result.vkInstance = nullptr;
    result.valid = false;
    result.stubPath = true;
    result.message = nullptr;
}

} // namespace fuse::editor

#endif // FUSE_HAS_QT_VULKAN
