#include <fuse/mechanics/delay_component.hpp>
#include <fuse/mechanics/rotate_component.hpp>
#include <fuse/mechanics/rotate_ms.hpp>
#include <fuse/mechanics/rotate_step.hpp>
#include <fuse/core/init.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

void testRotateMsOk() {
    expectTrue(fuse::mechanics::rotate_ms_ok(0), "rotate_ms_ok(0) is true");
    expectTrue(fuse::mechanics::rotate_ms_ok(60000), "rotate_ms_ok(60000) is true");
    expectTrue(!fuse::mechanics::rotate_ms_ok(-1), "rotate_ms_ok(-1) is false");
    expectTrue(!fuse::mechanics::rotate_ms_ok(60001), "rotate_ms_ok(60001) is false");
}

void testRotateStep() {
    constexpr float kTol = 1e-4f;
    expectNear(fuse::mechanics::rotate_step(10.f, 90.f, 1.f), 100.f, kTol,
               "rotate_step(10, 90, 1) == 100");
    expectNear(fuse::mechanics::rotate_step(350.f, 20.f, 1.f), 10.f, kTol,
               "rotate_step(350, 20, 1) == 10");
    expectNear(fuse::mechanics::rotate_step(10.f, 90.f, 0.f), 10.f, kTol,
               "rotate_step(10, 90, 0) == 10");
    expectNear(fuse::mechanics::rotate_step(10.f, 90.f, -1.f), 10.f, kTol,
               "negative dt counts as 0");
    expectNear(fuse::mechanics::rotate_step(10.f, -20.f, 1.f), 350.f, kTol,
               "negative rate wraps into [0, 360)");
    const float kept =
        fuse::mechanics::rotate_step(10.f, std::numeric_limits<float>::infinity(), 1.f);
    expectTrue(std::isfinite(kept), "non-finite rate keeps a finite angle");
    expectNear(kept, 10.f, kTol, "non-finite rate keeps a finite angle");
    const float cleared =
        fuse::mechanics::rotate_step(std::numeric_limits<float>::quiet_NaN(), 90.f, 1.f);
    expectTrue(cleared == 0.f, "non-finite angle returns 0");
}

} // namespace

int main() {
    testRotateMsOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_mechanics_delay_rotate: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testRotateStep();

    fuse::core::initialize();

    fuse::mechanics::DelayComponent delay("door_delay", 100);
    delay.advance(50);
    expectTrue(!delay.fired(), "delay not fired before threshold");
    delay.advance(60);
    expectTrue(delay.fired(), "delay fires after threshold");
    expectTrue(delay.fireCount() == 1u, "delay fire count");

    fuse::mechanics::RotateComponent rotate("lever_spin", 90.f);
    rotate.advance(0.5f);
    expectNear(rotate.angleDeg(), 45.f, 0.001f, "rotate advances angle");
    expectTrue(rotate.tickCount() == 1u, "rotate tick count");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_delay_rotate: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_delay_rotate: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
