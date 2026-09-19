#pragma once

#include <fuse/physics/config.hpp>
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
/// Aggregate counts for island graph build preflight (B4.4 deepen).
struct IslandBuildStats {
    u32 bodyCount = 0;
    u32 contactCount = 0;
    u32 distanceConstraintCount = 0;
    u32 invalidContacts = 0;
    u32 outOfRangeContacts = 0;
    u32 outOfRangeDistanceConstraints = 0;
    u32 validUnionEdges = 0;

/// Const preflight for island graph build inputs (B4.4 deepen).
struct IslandBuildPreflight {
    IslandBuildStats stats{};
    bool skipped = false;

    bool can_build() const { return !skipped; }

/// Summarize contact/constraint validity before union-find island build.
IslandBuildStats compute_island_build_stats(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `contactIslandGraphBuildRejectReason` matches `expected` (B4.4 deepen follow-up pass).
bool contactIslandGraphBuildRejectsForReason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    ContactIslandGraphBuildRejectReason expected);

/// Read-only island graph build diagnostics — no mutation (B4.4 deepen follow-up pass).
struct ContactIslandGraphBuildPreflight {
    ContactIslandGraphBuildRejectReason reason = ContactIslandGraphBuildRejectReason::None;
    u32 contactSlotCount = 0;
    u32 distanceSlotCount = 0;
    u32 validContactCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;

    bool has_unsafe_refs() const {
        return outOfRangeContactBodyCount > 0u || outOfRangeDistanceBodyCount > 0u;
    }

