#include <fuse/phase7/phase7_test_registry.hpp>

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

void testChecklistAndModules() {
    const auto& checklist = fuse::phase7::Phase7TestRegistry::checklist();
    expectTrue(!checklist.empty(), "phase 7 checklist non-empty");
    expectTrue(fuse::phase7::Phase7TestRegistry::stubLandedCount() >= 9u,
               "at least one stub landed per B7 module area");
    expectTrue(fuse::phase7::Phase7TestRegistry::automatedCount() >= 1u,
               "automated deliverables registered");
    expectTrue(fuse::phase7::Phase7TestRegistry::countByModule(fuse::phase7::Phase7Module::Animation) >= 1u,
               "animation module represented");
    expectTrue(fuse::phase7::Phase7TestRegistry::countByModule(fuse::phase7::Phase7Module::Platform) >= 1u,
               "platform module represented");
    expectTrue(fuse::phase7::Phase7TestRegistry::countByModule(fuse::phase7::Phase7Module::Assets) >= 1u,
               "assets module represented");
}

void testIntegrationSmoke() {
    expectTrue(fuse::phase7::Phase7TestRegistry::runIntegrationSmoke(),
               "phase 7 integration smoke constructs linked facades");
}

} // namespace

int main() {
    testChecklistAndModules();
    testIntegrationSmoke();

    if (g_failures == 0) {
        std::printf("fuse_phase7_integration: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_phase7_integration: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
