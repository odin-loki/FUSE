#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Why island graph build would early-out (B4.4 deepen follow-up).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    ZeroBodies,
    NoConstraints,
};

/// Human-readable label for island build reject reasons (logging / tests).
const char* island_build_reject_reason_name(IslandBuildRejectReason reason);

/// Input sizing for island graph build preflight (B4.4 deepen follow-up).
struct IslandBuildStats {
    u32 bodyCount = 0;
    u32 contactCount = 0;
    u32 validContactCount = 0;
    u32 distanceConstraintCount = 0;
    u32 constraintEdgeCount = 0;
};

/// Read-only island graph build diagnostics — no mutation (B4.4 deepen follow-up).
struct IslandBuildPreflight {
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    IslandBuildStats stats{};
    bool zeroBodies = false;
    bool noConstraints = false;
    bool skipped = false;

    bool can_build() const { return !skipped; }
};

/// Summarize island build inputs without mutating the graph (B4.4 deepen follow-up).
IslandBuildStats compute_island_build_input_stats(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Diagnose why island graph build would skip; vacuously succeeds on populated constrained scenes.
IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_build_reject_reason` matches `expected` (B4.4 deepen follow-up).
bool island_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected);

/// Const preflight for island graph build (B4.4 deepen follow-up).
IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// True when island graph build is a no-op (zero bodies).
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

/// Guarded island graph build; returns false when build is skipped (zero bodies).
bool build_island_graph_guarded(ContactIslandGraph& graph,
                                u32 bodyCount,
                                const std::vector<narrowphase::ContactManifold>& contacts,
                                const std::vector<DistanceConstraint>& distanceConstraints);

} // namespace fuse::physics
