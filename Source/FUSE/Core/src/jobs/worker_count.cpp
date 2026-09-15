#include <fuse/config.hpp>
#include <fuse/jobs/worker_count.hpp>
#include <fuse/platform/profile.hpp>
#include <fuse/platform/thread.hpp>

#include <algorithm>

namespace fuse::jobs {

namespace {

u32 clampU32(u32 value, u32 minValue, u32 maxValue) {
    return std::min(std::max(value, minValue), maxValue);
}

} // namespace

u32 computeWorkerCount(const WorkerCountParams& params) {
#if FUSE_JOBS_SINGLE_THREAD
    return 0;
#else
    u32 usable = params.usableCores;
    if (usable == 0) {
        usable = 1;
    }

    u32 reserve = 0;
    u32 minWorkers = 0;
    u32 maxWorkers = 0;

    if (params.powerState == platform::PowerState::Background) {
        // Background: at most one worker; none when only a single core is available.
        minWorkers = 0;
        maxWorkers = 1;
        const u32 available = (usable > 1u) ? 1u : 0u;
        return clampU32(available, minWorkers, maxWorkers);
    } else if (params.mobileProfile) {
        usable = (params.performanceCores > 0) ? params.performanceCores : usable;
        reserve = 1;
        minWorkers = 1;
        maxWorkers = 4;
    } else {
        reserve = 2;
        minWorkers = 1;
        maxWorkers = 16;
    }

    if (params.powerState == platform::PowerState::Thermal) {
        maxWorkers = std::max<u32>(1, maxWorkers / 2);
        reserve += 1;
    } else if (params.powerState == platform::PowerState::LowPower) {
        maxWorkers = std::min<u32>(maxWorkers, params.mobileProfile ? 2u : 4u);
    }

    if (params.hardCap > 0) {
        maxWorkers = std::min(maxWorkers, params.hardCap);
    }

    const u32 available = (usable > reserve) ? (usable - reserve) : 0;
    return clampU32(available, minWorkers, maxWorkers);
#endif
}

u32 computeWorkerCountForCurrentPlatform() {
    WorkerCountParams params;
    params.usableCores = platform::getCoreCount();
    params.performanceCores = platform::getPerformanceCoreCount();
    params.powerState = platform::getPowerState();
    params.mobileProfile = platform::isMobileProfile();

    return computeWorkerCount(params);
}

} // namespace fuse::jobs
