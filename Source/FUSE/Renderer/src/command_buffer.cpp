#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/render_graph.hpp>

#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

#if defined(FUSE_VULKAN_BACKEND)
bool isRealVulkanCommandBuffer(void* nativeCommandBuffer) {
    return nativeCommandBuffer != nullptr && nativeCommandBuffer != reinterpret_cast<void*>(0x1);
}

bool isDepthAttachmentLayout(RGImageLayout layout) {
    return layout == RGImageLayout::DepthAttachment;
}

void bindRasterBindlessDescriptorSets(VkCommandBuffer commandBuffer, const VkFrameEncodeContext& context) {
    if (context.bindlessDescriptorSet == nullptr || context.graphicsPipelineLayout == nullptr) {
        return;
    }

    VkDescriptorSet bindlessSet = static_cast<VkDescriptorSet>(context.bindlessDescriptorSet);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            static_cast<VkPipelineLayout>(context.graphicsPipelineLayout), 0, 1,
                            &bindlessSet, 0, nullptr);
}

void layoutStageAccessMask(VkImageLayout layout, VkPipelineStageFlags& stage, VkAccessFlags& access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        access = VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_GENERAL:
        stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        break;
    default: // UNDEFINED / PRESENT_SRC: no prior access to make available
        stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        access = 0;
        break;
    }
}

/// Transition `image` from its tracked layout (or `fallbackOld` when untracked) to `newLayout`,
/// then advance the tracker. Emitted even when old == new so it still orders memory access.
void transitionTrackedImage(VkCommandBuffer commandBuffer, void* image, u32* trackedLayout,
                            VkImageLayout fallbackOld, VkImageLayout newLayout, VkImageAspectFlags aspect) {
    const VkImageLayout oldLayout =
        trackedLayout != nullptr ? static_cast<VkImageLayout>(*trackedLayout) : fallbackOld;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkAccessFlags dstAccess = 0;
    layoutStageAccessMask(oldLayout, srcStage, srcAccess);
    if (newLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        layoutStageAccessMask(newLayout, dstStage, dstAccess);
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = static_cast<VkImage>(image);
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    if (trackedLayout != nullptr) {
        *trackedLayout = static_cast<u32>(newLayout);
    }
}

void setTrackedLayout(u32* trackedLayout, VkImageLayout layout) {
    if (trackedLayout != nullptr) {
        *trackedLayout = static_cast<u32>(layout);
    }
}
#endif

} // namespace

void CommandBufferRecorder::reset() {
    m_encodeContext = nullptr;
    m_nativeCommandBuffer = nullptr;
    m_recording = false;
    m_vulkanEncodeActive = false;
    m_vulkanRecordingComplete = false;
    m_insideRenderPass = false;
    m_activeRasterPass = false;
    m_pendingClearR = 0.f;
    m_pendingClearG = 0.f;
    m_pendingClearB = 0.f;
    m_pendingClearDepth = 1.f;
    m_vulkanRenderPassBeginCount = 0;
    m_vulkanViewportCount = 0;
    m_vulkanScissorCount = 0;
    m_vulkanPipelineBarrierCount = 0;
    m_vulkanBufferBarrierCount = 0;
    m_vulkanPresentRenderPassBeginCount = 0;
    m_vulkanCompositeDrawCount = 0;
    m_vulkanDrawIndexedCount = 0;
    m_vulkanDrawIndexedIndirectCount = 0;
    m_vulkanDispatchCount = 0;
    m_vulkanFillBufferCount = 0;
    m_vulkanUpdateBufferCount = 0;
    m_vulkanCopyBufferCount = 0;
    m_vulkanPipelineBindCount = 0;
    m_vulkanVertexBufferBindCount = 0;
    m_vulkanIndexBufferBindCount = 0;
    m_vulkanPushConstantCount = 0;
    invalidateBindState();
    m_records.clear();
}

void CommandBufferRecorder::invalidateBindState() {
    m_boundVertexBuffer = nullptr;
    m_boundIndexBuffer = nullptr;
    m_boundIndexType = UINT32_MAX;
    m_boundMaterialId = UINT32_MAX;
}

