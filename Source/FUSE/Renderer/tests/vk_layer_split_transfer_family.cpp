// Test-only Vulkan layer VK_LAYER_FUSE_split_transfer_family.
//
// Lavapipe exposes a single queue family (graphics | compute | transfer, one queue), so the RHI's
// cross-queue-family paths (ownership release/acquire barriers, semaphore hand-offs between a
// dedicated transfer queue and the graphics queue) never run on CI. This layer, loaded *below*
// VK_LAYER_KHRONOS_validation, makes the device look like discrete hardware to everything above it:
//
//   family 0: the ICD's family, unchanged (graphics | compute | transfer)
//   family 1: transfer-only, one queue
//   family 2: compute | transfer (no graphics), one queue — only when the environment variable
//             FUSE_SPLIT_LAYER_COMPUTE_FAMILY is set to a non-empty value other than "0" (the
//             "async compute" family discrete GPUs expose)
//
// The application and the validation layer see two families and two distinct VkQueue handles, so
// QFO release/acquire pairing, per-family stage/access limits and cross-queue synchronization are
// all validated for real. Downward, family 1 is folded onto family 0: its queue is a wrapper over
// the ICD's only queue, command pools / barriers / sharing lists get index 1 (and 2) rewritten to 0
// (an ownership transfer 1 -> 0 becomes an ordinary barrier, which is what the single family needs).
// Submissions from all "queues" reach the one real queue in the application's submission order
// (serialized by a per-device mutex, since the application may legally submit to its distinct
// queues from different threads), so semaphore waits are always on earlier signals and cannot
// deadlock.
//
// Only active when the ICD reports exactly one queue family; otherwise a pass-through.
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define FUSE_LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define FUSE_LAYER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

constexpr uint32_t kRealFamily = 0;
constexpr uint32_t kSplitFamily = 1;   ///< transfer-only
constexpr uint32_t kComputeFamily = 2; ///< compute | transfer, opt-in (FUSE_SPLIT_LAYER_COMPUTE_FAMILY)
constexpr uint32_t kMaxFamilies = 3;

bool computeFamilyEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("FUSE_SPLIT_LAYER_COMPUTE_FAMILY");
        return env != nullptr && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0');
    }();
    return enabled;
}

/// Families exposed upward when splitting: 0 (real) + transfer-only (+ compute-only).
uint32_t exposedFamilyCount() {
    return computeFamilyEnabled() ? 3u : 2u;
}

bool isSplitFamily(uint32_t family) {
    return family == kSplitFamily || (family == kComputeFamily && computeFamilyEnabled());
}

using DispatchKey = void*;

DispatchKey dispatchKey(const void* handle) {
    return *static_cast<void* const*>(handle);
}

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 queueFamilyProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 queueFamilyProperties2KHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surfaceSupport = nullptr;
};

/// Dispatchable wrapper handed out as the family-1 queue. First word = loader dispatch pointer.
struct SplitQueue {
    void* loaderData = nullptr;
    VkQueue real = VK_NULL_HANDLE;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    bool split = false;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkGetDeviceQueue2 getDeviceQueue2 = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
    PFN_vkQueueSubmit2 queueSubmit2KHR = nullptr;
    PFN_vkQueueWaitIdle queueWaitIdle = nullptr;
    PFN_vkQueueBindSparse queueBindSparse = nullptr;
    PFN_vkQueuePresentKHR queuePresent = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdPipelineBarrier2 cmdPipelineBarrier2 = nullptr;
    PFN_vkCmdPipelineBarrier2 cmdPipelineBarrier2KHR = nullptr;
    PFN_vkCmdWaitEvents cmdWaitEvents = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    /// Wrapper queue per split family (index = exposed family; [0] unused).
    std::unique_ptr<SplitQueue> splitQueues[kMaxFamilies];
    /// Serializes access to the one real queue shared by all exposed queues.
    std::mutex submitMutex;
};

std::mutex g_mutex;
std::unordered_map<DispatchKey, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<DispatchKey, std::unique_ptr<DeviceData>> g_devices;

InstanceData* instanceData(const void* handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(dispatchKey(handle));
    if (it != g_instances.end()) {
        return it->second.get();
    }
    // Physical devices normally share the instance key; fall back to the only instance.
    return g_instances.size() == 1 ? g_instances.begin()->second.get() : nullptr;
}

