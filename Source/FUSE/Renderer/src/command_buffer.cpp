#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/render_graph.hpp>

#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

bool isRealVulkanCommandBuffer(void* nativeCommandBuffer) {
    return nativeCommandBuffer != nullptr && nativeCommandBuffer != reinterpret_cast<void*>(0x1);
}

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
    m_vulkanRenderPassBeginCount = 0;
    m_vulkanPipelineBarrierCount = 0;
    m_vulkanPresentRenderPassBeginCount = 0;
    m_vulkanCompositeDrawCount = 0;
    m_records.clear();
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
    return std::strcmp(passName, "clear3d") == 0 || std::strcmp(passName, "sprites2d") == 0;
}

void CommandBufferRecorder::encodeVulkanPipelineBarrier(u32 fromLayout, u32 toLayout) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    void* barrierImage = m_encodeContext->barrierImage;
    if (barrierImage == nullptr && m_encodeContext->presentBarrierImage != nullptr) {
        barrierImage = m_encodeContext->presentBarrierImage;
    }
    if (barrierImage == nullptr) {
        return;
    }

    const auto from = static_cast<RGImageLayout>(fromLayout);
    const auto to = static_cast<RGImageLayout>(toLayout);
    if (from == RGImageLayout::Undefined && to == RGImageLayout::Undefined) {
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

    auto layoutStageAccess = [](RGImageLayout layout, VkPipelineStageFlags& stage, VkAccessFlags& access) {
        stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        access = 0;
        switch (layout) {
        case RGImageLayout::ColorAttachment:
            stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case RGImageLayout::ShaderReadOnly:
            stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            access = VK_ACCESS_SHADER_READ_BIT;
            break;
        case RGImageLayout::TransferSrc:
            stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            access = VK_ACCESS_TRANSFER_READ_BIT;
            break;
        case RGImageLayout::TransferDst:
            stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            access = VK_ACCESS_TRANSFER_WRITE_BIT;
            break;
        case RGImageLayout::PresentSrc:
            stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            access = 0;
            break;
        case RGImageLayout::General:
            stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            break;
        default:
            break;
        }
    };

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags srcAccess = 0;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkAccessFlags dstAccess = 0;
    if (from != RGImageLayout::Undefined) {
        layoutStageAccess(from, srcStage, srcAccess);
    }
    if (to != RGImageLayout::Undefined) {
        layoutStageAccess(to, dstStage, dstAccess);
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = toVkLayout(from);
    barrier.newLayout = toVkLayout(to);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = static_cast<VkImage>(barrierImage);
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    ++m_vulkanPipelineBarrierCount;
#else
    (void)fromLayout;
    (void)toLayout;
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

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(commandBuffer);
    ++m_vulkanPresentRenderPassBeginCount;
#else
    (void)0;
#endif
}

void CommandBufferRecorder::beginVulkanRenderPass() {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_vulkanEncodeActive || m_encodeContext == nullptr || m_insideRenderPass ||
        !isRealVulkanCommandBuffer(m_nativeCommandBuffer)) {
        return;
    }

    auto commandBuffer = static_cast<VkCommandBuffer>(m_nativeCommandBuffer);
    VkClearValue clearValue{};
    clearValue.color = {{m_pendingClearR, m_pendingClearG, m_pendingClearB, 1.f}};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = static_cast<VkRenderPass>(m_encodeContext->renderPass);
    renderPassInfo.framebuffer = static_cast<VkFramebuffer>(m_encodeContext->framebuffer);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_encodeContext->width, m_encodeContext->height};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      static_cast<VkPipeline>(m_encodeContext->graphicsPipeline));

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_encodeContext->width);
    viewport.height = static_cast<float>(m_encodeContext->height);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {m_encodeContext->width, m_encodeContext->height};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

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
    VkBuffer vertexBuffers[] = {static_cast<VkBuffer>(m_encodeContext->vertexBuffer)};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

    const u32 instances = instanceCount > 0u ? instanceCount : 1u;
    vkCmdDraw(commandBuffer, 3, instances, 0, 0);
#else
    (void)instanceCount;
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
    if (!m_recording) {
        return;
    }

    CommandRecord record;
    record.kind = CommandRecordKind::DrawIndexed;
    record.drawCount = indexCount;
    m_records.push_back(record);
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

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = static_cast<VkRenderPass>(m_encodeContext->compositeRenderPass);
    renderPassInfo.framebuffer = static_cast<VkFramebuffer>(m_encodeContext->compositeFramebuffer);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_encodeContext->compositeWidth, m_encodeContext->compositeHeight};

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      static_cast<VkPipeline>(m_encodeContext->compositePipeline));

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
    pushConstants.cudaTexIndex = 0u;
    vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(pushConstants), &pushConstants);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_encodeContext->compositeWidth);
    viewport.height = static_cast<float>(m_encodeContext->compositeHeight);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {m_encodeContext->compositeWidth, m_encodeContext->compositeHeight};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    VkBuffer vertexBuffers[] = {static_cast<VkBuffer>(m_encodeContext->compositeVertexBuffer)};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);
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
