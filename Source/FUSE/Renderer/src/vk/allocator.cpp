#include <fuse/renderer/vk/allocator.hpp>

#include <cstring>
#include <utility>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

#if defined(FUSE_VMA_AVAILABLE)
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>
#endif

namespace fuse::renderer {

namespace {

#if defined(FUSE_VULKAN_BACKEND)
bool hasUsage(BufferUsage usage, BufferUsage flag) {
    return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0;
}

bool hasUsage(ImageUsage usage, ImageUsage flag) {
    return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0;
}

VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) {
    VkBufferUsageFlags flags = 0;
    if (hasUsage(usage, BufferUsage::TransferSrc)) {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (hasUsage(usage, BufferUsage::TransferDst)) {
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (hasUsage(usage, BufferUsage::Uniform)) {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (hasUsage(usage, BufferUsage::Storage)) {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (hasUsage(usage, BufferUsage::Index)) {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (hasUsage(usage, BufferUsage::Vertex)) {
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    return flags;
}

VkImageUsageFlags toVkImageUsage(ImageUsage usage) {
    VkImageUsageFlags flags = 0;
    if (hasUsage(usage, ImageUsage::TransferSrc)) {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (hasUsage(usage, ImageUsage::TransferDst)) {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (hasUsage(usage, ImageUsage::Sampled)) {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (hasUsage(usage, ImageUsage::Storage)) {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (hasUsage(usage, ImageUsage::ColorAttachment)) {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (hasUsage(usage, ImageUsage::DepthStencilAttachment)) {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    return flags;
}

VkFormat toVkFormat(GpuFormat format) {
    return static_cast<VkFormat>(static_cast<u32>(format));
}

#if defined(FUSE_VMA_AVAILABLE)
VmaMemoryUsage toVmaMemoryUsage(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::GpuOnly:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryUsage::CpuToGpu:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    case MemoryUsage::GpuToCpu:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    case MemoryUsage::CpuOnly:
        return VMA_MEMORY_USAGE_CPU_ONLY;
    }
    return VMA_MEMORY_USAGE_AUTO;
}
#endif
#endif

} // namespace

std::unique_ptr<GpuAllocator> GpuAllocator::create(VulkanDevice& device) {
    auto allocator = std::unique_ptr<GpuAllocator>(new GpuAllocator());
    if (!allocator->initialize(device)) {
        allocator->m_info.valid = false;
    }
    return allocator;
}

GpuAllocator::~GpuAllocator() {
    shutdown();
}

void* GpuAllocator::nativeHandle() const {
    return m_allocator;
}

bool GpuAllocator::initialize(VulkanDevice& device) {
    m_device = &device;

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
    if (!device.isValid()) {
        m_info.message = "GpuAllocator skipped — device not ready";
        return false;
    }

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    allocatorInfo.device = static_cast<VkDevice>(device.nativeHandle());
    allocatorInfo.instance = static_cast<VkInstance>(device.instanceHandle());

    VmaAllocator vmaAllocator = VK_NULL_HANDLE;
    if (vmaCreateAllocator(&allocatorInfo, &vmaAllocator) != VK_SUCCESS) {
        m_info.message = "vmaCreateAllocator failed";
        return false;
    }

    m_allocator = vmaAllocator;
    m_info.valid = true;
    m_info.mode = GpuAllocatorMode::Vma;
    m_info.message = "VMA allocator ready";
    device.setVmaAllocator(vmaAllocator);
    return true;
#elif defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "GpuAllocator stub — device not ready";
        return false;
    }
    m_info.valid = true;
    m_info.mode = GpuAllocatorMode::Stub;
    m_info.message = "Stub allocator (VMA header not available)";
    return true;
#else
    m_info.valid = true;
    m_info.mode = GpuAllocatorMode::Stub;
    m_info.message = "Stub allocator — Vulkan backend disabled";
    return true;
#endif
}

void GpuAllocator::shutdown() {
#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
    if (m_allocator != nullptr) {
        vmaDestroyAllocator(static_cast<VmaAllocator>(m_allocator));
        m_allocator = nullptr;
        if (m_device != nullptr) {
            m_device->setVmaAllocator(nullptr);
        }
    }
#endif
    m_device = nullptr;
}

bool GpuAllocator::createBuffer(const BufferDesc& desc, Buffer& out) {
    if (!m_info.valid || desc.size == 0) {
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = toVkBufferUsage(desc.usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = toVmaMemoryUsage(desc.memoryUsage);

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateBuffer(static_cast<VmaAllocator>(m_allocator), &bufferInfo, &allocInfo, &buffer,
                        &allocation, nullptr) != VK_SUCCESS) {
        return false;
    }

    out.handle = buffer;
    out.allocation = allocation;
    out.desc = desc;
    out.deviceAddress = 0;
    vmaMapMemory(static_cast<VmaAllocator>(m_allocator), allocation, &out.mapped);
    return true;
#elif defined(FUSE_VULKAN_BACKEND)
    (void)desc;
    out.handle = reinterpret_cast<void*>(m_stubId++);
    out.allocation = out.handle;
    out.desc = desc;
    out.mapped = nullptr;
    out.deviceAddress = 0;
    return true;
#else
    out.handle = reinterpret_cast<void*>(m_stubId++);
    out.allocation = out.handle;
    out.desc = desc;
    out.mapped = nullptr;
    out.deviceAddress = 0;
    return true;
#endif
}

void GpuAllocator::destroyBuffer(Buffer& buffer) {
#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
    if (m_allocator != nullptr && buffer.handle != nullptr) {
        if (buffer.mapped != nullptr) {
            vmaUnmapMemory(static_cast<VmaAllocator>(m_allocator),
                           static_cast<VmaAllocation>(buffer.allocation));
        }
        vmaDestroyBuffer(static_cast<VmaAllocator>(m_allocator),
                         static_cast<VkBuffer>(buffer.handle),
                         static_cast<VmaAllocation>(buffer.allocation));
    }
#else
    (void)buffer;
#endif
    buffer = Buffer{};
}

bool GpuAllocator::createImage(const TextureDesc& desc, Texture& out) {
    if (!m_info.valid || desc.width == 0 || desc.height == 0) {
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = desc.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.format = toVkFormat(desc.format);
    imageInfo.extent = {desc.width, desc.height, desc.depth};
    imageInfo.mipLevels = desc.mipLevels;
    imageInfo.arrayLayers = desc.arrayLayers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = toVkImageUsage(desc.usage);
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateImage(static_cast<VmaAllocator>(m_allocator), &imageInfo, &allocInfo, &image,
                       &allocation, nullptr) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = toVkFormat(desc.format);
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.layerCount = desc.arrayLayers;

    VkImageView view = VK_NULL_HANDLE;
    const VkDevice device = static_cast<VkDevice>(m_device->nativeHandle());
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        vmaDestroyImage(static_cast<VmaAllocator>(m_allocator), image, allocation);
        return false;
    }

    out.image = image;
    out.view = view;
    out.allocation = allocation;
    out.desc = desc;
    return true;
#elif defined(FUSE_VULKAN_BACKEND)
    out.image = reinterpret_cast<void*>(m_stubId++);
    out.view = reinterpret_cast<void*>(m_stubId++);
    out.allocation = out.image;
    out.desc = desc;
    return true;
#else
    out.image = reinterpret_cast<void*>(m_stubId++);
    out.view = reinterpret_cast<void*>(m_stubId++);
    out.allocation = out.image;
    out.desc = desc;
    return true;
#endif
}

void GpuAllocator::destroyImage(Texture& texture) {
#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
    if (m_device != nullptr && texture.image != nullptr) {
        const VkDevice device = static_cast<VkDevice>(m_device->nativeHandle());
        if (texture.view != nullptr) {
            vkDestroyImageView(device, static_cast<VkImageView>(texture.view), nullptr);
        }
        vmaDestroyImage(static_cast<VmaAllocator>(m_allocator), static_cast<VkImage>(texture.image),
                        static_cast<VmaAllocation>(texture.allocation));
    }
#else
    (void)texture;
#endif
    texture = Texture{};
}

} // namespace fuse::renderer