void CommandBufferRecorder::setVulkanEncodeContext(const VkFrameEncodeContext* context) {
    m_encodeContext = context;
}

bool CommandBufferRecorder::beginRecording(void* nativeCommandBuffer) {
    if (m_recording) {
        return false;
    }

    m_nativeCommandBuffer = nativeCommandBuffer;
    m_recording = true;
    m_vulkanEncodeActive = false;
    m_vulkanRecordingComplete = false;
    m_insideRenderPass = false;
    m_activeRasterPass = false;
    invalidateBindState();

#if defined(FUSE_VULKAN_BACKEND)
    if (m_encodeContext != nullptr && m_encodeContext->active && isRealVulkanCommandBuffer(nativeCommandBuffer)) {
        auto commandBuffer = static_cast<VkCommandBuffer>(nativeCommandBuffer);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            m_recording = false;
            m_nativeCommandBuffer = nullptr;
            return false;
        }
        m_vulkanEncodeActive = true;
    }
#else
    (void)nativeCommandBuffer;
#endif

    return true;
}

bool CommandBufferRecorder::endRecording() {
    if (!m_recording) {
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (m_vulkanEncodeActive && m_insideRenderPass) {
        endVulkanRenderPass();
    }

    if (m_vulkanEncodeActive && isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            m_recording = false;
            return false;
        }
        m_vulkanRecordingComplete = true;
    }
#endif

    m_recording = false;
    return true;
}

void CommandBufferRecorder::push(CommandRecordKind kind) {
    CommandRecord record;
    record.kind = kind;
    m_records.push_back(record);
}

bool CommandBufferRecorder::shouldEncodeRasterPass(const char* passName) const {
    if (passName == nullptr) {
        return false;
    }
    // "meshes" is the DrawList pass (populateRenderGraphFromDrawList); without it submitDrawList
    // recorded draws on the CPU only and never encoded them.
    return std::strcmp(passName, "clear3d") == 0 || std::strcmp(passName, "sprites2d") == 0 ||
           std::strcmp(passName, "meshes") == 0;
}

void CommandBufferRecorder::encodeVulkanPipelineBarrier(u32 fromLayout, u32 toLayout) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    const auto from = static_cast<RGImageLayout>(fromLayout);
    const auto to = static_cast<RGImageLayout>(toLayout);
    if (from == RGImageLayout::Undefined && to == RGImageLayout::Undefined) {
        return;
    }

    const bool depthTransition = isDepthAttachmentLayout(from) || isDepthAttachmentLayout(to);
    const bool presentTransition = from == RGImageLayout::PresentSrc || to == RGImageLayout::PresentSrc;

    // Present layouts only apply to the acquired swapchain image; headless frames have none.
    void* barrierImage = nullptr;
    u32* trackedLayout = nullptr;
    if (presentTransition) {
        barrierImage = m_encodeContext->presentBarrierImage;
        trackedLayout = m_encodeContext->presentImageLayout;
    } else if (depthTransition) {
        barrierImage = m_encodeContext->depthImage;
        trackedLayout = m_encodeContext->depthImageLayout;
    } else if (m_encodeContext->barrierImage != nullptr) {
        barrierImage = m_encodeContext->barrierImage;
        trackedLayout = m_encodeContext->barrierImageLayout;
    } else {
        barrierImage = m_encodeContext->presentBarrierImage;
        trackedLayout = m_encodeContext->presentImageLayout;
    }
    if (barrierImage == nullptr) {
        return;
    }

    auto toVkLayout = [](RGImageLayout layout) -> VkImageLayout {
        switch (layout) {
        case RGImageLayout::ColorAttachment:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case RGImageLayout::DepthAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case RGImageLayout::ShaderReadOnly:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case RGImageLayout::TransferSrc:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case RGImageLayout::TransferDst:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case RGImageLayout::PresentSrc:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case RGImageLayout::General:
            return VK_IMAGE_LAYOUT_GENERAL;
        default:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        }
    };

    const VkImageLayout newLayout = toVkLayout(to);
    if (newLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        return; // Cannot transition into UNDEFINED.
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    transitionTrackedImage(commandBuffer, barrierImage, trackedLayout, toVkLayout(from), newLayout,
                           depthTransition ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    ++m_vulkanPipelineBarrierCount;
#else
    (void)fromLayout;
    (void)toLayout;
#endif
}

void CommandBufferRecorder::encodeFillBuffer(u32 value) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    if (m_insideRenderPass) {
        return;
    }

    if (m_encodeContext->barrierBuffer == nullptr) {
        return;
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    vkCmdFillBuffer(commandBuffer, static_cast<VkBuffer>(m_encodeContext->barrierBuffer), 0, VK_WHOLE_SIZE,
                    value);
    ++m_vulkanFillBufferCount;
#else
    (void)value;
#endif
}

void CommandBufferRecorder::encodeVulkanBufferBarrier(u32 fromAccess, u32 toAccess) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    if (m_encodeContext->barrierBuffer == nullptr) {
        return;
    }

    auto accessStageMask = [](RGResourceAccess access, VkPipelineStageFlags& stage, VkAccessFlags& mask) {
        switch (access) {
        case RGResourceAccess::ShaderRead:
            stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            mask = VK_ACCESS_SHADER_READ_BIT;
            break;
        case RGResourceAccess::ShaderWrite:
            stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            mask = VK_ACCESS_SHADER_WRITE_BIT;
            break;
        case RGResourceAccess::TransferSrc:
            stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            mask = VK_ACCESS_TRANSFER_READ_BIT;
            break;
        case RGResourceAccess::TransferDst:
            stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            mask = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case RGResourceAccess::CUDAWrite:
        case RGResourceAccess::CUDARead:
            stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            break;
        default:
            break;
        }
    };

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkAccessFlags dstAccess = 0;
    accessStageMask(static_cast<RGResourceAccess>(fromAccess), srcStage, srcAccess);
    accessStageMask(static_cast<RGResourceAccess>(toAccess), dstStage, dstAccess);

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = static_cast<VkBuffer>(m_encodeContext->barrierBuffer);
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
    ++m_vulkanBufferBarrierCount;
#else
    (void)fromAccess;
    (void)toAccess;
#endif
}

