#pragma once

#include <chrono>

namespace fuse::platform {

/// Block the calling thread for at least `duration` (never returns early), on every platform.
///
/// Use this instead of std::this_thread::sleep_for whenever the wait can be under a millisecond or
/// must not end early. Under MinGW-w64, libstdc++ implements sleep_for through winpthreads'
/// nanosleep, which truncates to whole milliseconds: a 500 us sleep returns after ~0.2 us (on
/// Windows and under Wine alike), and 1.9 ms sleeps for 1 ms.
///
/// - Windows: waits of 1 ms or more block on a high-resolution waitable timer
///   (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, falling back to a standard timer where unsupported);
///   shorter waits, and whatever the timer leaves, are topped up against steady_clock with yields.
/// - Elsewhere: std::this_thread::sleep_for, then the same steady_clock top-up.
///
/// Waits may overshoot by scheduler latency. Zero or negative durations return at once.
/// No heap allocation.
void sleepAtLeast(std::chrono::microseconds duration);

} // namespace fuse::platform