    bool can_build() const { return reason == ContactIslandGraphBuildRejectReason::None; }

ContactIslandGraphBuildPreflight preflightContactIslandGraphBuild(

/// Non-mutating build skip predicate — inverse of `can_build` (B4.4 deepen follow-up pass).
bool canSkipContactIslandGraphBuild(

/// Non-mutating build predicate — mirrors `preflightContactIslandGraphBuild` (B4.4 deepen follow-up pass).
bool shouldRunContactIslandGraphBuild(
/// Diagnostic reason an island-build input is rejected (B4.4 deepen).
enum class IslandBuildRejectReason : u8 {
    SelfPair,
    OutOfRangeBody,
    InvalidContact,

/// Human-readable label for diagnostics and test assertions (B4.4 deepen).
const char* islandBuildRejectReasonName(IslandBuildRejectReason reason);

/// True when `bodyIndex` is in range for `bodyCount`.
FUSE_PHYSICS_INLINE bool is_valid_body_index(u32 bodyIndex, u32 bodyCount) {
    return bodyCount > 0u && bodyIndex < bodyCount;

/// True when both body indices are in range and distinct.
FUSE_PHYSICS_INLINE bool is_valid_body_pair(u32 bodyA, u32 bodyB, u32 bodyCount) {
    if (bodyA == bodyB) {
        return false;
    if (bodyCount == 0u) {
    return bodyA < bodyCount && bodyB < bodyCount;

/// Returns the first reject reason for a contact manifold input, or `None` when valid.
FUSE_PHYSICS_INLINE IslandBuildRejectReason contactBuildRejectReason(
    const narrowphase::ContactManifold& contact,
    u32 bodyCount) {
    if (!contact.valid) {
        return IslandBuildRejectReason::InvalidContact;
    if (contact.bodyA == contact.bodyB) {
        return IslandBuildRejectReason::SelfPair;
    if (bodyCount == 0u || contact.bodyA >= bodyCount || contact.bodyB >= bodyCount) {
        return IslandBuildRejectReason::OutOfRangeBody;
    return IslandBuildRejectReason::None;

/// Returns the first reject reason for a distance constraint input, or `None` when valid.
FUSE_PHYSICS_INLINE IslandBuildRejectReason distanceBuildRejectReason(
    const DistanceConstraint& constraint,
    if (constraint.bodyA == constraint.bodyB) {
    if (bodyCount == 0u || constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount) {

/// Preflight diagnostics for island graph construction (B4.4 deepen).
    u32 invalidContactCount = 0;
    u32 oobContactCount = 0;
    u32 selfPairContactCount = 0;
    u32 validDistanceCount = 0;
    u32 oobDistanceCount = 0;
    u32 selfPairDistanceCount = 0;


/// Post-build validation of island constraint index references (B4.4 deepen).
struct IslandBuildValidation {
    u32 oobContactIndexCount = 0;
    u32 oobDistanceIndexCount = 0;
    bool valid = true;

/// Preflight island graph inputs before union-find build.
IslandBuildPreflight preflight_island_build(u32 bodyCount,

/// Early-out guard when island build has no bodies and no constraint inputs.
bool should_skip_island_build(u32 bodyCount,
/// Preflight island graph build; sets `skipped` when `bodyCount` is zero.
IslandBuildPreflight preflight_island_graph_build(

/// Early-out guard when there are no bodies to partition.
bool should_skip_island_graph_build(u32 bodyCount);
/// Diagnostic stats for island graph build preflight (B4.4 deepen follow-up).
    u32 outOfRangeContactCount = 0;
    u32 outOfRangeDistanceCount = 0;
    bool invalidBodyCount = false;

    bool can_build() const { return !skipped && !invalidBodyCount; }
    bool has_union_constraints() const { return validContactCount > 0u || validDistanceCount > 0u; }

/// True when every referenced body index is in range for `bodyCount`.
bool is_valid_island_build_body_count(u32 bodyCount,

/// Preflight island graph build without mutating graph state (B4.4 deepen follow-up).

/// Early-out when build inputs reference out-of-range body indices (B4.4 deepen follow-up).

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

    /// Build only when `preflightContactIslandGraphBuild` allows; returns false when skipped (B4.4 deepen follow-up pass).
    bool buildGuarded(u32 bodyCount,
                      const std::vector<narrowphase::ContactManifold>& contacts,
                      const std::vector<DistanceConstraint>& distanceConstraints);
    /// Build only when preflight passes; clears the graph when build is skipped (B4.4 deepen follow-up).
    void build_guarded(u32 bodyCount,

    void clear();

    u32 islandCount() const { return static_cast<u32>(islands_.size()); }
    /// Islands that carry at least one contact or distance constraint.
    u32 constrainedIslandCount() const;
    const Island& island(u32 index) const { return islands_[index]; }

    /// Body → island id, or `invalidIsland` when the body has no constraints.
    u32 bodyIsland(u32 bodyIndex) const;

    static constexpr u32 invalidIsland = ~0u;

    /// Guarded build: returns false when preflight rejects the inputs; graph is cleared on failure.
    bool build_guarded(u32 bodyCount,
                       const std::vector<narrowphase::ContactManifold>& contacts,
                       const std::vector<DistanceConstraint>& distanceConstraints);

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
/// Preflight diagnostics for island graph construction (B4.4 deepen follow-up).
struct IslandBuildPreflight {
    u32 bodyCount = 0;
    u32 contactCount = 0;
    u32 distanceConstraintCount = 0;
    u32 invalidContactCount = 0;
    u32 invalidDistanceCount = 0;
    u32 validContactCount = 0;
    u32 validDistanceCount = 0;
    bool zeroBodies = false;
    bool skipped = false;

    bool can_build() const { return !skipped && !zeroBodies; }
};

/// True when bodyCount is non-zero for island graph construction.
bool is_valid_island_build_body_count(u32 bodyCount);

/// True when both body indices are in range for the given body count.
bool contact_references_valid_bodies(const narrowphase::ContactManifold& contact, u32 bodyCount);

bool distance_constraint_references_valid_bodies(const DistanceConstraint& constraint, u32 bodyCount);

/// Preflight island graph build; sets `skipped` when bodyCount is zero.
IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard for island graph build when bodyCount is zero.
bool should_skip_island_build(u32 bodyCount);

/// Guarded build entry: clears graph and returns false when preflight rejects inputs.
bool build_island_graph_guarded(ContactIslandGraph& graph,

} // namespace fuse::physics
