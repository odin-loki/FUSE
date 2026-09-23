#include <fuse/renderer/deferred/gbuffer_raster_pass.hpp>

#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/debug_utils.hpp>
#include <fuse/renderer/vk/graphics_pipeline.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/render_pass.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

constexpr u32 kShaderStageVertex = 0x1u;
constexpr u32 kShaderStageFragment = 0x10u;
constexpr u32 kColorCount = static_cast<u32>(GBufferAttachment::Count);
static_assert(kColorCount <= RenderPassDesc::kMaxColorAttachments, "G-buffer exceeds MRT limit");

} // namespace

GBufferRasterPass::GBufferRasterPass() = default;

GBufferRasterPass::~GBufferRasterPass() {
    destroy();
}

bool GBufferRasterPass::init(VulkanDevice& device, ResourceManager& resources, GBuffer& gbuffer,
                             const GBufferRasterPassDesc& desc) {
    destroy();
    m_device = &device;
    m_resources = &resources;
    m_gbuffer = &gbuffer;

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid() || !gbuffer.isReady() || desc.vertexSpirvPath == nullptr ||
        desc.fragmentSpirvPath == nullptr) {
        m_stats.message = "G-buffer raster pass needs a device, a ready GBuffer and shader paths";
        return false;
    }
    m_width = gbuffer.desc().width;
    m_height = gbuffer.desc().height;

    RenderPassDesc passDesc{};
    passDesc.colorAttachmentCount = kColorCount;
    passDesc.colorFormat = static_cast<u32>(GBufferLayout::format(static_cast<GBufferAttachment>(0)));
    for (u32 i = 1; i < kColorCount; ++i) {
        passDesc.additionalColorFormats[i - 1u] =
            static_cast<u32>(GBufferLayout::format(static_cast<GBufferAttachment>(i)));
    }
    passDesc.depthFormat = static_cast<u32>(GpuFormat::D32Sfloat);
    passDesc.debugName = "fuse.gbuffer.render_pass";
    m_renderPass = RenderPass::create(device, passDesc);
    if (m_renderPass == nullptr || !m_renderPass->isValid()) {
        m_stats.message = "G-buffer MRT render pass creation failed";
        return false;
    }

    PipelineLayoutDesc layoutDesc{};
    layoutDesc.pushConstants.push_back(
        {0, static_cast<u32>(sizeof(GBufferDrawPush)), kShaderStageVertex | kShaderStageFragment});
    layoutDesc.debugName = "fuse.gbuffer.layout";
    m_layout = PipelineLayout::create(device, layoutDesc);
    m_vertexShader = ShaderModule::createFromFile(device, ShaderStage::Vertex, desc.vertexSpirvPath);
    m_fragmentShader = ShaderModule::createFromFile(device, ShaderStage::Fragment, desc.fragmentSpirvPath);
    if (m_layout == nullptr || !m_layout->isValid() || m_vertexShader == nullptr || !m_vertexShader->isValid() ||
        m_fragmentShader == nullptr || !m_fragmentShader->isValid()) {
        m_stats.message = "G-buffer layout / shader modules failed";
        return false;
    }

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.layout = m_layout.get();
    pipelineDesc.vertexShader = m_vertexShader.get();
    pipelineDesc.fragmentShader = m_fragmentShader.get();
    pipelineDesc.renderPass = m_renderPass.get();
    pipelineDesc.colorFormat = passDesc.colorFormat;
    pipelineDesc.colorAttachmentCount = kColorCount;
    pipelineDesc.depthFormat = passDesc.depthFormat;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompareOp = 1; // VK_COMPARE_OP_LESS
    pipelineDesc.vertexStrideBytes = desc.vertexStrideBytes;
    pipelineDesc.debugName = "fuse.gbuffer.pipeline";
    m_pipeline = GraphicsPipeline::create(device, pipelineDesc);
    if (m_pipeline == nullptr || !m_pipeline->isValid()) {
        m_stats.message = m_pipeline != nullptr ? m_pipeline->info().message : "G-buffer pipeline failed";
        return false;
    }

    TextureDesc depthDesc{};
    depthDesc.width = m_width;
    depthDesc.height = m_height;
    depthDesc.format = GpuFormat::D32Sfloat;
    depthDesc.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::DepthStencilAttachment) |
                                              static_cast<u32>(ImageUsage::Sampled));
    depthDesc.name = "fuse.gbuffer.depth_stencil";
    m_depth = resources.createTexture(depthDesc);
    const Texture* depth = resources.getTexture(m_depth);
    if (depth == nullptr || depth->view == nullptr) {
        m_stats.message = "G-buffer depth target creation failed";
        return false;
    }

    VkImageView views[kColorCount + 1u]{};
    for (u32 i = 0; i < kColorCount; ++i) {
        const Texture* target = resources.getTexture(gbuffer.targets().attachments[i]);
        if (target == nullptr || target->view == nullptr) {
            m_stats.message = "G-buffer attachment view missing";
            return false;
        }
        views[i] = static_cast<VkImageView>(target->view);
    }
    views[kColorCount] = static_cast<VkImageView>(depth->view);

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = static_cast<VkRenderPass>(m_renderPass->nativeHandle());
    framebufferInfo.attachmentCount = kColorCount + 1u;
    framebufferInfo.pAttachments = views;
    framebufferInfo.width = m_width;
    framebufferInfo.height = m_height;
    framebufferInfo.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(static_cast<VkDevice>(device.nativeHandle()), &framebufferInfo, nullptr,
                            &framebuffer) != VK_SUCCESS) {
        m_stats.message = "G-buffer framebuffer creation failed";
        return false;
    }
    m_framebuffer = framebuffer;
    nameVkObject(device.nativeHandle(), vk_object_type::kFramebuffer, m_framebuffer,
                       "fuse.gbuffer.framebuffer");

    m_ready = true;
    m_stats.message = "G-buffer raster pass ready (6 MRT + D32)";
    return true;
