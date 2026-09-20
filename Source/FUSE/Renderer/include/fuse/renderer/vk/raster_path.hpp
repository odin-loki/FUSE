#pragma once

#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/shader/shader_watch.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

struct RasterPathDesc {
    u32 width = 320;
    u32 height = 240;
    const char* vertexSpirvPath = nullptr;
    const char* fragmentSpirvPath = nullptr;
    BindlessDescriptors* bindless = nullptr;
};

struct RasterPathStats {
    bool pipelineReady = false;
    bool bindlessLayoutReady = false;
    bool indexBufferReady = false;
    bool depthAttachmentReady = false;
    u32 clearCount = 0;
    u32 triangleDrawCount = 0;
    u32 framesRecorded = 0;
    u32 shaderFilesWatched = 0;
    u32 shaderWatchPolls = 0;
    u32 pipelineReloadCount = 0;
    u64 pipelineContentHash = 0;
    u32 resizeCount = 0;
    u32 resizeNoOpCount = 0;
    u64 vertexDeviceAddress = 0;
    u64 indexDeviceAddress = 0;
    bool bufferDeviceAddressReady = false;
    std::string message;
};

/// B2.8 headless clear + triangle path — records against an offscreen target when Vulkan is ready.
class RasterPath {
public:
    static std::unique_ptr<RasterPath> create(VulkanDevice& device, const RasterPathDesc& desc);
    ~RasterPath();

    RasterPath(const RasterPath&) = delete;
    RasterPath& operator=(const RasterPath&) = delete;

    bool isReady() const { return m_stats.pipelineReady; }
    const RasterPathStats& lastStats() const { return m_stats; }

    /// Offscreen targets for frame-slot `vkCmdBeginRenderPass` encoding.
    VkFrameEncodeContext vulkanEncodeContext() const;

    /// Offscreen color image for graph-planned layout barriers.
    void* barrierImageHandle() const;

    /// Offscreen D32_SFLOAT depth image; null if not ready.
    void* depthImageHandle() const;

    /// Sampled color attachment view for composite bindless registration.
    void* colorViewHandle() const;

    /// UINT16 triangle index buffer (`{0,1,2}`); null if not ready.
    void* indexBufferHandle() const;

    /// Updates CPU stats from mirrored commands (GPU work lives in graph execute path).
    void updateStatsFromCommands(const RenderCommandList& commands);

    /// Legacy hook — stats only; real draws are encoded via `CommandBufferRecorder`.
    bool recordFrame(const RenderCommandList& commands);

    /// Recreate offscreen color+depth images, views, and framebuffer. Vertex/index buffers and
    /// the graphics pipeline stay. Zero size is rejected. Same size is a success no-op.
    bool resize(u32 width, u32 height);

private:
    RasterPath() = default;
    bool initialize(VulkanDevice& device, const RasterPathDesc& desc);
    void shutdown();
    void reloadPipelinesIfWatched();
    bool createOffscreenTargets();
    void destroyOffscreenTargets();

    VulkanDevice* m_device = nullptr;
    RasterPathDesc m_desc{};
    RasterPathStats m_stats;

    ShaderFileWatch m_shaderWatch;
    BindlessDescriptors m_ownedBindless;
    BindlessDescriptors* m_bindless = nullptr;
    bool m_ownedBindlessInitialized = false;

    std::unique_ptr<class RenderPass> m_renderPass;
    std::unique_ptr<class PipelineLayout> m_pipelineLayout;
    std::unique_ptr<class ShaderModule> m_vertexShader;
    std::unique_ptr<class ShaderModule> m_fragmentShader;
    std::unique_ptr<class GraphicsPipeline> m_graphicsPipeline;
    std::unique_ptr<class PipelineCache> m_pipelineCache;

#if defined(FUSE_VULKAN_BACKEND)
    void* m_vertexBuffer = nullptr;
    void* m_vertexMemory = nullptr;
    void* m_indexBuffer = nullptr;
    void* m_indexMemory = nullptr;
    void* m_colorImage = nullptr;
    void* m_colorMemory = nullptr;
    void* m_colorView = nullptr;
    void* m_depthImage = nullptr;
    void* m_depthMemory = nullptr;
    void* m_depthView = nullptr;
    void* m_framebuffer = nullptr;
#endif
};

} // namespace fuse::renderer
