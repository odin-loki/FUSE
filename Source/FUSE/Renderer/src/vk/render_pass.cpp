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

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "Vulkan device unavailable";
        return false;
    }

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = static_cast<VkFormat>(desc.colorFormat);
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = desc.clearOnLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = 1;
    createInfo.pAttachments = &colorAttachment;
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
    m_info.message = desc.debugName != nullptr ? desc.debugName : "headless color render pass";
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
