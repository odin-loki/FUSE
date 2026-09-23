// Gate: "Logical device created with graphics, compute, and transfer queues on separate families
// where available".
//
// Lavapipe exposes one queue family, so the test-only layer VK_LAYER_FUSE_split_transfer_family
// (tests/vk_layer_split_transfer_family.cpp) is used to present discrete-GPU-like topologies:
//   --mode transfer-compute : family 0 graphics|compute|transfer, 1 transfer-only, 2 compute|transfer
//                             (layer + FUSE_SPLIT_LAYER_COMPUTE_FAMILY=1)
//   --mode transfer         : family 0 graphics|compute|transfer, 1 transfer-only (layer)
//   --mode shared           : Lavapipe as is, one family (no layer)
// For each topology the test derives the expected families from the reported queue family
// properties (dedicated transfer = TRANSFER without GRAPHICS/COMPUTE, else TRANSFER without
// GRAPHICS, else the graphics family; dedicated compute = COMPUTE without GRAPHICS, else the
// graphics family), checks `VulkanDevice` picked exactly those, created a queue on each distinct
// family (distinct VkQueue handles for distinct families, one shared handle on fallback), and then
// runs a transfer -> compute -> graphics semaphore chain on the three queues (fill on the transfer
// queue, fill on the compute queue, copy + host readback on the graphics queue) and verifies the
// bytes. With the Khronos validation layer installed (synchronization validation on) any validation
// message fails the test.
//
// Exit 77 (skip) in the stub build or without an ICD.
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kSplitLayer = "VK_LAYER_FUSE_split_transfer_family";
constexpr u64 kTimeoutNs = 5000000000ull;

int g_failures = 0;
u32 g_messages = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n",
                 data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)",
                 data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
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

enum class Mode { Shared, Transfer, TransferCompute };

struct Expected {
    u32 graphics = 0;
    u32 compute = 0;
    u32 transfer = 0;
};

/// "Where available" as a spec, computed from the reported families independently of the RHI.
Expected expectedFamilies(const std::vector<VkQueueFamilyProperties>& families, u32 graphicsFamily) {
    Expected e{};
    e.graphics = graphicsFamily;
    e.compute = graphicsFamily;
    e.transfer = graphicsFamily;
    for (u32 i = 0; i < families.size(); ++i) {
        const VkQueueFlags f = families[i].queueFlags;
        if ((f & VK_QUEUE_COMPUTE_BIT) != 0 && (f & VK_QUEUE_GRAPHICS_BIT) == 0) {
            e.compute = i;
            break;
        }
    }
    bool found = false;
    for (u32 i = 0; i < families.size() && !found; ++i) {
        const VkQueueFlags f = families[i].queueFlags;
        if ((f & VK_QUEUE_TRANSFER_BIT) != 0 && (f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0) {
            e.transfer = i;
            found = true;
        }
    }
    for (u32 i = 0; i < families.size() && !found; ++i) {
        const VkQueueFlags f = families[i].queueFlags;
        if ((f & VK_QUEUE_TRANSFER_BIT) != 0 && (f & VK_QUEUE_GRAPHICS_BIT) == 0) {
            e.transfer = i;
            found = true;
        }
    }
    return e;
}

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

bool createHostBuffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage,
                      const std::vector<u32>& families, HostBuffer& out) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    // Distinct families touch the buffer: CONCURRENT avoids ownership transfers (the upload queue's
    // QFO path has its own gate, fuse_b2_upload_queue_family).
    if (families.size() > 1) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = static_cast<u32>(families.size());
        info.pQueueFamilyIndices = families.data();
    } else {
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    u32 type = UINT32_MAX;
    for (u32 i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; ++i) {
        if ((req.memoryTypeBits & (1u << i)) != 0 && (mem.memoryTypes[i].propertyFlags & want) == want) {
            type = i;
        }
    }
    if (type == UINT32_MAX) {
        return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
        return false;
    }
    return vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) == VK_SUCCESS;
}

void destroyHostBuffer(VkDevice device, HostBuffer& b) {
    if (b.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, b.buffer, nullptr);
    }
    if (b.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, b.memory, nullptr);
    }
    b = {};
}

