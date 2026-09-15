#pragma once

#include <fuse/types.hpp>

namespace fuse::platform {

/// Compile-time + runtime platform profile (architecture-parallel §3.1.1).
enum class PlatformProfile {
    Desktop,
    Mobile
};

PlatformProfile activeProfile();
bool isMobileProfile();
bool isDesktopProfile();

struct JobProfileLimits {
    u32 reserve = 2;
    u32 minWorkers = 1;
    u32 maxWorkers = 16;
    u32 fiberStackBytes = 64u * 1024u;
    u32 ioBudgetMicrosPerFrame = 8000u;
};

JobProfileLimits jobProfileLimitsForProfile(PlatformProfile profile);
JobProfileLimits currentJobProfileLimits();

/// Test hook — pass Desktop/Mobile to override; invalid sentinel clears override.
PlatformProfile setActiveProfileOverride(PlatformProfile profile);
void clearActiveProfileOverride();

} // namespace fuse::platform
