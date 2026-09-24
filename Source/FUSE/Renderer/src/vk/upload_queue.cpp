#include <fuse/renderer/vk/upload_queue.hpp>

#include <fuse/renderer/vk/bindless.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <numeric>
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

/// bufferOffset of a buffer->image copy must be a multiple of the texel size, and of 4 on queues
/// without graphics/compute (dedicated transfer families).
usize mipOffsetAlignment(u32 bytesPerTexel) {
    return std::lcm(static_cast<usize>(4), static_cast<usize>(bytesPerTexel));
}

bool validImageDesc(const UploadImageDesc& desc) {
    return desc.width > 0 && desc.height > 0 && desc.mipLevels > 0 && desc.layerCount > 0 &&
           desc.bytesPerTexel > 0 && desc.mipLevels <= 32;
}

u32 mipExtent(u32 extent, u32 mip) {
    return std::max(1u, (extent > 0 ? extent : 1u) >> mip);
}

usize mipBytes(const UploadImageDesc& desc, u32 mip) {
    return static_cast<usize>(mipExtent(desc.width, mip)) * mipExtent(desc.height, mip) *
           mipExtent(desc.depth, mip) * desc.layerCount * desc.bytesPerTexel;
}

/// Offset of `mip` inside the staged chain (each mip aligned; mip 0 at 0).
usize stagedMipOffset(const UploadImageDesc& desc, u32 mip) {
    const usize alignment = mipOffsetAlignment(desc.bytesPerTexel);
    usize offset = 0;
    for (u32 level = 0; level < mip; ++level) {
        offset = (offset + mipBytes(desc, level) + alignment - 1u) / alignment * alignment;
    }
    return offset;
}

#if defined(FUSE_VULKAN_BACKEND)
/// Everything a later graphics / compute / transfer consumer may do with an uploaded buffer. Writes
/// are included so a later shader/transfer write is ordered (WAW) after the upload, not just reads.
constexpr VkAccessFlags kConsumerBufferAccess =
    VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
    VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
/// Reads allowed in SHADER_READ_ONLY_OPTIMAL (the layout uploaded images end in).
constexpr VkAccessFlags kConsumerImageAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

/// Image destination of a cross-family batch: mips [0, mipLevels) x layers [0, layerCount).
struct ImageOwnership {
    VkImage image = VK_NULL_HANDLE;
    u32 mipLevels = 1;
    u32 layerCount = 1;
};

struct Batch {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    // Cross-family only (graphics-family pool): `releaseCmd` hands each destination graphics ->
    // transfer before the copies, `acquireCmd` takes it back after them.
    VkCommandPool graphicsPool = VK_NULL_HANDLE;
    VkCommandBuffer releaseCmd = VK_NULL_HANDLE;
    VkCommandBuffer acquireCmd = VK_NULL_HANDLE;
    VkSemaphore toTransfer = VK_NULL_HANDLE; ///< graphics release -> transfer copies
    VkSemaphore toGraphics = VK_NULL_HANDLE; ///< transfer release -> graphics acquire
    std::vector<VkBuffer> buffers;           ///< destinations owned by the transfer family this batch
    std::vector<ImageOwnership> images;
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

/// FIFO of in-flight batches in fixed storage (no heap traffic on push/pop, unlike std::deque,
/// which allocates a node every few pushes). Capacity kMaxBatches: at most kMaxBatches entries
/// own a batch object; command-less entries queued behind them make the owner wait (see flush).
class InFlightRing {
public:
    static constexpr u32 kCapacity = UploadQueue::kMaxBatches;