void CommandBufferRecorder::encodePresentSwapchainPass() {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr || !m_encodeContext->presentActive ||
        m_encodeContext->presentRenderPass == nullptr || m_encodeContext->presentFramebuffer == nullptr ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    if (m_insideRenderPass) {
        endVulkanRenderPass();
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = static_cast<VkRenderPass>(m_encodeContext->presentRenderPass);
    renderPassInfo.framebuffer = static_cast<VkFramebuffer>(m_encodeContext->presentFramebuffer);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_encodeContext->presentWidth, m_encodeContext->presentHeight};
    VkClearValue presentClear{};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &presentClear;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    encodeVulkanViewportAndScissor(m_encodeContext->presentWidth, m_encodeContext->presentHeight);
    vkCmdEndRenderPass(commandBuffer);
    setTrackedLayout(m_encodeContext->presentImageLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    ++m_vulkanPresentRenderPassBeginCount;
#else
    (void)0;
#endif
}

void CommandBufferRecorder::encodeVulkanViewportAndScissor(u32 width, u32 height) {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_encodeContext == nullptr || !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    const u32 resolvedWidth = width > 0u ? width : m_encodeContext->width;
    const u32 resolvedHeight = height > 0u ? height : m_encodeContext->height;
    const u32 viewportWidth = resolvedWidth > 0u ? resolvedWidth : 1u;
    const u32 viewportHeight = resolvedHeight > 0u ? resolvedHeight : 1u;

    VkViewport viewport{};
    viewport.width = static_cast<float>(viewportWidth);
    viewport.height = static_cast<float>(viewportHeight);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    ++m_vulkanViewportCount;

    VkRect2D scissor{};
    scissor.extent = {viewportWidth, viewportHeight};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    ++m_vulkanScissorCount;
#else
    (void)width;
    (void)height;
#endif
}

void CommandBufferRecorder::beginVulkanRenderPass() {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr || m_insideRenderPass ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    VkClearValue clearValues[2]{};
    clearValues[0].color = {{m_pendingClearR, m_pendingClearG, m_pendingClearB, 1.f}};
    clearValues[1].depthStencil = {m_pendingClearDepth, 0};
    const bool hasDepth = m_encodeContext->depthImage != nullptr || m_encodeContext->depthView != nullptr;

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = static_cast<VkRenderPass>(m_encodeContext->renderPass);
    renderPassInfo.framebuffer = static_cast<VkFramebuffer>(m_encodeContext->framebuffer);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_encodeContext->width, m_encodeContext->height};
    renderPassInfo.clearValueCount = hasDepth ? 2u : 1u;
    renderPassInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    encodeVulkanViewportAndScissor();
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      static_cast<VkPipeline>(m_encodeContext->graphicsPipeline));
    ++m_vulkanPipelineBindCount;
    invalidateBindState();
    bindRasterBindlessDescriptorSets(commandBuffer, *m_encodeContext);

    m_insideRenderPass = true;
    ++m_vulkanRenderPassBeginCount;
