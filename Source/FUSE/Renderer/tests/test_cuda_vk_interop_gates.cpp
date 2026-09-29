// B2 CUDA <-> Vulkan interop gates (FUSE_MASTER_PLAN B2 rows). One executable, one gate per argument:
//
//   buffer_roundtrip   "Vulkan-allocated external memory buffer reads back identical data when accessed via
//                      CUDA pointer": exportable GpuAllocator buffers of 4 KiB, 1 MiB + 4 B and 64 MiB are
//                      imported with cudaImportExternalMemory. Vulkan writes a full-buffer pattern (copy from
//                      a host-written staging buffer) that a CUDA kernel verifies word by word and the host
//                      verifies again through cudaMemcpy; CUDA writes a second full-buffer pattern that
//                      Vulkan copies to a host-visible buffer for the host to verify. The two directions are
//                      ordered by the SharedTimeline. The buffer is then imported a second time (the exported
//                      handle stays owned by the Vulkan buffer) and read back again.
//   timeline           "SharedTimeline semaphore correctly serialises Vulkan and CUDA execution — no race
//                      conditions under 10k frames": one exported timeline semaphore orders a Vulkan compute
//                      lane and a CUDA kernel lane over one shared 4 MiB buffer for 10 000 frames (protocol in
//                      cuda_vk_interop_gate_common.hpp). Every frame each lane verifies all words the other
//                      lane wrote in the previous step and logs the other lane's frame counter; the host
//                      checks every frame's log, the final contents and the final timeline value.
//   timeline_cpu_lane  The same protocol and Vulkan shader with the peer lane run by the host (vkWaitSemaphores /
//                      vkSignalSemaphore on the same SharedTimeline, the same peerLaneStep code the CUDA kernel
//                      runs) — proves the protocol, the shader and the checks without a CUDA device (Lavapipe).
//   surface_composite  "CUDA surface write to shared texture appears correctly in Vulkan composite pass": an
//                      exportable RGBA8 image is imported as a CUDA mipmapped array + surface object; per frame
//                      CUDA surf2Dwrite()s a frame-dependent pattern (after Vulkan moves the image to GENERAL,
//                      ordered by the SharedTimeline), Vulkan moves it to SHADER_READ_ONLY and runs the real
//                      composite pass (CompositeGpuPath pipeline, bindless heap, composite.frag) blending it with
//                      a raster image at alpha 0 / 0.5 / 1 / 0.25; the composite output is read back and every
//                      pixel must equal mix(cuda pattern, raster, alpha).
//
// The CUDA gates exit 77 (SKIP) without FUSE_BUILD_CUDA, without a CUDA device, without a Vulkan device whose
// UUID matches a CUDA device, or without external-memory interop. Frame count: FUSE_CUDA_VK_TIMELINE_FRAMES.

#include "b5_rhi_test_common.hpp"
#include "cuda_vk_interop_gate_common.hpp"

#include <fuse/core/init.hpp>
#include <fuse/renderer/cuda/interop.hpp>
#include <fuse/renderer/cuda/vk_sync.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/composite_gpu_path.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>

extern "C" int fuse_cvk_peer_lane(void* words, fuse::u32 frame, fuse::u32 count, fuse::u32* log, void* stream);
extern "C" int fuse_cvk_write_pattern(void* data, fuse::u32 count, fuse::u32 frame, fuse::u32 lane, void* stream);
extern "C" int fuse_cvk_count_mismatches(const void* data, fuse::u32 count, fuse::u32 frame, fuse::u32 lane,
                                         unsigned long long* mismatches, fuse::u32* firstBad, void* stream);
extern "C" int fuse_cvk_surface_pattern(void* surfaceObject, fuse::u32 width, fuse::u32 height, fuse::u32 frame,
                                        void* stream);
#endif

namespace {

using b5rhi::expectTrue;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
namespace gate = fuse::cuda_vk_gate;
namespace rhi = fuse::renderer;
namespace rcuda = fuse::renderer::cuda;

constexpr int kSkip = b5rhi::kSkipReturnCode;

[[maybe_unused]] u32 timelineFrames() {
    if (const char* env = std::getenv("FUSE_CUDA_VK_TIMELINE_FRAMES")) {
        const long value = std::strtol(env, nullptr, 10);
        if (value > 0) {
            return static_cast<u32>(value);
        }
    }
    return 10000u;
}

#if defined(FUSE_VULKAN_BACKEND)

constexpr u64 kWaitTimeoutNs = 30ull * 1000ull * 1000ull * 1000ull;

[[maybe_unused]] rhi::BufferUsage operator|(rhi::BufferUsage a, rhi::BufferUsage b) {
    return static_cast<rhi::BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[maybe_unused]] rhi::ImageUsage operator|(rhi::ImageUsage a, rhi::ImageUsage b) {
    return static_cast<rhi::ImageUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}

/// Vulkan instance + device + allocator + a resettable command pool on the graphics queue.
struct VkGateContext {
    std::unique_ptr<rhi::VulkanInstance> instance;
    std::unique_ptr<rhi::VulkanDevice> device;
    std::unique_ptr<rhi::GpuAllocator> allocator;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;

    ~VkGateContext() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
            if (pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(vkDevice, pool, nullptr);
            }
        }
        allocator.reset();
        device.reset();
        instance.reset();
    }
};

