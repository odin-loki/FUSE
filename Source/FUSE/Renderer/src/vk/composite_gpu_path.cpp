#include <fuse/renderer/vk/composite_gpu_path.hpp>
#include <fuse/renderer/vk/image_readback.hpp>

#include <fuse/renderer/cuda/interop.hpp>
#include <fuse/renderer/cuda/interop_fill.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/graphics_pipeline.hpp>
#include <fuse/renderer/vk/pipeline_cache.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/render_pass.hpp>

#include <array>
#include <cstring>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#endif

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

constexpr u32 kColorFormat = 37; // VK_FORMAT_R8G8B8A8_UNORM

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

bool exportDeviceMemoryHandle(VkDevice device, VkDeviceMemory memory, void*& outHandle) {
#if defined(_WIN32)
    using GetMemoryFn = PFN_vkGetMemoryWin32HandleKHR;
    const char* fnName = "vkGetMemoryWin32HandleKHR";
    VkExternalMemoryHandleTypeFlagBits handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    using GetMemoryFn = PFN_vkGetMemoryFdKHR;
    const char* fnName = "vkGetMemoryFdKHR";
    VkExternalMemoryHandleTypeFlagBits handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    auto getHandle = reinterpret_cast<GetMemoryFn>(vkGetDeviceProcAddr(device, fnName));
    if (getHandle == nullptr) {
        return false;
    }

#if defined(_WIN32)
    HANDLE winHandle = nullptr;
    VkMemoryGetWin32HandleInfoKHR handleInfo{};
    handleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    handleInfo.memory = memory;
    handleInfo.handleType = handleType;
    if (getHandle(device, &handleInfo, &winHandle) != VK_SUCCESS || winHandle == nullptr) {
        return false;
    }
    outHandle = winHandle;
#else
    int fd = -1;
    VkMemoryGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = memory;
    fdInfo.handleType = handleType;
    if (getHandle(device, &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
        return false;
    }
    outHandle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
#endif
    return true;
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

bool CompositeGpuPath::registerCudaSource(void* imageView) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.pipelineReady || !bindlessNativeHandleReady(imageView)) {
        m_stats.cudaTexturePlaceholder = true;
        m_stats.message = "composite bindless cuda registration skipped";
        return false;
    }

    fuse::renderer::Texture texture{};
    texture.view = imageView;
    texture.desc.width = m_desc.width;
    texture.desc.height = m_desc.height;
    texture.desc.format = GpuFormat::R8G8B8A8Unorm;
    texture.desc.cudaInterop = true;

    if (m_cudaTextureSlot.isValid()) {
        m_bindless.unregisterSlot(m_cudaTextureSlot);
    }

    m_cudaTextureSlot = m_bindless.registerTextureSlot(texture, false);
    if (!m_cudaTextureSlot.isValid()) {
        m_stats.cudaTexturePlaceholder = true;
        m_stats.message = "composite cuda bindless slot failed";
        return false;
    }

    m_stats.cudaTextureIndex = m_cudaTextureSlot.index;
    m_stats.cudaTextureActive = true;
    m_stats.cudaTexturePlaceholder = false;
    m_stats.message = "composite cuda texture registered in bindless heap";
    return true;
#else
    (void)imageView;
    return false;
#endif
}

