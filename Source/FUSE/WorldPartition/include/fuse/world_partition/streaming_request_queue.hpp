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
    bool success = true;
};

/// Worker-side I/O stub — production wiring reads cell assets from disk.
using StreamingWorkFn = std::function<bool(GridCoord coord, StreamingRequestKind kind)>;

/// Async request queue stub backed by JobScheduler (mirrors fuse::io VFS async loads).
class StreamingRequestQueue {
public:
    /// Submit work to JobScheduler. Returns false when the scheduler is unavailable.
    bool submit(StreamingRequest request, StreamingWorkFn work);

    /// Move completed requests into `out` and clear the internal completion buffer.
    u32 drain_completed(std::vector<CompletedStreamingRequest>& out);

    [[nodiscard]] u32 in_flight_count() const;
    [[nodiscard]] u32 completed_count() const;

    void clear();

private:
    void push_completed_(CompletedStreamingRequest completed);

    mutable std::mutex m_mutex;
    u32 m_inFlight = 0;
    std::vector<CompletedStreamingRequest> m_completed;
};

} // namespace fuse::world_partition
