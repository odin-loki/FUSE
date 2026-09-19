#include <fuse/renderer/cuda/interop_fill.hpp>

#include <fuse/jobs/cuda_jobs.hpp>
#include <fuse/jobs/job_counter.hpp>
#include <fuse/renderer/cuda/interop.hpp>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#endif

namespace fuse::renderer::cuda {

namespace {

#if defined(FUSE_HAS_CUDA)
extern "C" void launch_interop_fill_kernel(void* surfaceObject, unsigned int width, unsigned int height,
                                           const unsigned char rgba[4], void* stream);
#endif

InteropFillResult fillFromImportedSurface(const InteropFillDesc& desc, CudaSurfaceImport imported) {
    InteropFillResult result{};
    if (!imported.ok || imported.surfaceObject == nullptr) {
        result.reason = imported.reason != nullptr ? imported.reason : "cuda surface import failed";
        return result;
    }

#if defined(FUSE_HAS_CUDA)
    launch_interop_fill_kernel(imported.surfaceObject, desc.width, desc.height, desc.rgba, desc.cudaStream);
    const cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        result.reason = cudaGetErrorString(err);
        fuse::renderer::cuda::free_cuda_surface(imported.surfaceObject);
        return result;
    }

    const cudaStream_t stream =
        desc.cudaStream != nullptr ? static_cast<cudaStream_t>(desc.cudaStream) : 0;
    const cudaError_t syncErr = cudaStreamSynchronize(stream);
    if (syncErr != cudaSuccess) {
        result.reason = cudaGetErrorString(syncErr);
        fuse::renderer::cuda::free_cuda_surface(imported.surfaceObject);
        return result;
    }

    fuse::renderer::cuda::free_cuda_surface(imported.surfaceObject);
    result.ok = true;
    result.reason = "interop fill kernel completed";
    return result;
#else
    (void)desc;
    result.reason = interopUnavailableReasonString(InteropUnavailableReason::NoCudaToolkit);
    return result;
#endif
}

} // namespace

bool interopFillAvailable() {
#if defined(FUSE_HAS_CUDA)
    return interopAvailable();
#else
    return false;
#endif
}

InteropFillResult fillInteropTexture(const InteropFillDesc& desc) {
    InteropFillResult result{};
    VulkanImageImportDesc handleProbe{};
    handleProbe.exportedHandle = desc.exportedMemoryHandle;
    handleProbe.allocationSize = desc.allocationSize;
    if (!importDescHasExportedHandle(handleProbe)) {
        result.stubPath = true;
        result.reason = "interop fill requires exported platform memory handle";
        return result;
    }
    if (desc.width == 0 || desc.height == 0) {
        result.reason = "interop fill requires non-zero dimensions";
        return result;
    }

    if (!interopFillAvailable()) {
        result.stubPath = true;
        result.reason = interopUnavailableReasonString(interopUnavailableReason());
        return result;
    }

    VulkanImageImportDesc importDesc{};
    importDesc.vkDevice = reinterpret_cast<void*>(0x1);
    importDesc.vkMemory = reinterpret_cast<void*>(0x2);
    importDesc.exportedHandle = desc.exportedMemoryHandle;
    importDesc.allocationSize = desc.allocationSize;
    importDesc.width = desc.width;
    importDesc.height = desc.height;

    const CudaSurfaceImport imported = import_vulkan_image(importDesc);
    result = fillFromImportedSurface(desc, imported);
    return result;
}

