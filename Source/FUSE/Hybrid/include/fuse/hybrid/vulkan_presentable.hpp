#pragma once

#if defined(FUSE_HAS_VULKAN_RHI)

#include <fuse/platform/window.hpp>
#include <fuse/renderer/vk/surface.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::hybrid {

/// How Hybrid selects a presentable WSI path (B2.2 follow-up).
enum class PresentableBackend : u8 {
    /// CI default — `SurfaceKind::Headless`, no OS window.
    Headless = 0,
    /// B1.7 `fuse::platform::Window` wired to `SurfaceKind::External` when WSI exists.
    GameWindow = 1,
};

struct VulkanPresentableDesc {
    PresentableBackend backend = PresentableBackend::Headless;
    platform::WindowDesc window{};
    u32 swapchainWidth = 1280;
    u32 swapchainHeight = 720;
};

struct VulkanPresentableStatus {
    PresentableBackend backend = PresentableBackend::Headless;
    bool windowReady = false;
    bool surfaceReady = false;
    bool presentable = false;
    std::string message;
};

/// Own Hybrid presentable path — platform window + External `VulkanSurface` wiring.
class VulkanPresentable {
public:
    static std::unique_ptr<VulkanPresentable> create(const VulkanPresentableDesc& desc);

    ~VulkanPresentable();

    VulkanPresentable(const VulkanPresentable&) = delete;
    VulkanPresentable& operator=(const VulkanPresentable&) = delete;

    const VulkanPresentableStatus& status() const { return m_status; }
    const platform::Window* window() const { return m_window.get(); }

    /// WSI instance extensions required before `vkCreateInstance` (GLFW path only).
    const std::vector<const char*>& requiredInstanceExtensions() const { return m_requiredExtensions; }

    /// Create `VkSurfaceKHR` from the platform window after instance bootstrap.
    bool createVulkanSurface(void* vkInstance);

    /// Surface wired into `SwapchainDesc` / `VulkanBootstrap`.
    renderer::SurfaceDesc surfaceDesc() const;

    renderer::VulkanSurface vulkanSurface() const;

    u32 swapchainWidth() const { return m_desc.swapchainWidth; }
    u32 swapchainHeight() const { return m_desc.swapchainHeight; }

private:
    explicit VulkanPresentable(VulkanPresentableDesc desc);

    bool initialize();

    VulkanPresentableDesc m_desc;
    VulkanPresentableStatus m_status;
    std::unique_ptr<platform::Window> m_window;
    std::vector<const char*> m_requiredExtensions;
    void* m_vkSurface = nullptr;
};

} // namespace fuse::hybrid

#endif // defined(FUSE_HAS_VULKAN_RHI)
