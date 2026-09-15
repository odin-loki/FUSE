#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::physics {

enum class Phase4TestCategory : u8 {
    BroadPhase,
    NarrowPhase,
    Solver,
    Ccd,
    Destruction,
    SoftBody,
    Integration,
    Performance,
};

struct Phase4TestCase {
    const char* id = nullptr;
    const char* description = nullptr;
    Phase4TestCategory category = Phase4TestCategory::Integration;
    bool automated = false;
    bool passed = false;
};

/// B4.11 — Phase 4 deliverable checklist registry (narrative + future acceptance tests).
class Phase4TestRegistry {
public:
    static const std::vector<Phase4TestCase>& catalog();
    static u32 countByCategory(Phase4TestCategory category);
    static u32 automatedCount();
    static bool runAutomatedSmoke();
};

} // namespace fuse::physics
