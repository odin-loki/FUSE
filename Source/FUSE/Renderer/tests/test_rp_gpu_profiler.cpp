// WP-0.6 GPU-zone gate (Lavapipe, VK_LAYER_KHRONOS_validation with synchronization validation;
// every validation message fails the run).
//
//   * rg::Executor + GpuProfiler::passHooks(): a synthetic graph (transfer passes, one culled) over
//     5 frames; the newest resolved frame has exactly one GPU zone per executed pass, in execution
//     order, named after the pass, with monotonic device timestamps; no frame dropped.
//   * The timestamp-query path without the render graph: nested beginZone/endZone in a caller
//     command buffer, a zone skipped when the ring is full.
//   * Built twice (cmake/rp_wp06.cmake): fuse_rp_gpu_profiler (the configured FUSE_TRACY mode) and
//     fuse_rp_gpu_profiler_tracy (FUSE_TRACY=1: TracyVk contexts are created on the device and the
//     same checks hold with Tracy's zones in the pass hooks).
//
// Exit 77 = skip (stub build, no ICD, no validation layer).
#include <fuse/profiler/profiler.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/gpu_profiler.hpp>
#include <fuse/renderer/vk/instance.hpp>

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

constexpr int kSkip = 77;
int g_failures = 0;

[[maybe_unused]] void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    // The stub backend still links the profiler: create() needs a device, so only the API surface.
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
}

#else

namespace {

using namespace fuse::renderer;
using fuse::u32;
using fuse::u64;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
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

struct FillPass {
    rg::BufferRef buffer;
    u32 value = 0;
};

struct CopyPass {
    rg::BufferRef src;
    rg::BufferRef dst;
};

constexpr u64 kBufferBytes = 4096;

void fillFn(const rg::PassContext& context, void* user) {
    const FillPass& pass = *static_cast<const FillPass*>(user);
    vkCmdFillBuffer(static_cast<VkCommandBuffer>(context.commandBuffer), static_cast<VkBuffer>(context.buffer(pass.buffer)),
                    0, kBufferBytes, pass.value);
}

void copyFn(const rg::PassContext& context, void* user) {
    const CopyPass& pass = *static_cast<const CopyPass*>(user);
    VkBufferCopy region{0, 0, kBufferBytes};
    vkCmdCopyBuffer(static_cast<VkCommandBuffer>(context.commandBuffer), static_cast<VkBuffer>(context.buffer(pass.src)),
                    static_cast<VkBuffer>(context.buffer(pass.dst)), 1, &region);
}

/// fill a -> copy chain a -> b -> c -> ... (kChain copies); an unobserved fill is culled.
constexpr u32 kChain = 6;

struct FrameGraph {
    FillPass fill{};
    FillPass culled{};
    CopyPass copies[kChain]{};
    std::string names[kChain];
};

void buildGraph(rg::Graph& graph, FrameGraph& state) {
    graph.reset();
    rg::BufferRef buffers[kChain + 1];
    for (u32 i = 0; i <= kChain; ++i) {
        buffers[i] = graph.createBuffer({kBufferBytes, 0, "wp06.chain"});
    }
    const rg::BufferRef unused = graph.createBuffer({kBufferBytes, 0, "wp06.unused"});
    state.fill = {buffers[0], 0x5eedu};
    graph.addPass("wp06.fill", &fillFn, &state.fill, rg::QueueClass::Graphics)
        .use(buffers[0], rg::Access::TransferDst);
    state.culled = {unused, 1u};
    graph.addPass("wp06.culled", &fillFn, &state.culled, rg::QueueClass::Graphics).use(unused, rg::Access::TransferDst);
    for (u32 i = 0; i < kChain; ++i) {
        state.copies[i] = {buffers[i], buffers[i + 1]};
        state.names[i] = "wp06.copy." + std::to_string(i);
        rg::PassBuilder pass = graph.addPass(state.names[i].c_str(), &copyFn, &state.copies[i], rg::QueueClass::Graphics);
        pass.use(buffers[i], rg::Access::TransferSrc).use(buffers[i + 1], rg::Access::TransferDst);
        if (i + 1 == kChain) {
            pass.neverCull();
        }
    }
}

int runRenderGraphZones(VulkanDevice& device, GpuAllocator& allocator, bool expectTracy) {
    auto executor = rg::Executor::create(device, &allocator);
    if (executor == nullptr || !executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor: %s\n", executor != nullptr ? executor->message().c_str() : "null");
        return 1;
    }
    GpuProfilerDesc desc{};
    desc.name = "wp06.gpu";
    auto gpu = GpuProfiler::create(device, desc);
    if (gpu == nullptr || !gpu->isValid()) {
        std::fprintf(stderr, "FAIL: GpuProfiler: %s\n", gpu != nullptr ? gpu->message().c_str() : "null");
        return 1;
    }
    expect(gpu->timestampsSupported(rg::QueueClass::Graphics), "graphics queue supports timestamps on LVP");
    expect(gpu->queryPool() != nullptr, "timestamp query pool created");
    if (expectTracy) {
        expect(gpu->tracyContextCount() >= 1u, "TracyVk context created for the graphics queue");
    } else {
        expect(gpu->tracyContextCount() == 0u, "no TracyVk context without the Tracy backend");
    }
    executor->setPassHooks(gpu->passHooks());

    rg::Graph graph;
    FrameGraph state;
    constexpr u32 kFrames = 5;
    rg::ExecuteResult last{};
    std::vector<std::string> lastOrder;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        buildGraph(graph, state);
        gpu->beginFrame();
        last = executor->execute(graph);
        gpu->endFrame();
        if (!last.ok) {
            std::fprintf(stderr, "FAIL: execute frame %u: %s\n", frame, executor->message().c_str());
            return 1;
        }
        gpu->resolve();
        lastOrder.clear();
        for (u32 pass : graph.executionOrder()) {
            lastOrder.emplace_back(graph.passName(pass));
        }
    }
    executor->waitIdle();
    gpu->resolve();

