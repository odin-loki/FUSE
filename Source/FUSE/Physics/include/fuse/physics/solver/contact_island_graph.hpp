#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

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

private:
    void unionBodies(u32 a, u32 b);
    u32 findRoot(u32 index) const;
    void compressPath(u32 index);

    std::vector<u32> parent_;
    std::vector<Island> islands_;
};

/// Post-build integrity counts for island graph consistency checks (B4.4 deepen).
struct IslandGraphIntegrityStats {
    u32 islandCount = 0;
    u32 constrainedIslandCount = 0;
    u32 orphanedContactRefCount = 0;
    u32 orphanedDistanceRefCount = 0;
    u32 outOfRangeBodyIndexCount = 0;
};

/// Preflight diagnostics for built island graph consistency (B4.4 deepen).
struct IslandGraphIntegrityPreflight {
    IslandGraphIntegrityStats stats{};
    bool skipped = false;

    bool is_consistent() const {
        return !skipped && stats.orphanedContactRefCount == 0u && stats.orphanedDistanceRefCount == 0u &&
               stats.outOfRangeBodyIndexCount == 0u;
    }
};

/// Validate a built graph against body/constraint slot coverage; sets `skipped` for empty graphs.
IslandGraphIntegrityPreflight preflight_island_graph_integrity(
    const ContactIslandGraph& graph,
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when a built graph fails integrity preflight.
bool should_skip_island_graph_integrity(const ContactIslandGraph& graph,
                                        u32 bodyCount,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints);

} // namespace fuse::physics
