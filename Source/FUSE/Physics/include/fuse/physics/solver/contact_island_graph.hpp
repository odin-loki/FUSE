#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Input diagnostics for island graph build (B4.4 deepen follow-up).
struct IslandBuildStats {
    u32 bodyCount = 0;
    u32 validContactCount = 0;
    u32 skippedInvalidContactCount = 0;
    u32 validDistanceCount = 0;
    u32 skippedInvalidDistanceCount = 0;
};

/// Preflight guard before `ContactIslandGraph::build` (B4.4 deepen follow-up).
struct IslandBuildPreflight {
    IslandBuildStats stats{};
    bool zeroBodies = false;
    bool skipped = false;

    bool can_build() const { return !skipped && !zeroBodies; }
};

/// True when `bodyCount` is positive for island graph construction.
bool is_valid_island_build_body_count(u32 bodyCount);

/// True when `bodyIndex` is in range for island build inputs.
bool is_valid_island_build_body_index(u32 bodyIndex, u32 bodyCount);

/// True when a contact may participate in island union (valid + in-range bodies).
bool is_valid_island_contact_for_build(const narrowphase::ContactManifold& contact, u32 bodyCount);

/// True when a distance constraint may participate in island union (in-range bodies).
bool is_valid_island_distance_for_build(const DistanceConstraint& constraint, u32 bodyCount);

/// Count contacts that pass `is_valid_island_contact_for_build`.
u32 count_valid_island_build_contacts(const std::vector<narrowphase::ContactManifold>& contacts,
                                      u32 bodyCount);

/// Count distance constraints that pass `is_valid_island_distance_for_build`.
u32 count_valid_island_build_distance_constraints(
    const std::vector<DistanceConstraint>& distanceConstraints,
    u32 bodyCount);

/// Summarize build inputs; sets `skipped` when `bodyCount` is zero.
IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard for island graph build when `bodyCount` is zero.
bool should_skip_island_build(u32 bodyCount);

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

/// Guarded build entry: clears graph and returns false when build preflight fails.
bool build_island_graph_guarded(
    ContactIslandGraph& graph,
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

} // namespace fuse::physics