    bool empty() const { return m_count == 0; }
    bool full() const { return m_count == kCapacity; }
    u32 size() const { return m_count; }
    const InFlight& front() const { return m_entries[m_head]; }
    void push_back(const InFlight& entry) {
        m_entries[(m_head + m_count) % kCapacity] = entry;
        ++m_count;
    }
    void pop_front() {
        m_head = (m_head + 1u) % kCapacity;
        --m_count;
    }

private:
    std::array<InFlight, kCapacity> m_entries{};
    u32 m_head = 0;
    u32 m_count = 0;
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
    InFlightRing inFlight; ///< fixed storage inside Impl: flush never allocates
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
    // Scratch for the end-of-batch release/acquire barriers (kept to avoid per-flush allocations).
    std::vector<VkBufferMemoryBarrier> bufferBarriers;
    std::vector<VkImageMemoryBarrier> imageBarriers;
    std::vector<VkBufferImageCopy> imageRegions;
#endif

    bool gpu() const {
#if defined(FUSE_VULKAN_BACKEND)
        return vkDevice != VK_NULL_HANDLE && queue != VK_NULL_HANDLE && staging != VK_NULL_HANDLE;
#else
        return false;
#endif
    }

    void refreshStats() {
        stats.batchesInFlight = inFlight.size();
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

    /// Reserve `size` ring bytes, retiring / flushing / waiting (bounded) as needed.
    bool reserve(usize size, usize& outOffset) {
        lastStageTimedOut = false;
        if (!ready || size == 0 || size > capacity) {
            return false;
        }
        usize offset = 0;
        while (!tryAllocate(size, offset)) {
            if (pollFront()) {
                continue;
            }
            if (openBytes > 0) {
                flush(); // the open batch itself holds ring space: submit it so it can retire
                continue;
            }
            if (inFlight.empty()) {
                return false;
            }
            ++stats.ringStalls;
            if (!waitFront(UploadQueue::kDefaultFenceTimeoutNs)) {
                lastStageTimedOut = true;
                return false;
            }
        }
        ensureOpenSerial();
        refreshStats();
        outOffset = offset;
        return true;
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
            if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &batch.graphicsPool) != VK_SUCCESS) {
                return false;
            }
            VkCommandBuffer graphicsCmds[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            allocInfo.commandPool = batch.graphicsPool;
            allocInfo.commandBufferCount = 2;
            if (vkAllocateCommandBuffers(vkDevice, &allocInfo, graphicsCmds) != VK_SUCCESS) {
                return false;
            }
            batch.releaseCmd = graphicsCmds[0];
            batch.acquireCmd = graphicsCmds[1];
            if (!createSemaphore(batch.toTransfer) || !createSemaphore(batch.toGraphics)) {
                return false;
            }
        }
        return true;
    }

