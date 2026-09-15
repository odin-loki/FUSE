#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::phase7 {

enum class Phase7Module : u8 {
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

struct Phase7Deliverable {
    const char* id = nullptr;
    const char* description = nullptr;
    Phase7Module module = Phase7Module::Animation;
    bool stub_landed = false;
    bool automated = false;
};

/// B7.10 — Phase 7 deliverable checklist + cross-module integration smoke.
class Phase7TestRegistry {
public:
    static const std::vector<Phase7Deliverable>& checklist();
    static u32 countByModule(Phase7Module module);
    static u32 stubLandedCount();
    static u32 automatedCount();
    static bool runIntegrationSmoke();
};

} // namespace fuse::phase7
