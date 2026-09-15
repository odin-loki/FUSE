#include <fuse/renderer/vk/device.hpp>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

#if defined(FUSE_VULKAN_BACKEND)
bool extensionSupported(VkPhysicalDevice device, const char* name) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool queueFamilySupports(VkPhysicalDevice device, u32 family, VkQueueFlagBits flags) {
    VkQueueFamilyProperties props{};
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    if (family >= count) {
        return false;
    }
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    return (families[family].queueFlags & flags) != 0;
}

u32 findQueueFamily(VkPhysicalDevice device, VkQueueFlagBits flags) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        if ((families[i].queueFlags & flags) != 0) {
            return i;
        }
    }
    return 0;
}

const char* const kPreferredExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
};

const char* const kPlatformExternalExtensions[] = {
#if defined(_WIN32)
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#endif
};
#endif

} // namespace

std::unique_ptr<VulkanDevice> VulkanDevice::create(VulkanInstance& instance,
                                                   const VulkanDeviceDesc& desc) {
    auto device = std::unique_ptr<VulkanDevice>(new VulkanDevice());
    if (!device->initialize(instance, desc)) {
        device->m_info.valid = false;
    }
    return device;
}

VulkanDevice::~VulkanDevice() {
    shutdown();
}

void* VulkanDevice::nativeHandle() const {
    return m_handle;
}

bool VulkanDevice::initialize(VulkanInstance& instance, const VulkanDeviceDesc& desc) {
    m_instance = &instance;

#if defined(FUSE_VULKAN_BACKEND)
    if (!instance.isValid()) {
        m_info.message = "Device skipped — instance not ready";
        return false;
    }

    auto vkInstance = static_cast<VkInstance>(instance.nativeHandle());

    u32 deviceCount = 0;
    vkEnumeratePhysicalDevices(vkInstance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        m_info.message = "No Vulkan physical devices — CI headless stub path";
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(vkInstance, &deviceCount, devices.data());

    VkPhysicalDevice selected = devices.front();
    if (desc.preferDiscreteGpu) {
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                selected = candidate;
                break;
            }
        }
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(selected, &props);
    m_info.deviceName = props.deviceName;

    const u32 graphicsFamily = findQueueFamily(selected, VK_QUEUE_GRAPHICS_BIT);
    const u32 computeFamily = findQueueFamily(selected, VK_QUEUE_COMPUTE_BIT);
    const u32 transferFamily = findQueueFamily(selected, VK_QUEUE_TRANSFER_BIT);

    std::vector<const char*> enabledExtensions;
    for (const char* extension : kPreferredExtensions) {
        if (extensionSupported(selected, extension)) {
            enabledExtensions.push_back(extension);
        }
    }
    for (const char* extension : kPlatformExternalExtensions) {
        if (extensionSupported(selected, extension)) {
            enabledExtensions.push_back(extension);
        }
    }

    if (desc.requirePresentation &&
        !extensionSupported(selected, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        m_info.message = "Presentation requested but VK_KHR_swapchain unavailable";
        return false;
    }

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    const float queuePriority = 1.f;
    auto addQueue = [&](u32 family) {
        for (const VkDeviceQueueCreateInfo& existing : queueCreateInfos) {
            if (existing.queueFamilyIndex == family) {
                return;
            }
        }
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueInfo);
    };

    addQueue(graphicsFamily);
    if (computeFamily != graphicsFamily) {
        addQueue(computeFamily);
    }
    if (transferFamily != graphicsFamily && transferFamily != computeFamily) {
        addQueue(transferFamily);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<u32>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<u32>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames =
        enabledExtensions.empty() ? nullptr : enabledExtensions.data();

    VkDevice logicalDevice = VK_NULL_HANDLE;
    if (vkCreateDevice(selected, &createInfo, nullptr, &logicalDevice) != VK_SUCCESS) {
        m_info.message = "vkCreateDevice failed";
        return false;
    }

    m_physicalDevice = selected;
    m_handle = logicalDevice;
    m_info.valid = true;
    m_info.enabledExtensions = enabledExtensions;
    m_info.queues.graphicsFamily = graphicsFamily;
    m_info.queues.computeFamily = computeFamily;
    m_info.queues.transferFamily = transferFamily;

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    VkQueue transferQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(logicalDevice, graphicsFamily, 0, &graphicsQueue);
    vkGetDeviceQueue(logicalDevice, computeFamily, 0, &computeQueue);
    vkGetDeviceQueue(logicalDevice, transferFamily, 0, &transferQueue);
    m_info.queues.graphics = graphicsQueue;
    m_info.queues.compute = computeQueue;
    m_info.queues.transfer = transferQueue;
    m_info.vmaAllocator = nullptr;
    m_info.message = "Logical device ready (VMA deferred)";
    return true;
#else
    (void)desc;
    m_info.message = "Device skipped — Vulkan backend disabled at build time";
    return false;
#endif
}

void VulkanDevice::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle != nullptr) {
        vkDestroyDevice(static_cast<VkDevice>(m_handle), nullptr);
        m_handle = nullptr;
    }
    m_physicalDevice = nullptr;
#endif
}

} // namespace fuse::renderer
