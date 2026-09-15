#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer::cuda {

struct VulkanBufferImportDesc {
    void* vkDevice = nullptr;
    void* vkMemory = nullptr;
    usize offset = 0;
    usize size = 0;
};

struct VulkanImageImportDesc {
    void* vkDevice = nullptr;
    void* vkMemory = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 format = 0;
};

struct CudaBufferImport {
    void* devicePtr = nullptr;
    bool ok = false;
};

struct CudaSurfaceImport {
    void* surfaceObject = nullptr;
    bool ok = false;
};

/// True when CUDA toolkit and Vulkan backend are both available at runtime.
bool interopAvailable();

CudaBufferImport import_vulkan_buffer(VulkanBufferImportDesc desc);
CudaSurfaceImport import_vulkan_image(VulkanImageImportDesc desc);

void free_cuda_import(void* cudaDevicePtr);
void free_cuda_surface(void* surfaceObject);

} // namespace fuse::renderer::cuda
