#include <fuse/core/init.hpp>
#include <fuse/renderer/cuda/interop.hpp>
#include <fuse/renderer/cuda/interop_fill.hpp>
#include <fuse/renderer/cuda/stream_manager.hpp>
#include <fuse/renderer/cuda/vk_sync.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testInteropUnavailableOnCi() {
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (fuse::renderer::cuda::interopAvailable()) {
        std::printf("INFO: CUDA+Vulkan interop runtime available\n");
    } else {
        std::printf("SKIP: toolkit present but interop runtime unavailable\n");
    }
#else
    expectTrue(!fuse::renderer::cuda::interopAvailable(), "CI stub build has no interop runtime");
#endif

    fuse::renderer::cuda::VulkanBufferImportDesc bufferDesc{};
    bufferDesc.vkDevice = reinterpret_cast<void*>(0x1);
    bufferDesc.vkMemory = reinterpret_cast<void*>(0x2);
    bufferDesc.size = 4096;

    const fuse::renderer::cuda::CudaBufferImport bufferImport =
        fuse::renderer::cuda::import_vulkan_buffer(bufferDesc);
    expectTrue(!bufferImport.ok, "buffer import without exported handle returns failure");
    expectTrue(bufferImport.devicePtr == nullptr, "buffer import stub leaves pointer null");
    expectTrue(bufferImport.reason != nullptr, "buffer import stub exposes reason");

    fuse::renderer::cuda::VulkanImageImportDesc imageDesc{};
    imageDesc.vkDevice = reinterpret_cast<void*>(0x1);
    imageDesc.vkMemory = reinterpret_cast<void*>(0x2);
    imageDesc.width = 64;
    imageDesc.height = 64;

    const fuse::renderer::cuda::CudaSurfaceImport surfaceImport =
        fuse::renderer::cuda::import_vulkan_image(imageDesc);
    expectTrue(!surfaceImport.ok, "image import without exported handle returns failure");
    expectTrue(surfaceImport.reason != nullptr, "image import stub exposes reason");
}

void testSharedTimelineStub() {
    const fuse::renderer::cuda::SharedTimeline timeline =
        fuse::renderer::cuda::SharedTimeline::create(nullptr, nullptr);
    expectTrue(!timeline.valid, "SharedTimeline invalid without device handles");
    expectTrue(timeline.vkSemaphore == nullptr, "null-handle create leaves vkSemaphore null");
    expectTrue(!timeline.driverWired, "null-handle create is not driver-wired");
    expectTrue(timeline.message != nullptr, "SharedTimeline exposes honest message");
    expectTrue(!timeline.signalVulkan(nullptr, 1u), "signalVulkan returns false when invalid");
    expectTrue(!timeline.waitVulkan(nullptr, 1u), "waitVulkan returns false when invalid");
    expectTrue(!timeline.waitCuda(nullptr, 1u), "waitCuda returns false when invalid");
    expectTrue(!timeline.signalCuda(nullptr, 1u), "signalCuda returns false when invalid");
}

void testSharedTimelineVulkanDevice() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    expectTrue(instance != nullptr, "VulkanInstance allocated for SharedTimeline");
    if (instance == nullptr) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    expectTrue(device != nullptr, "VulkanDevice allocated for SharedTimeline");
    if (device == nullptr) {
        return;
    }

    fuse::renderer::cuda::SharedTimeline timeline = fuse::renderer::cuda::SharedTimeline::create(
        device->nativeHandle(), device->nativePhysicalDevice());

    if (!device->isValid() || !device->info().timelineSemaphore) {
        expectTrue(!timeline.valid, "SharedTimeline invalid when device stub or no timeline feature");
        expectTrue(timeline.vkSemaphore == nullptr, "stub SharedTimeline has no VkSemaphore");
        expectTrue(!timeline.driverWired, "stub SharedTimeline is not driver-wired");
        expectTrue(!timeline.signalVulkan(device->nativeHandle(), 1u),
                   "signalVulkan false when timeline invalid");
        expectTrue(!timeline.waitVulkan(device->nativeHandle(), 1u),
                   "waitVulkan false when timeline invalid");
        expectTrue(timeline.message != nullptr, "invalid SharedTimeline exposes honest message");
        return;
    }

    expectTrue(timeline.valid, "SharedTimeline valid with Vulkan timeline device");
    expectTrue(timeline.vkSemaphore != nullptr, "SharedTimeline has VkSemaphore");
    expectTrue(timeline.message != nullptr, "SharedTimeline exposes honest message");
    if (timeline.driverWired) {
        expectTrue(timeline.cudaSemaphore != nullptr, "driverWired SharedTimeline has cudaSemaphore");
    } else {
        expectTrue(timeline.cudaSemaphore == nullptr,
                   "Vulkan-only SharedTimeline has null cudaSemaphore");
        expectTrue(std::strstr(timeline.message, "Vulkan-only") != nullptr,
                   "Vulkan-only SharedTimeline reports honest CUDA-unavailable message");
        expectTrue(!timeline.waitCuda(nullptr, 1u), "waitCuda requires driverWired");
        expectTrue(!timeline.signalCuda(nullptr, 1u), "signalCuda requires driverWired");
    }

    expectTrue(timeline.signalVulkan(device->nativeHandle(), 1u),
               "signalVulkan succeeds on valid Vulkan timeline");
    expectTrue(timeline.waitVulkan(device->nativeHandle(), 1u),
               "waitVulkan succeeds on valid Vulkan timeline");

    timeline.destroy(device->nativeHandle());
    expectTrue(!timeline.valid, "SharedTimeline invalid after destroy");
    expectTrue(timeline.vkSemaphore == nullptr, "vkSemaphore cleared after destroy");
    expectTrue(timeline.cudaSemaphore == nullptr, "cudaSemaphore cleared after destroy");
    expectTrue(!timeline.driverWired, "driverWired cleared after destroy");
}