#else
    (void)0;
#endif
}

void CommandBufferRecorder::endVulkanRenderPass() {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || !m_insideRenderPass || !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    vkCmdEndRenderPass(commandBuffer);
    m_insideRenderPass = false;
    // Raster render pass final layouts (RenderPass::create).
    if (m_encodeContext != nullptr) {
        setTrackedLayout(m_encodeContext->barrierImageLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        setTrackedLayout(m_encodeContext->depthImageLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
#else
    (void)0;
#endif
}

void CommandBufferRecorder::encodeDraw(u32 instanceCount) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr || !m_insideRenderPass ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    if (m_boundVertexBuffer != m_encodeContext->vertexBuffer) {
        VkBuffer vertexBuffers[] = {static_cast<VkBuffer>(m_encodeContext->vertexBuffer)};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        m_boundVertexBuffer = m_encodeContext->vertexBuffer;
        ++m_vulkanVertexBufferBindCount;
    }

    const u32 instances = instanceCount > 0u ? instanceCount : 1u;
    vkCmdDraw(commandBuffer, 3, instances, 0, 0);
#else
    (void)instanceCount;
#endif
}

void CommandBufferRecorder::encodeDrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                                             i32 vertexOffset, u32 materialId, void* vertexBuffer,
                                             void* indexBuffer) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr || !m_insideRenderPass ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    void* resolvedIndex = indexBuffer != nullptr ? indexBuffer : m_encodeContext->indexBuffer;
    void* resolvedVertex = vertexBuffer != nullptr ? vertexBuffer : m_encodeContext->vertexBuffer;
    if (resolvedIndex == nullptr) {
        return;
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    // Material-sorted draw lists share buffers and materials across runs of draws: only encode
    // the state that actually changes (B3 gate: no redundant state changes).
    const u32 indexType = m_encodeContext->indexType == 1u ? 1u : 0u;
    if (m_boundIndexBuffer != resolvedIndex || m_boundIndexType != indexType) {
        vkCmdBindIndexBuffer(commandBuffer, static_cast<VkBuffer>(resolvedIndex), 0,
                             indexType == 1u ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        m_boundIndexBuffer = resolvedIndex;
        m_boundIndexType = indexType;
        ++m_vulkanIndexBufferBindCount;
    }

    if (resolvedVertex != nullptr && m_boundVertexBuffer != resolvedVertex) {
        VkBuffer vertexBuffers[] = {static_cast<VkBuffer>(resolvedVertex)};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        m_boundVertexBuffer = resolvedVertex;
        ++m_vulkanVertexBufferBindCount;
    }

    if (m_encodeContext->graphicsPipelineLayout != nullptr && m_boundMaterialId != materialId) {
        const u32 payload[4] = {materialId, 0u, 0u, 0u};
        vkCmdPushConstants(commandBuffer,
                           static_cast<VkPipelineLayout>(m_encodeContext->graphicsPipelineLayout),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, payload);
        m_boundMaterialId = materialId;
        ++m_vulkanPushConstantCount;
    }

    const u32 instances = instanceCount > 0u ? instanceCount : 1u;
    vkCmdDrawIndexed(commandBuffer, indexCount, instances, firstIndex, vertexOffset, 0);
    ++m_vulkanDrawIndexedCount;
#else
    (void)indexCount;
    (void)instanceCount;
    (void)firstIndex;
    (void)vertexOffset;
    (void)materialId;
    (void)vertexBuffer;
    (void)indexBuffer;
#endif
}

void CommandBufferRecorder::encodeDrawIndexedIndirect(void* indirectBuffer, u32 offset, u32 drawCount,
                                                      u32 stride) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr || !m_insideRenderPass ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    void* resolvedIndirect =
        indirectBuffer != nullptr ? indirectBuffer : m_encodeContext->indirectBuffer;
    if (resolvedIndirect == nullptr) {
        return;
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    const u32 indexType = m_encodeContext->indexType == 1u ? 1u : 0u;
    if (m_encodeContext->indexBuffer != nullptr &&
        (m_boundIndexBuffer != m_encodeContext->indexBuffer || m_boundIndexType != indexType)) {
        vkCmdBindIndexBuffer(commandBuffer, static_cast<VkBuffer>(m_encodeContext->indexBuffer), 0,
                             indexType == 1u ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        m_boundIndexBuffer = m_encodeContext->indexBuffer;
        m_boundIndexType = indexType;
        ++m_vulkanIndexBufferBindCount;
    }

    if (m_encodeContext->vertexBuffer != nullptr && m_boundVertexBuffer != m_encodeContext->vertexBuffer) {
        VkBuffer vertexBuffers[] = {static_cast<VkBuffer>(m_encodeContext->vertexBuffer)};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        m_boundVertexBuffer = m_encodeContext->vertexBuffer;
        ++m_vulkanVertexBufferBindCount;
    }

    const u32 resolvedDrawCount = drawCount > 0u ? drawCount : 1u;
    const u32 resolvedStride = stride > 0u ? stride : 20u;
    vkCmdDrawIndexedIndirect(commandBuffer, static_cast<VkBuffer>(resolvedIndirect), offset,
                             resolvedDrawCount, resolvedStride);
    ++m_vulkanDrawIndexedIndirectCount;
#else
    (void)indirectBuffer;
    (void)offset;
    (void)drawCount;
    (void)stride;
#endif
}

void CommandBufferRecorder::encodeUpdateBuffer(void* dstBuffer, u32 data) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    if (m_insideRenderPass) {
        return;
    }

    void* resolvedDst = dstBuffer;
    if (resolvedDst == nullptr && m_encodeContext != nullptr) {
        resolvedDst = m_encodeContext->barrierBuffer;
    }
    if (resolvedDst == nullptr) {
        return;
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    vkCmdUpdateBuffer(commandBuffer, static_cast<VkBuffer>(resolvedDst), 0, sizeof(u32), &data);
    ++m_vulkanUpdateBufferCount;
#else
    (void)dstBuffer;
    (void)data;
#endif
}

void CommandBufferRecorder::encodeCopyBuffer(void* src, void* dst, u32 size) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    if (m_insideRenderPass) {
        return;
    }

    if (src == nullptr || dst == nullptr || size == 0u) {
        return;
    }

    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = size;

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    vkCmdCopyBuffer(commandBuffer, static_cast<VkBuffer>(src), static_cast<VkBuffer>(dst), 1, &region);
    ++m_vulkanCopyBufferCount;
#else
    (void)src;
    (void)dst;
    (void)size;
#endif
}

void CommandBufferRecorder::encodeDispatch(u32 x, u32 y, u32 z) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    if (m_encodeContext->computePipeline == nullptr) {
        return;
    }

    if (m_insideRenderPass) {
        endVulkanRenderPass();
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      static_cast<VkPipeline>(m_encodeContext->computePipeline));

    if (m_encodeContext->bindlessDescriptorSet != nullptr &&
        m_encodeContext->computePipelineLayout != nullptr) {
        VkDescriptorSet bindlessSet = static_cast<VkDescriptorSet>(m_encodeContext->bindlessDescriptorSet);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                static_cast<VkPipelineLayout>(m_encodeContext->computePipelineLayout), 0, 1,
                                &bindlessSet, 0, nullptr);
    }

    vkCmdDispatch(commandBuffer, x, y, z);
    ++m_vulkanDispatchCount;
#else
    (void)x;
    (void)y;
    (void)z;
#endif
}

