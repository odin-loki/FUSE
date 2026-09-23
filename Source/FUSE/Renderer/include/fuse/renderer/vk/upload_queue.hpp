#pragma once

#include <fuse/renderer/vk/device.hpp>
#include <fuse/types.hpp>

#include <memory>

namespace fuse::renderer {

/// Completion token for data handed to the async upload queue (B2.11 gate 3.4).
/// `serial` is the batch that carries the copy; serials are monotonic in submission order, so a
/// ticket is complete once every batch up to and including it has signalled its fence.
/// `serial == 0` means there was nothing to wait for (host-visible destination, stub backend).
struct UploadTicket {
    u64 serial = 0;
    bool accepted = false;

    bool isValid() const { return accepted; }
};

struct UploadQueueStats {
    u64 submittedBatches = 0;
    u64 retiredBatches = 0;
    u64 recordedCopies = 0;
    u32 batchesInFlight = 0;
    u32 maxBatchesInFlight = 0;
    usize bytesInFlight = 0;
    usize maxBytesInFlight = 0;
    u32 ringWraps = 0;
    /// Allocations that had to wait for the oldest in-flight batch to retire before reusing ring space.
    u32 ringStalls = 0;
    u32 fenceTimeouts = 0;
    u32 submitFailures = 0;
    /// Copies run on a queue other than the graphics queue.
    bool dedicatedTransferQueue = false;
    /// Transfer and graphics families differ: every batch releases ownership on the transfer queue
    /// and a graphics-queue acquire submit (waiting on a semaphore) carries the fence.
    bool queueFamilyOwnershipTransfer = false;
    /// Cross-family only: destinations (whole buffers / image mip-layer ranges) handed graphics ->
    /// transfer before a batch's copies and back after them. One pair per destination per batch.
    u64 ownershipBufferTransfers = 0;
    u64 ownershipImageTransfers = 0;
};

/// Full image upload: every mip in [0, mipLevels) of every layer in [0, layerCount). Caller data is
/// tightly packed mip-major (mip 0 of layers 0..n, then mip 1 ...), uncompressed `bytesPerTexel`.
struct UploadImageDesc {
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mipLevels = 1;
    u32 layerCount = 1;
    u32 bytesPerTexel = 4;
};

/// Asynchronous staging-ring upload queue.
///
/// Callers `stage` bytes into the persistently mapped staging ring and record copies out of it;
/// copies accumulate in a transfer command buffer (dedicated transfer queue when the device has
/// one, else the graphics queue) until `flush` submits them with a fence. Ring space is retired
/// strictly in submission order and only after the batch's fence signals, so a region the GPU may
/// still read is never overwritten: when the ring is full, `stage` submits the open batch and
/// waits (bounded) on the oldest in-flight batch instead.
///
/// Visibility: on the same queue family each batch ends in a transfer-write -> all-commands read
/// barrier (images end in SHADER_READ_ONLY_OPTIMAL), so graphics work submitted after `flush`
/// sees the data without a CPU wait. Across families (graphics owns every resource at rest) a
/// batch is three submits: graphics releases each destination to the transfer family and signals
/// a semaphore (also the WAR dependency on earlier graphics work), the transfer queue waits,
/// acquires, copies, releases, signals; graphics waits and acquires (image layouts go
/// TRANSFER_DST -> SHADER_READ_ONLY once, in the matching release/acquire pair), then the fence
/// signals. Each destination is transferred once per batch, however many copies target it.
/// Work on other queues (async compute) must wait on the ticket.
///
/// Not thread-safe: one owner thread records, flushes and polls.
class UploadQueue {
public:
    static constexpr u64 kDefaultFenceTimeoutNs = 1000000000ull;
    static constexpr u32 kMaxBatches = 32;
    static constexpr usize kStagingAlignBytes = 256;

    UploadQueue();
    ~UploadQueue();

    UploadQueue(const UploadQueue&) = delete;
    UploadQueue& operator=(const UploadQueue&) = delete;

    /// `stagingBuffer` is the native VkBuffer of the ring, `stagingMapped` its persistent mapping.
    /// `device` may be null / invalid (stub backend): staging still works, batches retire at once.
    bool init(VulkanDevice* device, void* stagingBuffer, void* stagingMapped, usize capacity);
    /// Waits for all in-flight batches, then releases command pools, fences and semaphores.
    void destroy();
    bool isReady() const;

    /// Copies `size` bytes into the ring and returns their ring offset. Returns false (without
    /// touching the ring) when `size` exceeds the ring or a stall wait timed out.
    bool stage(const void* data, usize size, usize& outOffset);
    /// Record staging[srcOffset, +size) -> dstBuffer[dstOffset, +size) into the open batch.
    bool recordBufferCopy(void* dstBuffer, usize srcOffset, usize dstOffset, usize size);
    /// Record staging[srcOffset] -> mip 0 / layer 0 of a colour image; leaves it SHADER_READ_ONLY.
    bool recordImageCopy(void* dstImage, usize srcOffset, u32 width, u32 height, u32 depth);

    /// Ring bytes `stageImage` takes for `desc` (per-mip offsets aligned for transfer-only queues).
    static usize imageStagingBytes(const UploadImageDesc& desc);
    /// Tightly packed caller bytes for `desc`; 0 when `desc` is invalid.
    static usize imageSourceBytes(const UploadImageDesc& desc);
    /// Copies a tightly packed mip chain into the ring with each mip at an aligned offset.
    bool stageImage(const void* data, const UploadImageDesc& desc, usize& outOffset);
    /// Record the staged chain (from `stageImage`) into every mip/layer of `dstImage`; the whole
    /// range ends SHADER_READ_ONLY_OPTIMAL (previous contents are discarded).
    bool recordImageCopy(void* dstImage, usize srcOffset, const UploadImageDesc& desc);

    /// Ticket of the open (not yet submitted) batch; complete tickets have serial <= completedSerial.
    UploadTicket pendingTicket() const;
    /// Submit the open batch (no-op when empty). Returns the ticket of the newest batch.
    UploadTicket flush();
    /// Non-blocking poll: retires signalled batches and reports whether `ticket` completed.
    bool isComplete(UploadTicket ticket);
    /// Flushes if `ticket` is still open, then waits (bounded) for its fence.
    bool wait(UploadTicket ticket, u64 timeoutNs = kDefaultFenceTimeoutNs);
    bool waitAll(u64 timeoutNs = kDefaultFenceTimeoutNs);
    /// Poll fences in submission order and release their ring space; returns batches retired.
    u32 retireCompleted();

    bool hasPendingWork() const;
    u64 completedSerial() const;
    usize ringHead() const;
    usize ringCapacity() const;
    usize ringBytesInUse() const;
    const UploadQueueStats& stats() const;
    /// True when the most recent stage() had to wait and that wait timed out.
    bool lastStageTimedOut() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fuse::renderer
