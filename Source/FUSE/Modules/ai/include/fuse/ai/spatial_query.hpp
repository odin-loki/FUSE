#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::ai {

/// One agent considered for ally spatial queries (read-only snapshot slice).
struct AllyCandidate {
    u32 agentIndex = 0;
    float x = 0.f;
    float y = 0.f;
    u32 teamId = 0;
};

/// Result of a nearest-ally lookup — stub until navmesh / faction masks land.
struct NearestAllyResult {
    bool found = false;
    u32 allyIndex = 0;
    float distanceSq = 0.f;
};

/// Radius filter policy — mirrors B7.4 interest radii without unload hysteresis.
struct RadiusFilterPolicy {
    float radius = 0.f;
    u32 minCount = 1;
};

[[nodiscard]] float distance_sq_2d(float ax, float ay, float bx, float by);
[[nodiscard]] bool within_radius(float distanceSq, float radius);
[[nodiscard]] float effective_radius(const RadiusFilterPolicy& policy);

/// Collect ally indices on the same team within `radius` of (`x`, `y`), excluding `selfIndex`.
[[nodiscard]] u32 filter_allies_in_radius(u32 selfIndex,
                                          u32 teamId,
                                          float x,
                                          float y,
                                          float radius,
                                          const std::vector<AllyCandidate>& allies,
                                          std::vector<u32>& outIndices);

/// Find the nearest same-team ally (excluding self). Returns `found=false` when none exist.
[[nodiscard]] NearestAllyResult find_nearest_ally(u32 selfIndex,
                                                  u32 teamId,
                                                  float x,
                                                  float y,
                                                  const std::vector<AllyCandidate>& allies);

/// Success when at least `policy.minCount` allies lie within `policy.radius`.
[[nodiscard]] bool allies_in_radius_satisfied(u32 selfIndex,
                                              u32 teamId,
                                              float x,
                                              float y,
                                              const RadiusFilterPolicy& policy,
                                              const std::vector<AllyCandidate>& allies);

} // namespace fuse::ai
