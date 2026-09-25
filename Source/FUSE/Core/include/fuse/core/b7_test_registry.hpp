#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::core {

enum class B7Module : u8 {
    Animation,
    Audio,
    Script,
    Net,
    Terrain,
    WorldPartition,
    Vfx,
    Platform,
    Assets,
};

struct B7Deliverable {
    const char* id = nullptr;
    const char* description = nullptr;
    B7Module module = B7Module::Animation;
    bool stub_landed = false;
    bool automated = false;
    /// CTest name of the gate test that proves this row (nullptr when none does yet).
    const char* gate_test = nullptr;
};

/// B7.10 — Phase 7 deliverable checklist + cross-module integration smoke.
class B7TestRegistry {
public:
    static const std::vector<B7Deliverable>& checklist();
    static u32 countByModule(B7Module module);
    static u32 stubLandedCount();
    static u32 automatedCount();
    static bool runIntegrationSmoke();
};

} // namespace fuse::core
