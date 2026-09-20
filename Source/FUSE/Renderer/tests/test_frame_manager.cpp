#include <fuse/core/init.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <cstdio>
#include <cstdlib>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testScratchResetAndDescriptorPool() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    expectTrue(instance != nullptr, "VulkanInstance allocated");
    if (instance == nullptr) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    expectTrue(device != nullptr, "VulkanDevice allocated (GPU optional)");
    if (device == nullptr) {
        return;
    }

    auto frames = fuse::renderer::FrameManager::create(*device);
    expectTrue(frames != nullptr, "FrameManager allocated with CPU scratch");
    if (frames == nullptr) {
        return;
    }

    expectTrue(frames->scratchUsedBytes() == 0u, "scratch starts empty");
    expectTrue(frames->scratch().capacityBytes() == 8u * 1024u * 1024u, "scratch is 8MB per slot");

    void* ptr = frames->allocateScratch(64u);
    expectTrue(ptr != nullptr, "64-byte scratch allocation succeeds");
    expectTrue(frames->scratchUsedBytes() >= 64u, "usedBytes accounts for 64-byte allocation");

    void* dummySet = nullptr;
    expectTrue(frames->allocateDescriptorSets(nullptr, 1u, &dummySet) == 0u,
               "allocateDescriptorSets with null layout returns 0");

    frames->beginFrame(0u);
    expectTrue(frames->scratchUsedBytes() == 0u, "beginFrame resets current slot scratch");
    expectTrue(frames->descriptorSetsAllocatedThisFrame() == 0u,
               "beginFrame resets descriptorSetsAllocatedThisFrame");

    expectTrue(frames->currentDescriptorPool() == frames->current().commands.descriptorPool,
               "currentDescriptorPool matches current slot commands.descriptorPool");

#if defined(FUSE_VULKAN_BACKEND)
    if (device->isValid() && frames->isReady()) {
        expectTrue(frames->current().commands.descriptorPool != nullptr,
                   "native descriptor pool exists when Vulkan device is valid");
        expectTrue(frames->currentTransferCommandBuffer() != nullptr,
                   "current transfer command buffer exists when ready");
        expectTrue(frames->currentComputeCommandBuffer() != nullptr,
                   "current compute command buffer exists when ready");
        expectTrue(frames->currentTimelineSemaphore() != nullptr,
                   "current timeline semaphore exists when ready");
        expectTrue(frames->currentTimelineValue() == 0u,
                   "timeline value starts at 0");
        for (fuse::u32 i = 0; i < fuse::renderer::kFramesInFlight; ++i) {
            expectTrue(frames->slot(i).commands.descriptorPool != nullptr,
                       "each in-flight slot has a descriptor pool");
            expectTrue(frames->slot(i).commands.transferCommandBuffer != nullptr,
                       "each in-flight slot has a transfer command buffer");
            expectTrue(frames->slot(i).commands.computeCommandBuffer != nullptr,
                       "each in-flight slot has a compute command buffer");
            expectTrue(frames->slot(i).timelineSemaphore != nullptr,
                       "each in-flight slot has a timeline semaphore");
        }
        if (frames->currentDescriptorPool() != nullptr) {
            const fuse::u32 before = frames->descriptorPoolResetCount();
            frames->beginFrame(1u);
            expectTrue(frames->descriptorPoolResetCount() == before + 1u,
                       "beginFrame increments descriptorPoolResetCount when pool is reset");
            expectTrue(frames->descriptorSetsAllocatedThisFrame() == 0u,
                       "beginFrame resets descriptorSetsAllocatedThisFrame when pool is reset");
            expectTrue(frames->allocateDescriptorSets(nullptr, 1u, nullptr) == 0u,
                       "null layout does not allocate from a live pool");
        }
    } else {
        (void)frames->currentDescriptorPool();
        (void)frames->currentTransferCommandBuffer();
        (void)frames->currentComputeCommandBuffer();
        (void)frames->currentTimelineSemaphore();
        (void)frames->currentTimelineValue();
        for (fuse::u32 i = 0; i < fuse::renderer::kFramesInFlight; ++i) {
            (void)frames->slot(i).commands.transferCommandBuffer;
            (void)frames->slot(i).commands.computeCommandBuffer;
            (void)frames->slot(i).timelineSemaphore;
        }
    }
#else
    (void)frames->currentDescriptorPool();
    (void)frames->currentTransferCommandBuffer();
    (void)frames->currentComputeCommandBuffer();
    (void)frames->currentTimelineSemaphore();
    (void)frames->currentTimelineValue();
#endif
}

