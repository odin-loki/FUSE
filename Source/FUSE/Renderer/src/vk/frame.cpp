#include <fuse/renderer/vk/frame.hpp>

#include <fuse/renderer/vk/debug_utils.hpp>

#include <array>
#include <cstdio>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

constexpr u32 kFrameScratchBytes = 8u * 1024u * 1024u;
constexpr u32 kFrameDescriptorCount = 1024u;

#if defined(FUSE_VULKAN_BACKEND)
/// Clear a slot fence that was marked in-flight without a matching queue submit (headless stub).
bool clearStubInFlightFence(VkDevice device, FrameSyncData& slot) {
    if (slot.inFlightFence == nullptr) {
        return true;
    }

    auto fence = static_cast<VkFence>(slot.inFlightFence);
    const VkResult status = vkGetFenceStatus(device, fence);
    if (status == VK_SUCCESS) {
        return vkResetFences(device, 1, &fence) == VK_SUCCESS;
    }
    if (status == VK_NOT_READY) {
        return true;
    }
    return false;
}

bool createSlotTimestampQueryPool(VkDevice device, VkQueryPool* outPool) {
    VkQueryPoolCreateInfo queryInfo{};
    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2;

    VkQueryPool pool = VK_NULL_HANDLE;
    if (vkCreateQueryPool(device, &queryInfo, nullptr, &pool) != VK_SUCCESS) {
        return false;
    }
    *outPool = pool;
    return true;
}

bool createSlotDescriptorPool(VkDevice device, VkDescriptorPool* outPool) {
    const std::array<VkDescriptorPoolSize, 5> poolSizes = {{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFrameDescriptorCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFrameDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFrameDescriptorCount},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFrameDescriptorCount},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFrameDescriptorCount},
    }};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFrameDescriptorCount;
    poolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        return false;
    }
    *outPool = pool;
    return true;
}

u64 vulkanObjectHandle(void* handle) {
    return static_cast<u64>(reinterpret_cast<uintptr_t>(handle));
}

void tryNameFrameObject(void* vkDevice, VkObjectType type, void* handle, u32 slot, const char* suffix,
                        u32& namesSet) {
    if (handle == nullptr || suffix == nullptr) {
        return;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "fuse.frame.%u.%s", slot, suffix);
    if (setDebugObjectName(vkDevice, static_cast<u32>(type), vulkanObjectHandle(handle), name)) {
        ++namesSet;
    }
}

void nameSlotSyncObjects(void* vkDevice, FrameSyncData& slot, u32 index, u32& namesSet) {
    tryNameFrameObject(vkDevice, VK_OBJECT_TYPE_FENCE, slot.inFlightFence, index, "fence", namesSet);
    tryNameFrameObject(vkDevice, VK_OBJECT_TYPE_SEMAPHORE, slot.imageAvailable, index, "imageAvailable",
                       namesSet);
    tryNameFrameObject(vkDevice, VK_OBJECT_TYPE_SEMAPHORE, slot.renderFinished, index, "renderFinished",
                       namesSet);
    tryNameFrameObject(vkDevice, VK_OBJECT_TYPE_SEMAPHORE, slot.timelineSemaphore, index, "timeline",
                       namesSet);
    tryNameFrameObject(vkDevice, VK_OBJECT_TYPE_COMMAND_POOL, slot.commands.commandPool, index,
                       "commandPool", namesSet);
    tryNameFrameObject(vkDevice, VK_OBJECT_TYPE_COMMAND_BUFFER, slot.commands.primaryCommandBuffer, index,
                       "cmd", namesSet);
    tryNameFrameObject(vkDevice, VK_OBJECT_TYPE_COMMAND_BUFFER, slot.commands.transferCommandBuffer,
                       index, "transferCmd", namesSet);
    tryNameFrameObject(vkDevice, VK_OBJECT_TYPE_COMMAND_BUFFER, slot.commands.computeCommandBuffer, index,
                       "computeCmd", namesSet);
    tryNameFrameObject(vkDevice, VK_OBJECT_TYPE_QUERY_POOL, slot.timestampQueryPool, index,
                       "timestampPool", namesSet);
}

