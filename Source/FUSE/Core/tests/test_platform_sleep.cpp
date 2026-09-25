// fuse::platform::sleepAtLeast never returns early, at sub-millisecond and multi-millisecond waits.
// Regression: under MinGW-w64, std::this_thread::sleep_for truncates to whole milliseconds (winpthreads
// nanosleep), so a 500 us sleep returned after ~0.2 us. Overshoot is reported, not gated: it depends
// on the scheduler and machine load.

#include <fuse/platform/sleep.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace {

int g_failures = 0;

void checkNeverEarly(std::chrono::microseconds requested, int samples) {
    using Clock = std::chrono::steady_clock;
    const long long requestedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(requested).count();
    long long minNs = -1;
    long long maxNs = 0;
    long long totalNs = 0;
    int early = 0;
    for (int i = 0; i < samples; ++i) {
        const Clock::time_point start = Clock::now();
        fuse::platform::sleepAtLeast(requested);
        const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        if (ns < requestedNs) {
            ++early;
        }
        minNs = minNs < 0 ? ns : std::min(minNs, ns);
        maxNs = std::max(maxNs, ns);
        totalNs += ns;
    }
    std::printf("  sleepAtLeast(%lld us) x%d: min %.1f us, mean %.1f us, max %.1f us, early %d\n",
                static_cast<long long>(requested.count()), samples, static_cast<double>(minNs) / 1000.0,
                static_cast<double>(totalNs) / samples / 1000.0, static_cast<double>(maxNs) / 1000.0, early);
    if (early != 0) {
        std::fprintf(stderr, "FAIL: sleepAtLeast(%lld us) returned early %d/%d times (min %.1f us)\n",
                     static_cast<long long>(requested.count()), early, samples,
                     static_cast<double>(minNs) / 1000.0);
        ++g_failures;
    }
}

void checkNonPositiveReturnsImmediately() {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    fuse::platform::sleepAtLeast(std::chrono::microseconds(0));
    fuse::platform::sleepAtLeast(std::chrono::microseconds(-5));
    const auto elapsed = Clock::now() - start;
    // Generous bound: only catches a sleep that treats <= 0 as a real (or huge unsigned) wait.
    if (elapsed > std::chrono::milliseconds(100)) {
        std::fprintf(stderr, "FAIL: sleepAtLeast(<= 0) blocked\n");
        ++g_failures;
    }
}

} // namespace

int main() {
    constexpr int kSamples = 200;
    checkNeverEarly(std::chrono::microseconds(50), kSamples);
    checkNeverEarly(std::chrono::microseconds(500), kSamples);
    checkNeverEarly(std::chrono::milliseconds(2), kSamples);
    checkNonPositiveReturnsImmediately();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core_platform_sleep_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_core_platform_sleep_tests: OK\n");
    return 0;
}
