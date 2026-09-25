#pragma once

// Render graph v2 executor (WP-0.3): the Vulkan half of rg::Graph.
//
//  * Resource table: every TextureRef / BufferRef resolves to a real VkImage / VkBuffer —
//    imported handles as given, transients created by the executor with the usage the graph
//    derived from their declared accesses, plus a default full-range VkImageView per transient
//    image (PassContext::imageView).
//  * Transient heap: transients whose lifetimes (execution-order intervals) do not overlap share
//    one VMA allocation (vmaAllocateMemory + vmaBindImageMemory / vmaBindBufferMemory — memory
//    aliasing); the graph orders each occupant's first use after the previous one. Physical
//    transients are cached across frames: a frame with the same transient signature reuses them
//    without touching the heap or Vulkan allocation (B2.11).
//  * Barriers: per pass, one vkCmdPipelineBarrier2 (VkDependencyInfo with image, buffer barriers)
//    built from Graph::passBarriers(); synchronization1 vkCmdPipelineBarrier with the same masks
//    when the device has not enabled synchronization2 (the WP-0.1 caps decide).
//  * Queues: Graphics / AsyncCompute / Transfer map to the device's queues; one command buffer and
//    one vkQueueSubmit per Graph batch, a timeline semaphore per queue class (retirement, frame
//    pacing, cross-frame ordering: every queue's first batch of a frame waits for the other
//    queues' previous frame), the graph's cross-queue edges as binary semaphores (or timeline
//    waits, ExecutorDesc::timelineCrossQueueWaits), queue-family ownership release/acquire
//    barriers for EXCLUSIVE resources.
//  * Debug labels: vkCmdBegin/EndDebugUtilsLabelEXT around every pass when VK_EXT_debug_utils
//    is enabled on the instance.

#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {
class GpuAllocator;
}

namespace fuse::renderer::rg {

struct ExecutorDesc {
    /// Command buffers ring; a frame slot is reused only after its previous submission retired.
    u32 framesInFlight = 2;
    bool enableAsyncCompute = true;
    bool enableTransferQueue = true;
    /// false: every transient gets its own allocation (A/B reference for the aliasing gate).
    bool enableAliasing = true;
    /// false: never call vkCmdPipelineBarrier2 even when synchronization2 is enabled.
    bool allowSync2 = true;
    /// Intra-frame cross-queue edges: false (default) = one binary semaphore per edge, which the
    /// synchronization validation of every layer version follows (1.3.275 on Ubuntu 24.04 does not
    /// track timeline waits); true = timeline waits on the producer batch's value. Timelines always
    /// carry retirement, frame pacing and the cross-frame ordering.
    bool timelineCrossQueueWaits = false;
    /// Test-only negative control: record no barriers at all (sync validation must then fire).
    bool debugSkipBarriers = false;
    const char* name = "fuse.rg";
};

struct SubmitDesc {
    /// Binary semaphores (VkSemaphore): waited by the first graphics batch / signalled by the last.
    void* waitSemaphore = nullptr;
    u64 waitStages = 0x10000; ///< VkPipelineStageFlags2 (ALL_COMMANDS)
    void* signalSemaphore = nullptr;
    /// VkFence signalled by the frame's last submission.
    void* fence = nullptr;
};

struct ExecuteResult {
    bool ok = false;
    u32 executedPasses = 0;
    u32 submissions = 0;
    u32 commandBuffers = 0;
    u32 barrierCalls = 0; ///< vkCmdPipelineBarrier[2] calls recorded
    u32 imageBarriers = 0;
    u32 bufferBarriers = 0;
    u32 debugLabels = 0;
    u32 timelineWaits = 0;
    u32 binaryWaits = 0; ///< intra-frame cross-queue edges carried by binary semaphores
    /// Timeline value signalled per queue class this frame (0 = queue unused).
    u64 signalled[kQueueClassCount] = {0, 0, 0};
    bool transientsRebuilt = false;
};

struct TransientStats {
    u32 transients = 0;       ///< physical transient resources
    u32 allocations = 0;      ///< device memory blocks backing them
    u32 aliasedResources = 0; ///< transients sharing a block with an earlier occupant
    u64 bytesRequired = 0;    ///< sum of the transients' memory requirements
    u64 bytesAllocated = 0;   ///< sum of the blocks
    u32 rebuilds = 0;
    /// vmaCalculateStatistics().total.statistics.allocationCount delta caused by the transient
    /// heap (0 on the native allocator path).
    u32 vmaAllocationDelta = 0;
};

struct TransientMemory {
    void* deviceMemory = nullptr; ///< VkDeviceMemory
    u64 offset = 0;
    u64 size = 0;
    u32 slot = UINT32_MAX;
};

/// Optional hooks around each pass (GPU zones / timestamps, WP-0.6).
struct PassHooks {
    void (*begin)(const PassContext& context, const char* name, void* user) = nullptr;
    void (*end)(const PassContext& context, const char* name, void* user) = nullptr;
    void* user = nullptr;
};

class Executor {
public:
    static std::unique_ptr<Executor> create(VulkanDevice& device, GpuAllocator* allocator = nullptr,
                                            const ExecutorDesc& desc = {});
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    bool isValid() const { return m_valid; }
    const std::string& message() const { return m_message; }
    bool sync2() const { return m_sync2; }
    bool debugLabels() const { return m_labels; }
    bool queueAvailable(QueueClass queue) const;
    u32 queueFamily(QueueClass queue) const;
    CompileOptions compileOptions() const;

    /// compile + realize transients + plan. Called by execute(); public for inspection.
    /// `forceQueue` != kNoQueue puts every pass on that queue class (inline recording).
    bool prepare(Graph& graph, u8 forceQueue = kNoQueue);

    /// Record every batch into its own command buffer and submit them in order with timeline
    /// signals/waits. Non-blocking; waitIdle() or the returned timeline values retire the frame.
    ExecuteResult execute(Graph& graph, const SubmitDesc& submit = {});

    /// Record the whole graph (every pass forced onto `queue`, whose family the caller's command
    /// buffer belongs to) into a caller command buffer that is in the recording state; the caller
    /// submits it on that queue. Transients are supported (cached like execute()).
    ExecuteResult recordInline(Graph& graph, void* commandBuffer, QueueClass queue = QueueClass::Graphics);

    /// Block until everything this executor submitted has completed.
    bool waitIdle();

    const TransientStats& transientStats() const { return m_transientStats; }
    bool transientMemory(const Graph& graph, TextureRef ref, TransientMemory& out) const;
    bool transientMemory(const Graph& graph, BufferRef ref, TransientMemory& out) const;
    void setPassHooks(const PassHooks& hooks) { m_hooks = hooks; }

private:
    Executor() = default;
    bool initialize(VulkanDevice& device, GpuAllocator* allocator, const ExecutorDesc& desc);
    void shutdown();
    bool realizeTransients(Graph& graph);
    void destroyTransients();
    void recordBatchPasses(Graph& graph, u32 batchIndex, void* commandBuffer, ExecuteResult& result);
    void recordBarriers(const Graph& graph, const BarrierRange& range, void* commandBuffer, ExecuteResult& result,
                        bool late = false);
    bool beginFrameSlot();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    VulkanDevice* m_device = nullptr;
    ExecutorDesc m_desc{};
    PassHooks m_hooks{};
    TransientStats m_transientStats{};
    std::string m_message;
    bool m_valid = false;
    bool m_sync2 = false;
    bool m_labels = false;
};

} // namespace fuse::renderer::rg
