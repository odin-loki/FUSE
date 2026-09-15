#include <fuse/renderer/vk/bootstrap.hpp>

namespace fuse::renderer {

std::unique_ptr<VulkanBootstrap> VulkanBootstrap::create(const VulkanBootstrapDesc& desc) {
    auto bootstrap = std::unique_ptr<VulkanBootstrap>(new VulkanBootstrap());
    if (!bootstrap->initialize(desc)) {
        return bootstrap;
    }
    return bootstrap;
}

VulkanBootstrap::~VulkanBootstrap() = default;

bool VulkanBootstrap::initialize(const VulkanBootstrapDesc& desc) {
    m_instance = VulkanInstance::create(desc.instance);
    if (!m_instance) {
        m_status.message = "VulkanInstance allocation failed";
        return false;
    }

    m_status.mode = m_instance->info().mode;
    m_status.instanceReady = m_instance->isValid();
    if (!m_status.instanceReady) {
        m_status.message = m_instance->info().message;
        return true;
    }

    m_device = VulkanDevice::create(*m_instance, desc.device);
    if (!m_device) {
        m_status.message = "VulkanDevice allocation failed";
        return true;
    }

    m_status.deviceReady = m_device->isValid();
    if (!m_status.deviceReady) {
        m_status.message = m_device->info().message;
        return true;
    }

    if (desc.createSwapchainPlaceholder) {
        m_swapchain = VulkanSwapchain::create(*m_device, desc.swapchain);
        if (m_swapchain) {
            m_status.swapchainPlaceholderReady = m_swapchain->isReady();
            m_status.message = m_swapchain->info().message;
        }
    } else {
        m_status.swapchainPlaceholderReady = false;
        m_status.message = "Swapchain deferred (B2.2)";
    }

    return true;
}

} // namespace fuse::renderer
