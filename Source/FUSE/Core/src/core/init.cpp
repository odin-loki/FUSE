#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/worker_count.hpp>

namespace fuse::core {

namespace {
bool g_initialized = false;
}

bool initialize() {
    if (g_initialized) {
        return true;
    }

    const u32 workers = jobs::computeWorkerCountForCurrentPlatform();
    jobs::JobScheduler::instance().initialize(workers);
    g_initialized = true;
    return true;
}

void shutdown() {
    if (!g_initialized) {
        return;
    }
    jobs::JobScheduler::instance().shutdown();
    g_initialized = false;
}

bool isInitialized() {
    return g_initialized;
}

} // namespace fuse::core
