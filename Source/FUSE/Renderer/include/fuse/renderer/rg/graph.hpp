#pragma once

// Render graph v2 compiler (WP-0.3). Vulkan-free: builds and unit-tests in the stub backend.
//
//   rg::Graph g;                                   // reuse one instance: reset() keeps capacity
//   g.reset();
//   rg::TextureRef hdr = g.createImage({...});     // transient, placed in aliased memory
//   rg::BufferRef out = g.importBuffer({vkBuffer, size});
//   g.addPass("light", &lightFn, &state, rg::QueueClass::AsyncCompute)
//       .use(hdr, rg::Access::StorageWrite)
//       .use(out, rg::Access::StorageRead, {0, 256});
//   executor.execute(g);                           // rg/executor.hpp: compile + realize + submit
//
// Execution order is declaration order (a pass may only depend on passes declared before it).
// compile() culls passes whose writes nobody observes, resolves queue classes against the device,
// splits the order into per-queue submission batches, and computes resource lifetimes. plan()
// then derives, from the declared accesses alone, per-pass synchronization2 barrier batches
// (stage/access masks, image layouts per subresource range, buffer byte ranges), queue-family
// ownership release/acquire pairs and the cross-queue timeline waits. No pass records a barrier by
// hand.
//
// Hot-path storage is a set of vectors that are cleared, never shrunk: steady-state frames with
// the same shape do not touch the heap (B2.11). There is no pass cap.

#include <fuse/renderer/rg/rg_types.hpp>

#include <vector>

namespace fuse::renderer::rg {

class Graph;
class Executor;

/// Handed to every pass callback.
struct PassContext {
    void* commandBuffer = nullptr; ///< VkCommandBuffer (null when compiled without an executor)
    QueueClass queue = QueueClass::Graphics;
    u32 passIndex = 0;
    const Graph* graph = nullptr;
    const Executor* executor = nullptr;

    void* image(TextureRef ref) const;     ///< VkImage
    void* imageView(TextureRef ref) const; ///< default full-range VkImageView (transients; imported: ImportedImage::view)
    void* buffer(BufferRef ref) const;     ///< VkBuffer
};

using PassFn = void (*)(const PassContext& context, void* user);

struct CompileOptions {
    /// Queue classes with a queue distinct from Graphics. Others fall back to Graphics.
    bool asyncComputeAvailable = false;
    bool transferAvailable = false;
    /// Put every pass on this queue class (kNoQueue = per-pass request). Used for inline recording
    /// into one caller command buffer.
    u8 forceQueue = kNoQueue;
};

/// One image barrier (or one half of an ownership transfer). Numeric Vulkan values.
struct ImageBarrier {
    u32 resource = 0; ///< TextureRef::id
    u64 srcStages = 0;
    u64 srcAccess = 0;
    u64 dstStages = 0;
    u64 dstAccess = 0;
    u32 oldLayout = 0;
    u32 newLayout = 0;
    u8 srcQueue = kNoQueue; ///< queue classes of an ownership transfer; kNoQueue = none
    u8 dstQueue = kNoQueue;
    u32 baseMip = 0;
    u32 mipCount = 1;
    u32 baseLayer = 0;
    u32 layerCount = 1;
};

struct BufferBarrier {
    u32 resource = 0; ///< BufferRef::id
    u64 srcStages = 0;
    u64 srcAccess = 0;
    u64 dstStages = 0;
    u64 dstAccess = 0;
    u8 srcQueue = kNoQueue;
    u8 dstQueue = kNoQueue;
    u64 offset = 0;
    u64 size = 0;
};

/// Slices of Graph::imageBarriers() / bufferBarriers() recorded as one vkCmdPipelineBarrier2.
struct BarrierRange {
    u32 imageBegin = 0;
    u32 imageCount = 0;
    u32 bufferBegin = 0;
    u32 bufferCount = 0;
    bool empty() const { return imageCount == 0u && bufferCount == 0u; }
};

/// Consecutive passes on one queue: one command buffer, one vkQueueSubmit, one timeline signal.
struct Batch {
    QueueClass queue = QueueClass::Graphics;
    u32 orderBegin = 0; ///< range in executionOrder()
    u32 orderCount = 0;
    /// Latest batch index on each queue class this batch waits for (-1 = none). Always < own index.
    i32 waitBatch[kQueueClassCount] = {-1, -1, -1};
    /// Ownership releases and final-layout transitions recorded after the last pass.
    BarrierRange post{};
    /// Prologue batch (no passes): releases imported resources owned by this queue at graph start.
    bool prologue = false;
};

struct CompileStats {
    u32 passCount = 0;
    u32 executedPasses = 0;
    u32 culledPasses = 0;
    u32 batchCount = 0;
    u32 imageBarriers = 0;
    u32 bufferBarriers = 0;
    u32 passesWithBarriers = 0;
    u32 ownershipTransfers = 0; ///< release/acquire pairs
    u32 crossQueueWaits = 0;
    u32 queueFallbacks = 0;     ///< passes moved to Graphics (queue unavailable or access unsupported)
    u32 transientImages = 0;
    u32 transientBuffers = 0;
    /// Same subresource declared with incompatible layouts inside one pass (resolved to GENERAL).
    u32 layoutConflicts = 0;
    u32 compileUs = 0;
    bool compiled = false;
    bool planned = false;
};

struct Lifetime {
    u32 first = UINT32_MAX; ///< position in executionOrder()
    u32 last = UINT32_MAX;
};

class Graph;

/// Fluent access declaration for the pass most recently added.
class PassBuilder {
public:
    PassBuilder& use(TextureRef texture, Access access, ImageRange range = {}, u8 shaderStages = 0);
    PassBuilder& use(BufferRef buffer, Access access, BufferRange range = {}, u8 shaderStages = 0);
    /// Keep the pass even when nothing observes its writes (side effects outside the graph).
    PassBuilder& neverCull();
    u32 index() const { return m_pass; }

private:
    friend class Graph;
    PassBuilder(Graph* graph, u32 pass) : m_graph(graph), m_pass(pass) {}
    Graph* m_graph = nullptr;
    u32 m_pass = 0;
};

class Graph {
public:
    Graph() = default;

