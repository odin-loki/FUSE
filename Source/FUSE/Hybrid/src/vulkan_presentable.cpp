#if defined(FUSE_HAS_VULKAN_RHI)

#include <fuse/hybrid/vulkan_presentable.hpp>

#include <fuse/platform/window_wsi.hpp>

#include <type_traits>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::hybrid {

namespace {

template <typename T, typename = void>
struct WindowDescHasCreateNative : std::false_type {};

template <typename T>
struct WindowDescHasCreateNative<T, std::void_t<decltype(T::createNative)>> : std::true_type {};

template <typename Desc>
void enableNativeWindowIfSupported(Desc& desc) {
    if constexpr (WindowDescHasCreateNative<Desc>::value) {
        desc.createNative = true;
    }
}

} // namespace

VulkanPresentable::VulkanPresentable(VulkanPresentableDesc desc) : m_desc(desc) {}

VulkanPresentable::~VulkanPresentable() {
    destroyOwnedVulkanSurface();
}

void VulkanPresentable::destroyOwnedVulkanSurface() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_ownsVkSurface && m_vkSurface != nullptr && m_vkInstance != nullptr) {
        vkDestroySurfaceKHR(static_cast<VkInstance>(m_vkInstance),
                            static_cast<VkSurfaceKHR>(m_vkSurface), nullptr);
    }
#endif
    m_vkSurface = nullptr;
    m_vkInstance = nullptr;
    m_ownsVkSurface = false;
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
    m_status.vsyncMode = m_desc.vsyncMode;
    m_requiredExtensions.clear();

    if (m_desc.backend == PresentableBackend::Headless) {
        m_status.message = "Headless presentable path — no platform window";
        return true;
    }

    platform::requiredVulkanInstanceExtensions(m_requiredExtensions);

    platform::WindowDesc windowDesc = m_desc.window;
    enableNativeWindowIfSupported(windowDesc);
    m_desc.window = windowDesc;
    m_window = std::make_unique<platform::Window>(windowDesc);
    m_status.windowReady = m_window != nullptr && m_window->isValid();
    if (!m_status.windowReady) {
        m_status.message = "fuse::platform::Window allocation failed";
        return true;
    }

    const platform::VulkanSurfaceWire wire = m_window->vulkanSurfaceWire();
    if (wire.presentable && wire.nativeSurface != nullptr) {
        m_vkSurface = wire.nativeSurface;
        m_ownsVkSurface = false;
        m_status.surfaceReady = true;
        m_status.presentable = true;
        m_status.message = "External VkSurfaceKHR wired from platform::Window";
        return true;
    }

    if (platform::windowWsiAvailable() && m_window->nativeHandle().value != nullptr) {
        m_status.message = "Win32/platform WSI ready — call createVulkanSurface after vkCreateInstance";
        return true;
    }

    if (wire.presentable) {
        m_status.message = "Platform window ready — call createVulkanSurface after vkCreateInstance";
        return true;
    }

    m_status.message = "Null WSI desktop scaffold — no VkSurfaceKHR until display/GLFW available";
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
        destroyOwnedVulkanSurface();
        m_vkSurface = wire.nativeSurface;
        m_vkInstance = vkInstance;
        m_ownsVkSurface = false;
        m_status.surfaceReady = true;
        m_status.presentable = true;
        m_status.message = "External VkSurfaceKHR wired from platform::Window";
        return true;
    }

    void* createdSurface = nullptr;
    if (platform::createVulkanSurface(vkInstance, *m_window, &createdSurface) &&
        createdSurface != nullptr) {
        destroyOwnedVulkanSurface();
        m_vkSurface = createdSurface;
        m_vkInstance = vkInstance;
        m_ownsVkSurface = true;
        m_status.surfaceReady = true;
        m_status.presentable = true;
        m_status.message = "VkSurfaceKHR created via platform WSI backend";
        return true;
    }

    m_status.message = "Null WSI — createVulkanSurface unavailable (headless CI OK)";
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

void VulkanPresentable::requestResize(u32 width, u32 height) {
    m_desc.swapchainWidth = width;
    m_desc.swapchainHeight = height;
    m_status.pendingResizeWidth = width;
    m_status.pendingResizeHeight = height;
    m_status.resizePending = true;
    m_status.message = "Resize queued for present-path recreate";
}

renderer::SwapchainDesc VulkanPresentable::swapchainDesc() const {
    renderer::SwapchainDesc desc{};
    desc.surface = surfaceDesc();
    desc.width = m_desc.swapchainWidth;
    desc.height = m_desc.swapchainHeight;
    desc.vsyncMode = m_desc.vsyncMode;
    return desc;
}

} // namespace fuse::hybrid

#endif // defined(FUSE_HAS_VULKAN_RHI)