    bool createSemaphore(VkSemaphore& semaphore) const {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        return vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &semaphore) == VK_SUCCESS;
    }

    /// A signalled binary semaphore nobody will wait on: drain the device, then replace it.
    void replaceSignalledSemaphore(VkSemaphore& semaphore) {
        vkDeviceWaitIdle(vkDevice);
        vkDestroySemaphore(vkDevice, semaphore, nullptr);
        semaphore = VK_NULL_HANDLE;
        createSemaphore(semaphore);
    }

    void destroyBatch(Batch& batch) {
        if (batch.toTransfer != VK_NULL_HANDLE) {
            vkDestroySemaphore(vkDevice, batch.toTransfer, nullptr);
        }
        if (batch.toGraphics != VK_NULL_HANDLE) {
            vkDestroySemaphore(vkDevice, batch.toGraphics, nullptr);
        }
        if (batch.graphicsPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(vkDevice, batch.graphicsPool, nullptr);
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
            // No barrier here: the transfer submit waits (TRANSFER stage) on a semaphore the
            // graphics release submit signals, which orders it after all earlier graphics work.
            if (vkResetCommandPool(vkDevice, batch.graphicsPool, 0) != VK_SUCCESS ||
                vkBeginCommandBuffer(batch.releaseCmd, &beginInfo) != VK_SUCCESS ||
                vkBeginCommandBuffer(batch.acquireCmd, &beginInfo) != VK_SUCCESS) {
                vkEndCommandBuffer(batch.cmd);
                return nullptr;
            }
            batch.buffers.clear();
            batch.images.clear();
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

    /// Cross-family: first copy into `buffer` this batch hands it graphics -> transfer (the
    /// graphics side may have read or written it; a partial update must keep the other bytes).
    void claimBuffer(Batch& batch, VkBuffer buffer) {
        if (std::find(batch.buffers.begin(), batch.buffers.end(), buffer) != batch.buffers.end()) {
            return;
        }
        batch.buffers.push_back(buffer);
        VkBufferMemoryBarrier release{};
        release.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        release.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        release.dstAccessMask = 0;
        release.srcQueueFamilyIndex = graphicsFamily;
        release.dstQueueFamilyIndex = family;
        release.buffer = buffer;
        release.offset = 0;
        release.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(batch.releaseCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 1, &release, 0, nullptr);
        VkBufferMemoryBarrier acquire = release;
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        // srcStage TRANSFER chains with the semaphore wait (TRANSFER) on the transfer submit.
        vkCmdPipelineBarrier(batch.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 1, &acquire, 0, nullptr);
    }

    /// Cross-family: true when `image` needs its UNDEFINED -> TRANSFER_DST transition for
    /// [mips x layers] in this batch (first copy, or a larger range than before). Images need no
    /// graphics -> transfer release: every upload discards the previous contents.
    bool claimImage(Batch& batch, VkImage image, u32 mipLevels, u32 layerCount) {
        for (ImageOwnership& owned : batch.images) {
            if (owned.image != image) {
                continue;
            }
            if (mipLevels <= owned.mipLevels && layerCount <= owned.layerCount) {
                return false; // already TRANSFER_DST and owned by the transfer family
            }
            owned.mipLevels = std::max(owned.mipLevels, mipLevels);
            owned.layerCount = std::max(owned.layerCount, layerCount);
            return true;
        }
        batch.images.push_back(ImageOwnership{image, mipLevels, layerCount});
        return true;
    }

    /// End of a cross-family batch: one release (transfer queue) and matching acquire (graphics
    /// queue) per destination. Image layouts go TRANSFER_DST -> SHADER_READ_ONLY in both halves:
    /// the spec requires identical layouts in the pair and performs the transition once.
    void recordOwnershipReturn(Batch& batch) {
        bufferBarriers.clear();
        imageBarriers.clear();
        for (VkBuffer buffer : batch.buffers) {
            VkBufferMemoryBarrier release{};
            release.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            release.dstAccessMask = 0;
            release.srcQueueFamilyIndex = family;
            release.dstQueueFamilyIndex = graphicsFamily;
            release.buffer = buffer;
            release.offset = 0;
            release.size = VK_WHOLE_SIZE;
            bufferBarriers.push_back(release);
        }
        for (const ImageOwnership& owned : batch.images) {
            VkImageMemoryBarrier release{};
            release.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            release.dstAccessMask = 0;
            release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            release.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            release.srcQueueFamilyIndex = family;
            release.dstQueueFamilyIndex = graphicsFamily;
            release.image = owned.image;
            release.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            release.subresourceRange.baseMipLevel = 0;
            release.subresourceRange.levelCount = owned.mipLevels;
            release.subresourceRange.baseArrayLayer = 0;
            release.subresourceRange.layerCount = owned.layerCount;
            imageBarriers.push_back(release);
        }
        if (bufferBarriers.empty() && imageBarriers.empty()) {
            return;
        }
        vkCmdPipelineBarrier(batch.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, static_cast<u32>(bufferBarriers.size()), bufferBarriers.data(),
                             static_cast<u32>(imageBarriers.size()), imageBarriers.data());
        for (VkBufferMemoryBarrier& barrier : bufferBarriers) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = kConsumerBufferAccess;
        }
        for (VkImageMemoryBarrier& barrier : imageBarriers) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = kConsumerImageAccess;
        }
        // srcStage ALL_COMMANDS chains with the semaphore wait (ALL_COMMANDS) on the acquire submit.
        vkCmdPipelineBarrier(batch.acquireCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, static_cast<u32>(bufferBarriers.size()), bufferBarriers.data(),
                             static_cast<u32>(imageBarriers.size()), imageBarriers.data());
        stats.ownershipBufferTransfers += batch.buffers.size();
        stats.ownershipImageTransfers += batch.images.size();
    }

    bool submitOpenBatch() {
        Batch& batch = batches[openBatch];
        if (!crossFamily && batch.bufferCopies) {
            VkMemoryBarrier after{};
            after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            after.dstAccessMask = kConsumerBufferAccess;
            vkCmdPipelineBarrier(batch.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                 1, &after, 0, nullptr, 0, nullptr);
        }
        if (crossFamily) {
            recordOwnershipReturn(batch);
        }
        batch.recording = false;
        const bool ended = vkEndCommandBuffer(batch.cmd) == VK_SUCCESS &&
                           (!crossFamily || (vkEndCommandBuffer(batch.releaseCmd) == VK_SUCCESS &&
                                             vkEndCommandBuffer(batch.acquireCmd) == VK_SUCCESS));
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

        // 1. Graphics: release destinations to the transfer family (after all earlier graphics work).
        VkSubmitInfo release{};
        release.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        release.commandBufferCount = 1;
        release.pCommandBuffers = &batch.releaseCmd;
        release.signalSemaphoreCount = 1;
        release.pSignalSemaphores = &batch.toTransfer;
        if (vkQueueSubmit(graphicsQueue, 1, &release, VK_NULL_HANDLE) != VK_SUCCESS) {
            return false;
        }
        // 2. Transfer: acquire, copy, release back to graphics.
        const VkPipelineStageFlags transferWait = VK_PIPELINE_STAGE_TRANSFER_BIT;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &batch.toTransfer;
        submit.pWaitDstStageMask = &transferWait;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &batch.toGraphics;
        if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
            replaceSignalledSemaphore(batch.toTransfer);
            return false;
        }
        // 3. Graphics: acquire before any later graphics submission; the fence covers all three.
        const VkPipelineStageFlags graphicsWait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo acquire{};
        acquire.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        acquire.waitSemaphoreCount = 1;
        acquire.pWaitSemaphores = &batch.toGraphics;
        acquire.pWaitDstStageMask = &graphicsWait;
        acquire.commandBufferCount = 1;
        acquire.pCommandBuffers = &batch.acquireCmd;
        if (vkQueueSubmit(graphicsQueue, 1, &acquire, batch.fence) != VK_SUCCESS) {
            replaceSignalledSemaphore(batch.toGraphics);
            return false;
        }
        return true;
    }
