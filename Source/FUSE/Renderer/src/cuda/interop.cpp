#include <fuse/renderer/cuda/interop.hpp>

#include <fuse/jobs/cuda_jobs.hpp>

#include <cstdint>
#include <cstring>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#endif

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::cuda {

namespace {

#if defined(FUSE_HAS_CUDA)
cudaExternalMemoryHandleType externalMemoryHandleType() {
#if defined(_WIN32)
    return cudaExternalMemoryHandleTypeOpaqueWin32;
#else
    return cudaExternalMemoryHandleTypeOpaqueFd;
#endif
}

/// Imports the exported allocation. Win32: CUDA never takes the NT handle. POSIX: CUDA takes ownership
/// of the fd it is given, so it gets a dup() and the Vulkan resource keeps its own fd (same contract as
/// Win32: the allocator closes the exported handle when the resource is destroyed).
cudaError_t importExternalMemory(void* exportedHandle, u64 allocationSize, cudaExternalMemory_t* out) {
    cudaExternalMemoryHandleDesc externalDesc{};
    externalDesc.type = externalMemoryHandleType();
    externalDesc.size = allocationSize;
#if defined(_WIN32)
    externalDesc.handle.win32.handle = exportedHandle;
    externalDesc.flags = cudaExternalMemoryDedicated;
    return cudaImportExternalMemory(out, &externalDesc);
#else
    const int fd = dup(static_cast<int>(reinterpret_cast<intptr_t>(exportedHandle)));
    if (fd < 0) {
        return cudaErrorInvalidValue;
    }
    externalDesc.handle.fd = fd;
    const cudaError_t err = cudaImportExternalMemory(out, &externalDesc);
    if (err != cudaSuccess) {
        close(fd); // ownership passes to CUDA only on success
    }
    return err;
#endif
}

cudaChannelFormatDesc channelFormatFor(u32 format) {
    switch (format) {
    case 97: // R16G16B16A16Sfloat
        return cudaCreateChannelDesc(16, 16, 16, 16, cudaChannelFormatKindFloat);
    case 83: // R16G16Sfloat
        return cudaCreateChannelDesc(16, 16, 0, 0, cudaChannelFormatKindFloat);
    case 100: // R32Sfloat
        return cudaCreateChannelDesc(32, 0, 0, 0, cudaChannelFormatKindFloat);
    default: // R8G8B8A8Unorm / Srgb
        return cudaCreateChannelDesc(8, 8, 8, 8, cudaChannelFormatKindUnsigned);
    }
}
#endif

} // namespace

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
        return "import descriptor missing device, memory, or exported handle";
    case InteropUnavailableReason::ExternalMemoryUnsupported:
        return "external memory import unavailable on this device";
    }
    return "unknown";
}

bool interopAvailable() {
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    return fuse::jobs::cudaJobsAvailable() && cudaExternalMemoryImportSupported();
#else
    return false;
#endif
}

InteropUnavailableReason interopUnavailableReason() {
#if !defined(FUSE_VULKAN_BACKEND)
    return InteropUnavailableReason::NoVulkanBackend;
#elif !defined(FUSE_HAS_CUDA)
    return InteropUnavailableReason::NoCudaToolkit;
#else
    // Runtime probes: these were previously (invalidly) written as #elif conditions.
    if (!fuse::jobs::cudaJobsAvailable()) {
        return InteropUnavailableReason::NoCudaToolkit;
    }
    if (!cudaExternalMemoryImportSupported()) {
        return InteropUnavailableReason::ExternalMemoryUnsupported;
    }
    return InteropUnavailableReason::None;
#endif
}

bool cudaExternalMemoryImportSupported() {
#if defined(FUSE_HAS_CUDA)
    if (!fuse::jobs::cudaJobsAvailable()) {
        return false;
    }

    // The CUDA runtime has no "external memory supported" device attribute; cudaImportExternalMemory
    // is the authoritative check. Mapping an imported allocation requires unified addressing, so
    // gate on that (always 1 on 64-bit Linux/Windows TCC/WDDM devices that support interop).
    int unifiedAddressing = 0;
    const cudaError_t err = cudaDeviceGetAttribute(&unifiedAddressing, cudaDevAttrUnifiedAddressing, 0);
    return err == cudaSuccess && unifiedAddressing != 0;
#else
    return false;
#endif
}

bool importDescHasExportedHandle(const VulkanBufferImportDesc& desc) {
    return desc.exportedHandle != nullptr && desc.allocationSize > 0;
}

