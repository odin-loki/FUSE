#include <fuse/renderer/vk/upload_queue.hpp>

#include <fuse/renderer/vk/bindless.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

constexpr u32 kNoBatch = UINT32_MAX;

usize alignUp(usize value, usize alignment) {
    return (value + (alignment - 1u)) & ~(alignment - 1u);
}

#if defined(FUSE_VULKAN_BACKEND)
/// Everything a later graphics / compute / transfer consumer may read after an upload.
constexpr VkAccessFlags kConsumerReadAccess =
    VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
    VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

struct Batch {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    // Cross-family only: graphics-queue acquire side of the ownership transfer.
    VkCommandPool acquirePool = VK_NULL_HANDLE;
    VkCommandBuffer acquireCmd = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    bool recording = false;
    bool bufferCopies = false;
};
#endif

/// A submitted (or command-less) batch still holding ring space.
struct InFlight {
    u32 batch = kNoBatch; ///< kNoBatch: nothing to wait on (stub / staged without commands)
    u64 serial = 0;
    usize ringEnd = 0;
    usize bytes = 0;
};

} // namespace

struct UploadQueue::Impl {
    VulkanDevice* device = nullptr;
    u8* mapped = nullptr;
    usize capacity = 0;
    bool ready = false;

    // Ring state: [tail, head) (circular) is held by unretired batches; `used` includes wrap padding.
    usize head = 0;
    usize tail = 0;
    usize used = 0;

    // Open batch (accumulating, not submitted).
    u64 openSerial = 0;
    usize openBytes = 0;
    u32 openCopies = 0;
    u32 openBatch = kNoBatch;

    u64 nextSerial = 1;
    u64 lastSubmittedSerial = 0;
    u64 completed = 0;
    std::deque<InFlight> inFlight;
    UploadQueueStats stats{};
    bool lastStageTimedOut = false;

#if defined(FUSE_VULKAN_BACKEND)
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 family = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    u32 graphicsFamily = 0;
    bool crossFamily = false;
    std::vector<Batch> batches;
    std::vector<u32> freeBatches;
#endif

    bool gpu() const {
#if defined(FUSE_VULKAN_BACKEND)
        return vkDevice != VK_NULL_HANDLE && queue != VK_NULL_HANDLE && staging != VK_NULL_HANDLE;
#else
        return false;
#endif
    }

    void refreshStats() {
        stats.batchesInFlight = static_cast<u32>(inFlight.size());
        stats.maxBatchesInFlight = std::max(stats.maxBatchesInFlight, stats.batchesInFlight);
        stats.bytesInFlight = used;
        stats.maxBytesInFlight = std::max(stats.maxBytesInFlight, used);
    }

    u64 ensureOpenSerial() {
        if (openSerial == 0) {
            openSerial = nextSerial++;
        }
        return openSerial;
    }

    /// Reserve `size` bytes without waiting. Never hands out bytes inside [tail, head).
    bool tryAllocate(usize size, usize& outOffset) {
        if (used == 0) {
            tail = head;
            usize offset = head;
            if (head + size > capacity) {
                if (head != 0) {
                    ++stats.ringWraps;
                }
                offset = 0;
            }
            const usize newHead = std::min(alignUp(offset + size, kStagingAlignBytes), capacity);
            tail = offset;
            used = newHead - offset;
            openBytes += used;
            head = newHead;
            outOffset = offset;
            return true;
        }
        if (head == tail) {
            return false; // full
        }
        if (head > tail) {
            if (head + size <= capacity) {
                const usize newHead = std::min(alignUp(head + size, kStagingAlignBytes), capacity);
                used += newHead - head;
                openBytes += newHead - head;
                outOffset = head;
                head = newHead;
                return true;
            }
            if (size <= tail) {
                const usize padding = capacity - head;
                const usize newHead = std::min(alignUp(size, kStagingAlignBytes), capacity);
                used += padding + newHead;
                openBytes += padding + newHead;
                ++stats.ringWraps;
                outOffset = 0;
                head = newHead;
                return true;
            }
            return false;
        }
        if (head + size <= tail) {
            const usize newHead = std::min(alignUp(head + size, kStagingAlignBytes), capacity);
            used += newHead - head;
            openBytes += newHead - head;
            outOffset = head;
            head = newHead;
            return true;
        }
        return false;
    }