/// Returns an empty string on success, else why the Vulkan side is unavailable.
[[maybe_unused]] std::string makeContext(VkGateContext& ctx) {
    rhi::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;
    ctx.instance = rhi::VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr) {
        return "no Vulkan instance";
    }
    ctx.device = rhi::VulkanDevice::create(*ctx.instance);
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        return "no Vulkan device";
    }
    if (!ctx.device->info().timelineSemaphore) {
        return "Vulkan device without timeline semaphores";
    }
    ctx.allocator = rhi::GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        return "no GpuAllocator";
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.physicalDevice = static_cast<VkPhysicalDevice>(ctx.device->nativePhysicalDevice());
    ctx.queue = static_cast<VkQueue>(ctx.device->queues().graphics);
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = ctx.device->queues().graphicsFamily;
    if (ctx.queue == VK_NULL_HANDLE || vkCreateCommandPool(ctx.vkDevice, &poolInfo, nullptr, &ctx.pool) != VK_SUCCESS) {
        return "command pool creation failed";
    }
    std::printf("Vulkan device: %s\n", ctx.device->info().deviceName.c_str());
    return {};
}

[[maybe_unused]] VkCommandBuffer allocateCommandBuffer(const VkGateContext& ctx) {
    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = ctx.pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    return vkAllocateCommandBuffers(ctx.vkDevice, &info, &cb) == VK_SUCCESS ? cb : VK_NULL_HANDLE;
}

[[maybe_unused]] bool beginCommands(VkCommandBuffer cb) {
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS;
}

/// vkQueueSubmit that waits for `timeline >= waitValue` at `waitStage` and signals `timeline = signalValue`.
[[maybe_unused]] bool submitTimeline(const VkGateContext& ctx, VkCommandBuffer cb, VkSemaphore timeline, u64 waitValue,
                    VkPipelineStageFlags waitStage, u64 signalValue) {
    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.waitSemaphoreValueCount = 1;
    timelineInfo.pWaitSemaphoreValues = &waitValue;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &signalValue;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &timelineInfo;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &timeline;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &timeline;
    return vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
}

/// Plain submit + queue idle (setup work outside the timeline protocol).
[[maybe_unused]] bool submitAndWait(const VkGateContext& ctx, VkCommandBuffer cb) {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    return vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(ctx.queue) == VK_SUCCESS;
}

/// Host wait with a timeout (a lost signal fails the gate instead of hanging it).
[[maybe_unused]] bool waitTimeline(const VkGateContext& ctx, VkSemaphore timeline, u64 value) {
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &timeline;
    waitInfo.pValues = &value;
    return vkWaitSemaphores(ctx.vkDevice, &waitInfo, kWaitTimeoutNs) == VK_SUCCESS;
}

[[maybe_unused]] u64 timelineValue(const VkGateContext& ctx, VkSemaphore timeline) {
    u64 value = 0;
    (void)vkGetSemaphoreCounterValue(ctx.vkDevice, timeline, &value);
    return value;
}

[[maybe_unused]] void memoryBarrier(VkCommandBuffer cb, VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                   VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

[[maybe_unused]] void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage,
                  VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

[[maybe_unused]] std::vector<u32> readSpirv(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamsize bytes = file.tellg();
    if (bytes <= 0 || (bytes % 4) != 0) {
        return {};
    }
    std::vector<u32> words(static_cast<usize>(bytes) / 4u);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), bytes);
    return file ? words : std::vector<u32>{};
}

/// The Vulkan lane of the timeline gate: compute pipeline over {shared words, lane log}.
struct LanePipeline {
    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;

    ~LanePipeline() {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
    }

    bool create(VkDevice vkDevice, const std::vector<u32>& spirv, VkBuffer words, VkBuffer log) {
        device = vkDevice;
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = spirv.size() * sizeof(u32);
        moduleInfo.pCode = spirv.data();
        if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            return false;
        }
        VkDescriptorSetLayoutBinding bindings[2]{};
        for (u32 b = 0; b < 2u; ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = 2;
        setInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout) != VK_SUCCESS) {
            return false;
        }
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = 2u * sizeof(u32);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
            return false;
        }
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = layout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
            return false;
        }
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 2;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
            return false;
        }
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &setLayout;
        if (vkAllocateDescriptorSets(device, &allocInfo, &set) != VK_SUCCESS) {
            return false;
        }
        VkDescriptorBufferInfo bufferInfos[2]{};
        bufferInfos[0].buffer = words;
        bufferInfos[0].range = VK_WHOLE_SIZE;
        bufferInfos[1].buffer = log;
        bufferInfos[1].range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet writes[2]{};
        for (u32 b = 0; b < 2u; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &bufferInfos[b];
        }
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        return true;
    }
};

#if defined(FUSE_HAS_CUDA)
/// CUDA device check first (cheap, no Vulkan), then the Vulkan device, UUID match and interop runtime.
/// Returns an empty string when the CUDA gates can run.
std::string prepareCudaGate(VkGateContext& ctx) {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) {
        (void)cudaGetLastError();
        return "no CUDA device";
    }
    const std::string why = makeContext(ctx);
    if (!why.empty()) {
        return why;
    }
    // interopAvailable() initialises the CUDA job lane once (cudaSetDevice(0)); select the device that
    // matches the Vulkan device only afterwards so that choice sticks.
    if (!rcuda::interopAvailable()) {
        return rcuda::interopUnavailableReasonString(rcuda::interopUnavailableReason());
    }
    const int cudaDevice = rcuda::cuda_device_for_vulkan(ctx.physicalDevice);
    if (cudaDevice < 0) {
        return "the Vulkan device has no CUDA device with the same UUID";
    }
    if (cudaSetDevice(cudaDevice) != cudaSuccess) {
        return "cudaSetDevice failed";
    }
    cudaDeviceProp prop{};
    (void)cudaGetDeviceProperties(&prop, cudaDevice);
    std::printf("CUDA device %d: %s (matches the Vulkan device UUID)\n", cudaDevice, prop.name);
    return {};
}

