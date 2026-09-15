#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::core {

enum class Phase1Module : u8 {
    Build,
    Types,
    Memory,
    Math,
    Jobs,
    Logging,
    Platform,
};

struct Phase1Deliverable {
    const char* id = nullptr;
    const char* description = nullptr;
    Phase1Module module = Phase1Module::Build;
    bool stub_landed = false;
    bool automated = false;
};

/// B1.8 — Phase 1 core deliverable checklist + cross-subsystem integration smoke.
class Phase1TestRegistry {
public:
    static const std::vector<Phase1Deliverable>& checklist();
    static u32 countByModule(Phase1Module module);
    static u32 stubLandedCount();
    static u32 automatedCount();
    static bool runIntegrationSmoke();
};

} // namespace fuse::core
