// B4.11 gate row: "GPU radix sort produces correctly sorted (key, value) pairs — verified with
// reference CPU sort" (planned for CUDA; proven here with the Vulkan compute implementation,
// fuse_rhi GpuRadixSort, on Lavapipe).
//
//  * sizes 1, 2, 255, 256, 257, 4096, 65537, 1M (+ 1023, 1025, 4097: batch/tile boundaries) x
//    key distributions uniform, all-equal, sorted,
//    reverse, few-distinct, high-bit-only x key types u32 and u64; value = input index, and the
//    output must equal std::stable_sort by key exactly (keys and carried values: stability).
//  * partial key widths (odd pass count -> final copy path; fewer passes).
//  * everything runs under VK_LAYER_KHRONOS_validation with synchronization validation; a
//    negative control (two unsynchronised writes) proves sync validation is live, then the sort
//    must produce 0 warnings/errors.
//  * physics hook: spatial-hash broadphase with the GPU sorter plugged into
//    SpatialHashParams::entrySorter yields exactly the CPU pair set.
//  * throughput on the device (informational).
// Exit 77 (skip) without the Vulkan backend, the shaders, the validation layer or a device.
#include "b5_rhi_test_common.hpp"

#include <fuse/renderer/compute/gpu_radix_sort.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/physics/broadphase/pair_buffer.hpp>
#include <fuse/physics/broadphase/spatial_hash.hpp>
#include <fuse/physics/physics_data.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <utility>
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

using b5rhi::expectTrue;
using fuse::u32;
using fuse::u64;

constexpr const char* kTestName = "fuse_gpu_radix_sort_gates";

#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_RADIX_SORT_SHADERS_BUILT)

using fuse::renderer::GpuRadixSort;
using fuse::renderer::GpuRadixSortStats;

// ---- validation message capture ---------------------------------------------------------------

struct MessageLog {
    std::mutex mutex;
    u32 count = 0;
    u32 syncHazards = 0;
    std::vector<std::string> samples;
};
MessageLog g_log;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    std::lock_guard<std::mutex> lock(g_log.mutex);
    ++g_log.count;
    const char* id = data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)";
    if (std::strncmp(id, "SYNC-HAZARD", 11) == 0) {
        ++g_log.syncHazards;
    }
    if (g_log.samples.size() < 8u) {
        std::string text = id;
        if (data != nullptr && data->pMessage != nullptr) {
            text += ": ";
            text += std::string(data->pMessage).substr(0, 300);
        }
        g_log.samples.push_back(text);
    }
    return VK_FALSE;
}

