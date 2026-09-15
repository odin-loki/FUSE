#pragma once

#include <fuse/types.hpp>

#include <functional>
#include <mutex>
#include <vector>

namespace fuse::terrain {

enum class LodResidencyRequestKind : u8 {
    Load,
    Unload,
};

/// One async chunk load/unload request submitted to the worker pool.
struct LodResidencyRequest {
    u32 chunk_index = 0;
    LodResidencyRequestKind kind = LodResidencyRequestKind::Load;
    f32 priority = 0.f;
};

/// Completed request drained on the game thread after JobScheduler work finishes.
struct CompletedLodResidencyRequest {
    u32 chunk_index = 0;
    LodResidencyRequestKind kind = LodResidencyRequestKind::Load;
    bool success = true;
};

/// Worker-side mesh/heightfield I/O stub — production wiring reads chunk assets from disk.
using LodResidencyWorkFn = std::function<bool(u32 chunk_index, LodResidencyRequestKind kind)>;

/// Async LOD residency queue backed by JobScheduler (mirrors B7.6 StreamingRequestQueue).
class LodResidencyQueue {
public:
    /// Submit work to JobScheduler. Returns false when the scheduler is unavailable.
    bool submit(LodResidencyRequest request, LodResidencyWorkFn work);

    /// Move completed requests into `out` and clear the internal completion buffer.
    u32 drain_completed(std::vector<CompletedLodResidencyRequest>& out);

    [[nodiscard]] u32 in_flight_count() const;
    [[nodiscard]] u32 completed_count() const;

    void clear();

private:
    void push_completed_(CompletedLodResidencyRequest completed);

    mutable std::mutex m_mutex;
    u32 m_inFlight = 0;
    std::vector<CompletedLodResidencyRequest> m_completed;
};

} // namespace fuse::terrain
