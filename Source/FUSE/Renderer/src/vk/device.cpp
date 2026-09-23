#include <fuse/renderer/vk/device.hpp>

#include <algorithm>
#include <cstring>
#include <string>
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

u32 findComputeFamily(VkPhysicalDevice device) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            return i;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            return i;
        }
    }
    return 0;
}

u32 findTransferFamily(VkPhysicalDevice device) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_TRANSFER_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
            (flags & VK_QUEUE_COMPUTE_BIT) == 0) {
            return i;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_TRANSFER_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            return i;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0) {
            return i;
        }
    }
    return 0;
}

bool queueFamilyPresentsToSurface(VkPhysicalDevice device, u32 family, VkSurfaceKHR surface) {
    VkBool32 supported = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface, &supported) != VK_SUCCESS) {
        return false;
    }
    return supported == VK_TRUE;
}

u32 findPresentableGraphicsFamily(VkPhysicalDevice device, VkSurfaceKHR surface, bool& found) {
    found = false;
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        if (queueFamilyPresentsToSurface(device, i, surface)) {
            found = true;
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
#else
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
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

void* VulkanDevice::nativePhysicalDevice() const {
    return m_physicalDevice;
}

void* VulkanDevice::instanceHandle() const {
    if (m_instance == nullptr) {
        return nullptr;
    }
    return m_instance->nativeHandle();
}

void VulkanDevice::setVmaAllocator(void* allocator) {
    m_info.vmaAllocator = allocator;
}

void VulkanDevice::waitIdle() const {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle != nullptr) {
        vkDeviceWaitIdle(static_cast<VkDevice>(m_handle));
    }
#endif
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

    u32 graphicsFamily = findQueueFamily(selected, VK_QUEUE_GRAPHICS_BIT);
    const u32 computeFamily = findComputeFamily(selected);
    const u32 transferFamily = findTransferFamily(selected);

    // VK_KHR_swapchain and surface queries both depend on VK_KHR_surface at instance level.
    const bool instanceHasSurface = instance.info().instanceHasExtension(VK_KHR_SURFACE_EXTENSION_NAME);

    std::string presentNote;
    if (desc.presentSurface != nullptr && !instanceHasSurface) {
        presentNote = "present surface ignored: instance lacks VK_KHR_surface";
        m_info.message = presentNote;
        if (desc.requirePresentation) {
            return false;
        }
    } else if (desc.presentSurface != nullptr) {
        const auto surface = reinterpret_cast<VkSurfaceKHR>(desc.presentSurface);
        bool foundPresentableGraphics = false;
        const u32 presentableFamily =
            findPresentableGraphicsFamily(selected, surface, foundPresentableGraphics);
        if (foundPresentableGraphics) {
            graphicsFamily = presentableFamily;
        } else {
            presentNote = "no graphics queue family presents to the given surface";
            m_info.message = presentNote;
            if (desc.requirePresentation) {
                return false;
            }
        }
    }

    std::vector<const char*> enabledExtensions;
    for (const char* extension : kPreferredExtensions) {
        if (!instanceHasSurface && std::strcmp(extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            continue;
        }
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
        (!instanceHasSurface || !extensionSupported(selected, VK_KHR_SWAPCHAIN_EXTENSION_NAME))) {
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

    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceDynamicRenderingFeatures supportedDyn{};
    supportedDyn.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    supported12.pNext = &supportedDyn;
    VkPhysicalDeviceFeatures2 supported2{};
    supported2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported2.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(selected, &supported2);

    VkPhysicalDeviceVulkan12Features enabled12{};
    enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled12.descriptorIndexing = supported12.descriptorIndexing;
    enabled12.descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound;
    enabled12.runtimeDescriptorArray = supported12.runtimeDescriptorArray;
    enabled12.descriptorBindingSampledImageUpdateAfterBind =
        supported12.descriptorBindingSampledImageUpdateAfterBind;
    enabled12.descriptorBindingStorageBufferUpdateAfterBind =
        supported12.descriptorBindingStorageBufferUpdateAfterBind;
    enabled12.descriptorBindingStorageImageUpdateAfterBind =
        supported12.descriptorBindingStorageImageUpdateAfterBind;
    enabled12.descriptorBindingUniformBufferUpdateAfterBind =
        supported12.descriptorBindingUniformBufferUpdateAfterBind;
    // Composite/bindless shaders index texture arrays with nonuniformEXT.
    enabled12.shaderSampledImageArrayNonUniformIndexing =
        supported12.shaderSampledImageArrayNonUniformIndexing;
    enabled12.shaderStorageImageArrayNonUniformIndexing =
        supported12.shaderStorageImageArrayNonUniformIndexing;
    enabled12.shaderStorageBufferArrayNonUniformIndexing =
        supported12.shaderStorageBufferArrayNonUniformIndexing;
    enabled12.bufferDeviceAddress = supported12.bufferDeviceAddress;
    enabled12.timelineSemaphore = supported12.timelineSemaphore;

    VkPhysicalDeviceDynamicRenderingFeatures enabledDyn{};
    enabledDyn.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;

    bool hasDynamicRenderingExt = false;
    for (const char* extension : enabledExtensions) {
        if (std::strcmp(extension, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0) {
            hasDynamicRenderingExt = true;
            break;
        }
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = supported2.features.samplerAnisotropy;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &enabled12;
    if (hasDynamicRenderingExt && supportedDyn.dynamicRendering) {
        enabledDyn.dynamicRendering = VK_TRUE;
        enabledDyn.pNext = &enabled12;
        createInfo.pNext = &enabledDyn;
    }
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
    m_info.descriptorIndexing = enabled12.descriptorIndexing == VK_TRUE;
    m_info.sampledImageUpdateAfterBind = enabled12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
    m_info.storageImageUpdateAfterBind = enabled12.descriptorBindingStorageImageUpdateAfterBind == VK_TRUE;
    m_info.storageBufferUpdateAfterBind = enabled12.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE;
    m_info.uniformBufferUpdateAfterBind = enabled12.descriptorBindingUniformBufferUpdateAfterBind == VK_TRUE;
    m_info.sampledImageNonUniformIndexing = enabled12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    for (const char* extension : enabledExtensions) {
        if (std::strcmp(extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            m_info.swapchainExtension = true;
        }
    }
    m_info.bufferDeviceAddress = enabled12.bufferDeviceAddress == VK_TRUE;
    m_info.timelineSemaphore = enabled12.timelineSemaphore == VK_TRUE;
    m_info.dynamicRendering = enabledDyn.dynamicRendering == VK_TRUE;
    m_info.samplerAnisotropy = deviceFeatures.samplerAnisotropy == VK_TRUE;
    m_info.maxSamplerAnisotropy = props.limits.maxSamplerAnisotropy;
    {
        VkPhysicalDeviceVulkan12Properties props12{};
        props12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &props12;
        vkGetPhysicalDeviceProperties2(selected, &props2);
        const VkPhysicalDeviceLimits& core = props.limits;
        VulkanDescriptorLimits& limits = m_info.descriptorLimits;
        if (m_info.descriptorIndexing && props12.maxPerStageUpdateAfterBindResources > 0u) {
            limits.sampledImages = std::min(props12.maxDescriptorSetUpdateAfterBindSampledImages,
                                            props12.maxPerStageDescriptorUpdateAfterBindSampledImages);
            limits.storageImages = std::min(props12.maxDescriptorSetUpdateAfterBindStorageImages,
                                            props12.maxPerStageDescriptorUpdateAfterBindStorageImages);
            limits.storageBuffers = std::min(props12.maxDescriptorSetUpdateAfterBindStorageBuffers,
                                             props12.maxPerStageDescriptorUpdateAfterBindStorageBuffers);
            limits.uniformBuffers = std::min(props12.maxDescriptorSetUpdateAfterBindUniformBuffers,
                                             props12.maxPerStageDescriptorUpdateAfterBindUniformBuffers);
            limits.samplers = std::min(props12.maxDescriptorSetUpdateAfterBindSamplers,
                                       props12.maxPerStageDescriptorUpdateAfterBindSamplers);
            limits.perStageResources = props12.maxPerStageUpdateAfterBindResources;
            limits.allPools = props12.maxUpdateAfterBindDescriptorsInAllPools;
        } else {
            limits.sampledImages = std::min(core.maxDescriptorSetSampledImages, core.maxPerStageDescriptorSampledImages);
            limits.storageImages = std::min(core.maxDescriptorSetStorageImages, core.maxPerStageDescriptorStorageImages);
            limits.storageBuffers =
                std::min(core.maxDescriptorSetStorageBuffers, core.maxPerStageDescriptorStorageBuffers);
            limits.uniformBuffers =
                std::min(core.maxDescriptorSetUniformBuffers, core.maxPerStageDescriptorUniformBuffers);
            limits.samplers = std::min(core.maxDescriptorSetSamplers, core.maxPerStageDescriptorSamplers);
            limits.perStageResources = core.maxPerStageResources;
            limits.allPools = UINT32_MAX;
        }
    }
    m_info.deviceType = static_cast<u32>(props.deviceType);
    m_info.apiVersion = std::min(instance.info().apiVersion, props.apiVersion);
    m_info.queues.graphicsFamily = graphicsFamily;
    m_info.queues.computeFamily = computeFamily;
    m_info.queues.transferFamily = transferFamily;
    m_info.queues.dedicatedTransfer = transferFamily != graphicsFamily;
    m_info.queues.dedicatedCompute = computeFamily != graphicsFamily;

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
    m_info.message = "Logical device ready (VMA via GpuAllocator)";
    if (!presentNote.empty()) {
        m_info.message += " — ";
        m_info.message += presentNote;
    }
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
