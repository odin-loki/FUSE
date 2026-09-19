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
    const char* reason = nullptr;
};

struct CudaSurfaceImport {
    void* surfaceObject = nullptr;
    bool ok = false;
    const char* reason = nullptr;
};

/// Why external-memory import may be unavailable (honest CI messaging).
enum class InteropUnavailableReason : u8 {
    None = 0,
    BuildDisabled,
    NoCudaToolkit,
    NoVulkanBackend,
    MissingHandles,
    ExternalMemoryUnsupported,
};

const char* interopUnavailableReasonString(InteropUnavailableReason reason);

/// True when CUDA toolkit and Vulkan backend are both available at runtime.
bool interopAvailable();
InteropUnavailableReason interopUnavailableReason();

CudaBufferImport import_vulkan_buffer(VulkanBufferImportDesc desc);
CudaSurfaceImport import_vulkan_image(VulkanImageImportDesc desc);

void free_cuda_import(void* cudaDevicePtr);
void free_cuda_surface(void* surfaceObject);

} // namespace fuse::renderer::cuda
