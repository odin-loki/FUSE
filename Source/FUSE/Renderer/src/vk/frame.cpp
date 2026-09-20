#include <fuse/renderer/vk/frame.hpp>

#include <array>

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
        return false;
    }

    m_device = device.nativeHandle();
    auto vkDevice = static_cast<VkDevice>(m_device);

    const u32 graphicsFamily = device.queues().graphicsFamily;

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
        allocInfo.commandBufferCount = 2;

        VkCommandBuffer commandBuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
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
    }

    m_info.ready = true;
    m_info.message = "Frame ring ready (triple-buffered fences + scratch + descriptor pools)";
    return true;
#else
    (void)device;
    m_info.message = "FrameManager stub — Vulkan backend disabled (CPU scratch ready)";
    return false;
#endif
}

void FrameManager::shutdown() {
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
    }
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

void* FrameManager::currentTimelineSemaphore() const {
    return current().timelineSemaphore;
}

void* FrameManager::currentDescriptorPool() const {
    return current().commands.descriptorPool;
}

u64 FrameManager::currentTimelineValue() const {
    return current().timelineValue;
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
        }
        slot.fenceSignaled = false;

        if (slot.commands.descriptorPool != nullptr) {
            vkResetDescriptorPool(static_cast<VkDevice>(m_device),
                                  static_cast<VkDescriptorPool>(slot.commands.descriptorPool),
                                  0);
            ++m_descriptorPoolResetCount;
        }
    }
#else
    (void)frameIndex;
#endif

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