InteropFillResult submitInteropFillJob(const InteropFillDesc& desc) {
    InteropFillResult result{};
    VulkanImageImportDesc handleProbe{};
    handleProbe.exportedHandle = desc.exportedMemoryHandle;
    handleProbe.allocationSize = desc.allocationSize;
    if (!importDescHasExportedHandle(handleProbe)) {
        result.stubPath = true;
        result.reason = "interop fill job requires exported platform memory handle";
        return result;
    }
    if (desc.width == 0 || desc.height == 0) {
        result.reason = "interop fill job requires non-zero dimensions";
        return result;
    }

    if (!interopFillAvailable()) {
        result.stubPath = true;
        result.reason = interopUnavailableReasonString(interopUnavailableReason());
        return result;
    }

    fuse::jobs::JobCounter counter;
    InteropFillResult jobResult{};
    const InteropFillDesc jobDesc = desc;

    fuse::jobs::CUDAJobDesc cudaDesc{};
    cudaDesc.tag = "interop_fill";
    cudaDesc.counter = &counter;
    cudaDesc.kernel_launcher = [jobDesc, &jobResult](fuse::jobs::CUDAStreamHandle stream) {
        InteropFillDesc local = jobDesc;
        local.cudaStream = stream.native;

        if (local.frameSync != nullptr) {
            (void)local.frameSync->waitJobLaneOnRenderSignal(stream.native, local.frameIndex);
        }

        VulkanImageImportDesc importDesc{};
        importDesc.vkDevice = reinterpret_cast<void*>(0x1);
        importDesc.vkMemory = reinterpret_cast<void*>(0x2);
        importDesc.exportedHandle = local.exportedMemoryHandle;
        importDesc.allocationSize = local.allocationSize;
        importDesc.width = local.width;
        importDesc.height = local.height;

        const CudaSurfaceImport imported = import_vulkan_image(importDesc);
        jobResult = fillFromImportedSurface(local, imported);

        if (local.frameSync != nullptr && jobResult.ok) {
            (void)local.frameSync->signalJobLaneComplete(stream.native, local.frameIndex);
        }
    };

    fuse::jobs::submit_cuda(std::move(cudaDesc));
    counter.wait();

    if (desc.frameSync != nullptr && jobResult.ok) {
        (void)desc.frameSync->waitRenderLane(nullptr, desc.frameIndex);
    }

    jobResult.stubPath = false;
    return jobResult;
}

InteropFillLoadStressResult stressInteropFillUnderLoad(u32 iterations) {
    InteropFillLoadStressResult result{};
    InteropFillDesc desc{};
    desc.exportedMemoryHandle = reinterpret_cast<void*>(0x10);
    desc.allocationSize = 4096;
    desc.width = 64;
    desc.height = 64;

    for (u32 i = 0; i < iterations; ++i) {
        ++result.attempts;
        const InteropFillResult fill = fillInteropTexture(desc);
        if (fill.ok) {
            ++result.successes;
        } else if (fill.stubPath) {
            ++result.stubPaths;
        } else {
            ++result.failures;
        }
    }

    return result;
}

FrameSyncInteropCombinedStressResult stressFrameSyncAndInteropFillUnderLoad(u32 frameCount) {
    FrameSyncInteropCombinedStressResult result{};
    if (frameCount == 0u) {
        return result;
    }

    FrameSyncPair pair = FrameSyncPair::create(nullptr, nullptr);
    result.frameSync = stressFrameSyncUnderLoad(pair, nullptr, nullptr, frameCount);
    result.interopFill = stressInteropFillUnderLoad(frameCount);

    InteropFillDesc jobDesc{};
    jobDesc.exportedMemoryHandle = reinterpret_cast<void*>(0x10);
    jobDesc.allocationSize = 4096;
    jobDesc.width = 64;
    jobDesc.height = 64;
    jobDesc.frameSync = &pair;

    for (u32 i = 0; i < frameCount; ++i) {
        jobDesc.frameIndex = static_cast<u64>(i) + 1u;
        ++result.jobLaneFillAttempts;
        const InteropFillResult jobFill = submitInteropFillJob(jobDesc);
        if (jobFill.stubPath) {
            ++result.jobLaneFillStubPaths;
        }
    }

    pair.destroy(nullptr);
    return result;
}

} // namespace fuse::renderer::cuda
