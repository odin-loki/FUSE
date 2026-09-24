// WP-0.2 gate (docs/unification/RENDERER-EXECUTION.md): fuse_rhi loads Vulkan through volk.
//
//  * Global entry points are loaded before main() (volkInitialize at static init) and the loader
//    reports an instance version >= 1.3.
//  * VulkanInstance loads the instance table; VulkanDevice switches to device-level dispatch: while
//    it is the only live device, volk's globals equal vkGetDeviceProcAddr(device) and differ from the
//    loader trampolines vkGetInstanceProcAddr(instance) returns.
//  * A second live device falls back to the trampolines (valid for both devices); destroying it
//    restores device-level dispatch for the survivor. Real work (vkCmdFillBuffer + readback) runs on
//    each device in each mode.
//  * The reload hook (the mechanism b5_vk_call_hooks.cpp uses) wraps a global immediately and again
//    after every reload; removing it restores the unwrapped table.
//  * Zero validation errors and warnings (validation + sync validation enabled by the ctest env).
// The companion ctest fuse_rp_volk_no_loader_imports checks the link side (no vk* imports).
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/loader.hpp>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

using fuse::u32;
using namespace fuse::renderer;

constexpr int kSkip = 77;
int g_failures = 0;

void expectTrue(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

#if defined(FUSE_VULKAN_BACKEND)

const char* modeName(vkloader::DispatchMode mode) {
    switch (mode) {
    case vkloader::DispatchMode::Unavailable:
        return "Unavailable";
    case vkloader::DispatchMode::Global:
        return "Global";
    case vkloader::DispatchMode::Instance:
        return "Instance";
    case vkloader::DispatchMode::Device:
        return "Device";
    }
    return "?";
}

void expectMode(vkloader::DispatchMode expected, const std::string& what) {
    const vkloader::DispatchMode actual = vkloader::dispatchMode();
    expectTrue(actual == expected, what + ": dispatch mode " + modeName(actual) + ", expected " + modeName(expected));
}

// ---- Reload-hook wrapper around vkCmdFillBuffer ---------------------------------------------------

PFN_vkCmdFillBuffer g_nextFill = nullptr;
u32 g_fillCalls = 0;

VKAPI_ATTR void VKAPI_CALL countingFill(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                                        u32 data) {
    ++g_fillCalls;
    g_nextFill(cmd, buffer, offset, size, data);
}

void installCountingFill(void* user) {
    ++*static_cast<u32*>(user);
    if (vkCmdFillBuffer != nullptr && vkCmdFillBuffer != &countingFill) {
        g_nextFill = vkCmdFillBuffer;
        vkCmdFillBuffer = &countingFill;
    }
}

// ---- Real work on a device ------------------------------------------------------------------------

/// Fills a host-visible buffer with `value` on the device's graphics queue and reads it back.
bool fillAndReadBack(const VulkanDevice& device, u32 value, std::string& why) {
    const auto dev = static_cast<VkDevice>(device.nativeHandle());
    const auto physical = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    const auto queue = static_cast<VkQueue>(device.queues().graphics);
    constexpr VkDeviceSize kSize = 256;

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool ok = false;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = kSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkMemoryRequirements req{};
    VkPhysicalDeviceMemoryProperties props{};
    u32 typeIndex = UINT32_MAX;
    VkMemoryAllocateInfo allocInfo{};
    VkCommandPoolCreateInfo poolInfo{};
    VkCommandBufferAllocateInfo cmdInfo{};
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin{};
    VkFenceCreateInfo fenceInfo{};
    VkSubmitInfo submit{};
    void* mapped = nullptr;

    if (vkCreateBuffer(dev, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        why = "vkCreateBuffer";
        goto done;
    }
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (u32 i = 0; i < props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((req.memoryTypeBits & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & want) == want) {
            typeIndex = i;
            break;
        }
    }
    if (typeIndex == UINT32_MAX) {
        why = "no host-visible coherent memory type";
        goto done;
    }
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = typeIndex;
    if (vkAllocateMemory(dev, &allocInfo, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(dev, buffer, memory, 0) != VK_SUCCESS) {
        why = "vkAllocateMemory / vkBindBufferMemory";
        goto done;
    }
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = device.queues().graphicsFamily;
    if (vkCreateCommandPool(dev, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        why = "vkCreateCommandPool";
        goto done;
    }
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool = pool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkAllocateCommandBuffers(dev, &cmdInfo, &cmd) != VK_SUCCESS ||
        vkCreateFence(dev, &fenceInfo, nullptr, &fence) != VK_SUCCESS || vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
        why = "command buffer / fence setup";
        goto done;
    }
    vkCmdFillBuffer(cmd, buffer, 0, VK_WHOLE_SIZE, value);
    {
        // Make the transfer write visible to the host read after the fence.
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                             0, nullptr);
    }
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        why = "vkEndCommandBuffer";
        goto done;
    }
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
        vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) {
        why = "submit / fence wait";
        goto done;
    }
    if (vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || mapped == nullptr) {
        why = "vkMapMemory";
        goto done;
    }
    ok = true;
    for (VkDeviceSize i = 0; i < kSize / sizeof(u32); ++i) {
        u32 word = 0;
        std::memcpy(&word, static_cast<const char*>(mapped) + i * sizeof(u32), sizeof(word));
        if (word != value) {
            why = "readback mismatch at word " + std::to_string(i);
            ok = false;
            break;
        }
    }
    vkUnmapMemory(dev, memory);

done:
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(dev, fence, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev, pool, nullptr);
    }
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, buffer, nullptr);
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, memory, nullptr);
    }
    return ok;
}

