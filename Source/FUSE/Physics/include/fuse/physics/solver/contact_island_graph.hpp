#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Why island graph build would reject or skip constraint union (B4.4 deepen).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    ZeroBodies,
    OutOfRangeBodies,
};

/// Read-only island graph build diagnostics — no mutation (B4.4 deepen).
struct IslandBuildPreflight {
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    u32 bodyCount = 0;
    u32 validContactCount = 0;
    u32 skippedContactCount = 0;
    u32 validDistanceCount = 0;
    u32 skippedDistanceCount = 0;
    bool skipped = false;

    bool can_build() const { return !skipped && reason == IslandBuildRejectReason::None; }
};

/// True when `bodyIndex` is in range for island graph union.
bool is_valid_island_build_body_index(u32 bodyIndex, u32 bodyCount);

/// Preflight island graph build; flags out-of-range body references without mutating.
IslandBuildPreflight preflight_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when island graph build inputs are invalid.
bool should_skip_island_graph_build(
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

/// Build only when preflight passes; returns false without mutating on reject.
bool build_island_graph_guarded(ContactIslandGraph& graph,
                                u32 bodyCount,
                                const std::vector<narrowphase::ContactManifold>& contacts,
                                const std::vector<DistanceConstraint>& distanceConstraints);

} // namespace fuse::physics