    const GpuFrameTimings& timings = gpu->latest();
    std::printf("rg zones: executed passes %u, zones %zu, frame %llu, skipped %u, resolved %llu, dropped %llu\n",
                last.executedPasses, timings.zones.size(), static_cast<unsigned long long>(timings.frame),
                timings.skippedZones, static_cast<unsigned long long>(gpu->stats().framesResolved),
                static_cast<unsigned long long>(gpu->stats().framesDropped));
    expect(last.executedPasses == kChain + 1u, "the unobserved pass is culled (kChain copies + fill executed)");
    expect(timings.valid && timings.frame == kFrames, "the newest frame resolved after waitIdle");
    expect(timings.zones.size() == last.executedPasses, "GPU zone count equals executed-pass count");
    expect(timings.skippedZones == 0u, "no zone skipped");
    expect(gpu->stats().framesResolved == kFrames, "every frame resolved");
    expect(gpu->stats().framesDropped == 0u, "no frame dropped (ring deeper than the executor's frames in flight)");
    expect(gpu->stats().zonesRecorded == static_cast<u64>(kFrames) * last.executedPasses, "zones recorded per frame");
    for (size_t i = 0; i < timings.zones.size() && i < lastOrder.size(); ++i) {
        const GpuZoneTiming& zone = timings.zones[i];
        expect(lastOrder[i] == zone.name, "zone named after the pass, in execution order");
        expect(zone.endNs >= zone.beginNs, "zone end >= begin");
        expect(zone.depth == 0u, "render-graph zones do not nest");
        if (i > 0) {
            expect(zone.beginNs >= timings.zones[i - 1].beginNs, "zones on one queue are monotonic");
        }
    }
    return 0;
}