void testFrameSyncPairVulkanOnly() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    expectTrue(instance != nullptr, "VulkanInstance allocated for FrameSyncPair");
    if (instance == nullptr) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    expectTrue(device != nullptr, "VulkanDevice allocated for FrameSyncPair");
    if (device == nullptr) {
        return;
    }

    fuse::renderer::cuda::FrameSyncPair pair = fuse::renderer::cuda::FrameSyncPair::create(
        device->nativeHandle(), device->nativePhysicalDevice());

    if (!device->isValid() || !device->info().timelineSemaphore) {
        expectTrue(!pair.valid(), "FrameSyncPair invalid when device stub or no timeline feature");
        expectTrue(!pair.signalRenderLane(device->nativeHandle(), 1u),
                   "signalRenderLane false when timeline invalid");
        expectTrue(!pair.waitJobLaneOnRenderSignal(nullptr, 1u),
                   "waitJobLane false when timeline invalid");
        pair.destroy(device->nativeHandle());
        return;
    }

    expectTrue(pair.valid(), "FrameSyncPair valid with Vulkan timeline device");
    expectTrue(pair.signalRenderLane(device->nativeHandle(), 1u),
               "signalRenderLane succeeds on valid Vulkan timeline without CUDA import");
    expectTrue(pair.lastProgress().renderLaneSignals == 1u, "render lane signal counted");
    expectTrue(pair.lastProgress().frameIndex == 1u, "progress tracks signaled frame");

    if (!pair.driverWired()) {
        expectTrue(!pair.waitJobLaneOnRenderSignal(nullptr, 1u),
                   "waitJobLane stays false without CUDA");
        expectTrue(!pair.signalJobLaneComplete(nullptr, 1u),
                   "signalJobLaneComplete stays false without CUDA");
        expectTrue(!pair.waitRenderLane(device->nativeHandle(), 1u),
                   "waitRenderLane stays false without CUDA (no hang)");
    }

    pair.destroy(device->nativeHandle());
    expectTrue(!pair.valid(), "FrameSyncPair invalid after destroy");
}

void testFrameSyncPairStub() {
    const fuse::renderer::cuda::FrameSyncPair pair =
        fuse::renderer::cuda::FrameSyncPair::create(nullptr, nullptr);
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (pair.driverWired()) {
        expectTrue(pair.valid(), "FrameSyncPair valid when timelines driver-wired");
    } else {
        expectTrue(!pair.valid(), "FrameSyncPair invalid without Vulkan device");
    }
#else
    expectTrue(!pair.valid(), "FrameSyncPair invalid in stub build");
#endif
}