bool cudaOk(int err, const char* what) {
    if (err != 0) {
        std::fprintf(stderr, "FAIL: %s: %s\n", what, cudaGetErrorString(static_cast<cudaError_t>(err)));
        ++b5rhi::failures();
        return false;
    }
    return true;
}
#endif

// ---- timeline (CUDA lane) and timeline_cpu_lane ------------------------------------------------------

int gateTimeline(bool cpuLane) {
    const char* name = cpuLane ? "fuse_cuda_vk_timeline_10k_cpu_lane" : "fuse_cuda_vk_timeline_10k";
#if !defined(FUSE_CUDA_VK_LANE_SPV)
    return b5rhi::skip(name, "cuda_vk_timeline_lane.comp was not compiled (glslangValidator not found)");
#else
    VkGateContext ctx;
    if (cpuLane) {
        const std::string why = makeContext(ctx);
        if (!why.empty()) {
            return b5rhi::skip(name, why.c_str());
        }
    } else {
#if !defined(FUSE_HAS_CUDA)
        return b5rhi::skip(name, "built without CUDA (FUSE_BUILD_CUDA=OFF)");
#else
        const std::string why = prepareCudaGate(ctx);
        if (!why.empty()) {
            return b5rhi::skip(name, why.c_str());
        }
#endif
    }
    const std::vector<u32> spirv = readSpirv(FUSE_CUDA_VK_LANE_SPV);
    expectTrue(!spirv.empty(), "timeline lane SPIR-V loaded");
    if (spirv.empty()) {
        return b5rhi::finish(name);
    }

    const u32 frames = timelineFrames();
    const u32 count = cpuLane ? 4096u : (1u << 20);
    // CPU lane self-test (ctest fuse_cuda_vk_timeline_cpu_lane_detects_fault): corrupt one word after the
    // peer step of this frame; the gate must report exactly one bad word in the next Vulkan frame.
    const char* injectEnv = cpuLane ? std::getenv("FUSE_CUDA_VK_TIMELINE_INJECT_FRAME") : nullptr;
    const u32 injectFrame = injectEnv != nullptr ? static_cast<u32>(std::strtoul(injectEnv, nullptr, 10)) : 0u;
    const usize wordBytes = (static_cast<usize>(gate::kHeaderWords) + count) * sizeof(u32);
    const usize logBytes = (static_cast<usize>(frames) + 1u) * gate::kLogWordsPerFrame * sizeof(u32);

    rcuda::SharedTimeline timeline = rcuda::SharedTimeline::create(ctx.vkDevice, ctx.physicalDevice);
    expectTrue(timeline.valid, "SharedTimeline created");
    if (!cpuLane) {
        expectTrue(timeline.driverWired, "SharedTimeline imported into CUDA (cudaImportExternalSemaphore)");
    }
    if (!timeline.valid || (!cpuLane && !timeline.driverWired)) {
        std::fprintf(stderr, "SharedTimeline: %s\n", timeline.message != nullptr ? timeline.message : "?");
        timeline.destroy(ctx.vkDevice);
        return b5rhi::finish(name);
    }
    const VkSemaphore semaphore = static_cast<VkSemaphore>(timeline.vkSemaphore);

    rhi::BufferDesc wordsDesc{};
    wordsDesc.size = wordBytes;
    wordsDesc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst;
    wordsDesc.memoryUsage = cpuLane ? rhi::MemoryUsage::GpuToCpu : rhi::MemoryUsage::GpuOnly;
    wordsDesc.cudaInterop = !cpuLane;
    wordsDesc.name = "fuse.gate.timeline.words";
    rhi::BufferDesc logDesc{};
    logDesc.size = logBytes;
    logDesc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst;
    logDesc.memoryUsage = rhi::MemoryUsage::GpuToCpu;
    logDesc.name = "fuse.gate.timeline.vk_log";
    rhi::Buffer words{};
    rhi::Buffer vkLog{};
    const bool buffersOk = ctx.allocator->createBuffer(wordsDesc, words) && ctx.allocator->createBuffer(logDesc, vkLog) &&
                           vkLog.mapped != nullptr && (!cpuLane || words.mapped != nullptr);
    expectTrue(buffersOk, "shared word buffer + host-visible Vulkan lane log");

    void* peerWords = nullptr; // CUDA pointer (CUDA lane) or host mapping (CPU lane)
    std::vector<u32> peerLogHost;
    [[maybe_unused]] u32* peerLogDevice = nullptr; // CUDA lane only
    rcuda::CudaBufferImport imported{};
    [[maybe_unused]] void* stream = nullptr;       // cudaStream_t, CUDA lane only
    if (buffersOk && cpuLane) {
        peerWords = words.mapped;
        peerLogHost.assign(logBytes / sizeof(u32), 0u);
    }
#if defined(FUSE_HAS_CUDA)
    if (buffersOk && !cpuLane) {
        expectTrue(words.exportedHandle != nullptr, "shared word buffer exported a platform handle");
        imported = rcuda::import_vulkan_buffer(ctx.vkDevice, words);
        expectTrue(imported.ok, imported.reason != nullptr ? imported.reason : "import_vulkan_buffer");
        peerWords = imported.ok ? imported.devicePtr : nullptr;
        cudaStream_t cudaStream = nullptr;
        (void)cudaOk(cudaStreamCreateWithFlags(&cudaStream, cudaStreamNonBlocking), "cudaStreamCreate");
        stream = cudaStream;
        (void)cudaOk(cudaMalloc(reinterpret_cast<void**>(&peerLogDevice), logBytes), "cudaMalloc peer log");
        (void)cudaOk(cudaMemset(peerLogDevice, 0, logBytes), "cudaMemset peer log");
    }
#endif

    LanePipeline lane;
    const bool pipelineOk = buffersOk && peerWords != nullptr &&
                            lane.create(ctx.vkDevice, spirv, static_cast<VkBuffer>(words.handle),
                                        static_cast<VkBuffer>(vkLog.handle));
    expectTrue(pipelineOk, "Vulkan lane compute pipeline");

    constexpr u32 kInFlight = 4;
    VkCommandBuffer commandBuffers[kInFlight]{};
    for (VkCommandBuffer& cb : commandBuffers) {
        cb = allocateCommandBuffer(ctx);
    }

    u32 framesRun = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (u32 f = 1; pipelineOk && f <= frames && b5rhi::failures() == 0; ++f) {
        if (f > kInFlight && !waitTimeline(ctx, semaphore, 2ull * (f - kInFlight))) {
            expectTrue(false, "timeline wait for a free command buffer timed out");
            break;
        }
        VkCommandBuffer cb = commandBuffers[f % kInFlight];
        beginCommands(cb);
        if (f == 1u) {
            vkCmdFillBuffer(cb, static_cast<VkBuffer>(words.handle), 0, VK_WHOLE_SIZE, 0u);
            vkCmdFillBuffer(cb, static_cast<VkBuffer>(vkLog.handle), 0, VK_WHOLE_SIZE, 0u);
            memoryBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lane.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lane.layout, 0, 1, &lane.set, 0, nullptr);
        const u32 push[2] = {f, count};
        vkCmdPushConstants(cb, lane.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cb, (count + gate::kLaneWorkgroup - 1u) / gate::kLaneWorkgroup, 1, 1);
        memoryBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                      VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT);
        vkEndCommandBuffer(cb);
        if (!submitTimeline(ctx, cb, semaphore, 2ull * f - 2u, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 2ull * f - 1u)) {
            expectTrue(false, "Vulkan lane submit");
            break;
        }

        if (cpuLane) {
            if (!waitTimeline(ctx, semaphore, 2ull * f - 1u)) {
                expectTrue(false, "CPU lane wait for the Vulkan lane timed out");
                break;
            }
            auto* w = static_cast<u32*>(peerWords);
            peerLogHost[f * gate::kLogWordsPerFrame] = w[gate::kVulkanFrameWord];
            w[gate::kPeerFrameWord] = f;
            u32 bad = 0;
            for (u32 i = 0; i < count; ++i) {
                bad += gate::peerLaneStep(w, f, i);
            }
            peerLogHost[f * gate::kLogWordsPerFrame + 1u] = bad;
            if (f == injectFrame) {
                w[gate::kHeaderWords + 7u] ^= 0x10u; // fault injection: the next Vulkan lane frame must see it
            }
            if (!timeline.signalVulkan(ctx.vkDevice, 2ull * f)) {
                expectTrue(false, "CPU lane vkSignalSemaphore");
                break;
            }
        } else {
#if defined(FUSE_HAS_CUDA)
            if (!timeline.waitCuda(stream, 2ull * f - 1u)) {
                expectTrue(false, "cudaWaitExternalSemaphoresAsync");
                break;
            }
            if (!cudaOk(fuse_cvk_peer_lane(peerWords, f, count, peerLogDevice, stream), "CUDA lane kernel launch")) {
                break;
            }
            if (!timeline.signalCuda(stream, 2ull * f)) {
                expectTrue(false, "cudaSignalExternalSemaphoresAsync");
                break;
            }
#endif
        }
        framesRun = f;
    }
    const bool drained = pipelineOk && framesRun > 0u && waitTimeline(ctx, semaphore, 2ull * framesRun);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    vkQueueWaitIdle(ctx.queue);
