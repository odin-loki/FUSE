// B2 gate row: "Frame-in-flight management holds three independent frame data sets — verified by
// timeline semaphore values".
//
// Twelve RhiContext frames (four trips round the ring) on Lavapipe. Each FrameManager slot owns
// its own VK_SEMAPHORE_TYPE_TIMELINE semaphore; every submit from a slot signals that slot's
// timeline to value+1 and leaves the other two untouched. Checks, per frame:
//   * slots rotate 0,1,2,0,... and each owns a distinct semaphore, fence, command pool/buffer;
//   * the submitting slot's CPU value is monotonic (+1 per use); the other slots are unchanged;
//   * after beginFrame re-acquires a slot (fence wait) the GPU counter already equals the
//     slot's last signalled value, i.e. the previous use of that frame set has retired;
// and after idle every slot's GPU counter (vkGetSemaphoreCounterValue) == uses of that slot.
#include "b5_rhi_test_common.hpp"

#include <fuse/core/init.hpp>
#include <fuse/renderer/rhi_context.hpp>

#include <set>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

using b5rhi::expectTrue;
using fuse::u32;
using fuse::u64;

#if defined(FUSE_VULKAN_BACKEND)
u64 gpuCounter(VkDevice device, void* semaphore) {
    u64 value = ~0ull;
    if (vkGetSemaphoreCounterValue(device, static_cast<VkSemaphore>(semaphore), &value) != VK_SUCCESS) {
        return ~0ull;
    }
    return value;
}
#endif

} // namespace

