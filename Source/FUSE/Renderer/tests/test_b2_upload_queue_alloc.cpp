// B2.11 gate "zero steady-state heap allocations", UploadQueue slice: after warm-up, stage /
// record / flush / retire cycles must not call operator new. (The in-flight batch FIFO used to be
// a std::deque, which allocates a node every 16 pushes: 4 allocations per 64 flushes.)
//
// Global operator new/delete are replaced with counters armed only around the measured loop.
// Scenarios:
//   host  device == nullptr (stub path, every build): stage + flush, batches retire at once.
//   gpu   Lavapipe, validation off: stage + buffer copy + flush + retire, all kMaxBatches batch
//         objects created during warm-up; then command-less flushes queued behind submitted
//         batches fill the in-flight FIFO and must wait for the oldest instead of growing it.
// Injected layers (fuse_vulkan_validation_gate sets VK_INSTANCE_LAYERS) allocate through this
// process-wide operator new on every vk* call, so the gpu count is reported, not enforced, then.
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

thread_local bool g_countAllocations = false; // render-thread only: driver threads are not counted
thread_local unsigned long g_allocationCount = 0;

void* countedAlloc(std::size_t size) {
    if (g_countAllocations) {
        ++g_allocationCount;
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

} // namespace

void* operator new(std::size_t size) { return countedAlloc(size); }
void* operator new[](std::size_t size) { return countedAlloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (g_countAllocations) {
        ++g_allocationCount;
    }
    return std::malloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::renderer::UploadQueue;
using fuse::renderer::UploadTicket;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kWarmupFlushes = 2u * UploadQueue::kMaxBatches;
constexpr u32 kMeasuredFlushes = 64;

/// Counts operator new calls made by `body` (armed only for its duration).
template <typename Body>
unsigned long countAllocations(Body&& body) {
    g_allocationCount = 0;
    g_countAllocations = true;
    body();
    g_countAllocations = false;
    return g_allocationCount;
}

void testHostSteadyStateFlushesDoNotAllocate() {
    constexpr usize kRingBytes = 64u * 1024u;
    std::vector<u8> ring(kRingBytes);
    std::vector<u8> payload(3000u, 0x5Au);

    UploadQueue queue;
    expectTrue(queue.init(nullptr, nullptr, ring.data(), kRingBytes), "host upload queue init");

    bool ok = true;
    auto cycle = [&](u32 i) {
        usize offset = 0;
        payload[0] = static_cast<u8>(i);
        ok = queue.stage(payload.data(), payload.size(), offset) && ok;
        const UploadTicket ticket = queue.flush();
        ok = ticket.isValid() && queue.isComplete(ticket) && ok;
    };
    for (u32 i = 0; i < kWarmupFlushes; ++i) {
        cycle(i);
    }
    const u64 retiredBefore = queue.stats().retiredBatches;
    const unsigned long allocations = countAllocations([&] {
        for (u32 i = 0; i < kMeasuredFlushes; ++i) {
            cycle(kWarmupFlushes + i);
        }
    });

    std::printf("host: %lu heap allocations over %u steady-state flushes\n", allocations, kMeasuredFlushes);
    expectTrue(ok, "host: every stage and flush succeeded and completed");
    expectTrue(queue.stats().retiredBatches - retiredBefore == kMeasuredFlushes, "host: every flush retired a batch");
    expectTrue(queue.stats().ringWraps > 0, "host: the staging ring wrapped during the run");
    expectTrue(allocations == 0u, "host: steady-state flushes perform zero heap allocations");
    queue.destroy();
}

#if defined(FUSE_VULKAN_BACKEND)

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

bool createBuffer(VkPhysicalDevice physical, VkDevice device, usize size, VkBufferUsageFlags usage, bool hostVisible,
                  Buffer& out) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const VkMemoryPropertyFlags flags = hostVisible
                                            ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    u32 typeIndex = UINT32_MAX;
    for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
            typeIndex = i;
            break;
        }
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = typeIndex;
    if (typeIndex == UINT32_MAX || vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
        return false;
    }
    return !hostVisible || vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) == VK_SUCCESS;
}

void destroyBuffer(VkDevice device, Buffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, buffer.memory, nullptr);
    }
    buffer = Buffer{};
}