#if defined(FUSE_HAS_CUDA)
    if (stream != nullptr) {
        (void)cudaOk(cudaStreamSynchronize(static_cast<cudaStream_t>(stream)), "cudaStreamSynchronize");
    }
#endif
    expectTrue(drained, "timeline reached 2F at the end");
    expectTrue(framesRun == frames, "every frame ran");

    if (drained) {
        std::vector<u32> finalWords(wordBytes / sizeof(u32), 0u);
        std::vector<u32> peerLog = peerLogHost;
        if (cpuLane) {
            std::memcpy(finalWords.data(), words.mapped, wordBytes);
        }
#if defined(FUSE_HAS_CUDA)
        if (!cpuLane) {
            peerLog.assign(logBytes / sizeof(u32), 0u);
            (void)cudaOk(cudaMemcpy(peerLog.data(), peerLogDevice, logBytes, cudaMemcpyDeviceToHost), "peer log D2H");
            (void)cudaOk(cudaMemcpy(finalWords.data(), peerWords, wordBytes, cudaMemcpyDeviceToHost), "final words D2H");
        }
#endif
        const auto* vkLogWords = static_cast<const u32*>(vkLog.mapped);
        u32 badFrames = 0;
        for (u32 f = 1; f <= framesRun; ++f) {
            const u32 vkSaw = vkLogWords[f * gate::kLogWordsPerFrame];
            const u32 vkBad = vkLogWords[f * gate::kLogWordsPerFrame + 1u];
            const u32 peerSaw = peerLog[f * gate::kLogWordsPerFrame];
            const u32 peerBad = peerLog[f * gate::kLogWordsPerFrame + 1u];
            if (vkSaw != f - 1u || vkBad != 0u || peerSaw != f || peerBad != 0u) {
                if (badFrames < 8u) {
                    std::fprintf(stderr,
                                 "frame %u: Vulkan lane saw peer frame %u (want %u), %u bad words; peer lane saw Vulkan "
                                 "frame %u (want %u), %u bad words\n",
                                 f, vkSaw, f - 1u, vkBad, peerSaw, f, peerBad);
                }
                ++badFrames;
            }
        }
        u32 badFinal = 0;
        for (u32 i = 0; i < count; ++i) {
            badFinal += finalWords[gate::kHeaderWords + i] != gate::laneValue(framesRun, gate::kLanePeer, i) ? 1u : 0u;
        }
        std::printf("%s: %u frames x %u words in %.1f ms (%.1f us/frame), %u frame(s) with a race, %u bad final words, "
                    "timeline value %llu\n",
                    name, framesRun, count, ms, 1000.0 * ms / framesRun, badFrames, badFinal,
                    static_cast<unsigned long long>(timelineValue(ctx, semaphore)));
        expectTrue(badFrames == 0u, "every frame: each lane saw exactly the other lane's previous step (no race)");
        expectTrue(badFinal == 0u, "final buffer holds the peer lane's last frame everywhere");
        expectTrue(finalWords[gate::kVulkanFrameWord] == framesRun && finalWords[gate::kPeerFrameWord] == framesRun,
                   "both frame words end at F");
        expectTrue(timelineValue(ctx, semaphore) == 2ull * framesRun, "timeline value ends at 2F");
    }

    vkDeviceWaitIdle(ctx.vkDevice);