void testFrameSyncProgressStub() {
    fuse::renderer::cuda::FrameSyncPair pair =
        fuse::renderer::cuda::FrameSyncPair::create(nullptr, nullptr);

    expectTrue(pair.signalRenderLane(nullptr, 3u) == pair.driverWired(),
               "signalRenderLane succeeds only when driver-wired");
    expectTrue(pair.lastProgress().renderLaneSignals == 1u, "render lane signal counted");
    expectTrue(pair.lastProgress().frameIndex == 3u, "progress tracks frame index");

    expectTrue(pair.waitJobLaneOnRenderSignal(nullptr, 3u) == pair.driverWired(),
               "job lane wait succeeds only when driver-wired");
    expectTrue(pair.lastProgress().jobLaneWaits == 1u, "job lane wait counted");

    expectTrue(pair.signalJobLaneComplete(nullptr, 3u) == pair.driverWired(),
               "job lane signal succeeds only when driver-wired");
    expectTrue(pair.lastProgress().jobLaneSignals == 1u, "job lane signal counted");

    expectTrue(pair.waitRenderLane(nullptr, 3u) == pair.driverWired(),
               "render lane wait succeeds only when driver-wired");
    expectTrue(pair.lastProgress().renderLaneWaits == 1u, "render lane wait counted");

    pair.destroy(nullptr);
}

void testFrameSyncLoadStressStub() {
    fuse::renderer::cuda::FrameSyncPair pair =
        fuse::renderer::cuda::FrameSyncPair::create(nullptr, nullptr);

    const fuse::renderer::cuda::FrameSyncLoadStressResult stress =
        fuse::renderer::cuda::stressFrameSyncUnderLoad(pair, nullptr, nullptr, 24u);

    expectTrue(stress.framesAttempted == 24u, "stress attempts requested frame count");
    expectTrue(stress.framesCompleted == 24u, "stub build completes bookkeeping for each frame");
    expectTrue(stress.finalProgress.renderLaneSignals == 24u, "render lane signals scale with load");
    expectTrue(stress.finalProgress.jobLaneWaits == 24u, "job lane waits scale with load");
    expectTrue(stress.finalProgress.jobLaneSignals == 24u, "job lane signals scale with load");
    expectTrue(stress.finalProgress.renderLaneWaits == 24u, "render lane waits scale with load");
    expectTrue(stress.finalProgress.frameIndex == 24u, "final progress tracks last frame index");

    pair.destroy(nullptr);
}

void testFrameSyncTeardownStressStub() {
    const fuse::renderer::cuda::FrameSyncTeardownStressResult stress =
        fuse::renderer::cuda::stressFrameSyncTeardownCycle(nullptr, nullptr, nullptr, 6u, 16u);

    expectTrue(stress.teardownCycles == 6u, "teardown stress completes all cycles");
    expectTrue(stress.framesPerCycle == 16u, "teardown stress records frames per cycle");
    expectTrue(stress.totalFramesCompleted == 96u, "teardown stress completes every frame");
    expectTrue(stress.finalProgress.renderLaneSignals == 16u,
               "final cycle records render lane signal count");
    expectTrue(stress.finalProgress.jobLaneWaits == 16u, "final cycle records job lane waits");
}

void testFrameSyncInteropCombinedStressStub() {
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (fuse::renderer::cuda::interopFillAvailable()) {
        std::printf("SKIP: interop fill runtime available — combined stub counts deferred\n");
        return;
    }
#endif
    const fuse::renderer::cuda::FrameSyncInteropCombinedStressResult stress =
        fuse::renderer::cuda::stressFrameSyncAndInteropFillUnderLoad(24u);

    expectTrue(stress.frameSync.framesAttempted == 24u, "combined stress runs frame sync load");
    expectTrue(stress.frameSync.framesCompleted == 24u, "combined stress completes frame sync bookkeeping");
    expectTrue(stress.interopFill.attempts == 24u, "combined stress runs interop fill load");
    expectTrue(stress.jobLaneFillAttempts == 24u, "combined stress runs job-lane fill attempts");
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (fuse::renderer::cuda::interopFillAvailable()) {
        std::printf("SKIP: interop fill runtime available — combined stub counts deferred\n");
        return;
    }
#endif
    expectTrue(stress.interopFill.stubPaths == 24u, "combined interop fill stays on stub path");
    expectTrue(stress.jobLaneFillStubPaths == 24u, "combined job-lane fill stays on stub path");
}

void testInteropFillLoadStressStub() {
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (fuse::renderer::cuda::interopFillAvailable()) {
        std::printf("SKIP: interop fill runtime available — load stress needs exported handle\n");
        return;
    }
#endif
    const fuse::renderer::cuda::InteropFillLoadStressResult stress =
        fuse::renderer::cuda::stressInteropFillUnderLoad(24u);

    expectTrue(stress.attempts == 24u, "interop fill stress runs all iterations");
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (fuse::renderer::cuda::interopFillAvailable()) {
        std::printf("SKIP: interop fill runtime available — load stress needs exported handle\n");
        return;
    }
#endif
    expectTrue(stress.stubPaths == 24u, "CI stub path counts every iteration");
    expectTrue(stress.successes == 0u, "CI stub path has no successes without exported handle");
    expectTrue(stress.failures == 0u, "stub path is not counted as hard failure");
}

