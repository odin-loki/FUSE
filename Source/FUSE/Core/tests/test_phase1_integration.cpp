#include <fuse/core/phase1_test_registry.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

const fuse::core::Phase1Deliverable* findRow(const char* id) {
    for (const fuse::core::Phase1Deliverable& item : fuse::core::Phase1TestRegistry::checklist()) {
        if (std::strcmp(item.id, id) == 0) {
            return &item;
        }
    }
    return nullptr;
}

void testGateProvenRows() {
    expectTrue(fuse::core::Phase1TestRegistry::automatedCount() >= 18u, "B1.8 gate-proven rows flagged automated");
    for (const fuse::core::Phase1Deliverable& item : fuse::core::Phase1TestRegistry::checklist()) {
        if (item.gate_test != nullptr) {
            expectTrue(item.automated && std::strncmp(item.gate_test, "fuse_core_b1_", 13) == 0,
                       "gate-proven rows are automated and name a fuse_core_b1_* CTest");
        }
    }
    const fuse::core::Phase1Deliverable* sdf = findRow("math.sdf_primitives");
    expectTrue(sdf != nullptr && sdf->automated && sdf->gate_test != nullptr &&
                   std::strcmp(sdf->gate_test, "fuse_core_b1_math_gates") == 0,
               "math.sdf_primitives proven by fuse_core_b1_math_gates");
    // Ring integrity is gated, but the logger is still mutex-serialised: the lock-free row stays open.
    const fuse::core::Phase1Deliverable* ring = findRow("logging.async_ring");
    expectTrue(ring != nullptr && !ring->automated, "logging.async_ring stays open until the logger is lock-free");
}

void testIntegrationSmoke() {
    expectTrue(fuse::core::Phase1TestRegistry::runIntegrationSmoke(),
               "phase 1 integration smoke ties jobs + math + handles + vfs async + profiler");
}

} // namespace

int main() {
    testChecklistAndModules();
    testGateProvenRows();
    testIntegrationSmoke();

    if (g_failures == 0) {
        std::printf("fuse_core_phase1_integration: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core_phase1_integration: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
