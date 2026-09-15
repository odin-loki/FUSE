#include <fuse/core/init.hpp>
#include <fuse/renderer/cuda/interop.hpp>
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
        std::printf("INFO: CUDA+Vulkan interop runtime available — import still stubbed\n");
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
    expectTrue(!bufferImport.ok, "buffer import stub returns failure");
    expectTrue(bufferImport.devicePtr == nullptr, "buffer import stub leaves pointer null");

    fuse::renderer::cuda::VulkanImageImportDesc imageDesc{};
    imageDesc.vkDevice = reinterpret_cast<void*>(0x1);
    imageDesc.vkMemory = reinterpret_cast<void*>(0x2);
    imageDesc.width = 64;
    imageDesc.height = 64;

    const fuse::renderer::cuda::CudaSurfaceImport surfaceImport =
        fuse::renderer::cuda::import_vulkan_image(imageDesc);
    expectTrue(!surfaceImport.ok, "image import stub returns failure");
}

void testSharedTimelineStub() {
    const fuse::renderer::cuda::SharedTimeline timeline =
        fuse::renderer::cuda::SharedTimeline::create(nullptr);
    expectTrue(!timeline.valid, "SharedTimeline stub is invalid until full B2.6");
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
    testSharedTimelineStub();
    testStreamManagerStub();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_cuda_interop: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_cuda_interop: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
