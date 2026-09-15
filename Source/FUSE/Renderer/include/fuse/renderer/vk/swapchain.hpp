#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/surface.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {

/// Presentation pacing — maps to VkPresentModeKHR when a real swapchain exists.
enum class VsyncMode : u8 {
    Fifo = 0,     ///< VK_PRESENT_MODE_FIFO_KHR (default, vsync on)
    Mailbox = 1,  ///< VK_PRESENT_MODE_MAILBOX_KHR (low latency)
    Immediate = 2 ///< VK_PRESENT_MODE_IMMEDIATE_KHR (uncapped)
};

inline bool vsyncEnabled(VsyncMode mode) {
    return mode == VsyncMode::Fifo;
}

inline const char* vsyncModeName(VsyncMode mode) {
    switch (mode) {
    case VsyncMode::Fifo:
        return "Fifo";
    case VsyncMode::Mailbox:
        return "Mailbox";
    case VsyncMode::Immediate:
        return "Immediate";
    }
    return "Unknown";
}

struct SwapchainDesc {
    SurfaceDesc surface{};
    u32 width = 0;
    u32 height = 0;
    VsyncMode vsyncMode = VsyncMode::Fifo;
    u32 imageCount = 3;
    /// Preferred format when surface path is active (VK_FORMAT_B8G8R8A8_UNORM).
    u32 preferredFormat = 44; // VK_FORMAT_B8G8R8A8_UNORM without including vulkan.h in public API
};

struct SwapchainImageInfo {
    void* image = nullptr;
    void* view = nullptr;
    u32 index = 0;
};

struct SwapchainInfo {
    bool ready = false;
    bool headless = true;
    u32 width = 0;
    u32 height = 0;
    u32 imageCount = 0;
    u32 format = 0;
    u32 recreateCount = 0;
    std::string message;
};

/// B2.2 — real VkSwapchainKHR when External surface + swapchain extension; headless stub otherwise.
class VulkanSwapchain {
public:
    static std::unique_ptr<VulkanSwapchain> create(VulkanDevice& device, const SwapchainDesc& desc);
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    const SwapchainInfo& info() const { return m_info; }
    bool isReady() const { return m_info.ready; }
    bool isHeadless() const { return m_info.headless; }

    void* nativeHandle() const { return m_handle; }
    const std::vector<SwapchainImageInfo>& images() const { return m_images; }

    /// Returns image index or UINT32_MAX when headless / not acquired.
    u32 acquireNextImage(void* imageAvailableSemaphore);
    bool present(void* renderFinishedSemaphore, u32 imageIndex);

    bool rebuild(VulkanDevice& device, u32 width, u32 height);

private:
    VulkanSwapchain() = default;
    bool initialize(VulkanDevice& device, const SwapchainDesc& desc);
    void shutdown(VulkanDevice& device);
    bool createSwapchainResources(VulkanDevice& device, const SwapchainDesc& desc);

    SwapchainInfo m_info;
    SurfaceDesc m_surfaceDesc{};
    SwapchainDesc m_desc{};
    void* m_handle = nullptr;
    void* m_device = nullptr;
    void* m_physicalDevice = nullptr;
    void* m_graphicsQueue = nullptr;
    u32 m_graphicsQueueFamily = 0;
    std::vector<SwapchainImageInfo> m_images;
};

} // namespace fuse::renderer
