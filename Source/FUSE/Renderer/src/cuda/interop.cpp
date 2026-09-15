#include <fuse/renderer/cuda/interop.hpp>

#include <fuse/jobs/cuda_jobs.hpp>

namespace fuse::renderer::cuda {

bool interopAvailable() {
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    return fuse::jobs::cudaJobsAvailable();
#else
    return false;
#endif
}

CudaBufferImport import_vulkan_buffer(VulkanBufferImportDesc desc) {
    CudaBufferImport result{};
    if (!interopAvailable()) {
        return result;
    }
    if (desc.vkDevice == nullptr || desc.vkMemory == nullptr || desc.size == 0) {
        return result;
    }
    // Full external-memory import deferred — stub keeps API surface without drivers on CI.
    return result;
}

CudaSurfaceImport import_vulkan_image(VulkanImageImportDesc desc) {
    CudaSurfaceImport result{};
    if (!interopAvailable()) {
        return result;
    }
    if (desc.vkDevice == nullptr || desc.vkMemory == nullptr || desc.width == 0 || desc.height == 0) {
        return result;
    }
    return result;
}

void free_cuda_import(void* /*cudaDevicePtr*/) {}

void free_cuda_surface(void* /*surfaceObject*/) {}

} // namespace fuse::renderer::cuda
