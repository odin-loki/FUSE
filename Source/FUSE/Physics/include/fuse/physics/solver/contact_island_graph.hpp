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
/// Why island graph build would early-out (B4.4 deepen follow-up).
enum class IslandBuildRejectReason : u8 {
    ZeroBodyCount,
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
/// Preflight diagnostics for island graph build inputs (B4.4 deepen).
    u32 inRangeContactCount = 0;
    u32 outOfRangeContactCount = 0;
    u32 validContactCount = 0;
    u32 inRangeDistanceCount = 0;
/// Diagnostic counts for island build input validation (B4.4 deepen follow-up).
    u32 ownedContactCount = 0;
    u32 ownedDistanceCount = 0;
    u32 outOfRangeDistanceCount = 0;
    bool skipped = false;

    bool can_build() const { return !skipped; }

/// Summarize contact/constraint validity before union-find island build.
IslandBuildStats compute_island_build_stats(
/// Input diagnostics for island graph build (B4.4 deepen follow-up).
    u32 skippedInvalidContactCount = 0;
    u32 validDistanceCount = 0;
    u32 skippedInvalidDistanceCount = 0;

/// Preflight guard before `ContactIslandGraph::build` (B4.4 deepen follow-up).
    bool zeroBodies = false;

    bool can_build() const { return !skipped && !zeroBodies; }



/// True when `bodyCount` is positive for island graph construction.
bool is_valid_island_build_body_count(u32 bodyCount);

/// True when `bodyIndex` is in range for island build inputs.
bool is_valid_island_build_body_index(u32 bodyIndex, u32 bodyCount);

/// True when a contact may participate in island union (valid + in-range bodies).
bool is_valid_island_contact_for_build(const narrowphase::ContactManifold& contact, u32 bodyCount);

/// True when a distance constraint may participate in island union (in-range bodies).
bool is_valid_island_distance_for_build(const DistanceConstraint& constraint, u32 bodyCount);

/// Count contacts that pass `is_valid_island_contact_for_build`.
u32 count_valid_island_build_contacts(const std::vector<narrowphase::ContactManifold>& contacts,
                                      u32 bodyCount);

/// Count distance constraints that pass `is_valid_island_distance_for_build`.
u32 count_valid_island_build_distance_constraints(
    const std::vector<DistanceConstraint>& distanceConstraints,

/// Summarize build inputs; sets `skipped` when `bodyCount` is zero.
/// Why island graph build would reject inputs (B4.4 deepen follow-up).
    EmptyBodyCount,
    InvalidContactBodyIndex,
    InvalidDistanceBodyIndex,
    ZeroBodies,
    NoConstraints,
/// Why island graph build would reject input (B4.4 deepen follow-up).
    OutOfRangeContactBody,
    OutOfRangeDistanceBody,
/// Why island graph build would early-out (B4.4 deepen follow-up pass).

/// Human-readable label for island build reject reasons (logging / tests).
const char* island_build_reject_reason_name(IslandBuildRejectReason reason);

    UnsafeRefs,


/// Input sizing for island graph build without mutating the graph (B4.4 deepen follow-up).
struct ContactIslandBuildInputStats {
    u32 contactSlotCount = 0;
    u32 distanceSlotCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;

/// Summarize island graph build inputs without mutating the graph.
ContactIslandBuildInputStats compute_contact_island_build_input_stats(
/// Input coverage for island graph build (out-of-range body-index guards).
struct IslandBuildInputStats {

/// Input coverage scan for island graph build (out-of-range body-index guards).
struct IslandBuildInputScan {

    bool has_unsafe_refs() const {
        return outOfRangeContactBodyCount > 0u || outOfRangeDistanceBodyCount > 0u;
    }

/// Scan contact/distance refs for out-of-range body indices before `ContactIslandGraph::build`.
IslandBuildInputStats scan_island_build_inputs(
struct IslandGraphBuildStats {

struct IslandGraphBuildPreflight {
    IslandGraphBuildStats stats{};

        return stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u;

    bool can_build() const { return !skipped && !has_unsafe_refs(); }

/// Preflight island graph build inputs; sets `skipped` when nothing can partition.
IslandGraphBuildPreflight preflightIslandGraphBuild(
/// Input coverage for contact island graph build (out-of-range body-index guards).
struct ContactIslandBuildStats {

/// Preflight diagnostics for contact island graph build inputs (B4.4 deepen).
struct ContactIslandBuildPreflight {
    ContactIslandBuildStats stats{};



/// Preflight contact island graph build inputs; sets `skipped` when nothing can partition.
ContactIslandBuildPreflight preflight_contact_island_build(
enum class IslandGraphBuildRejectReason : u8 {
    EmptyInputs,

/// Human-readable label for island graph build reject reasons (B4.4 deepen follow-up pass).
    UnsafeContactRefs,
    UnsafeDistanceRefs,

const char* island_graph_build_reject_reason_name(IslandGraphBuildRejectReason reason);

/// Diagnose why island graph build would skip; vacuously succeeds when build may proceed.
IslandGraphBuildRejectReason island_graph_build_reject_reason(
/// Input coverage for contact-island graph build (out-of-range body-index guards).

/// Preflight diagnostics for contact-island graph build inputs (B4.4 deepen).



/// Preflight contact-island graph build inputs; sets `skipped` when nothing can partition.




/// Outcome for guarded island graph build (skip vs partition).
struct IslandGraphBuildOutcome {
    bool built = false;
    bool unsafeRefs = false;

/// Input coverage stats for island graph build (out-of-range body-index guards).

/// Preflight diagnostics for island graph build inputs (B4.4 deepen follow-up).



/// True when contact body indices are in range for `bodyCount`.
bool contact_refs_in_range(u32 bodyCount, const narrowphase::ContactManifold& contact);

/// True when distance constraint body indices are in range for `bodyCount`.
bool distance_refs_in_range(u32 bodyCount, const DistanceConstraint& constraint);



/// Diagnose why island graph build would reject; vacuously succeeds on safe in-range inputs.
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Returns true when `island_graph_build_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_graph_build_rejects_for_reason(
    IslandGraphBuildRejectReason expected);

/// Read-only island graph build diagnostics with reject reason (B4.4 deepen follow-up pass).
    IslandGraphBuildRejectReason reason = IslandGraphBuildRejectReason::None;

    bool can_build() const { return !skipped && reason == IslandGraphBuildRejectReason::None; }

/// Populate build preflight without mutating a graph (B4.4 deepen follow-up pass).
IslandGraphBuildPreflight preflight_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Diagnose why island graph build would skip; vacuously succeeds on populated in-range scenes.
IslandBuildRejectReason island_build_reject_reason(

/// Returns true when `island_build_reject_reason` matches `expected`.
bool island_build_rejects_for_reason(
    IslandBuildRejectReason expected);

/// True when `bodyIndex` is in range for island graph construction.

/// True when a contact references in-range body indices.
bool contact_references_valid_bodies(const narrowphase::ContactManifold& contact, u32 bodyCount);

/// True when a distance constraint references in-range body indices.
bool distance_constraint_references_valid_bodies(const DistanceConstraint& constraint, u32 bodyCount);

/// Count valid vs invalid contacts and distance constraints for build diagnostics.
    u32 invalidContactCount = 0;
    u32 invalidDistanceCount = 0;

/// Read-only island graph build diagnostics — no mutation (B4.4 deepen follow-up).
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;

    bool can_build() const { return !skipped && reason == IslandBuildRejectReason::None; }

/// Diagnose why island graph build would reject inputs.

/// Populate island build preflight without mutating a graph.
/// Count valid vs out-of-range contacts and distance constraints for build preflight.
struct IslandBuildInput {
    u32 invalidConstraintCount = 0;

    IslandBuildInput input{};


/// Summarize build inputs without mutating a graph (B4.4 deepen follow-up).
IslandBuildInput count_island_build_input(

/// Const preflight for island graph build dispatch (B4.4 deepen follow-up).
/// Input sizing for island graph build preflight (B4.4 deepen follow-up).
    u32 invalidContactBodyRefs = 0;
    u32 invalidDistanceBodyRefs = 0;

/// Preflight diagnostics for `ContactIslandGraph::build` (B4.4 deepen follow-up).
    IslandBuildInputStats stats{};
    bool emptyBodyCount = false;

    bool can_build() const { return !skipped && !emptyBodyCount; }

/// True when `bodyCount` is non-zero for island graph construction.

/// Summarize contacts/constraints referenced by an island build call.
IslandBuildInputStats compute_island_build_input_stats(

/// Preflight island graph build; sets `skipped` when `bodyCount` is zero.
    u32 constraintEdgeCount = 0;

    bool noConstraints = false;


/// Summarize island build inputs without mutating the graph (B4.4 deepen follow-up).
IslandBuildStats compute_island_build_input_stats(

/// Diagnose why island graph build would skip; vacuously succeeds on populated constrained scenes.

/// Returns true when `island_build_reject_reason` matches `expected` (B4.4 deepen follow-up).

/// Const preflight for island graph build (B4.4 deepen follow-up).






/// Why island graph build preflight rejected the inputs (B4.4 deepen follow-up).
enum class IslandBuildRejectReason : u32 {

/// Const name for diagnostics/logging (B4.4 deepen follow-up).


    bool can_build() const { return reason == IslandBuildRejectReason::None; }

/// Count manifolds with valid flag and in-range body indices.
u32 count_valid_island_contacts(
    u32* invalidContactCountOut = nullptr);

/// True when at least one valid contact or distance constraint can partition islands.
bool has_island_build_constraints(

/// Populate island build preflight without mutating a graph (B4.4 deepen follow-up).















    u32 skippedContactCount = 0;
    u32 unionCandidateCount = 0;


/// True when `bodyCount` is valid for island graph construction.

/// Preflight island graph build inputs; sets `skipped` when `bodyCount` is zero but constraints reference bodies.

    bool has_out_of_range_refs() const {
        return outOfRangeContactCount > 0u || outOfRangeDistanceCount > 0u;

/// Populate build preflight without mutating a graph (B4.4 deepen).
bool island_build_rejects_for_reason(u32 bodyCount,

/// True when a contact references a body index outside `[0, bodyCount)`.
bool has_out_of_range_contact_body(u32 bodyCount, const std::vector<narrowphase::ContactManifold>& contacts);

/// True when a distance constraint references a body index outside `[0, bodyCount)`.
bool has_out_of_range_distance_body(u32 bodyCount, const std::vector<DistanceConstraint>& distanceConstraints);

/// Diagnose why island build would skip; vacuously succeeds on in-range inputs.

/// Read-only island build diagnostics — no mutation (B4.4 deepen follow-up).
    u32 distanceCount = 0;


/// Populate island build preflight without mutating the graph.
/// Diagnose why island build would skip; vacuously succeeds when build may proceed.

/// Returns true when `island_build_reject_reason` matches `expected` (B4.4 deepen follow-up pass).

/// Read-only island build diagnostics — no mutation (B4.4 deepen follow-up pass).
    u32 skippedDistanceCount = 0;
    u32 connectableConstraintCount = 0;


/// Populate island build preflight without mutating a graph (B4.4 deepen follow-up pass).
const char* islandBuildRejectReasonName(IslandBuildRejectReason reason);

/// Diagnose why island build would skip or reject constraint wiring.

/// Read-only island build launch diagnostics — no mutation (B4.4 deepen follow-up).


/// Populate island build preflight without mutating the graph (B4.4 deepen follow-up).
IslandBuildPreflight preflight_island_build(

/// Returns true when `contactIslandGraphBuildRejectReason` matches `expected` (B4.4 deepen follow-up pass).
bool contactIslandGraphBuildRejectsForReason(
    ContactIslandGraphBuildRejectReason expected);

/// Read-only island graph build diagnostics — no mutation (B4.4 deepen follow-up pass).
struct ContactIslandGraphBuildPreflight {
    ContactIslandGraphBuildRejectReason reason = ContactIslandGraphBuildRejectReason::None;


    bool can_build() const { return reason == ContactIslandGraphBuildRejectReason::None; }

ContactIslandGraphBuildPreflight preflightContactIslandGraphBuild(

/// Non-mutating build skip predicate — inverse of `can_build` (B4.4 deepen follow-up pass).
bool canSkipContactIslandGraphBuild(

/// Non-mutating build predicate — mirrors `preflightContactIslandGraphBuild` (B4.4 deepen follow-up pass).
bool shouldRunContactIslandGraphBuild(
/// Diagnostic reason an island-build input is rejected (B4.4 deepen).
    SelfPair,
    OutOfRangeBody,
    InvalidContact,

/// Human-readable label for diagnostics and test assertions (B4.4 deepen).

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
    u32 oobContactCount = 0;
    u32 selfPairContactCount = 0;
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
IslandBuildPreflight preflight_island_graph_build(

/// Early-out guard when there are no bodies to partition.
bool should_skip_island_graph_build(u32 bodyCount);
/// Diagnostic stats for island graph build preflight (B4.4 deepen follow-up).
    bool invalidBodyCount = false;

    bool can_build() const { return !skipped && !invalidBodyCount; }
    bool has_union_constraints() const { return validContactCount > 0u || validDistanceCount > 0u; }

/// True when every referenced body index is in range for `bodyCount`.
bool is_valid_island_build_body_count(u32 bodyCount,

/// Preflight island graph build without mutating graph state (B4.4 deepen follow-up).

/// Early-out when build inputs reference out-of-range body indices (B4.4 deepen follow-up).
/// Reject counts for invalid body indices during island graph build (B4.4 deepen).
struct IslandBuildRejectCounts {
    u32 invalidContactPairCount = 0;
    u32 invalidDistancePairCount = 0;

/// Preflight diagnostics for island graph build (B4.4 deepen).
    IslandBuildRejectCounts rejects{};

/// Early-out guard for island graph build when `bodyCount` is zero.
bool should_skip_island_build(u32 bodyCount);
/// Early-out guard for island graph build when inputs are rejected.
/// Returns true when island graph build should be skipped (B4.4 deepen follow-up).
/// True when island graph build is a no-op (zero bodies).
/// Early-out guard when island graph build inputs are degenerate.
/// Early-out guard when island build inputs cannot produce a meaningful graph.
bool should_skip_island_build(
/// Why island graph build would reject or skip constraint union (B4.4 deepen).
    OutOfRangeBodies,

/// Read-only island graph build diagnostics — no mutation (B4.4 deepen).


/// True when `bodyIndex` is in range for island graph union.

/// Preflight island graph build; flags out-of-range body references without mutating.

/// Build only when preflight passes; clears graph and returns false on reject.
bool build_island_graph_guarded(u32 bodyCount,
/// Non-mutating island build skip predicate — inverse of `can_build`.
/// Returns true when island build preflight rejects the inputs (B4.4 deepen follow-up).

/// Returns true when island build preflight accepts the inputs (B4.4 deepen follow-up).
bool can_build_island_graph(u32 bodyCount,
/// Early-out guard when island graph build inputs are invalid.
bool should_skip_island_graph_build(
/// Why island graph build preflight rejected early-out (B4.4 deepen).
    AllConstraintsStale,

/// Const preflight for island graph build dispatch (B4.4 deepen).
    u32 staleContactCount = 0;
    u32 staleDistanceCount = 0;


/// Populate island build preflight without mutating a graph (B4.4 deepen).

/// Returns true when island graph build should be skipped before mutation (B4.4 deepen).

/// True when at least one contact or distance constraint references in-range bodies (B4.4 deepen).
bool has_usable_island_build_constraints(
    bool has_in_range_constraints() const {
        return inRangeContactCount > 0u || inRangeDistanceCount > 0u;

/// True when both contact body indices are in range for `bodyCount`.
bool contact_bodies_in_range(u32 bodyCount, u32 bodyA, u32 bodyB);
    bool is_empty() const {
        return bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u;

bool contact_manifold_bodies_in_range(u32 bodyCount, const narrowphase::ContactManifold& contact);

/// True when both distance-constraint body indices are in range for `bodyCount`.
bool distance_constraint_bodies_in_range(u32 bodyCount, const DistanceConstraint& constraint);

/// Read-only preflight for island graph build inputs; flags out-of-range constraint body refs.

/// Early-out guard when build inputs have no bodies to partition.
/// Build input preflight for island graph construction (B4.4 deepen follow-up).


/// Post-build island partition summary (B4.4 deepen follow-up).
    u32 totalIslands = 0;
    u32 constrainedCount = 0;
    u32 emptyCount = 0;

bool is_island_build_body_index_valid(u32 bodyIndex, u32 bodyCount);

/// True when a contact references in-range bodies for island build.
bool is_contact_valid_for_island_build(const narrowphase::ContactManifold& contact, u32 bodyCount);

/// True when a distance constraint references in-range bodies for island build.
bool is_distance_constraint_valid_for_island_build(const DistanceConstraint& constraint, u32 bodyCount);

/// Preflight island build inputs; sets `skipped` for empty no-op builds.
/// Input validation summary for island graph construction (B4.4 deepen).

    bool has_valid_constraints() const { return validContactCount > 0u || validDistanceCount > 0u; }

/// True when `bodyCount` is usable for island graph construction (B4.4 deepen).

/// Preflight island graph inputs; marks `skipped` when there is nothing to partition.

/// True when both contact body indices are in `[0, bodyCount)`.
bool contact_references_in_range_body(u32 bodyA, u32 bodyB, u32 bodyCount);

/// True when both distance-constraint body indices are in `[0, bodyCount)`.
bool distance_constraint_references_in_range_body(const DistanceConstraint& constraint, u32 bodyCount);

/// Reject reason for island graph build preflight (additive guards only).
    OutOfRangeBodyRef,

    u32 outOfRangeBodyRefCount = 0;
    bool rejected = false;

    bool can_build() const { return !rejected; }
    bool has_constraints() const { return validContactCount > 0u || validDistanceCount > 0u; }

/// Returns a reject reason when inputs reference out-of-range body indices.
/// Input validation for island graph build (B4.4 deepen).
    u32 validInRangeContactCount = 0;

    bool has_buildable_constraints() const {
        return validInRangeContactCount > 0u || inRangeDistanceCount > 0u;

/// Post-build partition summary for build guards (B4.4 deepen).
    u32 orphanContactCount = 0;
    u32 orphanDistanceCount = 0;

/// True when `bodyCount` is usable for island graph build (zero is valid).

/// Preflight island graph inputs; sets `skipped` when there is nothing to partition.
/// Why island graph build would skip (B4.4 deepen follow-up).
enum class ContactIslandGraphBuildRejectReason {
    None,
    SelfContact,

/// Input coverage for island graph build (out-of-range and degenerate ref guards).
struct ContactIslandGraphBuildStats {
    u32 selfContactCount = 0;

    ContactIslandGraphBuildStats stats{};


    bool has_self_contacts() const { return stats.selfContactCount > 0u; }

    bool can_build() const {
        return !skipped && reason == ContactIslandGraphBuildRejectReason::None && !has_unsafe_refs() &&
               !has_self_contacts();

/// Human-readable label for build reject reasons (logging / tests).

/// Diagnose island graph build inputs without mutating a graph.

/// Non-mutating island build skip predicate — inverse of a successful guarded build.
bool can_skip_contact_island_build(

/// Early-out guard when build inputs are an empty no-op.
/// Early-out guard when island build would produce an empty graph with no constraints.
/// Preflight island graph build inputs without mutating a graph.

/// Early-out guard when island build inputs are rejected.
/// Early-out guard when build inputs are empty (zero bodies and no constraints).
/// Non-mutating build skip predicate — inverse of guarded build (B4.4 deepen follow-up).
bool shouldSkipContactIslandGraphBuild(u32 bodyCount,
/// Non-mutating island build predicate — mirrors guarded build eligibility.
bool should_run_contact_island_build(
/// Input coverage for island graph build (out-of-range and degenerate body-index guards).



    bool has_degenerate_refs() const {
        return stats.selfPairContactCount > 0u || stats.selfPairDistanceCount > 0u;


/// Post-build integrity coverage for island body/constraint refs.
struct IslandGraphIntegrityStats {
    u32 islandCount = 0;
    u32 constrainedIslandCount = 0;
    u32 outOfRangeBodyIndexCount = 0;
    u32 outOfRangeContactRefCount = 0;
    u32 outOfRangeDistanceRefCount = 0;

/// Preflight diagnostics for a built island graph (B4.4 deepen follow-up).
struct IslandGraphIntegrityPreflight {
    IslandGraphIntegrityStats stats{};

        return stats.outOfRangeBodyIndexCount > 0u || stats.outOfRangeContactRefCount > 0u ||
               stats.outOfRangeDistanceRefCount > 0u;

    bool can_use() const { return !skipped && !has_unsafe_refs(); }
/// True when build inputs carry no out-of-range body references.
bool island_build_inputs_safe(u32 bodyCount,
/// True when `bodyIndex` refers to a body slot in an island partition build.
inline bool is_valid_island_body_index(u32 bodyIndex, u32 bodyCount) {
    return bodyIndex < bodyCount;

/// True when both body indices are in range for island union during graph build.
inline bool are_island_body_refs_in_range(u32 bodyA, u32 bodyB, u32 bodyCount) {
    return is_valid_island_body_index(bodyA, bodyCount) && is_valid_island_body_index(bodyB, bodyCount);

/// True when a contact manifold references in-range bodies for island build.
inline bool is_in_range_island_contact(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return are_island_body_refs_in_range(contact.bodyA, contact.bodyB, bodyCount);

inline bool is_in_range_island_distance(const DistanceConstraint& constraint, u32 bodyCount) {
    return are_island_body_refs_in_range(constraint.bodyA, constraint.bodyB, bodyCount);
/// Early-out guard when build inputs cannot form any constrained partition.
bool shouldSkipIslandGraphBuild(u32 bodyCount,
bool contact_bodies_in_range(const narrowphase::ContactManifold& contact, u32 bodyCount);

bool distance_bodies_in_range(const DistanceConstraint& constraint, u32 bodyCount);


/// True when `bodyA` and `bodyB` refer to the same body index (degenerate pair).
inline bool constraint_pair_is_degenerate(u32 bodyA, u32 bodyB) {
    return bodyA == bodyB;

/// Build-time counts for in-range-only partition (B4.4 deepen follow-up).
    u32 processedValidContactCount = 0;
    u32 skippedOutOfRangeContactCount = 0;
    u32 processedDistanceCount = 0;
    u32 skippedOutOfRangeDistanceCount = 0;

    bool any_skipped() const {
        return skippedOutOfRangeContactCount > 0u || skippedOutOfRangeDistanceCount > 0u;
bool should_skip_contact_island_build(u32 bodyCount,
/// True when both body indices are valid for a `bodyCount`-body partition.
bool body_pair_in_range(u32 bodyCount, u32 bodyA, u32 bodyB);
/// Read-only scan of island graph build inputs (B4.4 deepen follow-up pass).
struct IslandBuildInputCoverage {

    bool hasUnsafeRefs() const {

    bool isEmptyInput() const {
/// Scan contacts and distance constraints for partition-safe body references.
IslandBuildInputScan scan_island_build_inputs(
/// Coverage counts for island graph build inputs (out-of-range body-index guards).




/// Summarize build inputs without mutating a graph.
IslandGraphBuildStats count_island_graph_build_input(

/// True when scan reports no out-of-range body references.
bool island_build_inputs_safe(const IslandBuildInputScan& scan);

/// True when inputs can form a partition (non-empty bodies or in-range constraints, no unsafe refs).
bool can_partition_island_build_inputs(u32 bodyCount, const IslandBuildInputScan& scan);
/// Const preflight for island graph build dispatch.
IslandGraphBuildPreflight preflight_contact_island_graph_build(

/// Returns true when island graph build should be skipped before mutation.
bool should_skip_contact_island_graph_build(

/// True when both body indices are in range for union during graph build.
bool island_build_body_pair_in_range(u32 bodyCount, u32 bodyA, u32 bodyB);
/// Input coverage for island graph build (out-of-range and self-ref body-index guards).
    u32 selfReferencingContactCount = 0;
    u32 selfReferencingDistanceCount = 0;


        return stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u ||
               stats.selfReferencingContactCount > 0u || stats.selfReferencingDistanceCount > 0u;


/// Post-build partition summary for empty vs constrained island guards.
struct IslandGraphPartitionStats {
/// Returns true when `island_graph_build_reject_reason` matches `expected` (B4.4 deepen follow-up pass).
bool island_graph_build_rejects_for_reason(
    IslandGraphBuildRejectReason expected);

/// True when `bodyIndex` fits the declared body count for island partitioning.
bool island_body_index_in_range(u32 bodyIndex, u32 bodyCount);
/// Non-mutating island graph build skip predicate — inverse of `should_run_island_graph_build`.
bool can_skip_island_graph_build(
bool should_skip_contact_island_build(
/// Diagnostic reason island graph build would reject inputs (B4.4 deepen follow-up).
    OutOfRangeContact,
    OutOfRangeDistance,

/// Human-readable label for island build reject reasons (B4.4 deepen follow-up).

/// Returns the first reject reason for island graph build inputs.




/// Populate island graph build preflight without mutating the graph (B4.4 deepen follow-up).
    UnsafeContactRef,
    UnsafeDistanceRef,

/// Human-readable label for diagnostics and test assertions (B4.4 deepen follow-up).

/// Returns true when `island_graph_build_reject_reason` matches `expected` (B4.4 deepen follow-up).
bool island_graph_build_rejects_for_reason(u32 bodyCount,

    IslandGraphBuildRejectReason reason = IslandGraphBuildRejectReason::None;

    bool can_build() const { return !skipped && reason == IslandGraphBuildRejectReason::None; }

/// Populate graph build preflight without mutating the graph (B4.4 deepen follow-up).
ContactIslandGraphBuildPreflight preflight_contact_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating island graph build predicate — mirrors `island_graph_build_reject_reason`.
bool should_run_island_graph_build(
/// Non-mutating island graph build predicate (B4.4 deepen follow-up).
bool can_build_contact_island_graph(
bool should_skip_island_graph_build(u32 bodyCount,
/// Early-out guard when build inputs cannot form a safe constrained partition.
/// Returns true when graph build should be skipped before partition (B4.4 deepen follow-up).
/// True when island graph build should early-out before union-find (B4.4 deepen follow-up pass).
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Connected-component partition of bodies/constraints for job-safe PBD iteration.
/// Constraints in different islands may be resolved in parallel; within an island
/// contacts and distance constraints run sequentially (Gauss-Seidel stub).
/// Input validation and guarded build entry points live in `pbd_island_solve.hpp` (B4.5 deepen follow-up).
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
    /// Guarded build that skips invalid contact/distance body indices before union-find.
    /// Guarded build wrapper; returns false when preflight rejects inputs.
    bool build_guarded(u32 bodyCount,
    /// Build only when `preflight_island_build` passes; clears and returns false otherwise.
    /// Guarded build entry: returns false when preflight rejects zero-body input.
    /// Guarded build: returns false when preflight skips; otherwise identical to `build`.
    /// Guarded build — returns false when preflight skips the empty no-op path.
    /// Guarded build wrapper; returns false when preflight skips (B4.4 deepen).
    /// Guarded build; clears graph and returns false when preflight rejects inputs.
    /// Guarded build; returns false and clears when inputs are unsafe or empty.
    /// Guarded build; returns false when preflight skips build or rejects unsafe refs.
    /// Build partition using only in-range body refs; skips out-of-range constraints (B4.4 deepen).
    void buildInRange(u32 bodyCount,
                      const std::vector<DistanceConstraint>& distanceConstraints,
                      ContactIslandGraphBuildStats* outStats = nullptr);
    /// Guarded build; returns false and clears when preflight skips build.
    /// Scan build inputs without mutating the graph (B4.4 deepen follow-up pass).
    static IslandBuildInputCoverage scanBuildInputs(
        u32 bodyCount,

    /// True when both contact body indices fit within `bodyCount` (B4.4 deepen follow-up pass).
    static bool contactInRange(const narrowphase::ContactManifold& contact, u32 bodyCount);

    /// True when both distance constraint body indices fit within `bodyCount` (B4.4 deepen follow-up pass).
    static bool distanceInRange(const DistanceConstraint& constraint, u32 bodyCount);

    /// True when scanned inputs have no out-of-range constraint refs (B4.4 deepen follow-up pass).
    static bool canAcceptBuildInputs(const IslandBuildInputCoverage& coverage);

    /// Guarded build; clears and returns false when inputs are empty or unsafe (B4.4 deepen follow-up pass).
    /// Build only when `preflight_contact_island_graph_build` passes; clears and returns false otherwise.
    /// Guarded build; clears and returns false when `island_graph_build_reject_reason` is non-None.
    /// Guarded build; returns false when preflight skips build or refs are out of range.
    /// Build while skipping out-of-range constraint refs (additive safe-build stub).
    void build_skipping_unsafe_refs(u32 bodyCount,
    /// Guarded build; clears graph and returns `skipped` when preflight rejects inputs.
    IslandGraphBuildOutcome build_guarded(u32 bodyCount,
    /// Guarded build; returns false and clears when preflight skips unsafe inputs.
    /// Guarded build; clears the graph and returns false when preflight rejects inputs.

    void clear();

    u32 islandCount() const { return static_cast<u32>(islands_.size()); }
    /// True when the graph contains at least one body partition (B4.4 deepen follow-up).
    bool has_islands() const { return islandCount() > 0u; }
    /// Islands that carry at least one contact or distance constraint.
    u32 constrainedIslandCount() const;
    /// True when at least one island carries contacts or distance constraints (B4.4 deepen follow-up).
    bool has_constrained_islands() const { return constrainedIslandCount() > 0u; }
    const Island& island(u32 index) const { return islands_[index]; }

    /// Body → island id, or `invalidIsland` when the body has no constraints.
    u32 bodyIsland(u32 bodyIndex) const;

    /// True when both body indices are in range for union during graph build.
    static bool bodiesInRange(u32 bodyCount, u32 bodyA, u32 bodyB);
    /// Islands with no contacts or distance constraints.
    u32 emptyIslandCount() const;

    /// Summarize constrained vs empty islands after build.
    IslandGraphPartitionStats computePartitionStats() const;

    /// Preflight build inputs; sets `skipped` when nothing can partition.
    static IslandGraphBuildPreflight preflightBuildInputs(
        u32 bodyCount,
        const std::vector<narrowphase::ContactManifold>& contacts,
        const std::vector<DistanceConstraint>& distanceConstraints);

    /// Early-out guard when build inputs cannot form any constrained partition.
    static bool shouldSkipBuild(u32 bodyCount,

    /// Guarded build; returns false and clears the graph when preflight rejects inputs.
    bool buildGuarded(u32 bodyCount,

    static constexpr u32 invalidIsland = ~0u;

    /// Guarded build: returns false when preflight rejects the inputs; graph is cleared on failure.
    bool build_guarded(u32 bodyCount,
                       const std::vector<narrowphase::ContactManifold>& contacts,
                       const std::vector<DistanceConstraint>& distanceConstraints);
    /// Guarded build; returns false when preflight skips build.
    bool buildGuarded(u32 bodyCount,
    /// True when `islandIndex` is in range for graph accessors (B4.4 deepen follow-up).
    bool islandIndexInRange(u32 islandIndex) const { return islandIndex < islandCount(); }
    /// Guarded build entry; returns false when build inputs fail validation (B4.5 deepen follow-up).
    /// True when both body indices are in range for graph partition.
    static bool bodies_in_range(u32 bodyA, u32 bodyB, u32 bodyCount);

    /// True when a constraint references the same body on both ends.
    static bool is_self_contact(u32 bodyA, u32 bodyB);
    /// True when `bodyIndex` is in range for union-find partitioning.
    static bool partitionBodyInRange(u32 bodyCount, u32 bodyIndex);

    /// True when both contact body indices are in range for partitioning.
    static bool contactPartitionInRange(u32 bodyCount, const narrowphase::ContactManifold& contact);

    /// True when both distance-constraint body indices are in range for partitioning.
    static bool distancePartitionInRange(u32 bodyCount, const DistanceConstraint& constraint);

    /// Count valid contacts whose body indices are in range for partitioning.
    static u32 countUnionableContacts(u32 bodyCount,
                                      const std::vector<narrowphase::ContactManifold>& contacts);

    /// Count distance constraints whose body indices are in range for partitioning.
    static u32 countUnionableDistanceConstraints(u32 bodyCount,
                                                 const std::vector<DistanceConstraint>& constraints);
    /// True when both body indices are in `[0, bodyCount)`.
    static bool isBodyPairInRange(u32 bodyA, u32 bodyB, u32 bodyCount);

    /// True when a contact references in-range bodies.
    static bool isContactInRange(const narrowphase::ContactManifold& contact, u32 bodyCount);

    /// True when a distance constraint references in-range bodies.
    static bool isDistanceConstraintInRange(const DistanceConstraint& constraint, u32 bodyCount);
    /// Input coverage for graph build (out-of-range body-index guards, B4.4 deepen follow-up).
    struct BuildStats {
        u32 bodyCount = 0;
        u32 validContactCount = 0;
        u32 inRangeContactCount = 0;
        u32 skippedOutOfRangeContactCount = 0;
        u32 distanceSlotCount = 0;
        u32 inRangeDistanceCount = 0;
        u32 skippedOutOfRangeDistanceCount = 0;
    };

    /// Why graph build would early-out (B4.4 deepen follow-up).
    enum class BuildRejectReason : u8 {
        None = 0,
        EmptyInputs,
        OutOfRangeContactBodies,
        OutOfRangeDistanceBodies,

    /// Human-readable label for graph build reject reasons (logging / tests).
    static const char* buildRejectReasonName(BuildRejectReason reason);

    /// Diagnose why build would reject; vacuously succeeds when build may proceed.
    static BuildRejectReason buildRejectReason(
        u32 bodyCount,

    /// Returns true when `buildRejectReason` matches `expected` (B4.4 deepen follow-up).
    static bool buildRejectsForReason(
        const std::vector<DistanceConstraint>& distanceConstraints,
        BuildRejectReason expected);

    /// Read-only build diagnostics — no mutation (B4.4 deepen follow-up).
    struct BuildPreflight {
        BuildRejectReason reason = BuildRejectReason::None;
        BuildStats stats{};
        bool rejected = false;

        bool can_build() const { return !rejected; }

    /// Populate build preflight without mutating the graph (B4.4 deepen follow-up).
    static BuildPreflight preflightBuild(

    /// Non-mutating build skip predicate — inverse of `preflightBuild().can_build()` (B4.4 deepen follow-up).
    static bool shouldSkipBuild(

    /// Guarded build; clears the graph and returns false when preflight rejects inputs.

private:
    bool bodyIndexInRange(u32 bodyIndex) const;
    void unionBodies(u32 a, u32 b);
    u32 findRoot(u32 index) const;
    void compressPath(u32 index);

    std::vector<u32> parent_;
    std::vector<u32> bodyToIsland_;
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
/// Preflight island graph build; counts invalid contacts and out-of-range body indices.
IslandBuildPreflight preflight_island_build(
/// Guarded build entry: clears graph and returns false when build preflight fails.
bool build_island_graph_guarded(
    ContactIslandGraph& graph,
/// Why island graph build would early-out (B4.4 deepen pass).
enum class IslandBuildRejectReason : u8 {
    None = 0,
    ZeroBodyCount,
    StaleContactBodyRefs,
    StaleDistanceBodyRefs,

/// Human-readable label for island build reject reasons (logging / tests).
const char* islandBuildRejectReasonName(IslandBuildRejectReason reason);

/// Read-only island build diagnostics — no mutation (B4.4 deepen pass).
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    u32 distanceCount = 0;
    u32 staleContactRefCount = 0;
    u32 staleDistanceRefCount = 0;
    bool zeroBodyCount = false;

    bool can_build() const { return !zeroBodyCount; }
    bool inputs_clean() const { return reason == IslandBuildRejectReason::None; }

/// Diagnose island build inputs without mutating a graph.
/// Preflight island graph build inputs; sets `skipped` when nothing can partition.
IslandGraphBuildPreflight preflight_island_graph_build(
/// Post-build integrity counts for island graph consistency checks (B4.4 deepen).
struct IslandGraphIntegrityStats {
    u32 islandCount = 0;
    u32 constrainedIslandCount = 0;
    u32 orphanedContactRefCount = 0;
    u32 orphanedDistanceRefCount = 0;
    u32 outOfRangeBodyIndexCount = 0;

/// Preflight diagnostics for built island graph consistency (B4.4 deepen).
struct IslandGraphIntegrityPreflight {
    IslandGraphIntegrityStats stats{};

    bool is_consistent() const {
        return !skipped && stats.orphanedContactRefCount == 0u && stats.orphanedDistanceRefCount == 0u &&
               stats.outOfRangeBodyIndexCount == 0u;
    }

/// Validate a built graph against body/constraint slot coverage; sets `skipped` for empty graphs.
IslandGraphIntegrityPreflight preflight_island_graph_integrity(
    const ContactIslandGraph& graph,
/// Validate body-index coverage before graph build (B4.5 deepen follow-up pass).
bool island_graph_build_inputs_valid(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard for island graph build when bodyCount is zero.
bool should_skip_island_build(u32 bodyCount);

/// Guarded build entry: clears graph and returns false when preflight rejects inputs.
bool build_island_graph_guarded(ContactIslandGraph& graph,
/// Early-out guard when body count is zero.

/// True when every contact and distance constraint references in-range body indices.
bool island_build_inputs_valid(u32 bodyCount,
/// Guarded build entry: skips when `bodyCount` is zero, otherwise delegates to `build`.
/// Guarded island graph build; returns false when build is skipped (zero bodies).
/// Build only when preflight passes; returns false when build is skipped (B4.4 deepen follow-up).
/// Guarded build: runs `build` only when preflight passes; returns false when skipped.
bool build_contact_island_graph_guarded(ContactIslandGraph& graph,
/// Guarded island build; returns false when preflight rejects input.
bool build_guarded(ContactIslandGraph& graph,
/// Non-mutating island build skip predicate — vacuous zero-body early-out (B4.4 deepen pass).
bool should_skip_island_build(u32 bodyCount,

/// Guarded build entry: returns false without mutating when body count is zero.
/// Build only when preflight passes; returns false without mutating on reject.
/// Summarize constrained vs empty islands after build.
IslandBuildStats compute_island_build_stats(const ContactIslandGraph& graph);
/// Guarded build: clears and returns when preflight skips; otherwise delegates to `build`.
void build_island_graph_guarded(ContactIslandGraph& graph,
/// Guarded build entry; returns false when build inputs are skipped.

/// Summarize built graph vs input preflight orphan counts.
IslandBuildStats compute_island_build_stats(const ContactIslandGraph& graph,
                                            const IslandBuildPreflight& inputPreflight);
/// Early-out guard when build inputs cannot form any constrained partition.
bool should_skip_island_graph_build(u32 bodyCount,

/// Guarded island graph build; returns false when preflight skips build.

/// Preflight post-build integrity for body and constraint index coverage.
IslandGraphIntegrityPreflight preflight_island_graph_integrity(const ContactIslandGraph& graph,
                                                                u32 contactSlotCount,
                                                                u32 distanceSlotCount);

/// Early-out guard when a built graph carries out-of-range body or constraint refs.
bool should_skip_island_graph_integrity(const ContactIslandGraph& graph,
/// Early-out guard when a built graph fails integrity preflight.
/// True when contact body indices are within `[0, bodyCount)`.
bool contact_body_indices_in_range(const narrowphase::ContactManifold& contact, u32 bodyCount);

/// True when distance constraint body indices are within `[0, bodyCount)`.
bool distance_body_indices_in_range(const DistanceConstraint& constraint, u32 bodyCount);

/// True when `bodyA` and `bodyB` refer to the same body index (degenerate pair).
bool constraint_pair_is_degenerate(u32 bodyA, u32 bodyB);
/// Why body union would reject during island graph build (B4.4 deepen follow-up pass).
enum class IslandUnionRejectReason : u8 {
    OutOfRangeBodyA,
    OutOfRangeBodyB,

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
