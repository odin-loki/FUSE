#include <fuse/mechanics/delay_component.hpp>
#include <fuse/mechanics/rotate_component.hpp>
#include <fuse/core/init.hpp>

#include <cstdio>
#include <cstdlib>
#include <cmath>

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

} // namespace

int main() {
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
