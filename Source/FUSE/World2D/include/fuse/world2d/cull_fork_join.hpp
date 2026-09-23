#pragma once

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/types.hpp>

namespace fuse::world2d {

/// Heap-free fork-join over [0, count) for the per-frame dimension cull passes (gate 3.5).
///
/// Delegates to `JobScheduler::parallel_for`, which is heap-free in steady state (pooled dispatch
/// record, body reached by pointer, chunks claimed atomically by a capped number of helper jobs).
/// An earlier version submitted one queued job per chunk: a 1000-object cull at grain 4 put ~250
/// jobs in the worker queues each frame, and a steal that moved half a victim's queue into a
/// thief's already-refilled queue could push that queue past its grown capacity, reallocating the
/// ring in a later, timing-dependent frame (seen under ASan in fuse_b4_pipeline_alloc_gate).
///
/// Not reentrant: one `run()` per instance at a time, and the body must not call back into it.
class CullForkJoin {
public:
    CullForkJoin() = default;
    CullForkJoin(const CullForkJoin&) = delete;
    CullForkJoin& operator=(const CullForkJoin&) = delete;

    template <typename Body>
    void run(u32 count, u32 grainSize, const Body& body) {
        jobs::JobScheduler& scheduler = jobs::JobScheduler::instance();
        if (!scheduler.isInitialized() || scheduler.isSingleThreaded()) {
            for (u32 i = 0; i < count; ++i) {
                body(i);
            }
            return;
        }
        scheduler.parallel_for(0u, count, grainSize == 0 ? 1u : grainSize, body);
    }
};

} // namespace fuse::world2d