void expectFill(const VulkanDevice& device, u32 value, const std::string& what) {
    std::string why;
    expectTrue(fillAndReadBack(device, value, why), what + ": fill + readback (" + why + ")");
}

PFN_vkVoidFunction deviceFn(const VulkanDevice& device, const char* name) {
    return vkGetDeviceProcAddr(static_cast<VkDevice>(device.nativeHandle()), name);
}

PFN_vkVoidFunction trampolineFn(const VulkanInstance& instance, const char* name) {
    return vkGetInstanceProcAddr(static_cast<VkInstance>(instance.nativeHandle()), name);
}

/// Device-level dispatch for `device`: volk's globals are exactly the device's own entry points,
/// which are not the loader's trampolines.
void expectDeviceDispatch(const VulkanInstance& instance, const VulkanDevice& device, const std::string& what) {
    expectMode(vkloader::DispatchMode::Device, what);
    expectTrue(vkloader::dispatchDevice() == device.nativeHandle(), what + ": loaded device is the live one");
    expectTrue(reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit) == deviceFn(device, "vkQueueSubmit") &&
                   reinterpret_cast<PFN_vkVoidFunction>(vkCmdFillBuffer) == deviceFn(device, "vkCmdFillBuffer") &&
                   reinterpret_cast<PFN_vkVoidFunction>(vkCreateBuffer) == deviceFn(device, "vkCreateBuffer"),
               what + ": volk globals == vkGetDeviceProcAddr(device)");
    expectTrue(reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit) != trampolineFn(instance, "vkQueueSubmit") &&
                   reinterpret_cast<PFN_vkVoidFunction>(vkCmdFillBuffer) != trampolineFn(instance, "vkCmdFillBuffer"),
               what + ": device-level dispatch bypasses the loader trampolines");
}

void expectTrampolineDispatch(const VulkanInstance& instance, const std::string& what) {
    expectMode(vkloader::DispatchMode::Instance, what);
    expectTrue(vkloader::dispatchDevice() == nullptr, what + ": no single loaded device");
    expectTrue(reinterpret_cast<PFN_vkVoidFunction>(vkQueueSubmit) == trampolineFn(instance, "vkQueueSubmit") &&
                   reinterpret_cast<PFN_vkVoidFunction>(vkCmdFillBuffer) == trampolineFn(instance, "vkCmdFillBuffer"),
               what + ": volk globals are the loader trampolines");
}

