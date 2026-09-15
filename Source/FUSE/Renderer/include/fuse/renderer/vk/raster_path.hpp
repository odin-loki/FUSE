#pragma once

#include <fuse/renderer/render_command_list.hpp>
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
};

struct RasterPathStats {
    bool pipelineReady = false;
    u32 clearCount = 0;
    u32 triangleDrawCount = 0;
    u32 framesRecorded = 0;
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

    /// Mirrors RenderCommandList clears and issues one triangle draw per frame when ready.
    bool recordFrame(const RenderCommandList& commands);

private:
    RasterPath() = default;
    bool initialize(VulkanDevice& device, const RasterPathDesc& desc);
    void shutdown();
    bool recordFrameVulkan(const RenderCommandList& commands);
    bool recordFrameStub(const RenderCommandList& commands);

    VulkanDevice* m_device = nullptr;
    RasterPathDesc m_desc{};
    RasterPathStats m_stats;

    std::unique_ptr<class RenderPass> m_renderPass;
    std::unique_ptr<class PipelineLayout> m_pipelineLayout;
    std::unique_ptr<class ShaderModule> m_vertexShader;
    std::unique_ptr<class ShaderModule> m_fragmentShader;
    std::unique_ptr<class GraphicsPipeline> m_graphicsPipeline;

#if defined(FUSE_VULKAN_BACKEND)
    void* m_commandPool = nullptr;
    void* m_vertexBuffer = nullptr;
    void* m_vertexMemory = nullptr;
    void* m_colorImage = nullptr;
    void* m_colorMemory = nullptr;
    void* m_colorView = nullptr;
    void* m_framebuffer = nullptr;
#endif
};

} // namespace fuse::renderer