void resetLog() {
    std::lock_guard<std::mutex> lock(g_log.mutex);
    g_log.count = 0;
    g_log.syncHazards = 0;
    g_log.samples.clear();
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

/// Negative control: two vkCmdFillBuffer writes to one buffer without a barrier must be reported
/// as SYNC-HAZARD-WRITE-AFTER-WRITE, or synchronization validation is not actually running.
u32 runSyncHazardControl(fuse::renderer::VulkanDevice& device) {
    auto allocator = fuse::renderer::GpuAllocator::create(device);
    fuse::renderer::Buffer buffer;
    fuse::renderer::BufferDesc desc{};
    desc.size = 4096;
    desc.usage = static_cast<fuse::renderer::BufferUsage>(static_cast<u32>(fuse::renderer::BufferUsage::TransferDst) |
                                                          static_cast<u32>(fuse::renderer::BufferUsage::Storage));
    desc.name = "radix_sort_test.hazard_control";
    if (allocator == nullptr || !allocator->createBuffer(desc, buffer)) {
        return 0;
    }
    const VkDevice vkDevice = static_cast<VkDevice>(device.nativeHandle());
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = device.queues().computeFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vkDevice, &allocInfo, &cmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdFillBuffer(cmd, static_cast<VkBuffer>(buffer.handle), 0, VK_WHOLE_SIZE, 1u);
    vkCmdFillBuffer(cmd, static_cast<VkBuffer>(buffer.handle), 0, VK_WHOLE_SIZE, 2u); // no barrier: WAW
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(static_cast<VkQueue>(device.queues().compute), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(static_cast<VkQueue>(device.queues().compute));
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    allocator->destroyBuffer(buffer);
    std::lock_guard<std::mutex> lock(g_log.mutex);
    return g_log.syncHazards;
}

// ---- key generation + reference ---------------------------------------------------------------

enum class Dist { Uniform, AllEqual, Sorted, Reverse, FewDistinct, HighBitOnly };
constexpr Dist kDists[] = {Dist::Uniform, Dist::Sorted, Dist::Reverse, Dist::AllEqual, Dist::FewDistinct,
                           Dist::HighBitOnly};

const char* distName(Dist d) {
    switch (d) {
    case Dist::Uniform: return "uniform";
    case Dist::AllEqual: return "all-equal";
    case Dist::Sorted: return "sorted";
    case Dist::Reverse: return "reverse";
    case Dist::FewDistinct: return "few-distinct";
    case Dist::HighBitOnly: return "high-bit-only";
    }
    return "?";
}

template <typename K>
std::vector<K> makeKeys(Dist dist, u32 count, u32 keyBits, u64 seed) {
    std::mt19937_64 rng(seed);
    const K mask = keyBits >= sizeof(K) * 8u ? static_cast<K>(~K{0}) : static_cast<K>((K{1} << keyBits) - 1u);
    std::vector<K> keys(count);
    switch (dist) {
    case Dist::Uniform:
    case Dist::Sorted:
    case Dist::Reverse:
        for (K& k : keys) {
            k = static_cast<K>(rng()) & mask;
        }
        if (dist == Dist::Sorted) {
            std::sort(keys.begin(), keys.end());
        } else if (dist == Dist::Reverse) {
            std::sort(keys.begin(), keys.end(), [](K a, K b) { return a > b; });
        }
        break;
    case Dist::AllEqual: {
        const K value = static_cast<K>(0xA5C3F00D5A3C0FF0ull) & mask;
        std::fill(keys.begin(), keys.end(), value);
        break;
    }
    case Dist::FewDistinct: {
        K pool[5];
        for (K& p : pool) {
            p = static_cast<K>(rng()) & mask;
        }
        for (K& k : keys) {
            k = pool[rng() % 5u];
        }
        break;
    }
    case Dist::HighBitOnly:
        for (K& k : keys) {
            k = static_cast<K>(static_cast<K>(rng() & 1u) << (keyBits - 1u));
        }
        break;
    }
    return keys;
}

template <typename K>
struct Run {
    bool ok = false;
    bool matches = false;
    GpuRadixSortStats stats;
};

/// Sorts (key, index) on the GPU and compares with std::stable_sort by key (values = indices, so
/// any unstable reordering of equal keys shows up as a value mismatch).
template <typename K>
Run<K> sortAndCompare(GpuRadixSort& sorter, std::vector<K> keys, u32 keyBits) {
    const u32 count = static_cast<u32>(keys.size());
    std::vector<u32> values(count);
    for (u32 i = 0; i < count; ++i) {
        values[i] = i;
    }
    std::vector<std::pair<K, u32>> reference(count);
    for (u32 i = 0; i < count; ++i) {
        reference[i] = {keys[i], i};
    }
    std::stable_sort(reference.begin(), reference.end(),
                     [](const std::pair<K, u32>& a, const std::pair<K, u32>& b) { return a.first < b.first; });

    Run<K> run;
    if constexpr (sizeof(K) == 8) {
        run.ok = sorter.sort64(keys.data(), values.data(), count, keyBits, &run.stats);
    } else {
        run.ok = sorter.sort(keys.data(), values.data(), count, keyBits, &run.stats);
    }
    run.matches = run.ok;
    for (u32 i = 0; run.ok && i < count; ++i) {
        if (keys[i] != reference[i].first || values[i] != reference[i].second) {
            std::fprintf(stderr, "  first mismatch at %u: got (%llx, %u) want (%llx, %u)\n", i,
                         static_cast<unsigned long long>(keys[i]), values[i],
                         static_cast<unsigned long long>(reference[i].first), reference[i].second);
            run.matches = false;
            break;
        }
    }
    return run;
}

template <typename K>
void runMatrix(GpuRadixSort& sorter, const char* label, u32 keyBits) {
    // Spec sizes plus scatter-batch (1024) and tile (4096) boundaries.
    constexpr u32 kSizes[] = {1u, 2u, 255u, 256u, 257u, 1023u, 1025u, 4096u, 4097u, 65537u, 1u << 20};
    u32 cases = 0;
    u32 failed = 0;
    for (Dist dist : kDists) {
        for (u32 size : kSizes) {
            const std::vector<K> keys = makeKeys<K>(dist, size, keyBits, 0x5EED0000ull + size * 131u + static_cast<u32>(dist));
            const Run<K> run = sortAndCompare(sorter, keys, keyBits);
            ++cases;
            if (!run.ok || !run.matches) {
                ++failed;
                std::fprintf(stderr, "FAIL: %s keyBits %u %s n=%u (%s)\n", label, keyBits, distName(dist), size,
                             run.ok ? "wrong order" : sorter.message().c_str());
            }
        }
    }
    std::printf("%s keyBits %u: %u/%u cases equal std::stable_sort (keys + carried values)\n", label, keyBits,
                cases - failed, cases);
    expectTrue(failed == 0u, "GPU radix sort equals std::stable_sort for every size x distribution");
}

// ---- physics broadphase hook ------------------------------------------------------------------

struct HookContext {
    GpuRadixSort* sorter = nullptr;
    u32 calls = 0;
    u32 entries = 0;
    u32 keyBits = 0;
};

bool gpuEntrySort(void* user, u32* keys, u32* values, u32 count, u32 keyBits) {
    auto* ctx = static_cast<HookContext*>(user);
    ++ctx->calls;
    ctx->entries = count;
    ctx->keyBits = keyBits;
    return ctx->sorter->sort(keys, values, count, keyBits);
}

using Pair = std::pair<u32, u32>;

std::vector<Pair> canonical(const std::vector<fuse::physics::broadphase::CandidatePair>& pairs) {
    std::vector<Pair> out;
    out.reserve(pairs.size());
    for (const auto& p : pairs) {
        out.emplace_back(std::min(p.bodyA, p.bodyB), std::max(p.bodyA, p.bodyB));
    }
    std::sort(out.begin(), out.end());
    return out;
}

void testBroadphaseHook(GpuRadixSort& sorter) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(2);

    std::mt19937 rng(10'000u);
    std::uniform_real_distribution<float> pos(-60.f, 60.f);
    std::uniform_real_distribution<float> rad(0.1f, 1.5f);
    fuse::physics::RigidBodySoA bodies;
    fuse::physics::CollisionShapeSoA shapes;
    for (u32 i = 0; i < 10'000u; ++i) {
        const u32 body = bodies.addBody({pos(rng), pos(rng), pos(rng)}, 1.f);
        shapes.addShape(fuse::physics::CollisionShapeType::Sphere, body, {rad(rng), 0.f, 0.f});
    }
    fuse::physics::broadphase::SpatialHashParams params{};
    params.cellSize = 3.f;
    params.tableSize = 16384;

    fuse::physics::broadphase::PairBufferSoA cpuBuffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, cpuBuffer);
    const std::vector<Pair> cpuPairs = canonical(cpuBuffer.toVector());

    HookContext ctx;
    ctx.sorter = &sorter;
    fuse::physics::broadphase::BroadphaseKeyValueSorter hook{};
    hook.sort = gpuEntrySort;
    hook.user = &ctx;
    params.entrySorter = &hook;
    fuse::physics::broadphase::PairBufferSoA gpuBuffer;
    fuse::physics::broadphase::runBroadphaseIntoBuffer(bodies, shapes, params, gpuBuffer);
    const std::vector<Pair> gpuPairs = canonical(gpuBuffer.toVector());

    std::printf("broadphase hook: %u GPU sort call(s), %u (cell, body) entries, %u key bits; %zu pairs CPU, "
                "%zu pairs GPU-sorted\n",
                ctx.calls, ctx.entries, ctx.keyBits, cpuPairs.size(), gpuPairs.size());
    expectTrue(ctx.calls == 1u && ctx.entries > 10'000u, "broadphase routed its entry sort through the GPU hook");
    expectTrue(!cpuPairs.empty() && gpuPairs == cpuPairs, "GPU-sorted broadphase finds exactly the CPU pairs");
    scheduler.shutdown();
}

// ---- throughput (informational) ---------------------------------------------------------------

template <typename K>
void reportThroughput(GpuRadixSort& sorter, const char* label, u32 keyBits) {
    constexpr u32 kCount = 1u << 20;
    const std::vector<K> source = makeKeys<K>(Dist::Uniform, kCount, keyBits, 0xBEEF);
    std::vector<u32> values(kCount);
    double bestGpu = 1e30;
    double bestTotal = 1e30;
    GpuRadixSortStats stats;
    for (int rep = 0; rep < 3; ++rep) {
        std::vector<K> keys = source;
        for (u32 i = 0; i < kCount; ++i) {
            values[i] = i;
        }
        bool ok = false;
        if constexpr (sizeof(K) == 8) {
            ok = sorter.sort64(keys.data(), values.data(), kCount, keyBits, &stats);
        } else {
            ok = sorter.sort(keys.data(), values.data(), kCount, keyBits, &stats);
        }
        if (!ok) {
            return;
        }
        bestGpu = std::min(bestGpu, stats.gpuMs);
        bestTotal = std::min(bestTotal, stats.totalMs);
    }
    std::vector<std::pair<K, u32>> cpu(kCount);
    for (u32 i = 0; i < kCount; ++i) {
        cpu[i] = {source[i], i};
    }
    const auto start = std::chrono::steady_clock::now();
    std::stable_sort(cpu.begin(), cpu.end(),
                     [](const std::pair<K, u32>& a, const std::pair<K, u32>& b) { return a.first < b.first; });
    const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("throughput %s (1M pairs, %u passes, %u dispatches): GPU %.1f ms = %.1f Mpairs/s; blocking call "
                "incl. upload/readback %.1f ms; std::stable_sort %.1f ms\n",
                label, stats.passes, stats.dispatches, bestGpu, bestGpu > 0.0 ? kCount / (bestGpu * 1e3) : 0.0,
                bestTotal, cpuMs);
}

int run() {
    constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
    setenv("VK_INSTANCE_LAYERS", kValidationLayer, 1);
    setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
    setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
    // Messages go to the messengers only (the hazard control must not print "Validation Error").
    setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    if (!layerAvailable(kValidationLayer)) {
        return b5rhi::skip(kTestName, "VK_LAYER_KHRONOS_validation not installed");
    }

    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = kTestName;
    instanceDesc.enableValidation = true;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    if (instance == nullptr || !instance->isValid()) {
        return b5rhi::skip(kTestName, "no Vulkan instance");
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
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = onMessage;
        createMessenger(vkInstance, &info, nullptr, &messenger);
    }
    expectTrue(messenger != VK_NULL_HANDLE, "debug messenger created");

    int result = EXIT_SUCCESS;
    {
        auto device = fuse::renderer::VulkanDevice::create(*instance);
        if (device == nullptr || !device->isValid()) {
            if (messenger != VK_NULL_HANDLE) {
                destroyMessenger(vkInstance, messenger, nullptr);
            }
            return b5rhi::skip(kTestName, "no Vulkan device (needs an ICD, Lavapipe in CI)");
        }
        std::printf("device: %s\n", device->info().deviceName.c_str());

        const u32 hazards = runSyncHazardControl(*device);
        std::printf("negative control: %u SYNC-HAZARD message(s) for two unsynchronised fills\n", hazards);
        expectTrue(hazards > 0u, "synchronization validation is active (negative control reports a hazard)");
        resetLog();
        fuse::renderer::resetVulkanValidationCounters();

        auto sorter = GpuRadixSort::create(*device);
        expectTrue(sorter != nullptr && sorter->isValid(), "GpuRadixSort created");
        if (sorter != nullptr && sorter->isValid()) {
            std::printf("max count: %u (u32 keys), %u (u64 keys)\n", sorter->maxCount(fuse::renderer::RadixSortKeyType::U32),
                        sorter->maxCount(fuse::renderer::RadixSortKeyType::U64));
            runMatrix<u32>(*sorter, "u32", 32u);
            runMatrix<u64>(*sorter, "u64", 64u);
            // Partial widths: odd pass counts take the final temp -> keys copy.
            runMatrix<u32>(*sorter, "u32", 24u);
            runMatrix<u32>(*sorter, "u32", 10u);
            runMatrix<u64>(*sorter, "u64", 40u);

            // Descending then ascending sizes reuse the grown buffers (stale data must not leak).
            {
                std::vector<u32> big = makeKeys<u32>(Dist::Uniform, 300'000u, 32u, 1u);
                expectTrue(sortAndCompare(*sorter, big, 32u).matches, "300k after 1M (buffer reuse)");
                std::vector<u32> fewKeys = makeKeys<u32>(Dist::FewDistinct, 5000u, 32u, 2u); // not `small`: rpcndr.h macro
                expectTrue(sortAndCompare(*sorter, fewKeys, 32u).matches, "5000 after 300k (buffer reuse)");
            }
            // Invalid arguments are rejected without touching the data.
            {
                std::vector<u32> keys = {3u, 1u, 2u};
                std::vector<u32> values = {0u, 1u, 2u};
                expectTrue(!sorter->sort(keys.data(), values.data(), 3u, 33u), "keyBits > 32 rejected for u32 keys");
                expectTrue(keys[0] == 3u && values[0] == 0u, "rejected sort leaves data unchanged");
            }

            testBroadphaseHook(*sorter);
            reportThroughput<u32>(*sorter, "u32 keys", 32u);
            reportThroughput<u64>(*sorter, "u64 keys", 64u);
        }
        sorter.reset();
        device->waitIdle();
    }

    const fuse::renderer::VulkanValidationCounters counters = fuse::renderer::vulkanValidationCounters();
    u32 messages = 0;
    {
        std::lock_guard<std::mutex> lock(g_log.mutex);
        messages = g_log.count;
        for (const std::string& sample : g_log.samples) {
            std::fprintf(stderr, "  validation: %s\n", sample.c_str());
        }
    }
    std::printf("validation (core + sync): %u message(s) captured, instance counters %u error(s) %u warning(s)\n",
                messages, counters.errors, counters.warnings);
    expectTrue(messages == 0u, "0 validation / sync-validation messages during the sorts");
    expectTrue(counters.errors == 0u && counters.warnings == 0u, "instance validation counters stay at 0");

    if (messenger != VK_NULL_HANDLE) {
        destroyMessenger(vkInstance, messenger, nullptr);
    }
    (void)result;
    return b5rhi::finish(kTestName);
}

#endif

} // namespace

int main() {
#if !defined(FUSE_VULKAN_BACKEND)
    return b5rhi::skip(kTestName, "Vulkan backend disabled");
#elif !defined(FUSE_RADIX_SORT_SHADERS_BUILT)
    return b5rhi::skip(kTestName, "radix sort shaders not built (glslangValidator missing)");
#else
    return run();
#endif
}
