#pragma once

#include <QWindow>

namespace fuse::editor::qt {

/// Headless-safe Qt Vulkan surface helper (U6 wave 5).
/// Returns an opaque `VkSurfaceKHR` when `QVulkanInstance` succeeds; null otherwise.
class ViewportQtVulkanSurface {
public:
    bool initialize(QWindow* window);
    void* nativeSurface() const { return m_nativeSurface; }
    bool usesQVulkanInstance() const { return m_qVulkanReady; }
    const char* backendNote() const { return m_note; }

private:
    void* m_nativeSurface = nullptr;
    bool m_qVulkanReady = false;
    const char* m_note = "uninitialized";
};

} // namespace fuse::editor::qt
