#pragma once

#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/render_pass.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

struct GraphicsPipelineDesc {
    PipelineLayout* layout = nullptr;
    ShaderModule* vertexShader = nullptr;
    ShaderModule* fragmentShader = nullptr;
    RenderPass* renderPass = nullptr;
    u32 colorFormat = 37; // VK_FORMAT_R8G8B8A8_UNORM
    const char* debugName = nullptr;
};

struct GraphicsPipelineInfo {
    bool valid = false;
    std::string message;
};

/// B2.8 — graphics pipeline scaffold built from B2.4 shader modules + pipeline layout.
class GraphicsPipeline {
public:
    static std::unique_ptr<GraphicsPipeline> create(VulkanDevice& device,
                                                    const GraphicsPipelineDesc& desc);
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    const GraphicsPipelineInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }

    void* nativeHandle() const;

private:
    GraphicsPipeline() = default;
    bool initialize(VulkanDevice& device, const GraphicsPipelineDesc& desc);
    void shutdown();

    VulkanDevice* m_device = nullptr;
    GraphicsPipelineInfo m_info;
    void* m_handle = nullptr;
};

} // namespace fuse::renderer
