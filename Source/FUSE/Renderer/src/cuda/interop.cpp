#include <fuse/renderer/cuda/interop.hpp>

#include <fuse/jobs/cuda_jobs.hpp>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
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
    cudaExternalMemoryHandleDesc externalDesc{};
    externalDesc.type = externalMemoryHandleType();
    externalDesc.size = desc.allocationSize;
#if defined(_WIN32)
    externalDesc.handle.win32.handle = desc.exportedHandle;
    externalDesc.flags = cudaExternalMemoryDedicated;
#else
    externalDesc.handle.fd = static_cast<int>(reinterpret_cast<intptr_t>(desc.exportedHandle));
#endif

    cudaExternalMemory_t externalMemory = nullptr;
    const cudaError_t importErr = cudaImportExternalMemory(&externalMemory, &externalDesc);
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
    cudaExternalMemoryHandleDesc externalDesc{};
    externalDesc.type = externalMemoryHandleType();
    externalDesc.size = desc.allocationSize;
#if defined(_WIN32)
    externalDesc.handle.win32.handle = desc.exportedHandle;
    externalDesc.flags = cudaExternalMemoryDedicated;
#else
    externalDesc.handle.fd = static_cast<int>(reinterpret_cast<intptr_t>(desc.exportedHandle));
#endif

    cudaExternalMemory_t externalMemory = nullptr;
    const cudaError_t importErr = cudaImportExternalMemory(&externalMemory, &externalDesc);
    if (importErr != cudaSuccess) {
        result.reason = cudaGetErrorString(importErr);
        return result;
    }

    cudaExternalMemoryMipmappedArrayDesc mipDesc{};
    mipDesc.offset = 0;
    mipDesc.formatDesc = cudaCreateChannelDesc(8, 8, 8, 8, cudaChannelFormatKindUnsigned);
    mipDesc.extent.width = desc.width;
    mipDesc.extent.height = desc.height;
    mipDesc.extent.depth = 0;
    mipDesc.flags = cudaArrayColorAttachment;
    mipDesc.numLevels = 1;

    cudaMipmappedArray_t mipmappedArray = nullptr;
    const cudaError_t arrayErr =
        cudaExternalMemoryGetMappedMipmappedArray(&mipmappedArray, externalMemory, &mipDesc);
    if (arrayErr != cudaSuccess) {
        cudaDestroyExternalMemory(externalMemory);
        result.reason = cudaGetErrorString(arrayErr);
        return result;
    }

    cudaArray_t cudaArray = nullptr;
    const cudaError_t levelErr = cudaGetMipmappedArrayLevel(&cudaArray, mipmappedArray, 0);
    if (levelErr != cudaSuccess) {
        cudaDestroyExternalMemory(externalMemory);
        result.reason = cudaGetErrorString(levelErr);
        return result;
    }

    cudaResourceDesc resourceDesc{};
    resourceDesc.resType = cudaResourceTypeArray;
    resourceDesc.res.array.array = cudaArray;

    cudaSurfaceObject_t surfaceObject = 0;
    const cudaError_t surfaceErr = cudaCreateSurfaceObject(&surfaceObject, &resourceDesc);
    if (surfaceErr != cudaSuccess) {
        cudaDestroyExternalMemory(externalMemory);
        result.reason = cudaGetErrorString(surfaceErr);
        return result;
    }

    result.surfaceObject = reinterpret_cast<void*>(static_cast<uintptr_t>(surfaceObject));
    result.ok = true;
    result.reason = "cudaImportExternalMemory image surface succeeded";
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

} // namespace fuse::renderer::cuda
