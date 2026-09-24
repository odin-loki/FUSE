#include <fuse/renderer/vk/pipeline_layout.hpp>

#include <fuse/renderer/shader/shader_reflection.hpp>
#include <fuse/renderer/vk/debug_utils.hpp>

#include <string>
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

std::unique_ptr<PipelineLayout> PipelineLayout::createFromReflection(VulkanDevice& device,
                                                                     const ShaderReflection& reflection,
                                                                     const char* debugName) {
    auto layout = std::unique_ptr<PipelineLayout>(new PipelineLayout());
    if (!layout->initializeFromReflection(device, reflection, debugName)) {
        layout->m_info.valid = false;
        layout->shutdown();
    }
    return layout;
}

void* PipelineLayout::setLayoutHandle(u32 set) const {
    return set < m_ownedSetLayouts.size() ? m_ownedSetLayouts[set] : nullptr;
}

bool PipelineLayout::initializeFromReflection(VulkanDevice& device, const ShaderReflection& reflection,
                                              const char* debugName) {
    m_device = &device;
    if (!reflection.valid) {
        m_info.message = "invalid shader reflection";
        return false;
    }
    for (const ShaderBindingReflection& binding : reflection.bindings) {
        if (binding.count == 0u) {
            m_info.message = "runtime-sized array at set " + std::to_string(binding.set) + " binding " +
                             std::to_string(binding.binding) + " needs an explicit (bindless) layout";
            return false;
        }
    }
    const u32 setCount = reflection.setCount();
    m_info.descriptorSetCount = setCount;
    m_info.pushConstantRangeCount = reflection.pushConstantBytes > 0u ? 1u : 0u;

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "Vulkan device unavailable";
        return false;
    }
    const VkDevice vkDevice = static_cast<VkDevice>(device.nativeHandle());
    std::vector<VkDescriptorSetLayout> setLayouts;
    for (u32 set = 0; set < setCount; ++set) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const ShaderBindingReflection& binding : reflection.bindings) {
            if (binding.set != set) {
                continue;
            }
            VkDescriptorSetLayoutBinding vkBinding{};
            vkBinding.binding = binding.binding;
            vkBinding.descriptorType = static_cast<VkDescriptorType>(binding.descriptorType);
            vkBinding.descriptorCount = binding.count;
            vkBinding.stageFlags = toVkStageFlags(binding.stageFlags != 0u ? binding.stageFlags : reflection.stageFlags);
            bindings.push_back(vkBinding);
        }
        VkDescriptorSetLayoutCreateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = static_cast<u32>(bindings.size());
        setInfo.pBindings = bindings.empty() ? nullptr : bindings.data();
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(vkDevice, &setInfo, nullptr, &setLayout) != VK_SUCCESS) {
            m_info.message = "vkCreateDescriptorSetLayout failed for set " + std::to_string(set);
            return false;
        }
        m_ownedSetLayouts.push_back(setLayout);
        setLayouts.push_back(setLayout);
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = toVkStageFlags(reflection.stageFlags);
    pushRange.offset = 0;
    pushRange.size = reflection.pushConstantBytes;

    VkPipelineLayoutCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    createInfo.setLayoutCount = static_cast<u32>(setLayouts.size());
    createInfo.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
    createInfo.pushConstantRangeCount = reflection.pushConstantBytes > 0u ? 1u : 0u;
    createInfo.pPushConstantRanges = reflection.pushConstantBytes > 0u ? &pushRange : nullptr;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(vkDevice, &createInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        m_info.message = "vkCreatePipelineLayout failed";
        return false;
    }
    m_handle = pipelineLayout;
    m_info.valid = true;
    const char* name = debugName != nullptr ? debugName : "fuse.pipeline_layout.reflected";
    nameVkObject(device.nativeHandle(), vk_object_type::kPipelineLayout, m_handle, name);
    m_info.message = name;
    return true;
#else
    (void)debugName;
    m_info.valid = true;
    m_info.message = "reflected pipeline layout placeholder (stub backend)";
    return true;
#endif
}

PipelineLayout::~PipelineLayout() {
    shutdown();
}

void* PipelineLayout::nativeHandle() const {
    return m_handle;
}

bool PipelineLayout::initialize(VulkanDevice& device, const PipelineLayoutDesc& desc) {
    m_device = &device;
    const bool requestedBindless = desc.bindlessSetLayout != nullptr;
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

    // Set 0 is the bindless layout when provided (B2.4).
    VkDescriptorSetLayout bindlessLayout = VK_NULL_HANDLE;
    if (requestedBindless) {
        bindlessLayout = static_cast<VkDescriptorSetLayout>(desc.bindlessSetLayout);
    }

    VkPipelineLayoutCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    createInfo.pushConstantRangeCount = static_cast<u32>(pushRanges.size());
    createInfo.pPushConstantRanges = pushRanges.empty() ? nullptr : pushRanges.data();
    createInfo.setLayoutCount = bindlessLayout != VK_NULL_HANDLE ? 1u : 0u;
    createInfo.pSetLayouts = bindlessLayout != VK_NULL_HANDLE ? &bindlessLayout : nullptr;

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
    nameVkObject(device.nativeHandle(), vk_object_type::kPipelineLayout, m_handle,
                       desc.debugName != nullptr ? desc.debugName : "fuse.pipeline_layout");
    m_info.hasBindlessSet = requestedBindless;
    if (requestedBindless && m_info.descriptorSetCount < 1u) {
        m_info.descriptorSetCount = 1u;
    }
    m_info.message = desc.debugName != nullptr ? desc.debugName : "pipeline layout placeholder";
    return true;
#else
    m_info.valid = true;
    m_info.hasBindlessSet = requestedBindless;
    if (requestedBindless && m_info.descriptorSetCount < 1u) {
        m_info.descriptorSetCount = 1u;
    }
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
    if (m_device != nullptr && m_device->isValid()) {
        for (void* setLayout : m_ownedSetLayouts) {
            vkDestroyDescriptorSetLayout(static_cast<VkDevice>(m_device->nativeHandle()),
                                         static_cast<VkDescriptorSetLayout>(setLayout), nullptr);
        }
    }
#endif
    m_ownedSetLayouts.clear();
    m_handle = nullptr;
    m_device = nullptr;
}

} // namespace fuse::renderer
