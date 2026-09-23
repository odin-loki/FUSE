#pragma once

#include <fuse/renderer/vk/instance.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {

struct VulkanQueues {
    void* graphics = nullptr;
    void* compute = nullptr;
    void* transfer = nullptr;
    u32 graphicsFamily = 0;
    u32 computeFamily = 0;
    u32 transferFamily = 0;
    bool dedicatedTransfer = false;
    bool dedicatedCompute = false;
};

/// Descriptor limits that bind a VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT layout
/// (the bindless set): per type, min(maxDescriptorSetUpdateAfterBind*, maxPerStageDescriptorUpdateAfterBind*).
/// Falls back to the core (non update-after-bind) limits when descriptor indexing is unavailable.
/// All zero when no device was created.
struct VulkanDescriptorLimits {
    u32 sampledImages = 0;
    u32 storageImages = 0;
    u32 storageBuffers = 0;
    u32 uniformBuffers = 0;
    u32 samplers = 0;
    /// maxPerStageUpdateAfterBindResources: all non-sampler descriptors visible to one stage.
    u32 perStageResources = 0;
    /// maxUpdateAfterBindDescriptorsInAllPools.
    u32 allPools = 0;
};

struct VulkanDeviceInfo {
    bool valid = false;
    std::string deviceName;
    std::string message;
    VulkanQueues queues{};
    std::vector<const char*> enabledExtensions;
    /// VmaAllocator of the GpuAllocator bound to this device (null on the native/stub paths).
    void* vmaAllocator = nullptr;
    /// Effective Vulkan API version: min(instance apiVersion, physical device apiVersion).
    u32 apiVersion = 0;
    bool descriptorIndexing = false;
    /// Per-descriptor-type UPDATE_AFTER_BIND support (bindless layout gates binding flags on these).
    bool sampledImageUpdateAfterBind = false; ///< Also covers VK_DESCRIPTOR_TYPE_SAMPLER
    bool storageImageUpdateAfterBind = false;
    bool storageBufferUpdateAfterBind = false;
    bool uniformBufferUpdateAfterBind = false;
    bool sampledImageNonUniformIndexing = false;
    VulkanDescriptorLimits descriptorLimits{};
    /// True when VK_KHR_swapchain was enabled (requires VK_KHR_surface on the instance).
    bool swapchainExtension = false;
    bool bufferDeviceAddress = false;
    bool timelineSemaphore = false;
    bool dynamicRendering = false;
    bool samplerAnisotropy = false;
    float maxSamplerAnisotropy = 1.f;
    u32 deviceType = 0; // VkPhysicalDeviceType numeric
    /// Physical device selection: index of the chosen device in vkEnumeratePhysicalDevices order
    /// (UINT32_MAX when none), the number enumerated, and a per-device summary ("#i 'name' (type,
    /// MiB device-local, 2D limit): selected | suitable | rejected, reason").
    u32 physicalDeviceIndex = UINT32_MAX;
    u32 physicalDeviceCount = 0;
    std::string selection;
};

struct VulkanDeviceDesc {
    bool requirePresentation = false;
    /// Devices missing a hard requirement (Vulkan 1.2, a graphics queue family, timeline
    /// semaphores, and VK_KHR_swapchain + a present-capable family when `requirePresentation`) are
    /// never picked. Among the rest: true ranks discrete > integrated > virtual > CPU, then more
    /// device-local memory, then larger maxImageDimension2D, then enumeration order; false takes
    /// the first suitable device in enumeration order.
    bool preferDiscreteGpu = true;
    /// Opaque VkSurfaceKHR; used to pick a present-capable graphics queue family.
    void* presentSurface = nullptr;
};

class VulkanDevice {
public:
    static std::unique_ptr<VulkanDevice> create(VulkanInstance& instance,
                                                const VulkanDeviceDesc& desc = {});
    ~VulkanDevice();

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    const VulkanDeviceInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }

    void* nativeHandle() const;
    void* nativePhysicalDevice() const;
    void* instanceHandle() const;
    void setVmaAllocator(void* allocator);
    /// vkDeviceWaitIdle — call before destroying objects a submitted command buffer may still use.
    void waitIdle() const;

    const VulkanQueues& queues() const { return m_info.queues; }

private:
    VulkanDevice() = default;
    bool initialize(VulkanInstance& instance, const VulkanDeviceDesc& desc);
    void shutdown();

    VulkanInstance* m_instance = nullptr;
    VulkanDeviceInfo m_info;
    void* m_handle = nullptr;
    void* m_physicalDevice = nullptr;
};

} // namespace fuse::renderer
