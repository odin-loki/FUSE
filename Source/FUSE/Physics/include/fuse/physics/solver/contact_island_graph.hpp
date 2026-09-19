#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Build input preflight for island graph construction (B4.4 deepen follow-up).
struct IslandBuildPreflight {
    u32 bodyCount = 0;
    u32 validContactCount = 0;
    u32 staleContactCount = 0;
    u32 validDistanceCount = 0;
    u32 staleDistanceCount = 0;
    bool skipped = false;

    bool can_build() const { return !skipped; }
};

/// Post-build island partition summary (B4.4 deepen follow-up).
struct IslandBuildStats {
    u32 totalIslands = 0;
    u32 constrainedCount = 0;
    u32 emptyCount = 0;
};

/// True when `bodyIndex` is in range for island build inputs.
bool is_island_build_body_index_valid(u32 bodyIndex, u32 bodyCount);

/// True when a contact references in-range bodies for island build.
bool is_contact_valid_for_island_build(const narrowphase::ContactManifold& contact, u32 bodyCount);

/// True when a distance constraint references in-range bodies for island build.
bool is_distance_constraint_valid_for_island_build(const DistanceConstraint& constraint, u32 bodyCount);

/// Preflight island build inputs; sets `skipped` for empty no-op builds.
IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when build inputs are an empty no-op.
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

    /// Guarded build — returns false when preflight skips the empty no-op path.
    bool build_guarded(u32 bodyCount,
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

/// Summarize constrained vs empty islands after build.
IslandBuildStats compute_island_build_stats(const ContactIslandGraph& graph);

} // namespace fuse::physics
