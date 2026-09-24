#include <fuse/renderer/vk/allocator.hpp>

#include <fuse/renderer/vk/debug_utils.hpp>

#include <cstdint>
#include <cstdio>
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

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
// Vendored header-only VMA (Engine/lib/vma, pinned in Engine/lib/vma/VERSION); this TU holds the
// implementation. "Static" Vulkan functions: with volk (WP-0.2, vk/loader.hpp) every vk* name here
// is volk's function-pointer global, so vmaCreateAllocator copies the table that is current for
// the device (device-level dispatch, or loader trampolines while several devices are live) and
// every vkAllocateMemory / vkCreateBuffer / vkCreateImage VMA issues goes through the same
// pointers as the rest of fuse_rhi (layers, the b5 call-hook tests, which wrap them on reload).
// VMA_DYNAMIC_VULKAN_FUNCTIONS stays 0: nothing is fetched with vkGet*ProcAddr behind volk's back.
// VMA_VULKAN_VERSION matches the 1.2 instance (instance.cpp) so no 1.3+ entry points are referenced.
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_VULKAN_VERSION 1002000
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

// The header compiled here must be the release Engine/lib/vma/VERSION pins (CMake passes the pin).
static_assert(VMA_VERSION == VK_MAKE_VERSION(FUSE_VMA_PIN_MAJOR, FUSE_VMA_PIN_MINOR, FUSE_VMA_PIN_PATCH),
              "Engine/lib/vma/include/vk_mem_alloc.h does not match the version pinned in Engine/lib/vma/VERSION");
#endif