    /// Start a new graph; keeps every internal vector's capacity.
    void reset();

    TextureRef importImage(const ImportedImage& image);
    BufferRef importBuffer(const ImportedBuffer& buffer);
    TextureRef createImage(const ImageDesc& desc);
    BufferRef createBuffer(const BufferDesc& desc);

    PassBuilder addPass(const char* name, PassFn fn, void* user, QueueClass queue = QueueClass::Graphics);

    /// Cull, resolve queues, form batches, compute lifetimes and derived usage. Returns false for
    /// an empty or malformed graph (invalid refs are counted and ignored).
    bool compile(const CompileOptions& options = {});
    /// Derive the barrier batches, ownership transfers and cross-queue waits (after compile(); the
    /// executor assigns transient alias slots in between).
    bool plan();

    // --- results --------------------------------------------------------------------------------
    const CompileStats& stats() const { return m_stats; }
    u32 passCount() const { return static_cast<u32>(m_passes.size()); }
    const char* passName(u32 pass) const { return m_passes[pass].name; }
    QueueClass passQueue(u32 pass) const { return m_passes[pass].queue; }
    bool passCulled(u32 pass) const { return m_passes[pass].culled; }
    u32 passBatch(u32 pass) const { return m_passes[pass].batch; }
    const BarrierRange& passBarriers(u32 pass) const { return m_passes[pass].pre; }
    /// Layout transitions recorded right after passBarriers() of `pass` as a second call: the image
    /// half of an ownership transfer keeps its layout (release and acquire), the new layout is
    /// applied on the acquiring queue afterwards. Indices into lateImageBarriers().
    const BarrierRange& passLateBarriers(u32 pass) const { return m_passes[pass].late; }
    const std::vector<ImageBarrier>& lateImageBarriers() const { return m_lateImageBarriers; }
    /// Non-culled pass indices in execution order.
    const std::vector<u32>& executionOrder() const { return m_order; }
    const std::vector<Batch>& batches() const { return m_batches; }
    const std::vector<ImageBarrier>& imageBarriers() const { return m_imageBarriers; }
    const std::vector<BufferBarrier>& bufferBarriers() const { return m_bufferBarriers; }
    Lifetime lifetime(TextureRef ref) const;
    Lifetime lifetime(BufferRef ref) const;
    u32 resourceCount() const { return static_cast<u32>(m_resources.size()); }

private:
    friend class PassBuilder;
    friend class Executor;
    friend struct PassContext;

    struct Use {
        u32 resource = 0; ///< index into m_resources
        Access access = Access::None;
        u8 shaderStages = 0;
        bool image = false;
        ImageRange imageRange{};
        BufferRange bufferRange{};
    };

