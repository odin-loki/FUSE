#pragma once

#include <fuse/terrain/lod_residency_budget.hpp>
#include <fuse/terrain/lod_residency_set.hpp>
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
    u64 submit_sequence = 0; ///< FIFO tie-break when priorities match
    bool success = true;
    LodResidencyMorphSnapshot morph_snapshot{};
};

/// Raise pending priority when camera focus moves closer (stub heuristic).
[[nodiscard]] f32 promote_residency_priority(f32 current, f32 incoming);

/// Lower pending priority when focus moves away (scale clamped to [0, 1]).
[[nodiscard]] f32 demote_residency_priority(f32 current, f32 scale);

/// Priority ordering: higher priority first, unload before load at equal priority, FIFO tie-break.
[[nodiscard]] int compare_residency_request_order(f32 priority_a, LodResidencyRequestKind kind_a, u64 sequence_a,
                                                  f32 priority_b, LodResidencyRequestKind kind_b, u64 sequence_b);

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

    /// Remove the highest-priority pending request into `out`. Returns false when the pending queue is empty.
    bool dequeue(LodResidencyRequest& out);

    /// Lower priority for a pending request (returns false when not found).
    bool demote(u32 chunk_index, LodResidencyRequestKind kind, f32 scale);

    /// Submit up to `budget` highest-priority pending requests. Returns the number submitted.
    u32 flush(u32 budget, LodResidencyWorkFn work);

    /// Submit work to JobScheduler. Returns false when the scheduler is unavailable or
    /// `max_pending_submits` would be exceeded.
    bool submit(LodResidencyRequest request, LodResidencyWorkFn work);

    /// True when at least one request is waiting in the pending enqueue buffer.
    [[nodiscard]] bool has_pending_enqueue() const;

    /// Copy the highest-priority pending request into `out` without removing it. Returns false when empty.
    [[nodiscard]] bool peek_pending(LodResidencyRequest& out) const;

    /// Move completed requests into `out` (highest priority first) and clear the buffer.
    u32 drain_completed(std::vector<CompletedLodResidencyRequest>& out);

    void set_max_pending_submits(u32 max_pending) { m_max_pending_submits = max_pending; }
    [[nodiscard]] u32 max_pending_submits() const { return m_max_pending_submits; }
    [[nodiscard]] u32 pending_enqueue_count() const;
    [[nodiscard]] u32 pending_submit_count() const;
    /// True when a pending request exists for chunk+kind.
    [[nodiscard]] bool has_pending_for(u32 chunk_index, LodResidencyRequestKind kind) const;
    /// Pending priority for chunk/kind, or -1 when not enqueued.
    [[nodiscard]] f32 pending_priority_for(u32 chunk_index, LodResidencyRequestKind kind) const;

    [[nodiscard]] u32 in_flight_count() const;
    [[nodiscard]] u32 completed_count() const;
    [[nodiscard]] bool empty() const;

    void clear();

private:
    struct PendingLodResidencyRequest {
        LodResidencyRequest request{};
        u64 enqueue_sequence = 0;
    };

    [[nodiscard]] bool would_exceed_budget_() const;
    void sort_pending_by_priority_();
    void push_completed_(CompletedLodResidencyRequest completed);

    mutable std::mutex m_mutex;
    u32 m_inFlight = 0;
    u64 m_submit_sequence = 0;
    u64 m_enqueue_sequence = 0;
    u32 m_max_pending_submits = 0; ///< 0 = unlimited pending (in-flight + completed buffer)
    std::vector<PendingLodResidencyRequest> m_pending;
    std::vector<CompletedLodResidencyRequest> m_completed;
};

/// Empty-queue guard: true when the pending enqueue buffer has at least one request.
[[nodiscard]] inline bool has_pending_enqueue(const LodResidencyQueue& queue) {
    return queue.has_pending_enqueue();
}

/// Async-submit guard: true when pending/in-flight counts are below `max_pending_submits`.
[[nodiscard]] inline bool can_submit_residency_request(const LodResidencyQueue& queue) {
    return can_submit_pending_request(queue.in_flight_count(), queue.completed_count(),
                                      queue.max_pending_submits());
}

/// Dequeue helper: removes highest-priority pending request when non-empty.
[[nodiscard]] inline bool try_dequeue_pending(LodResidencyQueue& queue, LodResidencyRequest& out) {
    if (!queue.has_pending_enqueue()) {
        return false;
    }
    return queue.dequeue(out);
}

/// Peek helper: copies highest-priority pending request without removing it.
[[nodiscard]] inline bool peek_highest_pending(const LodResidencyQueue& queue, LodResidencyRequest& out) {
    return queue.peek_pending(out);
}

/// Peek helper: returns the highest pending priority, or -1 when the pending queue is empty.
[[nodiscard]] inline f32 peek_highest_pending_priority(const LodResidencyQueue& queue) {
    LodResidencyRequest peeked{};
    return peek_highest_pending(queue, peeked) ? peeked.priority : -1.f;
}

/// Dequeue helper: removes the highest-priority pending request only when its priority is at least `min_priority`.
[[nodiscard]] inline bool try_dequeue_pending_if(LodResidencyQueue& queue, f32 min_priority,
                                                 LodResidencyRequest& out) {
    if (!peek_highest_pending(queue, out) || out.priority < min_priority) {
        return false;
    }
    return try_dequeue_pending(queue, out);
}

/// Pending-queue guard: true when a pending request exists for chunk+kind.
[[nodiscard]] inline bool has_pending_for(const LodResidencyQueue& queue, u32 chunk_index,
                                          LodResidencyRequestKind kind) {
    return queue.has_pending_for(chunk_index, kind);
}

/// Guard: pending priority for chunk/kind, or -1 when chunk index is invalid or not enqueued.
[[nodiscard]] inline f32 pending_priority_for_guarded(const LodResidencyQueue& queue, u32 chunk_index,
                                                       LodResidencyRequestKind kind) {
    if (!is_valid_chunk_index(chunk_index)) {
        return -1.f;
    }
    return queue.pending_priority_for(chunk_index, kind);
}

/// Flush helper: submits pending batch only when the highest-priority request meets `min_priority`.
[[nodiscard]] inline u32 try_flush_pending_if(LodResidencyQueue& queue, u32 budget, f32 min_priority,
                                               LodResidencyWorkFn work) {
    if (!queue.has_pending_enqueue() || work == nullptr || budget == 0u) {
        return 0u;
    }

    LodResidencyRequest peeked{};
    if (!queue.peek_pending(peeked) || peeked.priority < min_priority) {
        return 0u;
    }
    return queue.flush(budget, work);
}

} // namespace fuse::terrain
