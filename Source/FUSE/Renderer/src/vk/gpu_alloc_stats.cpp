#include <fuse/renderer/vk/gpu_alloc_stats.hpp>

#include <fuse/renderer/resources.hpp>

#include <cstring>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

GpuStatsHookFn g_statsHook = nullptr;
void* g_statsHookUserData = nullptr;

usize bytesPerPixel(GpuFormat format) {
    switch (format) {
    case GpuFormat::R8G8B8A8Unorm:
    case GpuFormat::R8G8B8A8Srgb:
        return 4;
    case GpuFormat::R16G16Sfloat:
        return 4;
    case GpuFormat::R16G16B16A16Sfloat:
        return 8;
    case GpuFormat::D32Sfloat:
    case GpuFormat::R32Sfloat:
        return 4;
    case GpuFormat::Undefined:
    default:
        return 4;
    }
}

} // namespace

void setGlobalGpuStatsHook(GpuStatsHookFn hook, void* userData) {
    g_statsHook = hook;
    g_statsHookUserData = userData;
}

void clearGlobalGpuStatsHook() {
    g_statsHook = nullptr;
    g_statsHookUserData = nullptr;
}

void notifyGpuStats(const char* allocatorName, const GpuAllocStats& stats) {
    if (g_statsHook != nullptr) {
        g_statsHook(allocatorName, stats, g_statsHookUserData);
    }
}

namespace gpu_alloc_detail {

void recordBufferAlloc(GpuAllocStats& stats, usize bytes) {
    stats.bufferBytes += bytes;
    stats.usedBytes += bytes;
    stats.bufferCount += 1;
    stats.allocCount += 1;
    updatePeak(stats);
}

void recordImageAlloc(GpuAllocStats& stats, usize bytes) {
    stats.imageBytes += bytes;
    stats.usedBytes += bytes;
    stats.imageCount += 1;
    stats.allocCount += 1;
    updatePeak(stats);
}

void recordBufferFree(GpuAllocStats& stats, usize bytes) {
    if (bytes <= stats.bufferBytes) {
        stats.bufferBytes -= bytes;
    } else {
        stats.bufferBytes = 0;
    }
    if (bytes <= stats.usedBytes) {
        stats.usedBytes -= bytes;
    } else {
        stats.usedBytes = 0;
    }
    if (stats.bufferCount > 0) {
        stats.bufferCount -= 1;
    }
    stats.freeCount += 1;
}

void recordImageFree(GpuAllocStats& stats, usize bytes) {
    if (bytes <= stats.imageBytes) {
        stats.imageBytes -= bytes;
    } else {
        stats.imageBytes = 0;
    }
    if (bytes <= stats.usedBytes) {
        stats.usedBytes -= bytes;
    } else {
        stats.usedBytes = 0;
    }
    if (stats.imageCount > 0) {
        stats.imageCount -= 1;
    }
    stats.freeCount += 1;
}

void recordFailedAlloc(GpuAllocStats& stats) {
    stats.failedAllocs += 1;
}

void updatePeak(GpuAllocStats& stats) {
    if (stats.usedBytes > stats.peakUsedBytes) {
        stats.peakUsedBytes = stats.usedBytes;
    }
}

usize estimateImageBytes(const TextureDesc& desc) {
    const usize pixelBytes = bytesPerPixel(desc.format);
    const usize width = desc.width > 0 ? desc.width : 1u;
    const usize height = desc.height > 0 ? desc.height : 1u;
    const usize depth = desc.depth > 0 ? desc.depth : 1u;
    const usize mips = desc.mipLevels > 0 ? desc.mipLevels : 1u;
    const usize layers = desc.arrayLayers > 0 ? desc.arrayLayers : 1u;
    return width * height * depth * pixelBytes * mips * layers;
}

void queryDeviceLocalHeapBudget(void* vkInstance, void* vkPhysicalDevice, GpuAllocStats& stats) {
    stats.deviceLocalHeapBytes = 0;
    stats.deviceLocalBudgetBytes = 0;
    stats.deviceLocalHeapIndex = 0;

#if defined(FUSE_VULKAN_BACKEND)
    if (vkPhysicalDevice == nullptr) {
        return;
    }

    const auto physicalDevice = static_cast<VkPhysicalDevice>(vkPhysicalDevice);
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    bool foundDeviceLocal = false;
    for (u32 i = 0; i < memProperties.memoryHeapCount; ++i) {
        if ((memProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            stats.deviceLocalHeapIndex = i;
            stats.deviceLocalHeapBytes = static_cast<usize>(memProperties.memoryHeaps[i].size);
            foundDeviceLocal = true;
            break;
        }
    }
    if (!foundDeviceLocal) {
        return;
    }

    stats.deviceLocalBudgetBytes = stats.deviceLocalHeapBytes;

    bool hasMemoryBudgetExt = false;
    u32 extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
    if (extensionCount > 0) {
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount,
                                             extensions.data());
        for (const VkExtensionProperties& extension : extensions) {
            if (std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                hasMemoryBudgetExt = true;
                break;
            }
        }
    }
    if (!hasMemoryBudgetExt || vkInstance == nullptr) {
        return;
    }

    const auto instance = static_cast<VkInstance>(vkInstance);
    auto getMemProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2"));
    if (getMemProps2 == nullptr) {
        getMemProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2KHR"));
    }
    if (getMemProps2 == nullptr) {
        return;
    }

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProps{};
    budgetProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

    VkPhysicalDeviceMemoryProperties2 memProps2{};
    memProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    memProps2.pNext = &budgetProps;
    getMemProps2(physicalDevice, &memProps2);

    if (stats.deviceLocalHeapIndex < VK_MAX_MEMORY_HEAPS) {
        const VkDeviceSize budget = budgetProps.heapBudget[stats.deviceLocalHeapIndex];
        if (budget != 0) {
            stats.deviceLocalBudgetBytes = static_cast<usize>(budget);
        }
    }
#else
    (void)vkInstance;
    (void)vkPhysicalDevice;
#endif
}

} // namespace gpu_alloc_detail

} // namespace fuse::renderer
