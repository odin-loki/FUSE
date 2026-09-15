#include <fuse/renderer/vk/swapchain.hpp>

#include <fuse/renderer/vk/swapchain_util.hpp>

#include <algorithm>
#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

#if defined(FUSE_VULKAN_BACKEND)
bool hasSwapchainExtension(const VulkanDevice& device) {
    for (const char* extension : device.info().enabledExtensions) {
        if (std::strcmp(extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            return true;
        }
    }
    return false;
}

VkFormat toVkFormat(u32 format) {
    return static_cast<VkFormat>(format);
}
#endif

} // namespace

std::unique_ptr<VulkanSwapchain> VulkanSwapchain::create(VulkanDevice& device, const SwapchainDesc& desc) {
    auto swapchain = std::unique_ptr<VulkanSwapchain>(new VulkanSwapchain());
    if (!swapchain->initialize(device, desc)) {
        return swapchain;
    }
    return swapchain;
}

VulkanSwapchain::~VulkanSwapchain() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr) {
        if (m_handle != nullptr) {
            vkDestroySwapchainKHR(static_cast<VkDevice>(m_device),
                                  static_cast<VkSwapchainKHR>(m_handle),
                                  nullptr);
            m_handle = nullptr;
        }
        for (SwapchainImageInfo& image : m_images) {
            if (image.view != nullptr) {
                vkDestroyImageView(static_cast<VkDevice>(m_device),
                                   static_cast<VkImageView>(image.view),
                                   nullptr);
                image.view = nullptr;
            }
        }
    }
#endif
    m_images.clear();
    m_device = nullptr;
    m_physicalDevice = nullptr;
    m_graphicsQueue = nullptr;
}

bool VulkanSwapchain::initialize(VulkanDevice& device, const SwapchainDesc& desc) {
    m_desc = desc;
    m_surfaceDesc = desc.surface;
    m_info.width = desc.width;
    m_info.height = desc.height;
    m_info.imageCount = desc.imageCount;
    m_info.format = desc.preferredFormat;

    if (!device.isValid()) {
        m_info.headless = true;
        m_info.message = "Swapchain headless — device not ready";
        return true;
    }

    const VulkanSurface surface = VulkanSurface::fromDesc(desc.surface);
    if (!surface.isPresentable()) {
        m_info.headless = true;
        m_info.ready = false;
        m_info.message = surface.info().message;
        return true;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (!hasSwapchainExtension(device)) {
        m_info.headless = true;
        m_info.ready = false;
        m_info.message = "VkSurfaceKHR provided but VK_KHR_swapchain not enabled on device";
        return true;
    }

    m_device = device.nativeHandle();
    m_physicalDevice = device.nativePhysicalDevice();
    m_graphicsQueueFamily = device.queues().graphicsFamily;
    m_graphicsQueue = device.queues().graphics;

    if (!createSwapchainResources(device, desc)) {
        m_info.headless = true;
        m_info.ready = false;
        return true;
    }

    m_info.headless = false;
    m_info.ready = true;
    m_info.message = "VkSwapchainKHR created";
    return true;
#else
    m_info.headless = true;
    m_info.message = "Swapchain stub — Vulkan backend disabled at build time";
    return true;
#endif
}

void VulkanSwapchain::shutdown(VulkanDevice& device) {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle != nullptr && m_device != nullptr) {
        vkDestroySwapchainKHR(static_cast<VkDevice>(m_device),
                              static_cast<VkSwapchainKHR>(m_handle),
                              nullptr);
        m_handle = nullptr;
    }
    for (SwapchainImageInfo& image : m_images) {
        if (image.view != nullptr && m_device != nullptr) {
            vkDestroyImageView(static_cast<VkDevice>(m_device),
                               static_cast<VkImageView>(image.view),
                               nullptr);
            image.view = nullptr;
        }
    }
    m_images.clear();
#else
    (void)device;
#endif
}

