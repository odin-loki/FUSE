#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/swapchain.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

struct VulkanBootstrapDesc {
    VulkanInstanceDesc instance{};
    VulkanDeviceDesc device{};
    SwapchainDesc swapchain{};
    bool createSwapchain = true;
    bool createFrameManager = true;
};

struct VulkanBootstrapStatus {
    VulkanBackendMode mode = VulkanBackendMode::Stub;
    bool instanceReady = false;
    bool deviceReady = false;
    bool swapchainReady = false;
    bool swapchainHeadless = true;
    bool frameManagerReady = false;
    std::string message;
};

/// B2.2 aggregate — instance, device, swapchain (real or headless), frame ring.
class VulkanBootstrap {
public:
    static std::unique_ptr<VulkanBootstrap> create(const VulkanBootstrapDesc& desc);
    ~VulkanBootstrap();

    VulkanBootstrap(const VulkanBootstrap&) = delete;
    VulkanBootstrap& operator=(const VulkanBootstrap&) = delete;

    const VulkanBootstrapStatus& status() const { return m_status; }
    VulkanInstance* instance() { return m_instance.get(); }
    VulkanDevice* device() { return m_device.get(); }
    VulkanSwapchain* swapchain() { return m_swapchain.get(); }
    FrameManager* frameManager() { return m_frameManager.get(); }

    const VulkanInstance* instance() const { return m_instance.get(); }
    const VulkanDevice* device() const { return m_device.get(); }
    const VulkanSwapchain* swapchain() const { return m_swapchain.get(); }
    const FrameManager* frameManager() const { return m_frameManager.get(); }

private:
    VulkanBootstrap() = default;
    bool initialize(const VulkanBootstrapDesc& desc);

    VulkanBootstrapStatus m_status;
    std::unique_ptr<VulkanInstance> m_instance;
    std::unique_ptr<VulkanDevice> m_device;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<FrameManager> m_frameManager;
};

} // namespace fuse::renderer
