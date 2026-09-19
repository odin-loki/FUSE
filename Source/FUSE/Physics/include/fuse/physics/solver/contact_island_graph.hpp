#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Why island graph build would reject input (B4.4 deepen follow-up).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    OutOfRangeContactBody,
    OutOfRangeDistanceBody,
};

/// Human-readable label for island build reject reasons (logging / tests).
const char* island_build_reject_reason_name(IslandBuildRejectReason reason);

/// Returns true when `island_build_reject_reason` matches `expected`.
bool island_build_rejects_for_reason(u32 bodyCount,
                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     IslandBuildRejectReason expected);

/// True when a contact references a body index outside `[0, bodyCount)`.
bool has_out_of_range_contact_body(u32 bodyCount, const std::vector<narrowphase::ContactManifold>& contacts);

/// True when a distance constraint references a body index outside `[0, bodyCount)`.
bool has_out_of_range_distance_body(u32 bodyCount, const std::vector<DistanceConstraint>& distanceConstraints);

/// Diagnose why island build would skip; vacuously succeeds on in-range inputs.
IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Read-only island build diagnostics — no mutation (B4.4 deepen follow-up).
struct IslandBuildPreflight {
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    u32 bodyCount = 0;
    u32 contactCount = 0;
    u32 distanceCount = 0;
    u32 outOfRangeContactCount = 0;
    u32 outOfRangeDistanceCount = 0;
    bool skipped = false;

    bool can_build() const { return !skipped && reason == IslandBuildRejectReason::None; }
};

/// Populate island build preflight without mutating the graph.
IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating island build skip predicate — inverse of `can_build`.
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

/// Guarded island build; returns false when preflight rejects input.
bool build_guarded(ContactIslandGraph& graph,
                   u32 bodyCount,
                   const std::vector<narrowphase::ContactManifold>& contacts,
                   const std::vector<DistanceConstraint>& distanceConstraints);

} // namespace fuse::physics
