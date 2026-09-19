#include <fuse/renderer/vk/composite_gpu_path.hpp>

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/graphics_pipeline.hpp>
#include <fuse/renderer/vk/pipeline_cache.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/render_pass.hpp>

#include <array>
#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

constexpr u32 kColorFormat = 37; // VK_FORMAT_R8G8B8A8_UNORM

#if defined(FUSE_VULKAN_BACKEND)
u32 findMemoryType(VkPhysicalDevice physicalDevice, u32 typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (u32 i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

GraphicsPipelineDesc makeCompositePipelineDesc(PipelineLayout* layout, ShaderModule* vert, ShaderModule* frag,
                                               RenderPass* renderPass, PipelineCache* cache,
                                               const char* debugName) {
    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.layout = layout;
    pipelineDesc.vertexShader = vert;
    pipelineDesc.fragmentShader = frag;
    pipelineDesc.renderPass = renderPass;
    pipelineDesc.pipelineCache = cache;
    pipelineDesc.colorFormat = kColorFormat;
    pipelineDesc.debugName = debugName;
    return pipelineDesc;
}
#endif

} // namespace

std::unique_ptr<CompositeGpuPath> CompositeGpuPath::create(VulkanDevice& device,
                                                           const CompositeGpuPathDesc& desc) {
    auto path = std::unique_ptr<CompositeGpuPath>(new CompositeGpuPath());
    if (!path->initialize(device, desc)) {
        path->m_stats.pipelineReady = false;
    }
    return path;
}

CompositeGpuPath::~CompositeGpuPath() {
    shutdown();
}

bool CompositeGpuPath::registerRasterSource(void* imageView) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.pipelineReady || !bindlessNativeHandleReady(imageView)) {
        m_stats.message = "composite bindless raster registration skipped";
        return false;
    }

    fuse::renderer::Texture texture{};
    texture.view = imageView;
    texture.desc.width = m_desc.width;
    texture.desc.height = m_desc.height;
    texture.desc.format = GpuFormat::R8G8B8A8Unorm;

    if (m_rasterTextureSlot.isValid()) {
        m_bindless.unregisterSlot(m_rasterTextureSlot);
    }

    m_rasterTextureSlot = m_bindless.registerTextureSlot(texture, false);
    if (!m_rasterTextureSlot.isValid()) {
        m_stats.message = "composite raster bindless slot failed";
        return false;
    }

    m_stats.rasterTextureIndex = m_rasterTextureSlot.index;
    m_stats.bindlessBound = m_bindless.vulkanDescriptorsReady();
    m_stats.message = "composite raster texture registered in bindless heap";
    return true;
#else
    (void)imageView;
    return false;
#endif
}

bool CompositeGpuPath::ensurePresentPipeline(void* presentRenderPass) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.pipelineReady || presentRenderPass == nullptr) {
        return false;
    }
    if (m_presentPipeline != nullptr && m_presentPipeline->isValid()) {
        return true;
    }

    GraphicsPipelineDesc pipelineDesc = makeCompositePipelineDesc(
        m_pipelineLayout.get(), m_vertexShader.get(), m_fragmentShader.get(), m_offscreenRenderPass.get(),
        m_pipelineCache.get(), "composite_present_pipeline");
    pipelineDesc.nativeRenderPassOverride = presentRenderPass;
    m_presentPipeline = GraphicsPipeline::create(*m_device, pipelineDesc);

    return m_presentPipeline != nullptr && m_presentPipeline->isValid();
#else
    (void)presentRenderPass;
    return false;
#endif
}

void CompositeGpuPath::fillEncodeContext(VkFrameEncodeContext& context, float blend,
                                         bool presentActive) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.pipelineReady || !m_rasterTextureSlot.isValid()) {
        return;
    }

    context.compositeBlend = blend;
    context.rasterTextureBindlessIndex = m_stats.rasterTextureIndex;
    context.bindlessDescriptorSet = m_bindless.descriptorSetHandle();
    context.compositePipelineLayout = m_pipelineLayout->nativeHandle();
    context.compositeVertexBuffer = m_vertexBuffer;
    context.compositeTargetsSwapchain = presentActive && context.presentActive;

    if (presentActive && context.presentActive && context.presentFramebuffer != nullptr &&
        context.presentRenderPass != nullptr) {
        context.compositeRenderPass = context.presentRenderPass;
        context.compositeFramebuffer = context.presentFramebuffer;
        context.compositeWidth = context.presentWidth;
        context.compositeHeight = context.presentHeight;
        context.compositePipeline =
            m_presentPipeline != nullptr ? m_presentPipeline->nativeHandle()
                                         : m_offscreenPipeline->nativeHandle();
    } else if (context.active) {
        context.compositeRenderPass = context.renderPass;
        context.compositeFramebuffer = context.framebuffer;
        context.compositeWidth = context.width;
        context.compositeHeight = context.height;
        context.compositePipeline = m_offscreenPipeline->nativeHandle();
    } else {
        return;
    }

    context.compositeActive =
        context.compositePipeline != nullptr && context.compositeFramebuffer != nullptr &&
        context.compositeRenderPass != nullptr && context.bindlessDescriptorSet != nullptr &&
        context.compositeVertexBuffer != nullptr;
    if (context.compositeActive) {
        ++m_stats.framesEncoded;
    }
#else
    (void)context;
    (void)blend;
    (void)presentActive;
#endif
}