#if defined(VK_VERSION_1_2) || defined(VK_KHR_timeline_semaphore)
// VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO is an enum, not a #define — gate on 1.2 / KHR.
bool createSlotTimelineSemaphore(VkDevice device, VkSemaphore* outSemaphore) {
    VkSemaphoreTypeCreateInfo timelineTypeInfo{};
    timelineTypeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineTypeInfo.initialValue = 0;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &timelineTypeInfo;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
        return false;
    }
    *outSemaphore = semaphore;
    return true;
}
#endif
#endif

} // namespace

std::unique_ptr<FrameManager> FrameManager::create(VulkanDevice& device) {
    auto manager = std::unique_ptr<FrameManager>(new FrameManager());
    if (!manager->initialize(device)) {
        manager->m_info.ready = false;
    }
    return manager;
}

FrameManager::~FrameManager() {
    shutdown();
}

void FrameManager::constructScratchAllocators() {
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (!m_scratch[i]) {
            m_scratch[i] =
                std::make_unique<fuse::alloc::FrameAllocator>(kFrameScratchBytes, "frame-slot");
        }
    }
}

bool FrameManager::initialize(VulkanDevice& device) {
    constructScratchAllocators();
    m_info.framesInFlight = kFramesInFlight;

#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "FrameManager CPU scratch ready — device not valid";
        m_info.timestampsReady = false;
        return false;
    }

    m_device = device.nativeHandle();
    auto vkDevice = static_cast<VkDevice>(m_device);

    const u32 graphicsFamily = device.queues().graphicsFamily;

    m_timestampPeriod = 0.f;
    m_info.timestampsReady = false;
    if (device.nativePhysicalDevice() != nullptr) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(static_cast<VkPhysicalDevice>(device.nativePhysicalDevice()), &props);
        m_timestampPeriod = props.limits.timestampPeriod;
    }

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkSemaphore renderFinished = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;

        if (vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &imageAvailable) != VK_SUCCESS ||
            vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &renderFinished) != VK_SUCCESS ||
            vkCreateFence(vkDevice, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            m_info.message = "FrameManager sync object creation failed";
            shutdown();
            return false;
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphicsFamily;

        VkCommandPool commandPool = VK_NULL_HANDLE;
        if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            m_info.message = "FrameManager command pool creation failed";
            shutdown();
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 3;

        VkCommandBuffer commandBuffers[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (vkAllocateCommandBuffers(vkDevice, &allocInfo, commandBuffers) != VK_SUCCESS) {
            vkDestroyCommandPool(vkDevice, commandPool, nullptr);
            m_info.message = "FrameManager command buffer allocation failed";
            shutdown();
            return false;
        }

        m_slots[i].imageAvailable = imageAvailable;
        m_slots[i].renderFinished = renderFinished;
        m_slots[i].inFlightFence = fence;
        m_slots[i].fenceSignaled = true;
        m_slots[i].commands.commandPool = commandPool;
        m_slots[i].commands.primaryCommandBuffer = commandBuffers[0];
        m_slots[i].commands.transferCommandBuffer = commandBuffers[1];
        m_slots[i].commands.computeCommandBuffer = commandBuffers[2];

#if defined(VK_VERSION_1_2) || defined(VK_KHR_timeline_semaphore)
        VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
        if (!createSlotTimelineSemaphore(vkDevice, &timelineSemaphore)) {
            m_info.message = "FrameManager timeline semaphore creation failed";
            shutdown();
            return false;
        }
        m_slots[i].timelineSemaphore = timelineSemaphore;
        m_slots[i].timelineValue = 0;
#endif

        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        if (!createSlotDescriptorPool(vkDevice, &descriptorPool)) {
            m_info.message = "FrameManager descriptor pool creation failed";
            shutdown();
            return false;
        }
        m_slots[i].commands.descriptorPool = descriptorPool;

        if (m_timestampPeriod > 0.f) {
            VkQueryPool timestampPool = VK_NULL_HANDLE;
            if (createSlotTimestampQueryPool(vkDevice, &timestampPool)) {
                m_slots[i].timestampQueryPool = timestampPool;
            }
        }

        nameSlotSyncObjects(vkDevice, m_slots[i], i, m_debugNamesSet);
    }

    bool timestampsReady = m_timestampPeriod > 0.f;
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (m_slots[i].timestampQueryPool == nullptr) {
            timestampsReady = false;
            break;
        }
    }
    m_info.timestampsReady = timestampsReady;

    m_info.ready = true;
    m_info.message = "Frame ring ready (triple-buffered fences + scratch + descriptor pools)";
    return true;
