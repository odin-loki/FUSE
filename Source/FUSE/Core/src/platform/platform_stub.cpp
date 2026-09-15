#include <fuse/platform/power.hpp>
#include <fuse/platform/thread.hpp>

#include <thread>

namespace fuse::platform {

PowerState getPowerState() {
    return PowerState::Normal;
}

u32 getCoreCount() {
    const unsigned int cores = std::thread::hardware_concurrency();
    return cores > 0 ? static_cast<u32>(cores) : 1u;
}

u32 getPerformanceCoreCount() {
    return getCoreCount();
}

u32 recommendedFiberStackBytes() {
#if defined(FUSE_PLATFORM_MOBILE) && FUSE_PLATFORM_MOBILE
    return 32u * 1024u;
#else
    return 64u * 1024u;
#endif
}

ThreadId renderThread() {
    return static_cast<ThreadId>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

void setThreadPriority(ThreadId /*thread*/, int /*priority*/) {
    // Stub — platform backends land in WP-03+.
}

} // namespace fuse::platform