bool VulkanSwapchain::createSwapchainResources(VulkanDevice& device, const SwapchainDesc& desc) {
#if defined(FUSE_VULKAN_BACKEND)
    auto vkPhysicalDevice = static_cast<VkPhysicalDevice>(m_physicalDevice);
    auto vkDevice = static_cast<VkDevice>(m_device);
    auto vkSurface = static_cast<VkSurfaceKHR>(desc.surface.nativeSurface);

    VkBool32 presentSupported = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(vkPhysicalDevice, m_graphicsQueueFamily, vkSurface,
                                             &presentSupported) != VK_SUCCESS ||
        presentSupported != VK_TRUE) {
        m_info.message = "Graphics queue does not support presentation for surface";
        return false;
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkPhysicalDevice, vkSurface, &capabilities);

    u32 formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vkPhysicalDevice, vkSurface, &formatCount, nullptr);
    if (formatCount == 0) {
        m_info.message = "No surface formats available";
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vkPhysicalDevice, vkSurface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosenFormat = formats.front();
    const VkFormat preferred = toVkFormat(desc.preferredFormat);
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosenFormat = format;
            break;
        }
    }

    u32 presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vkPhysicalDevice, vkSurface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vkPhysicalDevice, vkSurface, &presentModeCount,
                                              presentModes.data());

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (desc.vsyncMode == VsyncMode::Mailbox) {
        for (VkPresentModeKHR mode : presentModes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                presentMode = mode;
                break;
            }
        }
    } else if (desc.vsyncMode == VsyncMode::Immediate) {
        for (VkPresentModeKHR mode : presentModes) {
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                presentMode = mode;
                break;
            }
        }
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(desc.width, capabilities.minImageExtent.width,
                                  capabilities.maxImageExtent.width);
        extent.height = std::clamp(desc.height, capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);
    }

    u32 imageCount = desc.imageCount;
    if (capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }
    imageCount = std::max(imageCount, capabilities.minImageCount);

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = vkSurface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = chosenFormat.format;
    createInfo.imageColorSpace = chosenFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(vkDevice, &createInfo, nullptr, &swapchain) != VK_SUCCESS) {
        m_info.message = "vkCreateSwapchainKHR failed";
        return false;
    }

    m_handle = swapchain;
    m_info.width = extent.width;
    m_info.height = extent.height;
    m_info.imageCount = imageCount;
    m_info.format = static_cast<u32>(chosenFormat.format);

    u32 swapchainImageCount = 0;
    vkGetSwapchainImagesKHR(vkDevice, swapchain, &swapchainImageCount, nullptr);
    std::vector<VkImage> swapchainImages(swapchainImageCount);
    vkGetSwapchainImagesKHR(vkDevice, swapchain, &swapchainImageCount, swapchainImages.data());

    m_images.resize(swapchainImageCount);
    for (u32 i = 0; i < swapchainImageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = chosenFormat.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &view) != VK_SUCCESS) {
            m_info.message = "Swapchain image view creation failed";
            shutdown(device);
            return false;
        }

        m_images[i].image = swapchainImages[i];
        m_images[i].view = view;
        m_images[i].index = i;
    }

    return true;
#else
    (void)device;
    (void)desc;
    return false;
#endif
}

bool VulkanSwapchain::rebuild(VulkanDevice& device, u32 width, u32 height) {
    if (!isValidSwapchainExtent(width, height)) {
        m_info.message = "Swapchain rebuild rejected — extent must be non-zero";
        return false;
    }

    if (m_info.headless || !m_info.ready) {
        m_desc.width = width;
        m_desc.height = height;
        m_info.width = width;
        m_info.height = height;
        ++m_info.recreateCount;
        m_info.message = "Headless swapchain recorded resize (no VkSwapchainKHR)";
        return true;
    }

#if defined(FUSE_VULKAN_BACKEND)
    shutdown(device);
    m_desc.width = width;
    m_desc.height = height;
    if (!createSwapchainResources(device, m_desc)) {
        m_info.ready = false;
        return false;
    }
    m_info.ready = true;
    ++m_info.recreateCount;
    m_info.message = "VkSwapchainKHR rebuilt";
    return true;
#else
    (void)device;
    (void)width;
    (void)height;
    return false;
#endif
}

u32 VulkanSwapchain::acquireNextImage(void* imageAvailableSemaphore) {
#if defined(FUSE_VULKAN_BACKEND)
    if (isEmpty()) {
        return UINT32_MAX;
    }

    u32 imageIndex = 0;
    const VkResult result = vkAcquireNextImageKHR(
        static_cast<VkDevice>(m_device),
        static_cast<VkSwapchainKHR>(m_handle),
        UINT64_MAX,
        static_cast<VkSemaphore>(imageAvailableSemaphore),
        VK_NULL_HANDLE,
        &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return UINT32_MAX;
    }
    if (result != VK_SUCCESS) {
        return UINT32_MAX;
    }
    return imageIndex;
#else
    (void)imageAvailableSemaphore;
    return UINT32_MAX;
#endif
}

bool VulkanSwapchain::present(void* renderFinishedSemaphore, u32 imageIndex) {
#if defined(FUSE_VULKAN_BACKEND)
    if (isEmpty() || isEmptyAcquireResult(imageIndex) || m_graphicsQueue == nullptr) {
        return false;
    }

    VkSemaphore waitSemaphore = static_cast<VkSemaphore>(renderFinishedSemaphore);
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &waitSemaphore;

    VkSwapchainKHR swapchain = static_cast<VkSwapchainKHR>(m_handle);
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult result =
        vkQueuePresentKHR(static_cast<VkQueue>(m_graphicsQueue), &presentInfo);
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
#else
    (void)renderFinishedSemaphore;
    (void)imageIndex;
    return false;
#endif
}

} // namespace fuse::renderer
