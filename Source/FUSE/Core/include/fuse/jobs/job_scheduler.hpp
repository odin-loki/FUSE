#pragma once

#include <fuse/types.hpp>

namespace fuse::jobs {

/// Fiber work-stealing scheduler spine (stub — U1/WP-03).
class JobScheduler {
public:
    static JobScheduler& instance();

    void initialize(u32 workerCount);
    void shutdown();

    u32 workerCount() const { return m_workerCount; }
    bool isSingleThreaded() const { return m_workerCount == 0; }

private:
    JobScheduler() = default;
    u32 m_workerCount = 0;
    bool m_initialized = false;
};

} // namespace fuse::jobs
