#include <fuse/renderer/vk/raster_path.hpp>

#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/debug_utils.hpp>
#include <fuse/renderer/vk/graphics_pipeline.hpp>
#include <fuse/renderer/vk/pipeline_cache.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/render_pass.hpp>

#include <array>
#include <cstdint>
#include <cstring>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

constexpr u32 kColorFormat = 37; // VK_FORMAT_R8G8B8A8_UNORM
constexpr u32 kDepthFormat = 126; // VK_FORMAT_D32_SFLOAT / GpuFormat::D32Sfloat
constexpr u32 kMaterialPushConstantSize = 16u;
constexpr u32 kShaderStageVertex = 0x1u;
constexpr u32 kShaderStageFragment = 0x10u;

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

u64 queryBufferDeviceAddress(VulkanDevice& device, VkDevice vkDevice, VkBuffer buffer) {
    if (!device.info().bufferDeviceAddress || vkDevice == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE) {
        return 0;
    }

    auto getAddr = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
        vkGetDeviceProcAddr(vkDevice, "vkGetBufferDeviceAddress"));
    if (getAddr == nullptr) {
        getAddr = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
            vkGetDeviceProcAddr(vkDevice, "vkGetBufferDeviceAddressKHR"));
    }
    if (getAddr == nullptr) {
        return 0;
    }

    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return static_cast<u64>(getAddr(vkDevice, &info));
}
#endif

} // namespace

std::unique_ptr<RasterPath> RasterPath::create(VulkanDevice& device, const RasterPathDesc& desc) {
    auto path = std::unique_ptr<RasterPath>(new RasterPath());
    if (!path->initialize(device, desc)) {
        path->m_stats.pipelineReady = false;
    }
    return path;
}

RasterPath::~RasterPath() {
    shutdown();
}

bool RasterPath::recordFrame(const RenderCommandList& commands) {
    reloadPipelinesIfWatched();
    updateStatsFromCommands(commands);
    return m_stats.pipelineReady;
}

void RasterPath::reloadPipelinesIfWatched() {
    const u32 changed = m_shaderWatch.pollChanged();
    ++m_stats.shaderWatchPolls;
    if (changed == 0u) {
        return;
    }

    const bool vertexReloaded = m_vertexShader != nullptr && m_vertexShader->reloadFromDisk();
    const bool fragmentReloaded = m_fragmentShader != nullptr && m_fragmentShader->reloadFromDisk();
    if (!vertexReloaded && !fragmentReloaded) {
        return;
    }

    if (m_graphicsPipeline == nullptr || !m_graphicsPipeline->rebuild()) {
        return;
    }

    ++m_stats.pipelineReloadCount;
    if (m_vertexShader != nullptr && m_fragmentShader != nullptr) {
        const u64 vertexHash = m_vertexShader->info().spirvHash;
        const u64 fragmentHash = m_fragmentShader->info().spirvHash;
        m_stats.pipelineContentHash = vertexHash ^ (fragmentHash * 0x9E3779B97F4A7C15ull);
    }
}

void RasterPath::updateStatsFromCommands(const RenderCommandList& commands) {
    reloadPipelinesIfWatched();

    if (!m_stats.pipelineReady) {
        m_stats.message = "raster path not ready";
        return;
    }

    m_stats.clearCount = 0;
    for (const RenderCommand& command : commands.commands()) {
        if (command.kind == RenderCommandKind::Clear3D) {
            ++m_stats.clearCount;
        }
    }

    m_stats.triangleDrawCount = m_stats.clearCount > 0 ? 1u : 0u;
    ++m_stats.framesRecorded;
    m_stats.message = "raster stats mirrored — GPU encode via render graph";
}

