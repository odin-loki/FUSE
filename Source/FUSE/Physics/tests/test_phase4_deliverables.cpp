#include <fuse/gameplay/slider.hpp>
#include <fuse/physics/deliverable_name.hpp>
#include <fuse/physics/phase4_test_registry.hpp>

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

void testDeliverableNameOk() {
    expectTrue(fuse::physics::deliverable_name_ok("Phase4"), "Phase4 is a deliverable name");
    expectTrue(!fuse::physics::deliverable_name_ok(""), "empty name is rejected");
    expectTrue(!fuse::physics::deliverable_name_ok("4Phase"), "name starting with a digit is rejected");
    constexpr char kThirtyTwo[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdef";
    static_assert(sizeof(kThirtyTwo) - 1 == 32);
    expectTrue(fuse::physics::deliverable_name_ok(kThirtyTwo), "32-character name is accepted");
    constexpr char kThirtyThree[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefg";
    static_assert(sizeof(kThirtyThree) - 1 == 33);
    expectTrue(!fuse::physics::deliverable_name_ok(kThirtyThree), "33-character name is rejected");
}

void testCatalogAndCategories() {
    const auto& catalog = fuse::physics::Phase4TestRegistry::catalog();
    expectTrue(!catalog.empty(), "phase 4 catalog non-empty");
    expectTrue(fuse::physics::Phase4TestRegistry::countByCategory(
                   fuse::physics::Phase4TestCategory::Destruction) >= 1u,
               "destruction category present");
    expectTrue(fuse::physics::Phase4TestRegistry::automatedCount() >= 1u, "automated tests registered");
}

void testAutomatedSmoke() {
    expectTrue(fuse::physics::Phase4TestRegistry::runAutomatedSmoke(), "automated smoke passes");
}

void testSliderDoor() {
    fuse::gameplay::Slider door;
    door.open = 1.f;
    door.speed = 2.f;
    fuse::gameplay::tick(door, 0.25f);
    expectTrue(std::fabs(door.position - 0.5f) < 1e-4f, "slider covers half the travel in a quarter second");
    fuse::gameplay::tick(door, 1.f);
    expectTrue(std::fabs(door.position - 1.f) < 1e-4f && fuse::gameplay::at_target(door),
               "slider reaches open and stops");
}

} // namespace

int main() {
    testDeliverableNameOk();
    testSliderDoor();
    testCatalogAndCategories();
    testAutomatedSmoke();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
