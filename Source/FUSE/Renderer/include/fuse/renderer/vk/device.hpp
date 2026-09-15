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
};

struct VulkanDeviceInfo {
    bool valid = false;
    std::string deviceName;
    std::string message;
    VulkanQueues queues{};
    std::vector<const char*> enabledExtensions;
    /// VMA placeholder — wired in a later B2.1 follow-up.
    void* vmaAllocator = nullptr;
};

struct VulkanDeviceDesc {
    bool requirePresentation = false;
    bool preferDiscreteGpu = true;
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
