#if defined(FUSE_HAS_VULKAN_RHI)

#include <fuse/hybrid/vulkan_presentable.hpp>

namespace fuse::hybrid {

VulkanPresentable::VulkanPresentable(VulkanPresentableDesc desc) : m_desc(desc) {}

VulkanPresentable::~VulkanPresentable() {
    m_vkSurface = nullptr;
}

std::unique_ptr<VulkanPresentable> VulkanPresentable::create(const VulkanPresentableDesc& desc) {
    auto presentable = std::unique_ptr<VulkanPresentable>(new VulkanPresentable(desc));
    if (!presentable->initialize()) {
        return presentable;
    }
    return presentable;
}

bool VulkanPresentable::initialize() {
    m_status.backend = m_desc.backend;

    if (m_desc.backend == PresentableBackend::Headless) {
        m_status.message = "Headless presentable path — no platform window";
        return true;
    }

    m_window = std::make_unique<platform::Window>(m_desc.window);
    m_status.windowReady = m_window != nullptr && m_window->isValid();
    if (!m_status.windowReady) {
        m_status.message = "fuse::platform::Window allocation failed";
        return true;
    }

    const platform::VulkanSurfaceWire wire = m_window->vulkanSurfaceWire();
    if (wire.presentable && wire.nativeSurface != nullptr) {
        m_vkSurface = wire.nativeSurface;
        m_status.surfaceReady = true;
        m_status.presentable = true;
        m_status.message = "External VkSurfaceKHR wired from platform::Window";
        return true;
    }

    m_status.message = "B1.7 window stub — WSI surface pending platform backend";
    return true;
}

bool VulkanPresentable::createVulkanSurface(void* vkInstance) {
    if (m_desc.backend == PresentableBackend::Headless) {
        m_status.message = "Headless presentable path — no VkSurfaceKHR";
        return false;
    }

    if (m_window == nullptr || !m_window->isValid()) {
        m_status.message = "Platform window not ready for WSI surface creation";
        return false;
    }

    const platform::VulkanSurfaceWire wire = m_window->vulkanSurfaceWire();
    if (wire.presentable && wire.nativeSurface != nullptr) {
        m_vkSurface = wire.nativeSurface;
        m_status.surfaceReady = true;
        m_status.presentable = true;
        m_status.message = "External VkSurfaceKHR wired from platform::Window";
        (void)vkInstance;
        return true;
    }

    (void)vkInstance;
    m_status.message = "B1.7 window stub — no VkSurfaceKHR until platform WSI lands";
    return false;
}

renderer::SurfaceDesc VulkanPresentable::surfaceDesc() const {
    renderer::SurfaceDesc desc{};
    if (m_status.presentable && m_vkSurface != nullptr) {
        desc.kind = renderer::SurfaceKind::External;
        desc.nativeSurface = m_vkSurface;
        return desc;
    }

    desc.kind = renderer::SurfaceKind::Headless;
    return desc;
}

renderer::VulkanSurface VulkanPresentable::vulkanSurface() const {
    return renderer::VulkanSurface::fromDesc(surfaceDesc());
}

} // namespace fuse::hybrid

#endif // defined(FUSE_HAS_VULKAN_RHI)
