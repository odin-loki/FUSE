#include <fuse/mechanics/area_component.hpp>

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

} // namespace

int main() {
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
