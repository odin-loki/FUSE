#include <fuse/core/init.hpp>
#include <fuse/renderer/cuda/interop.hpp>
#include <fuse/renderer/cuda/interop_fill.hpp>
#include <fuse/renderer/cuda/stream_manager.hpp>
#include <fuse/renderer/cuda/vk_sync.hpp>

#include <cstdio>
#include <cstdlib>

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
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (timeline.driverWired) {
        expectTrue(timeline.valid, "SharedTimeline valid when driver wired");
        expectTrue(timeline.signalVulkan(nullptr, 1u) == false,
                   "signalVulkan requires valid VkDevice handle");
    } else {
        expectTrue(!timeline.valid, "SharedTimeline invalid without device handles");
    }
#else
    expectTrue(!timeline.valid, "SharedTimeline stub is invalid until full B2.6");
#endif
    expectTrue(timeline.message != nullptr, "SharedTimeline exposes honest message");
    expectTrue(!timeline.waitCuda(nullptr, 1u), "waitCuda returns false when invalid");
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

void testInteropFillStub() {
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

} // namespace

int main() {
    fuse::core::initialize();

    testInteropUnavailableOnCi();
    testInteropReasonStrings();
    testSharedTimelineStub();
    testFrameSyncPairStub();
    testFrameSyncProgressStub();
    testInteropFillStub();
    testStreamManagerStub();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_cuda_interop: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_cuda_interop: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
