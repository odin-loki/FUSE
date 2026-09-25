#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {

struct ShaderReflection;

struct PushConstantRangeDesc {
    u32 offset = 0;
    u32 size = 0;
    u32 stageFlags = 0;
};

struct DescriptorSetLayoutDesc {
    u32 set = 0;
    u32 bindingCount = 0;
};

/// Pipeline layout for B2.4 — optional bindless descriptor set (set 0) from `BindlessDescriptors`.
struct PipelineLayoutDesc {
    std::vector<PushConstantRangeDesc> pushConstants;
    std::vector<DescriptorSetLayoutDesc> descriptorSets;
    /// When non-null, bound as set 0 (`VkDescriptorSetLayout` from `BindlessDescriptors::layoutHandle()`).
    void* bindlessSetLayout = nullptr;
    const char* debugName = nullptr;
};

struct PipelineLayoutInfo {
    bool valid = false;
    u32 pushConstantRangeCount = 0;
    u32 descriptorSetCount = 0;
    bool hasBindlessSet = false;
    std::string message;
};

class PipelineLayout {
public:
    static std::unique_ptr<PipelineLayout> create(VulkanDevice& device,
                                                  const PipelineLayoutDesc& desc = {});

    /// WP-0.5: layout from shader reflection (merge stages with mergeShaderReflection first). Creates
    /// and owns one VkDescriptorSetLayout per set index in [0, reflection.setCount()) (empty layouts
    /// fill holes) plus one push-constant range [0, pushConstantBytes) for all reflected stages.
    /// Runtime-sized arrays (count 0) are rejected: pass a bindless layout through `create` instead.
    static std::unique_ptr<PipelineLayout> createFromReflection(VulkanDevice& device,
                                                                const ShaderReflection& reflection,
                                                                const char* debugName = nullptr);
    ~PipelineLayout();

    PipelineLayout(const PipelineLayout&) = delete;
    PipelineLayout& operator=(const PipelineLayout&) = delete;

    const PipelineLayoutInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }

    void* nativeHandle() const;

    /// VkDescriptorSetLayout of `set` for layouts made by createFromReflection (nullptr otherwise or
    /// when out of range).
    void* setLayoutHandle(u32 set) const;
    u32 ownedSetLayoutCount() const { return static_cast<u32>(m_ownedSetLayouts.size()); }

private:
    PipelineLayout() = default;
    bool initialize(VulkanDevice& device, const PipelineLayoutDesc& desc);
    bool initializeFromReflection(VulkanDevice& device, const ShaderReflection& reflection, const char* debugName);
    void shutdown();

    std::vector<void*> m_ownedSetLayouts;

    VulkanDevice* m_device = nullptr;
    PipelineLayoutInfo m_info;
    void* m_handle = nullptr;
};

} // namespace fuse::renderer