    void retireFront() {
        const InFlight entry = inFlight.front();
        inFlight.pop_front();
#if defined(FUSE_VULKAN_BACKEND)
        if (entry.batch != kNoBatch) {
            freeBatches.push_back(entry.batch);
        }
#endif
        used -= std::min(used, entry.bytes);
        tail = entry.ringEnd;
        if (used == 0) {
            tail = head;
        }
        completed = entry.serial;
        ++stats.retiredBatches;
        refreshStats();
    }

    /// Poll the oldest batch's fence; true when it can be (and was) retired.
    bool pollFront() {
        if (inFlight.empty()) {
            return false;
        }
#if defined(FUSE_VULKAN_BACKEND)
        const InFlight& entry = inFlight.front();
        if (entry.batch != kNoBatch &&
            vkGetFenceStatus(vkDevice, batches[entry.batch].fence) != VK_SUCCESS) {
            return false;
        }
#endif
        retireFront();
        return true;
    }

    /// Block (bounded) on the oldest batch, then retire it.
    bool waitFront(u64 timeoutNs) {
        if (inFlight.empty()) {
            return true;
        }
#if defined(FUSE_VULKAN_BACKEND)
        const InFlight& entry = inFlight.front();
        if (entry.batch != kNoBatch) {
            VkFence fence = batches[entry.batch].fence;
            const VkResult result = vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, timeoutNs);
            if (result != VK_SUCCESS) {
                if (result == VK_TIMEOUT) {
                    ++stats.fenceTimeouts;
                }
                return false;
            }
        }
#else
        (void)timeoutNs;
#endif
        retireFront();
        return true;
    }

#if defined(FUSE_VULKAN_BACKEND)
    bool createBatch(Batch& batch) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = family;
        if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &batch.pool) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = batch.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &batch.cmd) != VK_SUCCESS) {
            return false;
        }
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(vkDevice, &fenceInfo, nullptr, &batch.fence) != VK_SUCCESS) {
            return false;
        }
        if (crossFamily) {
            poolInfo.queueFamilyIndex = graphicsFamily;
            if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &batch.acquirePool) != VK_SUCCESS) {
                return false;
            }
            allocInfo.commandPool = batch.acquirePool;
            if (vkAllocateCommandBuffers(vkDevice, &allocInfo, &batch.acquireCmd) != VK_SUCCESS) {
                return false;
            }
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &batch.semaphore) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    void destroyBatch(Batch& batch) {
        if (batch.semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(vkDevice, batch.semaphore, nullptr);
        }
        if (batch.acquirePool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(vkDevice, batch.acquirePool, nullptr);
        }
        if (batch.fence != VK_NULL_HANDLE) {
            vkDestroyFence(vkDevice, batch.fence, nullptr);
        }
        if (batch.pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(vkDevice, batch.pool, nullptr);
        }
        batch = Batch{};
    }

    /// Open a command buffer for the current batch, reusing a retired batch or creating one.
    Batch* beginRecording(u64 timeoutNs) {
        if (openBatch != kNoBatch) {
            return &batches[openBatch];
        }
        while (freeBatches.empty()) {
            if (batches.size() < kMaxBatches) {
                batches.emplace_back();
                if (!createBatch(batches.back())) {
                    destroyBatch(batches.back());
                    batches.pop_back();
                    return nullptr;
                }
                freeBatches.push_back(static_cast<u32>(batches.size() - 1u));
                break;
            }
            // Every batch object is in flight: recycle the oldest once its fence signals.
            if (!pollFront() && !waitFront(timeoutNs)) {
                return nullptr;
            }
        }

        const u32 index = freeBatches.back();
        Batch& batch = batches[index];
        if (vkResetFences(vkDevice, 1, &batch.fence) != VK_SUCCESS ||
            vkResetCommandPool(vkDevice, batch.pool, 0) != VK_SUCCESS) {
            return nullptr;
        }
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(batch.cmd, &beginInfo) != VK_SUCCESS) {
            return nullptr;
        }
        if (crossFamily) {
            if (vkResetCommandPool(vkDevice, batch.acquirePool, 0) != VK_SUCCESS ||
                vkBeginCommandBuffer(batch.acquireCmd, &beginInfo) != VK_SUCCESS) {
                vkEndCommandBuffer(batch.cmd);
                return nullptr;
            }
        } else {
            // WAR/WAW against earlier work on this queue that may still touch a destination
            // (re-uploads into live resources). Fresh resources pay one cheap execution dependency.
            VkMemoryBarrier before{};
            before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(batch.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 1, &before, 0, nullptr, 0, nullptr);
        }
        freeBatches.pop_back();
        batch.recording = true;
        batch.bufferCopies = false;
        openBatch = index;
        return &batch;
    }

    /// Copies inside one batch may target the same destination (rewrite, then patch): order them.
    void orderAfterPreviousCopies(Batch& batch) const {
        if (openCopies == 0) {
            return;
        }
        VkMemoryBarrier between{};
        between.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        between.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        between.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(batch.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                             &between, 0, nullptr, 0, nullptr);
    }

    bool submitOpenBatch() {
        Batch& batch = batches[openBatch];
        if (!crossFamily && batch.bufferCopies) {
            VkMemoryBarrier after{};
            after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            after.dstAccessMask = kConsumerReadAccess;
            vkCmdPipelineBarrier(batch.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                 1, &after, 0, nullptr, 0, nullptr);
        }
        batch.recording = false;
        const bool ended = vkEndCommandBuffer(batch.cmd) == VK_SUCCESS &&
                           (!crossFamily || vkEndCommandBuffer(batch.acquireCmd) == VK_SUCCESS);
        if (!ended) {
            return false;
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &batch.cmd;
        if (!crossFamily) {
            return vkQueueSubmit(queue, 1, &submit, batch.fence) == VK_SUCCESS;
        }

        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &batch.semaphore;
        if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
            return false;
        }
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo acquire{};
        acquire.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        acquire.waitSemaphoreCount = 1;
        acquire.pWaitSemaphores = &batch.semaphore;
        acquire.pWaitDstStageMask = &waitStage;
        acquire.commandBufferCount = 1;
        acquire.pCommandBuffers = &batch.acquireCmd;
        if (vkQueueSubmit(graphicsQueue, 1, &acquire, batch.fence) != VK_SUCCESS) {
            // The semaphore stays signalled; drain so the batch objects can be reused safely.
            vkQueueWaitIdle(queue);
            vkDeviceWaitIdle(vkDevice);
            vkDestroySemaphore(vkDevice, batch.semaphore, nullptr);
            batch.semaphore = VK_NULL_HANDLE;
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &batch.semaphore);
            return false;
        }
        return true;
    }