int main() {
#if !defined(FUSE_VULKAN_BACKEND)
    return b5rhi::skip("fuse_b5_rhi_frame_timelines", "Vulkan backend disabled");
#else
    fuse::core::initialize();

    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.bootstrap.createSwapchain = false;
    desc.raster.width = 64;
    desc.raster.height = 64;
    auto context = fuse::renderer::RhiContext::create(desc);
    if (context == nullptr || !context->bootstrap().status().deviceReady ||
        context->bootstrap().frameManager() == nullptr || !context->bootstrap().frameManager()->isReady()) {
        context.reset();
        fuse::core::shutdown();
        return b5rhi::skip("fuse_b5_rhi_frame_timelines", "no Vulkan device (needs an ICD, Lavapipe in CI)");
    }

    fuse::renderer::FrameManager& frames = *context->bootstrap().frameManager();
    fuse::renderer::VulkanDevice& device = *context->bootstrap().device();
    const auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    constexpr u32 kSlots = fuse::renderer::kFramesInFlight;
    expectTrue(kSlots == 3u, "frame ring is triple-buffered");
    expectTrue(device.info().timelineSemaphore, "device enabled timelineSemaphore");

    std::set<void*> semaphores;
    std::set<void*> fences;
    std::set<void*> pools;
    std::set<void*> commandBuffers;
    for (u32 i = 0; i < kSlots; ++i) {
        const fuse::renderer::FrameSyncData& slot = frames.slot(i);
        expectTrue(slot.timelineSemaphore != nullptr, "slot owns a timeline semaphore");
        expectTrue(slot.timelineValue == 0u, "slot timeline starts at 0");
        semaphores.insert(slot.timelineSemaphore);
        fences.insert(slot.inFlightFence);
        pools.insert(slot.commands.commandPool);
        commandBuffers.insert(slot.commands.primaryCommandBuffer);
    }
    expectTrue(semaphores.size() == kSlots, "three distinct timeline semaphores");
    expectTrue(fences.size() == kSlots, "three distinct in-flight fences");
    expectTrue(pools.size() == kSlots && commandBuffers.size() == kSlots,
               "three distinct command pools / primary command buffers");

    constexpr u32 kFrames = 12;
    u64 expected[kSlots] = {0, 0, 0};
    u32 uses[kSlots] = {0, 0, 0};
    bool rotationOk = true;
    bool monotonic = true;
    bool independent = true;
    bool retiredOnReacquire = true;
    bool waitedPrior = true;

    fuse::renderer::RenderCommandList commands;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        commands.reset();
        commands.clear3D(0.f, 0.f, 0.f);
        commands.drawSprite2D(0.f, 0.f, 0.f, 255, 255, 255);

        expectTrue(context->beginFrame(frame), "beginFrame");
        const u32 slotIndex = context->currentFrameSlot();
        rotationOk = rotationOk && slotIndex == frame % kSlots;
        const fuse::renderer::FrameSyncData& slot = frames.slot(slotIndex);

        // beginFrame waited on this slot's fence: its last timeline signal has completed.
        if (uses[slotIndex] > 0u) {
            retiredOnReacquire = retiredOnReacquire && gpuCounter(vkDevice, slot.timelineSemaphore) >=
                                                           expected[slotIndex];
        }

        expectTrue(context->submitFrame(commands, frame), "submitFrame");
        const fuse::renderer::GraphicsQueueSubmitResult& submit = context->lastQueueSubmit();
        expectTrue(submit.submitted && submit.timelineSignaled, "submit signalled the slot timeline");
        if (uses[slotIndex] > 0u) {
            waitedPrior = waitedPrior && submit.timelineWaited;
        }

        ++uses[slotIndex];
        expected[slotIndex] += 1u;
        monotonic = monotonic && submit.timelineValueAfter == expected[slotIndex] &&
                    frames.slot(slotIndex).timelineValue == expected[slotIndex];
        for (u32 other = 0; other < kSlots; ++other) {
            if (other != slotIndex) {
                independent = independent && frames.slot(other).timelineValue == expected[other];
            }
        }
        std::printf("frame %2u slot %u timeline -> %llu (slots: %llu %llu %llu)\n", frame, slotIndex,
                    static_cast<unsigned long long>(submit.timelineValueAfter),
                    static_cast<unsigned long long>(frames.slot(0).timelineValue),
                    static_cast<unsigned long long>(frames.slot(1).timelineValue),
                    static_cast<unsigned long long>(frames.slot(2).timelineValue));
    }

    expectTrue(rotationOk, "frames rotate through slots 0,1,2,0,...");
    expectTrue(monotonic, "each slot's timeline advances by exactly 1 per use");
    expectTrue(independent, "a submit never moves another slot's timeline");
    expectTrue(retiredOnReacquire, "re-acquired slot's previous timeline value already reached on the GPU");
    expectTrue(waitedPrior, "reused slot's submit waits on its own previous timeline value");

    device.waitIdle();
    for (u32 i = 0; i < kSlots; ++i) {
        const fuse::renderer::FrameSyncData& slot = frames.slot(i);
        const u64 gpu = gpuCounter(vkDevice, slot.timelineSemaphore);
        std::printf("slot %u: uses %u, cpu %llu, gpu %llu\n", i, uses[i],
                    static_cast<unsigned long long>(slot.timelineValue), static_cast<unsigned long long>(gpu));
        expectTrue(uses[i] == kFrames / kSlots, "each slot used equally often");
        expectTrue(slot.timelineValue == uses[i], "CPU timeline value == uses of the slot");
        expectTrue(gpu == slot.timelineValue, "GPU timeline counter == CPU value after idle");

        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        const VkSemaphore semaphore = static_cast<VkSemaphore>(slot.timelineSemaphore);
        const u64 value = slot.timelineValue;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &semaphore;
        waitInfo.pValues = &value;
        expectTrue(vkWaitSemaphores(vkDevice, &waitInfo, 0) == VK_SUCCESS, "host wait on reached value succeeds");
        const u64 future = value + 1u;
        waitInfo.pValues = &future;
        expectTrue(vkWaitSemaphores(vkDevice, &waitInfo, 0) == VK_TIMEOUT,
                   "host wait on a value no submit signals times out (slots are independent)");
    }

    context.reset();
    fuse::core::shutdown();
    return b5rhi::finish("fuse_b5_rhi_frame_timelines");
#endif
}