VkFrameEncodeContext RasterPath::vulkanEncodeContext() const {
    VkFrameEncodeContext context{};
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.pipelineReady || m_device == nullptr || !m_device->isValid() || m_renderPass == nullptr ||
        m_graphicsPipeline == nullptr || m_framebuffer == nullptr || m_vertexBuffer == nullptr) {
        return context;
    }

    context.renderPass = m_renderPass->nativeHandle();
    context.framebuffer = m_framebuffer;
    context.graphicsPipeline = m_graphicsPipeline->nativeHandle();
    context.graphicsPipelineLayout =
        m_pipelineLayout != nullptr ? m_pipelineLayout->nativeHandle() : nullptr;
    context.vertexBuffer = m_vertexBuffer;
    if (m_indexBuffer != nullptr) {
        context.indexBuffer = m_indexBuffer;
        context.indexType = 0; // UINT16
    }
    context.width = m_desc.width;
    context.height = m_desc.height;
    context.active = context.renderPass != nullptr && context.framebuffer != nullptr &&
                     context.graphicsPipeline != nullptr && context.vertexBuffer != nullptr &&
                     context.width > 0u && context.height > 0u;
    context.barrierImage = m_colorImage;
    context.depthImage = m_depthImage;
    context.barrierImageLayout = &m_colorLayout;
    context.depthImageLayout = &m_depthLayout;
    context.depthView = m_depthView;
    if (m_bindless != nullptr && m_bindless->descriptorSetHandle() != nullptr) {
        context.bindlessDescriptorSet = m_bindless->descriptorSetHandle();
    }
#else
    (void)0;
#endif
    return context;
}

void* RasterPath::barrierImageHandle() const {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.pipelineReady || m_colorImage == nullptr) {
        return nullptr;
    }
    return m_colorImage;
#else
    return nullptr;
#endif
}

void* RasterPath::depthImageHandle() const {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.pipelineReady || m_depthImage == nullptr) {
        return nullptr;
    }
    return m_depthImage;
#else
    return nullptr;
#endif
}

void* RasterPath::depthViewHandle() const {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.pipelineReady || m_depthView == nullptr) {
        return nullptr;
    }
    return m_depthView;
#else
    return nullptr;
#endif
}

void* RasterPath::colorViewHandle() const {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.pipelineReady || m_colorView == nullptr) {
        return nullptr;
    }
    return m_colorView;
#else
    return nullptr;
#endif
}

void* RasterPath::indexBufferHandle() const {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_stats.indexBufferReady || m_indexBuffer == nullptr) {
        return nullptr;
    }
    return m_indexBuffer;
#else
    return nullptr;
#endif
}

