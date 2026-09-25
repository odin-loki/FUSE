// Render-graph compiler model (WP-0.8): pure logic, no Vulkan types.
//
// Input : resources (size, alignment, imported/transient, initial state) and passes in
//         submission order, each on a requested queue class with a list of accesses
//         (resource + abstract state; the state implies read or write).
// Output: 1. an ordered barrier list (grouped by the pass each barrier precedes),
//         2. a transient aliasing plan (first-fit offsets in one heap; two transients share
//            bytes only if every use of one happens-before every use of the other),
//         3. queue assignment (with fallback when a queue class is missing), per-queue
//            timeline values and the cross-queue sync points (timeline waits) required by
//            every barrier whose source pass runs on another queue.
//
// Intended consumer: WP-0.3 render graph v2 maps RgState -> (stage, access, layout) and each
// RgBarrier -> one VkImageMemoryBarrier2/VkBufferMemoryBarrier2 in that pass's
// VkDependencyInfo; each RgSyncPoint -> a timeline-semaphore wait before `waitBeforePass`
// on `dstQueue` for value `waitValue` of `srcQueue`'s semaphore (the source queue signals
// passTimelineValue[p] after every pass p with passSignals[p] != 0). A barrier with
// crossQueue != 0 on an EXCLUSIVE resource also needs a queue-family ownership transfer.
//
// API surface (keep small; WP-0.3 depends on it):
//   RgGraphDesc, RgCompileResult, rg_compile(), rg_state_is_write().
//
// Semantics of a barrier (what the oracle in tests/test_core_logic_rg.cpp checks): a barrier
// on resource r placed before pass d (on queue q(d)) makes every pass in srcPassMask
// happen-before every later pass e >= d on q(d) that accesses r in dstState, up to the next
// barrier on r on that queue. Cross-queue sources are only valid together with a sync point.
//
// Capacities can be lowered for bounded model checking by defining the FUSE_CL_RG_* macros
// before including this header (the BMC harness does this). kRgMaxPasses must be <= 64
// (pass sets are uint64_t masks).
#pragma once

#include "fuse/core_logic/cl_common.hpp"

#ifndef FUSE_CL_RG_MAX_PASSES
#define FUSE_CL_RG_MAX_PASSES 64u
#endif
#ifndef FUSE_CL_RG_MAX_RESOURCES
#define FUSE_CL_RG_MAX_RESOURCES 64u
#endif
#ifndef FUSE_CL_RG_MAX_ACCESSES_PER_PASS
#define FUSE_CL_RG_MAX_ACCESSES_PER_PASS 8u
#endif