int runVulkan() {
    expectTrue(vkloader::initialize(), "volkInitialize found the loader");
    expectMode(vkloader::DispatchMode::Global, "before any instance");
    expectTrue(vkCreateInstance != nullptr && vkEnumerateInstanceLayerProperties != nullptr &&
                   vkEnumerateInstanceVersion != nullptr && vkGetInstanceProcAddr != nullptr,
               "global entry points loaded before main()");
    const u32 loaderVersion = vkloader::loaderInstanceVersion();
    expectTrue(loaderVersion >= VK_API_VERSION_1_3, "loader instance version >= 1.3");

    resetVulkanValidationCounters();
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = true;
    auto instance = VulkanInstance::create(instanceDesc);
    if (instance == nullptr || !instance->isValid()) {
        std::printf("SKIP: fuse_rp_volk_loader: no Vulkan instance (%s)\n",
                    instance != nullptr ? instance->info().message.c_str() : "null");
        return kSkip;
    }
    std::printf("instance: %s (layers %zu)\n", instance->info().message.c_str(), instance->info().enabledLayers.size());
    expectMode(vkloader::DispatchMode::Instance, "instance created");
    expectTrue(vkloader::dispatchInstance() == instance->nativeHandle(), "instance table loaded from the live instance");
    expectTrue(vkCreateDevice != nullptr && vkEnumeratePhysicalDevices != nullptr, "instance entry points loaded");

    auto first = VulkanDevice::create(*instance);
    if (first == nullptr || !first->isValid()) {
        std::printf("SKIP: fuse_rp_volk_loader: no Vulkan device (%s)\n",
                    first != nullptr ? first->info().message.c_str() : "null");
        return kSkip;
    }
    std::printf("device: %s\n", first->info().deviceName.c_str());
    {
        // Optional 1.3 feature enabled for the pipeline cache (WP-0.5): on exactly when supported.
        VkPhysicalDeviceVulkan13Features f13{};
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(static_cast<VkPhysicalDevice>(first->nativePhysicalDevice()), &f2);
        const bool supported = first->info().apiVersion >= VK_API_VERSION_1_3 && f13.pipelineCreationCacheControl == VK_TRUE;
        std::printf("pipelineCreationCacheControl: supported %d, enabled %d\n", supported ? 1 : 0,
                    first->info().pipelineCreationCacheControl ? 1 : 0);
        expectTrue(first->info().pipelineCreationCacheControl == supported,
                   "pipelineCreationCacheControl enabled exactly when supported");
    }
    expectDeviceDispatch(*instance, *first, "one device");
    expectFill(*first, 0x11111111u, "one device");

    {
        auto second = VulkanDevice::create(*instance);
        expectTrue(second != nullptr && second->isValid(), "second device created");
        if (second != nullptr && second->isValid()) {
            expectTrampolineDispatch(*instance, "two devices");
            expectFill(*first, 0x22222222u, "two devices, first");
            expectFill(*second, 0x33333333u, "two devices, second");
        }
    }
    expectDeviceDispatch(*instance, *first, "second device destroyed");
    expectFill(*first, 0x44444444u, "survivor after reload");

    // Reload hook: applied at once, re-applied on every reload, removed cleanly.
    u32 installs = 0;
    const u32 reloadsBefore = vkloader::reloadCount();
    vkloader::setReloadHook(&installCountingFill, &installs);
    expectTrue(installs == 1u && reinterpret_cast<void*>(vkCmdFillBuffer) == reinterpret_cast<void*>(&countingFill),
               "hook wraps the loaded table immediately");
    expectTrue(vkloader::reloadCount() == reloadsBefore, "setting a hook does not reload");
    g_fillCalls = 0;
    expectFill(*first, 0x55555555u, "hooked, one device");
    expectTrue(g_fillCalls == 1u, "wrapped vkCmdFillBuffer saw the call (" + std::to_string(g_fillCalls) + ")");
    {
        auto second = VulkanDevice::create(*instance);
        if (second != nullptr && second->isValid()) {
            expectTrue(installs == 2u && g_nextFill == reinterpret_cast<PFN_vkCmdFillBuffer>(
                                                           trampolineFn(*instance, "vkCmdFillBuffer")),
                       "hook re-applied over the trampoline table");
            expectFill(*first, 0x66666666u, "hooked, two devices, first");
            expectFill(*second, 0x77777777u, "hooked, two devices, second");
            expectTrue(g_fillCalls == 3u, "wrapper forwards for both devices");
        }
    }
    expectTrue(installs == 3u && g_nextFill == reinterpret_cast<PFN_vkCmdFillBuffer>(deviceFn(*first, "vkCmdFillBuffer")),
               "hook re-applied over the device table after the reload");
    vkloader::setReloadHook(nullptr, nullptr);
    expectTrue(reinterpret_cast<PFN_vkVoidFunction>(vkCmdFillBuffer) == deviceFn(*first, "vkCmdFillBuffer"),
               "removing the hook restores the unwrapped table");
    expectFill(*first, 0x88888888u, "unhooked");
    expectTrue(g_fillCalls == 3u, "no wrapper after removal");

    first->waitIdle();
    first.reset();
    expectTrampolineDispatch(*instance, "last device destroyed");
    instance.reset();
    expectMode(vkloader::DispatchMode::Global, "instance destroyed");

    const VulkanValidationCounters counters = vulkanValidationCounters();
    expectTrue(counters.errors == 0 && counters.warnings == 0,
               "zero validation messages (errors " + std::to_string(counters.errors) + ", warnings " +
                   std::to_string(counters.warnings) + ": " + counters.lastError + ")");
    return 0;
}

#endif

} // namespace

int main() {
#if defined(FUSE_VULKAN_BACKEND)
    const int rc = runVulkan();
    if (rc == kSkip) {
        return kSkip;
    }
#else
    // Stub build: the loader API compiles and reports nothing.
    expectTrue(!vkloader::initialize(), "stub: no loader");
    expectTrue(vkloader::dispatchMode() == vkloader::DispatchMode::Unavailable, "stub: dispatch unavailable");
    expectTrue(vkloader::loaderInstanceVersion() == 0u && vkloader::reloadCount() == 0u, "stub: no version, no reloads");
    vkloader::registerInstance(nullptr);
    vkloader::unregisterDevice(nullptr);
    if (g_failures != 0) {
        std::printf("fuse_rp_volk_loader: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("SKIP: fuse_rp_volk_loader: Vulkan backend disabled (stub loader API checked)\n");
    return kSkip;
#endif
    if (g_failures != 0) {
        std::printf("fuse_rp_volk_loader: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_rp_volk_loader: OK\n");
    return 0;
}