bool RasterPath::initialize(VulkanDevice& device, const RasterPathDesc& desc) {
    m_device = &device;
    m_desc = desc;

    if (desc.vertexSpirvPath == nullptr || desc.fragmentSpirvPath == nullptr) {
        m_stats.message = "fixture SPIR-V paths required";
        return false;
    }

    RenderPassDesc renderPassDesc{};
    renderPassDesc.colorFormat = kColorFormat;
    renderPassDesc.depthFormat = kDepthFormat;
    renderPassDesc.debugName = "raster_path_render_pass";
    m_renderPass = RenderPass::create(device, renderPassDesc);
    if (m_renderPass == nullptr || !m_renderPass->isValid()) {
        m_stats.message = "render pass creation failed";
        return false;
    }

    m_vertexShader = ShaderModule::createFromFile(device, ShaderStage::Vertex, desc.vertexSpirvPath);
    m_fragmentShader =
        ShaderModule::createFromFile(device, ShaderStage::Fragment, desc.fragmentSpirvPath);
    if (m_vertexShader == nullptr || m_fragmentShader == nullptr || !m_vertexShader->isValid() ||
        !m_fragmentShader->isValid()) {
        m_stats.message = "fixture shader modules failed";
        return false;
    }

    const u64 vertexHash = m_vertexShader->info().spirvHash;
    const u64 fragmentHash = m_fragmentShader->info().spirvHash;
    m_stats.pipelineContentHash = vertexHash ^ (fragmentHash * 0x9E3779B97F4A7C15ull);

    m_shaderWatch.watch(desc.vertexSpirvPath);
    m_shaderWatch.watch(desc.fragmentSpirvPath);
    m_stats.shaderFilesWatched = m_shaderWatch.watchedCount();

    m_bindless = nullptr;
    m_ownedBindlessInitialized = false;
    if (desc.bindless != nullptr) {
        m_bindless = desc.bindless;
    } else {
        m_ownedBindless.init(device);
        m_ownedBindlessInitialized = true;
        if (m_ownedBindless.vulkanDescriptorsReady()) {
            m_bindless = &m_ownedBindless;
        } else {
            m_ownedBindless.destroy(device);
            m_ownedBindlessInitialized = false;
            m_bindless = nullptr;
        }
    }

    PipelineLayoutDesc layoutDesc{};
    layoutDesc.debugName = "raster_path_layout";
    layoutDesc.pushConstants.push_back(
        {0, kMaterialPushConstantSize, kShaderStageVertex | kShaderStageFragment});
    if (m_bindless != nullptr && m_bindless->layoutHandle() != nullptr) {
        layoutDesc.bindlessSetLayout = m_bindless->layoutHandle();
    }
    m_pipelineLayout = PipelineLayout::create(device, layoutDesc);
    if (m_pipelineLayout == nullptr || !m_pipelineLayout->isValid()) {
        m_stats.message = "pipeline layout creation failed";
        return false;
    }
    m_stats.bindlessLayoutReady = m_pipelineLayout->info().hasBindlessSet;

    m_pipelineCache = PipelineCache::create(device);
    if (m_pipelineCache == nullptr || !m_pipelineCache->isValid()) {
        m_stats.message = "pipeline cache creation failed";
        return false;
    }

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.layout = m_pipelineLayout.get();
    pipelineDesc.vertexShader = m_vertexShader.get();
    pipelineDesc.fragmentShader = m_fragmentShader.get();
    pipelineDesc.renderPass = m_renderPass.get();
    pipelineDesc.pipelineCache = m_pipelineCache.get();
    pipelineDesc.colorFormat = kColorFormat;
    pipelineDesc.depthTest = true;
    pipelineDesc.depthWrite = true;
    pipelineDesc.depthCompareOp = 1; // VK_COMPARE_OP_LESS
    pipelineDesc.debugName = "raster_path_pipeline";
    m_graphicsPipeline = GraphicsPipeline::create(device, pipelineDesc);
    if (m_graphicsPipeline == nullptr || !m_graphicsPipeline->isValid()) {
        m_stats.message = m_graphicsPipeline != nullptr ? m_graphicsPipeline->info().message
                                                        : "graphics pipeline allocation failed";
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_stats.pipelineReady = true;
        m_stats.message = "graphics pipeline scaffold ready (stub device)";
        return true;
    }

    auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    auto physicalDevice = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());

    const std::array<float, 9> triangleVertices = {
        0.f, -0.5f, 0.f,
        0.5f, 0.5f, 0.f,
        -0.5f, 0.5f, 0.f,
    };

    const bool wantBufferDeviceAddress = device.info().bufferDeviceAddress;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(triangleVertices);
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (wantBufferDeviceAddress) {
        bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(vkDevice, &bufferInfo, nullptr, &vertexBuffer) != VK_SUCCESS) {
        m_stats.message = "vertex buffer creation failed";
        return false;
    }
    m_vertexBuffer = vertexBuffer;
    setDebugObjectName(vkDevice, 9u,
                       static_cast<u64>(reinterpret_cast<uintptr_t>(static_cast<void*>(vertexBuffer))),
                       "fuse.raster.vb");

    VkMemoryRequirements memRequirements{};
    vkGetBufferMemoryRequirements(vkDevice, vertexBuffer, &memRequirements);
    VkMemoryAllocateFlagsInfo vertexAllocFlags{};
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
        findMemoryType(physicalDevice, memRequirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (wantBufferDeviceAddress) {
        vertexAllocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        vertexAllocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        allocInfo.pNext = &vertexAllocFlags;
    }

    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &vertexMemory) != VK_SUCCESS) {
        m_stats.message = "vertex memory allocation failed";
        return false;
    }
    m_vertexMemory = vertexMemory;
    vkBindBufferMemory(vkDevice, vertexBuffer, vertexMemory, 0);
    m_stats.vertexDeviceAddress = queryBufferDeviceAddress(device, vkDevice, vertexBuffer);

    void* mapped = nullptr;
    vkMapMemory(vkDevice, vertexMemory, 0, sizeof(triangleVertices), 0, &mapped);
    std::memcpy(mapped, triangleVertices.data(), sizeof(triangleVertices));
    vkUnmapMemory(vkDevice, vertexMemory);

    const std::array<std::uint16_t, 3> triangleIndices = {0, 1, 2};

    VkBufferCreateInfo indexBufferInfo{};
    indexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    indexBufferInfo.size = sizeof(triangleIndices);
    indexBufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (wantBufferDeviceAddress) {
        indexBufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    indexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer indexBuffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(vkDevice, &indexBufferInfo, nullptr, &indexBuffer) == VK_SUCCESS) {
        setDebugObjectName(vkDevice, 9u,
                           static_cast<u64>(reinterpret_cast<uintptr_t>(static_cast<void*>(indexBuffer))),
                           "fuse.raster.ib");
        VkMemoryRequirements indexMemRequirements{};
        vkGetBufferMemoryRequirements(vkDevice, indexBuffer, &indexMemRequirements);
        VkMemoryAllocateFlagsInfo indexAllocFlags{};
        VkMemoryAllocateInfo indexAllocInfo{};
        indexAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        indexAllocInfo.allocationSize = indexMemRequirements.size;
        indexAllocInfo.memoryTypeIndex = findMemoryType(
            physicalDevice, indexMemRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (wantBufferDeviceAddress) {
            indexAllocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
            indexAllocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            indexAllocInfo.pNext = &indexAllocFlags;
        }

        VkDeviceMemory indexMemory = VK_NULL_HANDLE;
        if (vkAllocateMemory(vkDevice, &indexAllocInfo, nullptr, &indexMemory) == VK_SUCCESS) {
            m_indexBuffer = indexBuffer;
            m_indexMemory = indexMemory;
            vkBindBufferMemory(vkDevice, indexBuffer, indexMemory, 0);
            m_stats.indexDeviceAddress = queryBufferDeviceAddress(device, vkDevice, indexBuffer);

            void* indexMapped = nullptr;
            vkMapMemory(vkDevice, indexMemory, 0, sizeof(triangleIndices), 0, &indexMapped);
            std::memcpy(indexMapped, triangleIndices.data(), sizeof(triangleIndices));
            vkUnmapMemory(vkDevice, indexMemory);
        } else {
            vkDestroyBuffer(vkDevice, indexBuffer, nullptr);
        }
    }
    m_stats.indexBufferReady = (m_indexBuffer != nullptr);
    m_stats.bufferDeviceAddressReady = (m_stats.vertexDeviceAddress != 0);

    if (!createOffscreenTargets()) {
        return false;
    }
#endif

    m_stats.pipelineReady = true;
    m_stats.message = "headless raster path ready";
    return true;
}

