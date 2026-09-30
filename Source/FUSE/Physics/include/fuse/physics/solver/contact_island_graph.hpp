#pragma once

#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

/// Why island graph build would reject inputs (B4.4 deepen follow-up pass).
enum class ContactIslandGraphBuildRejectReason : u8 {
    None = 0,
    EmptyInput,
    OutOfRangeContactBodies,
    OutOfRangeDistanceBodies,
};

/// Human-readable label for island graph build reject reasons (logging / tests).
const char* contactIslandGraphBuildRejectReasonName(ContactIslandGraphBuildRejectReason reason);

/// Diagnose why build would reject; vacuously succeeds when build may proceed.
ContactIslandGraphBuildRejectReason contactIslandGraphBuildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `contactIslandGraphBuildRejectReason` matches `expected` (B4.4 deepen follow-up pass).
bool contactIslandGraphBuildRejectsForReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    ContactIslandGraphBuildRejectReason expected);

/// Read-only island graph build diagnostics — no mutation (B4.4 deepen follow-up pass).
struct ContactIslandGraphBuildPreflight {
    ContactIslandGraphBuildRejectReason reason = ContactIslandGraphBuildRejectReason::None;
    u32 bodyCount = 0;
    u32 contactSlotCount = 0;
    u32 distanceSlotCount = 0;
    u32 validContactCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;
    bool skipped = false;

    bool has_unsafe_refs() const {
        return outOfRangeContactBodyCount > 0u || outOfRangeDistanceBodyCount > 0u;
    }

    bool can_build() const { return reason == ContactIslandGraphBuildRejectReason::None; }
};

ContactIslandGraphBuildPreflight preflightContactIslandGraphBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating build skip predicate — inverse of `can_build` (B4.4 deepen follow-up pass).
bool canSkipContactIslandGraphBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating build predicate — mirrors `preflightContactIslandGraphBuild` (B4.4 deepen follow-up pass).
bool shouldRunContactIslandGraphBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Connected-component partition of bodies/constraints for job-safe PBD iteration.
/// Constraints in different islands may be resolved in parallel; within an island
/// contacts and distance constraints run sequentially (Gauss-Seidel stub).
struct ContactIslandGraph {
    /// Read-only view of one island's index list. The indices live in the graph's flat (CSR) arrays,
    /// so a rebuild reuses the same storage and never allocates within the reserve()d limits, however
    /// the islands merge or split between steps.
    struct IndexSpan {
        const u32* ptr = nullptr;
        u32 count = 0;

        const u32* begin() const { return ptr; }
        const u32* end() const { return ptr + count; }
        const u32* data() const { return ptr; }
        usize size() const { return count; }
        bool empty() const { return count == 0u; }
        u32 operator[](usize i) const { return ptr[i]; }
    };

    struct Island {
        IndexSpan bodyIndices;
        IndexSpan contactIndices;
        IndexSpan distanceIndices;

        /// True when the island has no contacts or distance constraints (lone body stub).
        bool isEmpty() const { return contactIndices.empty() && distanceIndices.empty(); }
    };

    ContactIslandGraph() = default;
    ContactIslandGraph(const ContactIslandGraph& other);
    ContactIslandGraph& operator=(const ContactIslandGraph& other);
    ContactIslandGraph(ContactIslandGraph&&) noexcept = default;
    ContactIslandGraph& operator=(ContactIslandGraph&&) noexcept = default;

    /// Pre-sizes every internal array for up to maxBodies bodies, maxContacts contact slots and
    /// maxConstraints distance constraints: build() within those limits performs no heap allocation.
    void reserve(u32 maxBodies, u32 maxContacts, u32 maxConstraints);

    void build(u32 bodyCount,
               const std::vector<narrowphase::ContactManifold>& contacts,
               const std::vector<DistanceConstraint>& distanceConstraints);

    /// Build only when `preflightContactIslandGraphBuild` allows; returns false when skipped (B4.4 deepen follow-up pass).
    bool buildGuarded(u32 bodyCount,
                      const std::vector<narrowphase::ContactManifold>& contacts,
                      const std::vector<DistanceConstraint>& distanceConstraints);

    void clear();

    u32 islandCount() const { return islandCount_; }
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

    /// Points every live island's spans at this graph's own flat arrays (after a build or a copy).
    void rebindSpans_();

    struct Range {
        u32 bodyBegin = 0;
        u32 bodyCount = 0;
        u32 contactBegin = 0;
        u32 contactCount = 0;
        u32 distanceBegin = 0;
        u32 distanceCount = 0;
    };

    std::vector<u32> parent_;
    /// Island slots; only the first islandCount_ are live. Each island's indices are a contiguous
    /// range of flatBodies_/flatContacts_/flatDistances_ (counting sort), so storage is bounded by
    /// the body/contact/constraint counts rather than by per-island high-water marks (B1.8).
    std::vector<Island> islands_;
    std::vector<Range> ranges_;
    u32 islandCount_ = 0;
    std::vector<u32> rootToIsland_;
    std::vector<u32> flatBodies_;
    std::vector<u32> flatContacts_;
    std::vector<u32> flatDistances_;
    std::vector<u32> cursor_;
};

} // namespace fuse::physics
