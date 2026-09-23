#include <fuse/renderer/vk/graphics_pipeline.hpp>

#include <fuse/renderer/vk/debug_utils.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

#include <vector>

namespace fuse::renderer {
namespace {

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
    depthStencil.stencilTestEnable = VK_FALSE;

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
    VkPipelineCache pipelineCache =
        desc.pipelineCache != nullptr && desc.pipelineCache->isValid()
            ? static_cast<VkPipelineCache>(desc.pipelineCache->nativeHandle())
            : VK_NULL_HANDLE;
    const VkResult result =
        vkCreateGraphicsPipelines(static_cast<VkDevice>(device.nativeHandle()), pipelineCache, 1,
                                  &pipelineInfo, nullptr, &graphicsPipeline);
    if (result != VK_SUCCESS) {
        m_info.message = "vkCreateGraphicsPipelines failed";
        return false;
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