#else
    (void)device;
    m_info.message = "FrameManager stub — Vulkan backend disabled (CPU scratch ready)";
    m_info.timestampsReady = false;
    return false;
#endif
}

void FrameManager::shutdown() {
    m_debugNamesSet = 0;
#if defined(FUSE_VULKAN_BACKEND)
    if (m_device == nullptr) {
        return;
    }

    auto vkDevice = static_cast<VkDevice>(m_device);
    for (u32 i = 0; i < kFramesInFlight; ++i) {
        if (m_slots[i].commands.descriptorPool != nullptr) {
            vkDestroyDescriptorPool(vkDevice,
                                    static_cast<VkDescriptorPool>(m_slots[i].commands.descriptorPool),
                                    nullptr);
            m_slots[i].commands.descriptorPool = nullptr;
        }
        if (m_slots[i].commands.primaryCommandBuffer != nullptr) {
            VkCommandBuffer primary =
                static_cast<VkCommandBuffer>(m_slots[i].commands.primaryCommandBuffer);
            vkFreeCommandBuffers(vkDevice,
                                 static_cast<VkCommandPool>(m_slots[i].commands.commandPool),
                                 1,
                                 &primary);
            m_slots[i].commands.primaryCommandBuffer = nullptr;
        }
        if (m_slots[i].commands.transferCommandBuffer != nullptr) {
            VkCommandBuffer transfer =
                static_cast<VkCommandBuffer>(m_slots[i].commands.transferCommandBuffer);
            vkFreeCommandBuffers(vkDevice,
                                 static_cast<VkCommandPool>(m_slots[i].commands.commandPool),
                                 1,
                                 &transfer);
            m_slots[i].commands.transferCommandBuffer = nullptr;
        }
        if (m_slots[i].commands.computeCommandBuffer != nullptr) {
            VkCommandBuffer compute =
                static_cast<VkCommandBuffer>(m_slots[i].commands.computeCommandBuffer);
            vkFreeCommandBuffers(vkDevice,
                                 static_cast<VkCommandPool>(m_slots[i].commands.commandPool),
                                 1,
                                 &compute);
            m_slots[i].commands.computeCommandBuffer = nullptr;
        }
        if (m_slots[i].commands.commandPool != nullptr) {
            vkDestroyCommandPool(vkDevice, static_cast<VkCommandPool>(m_slots[i].commands.commandPool), nullptr);
            m_slots[i].commands.commandPool = nullptr;
        }
        if (m_slots[i].imageAvailable != nullptr) {
            vkDestroySemaphore(vkDevice, static_cast<VkSemaphore>(m_slots[i].imageAvailable), nullptr);
            m_slots[i].imageAvailable = nullptr;
        }
        if (m_slots[i].renderFinished != nullptr) {
            vkDestroySemaphore(vkDevice, static_cast<VkSemaphore>(m_slots[i].renderFinished), nullptr);
            m_slots[i].renderFinished = nullptr;
        }
        if (m_slots[i].timelineSemaphore != nullptr) {
            vkDestroySemaphore(vkDevice, static_cast<VkSemaphore>(m_slots[i].timelineSemaphore), nullptr);
            m_slots[i].timelineSemaphore = nullptr;
            m_slots[i].timelineValue = 0;
        }
        if (m_slots[i].inFlightFence != nullptr) {
            vkDestroyFence(vkDevice, static_cast<VkFence>(m_slots[i].inFlightFence), nullptr);
            m_slots[i].inFlightFence = nullptr;
        }
        if (m_slots[i].timestampQueryPool != nullptr) {
            vkDestroyQueryPool(vkDevice, static_cast<VkQueryPool>(m_slots[i].timestampQueryPool), nullptr);
            m_slots[i].timestampQueryPool = nullptr;
        }
    }
    m_info.timestampsReady = false;
    m_timestampPeriod = 0.f;
    m_device = nullptr;
#endif
}

