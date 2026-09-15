#include <fuse/renderer/vk/frame.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

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

bool FrameManager::initialize(VulkanDevice& device) {
#if defined(FUSE_VULKAN_BACKEND)
    if (!device.isValid()) {
        m_info.message = "FrameManager skipped — device not ready";
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
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer primaryCommandBuffer = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &primaryCommandBuffer) != VK_SUCCESS) {
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
        m_slots[i].commands.primaryCommandBuffer = primaryCommandBuffer;
    }

    m_info.ready = true;
    m_info.framesInFlight = kFramesInFlight;
    m_info.message = "Frame ring ready (triple-buffered fences + semaphores)";
    return true;
#else
    (void)device;
    m_info.message = "FrameManager stub — Vulkan backend disabled";
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
        if (m_slots[i].commands.primaryCommandBuffer != nullptr) {
            VkCommandBuffer primary =
                static_cast<VkCommandBuffer>(m_slots[i].commands.primaryCommandBuffer);
            vkFreeCommandBuffers(vkDevice,
                                 static_cast<VkCommandPool>(m_slots[i].commands.commandPool),
                                 1,
                                 &primary);
            m_slots[i].commands.primaryCommandBuffer = nullptr;
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
        if (m_slots[i].inFlightFence != nullptr) {
            vkDestroyFence(vkDevice, static_cast<VkFence>(m_slots[i].inFlightFence), nullptr);
            m_slots[i].inFlightFence = nullptr;
        }
    }
    m_device = nullptr;
#endif
}

const FrameSyncData& FrameManager::current() const {
    return m_slots[m_info.currentIndex % kFramesInFlight];
}

const FrameSyncData& FrameManager::slot(u32 index) const {
    return m_slots[index % kFramesInFlight];
}

void* FrameManager::currentCommandBuffer() const {
    return current().commands.primaryCommandBuffer;
}

void FrameManager::signalTickComplete() {
    m_tickComplete = true;
}

void FrameManager::beginFrame(u32 frameIndex) {
    m_lastBarrierFrame = frameIndex;

#if defined(FUSE_VULKAN_BACKEND)
    if (!m_info.ready) {
        return;
    }

    // B2.2 sketch: sync objects are allocated; vkWaitForFences is deferred until
    // queue submission lands (B2.3). CPU ring tracks slot reuse via fenceSignaled.
    FrameSyncData& slot = m_slots[m_info.currentIndex % kFramesInFlight];
    if (slot.fenceSignaled && slot.inFlightFence != nullptr) {
        auto fence = static_cast<VkFence>(slot.inFlightFence);
        vkResetFences(static_cast<VkDevice>(m_device), 1, &fence);
    }
    slot.fenceSignaled = false;
#else
    (void)frameIndex;
#endif
}

void FrameManager::endFrame() {
    if (!m_info.ready) {
        return;
    }

    m_slots[m_info.currentIndex % kFramesInFlight].fenceSignaled = true;
    m_info.currentIndex = (m_info.currentIndex + 1u) % kFramesInFlight;
    ++m_info.totalFrames;
    m_tickComplete = false;
}

} // namespace fuse::renderer
