#pragma once

#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/pipeline_cache.hpp>
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
    /// When non-null, used instead of `renderPass->nativeHandle()` (swapchain present pass).
    void* nativeRenderPassOverride = nullptr;
    PipelineCache* pipelineCache = nullptr;
    u32 colorFormat = 37; // VK_FORMAT_R8G8B8A8_UNORM
    /// 0 = no depth attachment in dynamic rendering; 126 = D32_SFLOAT.
    u32 depthFormat = 0;
    u32 polygonMode = 0;  // VK_POLYGON_MODE_FILL
    u32 cullMode = 0;     // VK_CULL_MODE_NONE (keep current default so existing triangle tests do not flip)
    bool depthTest = false;
    bool depthWrite = false;
    u32 depthCompareOp = 7; // VK_COMPARE_OP_ALWAYS to match current defaults
    /// When true, create with VkPipelineRenderingCreateInfo and VK_NULL_HANDLE render pass.
    bool useDynamicRendering = false;
    const char* debugName = nullptr;
};

struct GraphicsPipelineInfo {
    bool valid = false;
    bool dynamicRendering = false;
    u32 depthFormat = 0;
    bool hasDynamicDepth = false;
    u32 cacheSnapshotBytes = 0;
    u32 rebuildCount = 0;
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

    /// Destroys the current pipeline and recreates it from the last desc, using current shader
    /// module native handles. Returns false if the stored device is missing or not valid.
    bool rebuild();

private:
    GraphicsPipeline() = default;
    bool initialize(VulkanDevice& device, const GraphicsPipelineDesc& desc);
    void shutdown();

    VulkanDevice* m_device = nullptr;
    GraphicsPipelineDesc m_desc{};
    GraphicsPipelineInfo m_info;
    void* m_handle = nullptr;
};

} // namespace fuse::renderer