namespace fuse {
namespace core_logic {

static constexpr uint32_t kRgMaxPasses = FUSE_CL_RG_MAX_PASSES;
static constexpr uint32_t kRgMaxResources = FUSE_CL_RG_MAX_RESOURCES;
static constexpr uint32_t kRgMaxAccessesPerPass = FUSE_CL_RG_MAX_ACCESSES_PER_PASS;
static constexpr uint32_t kRgQueueCount = 3u;
static constexpr uint32_t kRgMaxBarriers = kRgMaxPasses * kRgMaxAccessesPerPass;
static constexpr uint32_t kRgMaxSyncPoints = kRgMaxPasses * (kRgQueueCount - 1u);
static constexpr uint64_t kRgNoOffset = 0xFFFFFFFFFFFFFFFFull;

FUSE_CL_STATIC_ASSERT(kRgMaxPasses >= 1u && kRgMaxPasses <= 64u, "pass sets are uint64 masks");

enum class RgQueue : uint8_t { Graphics = 0, AsyncCompute = 1, Transfer = 2 };

// Abstract resource states. Write states: ColorAttachment, DepthStencilWrite, ShaderWrite,
// TransferDst. Everything else is a read. Undefined is only valid as an initial state.
enum class RgState : uint8_t {
    Undefined = 0,
    ColorAttachment,
    DepthStencilWrite,
    DepthStencilRead,
    ShaderRead,
    ShaderWrite,
    TransferSrc,
    TransferDst,
    IndirectArgs,
    Present,
    Count
};

bool rg_state_is_write(RgState s);

enum class RgBarrierKind : uint8_t {
    Initial = 0,         // first use: initial/undefined -> dstState (srcPassMask == 0)
    Alias,               // first use of an aliased transient; srcPassMask = previous tenants' passes on this queue
    ReadAfterWrite,
    WriteAfterRead,
    WriteAfterWrite,
    LayoutTransition,    // read -> read in a different state (srcPassMask = readers)
};

struct RgResourceDesc {
    uint64_t sizeBytes;     // transient only (> 0)
    uint64_t alignment;     // transient only (power of two)
    RgState initialState;   // imported only; transients start Undefined
    uint8_t imported;       // 1 = external (never aliased), 0 = transient
};

struct RgAccess {
    uint32_t resource;
    RgState state;          // must not be Undefined / Count
};

struct RgPassDesc {
    RgQueue queue;          // requested queue class
    uint32_t accessCount;
    RgAccess accesses[kRgMaxAccessesPerPass]; // one entry per resource per pass
};

struct RgQueueConfig {
    uint8_t hasAsyncCompute;  // else AsyncCompute passes run on Graphics
    uint8_t hasTransfer;      // else Transfer passes run on AsyncCompute (if present) or Graphics
};

struct RgGraphDesc {
    uint32_t resourceCount;
    uint32_t passCount;
    RgQueueConfig queues;
    RgResourceDesc resources[kRgMaxResources];
    RgPassDesc passes[kRgMaxPasses];
};

struct RgBarrier {
    uint32_t resource;
    uint32_t beforePass;     // barrier is recorded on passQueue[beforePass] just before it
    uint64_t srcPassMask;    // passes that must complete (bit i = pass i)
    RgState srcState;
    RgState dstState;
    RgBarrierKind kind;
    uint8_t crossQueue;      // some src pass runs on another queue (sync point + QFOT)
};

struct RgSyncPoint {
    RgQueue srcQueue;
    RgQueue dstQueue;
    uint32_t waitValue;      // timeline value of srcQueue (== passTimelineValue of the signalling pass)
    uint32_t waitBeforePass; // pass on dstQueue that waits
};

struct RgResourcePlan {
    uint32_t firstUse;       // kClInvalid when never used
    uint32_t lastUse;
    uint64_t usersMask;
    uint64_t heapOffset;     // transient + used only, else kRgNoOffset
    uint32_t aliasPredecessor; // latest previous tenant of overlapping bytes, or kClInvalid
    uint32_t initialBarrier; // index into barriers[] or kClInvalid
};

struct RgCompileResult {
    ClStatus status;
    uint32_t errorPass;      // on InvalidArgument: offending pass (or kClInvalid)
    uint32_t errorResource;  // on InvalidArgument: offending resource (or kClInvalid)

    uint32_t barrierCount;
    RgBarrier barriers[kRgMaxBarriers];      // sorted by beforePass
    uint32_t passBarrierBegin[kRgMaxPasses];
    uint32_t passBarrierCount[kRgMaxPasses];

    uint32_t syncPointCount;
    RgSyncPoint syncPoints[kRgMaxSyncPoints]; // sorted by waitBeforePass

    RgQueue passQueue[kRgMaxPasses];
    uint32_t passTimelineValue[kRgMaxPasses]; // 1-based index of the pass on its queue
    uint8_t passSignals[kRgMaxPasses];        // 1 = signal passTimelineValue after this pass
    uint64_t happensBefore[kRgMaxPasses];     // bit j: pass i happens-before pass j (same-queue order + sync points)

    RgResourcePlan resources[kRgMaxResources];
    uint64_t transientHeapBytes;
};

// Compiles `graph` into `out`. Deterministic, no allocation. Returns out.status.
ClStatus rg_compile(const RgGraphDesc& graph, RgCompileResult& out);

} // namespace core_logic
} // namespace fuse