#endif

    UploadTicket flush() {
        if (openSerial == 0) {
            return UploadTicket{lastSubmittedSerial, true};
        }
        InFlight entry{};
        entry.serial = openSerial;
        entry.ringEnd = head;
        entry.bytes = openBytes;
#if defined(FUSE_VULKAN_BACKEND)
        if (openBatch != kNoBatch) {
            if (submitOpenBatch()) {
                entry.batch = openBatch;
                ++stats.submittedBatches;
            } else {
                // Nothing reads the ring for this batch; its slot is reusable right away.
                ++stats.submitFailures;
                batches[openBatch].recording = false;
                freeBatches.push_back(openBatch);
            }
        }
        openBatch = kNoBatch;
#endif
        inFlight.push_back(entry);
        lastSubmittedSerial = openSerial;
        openSerial = 0;
        openBytes = 0;
        openCopies = 0;
        refreshStats();
        // Command-less batches (stub backend) retire immediately, in order.
        while (!inFlight.empty() && inFlight.front().batch == kNoBatch) {
            retireFront();
        }
        return UploadTicket{entry.serial, true};
    }

    void retireAll() {
        while (pollFront()) {
        }
    }
};

UploadQueue::UploadQueue() : m_impl(std::make_unique<Impl>()) {}

UploadQueue::~UploadQueue() {
    destroy();
}