#endif

    UploadTicket flush() {
        if (openSerial == 0) {
            return UploadTicket{lastSubmittedSerial, true};
        }
        // Every in-flight slot taken (only possible with command-less entries queued behind
        // kMaxBatches submitted ones): retire the oldest first, waiting (bounded) on its fence.
        // On timeout drain the device so the slot can be reused without overwriting live data.
        if (inFlight.full() && !pollFront() && !waitFront(UploadQueue::kDefaultFenceTimeoutNs)) {
#if defined(FUSE_VULKAN_BACKEND)
            if (vkDevice != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(vkDevice);
            }
#endif
            retireFront();
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
    usize offset = 0;
    if (data == nullptr) {
        s.lastStageTimedOut = false;
        return false;
    }
    if (!s.reserve(size, offset)) {
        return false;
    }
    std::memcpy(s.mapped + offset, data, size);
    outOffset = offset;
    return true;
}

usize UploadQueue::imageSourceBytes(const UploadImageDesc& desc) {
    if (!validImageDesc(desc)) {
        return 0;
    }
    usize total = 0;
    for (u32 mip = 0; mip < desc.mipLevels; ++mip) {
        total += mipBytes(desc, mip);
    }
    return total;
}

usize UploadQueue::imageStagingBytes(const UploadImageDesc& desc) {
    if (!validImageDesc(desc)) {
        return 0;
    }
    const u32 last = desc.mipLevels - 1u;
    return stagedMipOffset(desc, last) + mipBytes(desc, last);
}

bool UploadQueue::stageImage(const void* data, const UploadImageDesc& desc, usize& outOffset) {
    Impl& s = *m_impl;
    const usize bytes = imageStagingBytes(desc);
    usize offset = 0;
    if (data == nullptr || bytes == 0) {
        s.lastStageTimedOut = false;
        return false;
    }
    if (!s.reserve(bytes, offset)) {
        return false;
    }
    // The ring offset is 256-aligned, so aligned relative mip offsets stay aligned.
    const u8* src = static_cast<const u8*>(data);
    for (u32 mip = 0; mip < desc.mipLevels; ++mip) {
        const usize size = mipBytes(desc, mip);
        std::memcpy(s.mapped + offset + stagedMipOffset(desc, mip), src, size);
        src += size;
    }
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
    const VkBuffer dst = static_cast<VkBuffer>(dstBuffer);
    if (s.crossFamily) {
        // Ownership moves once per batch; the release back is recorded at flush, after the last
        // copy (a release per copy would let a second copy write a buffer already given away).
        s.claimBuffer(*batch, dst);
    }
    s.orderAfterPreviousCopies(*batch);
    VkBufferCopy region{};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size = size;
    vkCmdCopyBuffer(batch->cmd, s.staging, dst, 1, &region);
    batch->bufferCopies = true;
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
    UploadImageDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.depth = depth > 0 ? depth : 1u;
    desc.bytesPerTexel = 1; // single mip: no per-mip offsets to align
    return recordImageCopy(dstImage, srcOffset, desc);
}

bool UploadQueue::recordImageCopy(void* dstImage, usize srcOffset, const UploadImageDesc& desc) {
    Impl& s = *m_impl;
    if (!s.ready || !s.gpu() || dstImage == nullptr || !validImageDesc(desc)) {
        return false;
    }
#if defined(FUSE_VULKAN_BACKEND)
    Batch* batch = s.beginRecording(kDefaultFenceTimeoutNs);
    if (batch == nullptr) {
        return false;
    }
    s.ensureOpenSerial();
    const VkImage image = static_cast<VkImage>(dstImage);

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = desc.mipLevels;
    range.baseArrayLayer = 0;
    range.layerCount = desc.layerCount;

    const bool transition = !s.crossFamily || s.claimImage(*batch, image, desc.mipLevels, desc.layerCount);
    const bool earlierCopies = s.openCopies > 0;
    s.orderAfterPreviousCopies(*batch);
    if (transition) {
        // UNDEFINED: the upload replaces every texel of the range. An earlier copy in this batch
        // (same image) is ordered by the barrier above; its access is repeated here for the
        // layout transition's write-after-write.
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = earlierCopies ? VkAccessFlags{VK_ACCESS_TRANSFER_WRITE_BIT} : VkAccessFlags{0};
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = image;
        toTransfer.subresourceRange = range;
        vkCmdPipelineBarrier(batch->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toTransfer);
    }

    s.imageRegions.clear();
    for (u32 mip = 0; mip < desc.mipLevels; ++mip) {
        VkBufferImageCopy region{};
        region.bufferOffset = srcOffset + stagedMipOffset(desc, mip);
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = mip;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = desc.layerCount;
        region.imageExtent = {mipExtent(desc.width, mip), mipExtent(desc.height, mip), mipExtent(desc.depth, mip)};
        s.imageRegions.push_back(region);
    }
    vkCmdCopyBufferToImage(batch->cmd, s.staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<u32>(s.imageRegions.size()), s.imageRegions.data());

    if (!s.crossFamily) {
        VkImageMemoryBarrier toShader{};
        toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = kConsumerImageAccess;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.image = image;
        toShader.subresourceRange = range;
        vkCmdPipelineBarrier(batch->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &toShader);
    }
    // Cross-family: the image stays TRANSFER_DST until the batch's release/acquire pair.
    ++s.openCopies;
    ++s.stats.recordedCopies;
    return true;
#else
    (void)srcOffset;
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