bool CompositeGpuPath::initialize(VulkanDevice& device, const CompositeGpuPathDesc& desc) {
    m_device = &device;
    m_desc = desc;

    if (desc.vertexSpirvPath == nullptr || desc.fragmentSpirvPath == nullptr) {
        m_stats.message = "composite fixture SPIR-V paths required";
        return false;
    }

    m_bindless.init(device);
    if (!m_bindless.vulkanDescriptorsReady()) {
        m_stats.message = "bindless descriptors not ready for composite path";
        return false;
    }

    RenderPassDesc renderPassDesc{};
    renderPassDesc.colorFormat = kColorFormat;
    renderPassDesc.clearOnLoad = false;
    renderPassDesc.debugName = "composite_offscreen_render_pass";
    m_offscreenRenderPass = RenderPass::create(device, renderPassDesc);
    if (m_offscreenRenderPass == nullptr || !m_offscreenRenderPass->isValid()) {
        m_stats.message = "composite render pass creation failed";
        return false;
    }

    m_vertexShader = ShaderModule::createFromFile(device, ShaderStage::Vertex, desc.vertexSpirvPath);
    m_fragmentShader =
        ShaderModule::createFromFile(device, ShaderStage::Fragment, desc.fragmentSpirvPath);
    if (m_vertexShader == nullptr || m_fragmentShader == nullptr || !m_vertexShader->isValid() ||
        !m_fragmentShader->isValid()) {
        m_stats.message = "composite shader modules failed";
        return false;
    }

    PipelineLayoutDesc layoutDesc{};
    layoutDesc.bindlessSetLayout = m_bindless.layoutHandle();
    layoutDesc.pushConstants.push_back(
        {0, sizeof(float) + sizeof(u32) * 2u, 0x10}); // VK_SHADER_STAGE_FRAGMENT_BIT
    layoutDesc.debugName = "composite_bindless_layout";
    m_pipelineLayout = PipelineLayout::create(device, layoutDesc);
    if (m_pipelineLayout == nullptr || !m_pipelineLayout->isValid()) {
        m_stats.message = "composite pipeline layout failed";
        return false;
    }

    m_pipelineCache = PipelineCache::create(device);
    if (m_pipelineCache == nullptr || !m_pipelineCache->isValid()) {
        m_stats.message = "composite pipeline cache failed";
        return false;
    }

    GraphicsPipelineDesc pipelineDesc = makeCompositePipelineDesc(
        m_pipelineLayout.get(), m_vertexShader.get(), m_fragmentShader.get(), m_offscreenRenderPass.get(),
        m_pipelineCache.get(), "composite_offscreen_pipeline");
    m_offscreenPipeline = GraphicsPipeline::create(device, pipelineDesc);
    if (m_offscreenPipeline == nullptr || !m_offscreenPipeline->isValid()) {
        m_stats.message = m_offscreenPipeline != nullptr ? m_offscreenPipeline->info().message
                                                         : "composite pipeline allocation failed";
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_stats.pipelineReady = true;
        m_stats.message = "composite path scaffold ready (stub device)";
        return true;
    }

    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    auto physicalDevice = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(vkDevice, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
        m_stats.message = "composite sampler creation failed";
        return false;
    }
    m_sampler = sampler;
    m_samplerSlot = m_bindless.registerSamplerSlot(sampler);

    const std::array<float, 9> fullscreenVertices = {
        -1.f, -1.f, 0.f,
        3.f, -1.f, 0.f,
        -1.f, 3.f, 0.f,
    };

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(fullscreenVertices);
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(vkDevice, &bufferInfo, nullptr, &vertexBuffer) != VK_SUCCESS) {
        m_stats.message = "composite vertex buffer creation failed";
        return false;
    }
    m_vertexBuffer = vertexBuffer;

    VkMemoryRequirements memRequirements{};
    vkGetBufferMemoryRequirements(vkDevice, vertexBuffer, &memRequirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        findMemoryType(physicalDevice, memRequirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &vertexMemory) != VK_SUCCESS) {
        m_stats.message = "composite vertex memory allocation failed";
        return false;
    }
    m_vertexMemory = vertexMemory;
    vkBindBufferMemory(vkDevice, vertexBuffer, vertexMemory, 0);

    void* mapped = nullptr;
    vkMapMemory(vkDevice, vertexMemory, 0, sizeof(fullscreenVertices), 0, &mapped);
    std::memcpy(mapped, fullscreenVertices.data(), sizeof(fullscreenVertices));
    vkUnmapMemory(vkDevice, vertexMemory);
#endif

    m_stats.pipelineReady = true;
    m_stats.bindlessBound = m_bindless.vulkanDescriptorsReady();
    m_stats.message = "composite GPU path ready";
    return true;
}

void CompositeGpuPath::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && m_device->isValid()) {
        auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
        if (m_sampler != nullptr) {
            vkDestroySampler(vkDevice, static_cast<VkSampler>(m_sampler), nullptr);
        }
        if (m_vertexBuffer != nullptr) {
            vkDestroyBuffer(vkDevice, static_cast<VkBuffer>(m_vertexBuffer), nullptr);
        }
        if (m_vertexMemory != nullptr) {
            vkFreeMemory(vkDevice, static_cast<VkDeviceMemory>(m_vertexMemory), nullptr);
        }
    }
    m_sampler = nullptr;
    m_vertexBuffer = nullptr;
    m_vertexMemory = nullptr;
    m_rasterTextureSlot = {};
    m_samplerSlot = {};
#endif

    m_presentPipeline.reset();
    m_offscreenPipeline.reset();
    m_pipelineCache.reset();
    m_pipelineLayout.reset();
    m_fragmentShader.reset();
    m_vertexShader.reset();
    m_offscreenRenderPass.reset();
    if (m_device != nullptr) {
        m_bindless.destroy(*m_device);
    }
    m_device = nullptr;
}

} // namespace fuse::renderer
