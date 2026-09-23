// B2.11 gate 3.4 follow-up: the async upload queue's cross-queue-family path.
//
// With `--layer-dir <dir>` the test loads VK_LAYER_FUSE_split_transfer_family (built next to it)
// *below* VK_LAYER_KHRONOS_validation, so on Lavapipe the device reports a transfer-only family 1
// and the UploadQueue runs its release/acquire path for real, as seen by the validator. Without
// it the same scenarios run on the single-family (same-queue barrier) path.
//
// Everything runs under validation with synchronization validation on; any validation message
// fails the test. Scenarios:
//   A  buffers + a 7-mip x 3-layer image + a single-mip image, the same buffer and the same image
//      written twice in one batch; graphics-queue readback submitted right after flush (no CPU
//      wait) so the acquire must precede first use in queue order.
//   B  partial re-upload of a live buffer and full re-upload of the image while the previous
//      graphics readback is still queued (WAR across queues; bytes outside the patch preserved).
//   C  many batches in flight through a small ring (wraps) interleaved with graphics reads.
// Then negative controls prove the checks are live: a WAW hazard syncval must report and, in split
// mode, an acquire with no matching release and an unsynchronized transfer -> graphics read.
//
// Exit 77 (skip) in the stub build, without an ICD, or without the validation layer.
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

#if defined(_WIN32)
// The Windows CRT has no POSIX setenv; _putenv_s updates the CRT and process environment
// (what the Vulkan loader reads through getenv at vkCreateInstance time).
[[maybe_unused]] int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr int kSkip = 77;

#if defined(FUSE_VULKAN_BACKEND)

using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::renderer::UploadImageDesc;
using fuse::renderer::UploadQueue;
using fuse::renderer::UploadTicket;

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kSplitLayer = "VK_LAYER_FUSE_split_transfer_family";
constexpr u64 kTimeoutNs = 2000000000ull;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- validation message capture ---------------------------------------------------------------

struct MessageLog {
    u32 count = 0;
    bool quiet = false; ///< negative controls: record ids, do not print the full message
    std::vector<std::string> ids;
};

MessageLog g_log;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_log.count;
    const char* id = data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)";
    g_log.ids.emplace_back(id);
    if (!g_log.quiet) {
        std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n", id,
                     data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    }
    return VK_FALSE;
}