void testGpuTimestamps() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    expectTrue(instance != nullptr, "VulkanInstance allocated for timestamps");
    if (instance == nullptr) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    expectTrue(device != nullptr, "VulkanDevice allocated for timestamps");
    if (device == nullptr) {
        return;
    }

    auto frames = fuse::renderer::FrameManager::create(*device);
    expectTrue(frames != nullptr, "FrameManager allocated for timestamps");
    if (frames == nullptr) {
        return;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (device->isValid() && frames->isReady() && frames->timestampsReady()) {
        expectTrue(frames->info().timestampsReady, "info.timestampsReady is true when accessors report ready");
        expectTrue(frames->current().timestampQueryPool != nullptr,
                   "current slot timestamp query pool exists when ready");
        for (fuse::u32 i = 0; i < fuse::renderer::kFramesInFlight; ++i) {
            expectTrue(frames->slot(i).timestampQueryPool != nullptr,
                       "each in-flight slot has a timestamp query pool");
        }

        auto cmd = static_cast<VkCommandBuffer>(frames->currentCommandBuffer());
        expectTrue(cmd != VK_NULL_HANDLE, "current command buffer exists for timestamp write");
        if (cmd != VK_NULL_HANDLE) {
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS) {
                const fuse::u32 before = frames->timestampWriteCount();
                frames->writeTimestampBegin(cmd);
                frames->writeTimestampEnd(cmd);
                expectTrue(frames->timestampWriteCount() >= before + 2u,
                           "writeTimestampBegin/End increment timestampWriteCount");
                expectTrue(vkEndCommandBuffer(cmd) == VK_SUCCESS, "end command buffer after timestamp writes");
            }
        }

        // lastGpuTimeNs may be 0 until a submit+wait — do not require >0
        (void)frames->lastGpuTimeNs();
        fuse::u64 ns = 0;
        (void)frames->readLastGpuTimeNs(frames->currentIndex(), &ns);
        (void)ns;
    } else {
        expectTrue(!frames->timestampsReady(), "timestampsReady false when stub or unsupported");
        expectTrue(!frames->info().timestampsReady, "info.timestampsReady false when stub or unsupported");
        frames->writeTimestampBegin(frames->currentCommandBuffer());
        frames->writeTimestampEnd(frames->currentCommandBuffer());
        fuse::u64 ns = 0;
        expectTrue(!frames->readLastGpuTimeNs(0u, &ns), "readLastGpuTimeNs false when timestamps not ready");
        expectTrue(frames->lastGpuTimeNs() == 0u, "lastGpuTimeNs stays 0 when timestamps not ready");
    }
#else
    expectTrue(!frames->timestampsReady(), "stub: timestampsReady false");
    expectTrue(!frames->info().timestampsReady, "stub: info.timestampsReady false");
    frames->writeTimestampBegin(nullptr);
    frames->writeTimestampEnd(nullptr);
    fuse::u64 ns = 0;
    expectTrue(!frames->readLastGpuTimeNs(0u, &ns), "stub: readLastGpuTimeNs false");
    expectTrue(frames->lastGpuTimeNs() == 0u, "stub: lastGpuTimeNs is 0");
#endif
}

void testFrameDebugNamesAndFenceWait() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    expectTrue(instance != nullptr, "VulkanInstance allocated for frame debug names");
    if (instance == nullptr) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    expectTrue(device != nullptr, "VulkanDevice allocated for frame debug names");
    if (device == nullptr) {
        return;
    }

    auto frames = fuse::renderer::FrameManager::create(*device);
    expectTrue(frames != nullptr, "FrameManager create for debug names and fence wait");
    if (frames == nullptr) {
        return;
    }

    // debugNamesSet may be 0 on stub or when VK_EXT_debug_utils is missing.
    (void)frames->debugNamesSet();

    if (frames->isReady()) {
        const fuse::u32 names = frames->debugNamesSet();
        (void)names;
        expectTrue(frames->waitInFlightFence(frames->currentIndex()),
                   "waitInFlightFence succeeds when FrameManager is ready");
        // lastGpuTimeNs may stay 0 until a real submit — do not require >0
        (void)frames->lastGpuTimeNs();
        frames->beginFrame(0u);
        (void)frames->lastGpuTimeNs();
        expectTrue(frames->waitInFlightFence(0u), "waitInFlightFence after beginFrame does not crash");
    } else {
        expectTrue(frames->debugNamesSet() == 0u, "stub: debugNamesSet is 0");
        (void)frames->waitInFlightFence(0u);
        frames->beginFrame(0u);
        expectTrue(frames->lastGpuTimeNs() == 0u, "stub: lastGpuTimeNs stays 0");
    }
}

} // namespace

int main() {
    fuse::core::initialize();

    testScratchResetAndDescriptorPool();
    testGpuTimestamps();
    testFrameDebugNamesAndFenceWait();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_frame_manager: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_frame_manager: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
