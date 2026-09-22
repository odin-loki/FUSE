#pragma once

#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer::cuda {
struct FrameSyncPair;
}

namespace fuse::renderer {

struct CompositeGpuPathDesc {
    u32 width = 320;
    u32 height = 240;
    const char* vertexSpirvPath = nullptr;
    const char* fragmentSpirvPath = nullptr;
};

struct CompositeGpuPathStats {
    bool pipelineReady = false;
    bool bindlessBound = false;
    bool cudaTextureActive = false;
    bool cudaTexturePlaceholder = true;
    bool cudaFillAttempted = false;
    bool cudaFillOk = false;
    bool cudaFillStubPath = false;
    u32 framesEncoded = 0;
    u32 rasterTextureIndex = UINT32_MAX;
    u32 cudaTextureIndex = UINT32_MAX;
    u32 depthTextureIndex = UINT32_MAX;
    bool depthTextureBound = false;
    u64 lastFrameSyncIndex = 0;
    std::string message;
};

/// WP-06f — bindless composite shader blit into swapchain backbuffer (headless-honest offscreen when no WSI).
class CompositeGpuPath {
public:
    static std::unique_ptr<CompositeGpuPath> create(VulkanDevice& device, const CompositeGpuPathDesc& desc);
    ~CompositeGpuPath();

    CompositeGpuPath(const CompositeGpuPath&) = delete;
    CompositeGpuPath& operator=(const CompositeGpuPath&) = delete;

    bool isReady() const { return m_stats.pipelineReady; }
    const CompositeGpuPathStats& lastStats() const { return m_stats; }
    const BindlessDescriptors& bindless() const { return m_bindless; }

    /// Registers the raster color view for bindless sampling; call after RasterPath is ready.
    bool registerRasterSource(void* imageView);

    /// Registers the raster depth view into bindless. Stats-only until the composite shader samples it.
    bool registerRasterDepth(void* imageView);

    /// Registers a CUDA-interop Vulkan image view; when unavailable composite keeps placeholder colour.
    bool registerCudaSource(void* imageView);

    /// Attempts to allocate/export a CUDA-writable image when interop is available.
    bool ensureCudaInteropTexture();

    /// Fills the interop texture via CUDA kernel (honest stub when toolkit/interop absent).
    bool fillCudaInteropTexture(fuse::renderer::cuda::FrameSyncPair* frameSync, u64 frameIndex,
                                bool useJobLane = false);

    /// Ensures a present-target pipeline exists when swapchain present render pass is available.
    bool ensurePresentPipeline(void* presentRenderPass);

    /// Fills composite fields on `VkFrameEncodeContext` for command-buffer encoding.
    void fillEncodeContext(VkFrameEncodeContext& context, float blend, bool presentActive);

private:
    CompositeGpuPath() = default;
    bool initialize(VulkanDevice& device, const CompositeGpuPathDesc& desc);
    void shutdown();

    VulkanDevice* m_device = nullptr;
    CompositeGpuPathDesc m_desc{};
    CompositeGpuPathStats m_stats{};
    BindlessDescriptors m_bindless;

    std::unique_ptr<class RenderPass> m_offscreenRenderPass;
    std::unique_ptr<class PipelineLayout> m_pipelineLayout;
    std::unique_ptr<class ShaderModule> m_vertexShader;
    std::unique_ptr<class ShaderModule> m_fragmentShader;
    std::unique_ptr<class GraphicsPipeline> m_offscreenPipeline;
    std::unique_ptr<class GraphicsPipeline> m_presentPipeline;
    std::unique_ptr<class PipelineCache> m_pipelineCache;

#if defined(FUSE_VULKAN_BACKEND)
    void* m_vertexBuffer = nullptr;
    void* m_vertexMemory = nullptr;
    void* m_sampler = nullptr;
    BindlessSlotHandle m_samplerSlot{};
    BindlessSlotHandle m_rasterTextureSlot{};
    BindlessSlotHandle m_depthTextureSlot{};
    BindlessSlotHandle m_cudaTextureSlot{};
    void* m_cudaImage = nullptr;
    void* m_cudaImageMemory = nullptr;
    void* m_cudaImageView = nullptr;
    void* m_cudaExportedHandle = nullptr;
    u64 m_cudaAllocationSize = 0;
    u32 m_cudaImageLayout = 0; // VK_IMAGE_LAYOUT_UNDEFINED until composite first samples it
    /// Offscreen composite target (compatible with m_offscreenRenderPass). Composite never
    /// renders into the raster framebuffer it samples from.
    void* m_outputImage = nullptr;
    void* m_outputMemory = nullptr;
    void* m_outputView = nullptr;
    void* m_outputFramebuffer = nullptr;
#endif
};

} // namespace fuse::renderer
