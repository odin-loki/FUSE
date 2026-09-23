// B2 gate row: "Texture and buffer creation with all VMA memory types — verified with vkconfig
// overlay".
//
// GpuAllocator creates a buffer and a texture for every MemoryUsage (GpuOnly, CpuToGpu, GpuToCpu,
// CpuOnly). The memory-type property flags each allocation landed in are checked against the
// VMA usage contract (what the vkconfig overlay would show), host usages must come back mapped,
// and a GPU round trip proves every allocation is usable:
//   buffers : CpuToGpu --copy--> GpuOnly --copy--> GpuToCpu and CpuOnly   (host compare)
//   textures: CpuToGpu (linear) --copy--> GpuOnly (optimal) --copy--> GpuToCpu / CpuOnly (linear)
// A D32 depth texture must also get a valid (DEPTH-aspect) view. Destroying everything returns
// the allocator to zero live buffers/images/bytes.
#include "b5_rhi_test_common.hpp"

#include <fuse/core/init.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cstring>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

using b5rhi::expectTrue;
using fuse::u32;
using fuse::u8;
using namespace fuse::renderer;

#if defined(FUSE_VULKAN_BACKEND)
constexpr MemoryUsage kUsages[] = {MemoryUsage::GpuOnly, MemoryUsage::CpuToGpu, MemoryUsage::GpuToCpu,
                                   MemoryUsage::CpuOnly};
const char* const kUsageNames[] = {"GpuOnly", "CpuToGpu", "GpuToCpu", "CpuOnly"};
constexpr fuse::usize kBufferBytes = 64u * 1024u;
constexpr u32 kTexSize = 16u;

bool anyMemoryTypeHas(VkPhysicalDevice physical, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        if ((props.memoryTypes[i].propertyFlags & flags) == flags) {
            return true;
        }
    }
    return false;
}

