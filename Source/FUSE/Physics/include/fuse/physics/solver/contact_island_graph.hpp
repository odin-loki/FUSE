#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Preflight diagnostics for island graph build inputs (B4.4 deepen).
struct IslandBuildPreflight {
    u32 bodyCount = 0;
    u32 validContactCount = 0;
    u32 skippedContactCount = 0;
    u32 inRangeContactCount = 0;
    u32 outOfRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeDistanceCount = 0;
    u32 unionCandidateCount = 0;
    bool skipped = false;

    bool can_build() const { return !skipped; }
};

/// True when `bodyCount` is valid for island graph construction.
bool is_valid_island_build_body_count(u32 bodyCount);

/// Preflight island graph build inputs; sets `skipped` when `bodyCount` is zero but constraints reference bodies.
IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when island graph build inputs are degenerate.
bool should_skip_island_build(
    u32 bodyCount,
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

/// Guarded build: runs `build` only when preflight passes; returns false when skipped.
bool build_contact_island_graph_guarded(ContactIslandGraph& graph,
                                        u32 bodyCount,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints);

} // namespace fuse::physics