#else
    (void)desc;
    m_stats.message = "G-buffer raster pass unavailable — Vulkan backend disabled";
    return false;
#endif
}

void GBufferRasterPass::destroy() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_framebuffer != nullptr && m_device != nullptr && m_device->isValid()) {
        vkDestroyFramebuffer(static_cast<VkDevice>(m_device->nativeHandle()),
                             static_cast<VkFramebuffer>(m_framebuffer), nullptr);
    }
#endif
    m_framebuffer = nullptr;
    if (m_resources != nullptr && m_depth.isValid()) {
        m_resources->destroyTexture(m_depth);
    }
    m_depth = TextureHandle{};
    m_pipeline.reset();
    m_fragmentShader.reset();
    m_vertexShader.reset();
    m_layout.reset();
    m_renderPass.reset();
    m_ready = false;
}

bool GBufferRasterPass::record(void* commandBuffer, void* vertexBuffer, const GBufferDraw* draws,
                               u32 drawCount) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_ready || commandBuffer == nullptr || (drawCount > 0u && (draws == nullptr || vertexBuffer == nullptr))) {
        return false;
    }

    auto cmd = static_cast<VkCommandBuffer>(commandBuffer);
    VkClearValue clears[kColorCount + 1u]{};
    clears[kColorCount].depthStencil = {1.f, 0};

    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = static_cast<VkRenderPass>(m_renderPass->nativeHandle());
    beginInfo.framebuffer = static_cast<VkFramebuffer>(m_framebuffer);
    beginInfo.renderArea.extent = {m_width, m_height};
    beginInfo.clearValueCount = kColorCount + 1u;
    beginInfo.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VkPipeline>(m_pipeline->nativeHandle()));
    if (drawCount > 0u) {
        const VkBuffer buffers[] = {static_cast<VkBuffer>(vertexBuffer)};
        const VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    }
    const auto layout = static_cast<VkPipelineLayout>(m_layout->nativeHandle());
    for (u32 i = 0; i < drawCount; ++i) {
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(GBufferDrawPush), &draws[i].push);
        vkCmdDraw(cmd, draws[i].vertexCount, 1, draws[i].firstVertex, 0);
        ++m_stats.pushConstantUpdates;
        ++m_stats.drawsRecorded;
    }
    vkCmdEndRenderPass(cmd);

    // Render pass final layouts (RenderPass::create): colour -> COLOR_ATTACHMENT, depth -> DEPTH.
    for (u32 i = 0; i < kColorCount; ++i) {
        m_resources->setTextureLayout(m_gbuffer->targets().attachments[i],
                                      static_cast<u32>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
    }
    m_resources->setTextureLayout(m_depth, static_cast<u32>(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL));
    ++m_stats.passesRecorded;
    return true;
#else
    (void)commandBuffer;
    (void)vertexBuffer;
    (void)draws;
    (void)drawCount;
    return false;
#endif
}

} // namespace fuse::renderer