FrameSyncData& FrameManager::current() {
    return m_slots[currentSlotIndex()];
}

const FrameSyncData& FrameManager::current() const {
    return m_slots[currentSlotIndex()];
}

FrameSyncData& FrameManager::slot(u32 index) {
    return m_slots[index % kFramesInFlight];
}

const FrameSyncData& FrameManager::slot(u32 index) const {
    return m_slots[index % kFramesInFlight];
}

void* FrameManager::currentCommandBuffer() const {
    return current().commands.primaryCommandBuffer;
}

void* FrameManager::currentTransferCommandBuffer() const {
    return current().commands.transferCommandBuffer;
}

void* FrameManager::currentComputeCommandBuffer() const {
    return current().commands.computeCommandBuffer;
}

void* FrameManager::currentTimelineSemaphore() const {
    return current().timelineSemaphore;
}

void* FrameManager::currentDescriptorPool() const {
    return current().commands.descriptorPool;
}

u32 FrameManager::allocateDescriptorSets(void* setLayout, u32 count, void** outSets) {
    if (!isReady() || setLayout == nullptr || count == 0 || currentDescriptorPool() == nullptr) {
        return 0;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (count > 16u) {
        count = 16u;
    }

    VkDescriptorSetLayout layouts[16];
    VkDescriptorSet sets[16];
    for (u32 i = 0; i < count; ++i) {
        layouts[i] = static_cast<VkDescriptorSetLayout>(setLayout);
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = static_cast<VkDescriptorPool>(currentDescriptorPool());
    allocInfo.descriptorSetCount = count;
    allocInfo.pSetLayouts = layouts;

    if (vkAllocateDescriptorSets(static_cast<VkDevice>(m_device), &allocInfo, sets) != VK_SUCCESS) {
        return 0;
    }

    if (outSets != nullptr) {
        for (u32 i = 0; i < count; ++i) {
            outSets[i] = sets[i];
        }
    }

    m_descriptorSetsAllocatedThisFrame += count;
    return count;
#else
    (void)outSets;
    return 0;
#endif
}

u64 FrameManager::currentTimelineValue() const {
    return current().timelineValue;
}

void FrameManager::writeTimestampBegin(void* commandBuffer) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_info.timestampsReady || commandBuffer == nullptr || m_device == nullptr) {
        return;
    }

    FrameSyncData& slot = current();
    if (slot.timestampQueryPool == nullptr) {
        return;
    }

    auto cmd = static_cast<VkCommandBuffer>(commandBuffer);
    auto pool = static_cast<VkQueryPool>(slot.timestampQueryPool);
    vkCmdResetQueryPool(cmd, pool, 0, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
    ++m_info.timestampWriteCount;
#else
    (void)commandBuffer;
#endif
}

void FrameManager::writeTimestampEnd(void* commandBuffer) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!m_info.timestampsReady || commandBuffer == nullptr || m_device == nullptr) {
        return;
    }

    FrameSyncData& slot = current();
    if (slot.timestampQueryPool == nullptr) {
        return;
    }

    auto cmd = static_cast<VkCommandBuffer>(commandBuffer);
    auto pool = static_cast<VkQueryPool>(slot.timestampQueryPool);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
    ++m_info.timestampWriteCount;
#else
    (void)commandBuffer;
#endif
}

