#include <fuse/editor/viewport_vulkan_surface.hpp>

#if defined(FUSE_HAS_QT_VULKAN)

#include <QGuiApplication>
#include <QVulkanInstance>
#include <QWindow>

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
