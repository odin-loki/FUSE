#include <fuse/config.hpp>
#include <fuse/jobs/worker_count.hpp>
#include <fuse/math/bezier.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/platform/power.hpp>
#include <fuse/platform/worker_count.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

void testWorkerCountOk() {
    expectTrue(fuse::platform::worker_count_ok(1), "one worker");
    expectTrue(fuse::platform::worker_count_ok(64), "64 workers");
    expectTrue(!fuse::platform::worker_count_ok(0), "zero workers rejected");
    expectTrue(!fuse::platform::worker_count_ok(65), "65 workers rejected");
}

void testDesktopProfile() {
    fuse::jobs::WorkerCountParams params;
    params.usableCores = 8;
    params.performanceCores = 8;
    params.mobileProfile = false;
    params.powerState = fuse::platform::PowerState::Normal;

    expectEq(fuse::jobs::computeWorkerCount(params), 6u, "desktop 8-core should reserve 2 -> 6 workers");
}

void testMobileProfile() {
    fuse::jobs::WorkerCountParams params;
    params.usableCores = 8;
    params.performanceCores = 4;
    params.mobileProfile = true;
    params.powerState = fuse::platform::PowerState::Normal;

    expectEq(fuse::jobs::computeWorkerCount(params), 3u, "mobile 4 perf cores should reserve 1 -> 3 workers");
    expectTrue(fuse::jobs::computeWorkerCount(params) <= 4u, "mobile max workers is 4");
}

void testBackgroundProfile() {
    fuse::jobs::WorkerCountParams params;
    params.usableCores = 8;
    params.mobileProfile = false;
    params.powerState = fuse::platform::PowerState::Background;

    expectEq(fuse::jobs::computeWorkerCount(params), 1u, "background profile caps at 1 worker");

    params.usableCores = 1;
    expectEq(fuse::jobs::computeWorkerCount(params), 0u, "background on single-core yields 0 workers");
}

void testThermalProfile() {
    fuse::jobs::WorkerCountParams params;
    params.usableCores = 8;
    params.mobileProfile = false;
    params.powerState = fuse::platform::PowerState::Thermal;

    const fuse::u32 workers = fuse::jobs::computeWorkerCount(params);
    expectTrue(workers < 6u, "thermal should reduce workers below normal desktop 6");
    expectTrue(workers >= 1u, "thermal should keep at least 1 worker on desktop");
}

void testHardCap() {
    fuse::jobs::WorkerCountParams params;
    params.usableCores = 32;
    params.mobileProfile = false;
    params.powerState = fuse::platform::PowerState::Normal;
    params.hardCap = 4;

    expectEq(fuse::jobs::computeWorkerCount(params), 4u, "project hard cap should clamp workers");
}

void testCubicBezier() {
    const fuse::math::Vec3 p0{0.f, 0.f, 0.f};
    const fuse::math::Vec3 p1{2.f / 3.f, 0.f, 0.f};
    const fuse::math::Vec3 p2{4.f / 3.f, 0.f, 0.f};
    const fuse::math::Vec3 p3{2.f, 0.f, 0.f};
    const fuse::math::Vec3 mid = fuse::math::cubic_bezier(p0, p1, p2, p3, 0.5f);
    expectTrue(std::fabs(mid.x - 1.f) < 1e-4f && std::fabs(mid.y) < 1e-4f && std::fabs(mid.z) < 1e-4f,
               "cubic bezier midpoint of a colinear segment is the halfway point");
    const fuse::math::Vec3 start = fuse::math::cubic_bezier(p0, p1, p2, p3, 0.f);
    expectTrue(start.x == 0.f && start.y == 0.f && start.z == 0.f, "cubic bezier t=0 is p0");
}

} // namespace

#if FUSE_JOBS_SINGLE_THREAD
void testSingleThreadMacroForcesZero() {
    fuse::jobs::WorkerCountParams params;
    params.usableCores = 32;
    params.performanceCores = 16;
    params.mobileProfile = false;
    params.powerState = fuse::platform::PowerState::Normal;
    params.hardCap = 8;

    expectEq(fuse::jobs::computeWorkerCount(params), 0u, "single-thread macro forces zero workers");
}
#endif

int main() {
    testWorkerCountOk();
    testCubicBezier();
#if FUSE_JOBS_SINGLE_THREAD
    testSingleThreadMacroForcesZero();
#else
    testDesktopProfile();
    testMobileProfile();
    testBackgroundProfile();
    testThermalProfile();
    testHardCap();
#endif

    if (g_failures == 0) {
        std::printf("fuse_core_tests: all worker-count checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
