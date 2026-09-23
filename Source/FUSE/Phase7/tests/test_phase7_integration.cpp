#include <fuse/phase7/phase7_test_registry.hpp>

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

const fuse::phase7::Phase7Deliverable* findRow(const char* id) {
    for (const fuse::phase7::Phase7Deliverable& item : fuse::phase7::Phase7TestRegistry::checklist()) {
        if (std::strcmp(item.id, id) == 0) {
            return &item;
        }
    }
    return nullptr;
}

// Rows proven by a B7 gate CTest must be flagged automated and name that test.
void expectGateProven(const char* id, const char* gateTest) {
    const fuse::phase7::Phase7Deliverable* row = findRow(id);
    const bool ok = row != nullptr && row->stub_landed && row->automated && row->gate_test != nullptr &&
                    std::strcmp(row->gate_test, gateTest) == 0;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s should be automated and proven by %s\n", id, gateTest);
        ++g_failures;
    }
}

void testGateProvenRows() {
    expectTrue(fuse::phase7::Phase7TestRegistry::automatedCount() >= 20u, "B7 gate-proven rows flagged automated");
    for (const fuse::phase7::Phase7Deliverable& item : fuse::phase7::Phase7TestRegistry::checklist()) {
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

    // CPU reverb is proven, the CUDA FFT path is hardware-only: landed, not automated.
    const fuse::phase7::Phase7Deliverable* reverb = findRow("audio.cuda_reverb");
    expectTrue(reverb != nullptr && reverb->stub_landed && !reverb->automated,
               "audio.cuda_reverb: CPU path landed, CUDA row not automated");
    // Hardware-only rows stay open.
    const fuse::phase7::Phase7Deliverable* skinning = findRow("animation.gpu_skinning");
    expectTrue(skinning != nullptr && !skinning->automated, "GPU skinning stays a hardware row");
    const fuse::phase7::Phase7Deliverable* occupancy = findRow("vfx.cuda_occupancy");
    expectTrue(occupancy != nullptr && !occupancy->automated, "CUDA occupancy stays a hardware row");
}

void testIntegrationSmoke() {
    expectTrue(fuse::phase7::Phase7TestRegistry::runIntegrationSmoke(),
               "phase 7 integration smoke constructs linked facades");
}

} // namespace

int main() {
    testChecklistAndModules();
    testGateProvenRows();
    testIntegrationSmoke();

    if (g_failures == 0) {
        std::printf("fuse_phase7_integration: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_phase7_integration: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
