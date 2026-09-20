#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

enum class CommandRecordKind : u8 {
    BeginPass = 1,
    EndPass = 2,
    PipelineBarrier = 3,
    ClearColor = 4,
    DrawIndexed = 5,
    Draw = 6,
    Present = 7,
    Composite = 8,
    BufferBarrier = 9,
};

struct CommandRecord {
    CommandRecordKind kind = CommandRecordKind::BeginPass;
    const char* passName = nullptr;
    float clearR = 0.f;
    float clearG = 0.f;
    float clearB = 0.f;
    float compositeBlend = 0.f;
    u32 textureId = 0;
    u32 fromLayout = 0;
    u32 toLayout = 0;
    u32 bufferId = 0;
    u32 fromAccess = 0;
    u32 toAccess = 0;
    u32 drawCount = 0;
};

/// Offscreen raster + optional swapchain present targets for real `vkCmd*` encoding (B2.5 / B2.8).
struct VkFrameEncodeContext {
    void* renderPass = nullptr;
    void* framebuffer = nullptr;
    void* graphicsPipeline = nullptr;
    void* graphicsPipelineLayout = nullptr;
    void* vertexBuffer = nullptr;
    /// Backbuffer image for graph-planned `vkCmdPipelineBarrier` (offscreen color target).
    void* barrierImage = nullptr;
    /// Buffer for graph-planned `vkCmdPipelineBarrier` (`VkBufferMemoryBarrier`).
    void* barrierBuffer = nullptr;
    u32 width = 0;
    u32 height = 0;
    bool active = false;

    /// Swapchain present pass — used when WSI acquire yields a valid image index.
    void* presentRenderPass = nullptr;
    void* presentFramebuffer = nullptr;
    void* presentBarrierImage = nullptr;
    u32 presentWidth = 0;
    u32 presentHeight = 0;
    bool presentActive = false;

    /// Composite GPU blit — bindless fullscreen pass into backbuffer (WP-06f).
    void* compositeRenderPass = nullptr;
    void* compositeFramebuffer = nullptr;
    void* compositePipeline = nullptr;
    void* compositePipelineLayout = nullptr;
    void* compositeVertexBuffer = nullptr;
    void* bindlessDescriptorSet = nullptr;
    u32 rasterTextureBindlessIndex = 0;
    u32 cudaTextureBindlessIndex = UINT32_MAX;
    u32 compositeWidth = 0;
    u32 compositeHeight = 0;
    float compositeBlend = 0.5f;
    bool compositeActive = false;
    bool compositeTargetsSwapchain = false;
};

/// Command-buffer recorder — logical commands for tests; optional real `vkCmd*` when backend active.
class CommandBufferRecorder {
public:
    void reset();
    void setVulkanEncodeContext(const VkFrameEncodeContext* context);
    bool beginRecording(void* nativeCommandBuffer);
    bool endRecording();

    void beginPass(const char* name);
    void endPass();
    void pipelineBarrier(u32 textureId, u32 fromLayout, u32 toLayout);
    void bufferBarrier(u32 bufferId, u32 fromAccess, u32 toAccess);
    void clearColor(float r, float g, float b);
    void draw(u32 instanceCount);
    void drawIndexed(u32 indexCount);
    void composite(float blend);
    void present();

    bool isRecording() const { return m_recording; }
    void* nativeHandle() const { return m_nativeCommandBuffer; }
    const std::vector<CommandRecord>& records() const { return m_records; }
    u32 recordCount() const { return static_cast<u32>(m_records.size()); }
    bool vulkanEncodeActive() const { return m_vulkanEncodeActive; }
    bool vulkanRecordingComplete() const { return m_vulkanRecordingComplete; }
    u32 vulkanRenderPassBeginCount() const { return m_vulkanRenderPassBeginCount; }
    u32 vulkanPipelineBarrierCount() const { return m_vulkanPipelineBarrierCount; }
    u32 vulkanBufferBarrierCount() const { return m_vulkanBufferBarrierCount; }
    u32 vulkanPresentRenderPassBeginCount() const { return m_vulkanPresentRenderPassBeginCount; }
    u32 vulkanCompositeDrawCount() const { return m_vulkanCompositeDrawCount; }

private:
    void push(CommandRecordKind kind);
    bool shouldEncodeRasterPass(const char* passName) const;
    void beginVulkanRenderPass();
    void endVulkanRenderPass();
    void encodeVulkanPipelineBarrier(u32 fromLayout, u32 toLayout);
    void encodeVulkanBufferBarrier(u32 fromAccess, u32 toAccess);
    void encodePresentSwapchainPass();
    void encodeCompositePass(float blend);
    void encodeDraw(u32 instanceCount);

    const VkFrameEncodeContext* m_encodeContext = nullptr;
    void* m_nativeCommandBuffer = nullptr;
    bool m_recording = false;
    bool m_vulkanEncodeActive = false;
    bool m_vulkanRecordingComplete = false;
    bool m_insideRenderPass = false;
    bool m_activeRasterPass = false;
    float m_pendingClearR = 0.f;
    float m_pendingClearG = 0.f;
    float m_pendingClearB = 0.f;
    u32 m_vulkanRenderPassBeginCount = 0;
    u32 m_vulkanPipelineBarrierCount = 0;
    u32 m_vulkanBufferBarrierCount = 0;
    u32 m_vulkanPresentRenderPassBeginCount = 0;
    u32 m_vulkanCompositeDrawCount = 0;
    std::vector<CommandRecord> m_records;
};

} // namespace fuse::renderer