int runManualZones(VulkanDevice& device) {
    const VkDevice vkDevice = static_cast<VkDevice>(device.nativeHandle());
    GpuProfilerDesc desc{};
    desc.maxZonesPerFrame = 2; // third zone must be skipped (ring full), not corrupt the frame
    desc.framesInFlight = 2;
    desc.enableTracy = false;
    auto gpu = GpuProfiler::create(device, desc);
    if (gpu == nullptr || !gpu->isValid()) {
        std::fprintf(stderr, "FAIL: GpuProfiler (manual): %s\n", gpu != nullptr ? gpu->message().c_str() : "null");
        return 1;
    }
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // The command buffer is re-begun every frame: implicit reset needs RESET_COMMAND_BUFFER_BIT
    // (VUID-vkBeginCommandBuffer-commandBuffer-00050).
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device.queues().graphicsFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vkDevice, &allocInfo, &cmd);
    const VkQueue queue = static_cast<VkQueue>(device.queues().graphics);

    for (u32 frame = 0; frame < 3; ++frame) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        gpu->beginFrame();
        const u32 outer = gpu->beginZone(cmd, "manual.outer");
        const u32 inner = gpu->beginZone(cmd, "manual.inner");
        const u32 overflow = gpu->beginZone(cmd, "manual.overflow");
        expect(overflow == GpuProfiler::kInvalidZone, "zone beyond maxZonesPerFrame is skipped");
        gpu->endZone(cmd, overflow);
        gpu->endZone(cmd, inner);
        gpu->endZone(cmd, outer);
        gpu->endFrame();
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        expect(gpu->resolve() == 1u, "a completed frame resolves on the next resolve()");
    }
    const GpuFrameTimings& timings = gpu->latest();
    expect(timings.valid && timings.frame == 3u, "manual: newest frame");
    expect(timings.zones.size() == 2u && timings.skippedZones == 1u, "manual: 2 zones recorded, 1 skipped");
    if (timings.zones.size() == 2u) {
        expect(std::strcmp(timings.zones[0].name, "manual.outer") == 0 && timings.zones[0].depth == 0u, "outer zone");
        expect(std::strcmp(timings.zones[1].name, "manual.inner") == 0 && timings.zones[1].depth == 1u, "inner zone");
        expect(timings.zones[0].beginNs <= timings.zones[1].beginNs && timings.zones[1].endNs <= timings.zones[0].endNs,
               "inner zone nested inside the outer zone");
    }
    // Zones outside a frame are rejected, not recorded.
    expect(gpu->beginZone(cmd, "outside") == GpuProfiler::kInvalidZone, "zone outside begin/endFrame skipped");
    gpu.reset();
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    return 0;
}

} // namespace

int main() {
#if defined(FUSE_RP_GPU_PROFILER_EXPECT_TRACY) && FUSE_RP_GPU_PROFILER_EXPECT_TRACY
    constexpr bool kExpectTracy = true;
    static_assert(fuse::profiler::kTracyEnabled, "the _tracy variant must build with FUSE_TRACY=1");
#else
    constexpr bool kExpectTracy = fuse::profiler::kTracyEnabled;
#endif
    if (!layerAvailable(kValidationLayer)) {
        std::printf("SKIP: %s not installed\n", kValidationLayer);
        return kSkip;
    }
    setenv("VK_INSTANCE_LAYERS", kValidationLayer, 1);
    setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
    setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
    setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);

    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_rp_gpu_profiler";
    instanceDesc.enableValidation = true;
    std::unique_ptr<VulkanInstance> instance = VulkanInstance::create(instanceDesc);
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
    if (createMessenger == nullptr || destroyMessenger == nullptr) {
        std::fprintf(stderr, "FAIL: VK_EXT_debug_utils messenger unavailable\n");
        return 1;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = onMessage;
    createMessenger(vkInstance, &info, nullptr, &messenger);

    int rc = 0;
    {
        std::unique_ptr<VulkanDevice> device = VulkanDevice::create(*instance);
        if (device == nullptr || !device->isValid()) {
            std::printf("SKIP: no Vulkan device\n");
            destroyMessenger(vkInstance, messenger, nullptr);
            return kSkip;
        }
        std::unique_ptr<GpuAllocator> allocator = GpuAllocator::create(*device);
        if (allocator == nullptr || !allocator->isValid()) {
            std::fprintf(stderr, "FAIL: GpuAllocator\n");
            rc = 1;
        } else {
            rc = runRenderGraphZones(*device, *allocator, kExpectTracy);
            if (rc == 0) {
                rc = runManualZones(*device);
            }
        }
        device->waitIdle();
        allocator.reset();
        device.reset();
    }
    destroyMessenger(vkInstance, messenger, nullptr);
    instance.reset();

    expect(g_messages == 0u, "zero validation / synchronization-validation messages");
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "%d failure(s), %u validation message(s)\n", g_failures, g_messages);
        return 1;
    }
    std::printf("PASS: GPU zones (Tracy %s), 0 validation messages\n", kExpectTracy ? "on" : "off");
    return 0;
}

#endif
