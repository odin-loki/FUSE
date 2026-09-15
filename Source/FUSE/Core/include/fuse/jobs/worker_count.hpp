#pragma once

#include <fuse/platform/power.hpp>
#include <fuse/types.hpp>

namespace fuse::jobs {

struct WorkerCountParams {
    u32 usableCores = 1;
    u32 performanceCores = 0;
    platform::PowerState powerState = platform::PowerState::Normal;
    bool mobileProfile = false;
    u32 hardCap = 0; // 0 = no project.json override
};

/// Locked adaptive formula — architecture-parallel.md §3.1.1.
/// N = clamp(usable_cores - reserve, minWorkers, maxWorkers)
u32 computeWorkerCount(const WorkerCountParams& params);

/// Convenience: query platform stubs and compute worker count.
u32 computeWorkerCountForCurrentPlatform();

} // namespace fuse::jobs