#if defined(FUSE_HAS_CUDA)
    if (peerLogDevice != nullptr) {
        (void)cudaFree(peerLogDevice);
    }
    if (stream != nullptr) {
        (void)cudaStreamDestroy(static_cast<cudaStream_t>(stream));
    }
#endif
    rcuda::release_imported_buffer(imported);
    ctx.allocator->destroyBuffer(words);
    ctx.allocator->destroyBuffer(vkLog);
    for (VkCommandBuffer cb : commandBuffers) {
        if (cb != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(ctx.vkDevice, ctx.pool, 1, &cb);
        }
    }
    timeline.destroy(ctx.vkDevice);
    return b5rhi::finish(name);
#endif
}

// ---- buffer_roundtrip ---------------------------------------------------------------------------------

int gateBufferRoundtrip() {
    const char* name = "fuse_cuda_vk_buffer_roundtrip";
#if !defined(FUSE_HAS_CUDA)
    return b5rhi::skip(name, "built without CUDA (FUSE_BUILD_CUDA=OFF)");
#else
    VkGateContext ctx;
    const std::string why = prepareCudaGate(ctx);
    if (!why.empty()) {
        return b5rhi::skip(name, why.c_str());
    }
    rcuda::SharedTimeline timeline = rcuda::SharedTimeline::create(ctx.vkDevice, ctx.physicalDevice);
    expectTrue(timeline.valid && timeline.driverWired, "SharedTimeline imported into CUDA");
    if (!timeline.valid || !timeline.driverWired) {
        timeline.destroy(ctx.vkDevice);
        return b5rhi::finish(name);
    }
    const VkSemaphore semaphore = static_cast<VkSemaphore>(timeline.vkSemaphore);
    VkCommandBuffer cb = allocateCommandBuffer(ctx);
    cudaStream_t stream = nullptr;
    (void)cudaOk(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreate");
    unsigned long long* mismatches = nullptr;
    u32* firstBad = nullptr;
    (void)cudaOk(cudaMalloc(reinterpret_cast<void**>(&mismatches), sizeof(unsigned long long)), "cudaMalloc counter");
    (void)cudaOk(cudaMalloc(reinterpret_cast<void**>(&firstBad), sizeof(u32)), "cudaMalloc first-bad");

    const usize sizes[] = {4096u, (1u << 20) + 4u, 64u << 20};
    u64 timelineValueNow = 0;
    u32 frame = 0;
    for (const usize size : sizes) {
        ++frame;
        const u32 count = static_cast<u32>(size / sizeof(u32));
        rhi::BufferDesc sharedDesc{};
        sharedDesc.size = size;
        sharedDesc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst;
        sharedDesc.memoryUsage = rhi::MemoryUsage::GpuOnly;
        sharedDesc.cudaInterop = true;
        sharedDesc.name = "fuse.gate.roundtrip.shared";
        rhi::BufferDesc stagingDesc{};
        stagingDesc.size = size;
        stagingDesc.usage = rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst;
        stagingDesc.memoryUsage = rhi::MemoryUsage::GpuToCpu;
        stagingDesc.name = "fuse.gate.roundtrip.staging";
        rhi::Buffer shared{};
        rhi::Buffer staging{};
        const bool created = ctx.allocator->createBuffer(sharedDesc, shared) &&
                             ctx.allocator->createBuffer(stagingDesc, staging) && staging.mapped != nullptr &&
                             shared.exportedHandle != nullptr;
        expectTrue(created, "exportable shared buffer + host-visible staging buffer");
        rcuda::CudaBufferImport imported = created ? rcuda::import_vulkan_buffer(ctx.vkDevice, shared) : rcuda::CudaBufferImport{};
        expectTrue(imported.ok, imported.reason != nullptr ? imported.reason : "import_vulkan_buffer");
        if (created && imported.ok) {
            // 1) Vulkan writes pattern A over the whole buffer; CUDA reads it.
            auto* host = static_cast<u32*>(staging.mapped);
            for (u32 i = 0; i < count; ++i) {
                host[i] = gate::laneValue(frame, gate::kLanePatternA, i);
            }
            beginCommands(cb);
            VkBufferCopy region{0, 0, size};
            vkCmdCopyBuffer(cb, static_cast<VkBuffer>(staging.handle), static_cast<VkBuffer>(shared.handle), 1, &region);
            vkEndCommandBuffer(cb);
            expectTrue(submitTimeline(ctx, cb, semaphore, timelineValueNow, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                      timelineValueNow + 1u),
                       "Vulkan pattern-A copy submitted");
            ++timelineValueNow;
            expectTrue(timeline.waitCuda(stream, timelineValueNow), "CUDA waits for the Vulkan write");
            (void)cudaOk(cudaMemsetAsync(mismatches, 0, sizeof(unsigned long long), stream), "reset counter");
            (void)cudaOk(cudaMemsetAsync(firstBad, 0xff, sizeof(u32), stream), "reset first-bad");
            (void)cudaOk(fuse_cvk_count_mismatches(imported.devicePtr, count, frame, gate::kLanePatternA, mismatches,
                                                   firstBad, stream),
                         "CUDA verify kernel");
            unsigned long long bad = 0;
            u32 first = 0;
            std::vector<u32> viaCuda(count, 0u);
            (void)cudaOk(cudaMemcpyAsync(&bad, mismatches, sizeof(bad), cudaMemcpyDeviceToHost, stream), "counter D2H");
            (void)cudaOk(cudaMemcpyAsync(&first, firstBad, sizeof(first), cudaMemcpyDeviceToHost, stream), "first D2H");
            (void)cudaOk(cudaMemcpyAsync(viaCuda.data(), imported.devicePtr, size, cudaMemcpyDeviceToHost, stream),
                         "whole buffer D2H through the CUDA pointer");
            (void)cudaOk(cudaStreamSynchronize(stream), "stream sync");
            u32 hostBad = 0;
            for (u32 i = 0; i < count; ++i) {
                hostBad += viaCuda[i] != gate::laneValue(frame, gate::kLanePatternA, i) ? 1u : 0u;
            }
            std::printf("  %9llu B Vulkan -> CUDA: kernel %llu bad word(s)%s, cudaMemcpy %u bad word(s)\n",
                        static_cast<unsigned long long>(size), bad, bad != 0u ? " (see first)" : "", hostBad);
            if (bad != 0u) {
                std::fprintf(stderr, "  first bad word %u\n", first);
            }
            expectTrue(bad == 0u && hostBad == 0u, "every word Vulkan wrote reads back identically through the CUDA pointer");

            // 2) CUDA writes pattern B over the whole buffer; Vulkan reads it.
            (void)cudaOk(fuse_cvk_write_pattern(imported.devicePtr, count, frame, gate::kLanePatternB, stream),
                         "CUDA pattern-B kernel");
            ++timelineValueNow;
            expectTrue(timeline.signalCuda(stream, timelineValueNow), "CUDA signals its write");
            beginCommands(cb);
            vkCmdCopyBuffer(cb, static_cast<VkBuffer>(shared.handle), static_cast<VkBuffer>(staging.handle), 1, &region);
            memoryBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          VK_ACCESS_HOST_READ_BIT);
            vkEndCommandBuffer(cb);
            expectTrue(submitTimeline(ctx, cb, semaphore, timelineValueNow, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      timelineValueNow + 1u),
                       "Vulkan read-back copy submitted");
            ++timelineValueNow;
            const bool done = waitTimeline(ctx, semaphore, timelineValueNow);
            expectTrue(done, "Vulkan read-back finished");
            u32 vkBad = 0;
            for (u32 i = 0; done && i < count; ++i) {
                vkBad += host[i] != gate::laneValue(frame, gate::kLanePatternB, i) ? 1u : 0u;
            }
            std::printf("  %9llu B CUDA -> Vulkan: %u bad word(s)\n", static_cast<unsigned long long>(size), vkBad);
            expectTrue(done && vkBad == 0u, "every word CUDA wrote reads back identically through Vulkan");
            rcuda::release_imported_buffer(imported);

            // 3) Import again: the exported handle still belongs to the Vulkan buffer.
            rcuda::CudaBufferImport again = rcuda::import_vulkan_buffer(ctx.vkDevice, shared);
            expectTrue(again.ok, "second import of the same exported handle");
            if (again.ok) {
                (void)cudaOk(cudaMemsetAsync(mismatches, 0, sizeof(unsigned long long), stream), "reset counter");
                (void)cudaOk(cudaMemsetAsync(firstBad, 0xff, sizeof(u32), stream), "reset first-bad");
                (void)cudaOk(fuse_cvk_count_mismatches(again.devicePtr, count, frame, gate::kLanePatternB, mismatches,
                                                       firstBad, stream),
                             "CUDA verify kernel (second import)");
                (void)cudaOk(cudaMemcpyAsync(&bad, mismatches, sizeof(bad), cudaMemcpyDeviceToHost, stream), "counter D2H");
                (void)cudaOk(cudaStreamSynchronize(stream), "stream sync");
                expectTrue(bad == 0u, "second import sees the same memory");
            }
            rcuda::release_imported_buffer(again);
        }
        rcuda::release_imported_buffer(imported);
        vkDeviceWaitIdle(ctx.vkDevice);
        ctx.allocator->destroyBuffer(shared);
        ctx.allocator->destroyBuffer(staging);
    }
    (void)cudaFree(mismatches);
    (void)cudaFree(firstBad);
    (void)cudaStreamDestroy(stream);
    vkFreeCommandBuffers(ctx.vkDevice, ctx.pool, 1, &cb);
    timeline.destroy(ctx.vkDevice);
    return b5rhi::finish(name);
#endif
}

