#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Read-only scan of island graph build inputs (B4.4 deepen follow-up pass).
struct IslandBuildInputCoverage {
    u32 bodyCount = 0;
    u32 contactSlotCount = 0;
    u32 distanceSlotCount = 0;
    u32 validContactCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactCount = 0;
    u32 outOfRangeDistanceCount = 0;

    bool hasUnsafeRefs() const {
        return outOfRangeContactCount > 0u || outOfRangeDistanceCount > 0u;
    }

    bool isEmptyInput() const {
        return bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u;
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

    /// Scan build inputs without mutating the graph (B4.4 deepen follow-up pass).
    static IslandBuildInputCoverage scanBuildInputs(
        u32 bodyCount,
        const std::vector<narrowphase::ContactManifold>& contacts,
        const std::vector<DistanceConstraint>& distanceConstraints);

    /// True when both contact body indices fit within `bodyCount` (B4.4 deepen follow-up pass).
    static bool contactInRange(const narrowphase::ContactManifold& contact, u32 bodyCount);

    /// True when both distance constraint body indices fit within `bodyCount` (B4.4 deepen follow-up pass).
    static bool distanceInRange(const DistanceConstraint& constraint, u32 bodyCount);

    /// True when scanned inputs have no out-of-range constraint refs (B4.4 deepen follow-up pass).
    static bool canAcceptBuildInputs(const IslandBuildInputCoverage& coverage);

    /// Guarded build; clears and returns false when inputs are empty or unsafe (B4.4 deepen follow-up pass).
    bool buildGuarded(u32 bodyCount,
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
