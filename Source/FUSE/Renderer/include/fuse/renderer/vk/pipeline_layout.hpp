#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {

struct PushConstantRangeDesc {
    u32 offset = 0;
    u32 size = 0;
    u32 stageFlags = 0;
};

struct DescriptorSetLayoutDesc {
    u32 set = 0;
    u32 bindingCount = 0;
};

/// Placeholder pipeline layout for B2.4 — bindless descriptor scaffolding deferred to B2.3 follow-up.
struct PipelineLayoutDesc {
    std::vector<PushConstantRangeDesc> pushConstants;
    std::vector<DescriptorSetLayoutDesc> descriptorSets;
    const char* debugName = nullptr;
};

struct PipelineLayoutInfo {
    bool valid = false;
    u32 pushConstantRangeCount = 0;
    u32 descriptorSetCount = 0;
    std::string message;
};

class PipelineLayout {
public:
    static std::unique_ptr<PipelineLayout> create(VulkanDevice& device,
                                                  const PipelineLayoutDesc& desc = {});
    ~PipelineLayout();

    PipelineLayout(const PipelineLayout&) = delete;
    PipelineLayout& operator=(const PipelineLayout&) = delete;

    const PipelineLayoutInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }

    void* nativeHandle() const;

private:
    PipelineLayout() = default;
    bool initialize(VulkanDevice& device, const PipelineLayoutDesc& desc);
    void shutdown();

    VulkanDevice* m_device = nullptr;
    PipelineLayoutInfo m_info;
    void* m_handle = nullptr;
};

} // namespace fuse::renderer
