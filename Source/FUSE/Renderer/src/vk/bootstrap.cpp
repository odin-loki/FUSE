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

    if (desc.createFrameManager) {
        m_frameManager = FrameManager::create(*m_device);
        if (m_frameManager) {
            m_status.frameManagerReady = m_frameManager->isReady();
        }
    }

    if (desc.createSwapchain) {
        m_swapchain = VulkanSwapchain::create(*m_device, desc.swapchain);
        if (m_swapchain) {
            m_status.swapchainReady = m_swapchain->isReady();
            m_status.swapchainHeadless = m_swapchain->isHeadless();
            m_status.message = m_swapchain->info().message;
        }
    } else {
        m_status.swapchainReady = false;
        m_status.swapchainHeadless = true;
        m_status.message = "Swapchain deferred";
    }

    if (m_status.frameManagerReady && m_status.message.empty()) {
        m_status.message = m_frameManager->info().message;
    }

    return true;
}

bool VulkanBootstrap::ensureSwapchain(const SwapchainDesc& desc) {
    if (!m_device || !m_device->isValid()) {
        m_status.message = "ensureSwapchain requires a valid Vulkan device";
        return false;
    }

    m_swapchain.reset();
    m_swapchain = VulkanSwapchain::create(*m_device, desc);
    if (!m_swapchain) {
        m_status.swapchainReady = false;
        m_status.swapchainHeadless = true;
        m_status.message = "Swapchain allocation failed";
        return false;
    }

    m_status.swapchainReady = m_swapchain->isReady();
    m_status.swapchainHeadless = m_swapchain->isHeadless();
    m_status.message = m_swapchain->info().message;
    return true;
}

} // namespace fuse::renderer