void checkFlags(MemoryUsage usage, u32 flags, bool mapped, VkPhysicalDevice physical, const char* what) {
    constexpr u32 kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    char message[160];
    switch (usage) {
    case MemoryUsage::GpuOnly:
        std::snprintf(message, sizeof(message), "%s GpuOnly is DEVICE_LOCAL", what);
        expectTrue((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u, message);
        std::snprintf(message, sizeof(message), "%s GpuOnly is not mapped", what);
        expectTrue(!mapped, message);
        break;
    case MemoryUsage::CpuToGpu:
    case MemoryUsage::CpuOnly:
        std::snprintf(message, sizeof(message), "%s %s is HOST_VISIBLE|HOST_COHERENT and mapped", what,
                      usage == MemoryUsage::CpuToGpu ? "CpuToGpu" : "CpuOnly");
        expectTrue((flags & kHost) == kHost && mapped, message);
        break;
    case MemoryUsage::GpuToCpu:
        std::snprintf(message, sizeof(message), "%s GpuToCpu is HOST_VISIBLE|HOST_COHERENT and mapped", what);
        expectTrue((flags & kHost) == kHost && mapped, message);
        if (anyMemoryTypeHas(physical, kHost | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            std::snprintf(message, sizeof(message), "%s GpuToCpu prefers HOST_CACHED", what);
            expectTrue((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0u, message);
        }
        break;
    }
}

struct OneShot {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    bool begin(VulkanDevice& vulkan) {
        device = static_cast<VkDevice>(vulkan.nativeHandle());
        queue = static_cast<VkQueue>(vulkan.queues().graphics);
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = vulkan.queues().graphicsFamily;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &allocInfo, &cmd) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS;
    }

    bool submitAndWait() {
        VkMemoryBarrier hostRead{};
        hostRead.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostRead, 0,
                             nullptr, 0, nullptr);
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        const bool ok = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
                        vkQueueWaitIdle(queue) == VK_SUCCESS;
        vkDestroyCommandPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        return ok;
    }
};

void transferBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void imageLayout(VkCommandBuffer cmd, const Texture& texture, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = static_cast<VkImage>(texture.image);
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void copyImage(VkCommandBuffer cmd, const Texture& src, const Texture& dst) {
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {kTexSize, kTexSize, 1};
    vkCmdCopyImage(cmd, static_cast<VkImage>(src.image), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   static_cast<VkImage>(dst.image), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

u32 texel(u32 x, u32 y) {
    return 0xFF000000u | (x * 13u) << 16 | (y * 17u) << 8 | ((x ^ y) & 0xFFu);
}

void writeTexels(const Texture& texture) {
    for (u32 y = 0; y < kTexSize; ++y) {
        auto* row = reinterpret_cast<u32*>(static_cast<u8*>(texture.mapped) + y * texture.mappedRowPitch);
        for (u32 x = 0; x < kTexSize; ++x) {
            row[x] = texel(x, y);
        }
    }
}

bool texelsMatch(const Texture& texture) {
    for (u32 y = 0; y < kTexSize; ++y) {
        const auto* row =
            reinterpret_cast<const u32*>(static_cast<const u8*>(texture.mapped) + y * texture.mappedRowPitch);
        for (u32 x = 0; x < kTexSize; ++x) {
            if (row[x] != texel(x, y)) {
                return false;
            }
        }
    }
    return true;
}
#endif

} // namespace

int main() {
#if !defined(FUSE_VULKAN_BACKEND)
    return b5rhi::skip("fuse_b5_rhi_memory_types", "Vulkan backend disabled");
#else
    fuse::core::initialize();
    VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    bootstrapDesc.createFrameManager = false;
    auto bootstrap = VulkanBootstrap::create(bootstrapDesc);
    if (bootstrap == nullptr || !bootstrap->status().deviceReady) {
        bootstrap.reset();
        fuse::core::shutdown();
        return b5rhi::skip("fuse_b5_rhi_memory_types", "no Vulkan device (needs an ICD, Lavapipe in CI)");
    }
    VulkanDevice& device = *bootstrap->device();
    const auto physical = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    auto allocator = GpuAllocator::create(device);
    expectTrue(allocator != nullptr && allocator->isValid() && !allocator->isStub(), "real GPU allocator");
    std::printf("allocator mode: %s\n", allocator->info().message.c_str());
#if defined(FUSE_RHI_ALLOCATOR_VMA)
    // Default Vulkan build: the row is about VMA memory types, so it must run on the vendored VMA.
    expectTrue(allocator->info().mode == GpuAllocatorMode::Vma, "GpuAllocator runs on vendored VMA");
#else
    expectTrue(allocator->info().mode == GpuAllocatorMode::Native, "FUSE_RHI_USE_VMA=OFF: native allocator");
#endif

    // ---- Buffers ------------------------------------------------------------------------------
    Buffer buffers[4];
    for (u32 i = 0; i < 4; ++i) {
        BufferDesc desc{};
        desc.size = kBufferBytes;
        desc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::TransferSrc) |
                                              static_cast<u32>(BufferUsage::TransferDst) |
                                              static_cast<u32>(BufferUsage::Storage));
        desc.memoryUsage = kUsages[i];
        desc.name = kUsageNames[i];
        const bool created = allocator->createBuffer(desc, buffers[i]);
        expectTrue(created && buffers[i].handle != nullptr, "buffer created for every memory usage");
        std::printf("buffer %-8s flags 0x%x mapped %s\n", kUsageNames[i], buffers[i].memoryPropertyFlags,
                    buffers[i].mapped != nullptr ? "yes" : "no");
        checkFlags(kUsages[i], buffers[i].memoryPropertyFlags, buffers[i].mapped != nullptr, physical, "buffer");
    }

    std::vector<u32> pattern(kBufferBytes / sizeof(u32));
    for (fuse::usize i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<u32>(i * 2654435761u) ^ 0xA5A5A5A5u;
    }
    if (buffers[1].mapped != nullptr && buffers[2].mapped != nullptr && buffers[3].mapped != nullptr) {
        std::memcpy(buffers[1].mapped, pattern.data(), kBufferBytes);
        std::memset(buffers[2].mapped, 0, kBufferBytes);
        std::memset(buffers[3].mapped, 0, kBufferBytes);
        OneShot shot;
        expectTrue(shot.begin(device), "buffer copy command buffer");
        VkBufferCopy region{0, 0, kBufferBytes};
        vkCmdCopyBuffer(shot.cmd, static_cast<VkBuffer>(buffers[1].handle), static_cast<VkBuffer>(buffers[0].handle), 1,
                        &region);
        transferBarrier(shot.cmd);
        vkCmdCopyBuffer(shot.cmd, static_cast<VkBuffer>(buffers[0].handle), static_cast<VkBuffer>(buffers[2].handle), 1,
                        &region);
        vkCmdCopyBuffer(shot.cmd, static_cast<VkBuffer>(buffers[0].handle), static_cast<VkBuffer>(buffers[3].handle), 1,
                        &region);
        expectTrue(shot.submitAndWait(), "buffer round trip submitted");
        expectTrue(std::memcmp(buffers[2].mapped, pattern.data(), kBufferBytes) == 0,
                   "CpuToGpu -> GpuOnly -> GpuToCpu buffer round trip matches");
        expectTrue(std::memcmp(buffers[3].mapped, pattern.data(), kBufferBytes) == 0,
                   "CpuToGpu -> GpuOnly -> CpuOnly buffer round trip matches");
    }

    // ---- Textures -----------------------------------------------------------------------------
    Texture textures[4];
    for (u32 i = 0; i < 4; ++i) {
        TextureDesc desc{};
        desc.width = kTexSize;
        desc.height = kTexSize;
        desc.format = GpuFormat::R8G8B8A8Unorm;
        desc.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::TransferSrc) |
                                             static_cast<u32>(ImageUsage::TransferDst) |
                                             static_cast<u32>(ImageUsage::Sampled));
        desc.memoryUsage = kUsages[i];
        desc.name = kUsageNames[i];
        const bool created = allocator->createImage(desc, textures[i]);
        expectTrue(created && textures[i].image != nullptr && textures[i].view != nullptr,
                   "texture (+view) created for every memory usage");
        std::printf("texture %-8s flags 0x%x mapped %s rowPitch %llu\n", kUsageNames[i],
                    textures[i].memoryPropertyFlags, textures[i].mapped != nullptr ? "yes" : "no",
                    static_cast<unsigned long long>(textures[i].mappedRowPitch));
        checkFlags(kUsages[i], textures[i].memoryPropertyFlags, textures[i].mapped != nullptr, physical, "texture");
        if (kUsages[i] != MemoryUsage::GpuOnly) {
            expectTrue(textures[i].mappedRowPitch >= kTexSize * 4u, "linear texture reports a row pitch");
            expectTrue(textures[i].layout == VK_IMAGE_LAYOUT_PREINITIALIZED,
                       "host texture starts PREINITIALIZED (host writes survive the first transition)");
        }
    }

    if (textures[1].mapped != nullptr && textures[2].mapped != nullptr && textures[3].mapped != nullptr) {
        writeTexels(textures[1]);
        OneShot shot;
        expectTrue(shot.begin(device), "texture copy command buffer");
        imageLayout(shot.cmd, textures[1], VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        imageLayout(shot.cmd, textures[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyImage(shot.cmd, textures[1], textures[0]);
        imageLayout(shot.cmd, textures[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        imageLayout(shot.cmd, textures[2], VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        imageLayout(shot.cmd, textures[3], VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyImage(shot.cmd, textures[0], textures[2]);
        copyImage(shot.cmd, textures[0], textures[3]);
        imageLayout(shot.cmd, textures[2], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        imageLayout(shot.cmd, textures[3], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        expectTrue(shot.submitAndWait(), "texture round trip submitted");
        expectTrue(texelsMatch(textures[2]), "CpuToGpu -> GpuOnly -> GpuToCpu texture round trip matches");
        expectTrue(texelsMatch(textures[3]), "CpuToGpu -> GpuOnly -> CpuOnly texture round trip matches");
    }

    // Depth texture: the view must use the DEPTH aspect (a COLOR view of D32 is invalid).
    Texture depth;
    TextureDesc depthDesc{};
    depthDesc.width = kTexSize;
    depthDesc.height = kTexSize;
    depthDesc.format = GpuFormat::D32Sfloat;
    depthDesc.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::DepthStencilAttachment) |
                                              static_cast<u32>(ImageUsage::Sampled));
    depthDesc.name = "depth";
    expectTrue(allocator->createImage(depthDesc, depth) && depth.view != nullptr, "GpuOnly D32 texture + view");
    expectTrue((depth.memoryPropertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u, "depth is DEVICE_LOCAL");
    Texture hostDepth;
    depthDesc.memoryUsage = MemoryUsage::CpuToGpu;
    expectTrue(!allocator->createImage(depthDesc, hostDepth), "host-visible depth texture is rejected (no linear depth)");

    expectTrue(allocator->stats().bufferCount == 4u && allocator->stats().imageCount == 5u,
               "allocator tracks 4 buffers + 5 images");
    for (const Buffer& buffer : buffers) {
        expectTrue(buffer.allocationSize >= kBufferBytes, "buffer allocationSize covers the request");
    }
#if defined(FUSE_RHI_ALLOCATOR_VMA)
    expectTrue(allocator->stats().vmaPoolCount > 0u && allocator->stats().vmaPoolUsedBytes >= 4u * kBufferBytes,
               "VMA block statistics reflect the live allocations");
#endif
    for (Buffer& buffer : buffers) {
        allocator->destroyBuffer(buffer);
    }
    for (Texture& texture : textures) {
        allocator->destroyImage(texture);
    }
    allocator->destroyImage(depth);
    expectTrue(allocator->stats().bufferCount == 0u && allocator->stats().imageCount == 0u &&
                   allocator->stats().usedBytes == 0u,
               "no live allocations after destroy");

    allocator.reset();
    bootstrap.reset();
    fuse::core::shutdown();
    return b5rhi::finish("fuse_b5_rhi_memory_types");
#endif
}