// ---- surface_composite --------------------------------------------------------------------------------

int gateSurfaceComposite() {
    const char* name = "fuse_cuda_vk_surface_composite";
#if !defined(FUSE_HAS_CUDA)
    return b5rhi::skip(name, "built without CUDA (FUSE_BUILD_CUDA=OFF)");
#else
    VkGateContext ctx;
    const std::string why = prepareCudaGate(ctx);
    if (!why.empty()) {
        return b5rhi::skip(name, why.c_str());
    }
    constexpr u32 kW = 256;
    constexpr u32 kH = 160;
    constexpr u8 kRaster[4] = {200, 40, 120, 255};

    rhi::CompositeGpuPathDesc compositeDesc{};
    compositeDesc.width = kW;
    compositeDesc.height = kH;
    compositeDesc.vertexSpirvPath = FUSE_SHADER_FIXTURE_DIR "/composite.vert.spv";
    compositeDesc.fragmentSpirvPath = FUSE_SHADER_FIXTURE_DIR "/composite.frag.spv";
    std::unique_ptr<rhi::CompositeGpuPath> composite = rhi::CompositeGpuPath::create(*ctx.device, compositeDesc);
    expectTrue(composite != nullptr && composite->isReady(), "composite GPU path ready");

    rhi::TextureDesc cudaDesc{};
    cudaDesc.width = kW;
    cudaDesc.height = kH;
    cudaDesc.format = rhi::GpuFormat::R8G8B8A8Unorm;
    cudaDesc.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::Storage | rhi::ImageUsage::TransferSrc |
                     rhi::ImageUsage::TransferDst;
    cudaDesc.cudaInterop = true;
    cudaDesc.name = "fuse.gate.cuda_surface";
    rhi::TextureDesc rasterDesc = cudaDesc;
    rasterDesc.usage = rhi::ImageUsage::Sampled | rhi::ImageUsage::TransferDst;
    rasterDesc.cudaInterop = false;
    rasterDesc.name = "fuse.gate.raster";
    rhi::Texture cudaTex{};
    rhi::Texture raster{};
    const bool imagesOk = composite != nullptr && composite->isReady() && ctx.allocator->createImage(cudaDesc, cudaTex) &&
                          ctx.allocator->createImage(rasterDesc, raster) && cudaTex.exportedHandle != nullptr;
    expectTrue(imagesOk, "exportable CUDA image + raster image");

    rcuda::CudaSurfaceImport surface = imagesOk ? rcuda::import_vulkan_image(ctx.vkDevice, cudaTex) : rcuda::CudaSurfaceImport{};
    expectTrue(surface.ok, surface.reason != nullptr ? surface.reason : "import_vulkan_image");
    if (surface.ok) {
        std::printf("imported Vulkan image as CUDA mipmapped array (flags 0x%x%s)\n", surface.arrayFlags,
                    (surface.arrayFlags & cudaArraySurfaceLoadStore) != 0u ? ", SurfaceLoadStore" : "");
    }
    const bool registered = surface.ok && composite->registerRasterSource(raster.view) &&
                            composite->registerCudaSource(cudaTex.view);
    expectTrue(!surface.ok || registered, "raster + CUDA images registered in the composite bindless heap");

    rcuda::SharedTimeline timeline = rcuda::SharedTimeline::create(ctx.vkDevice, ctx.physicalDevice);
    expectTrue(timeline.valid && timeline.driverWired, "SharedTimeline imported into CUDA");
    const bool ready = registered && timeline.valid && timeline.driverWired;
    const VkSemaphore semaphore = static_cast<VkSemaphore>(timeline.vkSemaphore);
    VkCommandBuffer cb = allocateCommandBuffer(ctx);
    cudaStream_t stream = nullptr;
    (void)cudaOk(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "cudaStreamCreate");

    if (ready) {
        // Raster source: constant colour, left in SHADER_READ_ONLY (what the bindless descriptor says).
        beginCommands(cb);
        imageBarrier(cb, static_cast<VkImage>(raster.image), VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue clear{};
        for (int c = 0; c < 4; ++c) {
            clear.float32[c] = static_cast<float>(kRaster[c]) / 255.f;
        }
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, static_cast<VkImage>(raster.image), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                             &range);
        imageBarrier(cb, static_cast<VkImage>(raster.image), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkEndCommandBuffer(cb);
        expectTrue(submitAndWait(ctx, cb), "raster source cleared");
    }

    const float alphas[4] = {0.f, 0.5f, 1.f, 0.25f};
    constexpr u32 kFrames = 8;
    VkImageLayout cudaLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::vector<u8> previous;
    for (u32 f = 1; ready && f <= kFrames && b5rhi::failures() == 0; ++f) {
        const float alpha = alphas[(f - 1u) % 4u];
        const u64 base = 3ull * (f - 1u);
        // Vulkan: hand the image to CUDA in GENERAL.
        beginCommands(cb);
        imageBarrier(cb, static_cast<VkImage>(cudaTex.image), cudaLayout, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        vkEndCommandBuffer(cb);
        expectTrue(submitTimeline(ctx, cb, semaphore, base, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, base + 1u),
                   "layout -> GENERAL submitted");
        // CUDA: surface write of this frame's pattern.
        expectTrue(timeline.waitCuda(stream, base + 1u), "CUDA waits for the GENERAL transition");
        (void)cudaOk(fuse_cvk_surface_pattern(surface.surfaceObject, kW, kH, f, stream), "surf2Dwrite kernel");
        expectTrue(timeline.signalCuda(stream, base + 2u), "CUDA signals the surface write");
        // Vulkan: back to SHADER_READ_ONLY, then the composite pass.
        if (!waitTimeline(ctx, semaphore, base + 1u)) { // the command buffer is reused
            expectTrue(false, "layout submit finished");
            break;
        }
        rhi::VkFrameEncodeContext encode{};
        encode.active = true;
        composite->fillEncodeContext(encode, alpha, false);
        expectTrue(encode.compositeActive, "composite encode context active");
        if (!encode.compositeActive) {
            break;
        }
        beginCommands(cb);
        imageBarrier(cb, static_cast<VkImage>(cudaTex.image), VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = static_cast<VkRenderPass>(encode.compositeRenderPass);
        pass.framebuffer = static_cast<VkFramebuffer>(encode.compositeFramebuffer);
        pass.renderArea.extent = {encode.compositeWidth, encode.compositeHeight};
        VkClearValue clearValue{};
        pass.clearValueCount = 1;
        pass.pClearValues = &clearValue;
        vkCmdBeginRenderPass(cb, &pass, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0.f, 0.f, static_cast<float>(encode.compositeWidth),
                                  static_cast<float>(encode.compositeHeight), 0.f, 1.f};
        const VkRect2D scissor{{0, 0}, {encode.compositeWidth, encode.compositeHeight}};
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, static_cast<VkPipeline>(encode.compositePipeline));
        const VkPipelineLayout layout = static_cast<VkPipelineLayout>(encode.compositePipelineLayout);
        const VkDescriptorSet bindless = static_cast<VkDescriptorSet>(encode.bindlessDescriptorSet);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &bindless, 0, nullptr);
        struct {
            float blend;
            u32 rasterTexIndex;
            u32 cudaTexIndex;
            u32 depthTexIndex;
        } push{alpha, encode.rasterTextureBindlessIndex, encode.cudaTextureBindlessIndex, encode.depthTextureBindlessIndex};
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        const VkBuffer vertexBuffer = static_cast<VkBuffer>(encode.compositeVertexBuffer);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &offset);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        vkEndCommandBuffer(cb);
        expectTrue(submitTimeline(ctx, cb, semaphore, base + 2u, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, base + 3u),
                   "composite pass submitted after the CUDA write");
        if (!waitTimeline(ctx, semaphore, base + 3u)) {
            expectTrue(false, "composite pass finished");
            break;
        }
        cudaLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (encode.compositeTargetLayout != nullptr) {
            *encode.compositeTargetLayout = static_cast<u32>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }

        std::vector<u8> rgba;
        expectTrue(composite->readbackOutput(rgba) && rgba.size() == static_cast<usize>(kW) * kH * 4u,
                   "composite output read back");
        if (rgba.size() != static_cast<usize>(kW) * kH * 4u) {
            break;
        }
        const int tolerance = (alpha == 0.f || alpha == 1.f) ? 1 : 2;
        u32 bad = 0;
        int worst = 0;
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                u8 texel[4];
                gate::surfacePattern(f, x, y, texel);
                const u8* got = &rgba[(static_cast<usize>(y) * kW + x) * 4u];
                bool pixelOk = true;
                for (int c = 0; c < 4; ++c) {
                    const float expected = static_cast<float>(texel[c]) * (1.f - alpha) + static_cast<float>(kRaster[c]) * alpha;
                    const int diff = std::abs(static_cast<int>(got[c]) - static_cast<int>(std::lround(expected)));
                    worst = diff > worst ? diff : worst;
                    pixelOk = pixelOk && diff <= tolerance;
                }
                if (!pixelOk && bad < 4u) {
                    std::fprintf(stderr, "frame %u alpha %.2f pixel (%u,%u): got (%u,%u,%u,%u) cuda (%u,%u,%u,%u)\n", f,
                                 static_cast<double>(alpha), x, y, got[0], got[1], got[2], got[3], texel[0], texel[1],
                                 texel[2], texel[3]);
                }
                bad += pixelOk ? 0u : 1u;
            }
        }
        std::printf("frame %u alpha %.2f: %u of %u composite pixels off mix(cuda, raster, alpha) (worst channel diff %d)\n",
                    f, static_cast<double>(alpha), bad, kW * kH, worst);
        expectTrue(bad == 0u, "CUDA surface write appears in the composite output at every pixel");
        if (alpha == 0.f) {
            expectTrue(previous != rgba, "composite shows this frame's CUDA write, not a previous one");
            previous = rgba;
        }
    }

    vkDeviceWaitIdle(ctx.vkDevice);
    (void)cudaStreamSynchronize(stream);
    (void)cudaStreamDestroy(stream);
    rcuda::release_imported_surface(surface);
    vkFreeCommandBuffers(ctx.vkDevice, ctx.pool, 1, &cb);
    composite.reset();
    ctx.allocator->destroyImage(cudaTex);
    ctx.allocator->destroyImage(raster);
    timeline.destroy(ctx.vkDevice);
    return b5rhi::finish(name);
#endif
}

#endif // FUSE_VULKAN_BACKEND

} // namespace

int main(int argc, char** argv) {
    const char* gateName = argc > 1 ? argv[1] : "";
    fuse::core::initialize();
    int result = kSkip;
#if !defined(FUSE_VULKAN_BACKEND)
    std::printf("SKIP fuse_cuda_vk_interop_gates %s: Vulkan backend not built\n", gateName);
#else
    if (std::strcmp(gateName, "buffer_roundtrip") == 0) {
        result = gateBufferRoundtrip();
    } else if (std::strcmp(gateName, "timeline") == 0) {
        result = gateTimeline(false);
    } else if (std::strcmp(gateName, "timeline_cpu_lane") == 0) {
        result = gateTimeline(true);
    } else if (std::strcmp(gateName, "surface_composite") == 0) {
        result = gateSurfaceComposite();
    } else {
        std::fprintf(stderr, "usage: fuse_cuda_vk_interop_gates buffer_roundtrip|timeline|timeline_cpu_lane|"
                             "surface_composite\n");
        result = EXIT_FAILURE;
    }
#endif
    fuse::core::shutdown();
    return result;
}
