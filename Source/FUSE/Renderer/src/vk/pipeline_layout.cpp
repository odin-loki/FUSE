#include <fuse/renderer/vk/pipeline_layout.hpp>

#include <utility>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

#if defined(FUSE_VULKAN_BACKEND)
VkShaderStageFlags toVkStageFlags(u32 flags) {
    return static_cast<VkShaderStageFlags>(flags);
}
#endif

} // namespace

std::unique_ptr<PipelineLayout> PipelineLayout::create(VulkanDevice& device,
                                                       const PipelineLayoutDesc& desc) {
    auto layout = std::unique_ptr<PipelineLayout>(new PipelineLayout());
    if (!layout->initialize(device, desc)) {
        layout->m_info.valid = false;
    }
    return layout;
}

PipelineLayout::~PipelineLayout() {
    shutdown();
}

void* PipelineLayout::nativeHandle() const {
    return m_handle;
}

bool PipelineLayout::initialize(VulkanDevice& device, const PipelineLayoutDesc& desc) {
    m_device = &device;
    m_info.pushConstantRangeCount = static_cast<u32>(desc.pushConstants.size());
    m_info.descriptorSetCount = static_cast<u32>(desc.descriptorSets.size());

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "Vulkan device unavailable";
        return false;
    }

    std::vector<VkPushConstantRange> pushRanges;
    pushRanges.reserve(desc.pushConstants.size());
    for (const PushConstantRangeDesc& range : desc.pushConstants) {
        VkPushConstantRange vkRange{};
        vkRange.stageFlags = toVkStageFlags(range.stageFlags);
        vkRange.offset = range.offset;
        vkRange.size = range.size;
        pushRanges.push_back(vkRange);
    }

    VkPipelineLayoutCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    createInfo.pushConstantRangeCount = static_cast<u32>(pushRanges.size());
    createInfo.pPushConstantRanges = pushRanges.empty() ? nullptr : pushRanges.data();
    createInfo.setLayoutCount = 0;
    createInfo.pSetLayouts = nullptr;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    const VkResult result =
        vkCreatePipelineLayout(static_cast<VkDevice>(device.nativeHandle()), &createInfo, nullptr,
                               &pipelineLayout);
    if (result != VK_SUCCESS) {
        m_info.message = "vkCreatePipelineLayout failed";
        return false;
    }

    m_handle = pipelineLayout;
    m_info.valid = true;
    m_info.message = desc.debugName != nullptr ? desc.debugName : "pipeline layout placeholder";
    return true;
#else
    m_info.valid = true;
    m_info.message = "pipeline layout placeholder (stub backend)";
    return true;
#endif
}

void PipelineLayout::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle != nullptr && m_device != nullptr && m_device->isValid()) {
        vkDestroyPipelineLayout(static_cast<VkDevice>(m_device->nativeHandle()),
                                static_cast<VkPipelineLayout>(m_handle), nullptr);
    }
#endif
    m_handle = nullptr;
    m_device = nullptr;
}

} // namespace fuse::renderer