namespace fuse::renderer {

namespace {

void closeExportedHandle(void* handle);
bool bufferNeedsHostMapping(MemoryUsage usage);
[[maybe_unused]] const char* resolveDebugName(const char* name, const char* fallback);

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

bool isDepthFormat(GpuFormat format) {
    return format == GpuFormat::D32Sfloat;
}

/// Depth formats need a DEPTH-aspect view; a COLOR-aspect view of D32 is invalid.
VkImageAspectFlags imageAspectFor(GpuFormat format) {
    return isDepthFormat(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

bool textureNeedsHostMapping(const TextureDesc& desc) {
    return desc.memoryUsage != MemoryUsage::GpuOnly;
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

VkImageCreateInfo makeImageCreateInfo(const TextureDesc& desc) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = imageCreateFlags(desc);
    imageInfo.imageType = desc.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.format = toVkFormat(desc.format);
    imageInfo.extent = {desc.width, desc.height, desc.depth};
    imageInfo.mipLevels = desc.mipLevels;
    imageInfo.arrayLayers = desc.arrayLayers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    const bool hostVisible = textureNeedsHostMapping(desc);
    imageInfo.tiling = hostVisible ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = toVkImageUsage(desc.usage);
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // Linear host textures start PREINITIALIZED so texels the CPU writes before the first
    // transition survive it; optimal images start UNDEFINED.
    imageInfo.initialLayout = hostVisible ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_UNDEFINED;
    return imageInfo;
}

/// Linear tiling (host-mapped textures) is only guaranteed for single-mip, single-layer 2D colour
/// images, and per-format usage support must be queried.
bool linearImageSupported(VkPhysicalDevice physicalDevice, const VkImageCreateInfo& imageInfo,
                          const TextureDesc& desc) {
    if (imageInfo.imageType != VK_IMAGE_TYPE_2D || desc.mipLevels != 1u || desc.arrayLayers != 1u ||
        isDepthFormat(desc.format)) {
        return false;
    }
    VkImageFormatProperties formatProperties{};
    return vkGetPhysicalDeviceImageFormatProperties(physicalDevice, imageInfo.format, imageInfo.imageType,
                                                    imageInfo.tiling, imageInfo.usage, imageInfo.flags,
                                                    &formatProperties) == VK_SUCCESS;
}

/// Row pitch + offset of mip 0 / layer 0 of a linear image, for the persistently mapped pointer.
VkSubresourceLayout linearImageLayout(VkDevice device, VkImage image) {
    VkImageSubresource subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout{};
    vkGetImageSubresourceLayout(device, image, &subresource, &layout);
    return layout;
}

#if defined(FUSE_VMA_AVAILABLE)
/// MemoryUsage -> VMA allocation request. Host usages are persistently mapped (MAPPED_BIT) and
/// require HOST_COHERENT memory: FUSE writes/reads mapped pointers without explicit
/// vkFlushMappedMemoryRanges / vkInvalidateMappedMemoryRanges (same contract as the native path).
///   GpuOnly  -> AUTO_PREFER_DEVICE (DEVICE_LOCAL)
///   CpuToGpu -> AUTO + sequential-write host access (VMA picks BAR memory for non-staging buffers)
///   GpuToCpu -> AUTO_PREFER_HOST + random host access, preferring HOST_CACHED for fast CPU reads
///   CpuOnly  -> AUTO_PREFER_HOST + sequential-write host access (system memory)
VmaAllocationCreateInfo makeAllocationCreateInfo(MemoryUsage usage) {
    constexpr VkMemoryPropertyFlags kHost =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocationCreateInfo info{};
    switch (usage) {
    case MemoryUsage::GpuOnly:
        info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    case MemoryUsage::CpuToGpu:
        info.usage = VMA_MEMORY_USAGE_AUTO;
        info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        info.requiredFlags = kHost;
        break;
    case MemoryUsage::GpuToCpu:
        info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        info.requiredFlags = kHost;
        info.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    case MemoryUsage::CpuOnly:
        info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        info.requiredFlags = kHost;
        break;
    }
    return info;
}

/// Buffer through VMA. `enableExport` (CUDA interop) needs a whole exportable VkDeviceMemory, so it
/// goes through vmaCreateDedicatedBuffer with a VkExportMemoryAllocateInfo chain.
bool vmaCreateBufferImpl(VmaAllocator allocator, const VulkanDevice* vulkanDevice, const BufferDesc& desc,
                         bool enableExport, Buffer& out) {
    VkExternalMemoryBufferCreateInfo externalBufferInfo{};
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = resolveVkBufferUsage(vulkanDevice, desc.usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkExportMemoryAllocateInfo exportAllocInfo{};
    if (enableExport) {
        externalBufferInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        externalBufferInfo.handleTypes = platformExternalMemoryHandleType();
        bufferInfo.pNext = &externalBufferInfo;
        exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportAllocInfo.handleTypes = platformExternalMemoryHandleType();
    }

    const VmaAllocationCreateInfo allocCreateInfo = makeAllocationCreateInfo(desc.memoryUsage);
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocInfo{};
    const VkResult result =
        enableExport ? vmaCreateDedicatedBuffer(allocator, &bufferInfo, &allocCreateInfo, &exportAllocInfo, &buffer,
                                                &allocation, &allocInfo)
                     : vmaCreateBuffer(allocator, &bufferInfo, &allocCreateInfo, &buffer, &allocation, &allocInfo);
    if (result != VK_SUCCESS) {
        return false;
    }
    if (bufferNeedsHostMapping(desc.memoryUsage) && allocInfo.pMappedData == nullptr) {
        vmaDestroyBuffer(allocator, buffer, allocation);
        return false;
    }

    void* exported = nullptr;
    if (enableExport) {
        (void)exportDeviceMemoryHandle(static_cast<VkDevice>(vulkanDevice->nativeHandle()), allocInfo.deviceMemory,
                                       exported);
    }
    VkMemoryPropertyFlags memoryFlags = 0;
    vmaGetMemoryTypeProperties(allocator, allocInfo.memoryType, &memoryFlags);

    out.handle = buffer;
    out.allocation = allocation;
    out.desc = desc;
    out.mapped = bufferNeedsHostMapping(desc.memoryUsage) ? allocInfo.pMappedData : nullptr;
    out.deviceAddress = fetchBufferDeviceAddress(vulkanDevice, buffer, bufferInfo.usage);
    out.exportedHandle = exported;
    out.allocationSize = static_cast<u64>(allocInfo.size);
    out.memoryPropertyFlags = static_cast<u32>(memoryFlags);
    return true;
}

bool vmaCreateImageImpl(VmaAllocator allocator, VkDevice device, VkPhysicalDevice physicalDevice,
                        const TextureDesc& desc, bool enableExport, Texture& out) {
    VkImageCreateInfo imageInfo = makeImageCreateInfo(desc);
    const bool hostVisible = textureNeedsHostMapping(desc);
    if (hostVisible && !linearImageSupported(physicalDevice, imageInfo, desc)) {
        return false;
    }
    VkExternalMemoryImageCreateInfo externalImageInfo{};
    VkExportMemoryAllocateInfo exportAllocInfo{};
    if (enableExport) {
        externalImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalImageInfo.handleTypes = platformExternalMemoryHandleType();
        imageInfo.pNext = &externalImageInfo;
        exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exportAllocInfo.handleTypes = platformExternalMemoryHandleType();
    }

    const VmaAllocationCreateInfo allocCreateInfo = makeAllocationCreateInfo(desc.memoryUsage);
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocInfo{};
    const VkResult result =
        enableExport ? vmaCreateDedicatedImage(allocator, &imageInfo, &allocCreateInfo, &exportAllocInfo, &image,
                                               &allocation, &allocInfo)
                     : vmaCreateImage(allocator, &imageInfo, &allocCreateInfo, &image, &allocation, &allocInfo);
    if (result != VK_SUCCESS) {
        return false;
    }
    if (hostVisible && allocInfo.pMappedData == nullptr) {
        vmaDestroyImage(allocator, image, allocation);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = selectImageViewType(desc);
    viewInfo.format = toVkFormat(desc.format);
    viewInfo.subresourceRange.aspectMask = imageAspectFor(desc.format);
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.layerCount = desc.arrayLayers;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        vmaDestroyImage(allocator, image, allocation);
        return false;
    }

    void* exported = nullptr;
    if (enableExport) {
        (void)exportDeviceMemoryHandle(device, allocInfo.deviceMemory, exported);
    }
    VkMemoryPropertyFlags memoryFlags = 0;
    vmaGetMemoryTypeProperties(allocator, allocInfo.memoryType, &memoryFlags);

    out.image = image;
    out.view = view;
    out.allocation = allocation;
    out.desc = desc;
    out.exportedHandle = exported;
    out.allocationSize = static_cast<u64>(allocInfo.size);
    out.memoryPropertyFlags = static_cast<u32>(memoryFlags);
    out.mapped = nullptr;
    out.mappedRowPitch = 0;
    out.layout = 0;
    if (hostVisible) {
        // pMappedData points at the start of this allocation (VMA adds the block offset);
        // the subresource offset is relative to the image.
        const VkSubresourceLayout layout = linearImageLayout(device, image);
        out.mapped = static_cast<u8*>(allocInfo.pMappedData) + layout.offset;
        out.mappedRowPitch = static_cast<u64>(layout.rowPitch);
        out.layout = static_cast<u32>(VK_IMAGE_LAYOUT_PREINITIALIZED);
    }
    return true;
}
#else
u32 findMemoryType(VkPhysicalDevice physicalDevice, u32 typeFilter, VkMemoryPropertyFlags properties,
                   VkMemoryPropertyFlags* outFlags = nullptr) {
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (u32 i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            if (outFlags != nullptr) {
                *outFlags = memProperties.memoryTypes[i].propertyFlags;
            }
            return i;
        }
    }
    return UINT32_MAX;
}

/// Memory type for a MemoryUsage, most-preferred property set first (VMA usage semantics):
///   GpuOnly  -> DEVICE_LOCAL
///   CpuToGpu -> HOST_VISIBLE|COHERENT, preferring DEVICE_LOCAL (BAR) when the heap offers it
///   GpuToCpu -> HOST_VISIBLE|COHERENT, preferring HOST_CACHED for fast CPU reads
///   CpuOnly  -> HOST_VISIBLE|COHERENT, preferring non-device-local system memory
u32 selectMemoryType(VkPhysicalDevice physicalDevice, u32 typeFilter, MemoryUsage usage,
                     VkMemoryPropertyFlags* outFlags) {
    constexpr VkMemoryPropertyFlags kHost =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    switch (usage) {
    case MemoryUsage::GpuOnly:
        return findMemoryType(physicalDevice, typeFilter, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outFlags);
    case MemoryUsage::CpuToGpu: {
        const u32 bar = findMemoryType(physicalDevice, typeFilter, kHost | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                       outFlags);
        return bar != UINT32_MAX ? bar : findMemoryType(physicalDevice, typeFilter, kHost, outFlags);
    }
    case MemoryUsage::GpuToCpu: {
        const u32 cached =
            findMemoryType(physicalDevice, typeFilter, kHost | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, outFlags);
        return cached != UINT32_MAX ? cached : findMemoryType(physicalDevice, typeFilter, kHost, outFlags);
    }
    case MemoryUsage::CpuOnly: {
        VkPhysicalDeviceMemoryProperties memProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        for (u32 i = 0; i < memProperties.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags flags = memProperties.memoryTypes[i].propertyFlags;
            if ((typeFilter & (1u << i)) && (flags & kHost) == kHost &&
                (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
                if (outFlags != nullptr) {
                    *outFlags = flags;
                }
                return i;
            }
        }
        return findMemoryType(physicalDevice, typeFilter, kHost, outFlags);
    }
    }
    return UINT32_MAX;
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
    VkMemoryPropertyFlags memoryFlags = 0;
    const u32 memoryTypeIndex =
        selectMemoryType(physicalDevice, memRequirements.memoryTypeBits, desc.memoryUsage, &memoryFlags);
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
    out.memoryPropertyFlags = static_cast<u32>(memoryFlags);
    return true;
}

bool nativeCreateImage(VkDevice device, VkPhysicalDevice physicalDevice, const TextureDesc& desc,
                       bool enableExport, Texture& out) {
    VkImageCreateInfo imageInfo = makeImageCreateInfo(desc);
    const bool hostVisible = textureNeedsHostMapping(desc);
    if (hostVisible && !linearImageSupported(physicalDevice, imageInfo, desc)) {
        return false;
    }
    VkExternalMemoryImageCreateInfo externalImageInfo{};
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

    VkMemoryPropertyFlags memoryFlags = 0;
    const u32 memoryTypeIndex =
        selectMemoryType(physicalDevice, memRequirements.memoryTypeBits, desc.memoryUsage, &memoryFlags);
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
    viewInfo.subresourceRange.aspectMask = imageAspectFor(desc.format);
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.layerCount = desc.arrayLayers;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
        closeExportedHandle(exported);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
        return false;
    }

    void* mapped = nullptr;
    u64 rowPitch = 0;
    if (hostVisible) {
        const VkSubresourceLayout layout = linearImageLayout(device, image);
        void* base = nullptr;
        if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &base) != VK_SUCCESS) {
            vkDestroyImageView(device, view, nullptr);
            closeExportedHandle(exported);
            vkDestroyImage(device, image, nullptr);
            vkFreeMemory(device, memory, nullptr);
            return false;
        }
        mapped = static_cast<u8*>(base) + layout.offset;
        rowPitch = static_cast<u64>(layout.rowPitch);
    }

    out.image = image;
    out.view = view;
    out.allocation = memory;
    out.desc = desc;
    out.exportedHandle = exported;
    out.allocationSize = memRequirements.size;
    out.mapped = mapped;
    out.mappedRowPitch = rowPitch;
    out.memoryPropertyFlags = static_cast<u32>(memoryFlags);
    out.layout = hostVisible ? static_cast<u32>(VK_IMAGE_LAYOUT_PREINITIALIZED) : 0u;
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
    // CpuOnly is host memory too (staging / CPU-side scratch): it must be mapped like the others.
    return usage == MemoryUsage::CpuToGpu || usage == MemoryUsage::GpuToCpu || usage == MemoryUsage::CpuOnly;
}

[[maybe_unused]] const char* resolveDebugName(const char* name, const char* fallback) {
    return (name != nullptr && name[0] != '\0') ? name : fallback;
}

#if defined(FUSE_VULKAN_BACKEND)
// VkObjectType numeric: deviceMemory=8, buffer=9, image=10, imageView=14.
constexpr u32 kVkObjectTypeDeviceMemory = 8;
constexpr u32 kVkObjectTypeBuffer = 9;
constexpr u32 kVkObjectTypeImage = 10;
constexpr u32 kVkObjectTypeImageView = 14;

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

#if !defined(FUSE_VULKAN_BACKEND)
// Host-side stand-in for mapped buffers when the Vulkan backend is compiled out.
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
#endif

} // namespace

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_VMA_AVAILABLE)
/// VMA callbacks that need GpuAllocator internals (friend of GpuAllocator).
struct GpuAllocatorVmaAccess {
    /// Every VkDeviceMemory VMA allocates (pool blocks and dedicated allocations) gets a debug
    /// name at birth; dedicated allocations are renamed after their resource by nameMemory().
    static void VKAPI_PTR onDeviceMemoryAllocated(VmaAllocator /*allocator*/, uint32_t memoryType,
                                                  VkDeviceMemory memory, VkDeviceSize /*size*/, void* userData) {
        auto* self = static_cast<GpuAllocator*>(userData);
        if (self == nullptr || self->m_device == nullptr) {
            return;
        }
        char name[48];
        std::snprintf(name, sizeof(name), "fuse.gpu_alloc.vma_block.type%u", static_cast<unsigned>(memoryType));
        trySetDebugName(self->m_device->nativeHandle(), kVkObjectTypeDeviceMemory, reinterpret_cast<void*>(memory),
                        name, self->m_stats);
    }