struct Recorder {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

bool beginRecorder(VkDevice device, u32 family, Recorder& r) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = family;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &r.pool) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = r.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &alloc, &r.cmd) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(r.cmd, &begin) == VK_SUCCESS;
}

bool submit(VkQueue queue, VkCommandBuffer cmd, VkSemaphore wait, VkSemaphore signal, VkFence fence) {
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    info.waitSemaphoreCount = wait != VK_NULL_HANDLE ? 1u : 0u;
    info.pWaitSemaphores = &wait;
    info.pWaitDstStageMask = &waitStage;
    info.commandBufferCount = 1;
    info.pCommandBuffers = &cmd;
    info.signalSemaphoreCount = signal != VK_NULL_HANDLE ? 1u : 0u;
    info.pSignalSemaphores = &signal;
    return vkQueueSubmit(queue, 1, &info, fence) == VK_SUCCESS;
}

int run(const char* layerDir, Mode mode) {
    const bool useLayer = mode != Mode::Shared;
    const bool validation = layerAvailable(kValidationLayer);
    std::string layers = validation ? kValidationLayer : "";
    if (useLayer) {
        if (layerDir == nullptr) {
            std::fprintf(stderr, "FAIL: --layer-dir required for this mode\n");
            return 1;
        }
        std::string path = layerDir;
        const char* existing = std::getenv("VK_ADD_LAYER_PATH");
        if (existing != nullptr && existing[0] != '\0') {
            path += ":";
            path += existing;
        }
        setenv("VK_ADD_LAYER_PATH", path.c_str(), 1);
        layers += layers.empty() ? "" : ":";
        layers += kSplitLayer; // application -> driver order: validation sees the split device
    }
    setenv("FUSE_SPLIT_LAYER_COMPUTE_FAMILY", mode == Mode::TransferCompute ? "1" : "0", 1);
    setenv("VK_INSTANCE_LAYERS", layers.c_str(), 1);
    if (validation) {
        setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
        setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
        setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    } else {
        std::printf("note: %s not installed — running without validation\n", kValidationLayer);
    }
    if (useLayer && !layerAvailable(kSplitLayer)) {
        std::fprintf(stderr, "FAIL: %s not found under %s\n", kSplitLayer, layerDir);
        return 1;
    }

    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_b2_device_queue_families";
    instanceDesc.enableValidation = validation;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    if (instance == nullptr || !instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(instance->nativeHandle());
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (validation && createMessenger != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = onMessage;
        createMessenger(vkInstance, &info, nullptr, &messenger);
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    if (device == nullptr || !device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        if (messenger != VK_NULL_HANDLE) {
            destroyMessenger(vkInstance, messenger, nullptr);
        }
        return kSkip;
    }
    const VkPhysicalDevice physical = static_cast<VkPhysicalDevice>(device->nativePhysicalDevice());
    const VkDevice vkDevice = static_cast<VkDevice>(device->nativeHandle());
    const fuse::renderer::VulkanQueues& q = device->queues();

    u32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    std::printf("device %s: %u families;", device->info().deviceName.c_str(), familyCount);
    for (u32 i = 0; i < familyCount; ++i) {
        const VkQueueFlags f = families[i].queueFlags;
        std::printf(" [%u]%s%s%s", i, (f & VK_QUEUE_GRAPHICS_BIT) ? "G" : "", (f & VK_QUEUE_COMPUTE_BIT) ? "C" : "",
                    (f & VK_QUEUE_TRANSFER_BIT) ? "T" : "");
    }
    std::printf("\npicked: graphics %u, compute %u (dedicated %d), transfer %u (dedicated %d)\n", q.graphicsFamily,
                q.computeFamily, q.dedicatedCompute ? 1 : 0, q.transferFamily, q.dedicatedTransfer ? 1 : 0);

    // --- topology as advertised, and the RHI's picks against the "where available" spec ---
    const u32 wantFamilies = mode == Mode::TransferCompute ? 3u : mode == Mode::Transfer ? 2u : 1u;
    expectTrue(familyCount == wantFamilies, "queue family topology matches the mode");
    expectTrue(q.graphicsFamily < familyCount && (families[q.graphicsFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0,
               "graphics queue family supports graphics");
    expectTrue(q.computeFamily < familyCount && (families[q.computeFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0,
               "compute queue family supports compute");
    expectTrue(q.transferFamily < familyCount && (families[q.transferFamily].queueFlags &
                                                  (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0,
               "transfer queue family supports transfer");
    const Expected e = expectedFamilies(families, q.graphicsFamily);
    expectTrue(q.computeFamily == e.compute, "compute family = dedicated compute family where available, else shared");
    expectTrue(q.transferFamily == e.transfer, "transfer family = dedicated transfer family where available, else shared");
    expectTrue(q.dedicatedCompute == (q.computeFamily != q.graphicsFamily), "dedicatedCompute flag consistent");
    expectTrue(q.dedicatedTransfer == (q.transferFamily != q.graphicsFamily), "dedicatedTransfer flag consistent");
    switch (mode) {
    case Mode::TransferCompute:
        expectTrue(q.graphicsFamily == 0 && q.transferFamily == 1 && q.computeFamily == 2,
                   "three separate families: graphics 0, transfer 1 (transfer-only), compute 2 (compute-only)");
        expectTrue(q.dedicatedCompute && q.dedicatedTransfer, "dedicated compute and transfer");
        break;
    case Mode::Transfer:
        expectTrue(q.graphicsFamily == 0 && q.transferFamily == 1 && q.computeFamily == 0,
                   "transfer on its own family, compute falls back to the graphics family");
        expectTrue(!q.dedicatedCompute && q.dedicatedTransfer, "dedicated transfer only");
        break;
    case Mode::Shared:
        expectTrue(q.graphicsFamily == 0 && q.transferFamily == 0 && q.computeFamily == 0,
                   "single family: all three queues fall back to the shared family");
        expectTrue(!q.dedicatedCompute && !q.dedicatedTransfer, "no dedicated families");
        break;
    }

    // --- queues exist on each picked family; distinct families -> distinct handles ---
    expectTrue(q.graphics != nullptr && q.compute != nullptr && q.transfer != nullptr, "all three VkQueue handles");
    expectTrue((q.compute != q.graphics) == (q.computeFamily != q.graphicsFamily),
               "compute queue handle distinct iff on a separate family");
    expectTrue((q.transfer != q.graphics) == (q.transferFamily != q.graphicsFamily),
               "transfer queue handle distinct iff on a separate family");
    expectTrue((q.transfer != q.compute) == (q.transferFamily != q.computeFamily),
               "transfer vs compute handle distinct iff separate families");
    VkQueue probe = VK_NULL_HANDLE;
    vkGetDeviceQueue(vkDevice, q.transferFamily, 0, &probe);
    expectTrue(probe == static_cast<VkQueue>(q.transfer), "transfer queue was created at device creation");
    vkGetDeviceQueue(vkDevice, q.computeFamily, 0, &probe);
    expectTrue(probe == static_cast<VkQueue>(q.compute), "compute queue was created at device creation");

    // --- functional chain: transfer fill -> compute fill -> graphics copy + readback ---
    std::vector<u32> distinct;
    for (u32 f : {q.graphicsFamily, q.computeFamily, q.transferFamily}) {
        bool seen = false;
        for (u32 d : distinct) {
            seen = seen || d == f;
        }
        if (!seen) {
            distinct.push_back(f);
        }
    }
    constexpr VkDeviceSize kBytes = 4096;
    HostBuffer target;
    HostBuffer readback;
    expectTrue(createHostBuffer(physical, vkDevice, kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                distinct, target),
               "target buffer");
    expectTrue(createHostBuffer(physical, vkDevice, kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, {q.graphicsFamily}, readback),
               "readback buffer");
    std::memset(readback.mapped, 0, kBytes);

    Recorder onTransfer;
    Recorder onCompute;
    Recorder onGraphics;
    expectTrue(beginRecorder(vkDevice, q.transferFamily, onTransfer), "transfer command buffer");
    expectTrue(beginRecorder(vkDevice, q.computeFamily, onCompute), "compute command buffer");
    expectTrue(beginRecorder(vkDevice, q.graphicsFamily, onGraphics), "graphics command buffer");
    vkCmdFillBuffer(onTransfer.cmd, target.buffer, 0, kBytes / 2, 0xA5A5A5A5u);
    vkCmdFillBuffer(onCompute.cmd, target.buffer, kBytes / 2, kBytes / 2, 0x3C3C3C3Cu);
    VkBufferCopy region{0, 0, kBytes};
    vkCmdCopyBuffer(onGraphics.cmd, target.buffer, readback.buffer, 1, &region);
    VkMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(onGraphics.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0,
                         nullptr, 0, nullptr);
    for (Recorder* r : {&onTransfer, &onCompute, &onGraphics}) {
        expectTrue(vkEndCommandBuffer(r->cmd) == VK_SUCCESS, "end command buffer");
    }
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore transferDone = VK_NULL_HANDLE;
    VkSemaphore computeDone = VK_NULL_HANDLE;
    vkCreateSemaphore(vkDevice, &semInfo, nullptr, &transferDone);
    vkCreateSemaphore(vkDevice, &semInfo, nullptr, &computeDone);
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(vkDevice, &fenceInfo, nullptr, &fence);

    expectTrue(submit(static_cast<VkQueue>(q.transfer), onTransfer.cmd, VK_NULL_HANDLE, transferDone, VK_NULL_HANDLE),
               "submit on transfer queue");
    expectTrue(submit(static_cast<VkQueue>(q.compute), onCompute.cmd, transferDone, computeDone, VK_NULL_HANDLE),
               "submit on compute queue (waits transfer)");
    expectTrue(submit(static_cast<VkQueue>(q.graphics), onGraphics.cmd, computeDone, VK_NULL_HANDLE, fence),
               "submit on graphics queue (waits compute)");
    expectTrue(vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, kTimeoutNs) == VK_SUCCESS, "chain completes");
    const auto* bytes = static_cast<const unsigned char*>(readback.mapped);
    u32 wrong = 0;
    for (VkDeviceSize i = 0; i < kBytes; ++i) {
        wrong += bytes[i] != (i < kBytes / 2 ? 0xA5u : 0x3Cu) ? 1u : 0u;
    }
    expectTrue(wrong == 0, "graphics readback sees the transfer-queue and compute-queue writes");

    vkDeviceWaitIdle(vkDevice);
    vkDestroyFence(vkDevice, fence, nullptr);
    vkDestroySemaphore(vkDevice, transferDone, nullptr);
    vkDestroySemaphore(vkDevice, computeDone, nullptr);
    for (Recorder* r : {&onTransfer, &onCompute, &onGraphics}) {
        vkDestroyCommandPool(vkDevice, r->pool, nullptr);
    }
    destroyHostBuffer(vkDevice, target);
    destroyHostBuffer(vkDevice, readback);
    device.reset();
    if (messenger != VK_NULL_HANDLE) {
        destroyMessenger(vkInstance, messenger, nullptr);
    }
    instance.reset();

    expectTrue(g_messages == 0, "zero validation messages");
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_b2_device_queue_families: OK (%s validation)\n", validation ? "with" : "without");
    return 0;
}

#endif // FUSE_VULKAN_BACKEND

} // namespace

int main(int argc, char** argv) {
#if defined(FUSE_VULKAN_BACKEND)
    const char* layerDir = nullptr;
    Mode mode = Mode::TransferCompute;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layer-dir") == 0 && i + 1 < argc) {
            layerDir = argv[++i];
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char* m = argv[++i];
            mode = std::strcmp(m, "shared") == 0     ? Mode::Shared
                   : std::strcmp(m, "transfer") == 0 ? Mode::Transfer
                                                     : Mode::TransferCompute;
        }
    }
    return run(layerDir, mode);
#else
    (void)argc;
    (void)argv;
    std::printf("SKIP: Vulkan backend disabled at build time (stub build)\n");
    return kSkip;
#endif
}