void CommandBufferRecorder::beginPass(const char* name) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::BeginPass;
    record.passName = name;
    m_records.push_back(record);

    if (shouldEncodeRasterPass(name)) {
        m_activeRasterPass = true;
    }
}

void CommandBufferRecorder::endPass() {
    if (!m_recording) {
        return;
    }
    push(CommandRecordKind::EndPass);

    if (m_insideRenderPass) {
        endVulkanRenderPass();
    }
    m_activeRasterPass = false;
}

void CommandBufferRecorder::pipelineBarrier(u32 textureId, u32 fromLayout, u32 toLayout) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::PipelineBarrier;
    record.textureId = textureId;
    record.fromLayout = fromLayout;
    record.toLayout = toLayout;
    m_records.push_back(record);

    encodeVulkanPipelineBarrier(fromLayout, toLayout);
}

void CommandBufferRecorder::bufferBarrier(u32 bufferId, u32 fromAccess, u32 toAccess) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::BufferBarrier;
    record.bufferId = bufferId;
    record.fromAccess = fromAccess;
    record.toAccess = toAccess;
    m_records.push_back(record);

    encodeVulkanBufferBarrier(fromAccess, toAccess);
}

void CommandBufferRecorder::clearColor(float r, float g, float b) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::ClearColor;
    record.clearR = r;
    record.clearG = g;
    record.clearB = b;
    m_records.push_back(record);

    m_pendingClearR = r;
    m_pendingClearG = g;
    m_pendingClearB = b;

    if (m_activeRasterPass && !m_insideRenderPass) {
        beginVulkanRenderPass();
    }
}

