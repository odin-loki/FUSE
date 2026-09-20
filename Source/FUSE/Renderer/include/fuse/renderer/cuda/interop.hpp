#pragma once

#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer::cuda {

struct VulkanBufferImportDesc {
    void* vkDevice = nullptr;
    void* vkMemory = nullptr;
    /// Platform-exported handle (Linux fd, Win32 HANDLE) from `vkGetMemoryFdKHR` / Win32 export.
    void* exportedHandle = nullptr;
    u64 allocationSize = 0;
    usize offset = 0;
    usize size = 0;
};

struct VulkanImageImportDesc {
    void* vkDevice = nullptr;
    void* vkMemory = nullptr;
    void* exportedHandle = nullptr;
    u64 allocationSize = 0;
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

/// True when the CUDA device reports external-memory import support (toolkit builds only).
bool cudaExternalMemoryImportSupported();

/// True when import descriptors include an exported platform handle + allocation size.
bool importDescHasExportedHandle(const VulkanBufferImportDesc& desc);
bool importDescHasExportedHandle(const VulkanImageImportDesc& desc);

VulkanBufferImportDesc makeBufferImportDesc(void* vkDevice, const Buffer& buffer);
VulkanImageImportDesc makeImageImportDesc(void* vkDevice, const Texture& texture);

CudaBufferImport import_vulkan_buffer(VulkanBufferImportDesc desc);
CudaSurfaceImport import_vulkan_image(VulkanImageImportDesc desc);
CudaBufferImport import_vulkan_buffer(void* vkDevice, const Buffer& buffer);
CudaSurfaceImport import_vulkan_image(void* vkDevice, const Texture& texture);

void free_cuda_import(void* cudaDevicePtr);
void free_cuda_surface(void* surfaceObject);

} // namespace fuse::renderer::cuda
