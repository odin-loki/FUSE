#include <fuse/renderer/cuda/vk_sync.hpp>

#include <fuse/jobs/cuda_jobs.hpp>

#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
#include <cuda_runtime.h>
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::cuda {

namespace {

#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
bool physicalDeviceSupportsTimelineSemaphores(VkPhysicalDevice physicalDevice) {
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    return features12.timelineSemaphore != VK_FALSE;
}

cudaExternalSemaphoreHandleType externalSemaphoreHandleType() {
#if defined(_WIN32)
    return cudaExternalSemaphoreHandleTypeOpaqueWin32;
#else
    return cudaExternalSemaphoreHandleTypeOpaqueFd;
#endif
}
#endif

} // namespace

SharedTimeline SharedTimeline::create(void* vkDevice, void* vkPhysicalDevice) {
    SharedTimeline timeline{};
    timeline.message = "SharedTimeline stub — timeline semaphore pair deferred to B2.6";

#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (!fuse::jobs::cudaJobsAvailable() || vkDevice == nullptr || vkPhysicalDevice == nullptr) {
        return timeline;
    }

    auto device = static_cast<VkDevice>(vkDevice);
    auto physicalDevice = static_cast<VkPhysicalDevice>(vkPhysicalDevice);
    if (!physicalDeviceSupportsTimelineSemaphores(physicalDevice)) {
        timeline.message = "timeline semaphores unsupported on physical device";
        return timeline;
    }

    VkSemaphoreTypeCreateInfo timelineTypeInfo{};
    timelineTypeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineTypeInfo.initialValue = 0;

    VkExportSemaphoreCreateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
#if defined(_WIN32)
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    exportInfo.pNext = &timelineTypeInfo;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &exportInfo;

    VkSemaphore vkSemaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &vkSemaphore) != VK_SUCCESS) {
        timeline.message = "vkCreateSemaphore timeline export failed";
        return timeline;
    }

#if defined(_WIN32)
    using GetSemaphoreFn = PFN_vkGetSemaphoreWin32HandleKHR;
    const char* fnName = "vkGetSemaphoreWin32HandleKHR";
    VkExternalSemaphoreHandleTypeFlagBits handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    using GetSemaphoreFn = PFN_vkGetSemaphoreFdKHR;
    const char* fnName = "vkGetSemaphoreFdKHR";
    VkExternalSemaphoreHandleTypeFlagBits handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    auto getHandle = reinterpret_cast<GetSemaphoreFn>(vkGetDeviceProcAddr(device, fnName));
    if (getHandle == nullptr) {
        vkDestroySemaphore(device, vkSemaphore, nullptr);
        timeline.message = "Vulkan semaphore export proc missing";
        return timeline;
    }

    void* exportedHandle = nullptr;
#if defined(_WIN32)
    HANDLE winHandle = nullptr;
    VkSemaphoreGetWin32HandleInfoKHR handleInfo{};
    handleInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
    handleInfo.semaphore = vkSemaphore;
    handleInfo.handleType = handleType;
    if (getHandle(device, &handleInfo, &winHandle) != VK_SUCCESS || winHandle == nullptr) {
        vkDestroySemaphore(device, vkSemaphore, nullptr);
        timeline.message = "vkGetSemaphoreWin32HandleKHR failed";
        return timeline;
    }
    exportedHandle = winHandle;
#else
    int fd = -1;
    VkSemaphoreGetFdInfoKHR fdInfo{};
    fdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    fdInfo.semaphore = vkSemaphore;
    fdInfo.handleType = handleType;
    if (getHandle(device, &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
        vkDestroySemaphore(device, vkSemaphore, nullptr);
        timeline.message = "vkGetSemaphoreFdKHR failed";
        return timeline;
    }
    exportedHandle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
#endif

    cudaExternalSemaphoreHandleDesc cudaDesc{};
    cudaDesc.type = externalSemaphoreHandleType();
#if defined(_WIN32)
    cudaDesc.handle.win32.handle = exportedHandle;
    cudaDesc.flags = 0;
#else
    cudaDesc.handle.fd = static_cast<int>(reinterpret_cast<intptr_t>(exportedHandle));
#endif

    cudaExternalSemaphore_t cudaSemaphore = nullptr;
    const cudaError_t importErr = cudaImportExternalSemaphore(&cudaSemaphore, &cudaDesc);
    if (importErr != cudaSuccess) {
        vkDestroySemaphore(device, vkSemaphore, nullptr);
        timeline.message = cudaGetErrorString(importErr);
        return timeline;
    }

    timeline.vkSemaphore = vkSemaphore;
    timeline.cudaSemaphore = cudaSemaphore;
    timeline.valid = true;
    timeline.driverWired = true;
    timeline.message = "SharedTimeline driver-wired via export/import";
#endif

    return timeline;
}

void SharedTimeline::destroy(void* vkDevice) {
#if defined(FUSE_HAS_CUDA) && defined(FUSE_VULKAN_BACKEND)
    if (driverWired && cudaSemaphore != nullptr) {
        cudaDestroyExternalSemaphore(static_cast<cudaExternalSemaphore_t>(cudaSemaphore));
    }
    if (driverWired && vkDevice != nullptr && vkSemaphore != nullptr) {
        vkDestroySemaphore(static_cast<VkDevice>(vkDevice), static_cast<VkSemaphore>(vkSemaphore),
                           nullptr);
    }
#endif

    vkSemaphore = nullptr;
    cudaSemaphore = nullptr;
    value = 0;
    valid = false;
    driverWired = false;
    message = nullptr;
}

bool SharedTimeline::signalVulkan(void* vkDevice, u64 newValue) const {
    if (!valid || !driverWired || vkDevice == nullptr || vkSemaphore == nullptr) {
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    VkSemaphoreSignalInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signalInfo.semaphore = static_cast<VkSemaphore>(vkSemaphore);
    signalInfo.value = newValue;
    return vkSignalSemaphore(static_cast<VkDevice>(vkDevice), &signalInfo) == VK_SUCCESS;
#else
    (void)newValue;
    return false;
#endif
}

bool SharedTimeline::waitCuda(void* cudaStream, u64 waitValue) const {
    if (!valid || !driverWired || cudaSemaphore == nullptr) {
        return false;
    }

#if defined(FUSE_HAS_CUDA)
    cudaExternalSemaphoreWaitParams waitParams{};
    waitParams.params.fence.value = waitValue;
    const cudaStream_t stream = cudaStream != nullptr ? static_cast<cudaStream_t>(cudaStream) : 0;
    const cudaError_t err = cudaWaitExternalSemaphoresAsync(
        &static_cast<cudaExternalSemaphore_t>(cudaSemaphore), &waitParams, 1, stream);
    return err == cudaSuccess;
#else
    (void)cudaStream;
    (void)waitValue;
    return false;
#endif
}

FrameSyncPair FrameSyncPair::create(void* vkDevice, void* vkPhysicalDevice) {
    FrameSyncPair pair{};
    pair.vkToCuda = SharedTimeline::create(vkDevice, vkPhysicalDevice);
    pair.cudaToVk = SharedTimeline::create(vkDevice, vkPhysicalDevice);
    return pair;
}

void FrameSyncPair::destroy(void* vkDevice) {
    vkToCuda.destroy(vkDevice);
    cudaToVk.destroy(vkDevice);
}

} // namespace fuse::renderer::cuda
