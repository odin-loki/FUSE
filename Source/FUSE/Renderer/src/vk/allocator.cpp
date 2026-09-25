#include <fuse/renderer/vk/allocator.hpp>

#include <fuse/renderer/vk/debug_utils.hpp>

#include <cstdint>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

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

void closeExportedHandle(void* handle);
bool bufferNeedsHostMapping(MemoryUsage usage);

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
    if (hasUsage(usage, BufferUsage::ShaderDeviceAddress)) {
        flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (hasUsage(usage, BufferUsage::Indirect)) {
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    return flags;
}

bool deviceHasBufferDeviceAddress(const VulkanDevice* device) {
    return device != nullptr && device->info().bufferDeviceAddress;
}

bool usageWantsDeviceAddress(BufferUsage usage) {
    return hasUsage(usage, BufferUsage::ShaderDeviceAddress) ||
           hasUsage(usage, BufferUsage::Storage) || hasUsage(usage, BufferUsage::Uniform) ||
           hasUsage(usage, BufferUsage::Vertex);
}

VkBufferUsageFlags resolveVkBufferUsage(const VulkanDevice* device, BufferUsage usage) {
    VkBufferUsageFlags flags = toVkBufferUsage(usage);
    if (deviceHasBufferDeviceAddress(device) && usageWantsDeviceAddress(usage)) {
        flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    } else if (!deviceHasBufferDeviceAddress(device)) {
        flags &= ~static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    }
    return flags;
}

u64 fetchBufferDeviceAddress(const VulkanDevice* device, VkBuffer buffer,
                             VkBufferUsageFlags usageFlags) {
    if (!deviceHasBufferDeviceAddress(device) || buffer == VK_NULL_HANDLE ||
        (usageFlags & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) == 0) {
        return 0;
    }

    const VkDevice vkDevice = static_cast<VkDevice>(device->nativeHandle());
    if (vkDevice == VK_NULL_HANDLE) {
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

VkImageViewType selectImageViewType(const TextureDesc& desc) {
    if (desc.cubeMap && desc.arrayLayers >= 6) {
        return desc.arrayLayers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }
    if (desc.depth > 1) {
        return VK_IMAGE_VIEW_TYPE_3D;
    }
    if (desc.arrayLayers > 1) {
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

VkImageCreateFlags imageCreateFlags(const TextureDesc& desc) {
    return desc.cubeMap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
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
#else
u32 findMemoryType(VkPhysicalDevice physicalDevice, u32 typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (u32 i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

VkExternalMemoryHandleTypeFlagBits platformExternalMemoryHandleType() {
#if defined(_WIN32)
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
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

bool nativeCreateBuffer(const VulkanDevice* vulkanDevice, VkDevice device,
                        VkPhysicalDevice physicalDevice, const BufferDesc& desc, bool enableExport,
                        Buffer& out) {
    VkExternalMemoryBufferCreateInfo externalBufferInfo{};
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = resolveVkBufferUsage(vulkanDevice, desc.usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (enableExport) {
        externalBufferInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        externalBufferInfo.handleTypes = platformExternalMemoryHandleType();
        bufferInfo.pNext = &externalBufferInfo;
    }

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    const bool hostVisible = bufferNeedsHostMapping(desc.memoryUsage);
    const VkMemoryPropertyFlags memFlags =
        hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const u32 memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits, memFlags);
    if (memoryTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(device, buffer, nullptr);
        return false;
    }

    VkExportMemoryAllocateInfo exportAllocInfo{};
    VkMemoryAllocateFlagsInfo allocFlags{};
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    const void* next = nullptr;
    if (enableExport) {
        exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportAllocInfo.handleTypes = platformExternalMemoryHandleType();
        next = &exportAllocInfo;
    }
    if ((bufferInfo.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
        allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        allocFlags.pNext = next;
        next = &allocFlags;
    }
    allocInfo.pNext = next;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        return false;
    }

    if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        return false;
    }

    void* exported = nullptr;
    if (enableExport) {
        (void)exportDeviceMemoryHandle(device, memory, exported);
    }

    void* mapped = nullptr;
    if (hostVisible && vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        closeExportedHandle(exported);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
        return false;
    }

    out.handle = buffer;
    out.allocation = memory;
    out.desc = desc;
    out.mapped = mapped;
    out.deviceAddress = fetchBufferDeviceAddress(vulkanDevice, buffer, bufferInfo.usage);
    out.exportedHandle = exported;
    out.allocationSize = memRequirements.size;
    return true;
}

bool nativeCreateImage(VkDevice device, VkPhysicalDevice physicalDevice, const TextureDesc& desc,
                       bool enableExport, Texture& out) {
    VkExternalMemoryImageCreateInfo externalImageInfo{};
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = imageCreateFlags(desc);
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
    if (enableExport) {
        externalImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalImageInfo.handleTypes = platformExternalMemoryHandleType();
        imageInfo.pNext = &externalImageInfo;
    }

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(device, image, &memRequirements);

    const u32 memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryTypeIndex == UINT32_MAX) {
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    VkExportMemoryAllocateInfo exportAllocInfo{};
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (enableExport) {
        exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportAllocInfo.handleTypes = platformExternalMemoryHandleType();
        allocInfo.pNext = &exportAllocInfo;
    }

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    if (vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    void* exported = nullptr;
    if (enableExport) {
        (void)exportDeviceMemoryHandle(device, memory, exported);
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = selectImageViewType(desc);
    viewInfo.format = toVkFormat(desc.format);
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.layerCount = desc.arrayLayers;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        closeExportedHandle(exported);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        return false;
    }

    out.image = image;
    out.view = view;
    out.allocation = memory;
    out.desc = desc;
    out.exportedHandle = exported;
    out.allocationSize = memRequirements.size;
    return true;
}
#endif
#endif

void closeExportedHandle(void* handle) {
    if (handle == nullptr) {
        return;
    }
#if defined(_WIN32)
    CloseHandle(static_cast<HANDLE>(handle));
#else
    close(static_cast<int>(reinterpret_cast<intptr_t>(handle)));
#endif
}

bool bufferNeedsHostMapping(MemoryUsage usage) {
    return usage == MemoryUsage::CpuToGpu || usage == MemoryUsage::GpuToCpu;
}

#if defined(FUSE_VULKAN_BACKEND)
// VkObjectType numeric: deviceMemory=8, buffer=9, image=10.
constexpr u32 kVkObjectTypeDeviceMemory = 8;
constexpr u32 kVkObjectTypeBuffer = 9;
constexpr u32 kVkObjectTypeImage = 10;

void trySetDebugName(void* vkDevice, u32 vkObjectType, void* handle, const char* name,
                     GpuAllocStats& stats) {
    if (name == nullptr || handle == nullptr) {
        return;
    }
    const u64 objectHandle = static_cast<u64>(reinterpret_cast<uintptr_t>(handle));
    if (setDebugObjectName(vkDevice, vkObjectType, objectHandle, name)) {
        stats.debugNamesSet += 1;
    }
}
#endif

void* allocateStubMappedBuffer(usize size) {
    if (size == 0) {
        return nullptr;
    }
    auto* host = new u8[size]();
    return host;
}

void freeStubMappedBuffer(void* mapped) {
    if (mapped != nullptr) {
        delete[] static_cast<u8*>(mapped);
    }
}

} // namespace

void GpuAllocator::setStatsName(const char* name) {
    m_statsName = name != nullptr ? name : "gpu_allocator";
}

void GpuAllocator::notifyStats() const {
    notifyGpuStats(m_statsName, m_stats);
}

usize GpuAllocator::trackedBufferBytes(const Buffer& buffer) const {
    return buffer.desc.size;
}

usize GpuAllocator::trackedImageBytes(const Texture& texture) const {
    return gpu_alloc_detail::estimateImageBytes(texture.desc);
}

void GpuAllocator::refreshVmaPoolStats() {
#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
    if (m_allocator == nullptr || m_info.mode != GpuAllocatorMode::Vma) {
        m_stats.vmaPoolCount = 0;
        m_stats.vmaPoolUsedBytes = 0;
        return;
    }

    VmaTotalStatistics totalStats{};
    vmaCalculateStatistics(static_cast<VmaAllocator>(m_allocator), &totalStats);
    m_stats.vmaPoolCount = totalStats.poolCount;
    m_stats.vmaPoolUsedBytes = static_cast<usize>(totalStats.total.statistics.allocationBytes);
#else
    m_stats.vmaPoolCount = 0;
    m_stats.vmaPoolUsedBytes = 0;
#endif
}

void GpuAllocator::refreshBudget() {
    if (m_device == nullptr || !m_device->isValid() || m_info.mode == GpuAllocatorMode::Stub) {
        m_stats.deviceLocalHeapBytes = 0;
        m_stats.deviceLocalBudgetBytes = 0;
        m_stats.deviceLocalHeapIndex = 0;
        return;
    }

    gpu_alloc_detail::queryDeviceLocalHeapBudget(m_device->instanceHandle(),
                                                 m_device->nativePhysicalDevice(), m_stats);
}

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
    if (device.info().bufferDeviceAddress) {
        allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

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
    refreshBudget();
    return true;
#elif defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "GpuAllocator stub — device not ready";
        return false;
    }
    m_info.valid = true;
    m_info.mode = GpuAllocatorMode::Native;
    m_info.message = "Native Vulkan allocator (VMA header not available)";
    refreshBudget();
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
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = resolveVkBufferUsage(m_device, desc.usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = toVmaMemoryUsage(desc.memoryUsage);

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateBuffer(static_cast<VmaAllocator>(m_allocator), &bufferInfo, &allocInfo, &buffer,
                        &allocation, nullptr) != VK_SUCCESS) {
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }

    out.handle = buffer;
    out.allocation = allocation;
    out.desc = desc;
    out.deviceAddress = fetchBufferDeviceAddress(m_device, buffer, bufferInfo.usage);
    out.exportedHandle = nullptr;
    if (desc.cudaInterop) {
        out.allocationSize = desc.size;
    }
    vmaMapMemory(static_cast<VmaAllocator>(m_allocator), allocation, &out.mapped);
    trySetDebugName(m_device->nativeHandle(), kVkObjectTypeBuffer, out.handle, desc.name, m_stats);
    {
        VmaAllocationInfo vmaAllocInfo{};
        vmaGetAllocationInfo(static_cast<VmaAllocator>(m_allocator), allocation, &vmaAllocInfo);
        trySetDebugName(m_device->nativeHandle(), kVkObjectTypeDeviceMemory,
                        reinterpret_cast<void*>(vmaAllocInfo.deviceMemory), desc.name, m_stats);
    }
    gpu_alloc_detail::recordBufferAlloc(m_stats, desc.size);
    refreshVmaPoolStats();
    notifyStats();
    return true;
#elif defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr || !m_device->isValid()) {
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }

    const VkDevice device = static_cast<VkDevice>(m_device->nativeHandle());
    const VkPhysicalDevice physicalDevice =
        static_cast<VkPhysicalDevice>(m_device->nativePhysicalDevice());

    if (!nativeCreateBuffer(m_device, device, physicalDevice, desc, desc.cudaInterop, out) &&
        (!desc.cudaInterop ||
         !nativeCreateBuffer(m_device, device, physicalDevice, desc, false, out))) {
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }

    trySetDebugName(device, kVkObjectTypeBuffer, out.handle, desc.name, m_stats);
    trySetDebugName(device, kVkObjectTypeDeviceMemory, out.allocation, desc.name, m_stats);
    gpu_alloc_detail::recordBufferAlloc(m_stats, desc.size);
    notifyStats();
    return true;
#else
    out.handle = reinterpret_cast<void*>(m_stubId++);
    out.allocation = out.handle;
    out.desc = desc;
    out.mapped = bufferNeedsHostMapping(desc.memoryUsage) ? allocateStubMappedBuffer(desc.size)
                                                          : nullptr;
    out.deviceAddress = 0;
    out.exportedHandle = nullptr;
    out.allocationSize = desc.size;
    gpu_alloc_detail::recordBufferAlloc(m_stats, desc.size);
    notifyStats();
    return true;
#endif
}

void GpuAllocator::destroyBuffer(Buffer& buffer) {
    const usize bytes = trackedBufferBytes(buffer);
    closeExportedHandle(buffer.exportedHandle);
    buffer.exportedHandle = nullptr;
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
#elif defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && buffer.handle != nullptr) {
        const VkDevice device = static_cast<VkDevice>(m_device->nativeHandle());
        if (buffer.mapped != nullptr && buffer.allocation != nullptr) {
            vkUnmapMemory(device, static_cast<VkDeviceMemory>(buffer.allocation));
        }
        vkDestroyBuffer(device, static_cast<VkBuffer>(buffer.handle), nullptr);
        if (buffer.allocation != nullptr) {
            vkFreeMemory(device, static_cast<VkDeviceMemory>(buffer.allocation), nullptr);
        }
    }
#else
    freeStubMappedBuffer(buffer.mapped);
#endif
    if (bytes > 0) {
        gpu_alloc_detail::recordBufferFree(m_stats, bytes);
        refreshVmaPoolStats();
        notifyStats();
    }
    buffer = Buffer{};
}

bool GpuAllocator::readMapped(const Buffer& src, void* dst, usize size, usize srcOffset) const {
    if (src.mapped == nullptr || dst == nullptr || size == 0) {
        return false;
    }
    if (srcOffset > src.desc.size || size > src.desc.size - srcOffset) {
        return false;
    }
    std::memcpy(dst, static_cast<const u8*>(src.mapped) + srcOffset, size);
    return true;
}

bool GpuAllocator::createImage(const TextureDesc& desc, Texture& out) {
    if (!m_info.valid || desc.width == 0 || desc.height == 0) {
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }

    const usize imageBytes = gpu_alloc_detail::estimateImageBytes(desc);

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = imageCreateFlags(desc);
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
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = selectImageViewType(desc);
    viewInfo.format = toVkFormat(desc.format);
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.layerCount = desc.arrayLayers;

    VkImageView view = VK_NULL_HANDLE;
    const VkDevice device = static_cast<VkDevice>(m_device->nativeHandle());
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        vmaDestroyImage(static_cast<VmaAllocator>(m_allocator), image, allocation);
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }

    out.image = image;
    out.view = view;
    out.allocation = allocation;
    out.desc = desc;
    out.exportedHandle = nullptr;
    if (desc.cudaInterop) {
        out.allocationSize = imageBytes;
    }
    trySetDebugName(device, kVkObjectTypeImage, out.image, desc.name, m_stats);
    {
        VmaAllocationInfo vmaAllocInfo{};
        vmaGetAllocationInfo(static_cast<VmaAllocator>(m_allocator), allocation, &vmaAllocInfo);
        trySetDebugName(device, kVkObjectTypeDeviceMemory,
                        reinterpret_cast<void*>(vmaAllocInfo.deviceMemory), desc.name, m_stats);
    }
    gpu_alloc_detail::recordImageAlloc(m_stats, imageBytes);
    refreshVmaPoolStats();
    notifyStats();
    return true;
#elif defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr || !m_device->isValid()) {
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }

    const VkDevice device = static_cast<VkDevice>(m_device->nativeHandle());
    const VkPhysicalDevice physicalDevice =
        static_cast<VkPhysicalDevice>(m_device->nativePhysicalDevice());

    if (!nativeCreateImage(device, physicalDevice, desc, desc.cudaInterop, out) &&
        (!desc.cudaInterop || !nativeCreateImage(device, physicalDevice, desc, false, out))) {
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }

    trySetDebugName(device, kVkObjectTypeImage, out.image, desc.name, m_stats);
    trySetDebugName(device, kVkObjectTypeDeviceMemory, out.allocation, desc.name, m_stats);
    gpu_alloc_detail::recordImageAlloc(m_stats, imageBytes);
    notifyStats();
    return true;
#else
    out.image = reinterpret_cast<void*>(m_stubId++);
    out.view = reinterpret_cast<void*>(m_stubId++);
    out.allocation = out.image;
    out.desc = desc;
    out.exportedHandle = nullptr;
    out.allocationSize = imageBytes;
    gpu_alloc_detail::recordImageAlloc(m_stats, imageBytes);
    notifyStats();
    return true;
#endif
}

void GpuAllocator::destroyImage(Texture& texture) {
    const usize bytes = trackedImageBytes(texture);
    closeExportedHandle(texture.exportedHandle);
    texture.exportedHandle = nullptr;
#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
    if (m_device != nullptr && texture.image != nullptr) {
        const VkDevice device = static_cast<VkDevice>(m_device->nativeHandle());
        if (texture.view != nullptr) {
            vkDestroyImageView(device, static_cast<VkImageView>(texture.view), nullptr);
        }
        vmaDestroyImage(static_cast<VmaAllocator>(m_allocator), static_cast<VkImage>(texture.image),
                        static_cast<VmaAllocation>(texture.allocation));
    }
#elif defined(FUSE_VULKAN_BACKEND)
    if (m_device != nullptr && texture.image != nullptr) {
        const VkDevice device = static_cast<VkDevice>(m_device->nativeHandle());
        if (texture.view != nullptr) {
            vkDestroyImageView(device, static_cast<VkImageView>(texture.view), nullptr);
        }
        vkDestroyImage(device, static_cast<VkImage>(texture.image), nullptr);
        if (texture.allocation != nullptr) {
            vkFreeMemory(device, static_cast<VkDeviceMemory>(texture.allocation), nullptr);
        }
    }
#else
    (void)texture;
#endif
    if (bytes > 0) {
        gpu_alloc_detail::recordImageFree(m_stats, bytes);
        refreshVmaPoolStats();
        notifyStats();
    }
    texture = Texture{};
}

} // namespace fuse::renderer
