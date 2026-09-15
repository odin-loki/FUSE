#include <fuse/platform/profile.hpp>
#include <fuse/platform/thread.hpp>

#include <atomic>
#include <thread>

namespace fuse::platform {

namespace {
std::atomic<ThreadId> g_renderThreadId{0};
}

u32 getCoreCount() {
    const unsigned int cores = std::thread::hardware_concurrency();
    return cores > 0 ? static_cast<u32>(cores) : 1u;
}

u32 getPerformanceCoreCount() {
    return getCoreCount();
}

u32 recommendedFiberStackBytes() {
    return currentJobProfileLimits().fiberStackBytes;
}

void registerRenderThread() {
    const ThreadId id = static_cast<ThreadId>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    g_renderThreadId.store(id, std::memory_order_release);
}

ThreadId renderThread() {
    return g_renderThreadId.load(std::memory_order_acquire);
}

bool isRenderThread() {
    const ThreadId registered = g_renderThreadId.load(std::memory_order_acquire);
    if (registered == 0) {
        return true;
    }
    const ThreadId current = static_cast<ThreadId>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return current == registered;
}

void setThreadPriority(ThreadId /*thread*/, int /*priority*/) {
    // Stub — platform backends land in WP-03+.
}

} // namespace fuse::platform