bool RasterPath::resize(u32 width, u32 height) {
    if (!m_stats.pipelineReady || width == 0u || height == 0u) {
        return false;
    }
    if (width == m_desc.width && height == m_desc.height) {
        ++m_stats.resizeNoOpCount;
        return true;
    }

    m_desc.width = width;
    m_desc.height = height;
    destroyOffscreenTargets();
    if (!createOffscreenTargets()) {
        return false;
    }
    ++m_stats.resizeCount;
    m_stats.message = "raster path resized";
    return true;
}

bool RasterPath::createOffscreenTargets() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr || !m_device->isValid()) {
        return true;
    }
    if (m_renderPass == nullptr || m_renderPass->nativeHandle() == nullptr) {
        m_stats.message = "render pass required for offscreen targets";
        return false;
    }

    auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
    auto physicalDevice = static_cast<VkPhysicalDevice>(m_device->nativePhysicalDevice());

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {m_desc.width, m_desc.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = static_cast<VkFormat>(kColorFormat);
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage colorImage = VK_NULL_HANDLE;
    if (vkCreateImage(vkDevice, &imageInfo, nullptr, &colorImage) != VK_SUCCESS) {
        m_stats.message = "color image creation failed";
        return false;
    }
    m_colorImage = colorImage;
    setDebugObjectName(vkDevice, 10u,
                       static_cast<u64>(reinterpret_cast<uintptr_t>(static_cast<void*>(colorImage))),
                       "fuse.raster.color");

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(vkDevice, colorImage, &memRequirements);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory colorMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &colorMemory) != VK_SUCCESS) {
        m_stats.message = "color image memory allocation failed";
        return false;
    }
    m_colorMemory = colorMemory;
    vkBindImageMemory(vkDevice, colorImage, colorMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = colorImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = static_cast<VkFormat>(kColorFormat);
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView colorView = VK_NULL_HANDLE;
    if (vkCreateImageView(vkDevice, &viewInfo, nullptr, &colorView) != VK_SUCCESS) {
        m_stats.message = "color image view creation failed";
        return false;
    }
    m_colorView = colorView;

    VkImageCreateInfo depthImageInfo{};
    depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
    depthImageInfo.extent = {m_desc.width, m_desc.height, 1};
    depthImageInfo.mipLevels = 1;
    depthImageInfo.arrayLayers = 1;
    depthImageInfo.format = static_cast<VkFormat>(kDepthFormat);
    depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Sampled: composite binds raster depth in the bindless heap (B2.9).
    depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage depthImage = VK_NULL_HANDLE;
    if (vkCreateImage(vkDevice, &depthImageInfo, nullptr, &depthImage) != VK_SUCCESS) {
        m_stats.message = "depth image creation failed";
        return false;
    }
    m_depthImage = depthImage;
    setDebugObjectName(vkDevice, 10u,
                       static_cast<u64>(reinterpret_cast<uintptr_t>(static_cast<void*>(depthImage))),
                       "fuse.raster.depth");

    vkGetImageMemoryRequirements(vkDevice, depthImage, &memRequirements);
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &depthMemory) != VK_SUCCESS) {
        m_stats.message = "depth image memory allocation failed";
        return false;
    }
    m_depthMemory = depthMemory;
    vkBindImageMemory(vkDevice, depthImage, depthMemory, 0);

    VkImageViewCreateInfo depthViewInfo{};
    depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image = depthImage;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = static_cast<VkFormat>(kDepthFormat);
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.baseMipLevel = 0;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.baseArrayLayer = 0;
    depthViewInfo.subresourceRange.layerCount = 1;

    VkImageView depthView = VK_NULL_HANDLE;
    if (vkCreateImageView(vkDevice, &depthViewInfo, nullptr, &depthView) != VK_SUCCESS) {
        m_stats.message = "depth image view creation failed";
        return false;
    }
    m_depthView = depthView;

    const VkImageView framebufferAttachments[2] = {colorView, depthView};

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = static_cast<VkRenderPass>(m_renderPass->nativeHandle());
    framebufferInfo.attachmentCount = 2;
    framebufferInfo.pAttachments = framebufferAttachments;
    framebufferInfo.width = m_desc.width;
    framebufferInfo.height = m_desc.height;
    framebufferInfo.layers = 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(vkDevice, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS) {
        m_stats.message = "framebuffer creation failed";
        return false;
    }
    m_framebuffer = framebuffer;
    m_stats.depthAttachmentReady = true;
