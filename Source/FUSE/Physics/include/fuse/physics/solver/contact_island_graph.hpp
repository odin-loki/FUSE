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

/// Why body union would reject during island graph build (B4.4 deepen follow-up pass).
enum class IslandUnionRejectReason : u8 {
    None = 0,
    OutOfRangeBodyA,
    OutOfRangeBodyB,
};

/// Human-readable label for island-union reject reasons (logging / tests).
const char* islandUnionRejectReasonName(IslandUnionRejectReason reason);

/// Diagnose why union would skip; vacuously succeeds when union may proceed.
IslandUnionRejectReason islandUnionRejectReason(u32 bodyCount, u32 bodyA, u32 bodyB);

/// Returns true when `islandUnionRejectReason` matches `expected` (B4.4 deepen follow-up pass).
bool islandUnionRejectsForReason(u32 bodyCount, u32 bodyA, u32 bodyB, IslandUnionRejectReason expected);

/// Non-mutating union predicate — inverse of `islandUnionRejectReason` (B4.4 deepen follow-up pass).
bool bodies_in_union_range(u32 bodyCount, u32 bodyA, u32 bodyB);

/// Non-mutating build-range predicate for valid contacts (B4.4 deepen follow-up pass).
bool contact_in_island_build_range(u32 bodyCount, const narrowphase::ContactManifold& contact);

/// Non-mutating build-range predicate for distance constraints (B4.4 deepen follow-up pass).
bool distance_in_island_build_range(u32 bodyCount, const DistanceConstraint& constraint);

} // namespace fuse::physics
