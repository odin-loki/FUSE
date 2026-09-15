#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/worker_count.hpp>

namespace fuse::jobs {

JobScheduler& JobScheduler::instance() {
    static JobScheduler scheduler;
    return scheduler;
}

void JobScheduler::initialize(u32 workerCount) {
    m_workerCount = workerCount;
    m_initialized = true;
}

void JobScheduler::shutdown() {
    m_workerCount = 0;
    m_initialized = false;
}

} // namespace fuse::jobs