void testInteropFillStub() {
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (fuse::renderer::cuda::interopFillAvailable()) {
        std::printf("SKIP: interop fill runtime available — needs real exported handle\n");
        return;
    }
#endif
    fuse::renderer::cuda::InteropFillDesc desc{};
    desc.exportedMemoryHandle = reinterpret_cast<void*>(0x10);
    desc.allocationSize = 4096;
    desc.width = 64;
    desc.height = 64;

#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (fuse::renderer::cuda::interopFillAvailable()) {
        std::printf("SKIP: interop fill runtime available — needs real exported handle\n");
        return;
    }
#endif

    expectTrue(!fuse::renderer::cuda::interopFillAvailable(),
               "CI stub build has no interop fill runtime");

    const fuse::renderer::cuda::InteropFillResult syncFill =
        fuse::renderer::cuda::fillInteropTexture(desc);
    expectTrue(!syncFill.ok, "sync interop fill fails without runtime");
    expectTrue(syncFill.stubPath, "sync interop fill reports stub path");
    expectTrue(syncFill.reason != nullptr, "sync interop fill exposes reason");

    const fuse::renderer::cuda::InteropFillResult jobFill =
        fuse::renderer::cuda::submitInteropFillJob(desc);
    expectTrue(!jobFill.ok, "job-lane interop fill fails without runtime");
    expectTrue(jobFill.stubPath, "job-lane interop fill reports stub path");
    expectTrue(jobFill.reason != nullptr, "job-lane interop fill exposes reason");
}

void testInteropReasonStrings() {
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (fuse::renderer::cuda::interopAvailable()) {
        std::printf("SKIP: interop runtime available — reason is None\n");
        return;
    }
#endif
    const auto reason = fuse::renderer::cuda::interopUnavailableReason();
    expectTrue(reason != fuse::renderer::cuda::InteropUnavailableReason::None,
               "stub build reports unavailable interop reason");
    expectTrue(fuse::renderer::cuda::interopUnavailableReasonString(reason) != nullptr,
               "reason string non-null");
}

void testExportedBufferImport() {
#if !defined(FUSE_HAS_CUDA) || !defined(FUSE_VULKAN_BACKEND)
    std::printf("SKIP: exported buffer import needs CUDA and Vulkan\n");
    return;
#else
    if (!fuse::renderer::cuda::interopAvailable()) {
        std::printf("SKIP: external memory import not available\n");
        return;
    }

    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    expectTrue(instance != nullptr, "VulkanInstance for exported buffer import");
    if (instance == nullptr) {
        return;
    }
    auto device = fuse::renderer::VulkanDevice::create(*instance);
    expectTrue(device != nullptr && device->isValid(), "VulkanDevice for exported buffer import");
    if (device == nullptr || !device->isValid()) {
        return;
    }

    auto allocator = fuse::renderer::GpuAllocator::create(*device);
    expectTrue(allocator != nullptr && allocator->isValid(), "GpuAllocator for exported buffer");
    if (allocator == nullptr || !allocator->isValid()) {
        return;
    }

    fuse::renderer::BufferDesc desc{};
    desc.size = 256;
    desc.usage = static_cast<fuse::renderer::BufferUsage>(
        static_cast<fuse::u32>(fuse::renderer::BufferUsage::Storage) |
        static_cast<fuse::u32>(fuse::renderer::BufferUsage::TransferSrc) |
        static_cast<fuse::u32>(fuse::renderer::BufferUsage::TransferDst));
    desc.memoryUsage = fuse::renderer::MemoryUsage::GpuOnly;
    desc.cudaInterop = true;
    desc.name = "fuse.cuda.import";

    fuse::renderer::Buffer buffer{};
    expectTrue(allocator->createBuffer(desc, buffer), "exportable Vulkan buffer created");
    expectTrue(buffer.exportedHandle != nullptr, "Vulkan buffer exported a Win32 handle");
    if (buffer.exportedHandle == nullptr) {
        allocator->destroyBuffer(buffer);
        return;
    }

    const fuse::renderer::cuda::CudaBufferImport imported =
        fuse::renderer::cuda::import_vulkan_buffer(device->nativeHandle(), buffer);
    expectTrue(imported.ok, imported.reason != nullptr ? imported.reason : "cudaImportExternalMemory failed");
    expectTrue(imported.devicePtr != nullptr, "imported buffer has a CUDA pointer");
    if (!imported.ok || imported.devicePtr == nullptr) {
        fuse::renderer::cuda::CudaBufferImport failed = imported;
        fuse::renderer::cuda::release_imported_buffer(failed);
        allocator->destroyBuffer(buffer);
        return;
    }

    const fuse::u32 pattern = 0xC0DA5A5Au;
    expectTrue(cudaMemcpy(imported.devicePtr, &pattern, sizeof(pattern), cudaMemcpyHostToDevice) == cudaSuccess,
               "cudaMemcpy into imported Vulkan memory");
    fuse::u32 readback = 0;
    expectTrue(cudaMemcpy(&readback, imported.devicePtr, sizeof(readback), cudaMemcpyDeviceToHost) == cudaSuccess,
               "cudaMemcpy from imported Vulkan memory");
    expectTrue(readback == pattern, "round-trip through cudaImportExternalMemory");
    std::printf("cudaImportExternalMemory round-trip ok value=0x%08x\n", readback);

    fuse::renderer::cuda::CudaBufferImport owned = imported;
    fuse::renderer::cuda::release_imported_buffer(owned);
    allocator->destroyBuffer(buffer);
#endif
}

