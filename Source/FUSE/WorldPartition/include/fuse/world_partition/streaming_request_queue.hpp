#pragma once

#include <fuse/types.hpp>
#include <fuse/world_partition/grid_cell.hpp>

#include <functional>
#include <mutex>
#include <vector>

namespace fuse::world_partition {

enum class StreamingRequestKind : u8 {
    Load,
    Unload,
};

/// One async load/unload request submitted to the worker pool.
struct StreamingRequest {
    GridCoord coord{};
    StreamingRequestKind kind = StreamingRequestKind::Load;
    f32 priority = 0.f;
};

/// Completed request drained on the game thread after JobScheduler work finishes.
struct CompletedStreamingRequest {
    GridCoord coord{};
    StreamingRequestKind kind = StreamingRequestKind::Load;
    f32 priority = 0.f;
    u64 submit_sequence = 0; ///< FIFO tie-break when priorities match
    bool success = true;
};

/// Worker-side I/O stub — production wiring reads cell assets from disk.
using StreamingWorkFn = std::function<bool(GridCoord coord, StreamingRequestKind kind)>;

/// Raise pending priority when the same coord/kind is re-queued (stub heuristic).
[[nodiscard]] f32 promote_streaming_priority(f32 current, f32 incoming);

/// Lower pending priority when focus moves away (scale clamped to [0, 1]).
[[nodiscard]] f32 demote_streaming_priority(f32 current, f32 scale);

/// Priority ordering: higher priority first, unload before load at equal priority, FIFO tie-break.
[[nodiscard]] int compare_streaming_request_order(f32 priority_a, StreamingRequestKind kind_a, u64 sequence_a,
                                                  f32 priority_b, StreamingRequestKind kind_b, u64 sequence_b);

/// Sort pending requests by priority (highest first) without removing them. Returns count copied.
[[nodiscard]] u32 order_by_priority(std::vector<StreamingRequest>& out,
                                    const std::vector<StreamingRequest>& pending,
                                    const std::vector<u64>& enqueue_sequences);

/// Async request queue stub backed by JobScheduler (mirrors fuse::io VFS async loads).
class StreamingRequestQueue {
public:
    /// Queue a request for later submission. Promotes priority when the same coord/kind is already pending.
    bool enqueue(StreamingRequest request);

    /// Remove the highest-priority pending request into `out`. Returns false when the pending queue is empty.
    bool dequeue(StreamingRequest& out);

    /// Copy pending requests into `out` in priority order without removing them. Returns 0 when empty.
    [[nodiscard]] u32 order_by_priority(std::vector<StreamingRequest>& out) const;

    /// True when at least one request is waiting in the pending enqueue buffer.
    [[nodiscard]] bool has_pending_enqueue() const;

    /// Copy the highest-priority pending request into `out` without removing it. Returns false when empty.
    [[nodiscard]] bool peek_pending(StreamingRequest& out) const;

    /// Lower priority for a pending request (returns false when not found).
    bool demote(GridCoord coord, StreamingRequestKind kind, f32 scale);

    /// Submit up to `budget` highest-priority pending requests. Returns the number submitted.
    u32 flush(u32 budget, StreamingWorkFn work);

    /// Submit work to JobScheduler. Returns false when the scheduler is unavailable or
    /// `max_pending_submits` would be exceeded.
    bool submit(StreamingRequest request, StreamingWorkFn work);

    /// Move completed requests into `out` (highest priority first) and clear the buffer.
    u32 drain_completed(std::vector<CompletedStreamingRequest>& out);

    void set_max_pending_submits(u32 max_pending) { m_max_pending_submits = max_pending; }
    [[nodiscard]] u32 max_pending_submits() const { return m_max_pending_submits; }
    [[nodiscard]] u32 pending_enqueue_count() const;
    [[nodiscard]] u32 pending_submit_count() const;
    /// Pending priority for coord/kind, or -1 when not enqueued.
    [[nodiscard]] f32 pending_priority_for(GridCoord coord, StreamingRequestKind kind) const;

    [[nodiscard]] u32 in_flight_count() const;
    [[nodiscard]] u32 completed_count() const;
    [[nodiscard]] bool empty() const;

    void clear();

private:
    struct PendingStreamingRequest {
        StreamingRequest request{};
        u64 enqueue_sequence = 0;
    };

    [[nodiscard]] bool would_exceed_budget_() const;
    void sort_pending_by_priority_();
    void push_completed_(CompletedStreamingRequest completed);

    mutable std::mutex m_mutex;
    u32 m_inFlight = 0;
    u64 m_submit_sequence = 0;
    u64 m_enqueue_sequence = 0;
    u32 m_max_pending_submits = 0; ///< 0 = unlimited pending (in-flight + completed buffer)
    std::vector<PendingStreamingRequest> m_pending;
    std::vector<CompletedStreamingRequest> m_completed;
};

/// Empty-queue guard: true when the pending enqueue buffer has at least one request.
[[nodiscard]] inline bool has_pending_enqueue(const StreamingRequestQueue& queue) {
    return queue.has_pending_enqueue();
}

/// Dequeue helper: removes highest-priority pending request when non-empty.
[[nodiscard]] inline bool try_dequeue_pending(StreamingRequestQueue& queue, StreamingRequest& out) {
    if (!queue.has_pending_enqueue()) {
        return false;
    }
    return queue.dequeue(out);
}

/// Peek helper: copies highest-priority pending request without removing it.
[[nodiscard]] inline bool peek_highest_pending(const StreamingRequestQueue& queue, StreamingRequest& out) {
    return queue.peek_pending(out);
}

/// Peek helper: returns the highest pending priority, or -1 when the pending queue is empty.
[[nodiscard]] inline f32 peek_highest_pending_priority(const StreamingRequestQueue& queue) {
    StreamingRequest peeked{};
    return peek_highest_pending(queue, peeked) ? peeked.priority : -1.f;
}

/// Dequeue helper: removes the highest-priority pending request only when its priority is at least `min_priority`.
[[nodiscard]] inline bool try_dequeue_pending_if(StreamingRequestQueue& queue, f32 min_priority,
                                                 StreamingRequest& out) {
    if (!peek_highest_pending(queue, out) || out.priority < min_priority) {
        return false;
    }
    return try_dequeue_pending(queue, out);
}

} // namespace fuse::world_partition
