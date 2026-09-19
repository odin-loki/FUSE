#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Input coverage for island graph build (out-of-range and self-ref body-index guards).
struct IslandGraphBuildStats {
    u32 bodyCount = 0;
    u32 contactSlotCount = 0;
    u32 distanceSlotCount = 0;
    u32 validContactCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;
    u32 selfReferencingContactCount = 0;
    u32 selfReferencingDistanceCount = 0;
};

/// Preflight diagnostics for island graph build inputs (B4.4 deepen).
struct IslandGraphBuildPreflight {
    IslandGraphBuildStats stats{};
    bool skipped = false;

    bool has_unsafe_refs() const {
        return stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u ||
               stats.selfReferencingContactCount > 0u || stats.selfReferencingDistanceCount > 0u;
    }

    bool can_build() const { return !skipped && !has_unsafe_refs(); }
};

/// Post-build partition summary for empty vs constrained island guards.
struct IslandGraphPartitionStats {
    u32 totalIslands = 0;
    u32 constrainedCount = 0;
    u32 emptyCount = 0;
};

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

    void clear();

    u32 islandCount() const { return static_cast<u32>(islands_.size()); }
    /// Islands that carry at least one contact or distance constraint.
    u32 constrainedIslandCount() const;
    const Island& island(u32 index) const { return islands_[index]; }

    /// Body → island id, or `invalidIsland` when the body has no constraints.
    u32 bodyIsland(u32 bodyIndex) const;

    /// Islands with no contacts or distance constraints.
    u32 emptyIslandCount() const;

    /// Summarize constrained vs empty islands after build.
    IslandGraphPartitionStats computePartitionStats() const;

    /// Preflight build inputs; sets `skipped` when nothing can partition.
    static IslandGraphBuildPreflight preflightBuildInputs(
        u32 bodyCount,
        const std::vector<narrowphase::ContactManifold>& contacts,
        const std::vector<DistanceConstraint>& distanceConstraints);

    /// Early-out guard when build inputs cannot form any constrained partition.
    static bool shouldSkipBuild(u32 bodyCount,
                                const std::vector<narrowphase::ContactManifold>& contacts,
                                const std::vector<DistanceConstraint>& distanceConstraints);

    /// Guarded build; returns false and clears the graph when preflight rejects inputs.
    bool buildGuarded(u32 bodyCount,
                      const std::vector<narrowphase::ContactManifold>& contacts,
                      const std::vector<DistanceConstraint>& distanceConstraints);

    static constexpr u32 invalidIsland = ~0u;

private:
    void unionBodies(u32 a, u32 b);
    u32 findRoot(u32 index) const;
    void compressPath(u32 index);

    std::vector<u32> parent_;
    std::vector<Island> islands_;
};

} // namespace fuse::physics
