#pragma once

#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/types.hpp>

#include <functional>

namespace fuse::jobs {

/// Submit a parallel index range via the global scheduler.
template <typename Body>
inline void parallel_for(u32 begin, u32 end, u32 grainSize, const Body& body) {
    JobScheduler::instance().parallel_for(begin, end, grainSize, body);
}

inline void parallel_for(u32 count, const std::function<void(u32)>& body) {
    parallel_for(0, count, 1, body);
}

} // namespace fuse::jobs
