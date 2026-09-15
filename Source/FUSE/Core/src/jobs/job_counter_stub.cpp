#include <fuse/jobs/job_counter.hpp>

namespace fuse::jobs {

void JobCounter::wait() {
    // Stub — fiber yield lands in WP-03.
}

} // namespace fuse::jobs