void testStreamManagerStub() {
    fuse::renderer::cuda::StreamManager streams;
    streams.init();
#if defined(FUSE_HAS_CUDA)
    if (streams.available()) {
        expectTrue(streams.get(fuse::renderer::cuda::CUDAStreamKind::Render) != nullptr,
                   "render stream allocated when CUDA device present");
    } else {
        expectTrue(streams.get(fuse::renderer::cuda::CUDAStreamKind::Render) == nullptr,
                   "no CUDA device keeps streams null");
    }
#else
    expectTrue(!streams.available(), "stub build keeps StreamManager unavailable");
    expectTrue(streams.get(fuse::renderer::cuda::CUDAStreamKind::Render) == nullptr,
               "stub build returns null stream");
#endif
    streams.shutdown();
}

void testStreamManagerSynchronize() {
    fuse::renderer::cuda::StreamManager sm;
    sm.init();

#if defined(FUSE_HAS_CUDA)
    if (sm.available()) {
        expectTrue(sm.createdStreamCount() == static_cast<fuse::u32>(fuse::renderer::cuda::CUDAStreamKind::Count),
                   "available StreamManager creates one stream per kind");
        expectTrue(sm.synchronize(fuse::renderer::cuda::CUDAStreamKind::Render),
                   "synchronize Render succeeds when CUDA available");
        expectTrue(sm.synchronizeAll(), "synchronizeAll succeeds when CUDA available");
    } else {
        expectTrue(sm.createdStreamCount() == 0u, "unavailable CUDA leaves createdStreamCount at 0");
        expectTrue(!sm.synchronize(fuse::renderer::cuda::CUDAStreamKind::Render),
                   "synchronize returns false when CUDA unavailable");
        expectTrue(sm.synchronizeAll(), "synchronizeAll is vacuously true with no streams");
    }
#else
    expectTrue(sm.createdStreamCount() == 0u, "stub StreamManager createdStreamCount is 0");
    expectTrue(!sm.synchronize(fuse::renderer::cuda::CUDAStreamKind::Render),
               "stub synchronize returns false");
    expectTrue(sm.synchronizeAll(), "stub synchronizeAll is vacuously true");
#endif

    sm.shutdown();
    expectTrue(sm.get(fuse::renderer::cuda::CUDAStreamKind::Render) == nullptr,
               "get returns null after StreamManager shutdown");
}

} // namespace

int main() {
    fuse::core::initialize();

    testInteropUnavailableOnCi();
    testInteropReasonStrings();
    testSharedTimelineStub();
    testSharedTimelineVulkanDevice();
    testFrameSyncPairVulkanOnly();
    testFrameSyncPairStub();
    testFrameSyncProgressStub();
    testFrameSyncLoadStressStub();
    testFrameSyncTeardownStressStub();
    testFrameSyncInteropCombinedStressStub();
    testInteropFillLoadStressStub();
    testInteropFillStub();
    testExportedBufferImport();
    testStreamManagerStub();
    testStreamManagerSynchronize();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_cuda_interop: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_cuda_interop: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
