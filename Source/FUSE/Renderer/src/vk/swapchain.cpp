#include <fuse/renderer/vk/swapchain.hpp>

namespace fuse::renderer {

std::unique_ptr<VulkanSwapchain> VulkanSwapchain::create(VulkanDevice& device, const SwapchainDesc& desc) {
    auto swapchain = std::unique_ptr<VulkanSwapchain>(new VulkanSwapchain());
    if (!swapchain->initialize(device, desc)) {
        return swapchain;
    }
    return swapchain;
}

VulkanSwapchain::~VulkanSwapchain() = default;

bool VulkanSwapchain::initialize(VulkanDevice& device, const SwapchainDesc& desc) {
    m_info.width = desc.width;
    m_info.height = desc.height;
    m_info.imageCount = desc.imageCount;

    if (!device.isValid()) {
        m_info.message = "Swapchain placeholder skipped — device not ready";
        return true;
    }

    if (desc.surface == nullptr) {
        m_info.ready = false;
        m_info.message = "Swapchain placeholder recorded (headless — surface deferred to B2.2)";
        return true;
    }

    m_info.ready = false;
    m_info.message = "Surface present but swapchain creation deferred to B2.2";
    return true;
}

} // namespace fuse::renderer
