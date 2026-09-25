#include <fuse/renderer/vk/render_pass.hpp>

#include <fuse/renderer/vk/debug_utils.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

std::unique_ptr<RenderPass> RenderPass::create(VulkanDevice& device, const RenderPassDesc& desc) {
    auto pass = std::unique_ptr<RenderPass>(new RenderPass());
    if (!pass->initialize(device, desc)) {
        pass->m_info.valid = false;
    }
    return pass;
}

RenderPass::~RenderPass() {
    shutdown();
}

void* RenderPass::nativeHandle() const {
    return m_handle;
}

bool RenderPass::initialize(VulkanDevice& device, const RenderPassDesc& desc) {
    m_device = &device;
    m_info.colorFormat = desc.colorFormat;
    m_info.depthFormat = desc.depthFormat;
    m_info.colorAttachmentCount = desc.colorAttachmentCount;
    for (u32 i = 0; i < RenderPassDesc::kMaxColorAttachments && i < desc.colorAttachmentCount; ++i) {
        m_colorFormats[i] = i == 0u ? desc.colorFormat : desc.additionalColorFormats[i - 1u];
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "Vulkan device unavailable";
        return false;
    }

    const bool withDepth = desc.hasDepth();
    const u32 colorCount = desc.colorAttachmentCount;
    if (colorCount == 0u || colorCount > RenderPassDesc::kMaxColorAttachments) {
        m_info.message = "render pass colour attachment count out of range";
        return false;
    }

    VkAttachmentDescription attachments[RenderPassDesc::kMaxColorAttachments + 1]{};
    VkAttachmentReference colorRefs[RenderPassDesc::kMaxColorAttachments]{};
    for (u32 i = 0; i < colorCount; ++i) {
        const u32 format = i == 0u ? desc.colorFormat : desc.additionalColorFormats[i - 1u];
        if (format == 0u) {
            m_info.message = "render pass colour attachment format missing";
            return false;
        }
        attachments[i].format = static_cast<VkFormat>(format);
        attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp = desc.clearOnLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorRefs[i].attachment = i;
        colorRefs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkAttachmentReference depthRef{};
    if (withDepth) {
        VkAttachmentDescription& depth = attachments[colorCount];
        depth.format = static_cast<VkFormat>(desc.depthFormat);
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = desc.clearOnLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        depthRef.attachment = colorCount;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = colorCount;
    subpass.pColorAttachments = colorRefs;
    subpass.pDepthStencilAttachment = withDepth ? &depthRef : nullptr;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    // The previous pass's attachment writes must be available before this pass's UNDEFINED ->
    // attachment layout transition (a write) — srcAccessMask 0 is a write-after-write hazard.
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (withDepth) {
        dependency.srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkRenderPassCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = colorCount + (withDepth ? 1u : 0u);
    createInfo.pAttachments = attachments;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies = &dependency;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateRenderPass(static_cast<VkDevice>(device.nativeHandle()), &createInfo, nullptr,
                           &renderPass);
    if (result != VK_SUCCESS) {
        m_info.message = "vkCreateRenderPass failed";
        return false;
    }

    m_handle = renderPass;
    m_info.valid = true;
    m_info.colorAttachmentCount = colorCount;
    nameVkObject(device.nativeHandle(), vk_object_type::kRenderPass, m_handle,
                       desc.debugName != nullptr ? desc.debugName : "fuse.render_pass");
    if (desc.debugName != nullptr) {
        m_info.message = desc.debugName;
    } else {
        m_info.message = withDepth ? "headless color+depth render pass" : "headless color render pass";
    }
    return true;
#else
    m_info.valid = true;
    m_info.message = "render pass placeholder (stub backend)";
    return true;
#endif
}

void RenderPass::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle != nullptr && m_device != nullptr && m_device->isValid()) {
        vkDestroyRenderPass(static_cast<VkDevice>(m_device->nativeHandle()),
                            static_cast<VkRenderPass>(m_handle), nullptr);
    }
#endif
    m_handle = nullptr;
    m_device = nullptr;
}

} // namespace fuse::renderer
