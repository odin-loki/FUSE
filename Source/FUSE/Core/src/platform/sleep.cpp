#include <fuse/platform/sleep.hpp>

#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#endif

namespace fuse::platform {

namespace {

using Clock = std::chrono::steady_clock;

/// Yield until `deadline` has passed on steady_clock.
void topUpUntil(Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        std::this_thread::yield();
    }
}

#if defined(_WIN32)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/// Cleared the first time CreateWaitableTimerExW rejects the high-resolution flag (Windows before
/// 10 1803, older Wine) so later calls go straight to a standard timer.
std::atomic<bool> g_highResolutionTimer{true};

/// Block on a waitable timer for about `duration` (>= 1 ms). A kernel timer per call: no heap
/// allocation, no per-thread state to tear down, and the setup cost is noise next to a 1 ms wait.
/// Returns false when no timer could be created or armed.
bool timerWait(std::chrono::microseconds duration) {
    HANDLE timer = nullptr;
    if (g_highResolutionTimer.load(std::memory_order_relaxed)) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        if (timer == nullptr) {
            g_highResolutionTimer.store(false, std::memory_order_relaxed);
        }
    }
    if (timer == nullptr) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    if (timer == nullptr) {
        return false;
    }
    // Negative due time = relative, in 100 ns units.
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(duration.count()) * 10;
    bool waited = false;
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        waited = WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
    }
    CloseHandle(timer);
    return waited;
}

#endif

} // namespace

void sleepAtLeast(std::chrono::microseconds duration) {
    if (duration.count() <= 0) {
        return;
    }
    const Clock::time_point deadline = Clock::now() + duration;
#if defined(_WIN32)
    if (duration >= std::chrono::milliseconds(1) && !timerWait(duration)) {
        // No timer: Sleep() never returns early, it only rounds up to the system tick.
        const long long ms = std::chrono::ceil<std::chrono::milliseconds>(duration).count();
        constexpr long long kMaxMs = static_cast<long long>(INFINITE) - 1;
        Sleep(static_cast<DWORD>(ms < kMaxMs ? ms : kMaxMs));
    }
#else
    std::this_thread::sleep_for(duration);
#endif
    topUpUntil(deadline);
}

} // namespace fuse::platform
