#include <fuse/core/phase1_test_registry.hpp>

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
    const auto& checklist = fuse::core::Phase1TestRegistry::checklist();
    expectTrue(!checklist.empty(), "phase 1 checklist non-empty");
    expectTrue(fuse::core::Phase1TestRegistry::stubLandedCount() >= 7u,
               "at least one stub landed per B1.1-B1.7 area");
    expectTrue(fuse::core::Phase1TestRegistry::automatedCount() >= 10u,
               "automated deliverables registered");
    expectTrue(fuse::core::Phase1TestRegistry::countByModule(fuse::core::Phase1Module::Jobs) >= 3u,
               "jobs module represented");
    expectTrue(fuse::core::Phase1TestRegistry::countByModule(fuse::core::Phase1Module::Math) >= 1u,
               "math module represented");
    expectTrue(fuse::core::Phase1TestRegistry::countByModule(fuse::core::Phase1Module::Logging) >= 2u,
               "logging/profiler module represented");
}

void testIntegrationSmoke() {
    expectTrue(fuse::core::Phase1TestRegistry::runIntegrationSmoke(),
               "phase 1 integration smoke ties jobs + math + handles + vfs async + profiler");
}

} // namespace

int main() {
    testChecklistAndModules();
    testIntegrationSmoke();

    if (g_failures == 0) {
        std::printf("fuse_core_phase1_integration: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core_phase1_integration: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
