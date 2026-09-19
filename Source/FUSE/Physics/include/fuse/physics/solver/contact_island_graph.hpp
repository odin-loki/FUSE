#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Input coverage for island graph build (out-of-range body-index guards).
struct IslandGraphBuildStats {
    u32 bodyCount = 0;
    u32 contactSlotCount = 0;
    u32 distanceSlotCount = 0;
    u32 validContactCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactCount = 0;
    u32 outOfRangeDistanceCount = 0;
};

/// Preflight diagnostics for island graph build inputs (B4.4 deepen).
struct IslandGraphBuildPreflight {
    IslandGraphBuildStats stats{};
    bool skipped = false;

    bool has_unsafe_refs() const {
        return stats.outOfRangeContactCount > 0u || stats.outOfRangeDistanceCount > 0u;
    }

    bool can_build() const { return !skipped && !has_unsafe_refs(); }

    /// True when a filtered build can still partition in-range constraints.
    bool can_build_filtered() const {
        return !skipped && (stats.inRangeContactCount > 0u || stats.inRangeDistanceCount > 0u ||
                            stats.bodyCount > 0u);
    }
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

    /// Build partition using only in-range contacts and distance constraints.
    void buildFiltered(u32 bodyCount,
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
    static bool isContactInBodyRange(const narrowphase::ContactManifold& contact, u32 bodyCount);
    static bool isDistanceInBodyRange(const DistanceConstraint& constraint, u32 bodyCount);
    void unionBodies(u32 a, u32 b);
    u32 findRoot(u32 index) const;
    void compressPath(u32 index);

    std::vector<u32> parent_;
    std::vector<Island> islands_;
};

/// True when both contact body indices are within `[0, bodyCount)`.
bool is_contact_in_body_range(const narrowphase::ContactManifold& contact, u32 bodyCount);

/// True when both distance-constraint body indices are within `[0, bodyCount)`.
bool is_distance_in_body_range(const DistanceConstraint& constraint, u32 bodyCount);

/// Preflight island graph build inputs; sets `skipped` when nothing can partition.
IslandGraphBuildPreflight preflight_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when build inputs cannot form any constrained partition.
bool should_skip_graph_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded graph build; returns false when preflight skips build.
bool build_graph_guarded(ContactIslandGraph& graph,
                         u32 bodyCount,
                         const std::vector<narrowphase::ContactManifold>& contacts,
                         const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded filtered build; skips out-of-range refs and returns false when nothing remains.
bool build_graph_filtered_guarded(ContactIslandGraph& graph,
                                  u32 bodyCount,
                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                  const std::vector<DistanceConstraint>& distanceConstraints);

} // namespace fuse::physics