DeviceData* deviceData(const void* handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(dispatchKey(handle));
    return it != g_devices.end() ? it->second.get() : nullptr;
}

uint32_t realFamilyCount(InstanceData& data, VkPhysicalDevice physicalDevice) {
    uint32_t count = 0;
    data.queueFamilyProperties(physicalDevice, &count, nullptr);
    return count;
}

bool shouldSplit(InstanceData& data, VkPhysicalDevice physicalDevice) {
    if (realFamilyCount(data, physicalDevice) != 1) {
        return false;
    }
    VkQueueFamilyProperties props{};
    uint32_t count = 1;
    data.queueFamilyProperties(physicalDevice, &count, &props);
    return (props.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0 || (props.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
}

VkQueueFamilyProperties transferOnly(const VkQueueFamilyProperties& real) {
    VkQueueFamilyProperties props = real;
    props.queueFlags = VK_QUEUE_TRANSFER_BIT;
    props.queueCount = 1;
    props.minImageTransferGranularity = {1, 1, 1};
    return props;
}

VkQueueFamilyProperties computeOnly(const VkQueueFamilyProperties& real) {
    VkQueueFamilyProperties props = real;
    props.queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    props.queueCount = 1;
    props.minImageTransferGranularity = {1, 1, 1};
    return props;
}

/// Exposed family `index` derived from the real family 0 properties.
VkQueueFamilyProperties exposedFamily(const VkQueueFamilyProperties& real, uint32_t index) {
    if (index == kSplitFamily) {
        return transferOnly(real);
    }
    if (index == kComputeFamily) {
        return computeOnly(real);
    }
    return real;
}

uint32_t mapFamily(uint32_t family) {
    return isSplitFamily(family) ? kRealFamily : family;
}

VkQueue realQueue(DeviceData* data, VkQueue queue) {
    if (data == nullptr) {
        return queue;
    }
    for (const std::unique_ptr<SplitQueue>& split : data->splitQueues) {
        if (split && queue == reinterpret_cast<VkQueue>(split.get())) {
            return split->real;
        }
    }
    return queue;
}

// ---------------------------------------------------------------------------------------------
// Instance level

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    auto* chain = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext));
    while (chain != nullptr &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) {
        chain = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(chain->pNext));
    }
    if (chain == nullptr || chain->u.pLayerInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (createInstance == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = createInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        return result;
    }
    auto data = std::make_unique<InstanceData>();
    data->instance = *pInstance;
    data->gipa = gipa;
    data->destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*pInstance, "vkDestroyInstance"));
    data->queueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    data->queueFamilyProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
        gipa(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties2"));
    data->queueFamilyProperties2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>(
        gipa(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties2KHR"));
    data->surfaceSupport = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        gipa(*pInstance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances[dispatchKey(*pInstance)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    if (instance == VK_NULL_HANDLE) {
        return;
    }
    InstanceData* data = instanceData(instance);
    if (data == nullptr) {
        return;
    }
    const PFN_vkDestroyInstance destroy = data->destroyInstance;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances.erase(dispatchKey(instance));
    }
    destroy(instance, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pCount,
                                                                  VkQueueFamilyProperties* pProperties) {
    InstanceData* data = instanceData(physicalDevice);
    if (!shouldSplit(*data, physicalDevice)) {
        data->queueFamilyProperties(physicalDevice, pCount, pProperties);
        return;
    }
    if (pProperties == nullptr) {
        *pCount = exposedFamilyCount();
        return;
    }
    VkQueueFamilyProperties real{};
    uint32_t one = 1;
    data->queueFamilyProperties(physicalDevice, &one, &real);
    uint32_t written = 0;
    for (uint32_t i = 0; i < *pCount && i < exposedFamilyCount(); ++i) {
        pProperties[i] = exposedFamily(real, i);
        written = i + 1;
    }
    *pCount = written;
}

void queueFamilyProperties2(InstanceData* data, PFN_vkGetPhysicalDeviceQueueFamilyProperties2 next,
                            VkPhysicalDevice physicalDevice, uint32_t* pCount, VkQueueFamilyProperties2* pProperties) {
    if (!shouldSplit(*data, physicalDevice)) {
        next(physicalDevice, pCount, pProperties);
        return;
    }
    if (pProperties == nullptr) {
        *pCount = exposedFamilyCount();
        return;
    }
    uint32_t written = 0;
    for (uint32_t i = 0; i < *pCount && i < exposedFamilyCount(); ++i) {
        // Fill each element (and its pNext chain) from the real family, then narrow the split ones.
        uint32_t one = 1;
        next(physicalDevice, &one, &pProperties[i]);
        pProperties[i].queueFamilyProperties = exposedFamily(pProperties[i].queueFamilyProperties, i);
        written = i + 1;
    }
    *pCount = written;
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t* pCount,
                                                                   VkQueueFamilyProperties2* pProperties) {
    InstanceData* data = instanceData(physicalDevice);
    queueFamilyProperties2(data, data->queueFamilyProperties2, physicalDevice, pCount, pProperties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice physicalDevice, uint32_t* pCount,
                                                                      VkQueueFamilyProperties2* pProperties) {
    InstanceData* data = instanceData(physicalDevice);
    queueFamilyProperties2(data, data->queueFamilyProperties2KHR != nullptr ? data->queueFamilyProperties2KHR
                                                                            : data->queueFamilyProperties2,
                           physicalDevice, pCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
                                                                  uint32_t queueFamilyIndex, VkSurfaceKHR surface,
                                                                  VkBool32* pSupported) {
    InstanceData* data = instanceData(physicalDevice);
    if (isSplitFamily(queueFamilyIndex) && shouldSplit(*data, physicalDevice)) {
        *pSupported = VK_FALSE; // split (transfer / compute) families never present
        return VK_SUCCESS;
    }
    return data->surfaceSupport(physicalDevice, queueFamilyIndex, surface, pSupported);
}

// ---------------------------------------------------------------------------------------------
// Device level

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    InstanceData* instance = instanceData(physicalDevice);
    auto* chain = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    VkLayerDeviceCreateInfo* link = nullptr;
    for (; chain != nullptr;
         chain = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(chain->pNext))) {
        if (chain->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            continue;
        }
        if (chain->function == VK_LAYER_LINK_INFO && link == nullptr) {
            link = chain;
        } else if (chain->function == VK_LOADER_DATA_CALLBACK) {
            setLoaderData = chain->u.pfnSetDeviceLoaderData;
        }
    }
    if (instance == nullptr || link == nullptr || link->u.pLayerInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance->instance, "vkCreateDevice"));
    if (createDevice == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    const bool split = shouldSplit(*instance, physicalDevice);
    std::vector<VkDeviceQueueCreateInfo> queues;
    const float priority = 1.f;
    bool wantsSplitQueue = false;
    bool hasRealQueue = false;
    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
        const VkDeviceQueueCreateInfo& info = pCreateInfo->pQueueCreateInfos[i];
        if (split && isSplitFamily(info.queueFamilyIndex)) {
            wantsSplitQueue = true;
            continue;
        }
        hasRealQueue = hasRealQueue || info.queueFamilyIndex == kRealFamily;
        queues.push_back(info);
    }
    if (wantsSplitQueue && !hasRealQueue) {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = kRealFamily;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queues.push_back(info);
    }
    VkDeviceCreateInfo createInfo = *pCreateInfo;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
    createInfo.pQueueCreateInfos = queues.data();
    const VkResult result = createDevice(physicalDevice, &createInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }

    auto data = std::make_unique<DeviceData>();
    data->device = *pDevice;
    data->split = split;
    data->gdpa = gdpa;
    data->setLoaderData = setLoaderData;
#define FUSE_LOAD(member, type, name) data->member = reinterpret_cast<type>(gdpa(*pDevice, name))
    FUSE_LOAD(destroyDevice, PFN_vkDestroyDevice, "vkDestroyDevice");
    FUSE_LOAD(getDeviceQueue, PFN_vkGetDeviceQueue, "vkGetDeviceQueue");
    FUSE_LOAD(getDeviceQueue2, PFN_vkGetDeviceQueue2, "vkGetDeviceQueue2");
    FUSE_LOAD(queueSubmit, PFN_vkQueueSubmit, "vkQueueSubmit");
    FUSE_LOAD(queueSubmit2, PFN_vkQueueSubmit2, "vkQueueSubmit2");
    FUSE_LOAD(queueSubmit2KHR, PFN_vkQueueSubmit2, "vkQueueSubmit2KHR");
    FUSE_LOAD(queueWaitIdle, PFN_vkQueueWaitIdle, "vkQueueWaitIdle");
    FUSE_LOAD(queueBindSparse, PFN_vkQueueBindSparse, "vkQueueBindSparse");
    FUSE_LOAD(queuePresent, PFN_vkQueuePresentKHR, "vkQueuePresentKHR");
    FUSE_LOAD(createCommandPool, PFN_vkCreateCommandPool, "vkCreateCommandPool");
    FUSE_LOAD(cmdPipelineBarrier, PFN_vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
    FUSE_LOAD(cmdPipelineBarrier2, PFN_vkCmdPipelineBarrier2, "vkCmdPipelineBarrier2");
    FUSE_LOAD(cmdPipelineBarrier2KHR, PFN_vkCmdPipelineBarrier2, "vkCmdPipelineBarrier2KHR");
    FUSE_LOAD(cmdWaitEvents, PFN_vkCmdWaitEvents, "vkCmdWaitEvents");
    FUSE_LOAD(createBuffer, PFN_vkCreateBuffer, "vkCreateBuffer");
    FUSE_LOAD(createImage, PFN_vkCreateImage, "vkCreateImage");
#undef FUSE_LOAD
    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices[dispatchKey(*pDevice)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    std::unique_ptr<DeviceData> data;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_devices.find(dispatchKey(device));
        if (it == g_devices.end()) {
            return;
        }
        data = std::move(it->second);
        g_devices.erase(it);
    }
    data->destroyDevice(device, pAllocator);
}

VkQueue splitQueueFor(DeviceData* data, VkDevice device, uint32_t family) {
    std::unique_ptr<SplitQueue>& slot = data->splitQueues[family];
    if (!slot) {
        VkQueue real = VK_NULL_HANDLE;
        data->getDeviceQueue(device, kRealFamily, 0, &real);
        if (real == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }
        auto wrapper = std::make_unique<SplitQueue>();
        wrapper->loaderData = dispatchKey(real);
        wrapper->real = real;
        if (data->setLoaderData != nullptr) {
            data->setLoaderData(device, wrapper.get());
        }
        slot = std::move(wrapper);
    }
    return reinterpret_cast<VkQueue>(slot.get());
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* pQueue) {
    DeviceData* data = deviceData(device);
    if (data->split && isSplitFamily(family)) {
        *pQueue = index == 0 ? splitQueueFor(data, device, family) : VK_NULL_HANDLE;
        return;
    }
    data->getDeviceQueue(device, family, index, pQueue);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pInfo, VkQueue* pQueue) {
    DeviceData* data = deviceData(device);
    if (data->split && isSplitFamily(pInfo->queueFamilyIndex)) {
        *pQueue = pInfo->queueIndex == 0 ? splitQueueFor(data, device, pInfo->queueFamilyIndex) : VK_NULL_HANDLE;
        return;
    }
    data->getDeviceQueue2(device, pInfo, pQueue);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* pSubmits, VkFence fence) {
    DeviceData* data = deviceData(queue);
    std::lock_guard<std::mutex> lock(data->submitMutex);
    return data->queueSubmit(realQueue(data, queue), count, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* pSubmits, VkFence fence) {
    DeviceData* data = deviceData(queue);
    std::lock_guard<std::mutex> lock(data->submitMutex);
    return data->queueSubmit2(realQueue(data, queue), count, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* pSubmits,
                                               VkFence fence) {
    DeviceData* data = deviceData(queue);
    const PFN_vkQueueSubmit2 next = data->queueSubmit2KHR != nullptr ? data->queueSubmit2KHR : data->queueSubmit2;
    std::lock_guard<std::mutex> lock(data->submitMutex);
    return next(realQueue(data, queue), count, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue) {
    DeviceData* data = deviceData(queue);
    std::lock_guard<std::mutex> lock(data->submitMutex);
    return data->queueWaitIdle(realQueue(data, queue));
}

VKAPI_ATTR VkResult VKAPI_CALL QueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* pInfo,
                                               VkFence fence) {
    DeviceData* data = deviceData(queue);
    std::lock_guard<std::mutex> lock(data->submitMutex);
    return data->queueBindSparse(realQueue(data, queue), count, pInfo, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pInfo) {
    DeviceData* data = deviceData(queue);
    std::lock_guard<std::mutex> lock(data->submitMutex);
    return data->queuePresent(realQueue(data, queue), pInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* pInfo,
                                                 const VkAllocationCallbacks* pAllocator, VkCommandPool* pPool) {
    DeviceData* data = deviceData(device);
    VkCommandPoolCreateInfo info = *pInfo;
    if (data->split) {
        info.queueFamilyIndex = mapFamily(info.queueFamilyIndex);
    }
    return data->createCommandPool(device, &info, pAllocator, pPool);
}

template <typename Barrier>
std::vector<Barrier> remapBarriers(const Barrier* barriers, uint32_t count) {
    std::vector<Barrier> out(barriers, barriers + count);
    for (Barrier& barrier : out) {
        barrier.srcQueueFamilyIndex = mapFamily(barrier.srcQueueFamilyIndex);
        barrier.dstQueueFamilyIndex = mapFamily(barrier.dstQueueFamilyIndex);
    }
    return out;
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStage,
                                              VkPipelineStageFlags dstStage, VkDependencyFlags flags,
                                              uint32_t memoryCount, const VkMemoryBarrier* pMemory,
                                              uint32_t bufferCount, const VkBufferMemoryBarrier* pBuffers,
                                              uint32_t imageCount, const VkImageMemoryBarrier* pImages) {
    DeviceData* data = deviceData(cmd);
    if (!data->split) {
        data->cmdPipelineBarrier(cmd, srcStage, dstStage, flags, memoryCount, pMemory, bufferCount, pBuffers,
                                 imageCount, pImages);
        return;
    }
    const auto buffers = remapBarriers(pBuffers, bufferCount);
    const auto images = remapBarriers(pImages, imageCount);
    data->cmdPipelineBarrier(cmd, srcStage, dstStage, flags, memoryCount, pMemory, bufferCount, buffers.data(),
                             imageCount, images.data());
}

VKAPI_ATTR void VKAPI_CALL CmdWaitEvents(VkCommandBuffer cmd, uint32_t eventCount, const VkEvent* pEvents,
                                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                         uint32_t memoryCount, const VkMemoryBarrier* pMemory, uint32_t bufferCount,
                                         const VkBufferMemoryBarrier* pBuffers, uint32_t imageCount,
                                         const VkImageMemoryBarrier* pImages) {
    DeviceData* data = deviceData(cmd);
    const auto buffers = remapBarriers(pBuffers, bufferCount);
    const auto images = remapBarriers(pImages, imageCount);
    data->cmdWaitEvents(cmd, eventCount, pEvents, srcStage, dstStage, memoryCount, pMemory, bufferCount,
                        buffers.data(), imageCount, images.data());
}

void pipelineBarrier2(PFN_vkCmdPipelineBarrier2 next, VkCommandBuffer cmd, const VkDependencyInfo* pInfo) {
    VkDependencyInfo info = *pInfo;
    const auto buffers = remapBarriers(pInfo->pBufferMemoryBarriers, pInfo->bufferMemoryBarrierCount);
    const auto images = remapBarriers(pInfo->pImageMemoryBarriers, pInfo->imageMemoryBarrierCount);
    info.pBufferMemoryBarriers = buffers.data();
    info.pImageMemoryBarriers = images.data();
    next(cmd, &info);
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* pInfo) {
    DeviceData* data = deviceData(cmd);
    pipelineBarrier2(data->cmdPipelineBarrier2, cmd, pInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier2KHR(VkCommandBuffer cmd, const VkDependencyInfo* pInfo) {
    DeviceData* data = deviceData(cmd);
    pipelineBarrier2(data->cmdPipelineBarrier2KHR != nullptr ? data->cmdPipelineBarrier2KHR : data->cmdPipelineBarrier2,
                     cmd, pInfo);
}

/// CONCURRENT sharing lists: fold family 1 onto 0; a single remaining family becomes EXCLUSIVE.
template <typename Info>
void remapSharing(Info& info, std::vector<uint32_t>& families) {
    if (info.sharingMode != VK_SHARING_MODE_CONCURRENT || info.pQueueFamilyIndices == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < info.queueFamilyIndexCount; ++i) {
        const uint32_t family = mapFamily(info.pQueueFamilyIndices[i]);
        bool seen = false;
        for (uint32_t existing : families) {
            seen = seen || existing == family;
        }
        if (!seen) {
            families.push_back(family);
        }
    }
    if (families.size() < 2) {
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.queueFamilyIndexCount = 0;
        info.pQueueFamilyIndices = nullptr;
    } else {
        info.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
        info.pQueueFamilyIndices = families.data();
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* pInfo,
                                            const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
    DeviceData* data = deviceData(device);
    VkBufferCreateInfo info = *pInfo;
    std::vector<uint32_t> families;
    if (data->split) {
        remapSharing(info, families);
    }
    return data->createBuffer(device, &info, pAllocator, pBuffer);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* pInfo,
                                           const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    DeviceData* data = deviceData(device);
    VkImageCreateInfo info = *pInfo;
    std::vector<uint32_t> families;
    if (data->split) {
        remapSharing(info, families);
    }
    return data->createImage(device, &info, pAllocator, pImage);
}

// ---------------------------------------------------------------------------------------------
// Proc address lookup

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* pName);

PFN_vkVoidFunction interceptDevice(const char* name) {
    struct Entry {
        const char* name;
        PFN_vkVoidFunction fn;
    };
    static const Entry kEntries[] = {
        {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(&DestroyDevice)},
        {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceQueue)},
        {"vkGetDeviceQueue2", reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceQueue2)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(&QueueSubmit)},
        {"vkQueueSubmit2", reinterpret_cast<PFN_vkVoidFunction>(&QueueSubmit2)},
        {"vkQueueSubmit2KHR", reinterpret_cast<PFN_vkVoidFunction>(&QueueSubmit2KHR)},
        {"vkQueueWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(&QueueWaitIdle)},
        {"vkQueueBindSparse", reinterpret_cast<PFN_vkVoidFunction>(&QueueBindSparse)},
        {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(&QueuePresentKHR)},
        {"vkCreateCommandPool", reinterpret_cast<PFN_vkVoidFunction>(&CreateCommandPool)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(&CmdPipelineBarrier)},
        {"vkCmdPipelineBarrier2", reinterpret_cast<PFN_vkVoidFunction>(&CmdPipelineBarrier2)},
        {"vkCmdPipelineBarrier2KHR", reinterpret_cast<PFN_vkVoidFunction>(&CmdPipelineBarrier2KHR)},
        {"vkCmdWaitEvents", reinterpret_cast<PFN_vkVoidFunction>(&CmdWaitEvents)},
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(&CreateBuffer)},
        {"vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(&CreateImage)},
    };
    for (const Entry& entry : kEntries) {
        if (std::strcmp(entry.name, name) == 0) {
            return entry.fn;
        }
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* pName) {
    DeviceData* data = deviceData(device);
    if (data == nullptr) {
        return nullptr;
    }
    const PFN_vkVoidFunction next = data->gdpa(device, pName);
    if (next == nullptr) {
        return nullptr; // not enabled below: do not advertise an intercept
    }
    const PFN_vkVoidFunction own = interceptDevice(pName);
    return own != nullptr ? own : next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* pName) {
    struct Entry {
        const char* name;
        PFN_vkVoidFunction fn;
    };
    static const Entry kEntries[] = {
        {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(&GetInstanceProcAddr)},
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance)},
        {"vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(&DestroyInstance)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice)},
        {"vkGetPhysicalDeviceQueueFamilyProperties",
         reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceQueueFamilyProperties)},
        {"vkGetPhysicalDeviceQueueFamilyProperties2",
         reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceQueueFamilyProperties2)},
        {"vkGetPhysicalDeviceQueueFamilyProperties2KHR",
         reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceQueueFamilyProperties2KHR)},
        {"vkGetPhysicalDeviceSurfaceSupportKHR",
         reinterpret_cast<PFN_vkVoidFunction>(&GetPhysicalDeviceSurfaceSupportKHR)},
    };
    for (const Entry& entry : kEntries) {
        if (std::strcmp(entry.name, pName) == 0) {
            return entry.fn;
        }
    }
    if (instance == VK_NULL_HANDLE) {
        return nullptr;
    }
    InstanceData* data = instanceData(instance);
    if (data == nullptr) {
        return nullptr;
    }
    const PFN_vkVoidFunction next = data->gipa(instance, pName);
    if (next == nullptr) {
        return nullptr;
    }
    const PFN_vkVoidFunction own = interceptDevice(pName);
    return own != nullptr ? own : next;
}

} // namespace

FUSE_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return GetInstanceProcAddr(instance, pName);
}

FUSE_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return GetDeviceProcAddr(device, pName);
}

FUSE_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (pVersionStruct == nullptr || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
        pVersionStruct->pfnGetInstanceProcAddr = &GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = &GetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    return VK_SUCCESS;
}
