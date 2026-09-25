#include <fuse/renderer/vk/graphics_pipeline.hpp>

#include <fuse/renderer/vk/debug_utils.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

#include <vector>

namespace fuse::renderer {
namespace {

[[maybe_unused]] u64 graphicsCacheKey(const GraphicsPipelineDesc& desc) {
    if (desc.cacheKey != 0u) {
        return desc.cacheKey;
    }
    u64 key = combinePipelineKey(0u, 0x6772617068696331ull); // "graphic1"
    key = combinePipelineKey(key, desc.vertexShader->info().spirvHash);
    key = combinePipelineKey(key, desc.fragmentShader->info().spirvHash);
    const u64 state[] = {desc.colorFormat,
                         desc.colorAttachmentCount,
                         desc.depthFormat,
                         desc.polygonMode,
                         desc.cullMode,
                         desc.topology,
                         desc.frontFace,
                         desc.depthBiasEnable ? 1u : 0u,
                         desc.depthTest ? 1u : 0u,
                         desc.depthWrite ? 1u : 0u,
                         desc.depthCompareOp,
                         desc.blendEnable ? 1u : 0u,
                         desc.srcColorBlendFactor,
                         desc.dstColorBlendFactor,
                         desc.colorBlendOp,
                         desc.vertexStrideBytes,
                         desc.vertexFormat,
                         desc.useDynamicRendering ? 1u : 0u};
    for (const u64 value : state) {
        key = combinePipelineKey(key, value);
    }
    return key;
}

void snapshotPipelineCache(GraphicsPipelineInfo& info, PipelineCache* cache) {
    if (cache == nullptr) {
        return;
    }
    std::vector<u8> blob;
    cache->snapshotData(blob);
    info.cacheSnapshotBytes = static_cast<u32>(blob.size());
}

} // namespace

std::unique_ptr<GraphicsPipeline> GraphicsPipeline::create(VulkanDevice& device,
                                                           const GraphicsPipelineDesc& desc) {
    auto pipeline = std::unique_ptr<GraphicsPipeline>(new GraphicsPipeline());
    if (!pipeline->initialize(device, desc)) {
        pipeline->m_info.valid = false;
    }
    return pipeline;
}

GraphicsPipeline::~GraphicsPipeline() {
    shutdown();
}

void* GraphicsPipeline::nativeHandle() const {
    return m_handle;
}

bool GraphicsPipeline::rebuild() {
    if (m_device == nullptr) {
        return false;
    }
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_device->isValid()) {
        return false;
    }
#endif

