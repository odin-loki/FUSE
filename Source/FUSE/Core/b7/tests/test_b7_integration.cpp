#include <fuse/core/b7_test_registry.hpp>

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
    const auto& checklist = fuse::core::B7TestRegistry::checklist();
    expectTrue(!checklist.empty(), "B7 checklist non-empty");
    expectTrue(fuse::core::B7TestRegistry::stubLandedCount() >= 9u,
               "at least one stub landed per B7 module area");
    expectTrue(fuse::core::B7TestRegistry::automatedCount() >= 1u, "automated deliverables registered");
    expectTrue(fuse::core::B7TestRegistry::countByModule(fuse::core::B7Module::Animation) >= 1u,
               "animation module represented");
    expectTrue(fuse::core::B7TestRegistry::countByModule(fuse::core::B7Module::Platform) >= 1u,
               "platform module represented");
    expectTrue(fuse::core::B7TestRegistry::countByModule(fuse::core::B7Module::Assets) >= 1u,
               "assets module represented");
}

const fuse::core::B7Deliverable* findRow(const char* id) {
    for (const fuse::core::B7Deliverable& item : fuse::core::B7TestRegistry::checklist()) {
        if (std::strcmp(item.id, id) == 0) {
            return &item;
        }
    }
    return nullptr;
}

void expectGateProven(const char* id, const char* gateTest) {
    const fuse::core::B7Deliverable* row = findRow(id);
    const bool ok = row != nullptr && row->stub_landed && row->automated && row->gate_test != nullptr &&
                    std::strcmp(row->gate_test, gateTest) == 0;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s should be automated and proven by %s\n", id, gateTest);
        ++g_failures;
    }
}

void testGateProvenRows() {
    expectTrue(fuse::core::B7TestRegistry::automatedCount() >= 20u, "B7 gate-proven rows flagged automated");
    for (const fuse::core::B7Deliverable& item : fuse::core::B7TestRegistry::checklist()) {
        if (item.gate_test != nullptr) {
            expectTrue(std::strncmp(item.gate_test, "fuse_", 5) == 0, "gate_test names a fuse CTest");
        }
    }
    expectGateProven("script.lua_hello", "fuse_script_b7_gates");
    expectGateProven("script.hot_reload", "fuse_script_b7_gates");
    expectGateProven("net.enet_process_pair", "fuse_b7_net_gates");
    expectGateProven("net.rollback_resim", "fuse_b7_net_gates");
    expectGateProven("terrain.svo_cave", "fuse_b7_terrain_gates");
    expectGateProven("assets.real_mesh_cook", "fuse_b7_cook_gates");

    const fuse::core::B7Deliverable* reverb = findRow("audio.cuda_reverb");
    expectTrue(reverb != nullptr && reverb->stub_landed && !reverb->automated,
               "audio.cuda_reverb: CPU path landed, CUDA row not automated");

    const fuse::core::B7Deliverable* skinning = findRow("animation.gpu_skinning");
    expectTrue(skinning != nullptr && !skinning->automated, "GPU skinning stays a hardware row");

    const fuse::core::B7Deliverable* occupancy = findRow("vfx.cuda_occupancy");
    expectTrue(occupancy != nullptr && !occupancy->automated, "CUDA occupancy stays a hardware row");
}

void testIntegrationSmoke() {
    expectTrue(fuse::core::B7TestRegistry::runIntegrationSmoke(),
               "B7 integration smoke constructs linked facades");
}

} // namespace

int main() {
    testChecklistAndModules();
    testGateProvenRows();
    testIntegrationSmoke();

    if (g_failures == 0) {
        std::printf("fuse_b7_integration: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_b7_integration: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