bool layerAvailable(const char* name) {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

void setEnv(const char* name, const std::string& value, bool overwrite) {
    setenv(name, value.c_str(), overwrite ? 1 : 0);
}

// --- raw Vulkan resources ---------------------------------------------------------------------

struct Gpu {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics = VK_NULL_HANDLE;
    VkQueue transfer = VK_NULL_HANDLE;
    u32 graphicsFamily = 0;
    u32 transferFamily = 0;
    VkPhysicalDeviceMemoryProperties memory{};
};

u32 findMemoryType(const Gpu& gpu, u32 typeBits, VkMemoryPropertyFlags flags) {
    for (u32 i = 0; i < gpu.memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 && (gpu.memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    usize size = 0;
};

bool createBuffer(const Gpu& gpu, usize size, VkBufferUsageFlags usage, bool hostVisible, Buffer& out) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(gpu.device, &info, nullptr, &out.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(gpu.device, out.buffer, &req);
    const VkMemoryPropertyFlags flags = hostVisible
                                            ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(gpu, req.memoryTypeBits, flags);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(gpu.device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindBufferMemory(gpu.device, out.buffer, out.memory, 0) != VK_SUCCESS) {
        return false;
    }
    if (hostVisible && vkMapMemory(gpu.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) {
        return false;
    }
    out.size = size;
    return true;
}

void destroyBuffer(const Gpu& gpu, Buffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(gpu.device, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(gpu.device, buffer.memory, nullptr);
    }
    buffer = Buffer{};
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    UploadImageDesc desc{};
};

bool createImage(const Gpu& gpu, const UploadImageDesc& desc, Image& out) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UINT;
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = desc.mipLevels;
    info.arrayLayers = desc.layerCount;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(gpu.device, &info, nullptr, &out.image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(gpu.device, out.image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(gpu.device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindImageMemory(gpu.device, out.image, out.memory, 0) != VK_SUCCESS) {
        return false;
    }
    out.desc = desc;
    return true;
}

void destroyImage(const Gpu& gpu, Image& image) {
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(gpu.device, image.image, nullptr);
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(gpu.device, image.memory, nullptr);
    }
    image = Image{};
}

// --- data patterns ----------------------------------------------------------------------------

std::vector<u8> pattern(usize bytes, u32 seed) {
    std::vector<u8> out(bytes);
    u32 state = seed * 0x9E3779B1u + 0x7F4A7C15u;
    for (usize i = 0; i < bytes; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        out[i] = static_cast<u8>(state >> 11);
    }
    return out;
}

// --- graphics-queue consumer: reads resources back without waiting on the upload fence --------

struct Consumer {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer readback;
    bool pending = false;
};

bool createConsumer(const Gpu& gpu, usize readbackBytes, Consumer& out) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = gpu.graphicsFamily;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateCommandPool(gpu.device, &poolInfo, nullptr, &out.pool) != VK_SUCCESS) {
        return false;
    }
    allocInfo.commandPool = out.pool;
    return vkAllocateCommandBuffers(gpu.device, &allocInfo, &out.cmd) == VK_SUCCESS &&
           vkCreateFence(gpu.device, &fenceInfo, nullptr, &out.fence) == VK_SUCCESS &&
           createBuffer(gpu, readbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, out.readback);
}

void destroyConsumer(const Gpu& gpu, Consumer& consumer) {
    destroyBuffer(gpu, consumer.readback);
    if (consumer.fence != VK_NULL_HANDLE) {
        vkDestroyFence(gpu.device, consumer.fence, nullptr);
    }
    if (consumer.pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(gpu.device, consumer.pool, nullptr);
    }
    consumer = Consumer{};
}

/// Where each resource landed in the consumer's readback buffer.
struct ReadbackLayout {
    std::vector<usize> bufferOffsets;
    std::vector<usize> imageOffsets;
    usize total = 0;
};

ReadbackLayout layoutFor(const std::vector<const Buffer*>& buffers, const std::vector<const Image*>& images) {
    ReadbackLayout layout;
    usize offset = 0;
    for (const Buffer* buffer : buffers) {
        layout.bufferOffsets.push_back(offset);
        offset = (offset + buffer->size + 255u) & ~usize{255u};
    }
    for (const Image* image : images) {
        layout.imageOffsets.push_back(offset);
        offset = (offset + UploadQueue::imageSourceBytes(image->desc) + 255u) & ~usize{255u};
    }
    layout.total = offset;
    return layout;
}

/// Record + submit on the graphics queue: copy every buffer and every mip/layer of every image
/// (tightly packed, mip-major like the upload source) into the consumer's readback buffer.
bool submitReadback(const Gpu& gpu, Consumer& consumer, const std::vector<const Buffer*>& buffers,
                    const std::vector<const Image*>& images, const ReadbackLayout& layout) {
    if (layout.total > consumer.readback.size) {
        return false;
    }
    vkResetCommandPool(gpu.device, consumer.pool, 0);
    vkResetFences(gpu.device, 1, &consumer.fence);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(consumer.cmd, &begin);

    // The readback buffer is rewritten each round; order after the host read of the last round.
    for (usize i = 0; i < buffers.size(); ++i) {
        VkBufferCopy region{};
        region.dstOffset = layout.bufferOffsets[i];
        region.size = buffers[i]->size;
        vkCmdCopyBuffer(consumer.cmd, buffers[i]->buffer, consumer.readback.buffer, 1, &region);
    }
    for (usize i = 0; i < images.size(); ++i) {
        const UploadImageDesc& desc = images[i]->desc;
        VkImageMemoryBarrier toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.srcAccessMask = 0;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = images[i]->image;
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mipLevels, 0, desc.layerCount};
        vkCmdPipelineBarrier(consumer.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toSrc);
        std::vector<VkBufferImageCopy> regions;
        usize offset = layout.imageOffsets[i];
        for (u32 mip = 0; mip < desc.mipLevels; ++mip) {
            const u32 w = std::max(1u, desc.width >> mip);
            const u32 h = std::max(1u, desc.height >> mip);
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, desc.layerCount};
            region.imageExtent = {w, h, 1};
            regions.push_back(region);
            offset += static_cast<usize>(w) * h * desc.layerCount * desc.bytesPerTexel;
        }
        vkCmdCopyImageToBuffer(consumer.cmd, images[i]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               consumer.readback.buffer, static_cast<u32>(regions.size()), regions.data());
        VkImageMemoryBarrier back = toSrc;
        back.srcAccessMask = 0;
        back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(consumer.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &back);
    }
    VkMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(consumer.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0,
                         nullptr, 0, nullptr);
    vkEndCommandBuffer(consumer.cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &consumer.cmd;
    consumer.pending = vkQueueSubmit(gpu.graphics, 1, &submit, consumer.fence) == VK_SUCCESS;
    return consumer.pending;
}

bool waitConsumer(const Gpu& gpu, Consumer& consumer) {
    if (!consumer.pending) {
        return false;
    }
    consumer.pending = false;
    return vkWaitForFences(gpu.device, 1, &consumer.fence, VK_TRUE, kTimeoutNs) == VK_SUCCESS;
}

bool sameBytes(const Consumer& consumer, usize offset, const std::vector<u8>& expected) {
    return std::memcmp(static_cast<const u8*>(consumer.readback.mapped) + offset, expected.data(), expected.size()) == 0;
}

// --- UploadQueue helpers ----------------------------------------------------------------------

bool uploadBuffer(UploadQueue& queue, const Buffer& dst, const std::vector<u8>& bytes, usize dstOffset) {
    usize src = 0;
    return queue.stage(bytes.data(), bytes.size(), src) && queue.recordBufferCopy(dst.buffer, src, dstOffset, bytes.size());
}

bool uploadImage(UploadQueue& queue, const Image& dst, const std::vector<u8>& bytes) {
    usize src = 0;
    return queue.stageImage(bytes.data(), dst.desc, src) && queue.recordImageCopy(dst.image, src, dst.desc);
}

// --- negative controls ------------------------------------------------------------------------

struct OneShot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

OneShot beginOneShot(const Gpu& gpu, u32 family) {
    OneShot shot;
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family;
    vkCreateCommandPool(gpu.device, &poolInfo, nullptr, &shot.pool);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = shot.pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(gpu.device, &allocInfo, &shot.cmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(shot.cmd, &begin);
    return shot;
}

void submitOneShot(VkQueue queue, OneShot& shot) {
    vkEndCommandBuffer(shot.cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &shot.cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
}

/// Runs `body`, returns the validation message ids it produced (not printed as failures).
template <typename Body>
std::vector<std::string> expectReported(const Gpu& gpu, Body body) {
    vkDeviceWaitIdle(gpu.device);
    const usize before = g_log.ids.size();
    const u32 countBefore = g_log.count;
    g_log.quiet = true;
    body();
    vkDeviceWaitIdle(gpu.device);
    g_log.quiet = false;
    std::vector<std::string> ids(g_log.ids.begin() + static_cast<std::ptrdiff_t>(before), g_log.ids.end());
    g_log.ids.resize(before);
    g_log.count = countBefore;
    return ids;
}

bool anyStartsWith(const std::vector<std::string>& ids, const char* prefix) {
    for (const std::string& id : ids) {
        if (id.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string joined(const std::vector<std::string>& ids) {
    std::string out;
    for (const std::string& id : ids) {
        if (out.find(id) == std::string::npos) {
            out += out.empty() ? "" : ", ";
            out += id;
        }
    }
    return out.empty() ? "(none)" : out;
}

int run(const char* layerDir, bool expectSplit) {
    // Layer order in VK_INSTANCE_LAYERS is application -> driver: validation sees the split device.
    std::string layers = kValidationLayer;
    if (layerDir != nullptr) {
        const char* existing = std::getenv("VK_ADD_LAYER_PATH");
        std::string path = layerDir;
        if (existing != nullptr && existing[0] != '\0') {
            path += ":";
            path += existing;
        }
        setEnv("VK_ADD_LAYER_PATH", path, true);
        layers += ":";
        layers += kSplitLayer;
    }
    setEnv("VK_INSTANCE_LAYERS", layers, true);
    setEnv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", true);
    setEnv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", true);
    // Messages reach the test's messenger only (negative controls must not print "Validation
    // Error" lines that fuse_vulkan_validation_gate would count).
    setEnv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", true);

    if (!layerAvailable(kValidationLayer)) {
        std::printf("SKIP: %s not installed\n", kValidationLayer);
        return kSkip;
    }
    if (expectSplit && !layerAvailable(kSplitLayer)) {
        std::fprintf(stderr, "FAIL: %s not found under %s\n", kSplitLayer, layerDir != nullptr ? layerDir : "(none)");
        return 1;
    }

    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_b2_upload_queue_family";
    instanceDesc.enableValidation = true;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    if (instance == nullptr || !instance->isValid()) {
        std::printf("SKIP: no Vulkan instance (%s)\n", instance != nullptr ? instance->info().message.c_str() : "");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(instance->nativeHandle());
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (createMessenger != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = onMessage;
        createMessenger(vkInstance, &info, nullptr, &messenger);
    }
    expectTrue(messenger != VK_NULL_HANDLE, "debug messenger created (validation output is captured)");

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    if (device == nullptr || !device->isValid()) {
        std::printf("SKIP: no Vulkan device (%s)\n", device != nullptr ? device->info().message.c_str() : "");
        if (messenger != VK_NULL_HANDLE) {
            destroyMessenger(vkInstance, messenger, nullptr);
        }
        return kSkip;
    }

    Gpu gpu;
    gpu.physical = static_cast<VkPhysicalDevice>(device->nativePhysicalDevice());
    gpu.device = static_cast<VkDevice>(device->nativeHandle());
    gpu.graphics = static_cast<VkQueue>(device->queues().graphics);
    gpu.transfer = static_cast<VkQueue>(device->queues().transfer);
    gpu.graphicsFamily = device->queues().graphicsFamily;
    gpu.transferFamily = device->queues().transferFamily;
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &gpu.memory);

    u32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &familyCount, nullptr);
    const bool split = gpu.transferFamily != gpu.graphicsFamily;
    std::printf("device: %s, %u queue families, graphics family %u, transfer family %u (%s)\n",
                device->info().deviceName.c_str(), familyCount, gpu.graphicsFamily, gpu.transferFamily,
                split ? "cross-family ownership transfers" : "single family");
    if (expectSplit) {
        expectTrue(familyCount == 2, "split layer exposes two queue families");
        expectTrue(split, "device picked the transfer-only family for uploads");
        expectTrue(gpu.transfer != gpu.graphics, "transfer and graphics queues are distinct handles");
    }

    // --- resources ---
    constexpr usize kRingBytes = 256u * 1024u;
    Buffer ring;
    expectTrue(createBuffer(gpu, kRingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, ring), "staging ring");
    const VkBufferUsageFlags bufferUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    Buffer b0;
    Buffer b1;
    expectTrue(createBuffer(gpu, 64u * 1024u, bufferUsage, false, b0), "buffer b0");
    expectTrue(createBuffer(gpu, 4u * 1024u + 12u, bufferUsage, false, b1), "buffer b1");
    UploadImageDesc chainDesc{};
    chainDesc.width = 64;
    chainDesc.height = 32;
    chainDesc.mipLevels = 7; // 64x32 .. 1x1
    chainDesc.layerCount = 3;
    chainDesc.bytesPerTexel = 4;
    UploadImageDesc singleDesc{};
    singleDesc.width = 16;
    singleDesc.height = 16;
    Image chain;
    Image single;
    expectTrue(createImage(gpu, chainDesc, chain), "7-mip x 3-layer image");
    expectTrue(createImage(gpu, singleDesc, single), "single-mip image");
    if (g_failures != 0) {
        return 1;
    }

    UploadQueue queue;
    expectTrue(queue.init(device.get(), ring.buffer, ring.mapped, kRingBytes), "upload queue init");
    expectTrue(queue.stats().queueFamilyOwnershipTransfer == split, "upload queue follows the device's families");
    expectTrue(queue.stats().dedicatedTransferQueue == split, "copies run on the transfer queue when split");

    const std::vector<const Buffer*> buffers = {&b0, &b1};
    const std::vector<const Image*> images = {&chain, &single};
    const ReadbackLayout layout = layoutFor(buffers, images);
    Consumer consumerA;
    Consumer consumerB;
    expectTrue(createConsumer(gpu, layout.total, consumerA) && createConsumer(gpu, layout.total, consumerB),
               "graphics consumers");

    // --- A: first uploads, duplicates in one batch, consumer right after flush ---
    std::vector<u8> b0Data = pattern(b0.size, 1);
    const std::vector<u8> b0Patch = pattern(512, 2);
    std::vector<u8> b1Data = pattern(b1.size, 3);
    const std::vector<u8> chainFirst = pattern(UploadQueue::imageSourceBytes(chainDesc), 4);
    std::vector<u8> chainData = pattern(UploadQueue::imageSourceBytes(chainDesc), 5);
    const std::vector<u8> singleData = pattern(UploadQueue::imageSourceBytes(singleDesc), 6);
    expectTrue(UploadQueue::imageStagingBytes(chainDesc) >= chainData.size(), "staged chain holds every mip");

    expectTrue(uploadBuffer(queue, b0, b0Data, 0), "A: b0 full upload");
    expectTrue(uploadBuffer(queue, b1, b1Data, 0), "A: b1 full upload");
    expectTrue(uploadBuffer(queue, b0, b0Patch, 1024), "A: b0 patched in the same batch");
    std::memcpy(b0Data.data() + 1024, b0Patch.data(), b0Patch.size());
    expectTrue(uploadImage(queue, chain, chainFirst), "A: chain upload");
    expectTrue(uploadImage(queue, chain, chainData), "A: chain rewritten in the same batch");
    {
        usize src = 0;
        expectTrue(queue.stage(singleData.data(), singleData.size(), src) &&
                       queue.recordImageCopy(single.image, src, singleDesc.width, singleDesc.height, 1),
                   "A: single-mip image via the mip-0 overload");
    }
    const UploadTicket ticketA = queue.flush();
    expectTrue(ticketA.isValid() && ticketA.serial > 0, "A: batch submitted");
    if (split) {
        // b0 twice + b1: two buffers; chain twice + single: two images. One transfer each.
        expectTrue(queue.stats().ownershipBufferTransfers == 2, "A: one ownership round trip per buffer per batch");
        expectTrue(queue.stats().ownershipImageTransfers == 2, "A: one ownership round trip per image per batch");
    }
    // No CPU wait: the acquire (graphics queue) precedes this submission in queue order.
    expectTrue(submitReadback(gpu, consumerA, buffers, images, layout), "A: graphics readback submitted");

    // --- B: re-upload live resources while A's readback is still queued ---
    std::vector<u8> b1Next = b1Data;
    const std::vector<u8> b1Patch = pattern(256, 7);
    std::memcpy(b1Next.data() + 256, b1Patch.data(), b1Patch.size());
    const std::vector<u8> chainNext = pattern(chainData.size(), 8);
    expectTrue(uploadBuffer(queue, b1, b1Patch, 256), "B: partial update of live b1");
    expectTrue(uploadImage(queue, chain, chainNext), "B: chain re-upload");
    const UploadTicket ticketB = queue.flush();
    expectTrue(ticketB.serial > ticketA.serial, "B: batch submitted");
    expectTrue(submitReadback(gpu, consumerB, buffers, images, layout), "B: graphics readback submitted");

    expectTrue(waitConsumer(gpu, consumerA), "A: readback fence");
    expectTrue(sameBytes(consumerA, layout.bufferOffsets[0], b0Data), "A: b0 = full upload + same-batch patch");
    expectTrue(sameBytes(consumerA, layout.bufferOffsets[1], b1Data), "A: b1 intact");
    expectTrue(sameBytes(consumerA, layout.imageOffsets[0], chainData), "A: every mip/layer = second same-batch write");
    expectTrue(sameBytes(consumerA, layout.imageOffsets[1], singleData), "A: single-mip image intact");
    expectTrue(waitConsumer(gpu, consumerB), "B: readback fence");
    expectTrue(sameBytes(consumerB, layout.bufferOffsets[0], b0Data), "B: b0 untouched");
    expectTrue(sameBytes(consumerB, layout.bufferOffsets[1], b1Next), "B: b1 patched, bytes outside the patch kept");
    expectTrue(sameBytes(consumerB, layout.imageOffsets[0], chainNext), "B: chain re-upload in every mip/layer");
    expectTrue(sameBytes(consumerB, layout.imageOffsets[1], singleData), "B: single-mip image untouched");
    expectTrue(queue.wait(ticketB, kTimeoutNs), "B: ticket completes");

    // --- C: many batches in flight through a small ring, interleaved with graphics reads ---
    {
        constexpr u32 kRounds = 48;
        const usize chunk = 20u * 1024u;
        std::vector<u8> expectB0 = b0Data;
        std::vector<u8> expectChain = chainNext;
        for (u32 round = 0; round < kRounds; ++round) {
            const usize at = (static_cast<usize>(round) % 3u) * chunk; // 0, 20K, 40K
            const std::vector<u8> bytes = pattern(chunk, 100 + round);
            expectTrue(uploadBuffer(queue, b0, bytes, at), "C: b0 chunk upload");
            std::memcpy(expectB0.data() + at, bytes.data(), bytes.size());
            if (round % 8u == 5u) {
                expectChain = pattern(chainData.size(), 200 + round);
                expectTrue(uploadImage(queue, chain, expectChain), "C: chain upload");
            }
            if (round % 4u == 3u) {
                queue.flush();
            }
            if (round % 12u == 11u) {
                Consumer& consumer = (round / 12u) % 2u == 0u ? consumerA : consumerB;
                expectTrue(submitReadback(gpu, consumer, buffers, images, layout), "C: interleaved readback");
                expectTrue(waitConsumer(gpu, consumer), "C: interleaved readback fence");
                expectTrue(sameBytes(consumer, layout.bufferOffsets[0], expectB0), "C: b0 matches at readback");
                expectTrue(sameBytes(consumer, layout.imageOffsets[0], expectChain), "C: chain matches at readback");
            }
        }
        queue.flush();
        expectTrue(submitReadback(gpu, consumerA, buffers, images, layout), "C: final readback");
        expectTrue(waitConsumer(gpu, consumerA), "C: final readback fence");
        expectTrue(sameBytes(consumerA, layout.bufferOffsets[0], expectB0), "C: b0 final");
        expectTrue(sameBytes(consumerA, layout.imageOffsets[0], expectChain), "C: chain final");
        expectTrue(queue.waitAll(kTimeoutNs), "C: all tickets complete");
    }

    const fuse::renderer::UploadQueueStats& stats = queue.stats();
    std::printf("upload stats: %llu batches, %llu copies, max %u in flight, %u ring wraps, %u stalls, "
                "%u fence timeouts, %u submit failures, ownership transfers: %llu buffers / %llu images\n",
                static_cast<unsigned long long>(stats.submittedBatches),
                static_cast<unsigned long long>(stats.recordedCopies), stats.maxBatchesInFlight, stats.ringWraps,
                stats.ringStalls, stats.fenceTimeouts, stats.submitFailures,
                static_cast<unsigned long long>(stats.ownershipBufferTransfers),
                static_cast<unsigned long long>(stats.ownershipImageTransfers));
    expectTrue(stats.ringWraps > 0, "ring wrapped under in-flight batches");
    expectTrue(stats.maxBatchesInFlight > 1, "several batches in flight at once");
    expectTrue(stats.fenceTimeouts == 0 && stats.submitFailures == 0, "no fence timeouts or submit failures");
    if (split) {
        expectTrue(stats.ownershipBufferTransfers >= stats.submittedBatches, "every batch transferred its buffers");
        expectTrue(stats.ownershipImageTransfers > 2, "image ownership transferred in later batches too");
    }
    queue.destroy();
    vkDeviceWaitIdle(gpu.device);
    const u32 positiveMessages = g_log.count;
    expectTrue(positiveMessages == 0, "zero validation messages (incl. synchronization validation)");

    // --- negative controls: the checks above are live ---
    Buffer scratch;
    Buffer scratchSrc;
    expectTrue(createBuffer(gpu, 4096, bufferUsage, false, scratch) && createBuffer(gpu, 4096, bufferUsage, false, scratchSrc),
               "negative-control buffers");
    VkBufferCopy whole{};
    whole.size = 4096;

    const auto waw = expectReported(gpu, [&] {
        OneShot shot = beginOneShot(gpu, gpu.graphicsFamily);
        vkCmdCopyBuffer(shot.cmd, scratchSrc.buffer, scratch.buffer, 1, &whole);
        vkCmdCopyBuffer(shot.cmd, scratchSrc.buffer, scratch.buffer, 1, &whole); // no barrier: WAW
        submitOneShot(gpu.graphics, shot);
        vkQueueWaitIdle(gpu.graphics);
        vkDestroyCommandPool(gpu.device, shot.pool, nullptr);
    });
    std::printf("negative control (unsynchronized WAW): %s\n", joined(waw).c_str());
    expectTrue(anyStartsWith(waw, "SYNC-HAZARD-WRITE-AFTER-WRITE"), "synchronization validation is active");

    if (split) {
        const auto orphanAcquire = expectReported(gpu, [&] {
            OneShot shot = beginOneShot(gpu, gpu.graphicsFamily);
            VkBufferMemoryBarrier acquire{};
            acquire.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            acquire.srcQueueFamilyIndex = gpu.transferFamily;
            acquire.dstQueueFamilyIndex = gpu.graphicsFamily;
            acquire.buffer = scratch.buffer;
            acquire.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(shot.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 1, &acquire, 0, nullptr);
            submitOneShot(gpu.graphics, shot);
            vkQueueWaitIdle(gpu.graphics);
            vkDestroyCommandPool(gpu.device, shot.pool, nullptr);
        });
        std::printf("negative control (acquire without release): %s\n", joined(orphanAcquire).c_str());
        expectTrue(!orphanAcquire.empty(), "validation pairs queue family release/acquire barriers");

        const auto crossQueue = expectReported(gpu, [&] {
            OneShot write = beginOneShot(gpu, gpu.transferFamily);
            vkCmdCopyBuffer(write.cmd, scratchSrc.buffer, scratch.buffer, 1, &whole);
            OneShot read = beginOneShot(gpu, gpu.graphicsFamily);
            vkCmdCopyBuffer(read.cmd, scratch.buffer, scratchSrc.buffer, 1, &whole);
            submitOneShot(gpu.transfer, write);
            submitOneShot(gpu.graphics, read); // no semaphore between the queues
            vkDeviceWaitIdle(gpu.device);
            vkDestroyCommandPool(gpu.device, write.pool, nullptr);
            vkDestroyCommandPool(gpu.device, read.pool, nullptr);
        });
        std::printf("negative control (transfer write -> graphics read, no semaphore): %s\n",
                    joined(crossQueue).c_str());
        expectTrue(anyStartsWith(crossQueue, "SYNC-HAZARD"), "synchronization validation checks across queues");
    }

    destroyBuffer(gpu, scratch);
    destroyBuffer(gpu, scratchSrc);
    destroyConsumer(gpu, consumerA);
    destroyConsumer(gpu, consumerB);
    destroyImage(gpu, chain);
    destroyImage(gpu, single);
    destroyBuffer(gpu, b0);
    destroyBuffer(gpu, b1);
    destroyBuffer(gpu, ring);
    device.reset();
    expectTrue(g_log.count == positiveMessages, "no validation messages during teardown");
    if (messenger != VK_NULL_HANDLE) {
        destroyMessenger(vkInstance, messenger, nullptr);
    }
    instance.reset();

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_b2_upload_queue_family: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_b2_upload_queue_family: PASS (%s path, 0 validation messages)\n",
                split ? "cross-family" : "same-family");
    return 0;
}

#endif // FUSE_VULKAN_BACKEND

} // namespace

int main(int argc, char** argv) {
#if defined(FUSE_VULKAN_BACKEND)
    const char* layerDir = nullptr;
    bool expectSplit = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layer-dir") == 0 && i + 1 < argc) {
            layerDir = argv[++i];
        } else if (std::strcmp(argv[i], "--expect-split") == 0) {
            expectSplit = true;
        }
    }
    return run(layerDir, expectSplit);
#else
    (void)argc;
    (void)argv;
    std::printf("SKIP: Vulkan backend disabled at build time (stub build)\n");
    return kSkip;
#endif
}