bool UploadQueue::init(VulkanDevice* device, void* stagingBuffer, void* stagingMapped, usize capacity) {
    destroy();
    Impl& s = *m_impl;
    s = Impl{};
    s.device = device;
    s.mapped = static_cast<u8*>(stagingMapped);
    s.capacity = capacity;
#if defined(FUSE_VULKAN_BACKEND)
    if (device != nullptr && device->isValid() && device->nativeHandle() != nullptr &&
        bindlessNativeHandleReady(stagingBuffer)) {
        const VulkanQueues& queues = device->queues();
        s.vkDevice = static_cast<VkDevice>(device->nativeHandle());
        s.staging = static_cast<VkBuffer>(stagingBuffer);
        s.graphicsQueue = static_cast<VkQueue>(queues.graphics);
        s.graphicsFamily = queues.graphicsFamily;
        if (queues.transfer != nullptr) {
            s.queue = static_cast<VkQueue>(queues.transfer);
            s.family = queues.transferFamily;
        } else {
            s.queue = s.graphicsQueue;
            s.family = queues.graphicsFamily;
        }
        s.crossFamily = s.family != s.graphicsFamily && s.graphicsQueue != VK_NULL_HANDLE;
        s.stats.dedicatedTransferQueue = s.queue != s.graphicsQueue;
        s.stats.queueFamilyOwnershipTransfer = s.crossFamily;
        s.batches.reserve(kMaxBatches);
        s.freeBatches.reserve(kMaxBatches);
    }
#else
    (void)stagingBuffer;
#endif
    s.ready = s.mapped != nullptr && capacity > 0;
    return s.ready;
}

void UploadQueue::destroy() {
    if (!m_impl) {
        return;
    }
    Impl& s = *m_impl;
    if (s.ready) {
        s.flush();
        // Bounded per batch; on timeout drain the device so nothing is destroyed in use.
        while (!s.inFlight.empty()) {
            if (!s.waitFront(kDefaultFenceTimeoutNs)) {
#if defined(FUSE_VULKAN_BACKEND)
                if (s.vkDevice != VK_NULL_HANDLE) {
                    vkDeviceWaitIdle(s.vkDevice);
                }
#endif
                s.retireFront();
            }
        }
    }
#if defined(FUSE_VULKAN_BACKEND)
    for (Batch& batch : s.batches) {
        s.destroyBatch(batch);
    }
    s.batches.clear();
    s.freeBatches.clear();
#endif
    s.ready = false;
}

bool UploadQueue::isReady() const {
    return m_impl->ready;
}

bool UploadQueue::stage(const void* data, usize size, usize& outOffset) {
    Impl& s = *m_impl;
    s.lastStageTimedOut = false;
    if (!s.ready || data == nullptr || size == 0 || size > s.capacity) {
        return false;
    }
    usize offset = 0;
    while (!s.tryAllocate(size, offset)) {
        if (s.pollFront()) {
            continue;
        }
        if (s.openBytes > 0) {
            s.flush(); // the open batch itself holds ring space: submit it so it can retire
            continue;
        }
        if (s.inFlight.empty()) {
            return false;
        }
        ++s.stats.ringStalls;
        if (!s.waitFront(kDefaultFenceTimeoutNs)) {
            s.lastStageTimedOut = true;
            return false;
        }
    }
    s.ensureOpenSerial();
    std::memcpy(s.mapped + offset, data, size);
    s.refreshStats();
    outOffset = offset;
    return true;
}

bool UploadQueue::recordBufferCopy(void* dstBuffer, usize srcOffset, usize dstOffset, usize size) {
    Impl& s = *m_impl;
    if (!s.ready || !s.gpu() || dstBuffer == nullptr || size == 0) {
        return false;
    }
#if defined(FUSE_VULKAN_BACKEND)
    Batch* batch = s.beginRecording(kDefaultFenceTimeoutNs);
    if (batch == nullptr) {
        return false;
    }
    s.ensureOpenSerial();
    s.orderAfterPreviousCopies(*batch);
    const VkBuffer dst = static_cast<VkBuffer>(dstBuffer);
    VkBufferCopy region{};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size = size;
    vkCmdCopyBuffer(batch->cmd, s.staging, dst, 1, &region);
    batch->bufferCopies = true;

    if (s.crossFamily) {
        VkBufferMemoryBarrier release{};
        release.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        release.dstAccessMask = 0;
        release.srcQueueFamilyIndex = s.family;
        release.dstQueueFamilyIndex = s.graphicsFamily;
        release.buffer = dst;
        release.offset = 0;
        release.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(batch->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 1, &release, 0, nullptr);
        VkBufferMemoryBarrier acquire = release;
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = kConsumerReadAccess;
        vkCmdPipelineBarrier(batch->acquireCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &acquire, 0, nullptr);
    }
    ++s.openCopies;
    ++s.stats.recordedCopies;
    return true;
#else
    (void)srcOffset;
    (void)dstOffset;
    return false;
#endif
}

