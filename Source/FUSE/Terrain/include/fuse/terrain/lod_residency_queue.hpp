#pragma once

#include <fuse/terrain/terrain_desc.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <mutex>
#include <vector>

namespace fuse::terrain {

enum class LodResidencyRequestKind : u8 {
    Load,
    Unload,
};

/// LOD/morph snapshot captured when a residency request is queued (keeps async I/O in sync).
struct LodResidencyMorphSnapshot {
    u32 lod = 0;
    f32 morph_factor = 0.f;
};

/// One async chunk load/unload request submitted to the worker pool.
struct LodResidencyRequest {
    u32 chunk_index = 0;
    LodResidencyRequestKind kind = LodResidencyRequestKind::Load;
    f32 priority = 0.f;
    LodResidencyMorphSnapshot morph_snapshot{};
};

/// Completed request drained on the game thread after JobScheduler work finishes.
struct CompletedLodResidencyRequest {
    u32 chunk_index = 0;
    LodResidencyRequestKind kind = LodResidencyRequestKind::Load;
    f32 priority = 0.f;
    bool success = true;
    LodResidencyMorphSnapshot morph_snapshot{};
};

/// Raise pending priority when camera focus moves closer (stub heuristic).
[[nodiscard]] f32 promote_residency_priority(f32 current, f32 incoming);

/// Lower pending priority when focus moves away (scale clamped to [0, 1]).
[[nodiscard]] f32 demote_residency_priority(f32 current, f32 scale);

/// Capture a clamped LOD/morph snapshot for residency queue handoff.
[[nodiscard]] LodResidencyMorphSnapshot capture_morph_snapshot(u32 lod, f32 morph_factor);

/// Apply a completion snapshot when LOD promotion/demotion finishes (stub sync helper).
void sync_morph_after_residency(TerrainChunk& chunk, const LodResidencyMorphSnapshot& snapshot);

/// Worker-side mesh/heightfield I/O stub — production wiring reads chunk assets from disk.
using LodResidencyWorkFn = std::function<bool(u32 chunk_index, LodResidencyRequestKind kind)>;

/// Async LOD residency queue backed by JobScheduler (mirrors B7.6 StreamingRequestQueue).
class LodResidencyQueue {
public:
    /// Queue a request for later submission. Promotes priority when the same chunk/kind is already pending.
    bool enqueue(LodResidencyRequest request);

    /// Lower priority for a pending request (returns false when not found).
    bool demote(u32 chunk_index, LodResidencyRequestKind kind, f32 scale);

    /// Submit up to `budget` highest-priority pending requests. Returns the number submitted.
    u32 flush(u32 budget, LodResidencyWorkFn work);

    /// Submit work to JobScheduler. Returns false when the scheduler is unavailable or
    /// `max_pending_submits` would be exceeded.
    bool submit(LodResidencyRequest request, LodResidencyWorkFn work);

    /// Move completed requests into `out` (highest priority first) and clear the buffer.
    u32 drain_completed(std::vector<CompletedLodResidencyRequest>& out);

    void set_max_pending_submits(u32 max_pending) { m_max_pending_submits = max_pending; }
    [[nodiscard]] u32 max_pending_submits() const { return m_max_pending_submits; }
    void set_max_async_in_flight(u32 max_in_flight) { m_max_async_in_flight = max_in_flight; }
    [[nodiscard]] u32 max_async_in_flight() const { return m_max_async_in_flight; }
    [[nodiscard]] u32 pending_enqueue_count() const;
    [[nodiscard]] u32 pending_submit_count() const;

    [[nodiscard]] u32 in_flight_count() const;
    [[nodiscard]] u32 completed_count() const;
    [[nodiscard]] bool empty() const;

    void clear();

private:
    [[nodiscard]] bool would_exceed_budget_() const;
    [[nodiscard]] bool would_exceed_async_in_flight_() const;
    void push_completed_(CompletedLodResidencyRequest completed);

    mutable std::mutex m_mutex;
    u32 m_inFlight = 0;
    u32 m_max_async_in_flight = 0; ///< 0 = unlimited concurrent in-flight submissions
    u32 m_max_pending_submits = 0; ///< 0 = unlimited pending (in-flight + completed buffer)
    std::vector<LodResidencyRequest> m_pending;
    std::vector<CompletedLodResidencyRequest> m_completed;
};

} // namespace fuse::terrain