void CommandBufferRecorder::clearDepth(float depth) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::ClearDepth;
    record.clearDepth = depth;
    m_records.push_back(record);

    m_pendingClearDepth = depth;
}

void CommandBufferRecorder::fillBuffer(u32 bufferId, u32 value) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::FillBuffer;
    record.bufferId = bufferId;
    record.fillValue = value;
    m_records.push_back(record);

    encodeFillBuffer(value);
}

void CommandBufferRecorder::draw(u32 instanceCount) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::Draw;
    record.drawCount = instanceCount;
    m_records.push_back(record);

    if (m_activeRasterPass && !m_insideRenderPass) {
        beginVulkanRenderPass();
    }
    encodeDraw(instanceCount);
}

void CommandBufferRecorder::drawIndexed(u32 indexCount) {
    drawIndexed(indexCount, 1u, 0u, 0, 0u);
}

void CommandBufferRecorder::drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset,
                                        u32 materialId, void* vertexBuffer, void* indexBuffer) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::DrawIndexed;
    record.drawCount = indexCount;
    record.indexCount = indexCount;
    record.instanceCount = instanceCount > 0u ? instanceCount : 1u;
    record.firstIndex = firstIndex;
    record.vertexOffset = vertexOffset;
    record.materialId = materialId;
    record.nativeVertexBuffer = vertexBuffer;
    record.nativeIndexBuffer = indexBuffer;
    m_records.push_back(record);

    if (m_activeRasterPass && !m_insideRenderPass) {
        beginVulkanRenderPass();
    }
    encodeDrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, materialId, vertexBuffer,
                      indexBuffer);
}

void CommandBufferRecorder::drawIndexedIndirect(void* indirectBuffer, u32 offset, u32 drawCount, u32 stride) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::DrawIndexedIndirect;
    record.nativeIndirectBuffer = indirectBuffer;
    record.bufferOffset = offset;
    record.drawCount = drawCount;
    record.stride = stride;
    m_records.push_back(record);

    if (m_activeRasterPass && !m_insideRenderPass) {
        beginVulkanRenderPass();
    }
    encodeDrawIndexedIndirect(indirectBuffer, offset, drawCount, stride);
}

void CommandBufferRecorder::updateBuffer(void* dstBuffer, u32 data) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::UpdateBuffer;
    record.nativeDstBuffer = dstBuffer;
    record.fillValue = data;
    m_records.push_back(record);

    encodeUpdateBuffer(dstBuffer, data);
}

