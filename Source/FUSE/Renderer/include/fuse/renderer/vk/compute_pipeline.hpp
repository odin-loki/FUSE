#pragma once

#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/pipeline_cache.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

struct ComputePipelineDesc {
    PipelineLayout* layout = nullptr;
    ShaderModule* computeShader = nullptr;
    PipelineCache* pipelineCache = nullptr;
    u32 localSizeX = 1;
    u32 localSizeY = 1;
    u32 localSizeZ = 1;
    const char* debugName = nullptr;
};

struct ComputePipelineInfo {
    bool valid = false;
    u32 localSizeX = 1;
    u32 localSizeY = 1;
    u32 localSizeZ = 1;
    u32 cacheSnapshotBytes = 0;
    u32 rebuildCount = 0;
    std::string message;
};

/// B2.4 — compute pipeline built from a compute ShaderModule + PipelineLayout.
class ComputePipeline {
public:
    static std::unique_ptr<ComputePipeline> create(VulkanDevice& device,
                                                   const ComputePipelineDesc& desc);
    ~ComputePipeline();

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    const ComputePipelineInfo& info() const { return m_info; }
    bool isValid() const { return m_info.valid; }

    void* nativeHandle() const;

    /// Destroys the current pipeline and recreates it from the last desc, using current shader
    /// module native handles. Returns false if the stored device is missing or not valid.
    bool rebuild();

private:
    ComputePipeline() = default;
    bool initialize(VulkanDevice& device, const ComputePipelineDesc& desc);
    void shutdown();

    VulkanDevice* m_device = nullptr;
    ComputePipelineDesc m_desc{};
    ComputePipelineInfo m_info;
    void* m_handle = nullptr;
};

} // namespace fuse::renderer