bool UploadQueue::recordImageCopy(void* dstImage, usize srcOffset, u32 width, u32 height, u32 depth) {
    Impl& s = *m_impl;
    if (!s.ready || !s.gpu() || dstImage == nullptr || width == 0 || height == 0) {
        return false;
    }
#if defined(FUSE_VULKAN_BACKEND)
    Batch* batch = s.beginRecording(kDefaultFenceTimeoutNs);
    if (batch == nullptr) {
        return false;
    }
    s.ensureOpenSerial();
    s.orderAfterPreviousCopies(*batch);
    const VkImage image = static_cast<VkImage>(dstImage);

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(batch->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset = srcOffset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, depth > 0 ? depth : 1u};
    vkCmdCopyBufferToImage(batch->cmd, s.staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader{};
    toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.image = image;
    toShader.subresourceRange = toTransfer.subresourceRange;
    if (!s.crossFamily) {
        vkCmdPipelineBarrier(batch->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toShader);
    } else {
        // Release (with the layout transition) on the transfer family, matching acquire on graphics.
        VkImageMemoryBarrier release = toShader;
        release.dstAccessMask = 0;
        release.srcQueueFamilyIndex = s.family;
        release.dstQueueFamilyIndex = s.graphicsFamily;
        vkCmdPipelineBarrier(batch->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &release);
        VkImageMemoryBarrier acquire = release;
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(batch->acquireCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &acquire);
    }
    ++s.openCopies;
    ++s.stats.recordedCopies;
    return true;
#else
    (void)srcOffset;
    (void)depth;
    return false;
#endif
}

UploadTicket UploadQueue::pendingTicket() const {
    return UploadTicket{m_impl->openSerial, m_impl->openSerial != 0};
}

UploadTicket UploadQueue::flush() {
    if (!m_impl->ready) {
        return UploadTicket{};
    }
    return m_impl->flush();
}

bool UploadQueue::isComplete(UploadTicket ticket) {
    Impl& s = *m_impl;
    if (ticket.serial <= s.completed) {
        return true;
    }
    if (ticket.serial == s.openSerial) {
        return false;
    }
    s.retireAll();
    return ticket.serial <= s.completed;
}

bool UploadQueue::wait(UploadTicket ticket, u64 timeoutNs) {
    Impl& s = *m_impl;
    if (ticket.serial <= s.completed) {
        return true;
    }
    if (ticket.serial >= s.openSerial && s.openSerial != 0) {
        s.flush();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeoutNs);
    while (s.completed < ticket.serial && !s.inFlight.empty()) {
        const auto now = std::chrono::steady_clock::now();
        const u64 remaining =
            now >= deadline
                ? 0u
                : static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count());
        if (!s.waitFront(remaining)) {
            return false;
        }
    }
    return ticket.serial <= s.completed;
}

bool UploadQueue::waitAll(u64 timeoutNs) {
    Impl& s = *m_impl;
    if (!s.ready) {
        return true;
    }
    const UploadTicket last = s.flush();
    return wait(last, timeoutNs);
}

u32 UploadQueue::retireCompleted() {
    Impl& s = *m_impl;
    u32 retired = 0;
    while (s.pollFront()) {
        ++retired;
    }
    return retired;
}

bool UploadQueue::hasPendingWork() const {
    return m_impl->openSerial != 0 || !m_impl->inFlight.empty();
}

u64 UploadQueue::completedSerial() const {
    return m_impl->completed;
}

usize UploadQueue::ringHead() const {
    return m_impl->head;
}

usize UploadQueue::ringCapacity() const {
    return m_impl->capacity;
}

usize UploadQueue::ringBytesInUse() const {
    return m_impl->used;
}

const UploadQueueStats& UploadQueue::stats() const {
    return m_impl->stats;
}

bool UploadQueue::lastStageTimedOut() const {
    return m_impl->lastStageTimedOut;
}

} // namespace fuse::renderer