#else
    (void)0;
#endif
    return true;
}

void RasterPath::destroyOffscreenTargets() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && m_device->isValid()) {
        auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
        if (m_framebuffer != nullptr) {
            vkDestroyFramebuffer(vkDevice, static_cast<VkFramebuffer>(m_framebuffer), nullptr);
        }
        if (m_colorView != nullptr) {
            vkDestroyImageView(vkDevice, static_cast<VkImageView>(m_colorView), nullptr);
        }
        if (m_depthView != nullptr) {
            vkDestroyImageView(vkDevice, static_cast<VkImageView>(m_depthView), nullptr);
        }
        if (m_colorImage != nullptr) {
            vkDestroyImage(vkDevice, static_cast<VkImage>(m_colorImage), nullptr);
        }
        if (m_depthImage != nullptr) {
            vkDestroyImage(vkDevice, static_cast<VkImage>(m_depthImage), nullptr);
        }
        if (m_colorMemory != nullptr) {
            vkFreeMemory(vkDevice, static_cast<VkDeviceMemory>(m_colorMemory), nullptr);
        }
        if (m_depthMemory != nullptr) {
            vkFreeMemory(vkDevice, static_cast<VkDeviceMemory>(m_depthMemory), nullptr);
        }
    }
    m_framebuffer = nullptr;
    m_colorView = nullptr;
    m_depthView = nullptr;
    m_colorImage = nullptr;
    m_depthImage = nullptr;
    m_colorLayout = 0;
    m_depthLayout = 0;
    m_colorMemory = nullptr;
    m_depthMemory = nullptr;
    m_stats.depthAttachmentReady = false;
#endif
}

