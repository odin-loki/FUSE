#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Diagnostic reason an island-build input is rejected (B4.4 deepen).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    SelfPair,
    OutOfRangeBody,
    InvalidContact,
};

/// Human-readable label for diagnostics and test assertions (B4.4 deepen).
const char* islandBuildRejectReasonName(IslandBuildRejectReason reason);

/// True when `bodyIndex` is in range for `bodyCount`.
FUSE_PHYSICS_INLINE bool is_valid_body_index(u32 bodyIndex, u32 bodyCount) {
    return bodyCount > 0u && bodyIndex < bodyCount;
}

/// True when both body indices are in range and distinct.
FUSE_PHYSICS_INLINE bool is_valid_body_pair(u32 bodyA, u32 bodyB, u32 bodyCount) {
    if (bodyA == bodyB) {
        return false;
    }
    if (bodyCount == 0u) {
        return false;
    }
    return bodyA < bodyCount && bodyB < bodyCount;
}

/// Returns the first reject reason for a contact manifold input, or `None` when valid.
FUSE_PHYSICS_INLINE IslandBuildRejectReason contactBuildRejectReason(
    const narrowphase::ContactManifold& contact,
    u32 bodyCount) {
    if (!contact.valid) {
        return IslandBuildRejectReason::InvalidContact;
    }
    if (contact.bodyA == contact.bodyB) {
        return IslandBuildRejectReason::SelfPair;
    }
    if (bodyCount == 0u || contact.bodyA >= bodyCount || contact.bodyB >= bodyCount) {
        return IslandBuildRejectReason::OutOfRangeBody;
    }
    return IslandBuildRejectReason::None;
}

/// Returns the first reject reason for a distance constraint input, or `None` when valid.
FUSE_PHYSICS_INLINE IslandBuildRejectReason distanceBuildRejectReason(
    const DistanceConstraint& constraint,
    u32 bodyCount) {
    if (constraint.bodyA == constraint.bodyB) {
        return IslandBuildRejectReason::SelfPair;
    }
    if (bodyCount == 0u || constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount) {
        return IslandBuildRejectReason::OutOfRangeBody;
    }
    return IslandBuildRejectReason::None;
}

/// Preflight diagnostics for island graph construction (B4.4 deepen).
struct IslandBuildPreflight {
    u32 bodyCount = 0;
    u32 validContactCount = 0;
    u32 invalidContactCount = 0;
    u32 oobContactCount = 0;
    u32 selfPairContactCount = 0;
    u32 validDistanceCount = 0;
    u32 oobDistanceCount = 0;
    u32 selfPairDistanceCount = 0;
    bool skipped = false;

    bool can_build() const { return !skipped; }
};

/// Post-build validation of island constraint index references (B4.4 deepen).
struct IslandBuildValidation {
    u32 oobContactIndexCount = 0;
    u32 oobDistanceIndexCount = 0;
    bool valid = true;
};

/// Preflight island graph inputs before union-find build.
IslandBuildPreflight preflight_island_build(u32 bodyCount,
                                            const std::vector<narrowphase::ContactManifold>& contacts,
                                            const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when island build has no bodies and no constraint inputs.
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

/// Validate island constraint index references against contact/distance slot counts.
IslandBuildValidation validate_island_indices(const ContactIslandGraph& graph,
                                              u32 contactCount,
                                              u32 distanceCount);

} // namespace fuse::physics
