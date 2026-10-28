#include <fuse/core/init.hpp>
#include <fuse/mechanics/destroy_component.hpp>
#include <fuse/mechanics/destroy_delay.hpp>
#include <fuse/mechanics/destroy_timer.hpp>

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

void testDestroyDelayOk() {
    expectTrue(fuse::mechanics::destroy_delay_ok(0.f), "destroy_delay_ok(0) is true");
    expectTrue(fuse::mechanics::destroy_delay_ok(1.f), "destroy_delay_ok(1) is true");
    expectTrue(fuse::mechanics::destroy_delay_ok(60.f), "destroy_delay_ok(60) is true");
    expectTrue(!fuse::mechanics::destroy_delay_ok(-0.1f), "destroy_delay_ok(-0.1) is false");
    expectTrue(!fuse::mechanics::destroy_delay_ok(61.f), "destroy_delay_ok(61) is false");
    const float nonFinite = std::numeric_limits<float>::quiet_NaN();
    expectTrue(!fuse::mechanics::destroy_delay_ok(nonFinite), "destroy_delay_ok(non-finite) is false");
}

void test_destroy_timer() {
    expectTrue(!fuse::mechanics::destroy_ready(0.4f, 0.5f), "age 0.4 delay 0.5 is not ready");
    expectTrue(fuse::mechanics::destroy_ready(0.5f, 0.5f), "age 0.5 delay 0.5 is ready");
    const float aged = fuse::mechanics::destroy_age(0.4f, 0.1f);
    const float error = aged - 0.5f;
    expectTrue(aged == 0.4f + 0.1f && error > -1e-6f && error < 1e-6f,
               "destroy_age(0.4, 0.1) == 0.5");
}

} // namespace

int main() {
    testDestroyDelayOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_mechanics_destroy: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    fuse::core::initialize();

    fuse::mechanics::DestroyComponent destroy("lever_destroy");
    expectTrue(destroy.destroyOnTrigger(), "destroy on trigger default true");
    expectTrue(destroy.triggerDestroy(), "first trigger succeeds");
    expectTrue(destroy.triggered(), "destroy marked triggered");
    expectTrue(destroy.destroyCount() == 1u, "destroy count tracked");
    expectTrue(!destroy.triggerDestroy(), "second trigger rejected");

    destroy.setDestroyOnTrigger(false);
    expectTrue(!destroy.triggerDestroy(), "destroy disabled when flag off");

    test_destroy_timer();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_destroy: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_destroy: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
