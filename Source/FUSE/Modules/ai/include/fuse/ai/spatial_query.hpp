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
[[nodiscard]] bool is_valid_ally_radius(float radius);
/// True when `radius` is positive and finite — required for count/filter leaves.
[[nodiscard]] bool is_finite_ally_radius(float radius);
/// True when `radius` is zero or negative — nearest-ally max-radius unlimited sentinel.
[[nodiscard]] bool is_unlimited_radius(float radius);
[[nodiscard]] bool radius_filter_policy_is_valid(const RadiusFilterPolicy& policy);
[[nodiscard]] float effective_radius(const RadiusFilterPolicy& policy);
[[nodiscard]] u32 effective_min_count(const RadiusFilterPolicy& policy);
[[nodiscard]] float radius_sq_from_policy(const RadiusFilterPolicy& policy);

/// Count same-team allies outside `radius` of (`x`, `y`), excluding `selfIndex`.
[[nodiscard]] u32 count_allies_outside_radius(u32 selfIndex,
                                              u32 teamId,
                                              float x,
                                              float y,
                                              float radius,
                                              const std::vector<AllyCandidate>& allies);

/// Count same-team allies within `radius` of (`x`, `y`), excluding `selfIndex`.
[[nodiscard]] u32 count_allies_in_radius(u32 selfIndex,
                                           u32 teamId,
                                           float x,
                                           float y,
                                           float radius,
                                           const std::vector<AllyCandidate>& allies);

/// Collect ally indices on the same team within `radius` of (`x`, `y`), excluding `selfIndex`.
[[nodiscard]] u32 filter_allies_in_radius(u32 selfIndex,
                                          u32 teamId,
                                          float x,
                                          float y,
                                          float radius,
                                          const std::vector<AllyCandidate>& allies,
                                          std::vector<u32>& outIndices);

/// Find the nearest same-team ally (excluding self). Ties break toward the lowest `agentIndex`.
/// Returns `found=false` when none exist.
[[nodiscard]] NearestAllyResult find_nearest_ally(u32 selfIndex,
                                                  u32 teamId,
                                                  float x,
                                                  float y,
                                                  const std::vector<AllyCandidate>& allies);

/// Like `find_nearest_ally`, but only considers allies within `maxRadius` (0 = unlimited).
[[nodiscard]] NearestAllyResult find_nearest_ally_within_radius(u32 selfIndex,
                                                                u32 teamId,
                                                                float x,
                                                                float y,
                                                                float maxRadius,
                                                                const std::vector<AllyCandidate>& allies);

/// Success when at least `policy.minCount` allies lie within `policy.radius`.
[[nodiscard]] bool allies_in_radius_satisfied(u32 selfIndex,
                                              u32 teamId,
                                              float x,
                                              float y,
                                              const RadiusFilterPolicy& policy,
                                              const std::vector<AllyCandidate>& allies);

/// True when no same-team ally lies within `radius` (excludes self).
[[nodiscard]] bool has_no_allies_in_radius(u32 selfIndex,
                                           u32 teamId,
                                           float x,
                                           float y,
                                           float radius,
                                           const std::vector<AllyCandidate>& allies);

/// True when at least one same-team ally lies within `radius` (excludes self).
[[nodiscard]] bool has_any_ally_in_radius(u32 selfIndex,
                                          u32 teamId,
                                          float x,
                                          float y,
                                          float radius,
                                          const std::vector<AllyCandidate>& allies);

/// Squared distance to the nearest same-team ally; returns 0 when none exist.
[[nodiscard]] float nearest_ally_distance_sq(u32 selfIndex,
                                             u32 teamId,
                                             float x,
                                             float y,
                                             const std::vector<AllyCandidate>& allies);

/// True when `allies` is non-null and contains at least one entry.
[[nodiscard]] bool ally_context_available(const std::vector<AllyCandidate>* allies);

} // namespace fuse::ai
