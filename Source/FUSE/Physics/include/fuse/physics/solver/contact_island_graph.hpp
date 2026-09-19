#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Why island graph build preflight rejected the inputs (B4.4 deepen follow-up).
enum class IslandBuildRejectReason : u32 {
    None = 0,
    EmptyBodyCount,
};

/// Const name for diagnostics/logging (B4.4 deepen follow-up).
const char* island_build_reject_reason_name(IslandBuildRejectReason reason);

/// Read-only island graph build diagnostics — no mutation (B4.4 deepen follow-up).
struct IslandBuildPreflight {
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    u32 bodyCount = 0;
    u32 validContactCount = 0;
    u32 distanceConstraintCount = 0;
    u32 invalidContactCount = 0;
    u32 invalidDistanceCount = 0;
    bool noConstraints = false;
    bool skipped = false;

    bool can_build() const { return reason == IslandBuildRejectReason::None; }
};

/// Count manifolds with valid flag and in-range body indices.
u32 count_valid_island_contacts(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    u32* invalidContactCountOut = nullptr);

/// True when at least one valid contact or distance constraint can partition islands.
bool has_island_build_constraints(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Populate island build preflight without mutating a graph (B4.4 deepen follow-up).
IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when island graph build should be skipped (B4.4 deepen follow-up).
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

/// Build only when preflight passes; returns false when build is skipped (B4.4 deepen follow-up).
bool build_island_graph_guarded(ContactIslandGraph& graph,
                                u32 bodyCount,
                                const std::vector<narrowphase::ContactManifold>& contacts,
                                const std::vector<DistanceConstraint>& distanceConstraints);

} // namespace fuse::physics