    struct Pass {
        const char* name = nullptr;
        PassFn fn = nullptr;
        void* user = nullptr;
        QueueClass requested = QueueClass::Graphics;
        QueueClass queue = QueueClass::Graphics;
        u32 useBegin = 0;
        u32 useCount = 0;
        bool neverCull = false;
        bool culled = false;
        u32 batch = 0;
        BarrierRange pre{};
        BarrierRange late{};
    };

    struct Resource {
        bool image = true;
        bool imported = false;
        u32 format = 0;
        u32 width = 1, height = 1, depth = 1, mips = 1, layers = 1;
        u64 size = 0;
        void* handle = nullptr; ///< VkImage / VkBuffer: imported, or bound by the executor
        void* view = nullptr;
        u32 initialLayout = 0;
        u32 finalLayout = UINT32_MAX;
        u32* layoutTracker = nullptr;
        u8* queueTracker = nullptr;
        u8 initialQueue = kNoQueue;
        u32 extraUsage = 0;
        u32 usage = 0; ///< derived VkImageUsageFlags / VkBufferUsageFlags
        const char* name = nullptr;
        Lifetime life{};
        u32 aliasSlot = UINT32_MAX; ///< executor-assigned transient memory slot
        u32 aliasPrev = UINT32_MAX; ///< previous occupant of the slot in execution order
        u32 stateBase = 0;
        u32 stateCount = 0;
    };

    /// Per image subresource / per buffer hazard state during plan().
    struct SubState {
        u64 writeStages = 0; ///< last write (or the chain stage of a layout transition)
        u64 writeAccess = 0;
        u64 readStages = 0; ///< reads since the last write
        u64 visStages[2] = {0, 0};
        u64 visAccess[2] = {0, 0};
        u64 rangeLo = 0; ///< buffers: bounding range of tracked accesses
        u64 rangeHi = 0;
        i32 lastBatch[kQueueClassCount] = {-1, -1, -1};
        u32 layout = 0;
        u32 lastPass = UINT32_MAX;
        u8 queue = kNoQueue;
        bool contents = false; ///< holds data an ownership transfer must preserve
        bool touched = false;
    };

    /// One pending barrier computed for a subresource (coalesced into ranges before emission).
    struct Pending {
        u64 srcStages = 0, srcAccess = 0, dstStages = 0, dstAccess = 0;
        u32 oldLayout = 0, newLayout = 0;
        u8 srcQueue = kNoQueue, dstQueue = kNoQueue;
        i32 releaseBatch = -1; ///< ownership transfer: batch whose post list gets the release
        bool operator==(const Pending& o) const;
    };

    struct PostBarrier {
        u32 batch = 0;
        bool image = true;
        ImageBarrier imageBarrier{};
        BufferBarrier bufferBarrier{};
    };

    u32 addUse(u32 pass, const Use& use);
    void resetStates();
    bool computePending(const Resource& resource, SubState& state, const Use& use, u32 orderPos, u32 batchIndex,
                        u64 dstStages, u64 dstAccess, u32 dstLayout, bool write, Pending& out);
    void planUse(const Use& use, u32 orderPos, u32 passIndex);
    void emitImage(u32 resourceId, const Pending& pending, u32 mip, u32 mipCount, u32 layer, u32 layerCount,
                   u32 preStart);
    void emitFinalTransitions();
    void flushPostBarriers();
    Resource* resourceFor(TextureRef ref);
    Resource* resourceFor(BufferRef ref);

    CompileOptions m_options{};
    CompileStats m_stats{};
    std::vector<Pass> m_passes;
    std::vector<Use> m_uses;
    std::vector<Resource> m_resources;
    std::vector<u32> m_order;
    std::vector<Batch> m_batches;
    std::vector<ImageBarrier> m_imageBarriers;
    std::vector<ImageBarrier> m_lateImageBarriers;
    std::vector<BufferBarrier> m_bufferBarriers;
    std::vector<PostBarrier> m_post;
    std::vector<SubState> m_states;
    // compile()/plan() scratch — cleared, never shrunk.
    std::vector<u8> m_scratchNeeded;
    std::vector<i32> m_scratchPrologue;
    std::vector<Pending> m_scratchPending;
    u32 m_invalidRefs = 0;
};

} // namespace fuse::renderer::rg