void CommandBufferRecorder::copyBuffer(void* src, void* dst, u32 size) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::CopyBuffer;
    record.nativeSrcBuffer = src;
    record.nativeDstBuffer = dst;
    record.copySize = size;
    m_records.push_back(record);

    encodeCopyBuffer(src, dst, size);
}

void CommandBufferRecorder::dispatch(u32 x, u32 y, u32 z) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::Dispatch;
    record.dispatchX = x;
    record.dispatchY = y;
    record.dispatchZ = z;
    m_records.push_back(record);

    encodeDispatch(x, y, z);
}

void CommandBufferRecorder::encodeCompositePass(float blend) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr || !m_encodeContext->compositeActive ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    if (m_insideRenderPass) {
        endVulkanRenderPass();
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);

    // Composite samples raster color/depth and the CUDA interop image through the bindless heap,
    // whose descriptors are written with SHADER_READ_ONLY_OPTIMAL.
    const VkImageLayout sampled = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (m_encodeContext->barrierImage != nullptr) {
        transitionTrackedImage(commandBuffer, m_encodeContext->barrierImage, m_encodeContext->barrierImageLayout,
                               VK_IMAGE_LAYOUT_UNDEFINED, sampled, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (m_encodeContext->depthImage != nullptr && m_encodeContext->depthTextureBindlessIndex != UINT32_MAX) {
        transitionTrackedImage(commandBuffer, m_encodeContext->depthImage, m_encodeContext->depthImageLayout,
                               VK_IMAGE_LAYOUT_UNDEFINED, sampled, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    if (m_encodeContext->cudaImage != nullptr && m_encodeContext->cudaImageLayout != nullptr &&
        *m_encodeContext->cudaImageLayout != static_cast<u32>(sampled)) {
        transitionTrackedImage(commandBuffer, m_encodeContext->cudaImage, m_encodeContext->cudaImageLayout,
                               VK_IMAGE_LAYOUT_UNDEFINED, sampled, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = static_cast<VkRenderPass>(m_encodeContext->compositeRenderPass);
    renderPassInfo.framebuffer = static_cast<VkFramebuffer>(m_encodeContext->compositeFramebuffer);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_encodeContext->compositeWidth, m_encodeContext->compositeHeight};
    VkClearValue compositeClear{};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &compositeClear;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    encodeVulkanViewportAndScissor(m_encodeContext->compositeWidth, m_encodeContext->compositeHeight);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      static_cast<VkPipeline>(m_encodeContext->compositePipeline));
    ++m_vulkanPipelineBindCount;
    // Composite binds its own pipeline layout, push constants and vertex buffer.
    invalidateBindState();

    const VkPipelineLayout pipelineLayout =
        static_cast<VkPipelineLayout>(m_encodeContext->compositePipelineLayout);

    VkDescriptorSet bindlessSet = static_cast<VkDescriptorSet>(m_encodeContext->bindlessDescriptorSet);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                              &bindlessSet, 0, nullptr);

    struct CompositePushConstants {
        float blendFactor;
        u32 rasterTexIndex;
        u32 cudaTexIndex;
    } pushConstants{};
    pushConstants.blendFactor = blend;
    pushConstants.rasterTexIndex = m_encodeContext->rasterTextureBindlessIndex;
    pushConstants.cudaTexIndex = m_encodeContext->cudaTextureBindlessIndex;
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(pushConstants), &pushConstants);

    VkBuffer vertexBuffers[] = {static_cast<VkBuffer>(m_encodeContext->compositeVertexBuffer)};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);
    if (m_encodeContext->compositeTargetsSwapchain) {
        setTrackedLayout(m_encodeContext->presentImageLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        setTrackedLayout(m_encodeContext->compositeTargetLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    ++m_vulkanCompositeDrawCount;
#else
    (void)blend;
#endif
}

void CommandBufferRecorder::composite(float blend) {
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::Composite;
    record.compositeBlend = blend;
    m_records.push_back(record);

    encodeCompositePass(blend);
}

void CommandBufferRecorder::present() {
    if (!m_recording) {
        return;
    }
    push(CommandRecordKind::Present);
    encodePresentSwapchainPass();
}

} // namespace fuse::renderer