bool importDescHasExportedHandle(const VulkanImageImportDesc& desc) {
    return desc.exportedHandle != nullptr && desc.allocationSize > 0;
}

VulkanBufferImportDesc makeBufferImportDesc(void* vkDevice, const Buffer& buffer) {
    VulkanBufferImportDesc desc{};
    desc.vkDevice = vkDevice;
    desc.vkMemory = buffer.allocation;
    desc.exportedHandle = buffer.exportedHandle;
    desc.allocationSize = buffer.allocationSize;
    desc.offset = 0;
    desc.size = buffer.desc.size;
    return desc;
}

VulkanImageImportDesc makeImageImportDesc(void* vkDevice, const Texture& texture) {
    VulkanImageImportDesc desc{};
    desc.vkDevice = vkDevice;
    desc.vkMemory = texture.allocation;
    desc.exportedHandle = texture.exportedHandle;
    desc.allocationSize = texture.allocationSize;
    desc.width = texture.desc.width;
    desc.height = texture.desc.height;
    desc.format = static_cast<u32>(texture.desc.format);
    desc.colorAttachment =
        (static_cast<u32>(texture.desc.usage) & static_cast<u32>(ImageUsage::ColorAttachment)) != 0u;
    return desc;
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
    if (!importDescHasExportedHandle(desc)) {
        result.reason =
            "Vulkan buffer import requires exported platform handle + allocation size (B2.6)";
        return result;
    }

#if defined(FUSE_HAS_CUDA)
    cudaExternalMemory_t externalMemory = nullptr;
    const cudaError_t importErr = importExternalMemory(desc.exportedHandle, desc.allocationSize, &externalMemory);
    if (importErr != cudaSuccess) {
        result.reason = cudaGetErrorString(importErr);
        return result;
    }

    cudaExternalMemoryBufferDesc bufferDesc{};
    bufferDesc.offset = desc.offset;
    bufferDesc.size = desc.size;

    void* devicePtr = nullptr;
    const cudaError_t mapErr =
        cudaExternalMemoryGetMappedBuffer(&devicePtr, externalMemory, &bufferDesc);
    if (mapErr != cudaSuccess) {
        cudaDestroyExternalMemory(externalMemory);
        result.reason = cudaGetErrorString(mapErr);
        return result;
    }

    result.devicePtr = devicePtr;
    result.externalMemory = externalMemory;
    result.ok = true;
    result.reason = "cudaImportExternalMemory succeeded";
    return result;
#else
    result.reason = interopUnavailableReasonString(InteropUnavailableReason::ExternalMemoryUnsupported);
    return result;
#endif
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
    if (!importDescHasExportedHandle(desc)) {
        result.reason =
            "Vulkan image import requires exported platform handle + allocation size (B2.6)";
        return result;
    }

#if defined(FUSE_HAS_CUDA)
    cudaExternalMemory_t externalMemory = nullptr;
    const cudaError_t importErr = importExternalMemory(desc.exportedHandle, desc.allocationSize, &externalMemory);
    if (importErr != cudaSuccess) {
        result.reason = cudaGetErrorString(importErr);
        return result;
    }

    // Surface writes need cudaArraySurfaceLoadStore; some drivers refuse it on imported images, in which
    // case the plain mapping is tried (surface objects over imported arrays work there).
    const unsigned int colorFlag = desc.colorAttachment ? cudaArrayColorAttachment : 0u;
    const unsigned int flagSets[2] = {cudaArraySurfaceLoadStore | colorFlag, colorFlag};
    cudaError_t lastErr = cudaSuccess;
    for (const unsigned int flags : flagSets) {
        cudaExternalMemoryMipmappedArrayDesc mipDesc{};
        mipDesc.offset = 0;
        mipDesc.formatDesc = channelFormatFor(desc.format);
        mipDesc.extent.width = desc.width;
        mipDesc.extent.height = desc.height;
        mipDesc.extent.depth = 0;
        mipDesc.flags = flags;
        mipDesc.numLevels = 1;

        cudaMipmappedArray_t mipmappedArray = nullptr;
        lastErr = cudaExternalMemoryGetMappedMipmappedArray(&mipmappedArray, externalMemory, &mipDesc);
        if (lastErr != cudaSuccess) {
            (void)cudaGetLastError();
            continue;
        }
        cudaArray_t cudaArray = nullptr;
        lastErr = cudaGetMipmappedArrayLevel(&cudaArray, mipmappedArray, 0);
        cudaSurfaceObject_t surfaceObject = 0;
        if (lastErr == cudaSuccess) {
            cudaResourceDesc resourceDesc{};
            resourceDesc.resType = cudaResourceTypeArray;
            resourceDesc.res.array.array = cudaArray;
            lastErr = cudaCreateSurfaceObject(&surfaceObject, &resourceDesc);
        }
        if (lastErr != cudaSuccess) {
            (void)cudaGetLastError();
            (void)cudaFreeMipmappedArray(mipmappedArray);
            continue;
        }
        result.surfaceObject = reinterpret_cast<void*>(static_cast<uintptr_t>(surfaceObject));
        result.externalMemory = externalMemory;
        result.mipmappedArray = mipmappedArray;
        result.array = cudaArray;
        result.arrayFlags = flags;
        result.ok = true;
        result.reason = "cudaImportExternalMemory image surface succeeded";
        return result;
    }
    (void)cudaDestroyExternalMemory(externalMemory);
    result.reason = cudaGetErrorString(lastErr);
    return result;
#else
    result.reason = interopUnavailableReasonString(InteropUnavailableReason::ExternalMemoryUnsupported);
    return result;
#endif
}