bool FrameManager::readLastGpuTimeNs(u32 slotIndex, u64* outNs) {
    if (outNs != nullptr) {
        *outNs = 0;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (!m_info.timestampsReady || m_device == nullptr || m_timestampPeriod <= 0.f) {
        return false;
    }

    const u32 index = slotIndex % kFramesInFlight;
    const FrameSyncData& slot = m_slots[index];
    if (slot.timestampQueryPool == nullptr) {
        return false;
    }

    u64 timestamps[2] = {0, 0};
    const VkResult result = vkGetQueryPoolResults(static_cast<VkDevice>(m_device),
                                                  static_cast<VkQueryPool>(slot.timestampQueryPool),
                                                  0,
                                                  2,
                                                  sizeof(timestamps),
                                                  timestamps,
                                                  sizeof(u64),
                                                  VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        return false;
    }

    const u64 ticks = timestamps[1] - timestamps[0];
    const u64 ns = static_cast<u64>(static_cast<double>(ticks) * static_cast<double>(m_timestampPeriod));
    m_info.lastGpuTimeNs = ns;
    if (outNs != nullptr) {
        *outNs = ns;
    }
    return true;
#else
    (void)slotIndex;
    return false;
#endif
}

fuse::alloc::FrameAllocator& FrameManager::scratch() {
    return *m_scratch[currentSlotIndex()];
}

const fuse::alloc::FrameAllocator& FrameManager::scratch() const {
    return *m_scratch[currentSlotIndex()];
}

void* FrameManager::allocateScratch(u32 bytes, u32 align) {
    return scratch().allocate(bytes, align);
}

u32 FrameManager::scratchUsedBytes() const {
    return scratch().usedBytes();
}

void FrameManager::signalTickComplete() {
    m_tickComplete = true;
}

bool FrameManager::waitInFlightFence(u32 slotIndex) {
    const u32 index = slotIndex % kFramesInFlight;
    FrameSyncData& slot = m_slots[index];

#if defined(FUSE_VULKAN_BACKEND)
    if (!m_info.ready || slot.inFlightFence == nullptr) {
        return false;
    }

    if (slot.fenceSignaled) {
        auto fence = static_cast<VkFence>(slot.inFlightFence);
        const VkResult status = vkGetFenceStatus(static_cast<VkDevice>(m_device), fence);
        if (status == VK_NOT_READY) {
            if (!clearStubInFlightFence(static_cast<VkDevice>(m_device), slot)) {
                return false;
            }
        } else if (status == VK_SUCCESS) {
            if (vkResetFences(static_cast<VkDevice>(m_device), 1, &fence) != VK_SUCCESS) {
                return false;
            }
        } else if (vkWaitForFences(static_cast<VkDevice>(m_device), 1, &fence, VK_TRUE, UINT64_MAX) !=
                   VK_SUCCESS) {
            return false;
        } else if (vkResetFences(static_cast<VkDevice>(m_device), 1, &fence) != VK_SUCCESS) {
            return false;
        }

        u64 ns = 0;
        if (readLastGpuTimeNs(index, &ns)) {
            m_info.lastGpuTimeNs = ns;
        }
    }
#else
    if (!m_info.ready) {
        return false;
    }
#endif

    slot.fenceSignaled = false;
    return true;
}

void FrameManager::beginFrame(u32 frameIndex) {
    m_lastBarrierFrame = frameIndex;
    const u32 index = currentSlotIndex();

#if defined(FUSE_VULKAN_BACKEND)
    FrameSyncData& slot = m_slots[index];
    if (m_info.ready) {
        if (slot.fenceSignaled && slot.inFlightFence != nullptr) {
            if (!clearStubInFlightFence(static_cast<VkDevice>(m_device), slot)) {
                return;
            }
            u64 ns = 0;
            if (readLastGpuTimeNs(index, &ns)) {
                m_info.lastGpuTimeNs = ns;
            }
        }
        slot.fenceSignaled = false;

        if (slot.commands.descriptorPool != nullptr) {
            vkResetDescriptorPool(static_cast<VkDevice>(m_device),
                                  static_cast<VkDescriptorPool>(slot.commands.descriptorPool),
                                  0);
            ++m_descriptorPoolResetCount;
            m_descriptorSetsAllocatedThisFrame = 0;
        }
    }
#else
    (void)frameIndex;
#endif

    m_descriptorSetsAllocatedThisFrame = 0;
    if (m_scratch[index]) {
        m_scratch[index]->reset();
    }
}

void FrameManager::endFrame() {
    if (!m_info.ready) {
        return;
    }

    m_slots[currentSlotIndex()].fenceSignaled = true;
    m_info.currentIndex = (m_info.currentIndex + 1u) % kFramesInFlight;
    ++m_info.totalFrames;
    m_tickComplete = false;
}

} // namespace fuse::renderer