void RasterPath::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    destroyOffscreenTargets();
    if (m_device != nullptr && m_device->isValid()) {
        auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
        if (m_indexBuffer != nullptr) {
            vkDestroyBuffer(vkDevice, static_cast<VkBuffer>(m_indexBuffer), nullptr);
        }
        if (m_indexMemory != nullptr) {
            vkFreeMemory(vkDevice, static_cast<VkDeviceMemory>(m_indexMemory), nullptr);
        }
        if (m_vertexBuffer != nullptr) {
            vkDestroyBuffer(vkDevice, static_cast<VkBuffer>(m_vertexBuffer), nullptr);
        }
        if (m_vertexMemory != nullptr) {
            vkFreeMemory(vkDevice, static_cast<VkDeviceMemory>(m_vertexMemory), nullptr);
        }
    }
    m_indexBuffer = nullptr;
    m_indexMemory = nullptr;
    m_vertexBuffer = nullptr;
    m_vertexMemory = nullptr;
#endif

    m_pipelineCache.reset();
    m_graphicsPipeline.reset();
    m_pipelineLayout.reset();
    m_fragmentShader.reset();
    m_vertexShader.reset();
    m_renderPass.reset();

    if (m_ownedBindlessInitialized && m_device != nullptr) {
        m_ownedBindless.destroy(*m_device);
        m_ownedBindlessInitialized = false;
    }
    m_bindless = nullptr;
    m_device = nullptr;
}

bool RasterPath::readbackColor(std::vector<u8>& outRgba) const {
    outRgba.clear();
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr || !m_device->isValid() || m_colorImage == nullptr || m_desc.width == 0u ||
        m_desc.height == 0u) {
        return false;
    }

    auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
    auto physicalDevice = static_cast<VkPhysicalDevice>(m_device->nativePhysicalDevice());
    auto queue = static_cast<VkQueue>(m_device->queues().graphics);
    if (queue == VK_NULL_HANDLE) {
        return false;
    }
    m_device->waitIdle();

    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(m_desc.width) * m_desc.height * 4u;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool ok = false;

    do {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = byteCount;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vkDevice, &bufferInfo, nullptr, &staging) != VK_SUCCESS) {
            break;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(vkDevice, staging, &requirements);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex =
            findMemoryType(physicalDevice, requirements.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
            break;
        }
        vkBindBufferMemory(vkDevice, staging, stagingMemory, 0);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = m_device->queues().graphicsFamily;
        if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            break;
        }
        VkCommandBufferAllocateInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandPool = pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(vkDevice, &cmdInfo, &cmd) != VK_SUCCESS) {
            break;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Tracked layout from the last recorded frame; UNDEFINED means nothing rendered yet.
        const auto restoreLayout = static_cast<VkImageLayout>(m_colorLayout);
        VkImageMemoryBarrier toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.oldLayout = restoreLayout;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = static_cast<VkImage>(m_colorImage);
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toSrc);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {m_desc.width, m_desc.height, 1};
        vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(m_colorImage), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging, 1, &region);

        const VkImageLayout finalLayout =
            restoreLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : restoreLayout;
        if (finalLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            VkImageMemoryBarrier back = toSrc;
            back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            back.newLayout = finalLayout;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &back);
        }
        VkBufferMemoryBarrier hostRead{};
        hostRead.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostRead.buffer = staging;
        hostRead.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &hostRead, 0, nullptr);
        vkEndCommandBuffer(cmd);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(vkDevice, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            break;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
            vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            break;
        }
        m_colorLayout = static_cast<u32>(finalLayout);

        void* mapped = nullptr;
        if (vkMapMemory(vkDevice, stagingMemory, 0, byteCount, 0, &mapped) != VK_SUCCESS) {
            break;
        }
        outRgba.resize(static_cast<usize>(byteCount));
        std::memcpy(outRgba.data(), mapped, outRgba.size());
        vkUnmapMemory(vkDevice, stagingMemory);
        ok = true;
    } while (false);

    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(vkDevice, fence, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vkDevice, pool, nullptr);
    }
    if (staging != VK_NULL_HANDLE) {
        vkDestroyBuffer(vkDevice, staging, nullptr);
    }
    if (stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(vkDevice, stagingMemory, nullptr);
    }
    return ok;
#else
    return false;
#endif
}

} // namespace fuse::renderer