CudaBufferImport import_vulkan_buffer(void* vkDevice, const Buffer& buffer) {
    return import_vulkan_buffer(makeBufferImportDesc(vkDevice, buffer));
}

CudaSurfaceImport import_vulkan_image(void* vkDevice, const Texture& texture) {
    return import_vulkan_image(makeImageImportDesc(vkDevice, texture));
}

void release_imported_buffer(CudaBufferImport& imported) {
#if defined(FUSE_HAS_CUDA)
    if (imported.externalMemory != nullptr) {
        (void)cudaDestroyExternalMemory(static_cast<cudaExternalMemory_t>(imported.externalMemory));
    }
#else
    (void)imported;
#endif
    imported.externalMemory = nullptr;
    imported.devicePtr = nullptr;
    imported.ok = false;
}

void free_cuda_import(void* cudaDevicePtr) {
#if defined(FUSE_HAS_CUDA)
    if (cudaDevicePtr != nullptr) {
        (void)cudaFree(cudaDevicePtr);
    }
#else
    (void)cudaDevicePtr;
#endif
}

void free_cuda_surface(void* surfaceObject) {
#if defined(FUSE_HAS_CUDA)
    if (surfaceObject != nullptr) {
        const cudaSurfaceObject_t surface =
            static_cast<cudaSurfaceObject_t>(reinterpret_cast<uintptr_t>(surfaceObject));
        (void)cudaDestroySurfaceObject(surface);
    }
#else
    (void)surfaceObject;
#endif
}

void release_imported_surface(CudaSurfaceImport& imported) {
#if defined(FUSE_HAS_CUDA)
    if (imported.surfaceObject != nullptr) {
        (void)cudaDestroySurfaceObject(
            static_cast<cudaSurfaceObject_t>(reinterpret_cast<uintptr_t>(imported.surfaceObject)));
    }
    if (imported.mipmappedArray != nullptr) {
        (void)cudaFreeMipmappedArray(static_cast<cudaMipmappedArray_t>(imported.mipmappedArray));
    }
    if (imported.externalMemory != nullptr) {
        (void)cudaDestroyExternalMemory(static_cast<cudaExternalMemory_t>(imported.externalMemory));
    }
#endif
    imported.surfaceObject = nullptr;
    imported.mipmappedArray = nullptr;
    imported.array = nullptr;
    imported.externalMemory = nullptr;
    imported.ok = false;
}

int cuda_device_for_vulkan(void* vkPhysicalDevice) {
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (vkPhysicalDevice == nullptr) {
        return -1;
    }
    VkPhysicalDeviceIDProperties idProperties{};
    idProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &idProperties;
    vkGetPhysicalDeviceProperties2(static_cast<VkPhysicalDevice>(vkPhysicalDevice), &properties);

    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess) {
        (void)cudaGetLastError();
        return -1;
    }
    for (int device = 0; device < count; ++device) {
        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, device) != cudaSuccess) {
            continue;
        }
        static_assert(sizeof(prop.uuid.bytes) == VK_UUID_SIZE, "CUDA and Vulkan device UUIDs are both 16 bytes");
        if (std::memcmp(prop.uuid.bytes, idProperties.deviceUUID, VK_UUID_SIZE) == 0) {
            return device;
        }
    }
    return -1;
#else
    (void)vkPhysicalDevice;
    return -1;
#endif
}

} // namespace fuse::renderer::cuda
