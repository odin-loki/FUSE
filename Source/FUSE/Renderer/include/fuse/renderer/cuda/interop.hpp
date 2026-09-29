#pragma once

#include <fuse/renderer/resources.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer::cuda {

/// Handle ownership (both platforms): the exported handle stays owned by the Vulkan resource (the
/// allocator closes it in destroyBuffer / destroyImage). Win32 imports never take the NT handle; on
/// POSIX the import hands CUDA a dup() of the fd, so the resource's fd stays valid and can be imported
/// again.
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
    /// GpuFormat numeric (VkFormat value): RGBA8 (unorm/srgb), RGBA16F, RG16F, R32F map to their CUDA
    /// channel formats; anything else is imported as RGBA8.
    u32 format = 0;
    /// The Vulkan image has VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT (maps to cudaArrayColorAttachment).
    bool colorAttachment = false;
};

struct CudaBufferImport {
    void* devicePtr = nullptr;
    /// cudaExternalMemory_t from a successful import. Release with release_imported_buffer
    /// before the Vulkan allocation is freed. Not a cudaMalloc pointer.
    void* externalMemory = nullptr;
    bool ok = false;
    const char* reason = nullptr;
};

struct CudaSurfaceImport {
    /// cudaSurfaceObject_t (as void*) over mip 0 of the imported image.
    void* surfaceObject = nullptr;
    /// cudaExternalMemory_t / cudaMipmappedArray_t / cudaArray_t (mip 0) behind the surface. Release all
    /// of them with release_imported_surface before the Vulkan image is destroyed.
    void* externalMemory = nullptr;
    void* mipmappedArray = nullptr;
    void* array = nullptr;
    /// cudaArray* flags the mapping was made with (cudaArraySurfaceLoadStore first, then without it when
    /// the driver refuses; plus cudaArrayColorAttachment for colour attachments).
    u32 arrayFlags = 0;
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
/// Drops a buffer imported with cudaImportExternalMemory. Does not cudaFree the mapped pointer.
void release_imported_buffer(CudaBufferImport& imported);
/// Destroys the surface object only (legacy; leaves the mapping alive — prefer release_imported_surface).
void free_cuda_surface(void* surfaceObject);
/// Destroys the surface object, frees the mapped mipmapped array and destroys the external memory.
void release_imported_surface(CudaSurfaceImport& imported);

/// CUDA device ordinal whose UUID matches the Vulkan physical device (VkPhysicalDeviceIDProperties::
/// deviceUUID vs cudaDeviceProp::uuid); -1 when none does, or without CUDA + Vulkan. Interop must run on
/// that device (cudaSetDevice) — on a multi-GPU machine CUDA device 0 need not be the Vulkan device.
int cuda_device_for_vulkan(void* vkPhysicalDevice);

} // namespace fuse::renderer::cuda