    VulkanDevice* device = m_device;
    const GraphicsPipelineDesc desc = m_desc;
    const GraphicsPipelineInfo previousInfo = m_info;
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

bool GraphicsPipeline::initialize(VulkanDevice& device, const GraphicsPipelineDesc& desc) {
    m_device = &device;
    m_desc = desc;

    if (desc.layout == nullptr || desc.vertexShader == nullptr || desc.fragmentShader == nullptr ||
        (!desc.useDynamicRendering && desc.renderPass == nullptr &&
         desc.nativeRenderPassOverride == nullptr)) {
        m_info.message = "graphics pipeline requires layout, shaders, and render pass";
        return false;
    }

    if (!desc.layout->isValid() || !desc.vertexShader->isValid() || !desc.fragmentShader->isValid() ||
        (desc.renderPass != nullptr && !desc.renderPass->isValid())) {
        m_info.message = "graphics pipeline inputs are not valid";
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "Vulkan device unavailable";
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[2]{};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = static_cast<VkShaderModule>(desc.vertexShader->nativeHandle());
    shaderStages[0].pName = "main";
    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = static_cast<VkShaderModule>(desc.fragmentShader->nativeHandle());
    shaderStages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = desc.vertexStrideBytes;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attribute{};
    attribute.location = 0;
    attribute.binding = 0;
    attribute.format = static_cast<VkFormat>(desc.vertexFormat);
    attribute.offset = 0;

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attribute;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = static_cast<VkPrimitiveTopology>(desc.topology);
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = static_cast<VkPolygonMode>(desc.polygonMode);
    rasterizer.lineWidth = 1.f;
    rasterizer.cullMode = static_cast<VkCullModeFlags>(desc.cullMode);
    rasterizer.frontFace = static_cast<VkFrontFace>(desc.frontFace);
    rasterizer.depthBiasEnable = desc.depthBiasEnable ? VK_TRUE : VK_FALSE;
    rasterizer.depthBiasConstantFactor = desc.depthBiasConstantFactor;
    rasterizer.depthBiasClamp = desc.depthBiasClamp;
    rasterizer.depthBiasSlopeFactor = desc.depthBiasSlopeFactor;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = static_cast<VkCompareOp>(desc.depthCompareOp);
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = desc.stencilTest ? VK_TRUE : VK_FALSE;
    VkStencilOpState stencilState{};
    stencilState.failOp = static_cast<VkStencilOp>(desc.stencilFailOp);
    stencilState.passOp = static_cast<VkStencilOp>(desc.stencilPassOp);
    stencilState.depthFailOp = static_cast<VkStencilOp>(desc.stencilDepthFailOp);
    stencilState.compareOp = static_cast<VkCompareOp>(desc.stencilCompareOp);
    stencilState.compareMask = desc.stencilCompareMask;
    stencilState.writeMask = desc.stencilWriteMask;
    stencilState.reference = desc.stencilReference;
    depthStencil.front = stencilState;
    depthStencil.back = stencilState;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE;
    colorBlendAttachment.srcColorBlendFactor = static_cast<VkBlendFactor>(desc.srcColorBlendFactor);
    colorBlendAttachment.dstColorBlendFactor = static_cast<VkBlendFactor>(desc.dstColorBlendFactor);
    colorBlendAttachment.colorBlendOp = static_cast<VkBlendOp>(desc.colorBlendOp);
    colorBlendAttachment.srcAlphaBlendFactor = static_cast<VkBlendFactor>(desc.srcColorBlendFactor);
    colorBlendAttachment.dstAlphaBlendFactor = static_cast<VkBlendFactor>(desc.dstColorBlendFactor);
    colorBlendAttachment.alphaBlendOp = static_cast<VkBlendOp>(desc.colorBlendOp);

    const u32 colorCount = desc.colorAttachmentCount;
    if (colorCount == 0u || colorCount > RenderPassDesc::kMaxColorAttachments) {
        m_info.message = "graphics pipeline colour attachment count out of range";
        return false;
    }
    VkPipelineColorBlendAttachmentState colorBlendAttachments[RenderPassDesc::kMaxColorAttachments]{};
    for (u32 i = 0; i < colorCount; ++i) {
        colorBlendAttachments[i] = colorBlendAttachment;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = colorCount;
    colorBlending.pAttachments = colorBlendAttachments;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineLayout pipelineLayout =
        static_cast<VkPipelineLayout>(desc.layout->nativeHandle());

    VkFormat colorFormats[RenderPassDesc::kMaxColorAttachments]{};
    for (u32 i = 0; i < colorCount; ++i) {
        const u32 format = i == 0u || desc.renderPass == nullptr
                               ? desc.colorFormat
                               : desc.renderPass->colorFormatAt(i);
        colorFormats[i] = static_cast<VkFormat>(format);
    }
    VkFormat depthFormat = static_cast<VkFormat>(desc.depthFormat);
    VkPipelineRenderingCreateInfo rendering{};

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.subpass = 0;
    if (desc.useDynamicRendering) {
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = colorCount;
        rendering.pColorAttachmentFormats = colorFormats;
        if (desc.depthFormat != 0) {
            rendering.depthAttachmentFormat = depthFormat;
        }
        pipelineInfo.pNext = &rendering;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
    } else {
        pipelineInfo.renderPass = desc.nativeRenderPassOverride != nullptr
                                      ? static_cast<VkRenderPass>(desc.nativeRenderPassOverride)
                                      : static_cast<VkRenderPass>(desc.renderPass->nativeHandle());
    }

    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    PipelineCache* cache =
        desc.pipelineCache != nullptr && desc.pipelineCache->isValid() ? desc.pipelineCache : nullptr;
    VkPipelineCache pipelineCache =
        cache != nullptr ? static_cast<VkPipelineCache>(cache->nativeHandle()) : VK_NULL_HANDLE;

    // WP-0.5: creation feedback + optional cache-control probe (see compute_pipeline.cpp).
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedbackCreateInfo feedbackInfo{};
    feedbackInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
    feedbackInfo.pPipelineCreationFeedback = &feedback;
    const bool useFeedback = cache != nullptr && cache->creationFeedback();
    if (useFeedback) {
        feedbackInfo.pNext = pipelineInfo.pNext;
        pipelineInfo.pNext = &feedbackInfo;
    }
    if (desc.failIfNotCached && cache != nullptr && cache->creationCacheControl()) {
        pipelineInfo.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    }
    m_info.cache = {};
    m_info.cache.key = cache != nullptr ? graphicsCacheKey(desc) : 0u;

    const VkResult result =
        vkCreateGraphicsPipelines(static_cast<VkDevice>(device.nativeHandle()), pipelineCache, 1,
                                  &pipelineInfo, nullptr, &graphicsPipeline);
    if (result == VK_PIPELINE_COMPILE_REQUIRED) {
        m_info.cache.compileRequired = true;
        cache->noteCompileRequired();
        m_info.message = "graphics pipeline not in cache (compile required)";
        return false;
    }
    if (result != VK_SUCCESS) {
        m_info.message = "vkCreateGraphicsPipelines failed";
        return false;
    }
    if (cache != nullptr) {
        m_info.cache.warmStart = cache->isWarm(m_info.cache.key);
        m_info.cache.feedbackValid =
            useFeedback && (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0u;
        m_info.cache.driverCacheHit =
            m_info.cache.feedbackValid &&
            (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0u;
        m_info.cache.durationNs = m_info.cache.feedbackValid ? feedback.duration : 0u;
        cache->notePipelineCreated(m_info.cache.key, m_info.cache.feedbackValid, m_info.cache.driverCacheHit);
    }

    m_handle = graphicsPipeline;
    m_info.valid = true;
    nameVkObject(device.nativeHandle(), vk_object_type::kPipeline, m_handle,
                       desc.debugName != nullptr ? desc.debugName : "fuse.graphics_pipeline");
    m_info.dynamicRendering = desc.useDynamicRendering;
    m_info.depthFormat = desc.depthFormat;
    m_info.hasDynamicDepth = desc.useDynamicRendering && desc.depthFormat != 0;
    m_info.blendEnabled = desc.blendEnable;
    m_info.vertexStrideBytes = desc.vertexStrideBytes;
    m_info.topology = desc.topology;
    m_info.depthBiasEnabled = desc.depthBiasEnable;
    m_info.stencilEnabled = desc.stencilTest;
    if (desc.useDynamicRendering) {
        m_info.message = desc.debugName != nullptr
                             ? std::string(desc.debugName) + " (dynamic rendering)"
                             : "graphics pipeline (dynamic rendering)";
    } else {
        m_info.message = desc.debugName != nullptr ? desc.debugName : "graphics pipeline scaffold";
    }
    snapshotPipelineCache(m_info, desc.pipelineCache);
    return true;
#else
    m_info.valid = true;
    m_info.dynamicRendering = desc.useDynamicRendering;
    m_info.depthFormat = desc.depthFormat;
    m_info.hasDynamicDepth = desc.useDynamicRendering && desc.depthFormat != 0;
    m_info.blendEnabled = desc.blendEnable;
    m_info.vertexStrideBytes = desc.vertexStrideBytes;
    m_info.topology = desc.topology;
    m_info.depthBiasEnabled = desc.depthBiasEnable;
    m_info.stencilEnabled = desc.stencilTest;
    if (desc.useDynamicRendering) {
        m_info.message = desc.debugName != nullptr
                             ? std::string(desc.debugName) + " (dynamic rendering)"
                             : "graphics pipeline placeholder (stub backend, dynamic rendering)";
    } else {
        m_info.message = "graphics pipeline placeholder (stub backend)";
    }
    snapshotPipelineCache(m_info, desc.pipelineCache);
    return true;
#endif
}

void GraphicsPipeline::shutdown() {
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