bool CompositeGpuPath::ensureCudaInteropTexture() {
#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_HAS_CUDA)
    if (!m_stats.pipelineReady || m_device == nullptr || !m_device->isValid()) {
        m_stats.cudaTexturePlaceholder = true;
        return false;
    }
    if (m_cudaTextureSlot.isValid()) {
        return m_stats.cudaTextureActive;
    }
    if (!fuse::renderer::cuda::interopAvailable()) {
        m_stats.cudaTexturePlaceholder = true;
        m_stats.message = "cuda interop unavailable — composite uses placeholder colour";
        return false;
    }

    auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
    auto physicalDevice = static_cast<VkPhysicalDevice>(m_device->nativePhysicalDevice());

    VkExternalMemoryImageCreateInfo externalImageInfo{};
    externalImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
#if defined(_WIN32)
    externalImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    externalImageInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalImageInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {m_desc.width, m_desc.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkExportMemoryAllocateInfo exportAllocInfo{};
    exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
#if defined(_WIN32)
    exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &exportAllocInfo;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(vkDevice, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        m_stats.message = "cuda interop image creation failed";
        return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(vkDevice, image, &memRequirements);
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        findMemoryType(physicalDevice, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(vkDevice, image, nullptr);
        m_stats.message = "cuda interop image memory allocation failed";
        return false;
    }
    vkBindImageMemory(vkDevice, image, memory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        vkFreeMemory(vkDevice, memory, nullptr);
        vkDestroyImage(vkDevice, image, nullptr);
        m_stats.message = "cuda interop image view creation failed";
        return false;
    }

    m_cudaImage = image;
    m_cudaImageLayout = 0; // VK_IMAGE_LAYOUT_UNDEFINED
    m_cudaImageMemory = memory;
    m_cudaImageView = view;
    m_cudaAllocationSize = memRequirements.size;
    m_cudaExportedHandle = nullptr;
    if (!exportDeviceMemoryHandle(vkDevice, memory, m_cudaExportedHandle)) {
        m_stats.message = "cuda interop memory export unavailable — fill uses stub path";
    }

    if (!registerCudaSource(view)) {
        return false;
    }

    m_stats.message = "cuda interop texture allocated for composite sampling";
    return true;
#else
    m_stats.cudaTexturePlaceholder = true;
    m_stats.message = "cuda interop texture unavailable in stub build";
    return false;
#endif
}

bool CompositeGpuPath::fillCudaInteropTexture(fuse::renderer::cuda::FrameSyncPair* frameSync, u64 frameIndex,
                                              bool useJobLane) {
    m_stats.cudaFillAttempted = true;
    m_stats.lastFrameSyncIndex = frameIndex;

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_HAS_CUDA)
    if (!m_stats.pipelineReady || m_cudaExportedHandle == nullptr || m_cudaAllocationSize == 0) {
        m_stats.cudaFillStubPath = true;
        m_stats.cudaFillOk = false;
        if (m_stats.message.empty()) {
            m_stats.message = "cuda interop fill skipped — texture export unavailable";
        }
        return false;
    }

    fuse::renderer::cuda::InteropFillDesc fillDesc{};
    fillDesc.exportedMemoryHandle = m_cudaExportedHandle;
    fillDesc.allocationSize = m_cudaAllocationSize;
    fillDesc.width = m_desc.width;
    fillDesc.height = m_desc.height;
    fillDesc.frameSync = frameSync;
    fillDesc.frameIndex = frameIndex;

    const fuse::renderer::cuda::InteropFillResult fillResult =
        useJobLane ? fuse::renderer::cuda::submitInteropFillJob(fillDesc)
                   : fuse::renderer::cuda::fillInteropTexture(fillDesc);

    m_stats.cudaFillStubPath = fillResult.stubPath;
    m_stats.cudaFillOk = fillResult.ok;
    if (fillResult.reason != nullptr) {
        m_stats.message = fillResult.reason;
    }
    return fillResult.ok;
#else
    (void)frameSync;
    (void)frameIndex;
    (void)useJobLane;
    m_stats.cudaFillStubPath = true;
    m_stats.cudaFillOk = false;
    m_stats.message = "cuda interop fill unavailable in stub build";
    return false;
#endif
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

bool CompositeGpuPath::registerRasterDepth(void* imageView) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.pipelineReady || !bindlessNativeHandleReady(imageView)) {
        m_stats.depthTextureBound = false;
        m_stats.message = "composite bindless depth registration skipped";
        return false;
    }

    fuse::renderer::Texture texture{};
    texture.view = imageView;
    texture.desc.width = m_desc.width;
    texture.desc.height = m_desc.height;
    texture.desc.format = GpuFormat::D32Sfloat;
    texture.desc.usage = ImageUsage::Sampled;

    if (m_depthTextureSlot.isValid()) {
        m_bindless.unregisterSlot(m_depthTextureSlot);
    }

    m_depthTextureSlot = m_bindless.registerTextureSlot(texture, false);
    if (!m_depthTextureSlot.isValid()) {
        m_stats.depthTextureBound = false;
        m_stats.depthTextureIndex = UINT32_MAX;
        m_stats.message = "composite depth bindless slot failed";
        return false;
    }

    m_stats.depthTextureIndex = m_depthTextureSlot.index;
    m_stats.depthTextureBound = true;
    m_stats.bindlessBound = m_bindless.vulkanDescriptorsReady();
    m_stats.message = "composite raster depth registered in bindless heap";
    return true;
#else
    (void)imageView;
    m_stats.depthTextureBound = false;
    m_stats.depthTextureIndex = UINT32_MAX;
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

bool CompositeGpuPath::readbackOutput(std::vector<u8>& outRgba) const {
    outRgba.clear();
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr || m_outputImage == nullptr) {
        return false;
    }
    const u32 width = m_desc.width > 0u ? m_desc.width : 1u;
    const u32 height = m_desc.height > 0u ? m_desc.height : 1u;
    return readbackColorImage(*m_device, m_outputImage, width, height, m_outputLayout, outRgba);
#else
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
    context.cudaTextureBindlessIndex =
        m_stats.cudaTextureActive ? m_stats.cudaTextureIndex : UINT32_MAX;
    context.depthTextureBindlessIndex =
        m_stats.depthTextureBound ? m_stats.depthTextureIndex : UINT32_MAX;
    context.bindlessDescriptorSet = m_bindless.descriptorSetHandle();
    context.compositePipelineLayout = m_pipelineLayout->nativeHandle();
    context.compositeVertexBuffer = m_vertexBuffer;
    context.compositeTargetsSwapchain = presentActive && context.presentActive;
    if (m_stats.cudaTextureActive && m_cudaImage != nullptr) {
        context.cudaImage = m_cudaImage;
        context.cudaImageLayout = &m_cudaImageLayout;
    }

    if (presentActive && context.presentActive && context.presentFramebuffer != nullptr &&
        context.presentRenderPass != nullptr) {
        context.compositeRenderPass = context.presentRenderPass;
        context.compositeFramebuffer = context.presentFramebuffer;
        context.compositeWidth = context.presentWidth;
        context.compositeHeight = context.presentHeight;
        context.compositePipeline =
            m_presentPipeline != nullptr ? m_presentPipeline->nativeHandle()
                                         : m_offscreenPipeline->nativeHandle();
    } else if (context.active && m_outputFramebuffer != nullptr) {
        context.compositeRenderPass = m_offscreenRenderPass->nativeHandle();
        context.compositeFramebuffer = m_outputFramebuffer;
        context.compositeTargetLayout = &m_outputLayout;
        context.compositeWidth = m_desc.width > 0u ? m_desc.width : 1u;
        context.compositeHeight = m_desc.height > 0u ? m_desc.height : 1u;
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

    VkImageCreateInfo outputInfo{};
    outputInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    outputInfo.imageType = VK_IMAGE_TYPE_2D;
    outputInfo.format = static_cast<VkFormat>(kColorFormat);
    outputInfo.extent = {m_desc.width > 0u ? m_desc.width : 1u, m_desc.height > 0u ? m_desc.height : 1u, 1};
    outputInfo.mipLevels = 1;
    outputInfo.arrayLayers = 1;
    outputInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    outputInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    outputInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    outputInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    outputInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage outputImage = VK_NULL_HANDLE;
    if (vkCreateImage(vkDevice, &outputInfo, nullptr, &outputImage) != VK_SUCCESS) {
        m_stats.message = "composite output image creation failed";
        return false;
    }
    m_outputImage = outputImage;

    VkMemoryRequirements outputRequirements{};
    vkGetImageMemoryRequirements(vkDevice, outputImage, &outputRequirements);
    VkMemoryAllocateInfo outputAlloc{};
    outputAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    outputAlloc.allocationSize = outputRequirements.size;
    outputAlloc.memoryTypeIndex = findMemoryType(physicalDevice, outputRequirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory outputMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(vkDevice, &outputAlloc, nullptr, &outputMemory) != VK_SUCCESS) {
        m_stats.message = "composite output memory allocation failed";
        return false;
    }
    m_outputMemory = outputMemory;
    vkBindImageMemory(vkDevice, outputImage, outputMemory, 0);

    VkImageViewCreateInfo outputViewInfo{};
    outputViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    outputViewInfo.image = outputImage;
    outputViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    outputViewInfo.format = outputInfo.format;
    outputViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outputViewInfo.subresourceRange.levelCount = 1;
    outputViewInfo.subresourceRange.layerCount = 1;
    VkImageView outputView = VK_NULL_HANDLE;
    if (vkCreateImageView(vkDevice, &outputViewInfo, nullptr, &outputView) != VK_SUCCESS) {
        m_stats.message = "composite output view creation failed";
        return false;
    }
    m_outputView = outputView;

    VkFramebufferCreateInfo outputFramebufferInfo{};
    outputFramebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    outputFramebufferInfo.renderPass = static_cast<VkRenderPass>(m_offscreenRenderPass->nativeHandle());
    outputFramebufferInfo.attachmentCount = 1;
    outputFramebufferInfo.pAttachments = &outputView;
    outputFramebufferInfo.width = outputInfo.extent.width;
    outputFramebufferInfo.height = outputInfo.extent.height;
    outputFramebufferInfo.layers = 1;
    VkFramebuffer outputFramebuffer = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(vkDevice, &outputFramebufferInfo, nullptr, &outputFramebuffer) != VK_SUCCESS) {
        m_stats.message = "composite output framebuffer creation failed";
        return false;
    }
    m_outputFramebuffer = outputFramebuffer;

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
    if (m_device != nullptr && m_device->isValid()) {
        auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
        if (m_outputFramebuffer != nullptr) {
            vkDestroyFramebuffer(vkDevice, static_cast<VkFramebuffer>(m_outputFramebuffer), nullptr);
        }
        if (m_outputView != nullptr) {
            vkDestroyImageView(vkDevice, static_cast<VkImageView>(m_outputView), nullptr);
        }
        if (m_outputImage != nullptr) {
            vkDestroyImage(vkDevice, static_cast<VkImage>(m_outputImage), nullptr);
        }
        if (m_outputMemory != nullptr) {
            vkFreeMemory(vkDevice, static_cast<VkDeviceMemory>(m_outputMemory), nullptr);
        }
    }
    m_outputFramebuffer = nullptr;
    m_outputView = nullptr;
    m_outputImage = nullptr;
    m_outputMemory = nullptr;
    m_cudaImageLayout = 0;
    m_outputLayout = 0;
    m_sampler = nullptr;
    m_vertexBuffer = nullptr;
    m_vertexMemory = nullptr;
    m_rasterTextureSlot = {};
    m_depthTextureSlot = {};
    m_cudaTextureSlot = {};
    m_samplerSlot = {};
    if (m_device != nullptr && m_device->isValid()) {
        auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
        if (m_cudaImageView != nullptr) {
            vkDestroyImageView(vkDevice, static_cast<VkImageView>(m_cudaImageView), nullptr);
        }
        if (m_cudaImage != nullptr) {
            vkDestroyImage(vkDevice, static_cast<VkImage>(m_cudaImage), nullptr);
        }
        if (m_cudaImageMemory != nullptr) {
            vkFreeMemory(vkDevice, static_cast<VkDeviceMemory>(m_cudaImageMemory), nullptr);
        }
    }
    m_cudaImageView = nullptr;
    m_cudaImage = nullptr;
    m_cudaImageMemory = nullptr;
    m_cudaExportedHandle = nullptr;
    m_cudaAllocationSize = 0;
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
