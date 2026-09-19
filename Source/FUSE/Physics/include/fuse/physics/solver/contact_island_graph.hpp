#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Diagnostic stats for island graph build preflight (B4.4 deepen follow-up).
struct IslandBuildPreflight {
    u32 bodyCount = 0;
    u32 validContactCount = 0;
    u32 validDistanceCount = 0;
    u32 invalidContactCount = 0;
    u32 outOfRangeContactCount = 0;
    u32 outOfRangeDistanceCount = 0;
    bool invalidBodyCount = false;
    bool skipped = false;

    bool can_build() const { return !skipped && !invalidBodyCount; }
    bool has_union_constraints() const { return validContactCount > 0u || validDistanceCount > 0u; }
};

/// True when every referenced body index is in range for `bodyCount`.
bool is_valid_island_build_body_count(u32 bodyCount,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight island graph build without mutating graph state (B4.4 deepen follow-up).
IslandBuildPreflight preflight_island_build(u32 bodyCount,
                                            const std::vector<narrowphase::ContactManifold>& contacts,
                                            const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out when build inputs reference out-of-range body indices (B4.4 deepen follow-up).
bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints);

/// Connected-component partition of bodies/constraints for job-safe PBD iteration.
/// Constraints in different islands may be resolved in parallel; within an island
/// contacts and distance constraints run sequentially (Gauss-Seidel stub).
struct ContactIslandGraph {
    struct Island {
        std::vector<u32> bodyIndices;
        std::vector<u32> contactIndices;
        std::vector<u32> distanceIndices;

        /// True when the island has no contacts or distance constraints (lone body stub).
        bool isEmpty() const { return contactIndices.empty() && distanceIndices.empty(); }
    };

    void build(u32 bodyCount,
               const std::vector<narrowphase::ContactManifold>& contacts,
               const std::vector<DistanceConstraint>& distanceConstraints);

    /// Build only when preflight passes; clears the graph when build is skipped (B4.4 deepen follow-up).
    void build_guarded(u32 bodyCount,
                       const std::vector<narrowphase::ContactManifold>& contacts,
                       const std::vector<DistanceConstraint>& distanceConstraints);

    void clear();

    u32 islandCount() const { return static_cast<u32>(islands_.size()); }
    /// Islands that carry at least one contact or distance constraint.
    u32 constrainedIslandCount() const;
    const Island& island(u32 index) const { return islands_[index]; }

    /// Body → island id, or `invalidIsland` when the body has no constraints.
    u32 bodyIsland(u32 bodyIndex) const;

    static constexpr u32 invalidIsland = ~0u;

private:
    void unionBodies(u32 a, u32 b);
    u32 findRoot(u32 index) const;
    void compressPath(u32 index);

    std::vector<u32> parent_;
    std::vector<Island> islands_;
};

} // namespace fuse::physics
