#if defined(FUSE_HAS_VULKAN_RHI)

#include <fuse/hybrid/vulkan_presentable.hpp>

#if defined(FUSE_HAS_GLFW_WINDOW)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::hybrid {

VulkanPresentable::VulkanPresentable(VulkanPresentableDesc desc) : m_desc(desc) {}

VulkanPresentable::~VulkanPresentable() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_vkSurface != nullptr) {
        // Instance is owned by RendererBootstrap; surface is destroyed when presentable is reset.
        m_vkSurface = nullptr;
    }
#endif
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

    m_window = platform::PlatformWindow::create(m_desc.window);
    m_status.windowReady = m_window != nullptr && m_window->isValid();
    if (!m_status.windowReady) {
        m_status.message = m_window ? m_window->info().message : "PlatformWindow allocation failed";
        return true;
    }

#if defined(FUSE_HAS_GLFW_WINDOW)
    if (m_desc.window.backend == platform::WindowBackend::Glfw) {
        u32 extensionCount = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
        if (extensions != nullptr && extensionCount > 0) {
            m_requiredExtensions.assign(extensions, extensions + extensionCount);
        }
    }
#endif

    m_status.message = m_window->info().message;
    return true;
}

bool VulkanPresentable::createVulkanSurface(void* vkInstance) {
    if (m_desc.backend == PresentableBackend::Headless) {
        m_status.message = "Headless presentable path — no VkSurfaceKHR";
        return false;
    }

    if (m_window == nullptr || !m_window->isValid() || m_window->nativeHandle() == nullptr) {
        m_status.message = "Platform window not ready for WSI surface creation";
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_HAS_GLFW_WINDOW)
    if (m_desc.window.backend == platform::WindowBackend::Glfw) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        const VkResult result = glfwCreateWindowSurface(static_cast<VkInstance>(vkInstance),
                                                        static_cast<GLFWwindow*>(m_window->nativeHandle()),
                                                        nullptr, &surface);
        if (result != VK_SUCCESS) {
            m_status.message = "glfwCreateWindowSurface failed";
            return false;
        }

        m_vkSurface = surface;
        m_status.surfaceReady = true;
        m_status.presentable = true;
        m_status.message = "External VkSurfaceKHR created from GLFW window";
        return true;
    }
#else
    (void)vkInstance;
#endif

    m_status.message = "WSI surface creation stub — enable FUSE_PLATFORM_WINDOW_GLFW for GLFW path";
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
