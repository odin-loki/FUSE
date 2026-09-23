#include "viewport_qt_vulkan_surface.hpp"

#if defined(FUSE_EDITOR_HAS_QT_VULKAN)
#include <QVersionNumber>
#include <QCoreApplication>
#include <QVulkanInstance>
#include <vulkan/vulkan.h>
#endif

#include <cstdlib>

namespace fuse::editor::qt {

namespace {

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

} // namespace

bool ViewportQtVulkanSurface::initialize(QWindow* window) {
    m_nativeSurface = nullptr;
    m_qVulkanReady = false;

    if (window == nullptr) {
        m_note = "null_window";
        return false;
    }

    if (!hasDisplayServer()) {
        m_note = "headless_no_display_server";
        return false;
    }

#if defined(FUSE_EDITOR_HAS_QT_VULKAN)
    // Heap instance destroyed by a QCoreApplication post routine: a function-local static would
    // be destroyed at exit, after the xcb platform plugin (and any Vulkan layers) were torn down,
    // and crash in the loader (seen with VK_LAYER_KHRONOS_validation).
    static QVulkanInstance* s_instancePtr = nullptr;
    if (s_instancePtr == nullptr) {
        s_instancePtr = new QVulkanInstance();
        qAddPostRoutine([] {
            delete s_instancePtr;
            s_instancePtr = nullptr;
        });
    }
    QVulkanInstance& s_instance = *s_instancePtr;
    if (!s_instance.isValid()) {
        s_instance.setApiVersion(QVersionNumber(1, 0)); // apiVersion 0 is invalid (VUID-VkApplicationInfo-apiVersion)
        if (!s_instance.create()) {
            m_note = "qvulkan_instance_create_failed";
            return false;
        }
    }

    // Only a window created as a Vulkan surface on this instance yields a VkSurfaceKHR; a raster
    // widget window returns VK_NULL_HANDLE (stub hand-off). Never retype a live window here.
    const VkSurfaceKHR surface =
        window->vulkanInstance() == &s_instance ? QVulkanInstance::surfaceForWindow(window) : VK_NULL_HANDLE;
    if (surface == VK_NULL_HANDLE) {
        m_note = "qvulkan_surface_for_window_failed";
        return false;
    }

    m_nativeSurface = reinterpret_cast<void*>(surface);
    m_qVulkanReady = true;
    m_note = "qvulkan_instance_surface";
    return true;
#else
    (void)window;
    m_note = "qt_vulkan_module_unavailable";
    return false;
#endif
}

} // namespace fuse::editor::qt
