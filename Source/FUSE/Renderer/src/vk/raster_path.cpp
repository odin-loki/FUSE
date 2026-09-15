#include <fuse/renderer/vk/raster_path.hpp>

#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/graphics_pipeline.hpp>
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
    if (!m_stats.pipelineReady) {
        m_stats.message = "raster path not ready";
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && m_device->isValid()) {
        return recordFrameVulkan(commands);
    }
#endif
    return recordFrameStub(commands);
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

    PipelineLayoutDesc layoutDesc{};
    layoutDesc.debugName = "raster_path_layout";
    m_pipelineLayout = PipelineLayout::create(device, layoutDesc);
    if (m_pipelineLayout == nullptr || !m_pipelineLayout->isValid()) {
        m_stats.message = "pipeline layout creation failed";
        return false;
    }

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.layout = m_pipelineLayout.get();
    pipelineDesc.vertexShader = m_vertexShader.get();
    pipelineDesc.fragmentShader = m_fragmentShader.get();
    pipelineDesc.renderPass = m_renderPass.get();
    pipelineDesc.colorFormat = kColorFormat;
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

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device.queues().graphicsFamily;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        m_stats.message = "command pool creation failed";
        return false;
    }
    m_commandPool = commandPool;

    const std::array<float, 9> triangleVertices = {
        0.f, -0.5f, 0.f,
        0.5f, 0.5f, 0.f,
        -0.5f, 0.5f, 0.f,
    };

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(triangleVertices);
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(vkDevice, &bufferInfo, nullptr, &vertexBuffer) != VK_SUCCESS) {
        m_stats.message = "vertex buffer creation failed";
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
        m_stats.message = "vertex memory allocation failed";
        return false;
    }
    m_vertexMemory = vertexMemory;
    vkBindBufferMemory(vkDevice, vertexBuffer, vertexMemory, 0);

    void* mapped = nullptr;
    vkMapMemory(vkDevice, vertexMemory, 0, sizeof(triangleVertices), 0, &mapped);
    std::memcpy(mapped, triangleVertices.data(), sizeof(triangleVertices));
    vkUnmapMemory(vkDevice, vertexMemory);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {desc.width, desc.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = static_cast<VkFormat>(kColorFormat);
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage colorImage = VK_NULL_HANDLE;
    if (vkCreateImage(vkDevice, &imageInfo, nullptr, &colorImage) != VK_SUCCESS) {
        m_stats.message = "color image creation failed";
        return false;
    }
    m_colorImage = colorImage;

    vkGetImageMemoryRequirements(vkDevice, colorImage, &memRequirements);
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

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = static_cast<VkRenderPass>(m_renderPass->nativeHandle());
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &colorView;
    framebufferInfo.width = desc.width;
    framebufferInfo.height = desc.height;
    framebufferInfo.layers = 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(vkDevice, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS) {
        m_stats.message = "framebuffer creation failed";
        return false;
    }
    m_framebuffer = framebuffer;
#endif

    m_stats.pipelineReady = true;
    m_stats.message = "headless raster path ready";
    return true;
}

void RasterPath::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && m_device->isValid()) {
        auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
        if (m_framebuffer != nullptr) {
            vkDestroyFramebuffer(vkDevice, static_cast<VkFramebuffer>(m_framebuffer), nullptr);
        }
        if (m_colorView != nullptr) {
            vkDestroyImageView(vkDevice, static_cast<VkImageView>(m_colorView), nullptr);
        }
        if (m_colorImage != nullptr) {
            vkDestroyImage(vkDevice, static_cast<VkImage>(m_colorImage), nullptr);
        }
        if (m_colorMemory != nullptr) {
            vkFreeMemory(vkDevice, static_cast<VkDeviceMemory>(m_colorMemory), nullptr);
        }
        if (m_vertexBuffer != nullptr) {
            vkDestroyBuffer(vkDevice, static_cast<VkBuffer>(m_vertexBuffer), nullptr);
        }
        if (m_vertexMemory != nullptr) {
            vkFreeMemory(vkDevice, static_cast<VkDeviceMemory>(m_vertexMemory), nullptr);
        }
        if (m_commandPool != nullptr) {
            vkDestroyCommandPool(vkDevice, static_cast<VkCommandPool>(m_commandPool), nullptr);
        }
    }
    m_framebuffer = nullptr;
    m_colorView = nullptr;
    m_colorImage = nullptr;
    m_colorMemory = nullptr;
    m_vertexBuffer = nullptr;
    m_vertexMemory = nullptr;
    m_commandPool = nullptr;
#endif

    m_graphicsPipeline.reset();
    m_pipelineLayout.reset();
    m_fragmentShader.reset();
    m_vertexShader.reset();
    m_renderPass.reset();
    m_device = nullptr;
}

bool RasterPath::recordFrameStub(const RenderCommandList& commands) {
    m_stats.clearCount = 0;
    for (const RenderCommand& command : commands.commands()) {
        if (command.kind == RenderCommandKind::Clear3D) {
            ++m_stats.clearCount;
        }
    }
    m_stats.triangleDrawCount = m_stats.clearCount > 0 ? 1u : 0u;
    ++m_stats.framesRecorded;
    m_stats.message = "raster path recorded in stub mode";
    return true;
}

bool RasterPath::recordFrameVulkan(const RenderCommandList& commands) {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr || !m_device->isValid()) {
        return recordFrameStub(commands);
    }

    float clearR = 0.f;
    float clearG = 0.f;
    float clearB = 0.f;
    m_stats.clearCount = 0;
    for (const RenderCommand& command : commands.commands()) {
        if (command.kind == RenderCommandKind::Clear3D) {
            clearR = command.clear3D.r;
            clearG = command.clear3D.g;
            clearB = command.clear3D.b;
            ++m_stats.clearCount;
        }
    }

    auto vkDevice = static_cast<VkDevice>(m_device->nativeHandle());
    auto graphicsQueue = static_cast<VkQueue>(m_device->queues().graphics);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = static_cast<VkCommandPool>(m_commandPool);
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &commandBuffer) != VK_SUCCESS) {
        m_stats.message = "command buffer allocation failed";
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkClearValue clearValue{};
    clearValue.color = {{clearR, clearG, clearB, 1.f}};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = static_cast<VkRenderPass>(m_renderPass->nativeHandle());
    renderPassInfo.framebuffer = static_cast<VkFramebuffer>(m_framebuffer);
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_desc.width, m_desc.height};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      static_cast<VkPipeline>(m_graphicsPipeline->nativeHandle()));

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_desc.width);
    viewport.height = static_cast<float>(m_desc.height);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {m_desc.width, m_desc.height};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    VkBuffer vertexBuffers[] = {static_cast<VkBuffer>(m_vertexBuffer)};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkFreeCommandBuffers(vkDevice, static_cast<VkCommandPool>(m_commandPool), 1, &commandBuffer);

    m_stats.triangleDrawCount = 1;
    ++m_stats.framesRecorded;
    m_stats.message = "headless clear + triangle recorded";
    return true;
#else
    return recordFrameStub(commands);
#endif
}

} // namespace fuse::renderer
