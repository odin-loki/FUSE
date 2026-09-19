#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Input coverage for island graph build (out-of-range and degenerate body-index guards).
struct IslandGraphBuildStats {
    u32 bodyCount = 0;
    u32 contactSlotCount = 0;
    u32 distanceSlotCount = 0;
    u32 validContactCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 selfPairContactCount = 0;
    u32 selfPairDistanceCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;
};

/// Preflight diagnostics for island graph build inputs (B4.4 deepen follow-up).
struct IslandGraphBuildPreflight {
    IslandGraphBuildStats stats{};
    bool skipped = false;

    bool has_unsafe_refs() const {
        return stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u;
    }

    bool has_degenerate_refs() const {
        return stats.selfPairContactCount > 0u || stats.selfPairDistanceCount > 0u;
    }

    bool can_build() const { return !skipped && !has_unsafe_refs(); }
};

/// Post-build integrity coverage for island body/constraint refs.
struct IslandGraphIntegrityStats {
    u32 islandCount = 0;
    u32 constrainedIslandCount = 0;
    u32 outOfRangeBodyIndexCount = 0;
    u32 outOfRangeContactRefCount = 0;
    u32 outOfRangeDistanceRefCount = 0;
};

/// Preflight diagnostics for a built island graph (B4.4 deepen follow-up).
struct IslandGraphIntegrityPreflight {
    IslandGraphIntegrityStats stats{};
    bool skipped = false;

    bool has_unsafe_refs() const {
        return stats.outOfRangeBodyIndexCount > 0u || stats.outOfRangeContactRefCount > 0u ||
               stats.outOfRangeDistanceRefCount > 0u;
    }

    bool can_use() const { return !skipped && !has_unsafe_refs(); }
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

    static constexpr u32 invalidIsland = ~0u;

    /// Guarded build; returns false when preflight skips build.
    bool buildGuarded(u32 bodyCount,
                      const std::vector<narrowphase::ContactManifold>& contacts,
                      const std::vector<DistanceConstraint>& distanceConstraints);

private:
    void unionBodies(u32 a, u32 b);
    u32 findRoot(u32 index) const;
    void compressPath(u32 index);

    std::vector<u32> parent_;
    std::vector<Island> islands_;
};

/// Preflight island graph build inputs; sets `skipped` when nothing can partition.
IslandGraphBuildPreflight preflight_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when build inputs cannot form any constrained partition.
bool should_skip_island_graph_build(u32 bodyCount,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island graph build; returns false when preflight skips build.
bool build_island_graph_guarded(ContactIslandGraph& graph,
                                u32 bodyCount,
                                const std::vector<narrowphase::ContactManifold>& contacts,
                                const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight post-build integrity for body and constraint index coverage.
IslandGraphIntegrityPreflight preflight_island_graph_integrity(const ContactIslandGraph& graph,
                                                                u32 bodyCount,
                                                                u32 contactSlotCount,
                                                                u32 distanceSlotCount);

/// Early-out guard when a built graph carries out-of-range body or constraint refs.
bool should_skip_island_graph_integrity(const ContactIslandGraph& graph,
                                        u32 bodyCount,
                                        u32 contactSlotCount,
                                        u32 distanceSlotCount);

} // namespace fuse::physics
