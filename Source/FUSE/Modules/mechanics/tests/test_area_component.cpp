#include <fuse/mechanics/area_component.hpp>
#include <fuse/mechanics/area_id.hpp>
#include <fuse/mechanics/circle_area.hpp>

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

void testAreaIdOk() {
    expectTrue(fuse::mechanics::area_id_ok(0), "area_id_ok(0) is true");
    expectTrue(fuse::mechanics::area_id_ok(1023), "area_id_ok(1023) is true");
    expectTrue(!fuse::mechanics::area_id_ok(-1), "area_id_ok(-1) is false");
    expectTrue(!fuse::mechanics::area_id_ok(1024), "area_id_ok(1024) is false");
}

void testCircleContains() {
    expectTrue(fuse::mechanics::circle_contains(3.f, 4.f, 0.f, 0.f, 5.f),
               "circle contains the 3-4-5 boundary");
    expectTrue(!fuse::mechanics::circle_contains(3.f, 4.f, 0.f, 0.f, 4.f),
               "circle rejects 3-4 outside radius 4");
    expectTrue(fuse::mechanics::circle_contains(1.f, 0.f, 0.f, 0.f, 2.f),
               "circle contains a point strictly inside");
    expectTrue(fuse::mechanics::circle_contains(4.f, 5.f, 1.f, 1.f, 5.f),
               "circle contains the boundary of an offset center");
    expectTrue(!fuse::mechanics::circle_contains(0.f, 0.f, 0.f, 0.f, 0.f),
               "circle rejects a zero radius");
    expectTrue(!fuse::mechanics::circle_contains(0.f, 0.f, 0.f, 0.f, -1.f),
               "circle rejects a negative radius");

    const float nan = std::nanf("");
    const float inf = HUGE_VALF;
    expectTrue(!fuse::mechanics::circle_contains(nan, 0.f, 0.f, 0.f, 1.f),
               "circle rejects a non-finite px");
    expectTrue(!fuse::mechanics::circle_contains(0.f, inf, 0.f, 0.f, 1.f),
               "circle rejects a non-finite py");
    expectTrue(!fuse::mechanics::circle_contains(0.f, 0.f, -inf, 0.f, 1.f),
               "circle rejects a non-finite cx");
    expectTrue(!fuse::mechanics::circle_contains(0.f, 0.f, 0.f, nan, 1.f),
               "circle rejects a non-finite cy");
    expectTrue(!fuse::mechanics::circle_contains(0.f, 0.f, 0.f, 0.f, inf),
               "circle rejects a non-finite radius");
    expectTrue(!fuse::mechanics::circle_contains(nan, nan, nan, nan, nan),
               "circle rejects all non-finite inputs");
}

} // namespace

int main() {
    testAreaIdOk();
    if (g_failures != 0) {
        return EXIT_FAILURE;
    }

    testCircleContains();

    fuse::mechanics::AreaComponent area("patrol_area", 2.f);
    expectTrue(area.contains(1.f, 0.f, 0.f), "area contains point inside radius");
    expectTrue(!area.contains(3.f, 0.f, 0.f), "area rejects point outside radius");

    expectTrue(area.testObject(7u, 0.5f, 0.f, 0.f), "area enter detected");
    expectTrue(area.enterCount() == 1u, "area enter counted");
    area.testObject(7u, 3.f, 0.f, 0.f);
    expectTrue(area.leaveCount() == 1u, "area leave counted");

    if (g_failures == 0) {
        std::printf("fuse_mechanics_area_component: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_area_component: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
