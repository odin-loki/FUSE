#include <fuse/renderer/cuda/interop.hpp>

#include <fuse/jobs/cuda_jobs.hpp>

namespace fuse::renderer::cuda {

const char* interopUnavailableReasonString(InteropUnavailableReason reason) {
    switch (reason) {
    case InteropUnavailableReason::None:
        return "available";
    case InteropUnavailableReason::BuildDisabled:
        return "FUSE_BUILD_CUDA or FUSE_VULKAN_BACKEND disabled";
    case InteropUnavailableReason::NoCudaToolkit:
        return "CUDA toolkit not present at runtime";
    case InteropUnavailableReason::NoVulkanBackend:
        return "Vulkan backend not active";
    case InteropUnavailableReason::MissingHandles:
        return "import descriptor missing device or memory handle";
    case InteropUnavailableReason::ExternalMemoryUnsupported:
        return "external memory import not implemented (B2.6 stub)";
    }
    return "unknown";
}

bool interopAvailable() {
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    return fuse::jobs::cudaJobsAvailable();
#else
    return false;
#endif
}

InteropUnavailableReason interopUnavailableReason() {
#if !defined(FUSE_VULKAN_BACKEND)
    return InteropUnavailableReason::NoVulkanBackend;
#elif !defined(FUSE_HAS_CUDA)
    return InteropUnavailableReason::NoCudaToolkit;
#elif !fuse::jobs::cudaJobsAvailable()
    return InteropUnavailableReason::NoCudaToolkit;
#else
    return InteropUnavailableReason::ExternalMemoryUnsupported;
#endif
}

CudaBufferImport import_vulkan_buffer(VulkanBufferImportDesc desc) {
    CudaBufferImport result{};
    if (!interopAvailable()) {
        result.reason = interopUnavailableReasonString(interopUnavailableReason());
        return result;
    }
    if (desc.vkDevice == nullptr || desc.vkMemory == nullptr || desc.size == 0) {
        result.reason = interopUnavailableReasonString(InteropUnavailableReason::MissingHandles);
        return result;
    }
    result.reason = interopUnavailableReasonString(InteropUnavailableReason::ExternalMemoryUnsupported);
    return result;
}

CudaSurfaceImport import_vulkan_image(VulkanImageImportDesc desc) {
    CudaSurfaceImport result{};
    if (!interopAvailable()) {
        result.reason = interopUnavailableReasonString(interopUnavailableReason());
        return result;
    }
    if (desc.vkDevice == nullptr || desc.vkMemory == nullptr || desc.width == 0 || desc.height == 0) {
        result.reason = interopUnavailableReasonString(InteropUnavailableReason::MissingHandles);
        return result;
    }
    result.reason = interopUnavailableReasonString(InteropUnavailableReason::ExternalMemoryUnsupported);
    return result;
}

void free_cuda_import(void* /*cudaDevicePtr*/) {}

void free_cuda_surface(void* /*surfaceObject*/) {}

} // namespace fuse::renderer::cuda