void testGpuSteadyStateFlushesDoNotAllocate() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_b2_upload_queue_alloc";
    instanceDesc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    if (instance == nullptr || !instance->isValid()) {
        std::printf("SKIP gpu: no Vulkan instance\n");
        return;
    }
    auto device = fuse::renderer::VulkanDevice::create(*instance);
    if (device == nullptr || !device->isValid()) {
        std::printf("SKIP gpu: no Vulkan device\n");
        return;
    }
    const VkPhysicalDevice physical = static_cast<VkPhysicalDevice>(device->nativePhysicalDevice());
    const VkDevice vkDevice = static_cast<VkDevice>(device->nativeHandle());

    constexpr usize kRingBytes = 256u * 1024u;
    constexpr usize kCopyBytes = 1024u;
    Buffer ring;
    Buffer dst;
    const bool created =
        createBuffer(physical, vkDevice, kRingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, ring) &&
        createBuffer(physical, vkDevice, 64u * 1024u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, dst);
    expectTrue(created, "gpu: staging ring and destination buffer");
    if (!created) {
        destroyBuffer(vkDevice, ring);
        destroyBuffer(vkDevice, dst);
        return;
    }

    std::vector<u8> payload(kCopyBytes, 0xC3u);
    bool ok = true;
    {
        UploadQueue queue;
        expectTrue(queue.init(device.get(), ring.buffer, ring.mapped, kRingBytes), "gpu upload queue init");

        auto copyCycle = [&](u32 i, bool retire) {
            usize offset = 0;
            payload[0] = static_cast<u8>(i);
            ok = queue.stage(payload.data(), payload.size(), offset) && ok;
            ok = queue.recordBufferCopy(dst.buffer, offset, (i % 64u) * kCopyBytes, kCopyBytes) && ok;
            ok = queue.flush().isValid() && ok;
            if (retire) {
                queue.retireCompleted();
            }
        };

        // Warm-up without retiring: every batch object (kMaxBatches) is created and recycled.
        for (u32 i = 0; i < kWarmupFlushes; ++i) {
            copyCycle(i, false);
        }
        ok = queue.waitAll() && ok;

        const u64 submittedBefore = queue.stats().submittedBatches;
        const unsigned long steady = countAllocations([&] {
            for (u32 i = 0; i < kMeasuredFlushes; ++i) {
                copyCycle(i, true);
                if (i % 16u == 15u) {
                    ok = queue.waitAll() && ok; // frame-boundary style drain
                }
            }
        });
        const u64 submitted = queue.stats().submittedBatches - submittedBefore;

        // Command-less flushes (staged, nothing recorded) queued behind submitted batches: the
        // in-flight FIFO fills (kMaxBatches entries) and flush retires the oldest before pushing.
        const unsigned long mixed = countAllocations([&] {
            for (u32 i = 0; i < 2u * UploadQueue::kMaxBatches; ++i) {
                copyCycle(i, false);
                usize offset = 0;
                ok = queue.stage(payload.data(), payload.size(), offset) && ok;
                ok = queue.flush().isValid() && ok;
            }
        });
        const UploadTicket last = queue.flush();
        ok = queue.waitAll() && queue.isComplete(last) && ok;

        std::printf("gpu: %lu heap allocations over %u steady-state flushes (%llu submitted), %lu over %u mixed "
                    "flushes (max %u in flight)\n",
                    steady, kMeasuredFlushes, static_cast<unsigned long long>(submitted), mixed,
                    4u * UploadQueue::kMaxBatches, queue.stats().maxBatchesInFlight);
        expectTrue(ok, "gpu: every stage, copy, flush and wait succeeded");
        expectTrue(submitted == kMeasuredFlushes, "gpu: every measured flush submitted a batch");
        expectTrue(queue.stats().maxBatchesInFlight == UploadQueue::kMaxBatches,
                   "gpu: in-flight FIFO filled to kMaxBatches");
        expectTrue(queue.stats().submitFailures == 0u && queue.stats().fenceTimeouts == 0u,
                   "gpu: no submit failures or fence timeouts");
        expectTrue(!queue.hasPendingWork(), "gpu: queue drained");

        const char* layers = std::getenv("VK_INSTANCE_LAYERS");
        if (layers != nullptr && std::strstr(layers, "validation") != nullptr) {
            std::printf("NOTE: validation layer injected — gpu allocation count reported, not enforced\n");
        } else {
            expectTrue(steady == 0u, "gpu: steady-state flushes perform zero heap allocations");
            expectTrue(mixed == 0u, "gpu: a full in-flight FIFO waits instead of allocating");
        }
        queue.destroy();
    }
    destroyBuffer(vkDevice, dst);
    destroyBuffer(vkDevice, ring);
}

#endif

} // namespace

int main() {
    testHostSteadyStateFlushesDoNotAllocate();
#if defined(FUSE_VULKAN_BACKEND)
    testGpuSteadyStateFlushesDoNotAllocate();
#else
    std::printf("SKIP gpu: stub backend\n");
#endif

    if (g_failures == 0) {
        std::printf("fuse_b2_upload_queue_alloc: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_upload_queue_alloc: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
