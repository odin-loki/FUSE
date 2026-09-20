#include <fuse/renderer/vk/render_pass.hpp>

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

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "Vulkan device unavailable";
        return false;
    }

    const bool withDepth = desc.hasDepth();

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = static_cast<VkFormat>(desc.colorFormat);
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = desc.clearOnLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    if (withDepth) {
        attachments[1].format = static_cast<VkFormat>(desc.depthFormat);
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = desc.clearOnLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        depthRef.attachment = 1;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = withDepth ? &depthRef : nullptr;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (withDepth) {
        dependency.srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkRenderPassCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = withDepth ? 2u : 1u;
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
