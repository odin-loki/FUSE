#include <fuse/mechanics/polyhedron_trigger.hpp>
#include <fuse/core/init.hpp>

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
    fuse::core::initialize();

    const fuse::mechanics::ConvexPolyhedron cube =
        fuse::mechanics::ConvexPolyhedron::axis_aligned_box(0.f, 0.f, 0.f, 2.f, 2.f, 2.f);
    expectTrue(cube.contains(1.f, 1.f, 1.f), "unit cube contains interior point");
    expectTrue(!cube.contains(3.f, 1.f, 1.f), "unit cube rejects exterior point");

    fuse::mechanics::PolyhedronTriggerZone trigger("poly_trigger", cube);
    bool entered = false;
    trigger.setOnEnter([&entered](fuse::u32 /*objectId*/) { entered = true; });

    trigger.testObject(1u, 3.f, 1.f, 1.f);
    expectTrue(trigger.enterCount() == 0u, "outside point does not enter");
    trigger.testObject(1u, 1.f, 1.f, 1.f);
    expectTrue(trigger.enterCount() == 1u, "inside point enters polyhedron trigger");
    expectTrue(entered, "enter callback fired");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_polyhedron_trigger: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_polyhedron_trigger: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
