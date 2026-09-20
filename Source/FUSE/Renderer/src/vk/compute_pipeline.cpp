#include <fuse/renderer/vk/compute_pipeline.hpp>

#include <fuse/renderer/vk/debug_utils.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

#include <vector>

namespace fuse::renderer {
namespace {

constexpr u32 kVkObjectTypePipeline = 19;

void snapshotPipelineCache(ComputePipelineInfo& info, PipelineCache* cache) {
    if (cache == nullptr) {
        return;
    }
    std::vector<u8> blob;
    cache->snapshotData(blob);
    info.cacheSnapshotBytes = static_cast<u32>(blob.size());
}

void recordLocalSize(ComputePipelineInfo& info, const ComputePipelineDesc& desc) {
    info.localSizeX = desc.localSizeX;
    info.localSizeY = desc.localSizeY;
    info.localSizeZ = desc.localSizeZ;
}

} // namespace

std::unique_ptr<ComputePipeline> ComputePipeline::create(VulkanDevice& device,
                                                         const ComputePipelineDesc& desc) {
    auto pipeline = std::unique_ptr<ComputePipeline>(new ComputePipeline());
    if (!pipeline->initialize(device, desc)) {
        pipeline->m_info.valid = false;
    }
    return pipeline;
}

ComputePipeline::~ComputePipeline() {
    shutdown();
}

void* ComputePipeline::nativeHandle() const {
    return m_handle;
}

bool ComputePipeline::rebuild() {
    if (m_device == nullptr) {
        return false;
    }
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_device->isValid()) {
        return false;
    }
#endif

    VulkanDevice* device = m_device;
    const ComputePipelineDesc desc = m_desc;
    const ComputePipelineInfo previousInfo = m_info;
    void* previousHandle = m_handle;
    m_handle = nullptr;
    shutdown();

    if (!initialize(*device, desc)) {
        m_handle = previousHandle;
        m_device = device;
        m_desc = desc;
        m_info = previousInfo;
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (previousHandle != nullptr && device->isValid()) {
        vkDestroyPipeline(static_cast<VkDevice>(device->nativeHandle()),
                          static_cast<VkPipeline>(previousHandle), nullptr);
    }
#endif
    m_info.rebuildCount = previousInfo.rebuildCount + 1u;
    return true;
}

bool ComputePipeline::initialize(VulkanDevice& device, const ComputePipelineDesc& desc) {
    m_device = &device;
    m_desc = desc;
    recordLocalSize(m_info, desc);

    if (desc.layout == nullptr || desc.computeShader == nullptr) {
        m_info.message = "compute pipeline requires layout and compute shader";
        return false;
    }

    if (!desc.layout->isValid() || !desc.computeShader->isValid()) {
        m_info.message = "compute pipeline inputs are not valid";
        return false;
    }

    if (desc.computeShader->stage() != ShaderStage::Compute) {
        m_info.message = "compute pipeline requires ShaderStage::Compute";
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "Vulkan device unavailable";
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = static_cast<VkShaderModule>(desc.computeShader->nativeHandle());
    shaderStage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStage;
    pipelineInfo.layout = static_cast<VkPipelineLayout>(desc.layout->nativeHandle());

    VkPipeline computePipeline = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache =
        desc.pipelineCache != nullptr && desc.pipelineCache->isValid()
            ? static_cast<VkPipelineCache>(desc.pipelineCache->nativeHandle())
            : VK_NULL_HANDLE;
    const VkResult result =
        vkCreateComputePipelines(static_cast<VkDevice>(device.nativeHandle()), pipelineCache, 1,
                                 &pipelineInfo, nullptr, &computePipeline);
    if (result != VK_SUCCESS) {
        m_info.message = "vkCreateComputePipelines failed";
        return false;
    }

    m_handle = computePipeline;
    m_info.valid = true;
    m_info.message = desc.debugName != nullptr ? desc.debugName : "compute pipeline";
    if (desc.debugName != nullptr) {
        const u64 objectHandle = static_cast<u64>(reinterpret_cast<uintptr_t>(m_handle));
        setDebugObjectName(device.nativeHandle(), kVkObjectTypePipeline, objectHandle,
                           desc.debugName);
    }
    snapshotPipelineCache(m_info, desc.pipelineCache);
    return true;
#else
    (void)device;
    m_info.message = "compute pipeline requires Vulkan backend";
    return false;
#endif
}

void ComputePipeline::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle != nullptr && m_device != nullptr && m_device->isValid()) {
        vkDestroyPipeline(static_cast<VkDevice>(m_device->nativeHandle()),
                          static_cast<VkPipeline>(m_handle), nullptr);
    }
#endif
    m_handle = nullptr;
    m_device = nullptr;
}

} // namespace fuse::renderer
