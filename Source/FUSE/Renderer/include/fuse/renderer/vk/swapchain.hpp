#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

struct SwapchainDesc {
    void* surface = nullptr;
    u32 width = 0;
    u32 height = 0;
    bool vsync = true;
    u32 imageCount = 3;
};

struct SwapchainInfo {
    bool ready = false;
    u32 width = 0;
    u32 height = 0;
    u32 imageCount = 0;
    std::string message;
};

/// B2.2 placeholder — records desired surface parameters; no present yet.
class VulkanSwapchain {
public:
    static std::unique_ptr<VulkanSwapchain> create(VulkanDevice& device, const SwapchainDesc& desc);
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    const SwapchainInfo& info() const { return m_info; }
    bool isReady() const { return m_info.ready; }

private:
    VulkanSwapchain() = default;
    bool initialize(VulkanDevice& device, const SwapchainDesc& desc);

    SwapchainInfo m_info;
};

} // namespace fuse::renderer
