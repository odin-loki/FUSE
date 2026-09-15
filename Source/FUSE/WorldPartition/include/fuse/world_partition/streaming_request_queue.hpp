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

/// Async request queue stub backed by JobScheduler (mirrors fuse::io VFS async loads).
class StreamingRequestQueue {
public:
    /// Submit work to JobScheduler. Returns false when the scheduler is unavailable or
    /// `max_pending_submits` would be exceeded.
    bool submit(StreamingRequest request, StreamingWorkFn work);

    /// Move completed requests into `out` (highest priority first) and clear the buffer.
    u32 drain_completed(std::vector<CompletedStreamingRequest>& out);

    void set_max_pending_submits(u32 max_pending) { m_max_pending_submits = max_pending; }
    [[nodiscard]] u32 max_pending_submits() const { return m_max_pending_submits; }
    [[nodiscard]] u32 pending_submit_count() const;

    [[nodiscard]] u32 in_flight_count() const;
    [[nodiscard]] u32 completed_count() const;
    [[nodiscard]] bool empty() const;

    void clear();

private:
    void push_completed_(CompletedStreamingRequest completed);

    mutable std::mutex m_mutex;
    u32 m_inFlight = 0;
    u64 m_submit_sequence = 0;
    u32 m_max_pending_submits = 0; ///< 0 = unlimited pending (in-flight + completed buffer)
    std::vector<CompletedStreamingRequest> m_completed;
};

} // namespace fuse::world_partition
