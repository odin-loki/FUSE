#pragma once

#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

class RenderPass;
class PipelineLayout;
class ShaderModule;
class GraphicsPipeline;

/// Per-draw surface data, pushed as a 64-byte vertex|fragment push-constant block. Mirrors
/// `GBufferPush` in shaders/raster/gbuffer.{vert,frag}.
struct GBufferDrawPush {
    f32 albedo[4] = {1.f, 1.f, 1.f, 1.f};   // rgb albedo
    f32 normal[4] = {0.f, 0.f, 1.f, 0.f};   // xyz normal
    f32 surface[4] = {0.5f, 0.f, 0.5f, 1.f}; // roughness, metallic, NDC depth, ambient occlusion
    f32 emissive[4] = {0.f, 0.f, 0.f, 0.f}; // xyz emissive radiance, w shading model
};
static_assert(sizeof(GBufferDrawPush) == 64, "GBufferDrawPush must match the shader push block");

struct GBufferDraw {
    u32 vertexCount = 0;
    u32 firstVertex = 0;
    GBufferDrawPush push{};
};

struct GBufferRasterPassDesc {
    const char* vertexSpirvPath = nullptr;
    const char* fragmentSpirvPath = nullptr;
    /// Bytes per vertex (position = 3 x f32 at offset 0).
    u32 vertexStrideBytes = 12;
};

struct GBufferRasterPassStats {
    u32 passesRecorded = 0;
    u32 drawsRecorded = 0;
    u32 pushConstantUpdates = 0;
    std::string message;
};

/// B5.2 raster G-buffer pass: one MRT render pass writing all six `GBufferAttachment` targets
/// (RT0..RT5, formats from `GBufferLayout`) plus a D32 depth buffer for the depth test. Draws
/// carry their surface data in push constants; fragments are packed by `write_gbuffer`
/// (shaders/common/gbuffer.glsl), the GPU twin of `GBufferPacking`.
class GBufferRasterPass {
public:
    GBufferRasterPass();
    ~GBufferRasterPass();

    GBufferRasterPass(const GBufferRasterPass&) = delete;
    GBufferRasterPass& operator=(const GBufferRasterPass&) = delete;

    /// `gbuffer` must be initialised on `resources`. Creates the render pass, pipeline, depth
    /// target and framebuffer. Returns false in the stub backend or on any Vulkan failure.
    bool init(VulkanDevice& device, ResourceManager& resources, GBuffer& gbuffer,
              const GBufferRasterPassDesc& desc);
    void destroy();

    bool isReady() const { return m_ready; }
    const GBufferRasterPassStats& stats() const { return m_stats; }
    TextureHandle depthTarget() const { return m_depth; }

    /// Record the pass into `commandBuffer` (a VkCommandBuffer in the recording state): clears
    /// every attachment (colour 0, depth 1), draws each entry with its push constants from
    /// `vertexBuffer` (VkBuffer), and reports the attachments' final layouts to ResourceManager.
    bool record(void* commandBuffer, void* vertexBuffer, const GBufferDraw* draws, u32 drawCount);

private:
    VulkanDevice* m_device = nullptr;
    ResourceManager* m_resources = nullptr;
    GBuffer* m_gbuffer = nullptr;
    TextureHandle m_depth{};
    u32 m_width = 0;
    u32 m_height = 0;
    std::unique_ptr<RenderPass> m_renderPass;
    std::unique_ptr<PipelineLayout> m_layout;
    std::unique_ptr<ShaderModule> m_vertexShader;
    std::unique_ptr<ShaderModule> m_fragmentShader;
    std::unique_ptr<GraphicsPipeline> m_pipeline;
    void* m_framebuffer = nullptr;
    bool m_ready = false;
    GBufferRasterPassStats m_stats;
};

} // namespace fuse::renderer