    static void nameMemory(GpuAllocator& self, VmaAllocation allocation, const char* name) {
        VmaAllocationInfo2 info{};
        vmaGetAllocationInfo2(static_cast<VmaAllocator>(self.m_allocator), allocation, &info);
        if (info.dedicatedMemory == VK_TRUE) {
            trySetDebugName(self.m_device->nativeHandle(), kVkObjectTypeDeviceMemory,
                            reinterpret_cast<void*>(info.allocationInfo.deviceMemory), name, self.m_stats);
        }
    }
};
#endif

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

    // VMA has no per-allocator pool count: report the VkDeviceMemory blocks it owns (default
    // pools + dedicated allocations) and the bytes sub-allocated from them.
    VmaTotalStatistics totalStats{};
    vmaCalculateStatistics(static_cast<VmaAllocator>(m_allocator), &totalStats);
    m_stats.vmaPoolCount = totalStats.total.statistics.blockCount;
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

    // VMA must be told the API version the device actually runs (core 1.1/1.2 entry points and
    // dedicated-allocation / BDA handling depend on it), clamped to what this TU compiled for.
    u32 apiVersion = device.info().apiVersion != 0u ? device.info().apiVersion : VK_API_VERSION_1_0;
    if (apiVersion > VK_API_VERSION_1_2) {
        apiVersion = VK_API_VERSION_1_2;
    }
    VmaDeviceMemoryCallbacks memoryCallbacks{};
    memoryCallbacks.pfnAllocate = &GpuAllocatorVmaAccess::onDeviceMemoryAllocated;
    memoryCallbacks.pUserData = this;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.vulkanApiVersion = apiVersion;
    allocatorInfo.physicalDevice = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    allocatorInfo.device = static_cast<VkDevice>(device.nativeHandle());
    allocatorInfo.instance = static_cast<VkInstance>(device.instanceHandle());
    allocatorInfo.pDeviceMemoryCallbacks = &memoryCallbacks;
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
    m_info.message = "VMA allocator ready (vendored VMA " FUSE_VMA_VERSION_STRING ")";
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
    m_info.message = "Native Vulkan allocator (FUSE_RHI_USE_VMA=OFF)";
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
        if (m_device != nullptr && m_device->info().vmaAllocator == m_allocator) {
            m_device->setVmaAllocator(nullptr);
        }
        m_allocator = nullptr;
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
    const auto vma = static_cast<VmaAllocator>(m_allocator);
    // CUDA interop wants an exportable dedicated allocation; fall back to a plain (sub-)allocation
    // when the driver cannot export, exactly like the native path.
    if (!vmaCreateBufferImpl(vma, m_device, desc, desc.cudaInterop, out) &&
        (!desc.cudaInterop || !vmaCreateBufferImpl(vma, m_device, desc, false, out))) {
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }
    const char* bufferName = resolveDebugName(desc.name, "fuse.gpu_alloc.buffer");
    trySetDebugName(m_device->nativeHandle(), kVkObjectTypeBuffer, out.handle, bufferName, m_stats);
    GpuAllocatorVmaAccess::nameMemory(*this, static_cast<VmaAllocation>(out.allocation), bufferName);
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

    const char* bufferName = resolveDebugName(desc.name, "fuse.gpu_alloc.buffer");
    trySetDebugName(device, kVkObjectTypeBuffer, out.handle, bufferName, m_stats);
    trySetDebugName(device, kVkObjectTypeDeviceMemory, out.allocation, bufferName, m_stats);
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
        // Host allocations are persistently mapped (VMA_ALLOCATION_CREATE_MAPPED_BIT): VMA unmaps.
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
    const auto vma = static_cast<VmaAllocator>(m_allocator);
    const VkDevice device = static_cast<VkDevice>(m_device->nativeHandle());
    const VkPhysicalDevice physicalDevice = static_cast<VkPhysicalDevice>(m_device->nativePhysicalDevice());
    if (!vmaCreateImageImpl(vma, device, physicalDevice, desc, desc.cudaInterop, out) &&
        (!desc.cudaInterop || !vmaCreateImageImpl(vma, device, physicalDevice, desc, false, out))) {
        gpu_alloc_detail::recordFailedAlloc(m_stats);
        notifyStats();
        return false;
    }
    const char* imageName = resolveDebugName(desc.name, "fuse.gpu_alloc.image");
    trySetDebugName(device, kVkObjectTypeImage, out.image, imageName, m_stats);
    trySetDebugName(device, kVkObjectTypeImageView, out.view, imageName, m_stats);
    GpuAllocatorVmaAccess::nameMemory(*this, static_cast<VmaAllocation>(out.allocation), imageName);
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

    const char* imageName = resolveDebugName(desc.name, "fuse.gpu_alloc.image");
    trySetDebugName(device, kVkObjectTypeImage, out.image, imageName, m_stats);
    trySetDebugName(device, kVkObjectTypeImageView, out.view, imageName, m_stats);
    trySetDebugName(device, kVkObjectTypeDeviceMemory, out.allocation, imageName, m_stats);
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
        if (texture.mapped != nullptr && texture.allocation != nullptr) {
            vkUnmapMemory(device, static_cast<VkDeviceMemory>(texture.allocation));
        }
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
