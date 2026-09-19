#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::narrowphase {

/// SoA contact storage with clear/reuse for frame-to-frame narrowphase output (B4.3 deepen).
struct ContactBufferSoA {
    std::vector<vec3> contactPoints;
    std::vector<vec3> contactNormals;
    std::vector<f32> penetrationDepths;
    std::vector<f32> minSeparations;
    std::vector<u32> bodyA;
    std::vector<u32> bodyB;
    std::vector<u8> validFlags;
    std::vector<u8> pointCounts;
    std::vector<vec3> pointSlots;
    std::vector<f32> pointPenetrations;
    std::vector<f32> warmNormalImpulses;
    std::vector<vec2> warmTangentImpulses;
    std::vector<vec3> tangent1;
    std::vector<vec3> tangent2;

    u32 activeCount = 0;
    u32 pairSlotCount = 0;
    u32 maxCapacity = 0;
    u32 droppedCount = 0;

    bool isEmpty() const { return activeCount == 0u; }
    bool hasValidContacts() const { return activeCount > 0u; }
    bool hasDroppedContacts() const { return droppedCount > 0u; }
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    bool canAcceptContacts(u32 additionalCount = 1u) const;
    u32 remainingCapacity() const;
    bool canApplyMaxCapacityClamp() const;
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    bool canSkipCompaction() const;
    /// True when clamping dropped one or more finalized contacts.
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    /// True when slot storage has no invalid flags (compact is a no-op).
    /// True when post-pass truncation would drop contacts.
    /// Inverse of `canApplyMaxCapacityClamp` (B4.4 deepen pass).
    /// True when `maxCapacity` is set and no additional contacts may be written.
    /// Remaining write slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;
    /// True when both dense and slot storage are empty (safe to skip SoA scans) (B4.4 deepen follow-up pass).
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.4 deepen follow-up pass).
    /// True when active count is within `maxCapacity` (clamp is a no-op) (B4.4 deepen follow-up pass).
    bool canSkipMaxCapacityClamp() const;
    /// True when compact+clamp would leave the buffer unchanged (B4.4 deepen follow-up pass).
    bool canSkipCompactAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction (B4.4 deepen follow-up pass).

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const ContactManifold& manifold);
    void applyWarmStartStub(u32 slot, ContactManifold& manifold) const;
    void buildFrictionTangentBases();
    void buildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    /// Rebuild friction tangents only when orthonormal frames are missing (B4.4 deepen follow-up pass).
    void buildFrictionTangentBasesIfNeeded();
    u32 compact();
    u32 applyMaxCapacityClamp();
    u32 compactAndClamp();
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.4 deepen follow-up pass).
    bool canSkipCompaction() const;
    /// True when post-pass truncation would drop contacts (B4.4 deepen follow-up pass).
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.4 deepen follow-up pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// Rebuild friction tangents only for slots with missing or stale bases (B4.4 deepen follow-up pass).
    void buildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

    /// Non-mutating write skip predicate — mirrors `preflightContactBufferWrite` (B4.4 deepen guard pass).
    bool canSkipWrite(u32 slot, const ContactManifold& manifold) const;

    /// Non-mutating compaction skip predicate — inverse of `shouldRunCompaction` (B4.4 deepen guard pass).
    bool canSkipCompaction() const;

    /// Non-mutating clamp skip predicate — inverse of `shouldRunClamp` (B4.4 deepen guard pass).
    bool canSkipClamp() const;
    /// True when slot storage has no valid flags (compact is a no-op).
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.4 deepen guard pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.5 deepen pass).
    /// True when post-pass truncation would drop contacts (B4.5 deepen pass).
    /// Inverse of `canApplyMaxCapacityClamp` (B4.5 deepen pass).
    /// Count valid flags in prepared slot storage before compaction (B4.5 deepen pass).
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;
    /// Returns true when slot is in range and manifold is valid with distinct bodies (B4.5 deepen pass).
    static bool canWriteSlot(u32 slot, u32 pairSlotCount, const ContactManifold& manifold);

    /// Write slot only when buffer preflight passes (B4.5 deepen pass).
    /// Write only when `preflight_contact_buffer_write` allows (B4.4 deepen pass follow-up).
    bool writeSlotIfValid(u32 slot, const ContactManifold& manifold);

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for contact-buffer compaction reject reasons (B4.6 deepen pass).
const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen pass).
ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.6 deepen pass).
/// Why contact-buffer write would reject (B4.4 deepen guard pass).
enum class ContactBufferWriteRejectReason : u8 {
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,

const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);

ContactBufferWriteRejectReason contact_buffer_write_reject_reason(
/// Why contact-buffer write would reject (B4.4 deepen pass).
    InvalidSlot,
/// Why contact-buffer slot write would reject (B4.4 deepen pass).
    OutOfRange,

/// Human-readable label for contact-buffer write reject reasons (logging / tests).
const char* contactBufferWriteRejectReasonName(ContactBufferWriteRejectReason reason);

/// Diagnose why write would reject; vacuously succeeds when write may proceed.
ContactBufferWriteRejectReason contactBufferWriteRejectReason(


/// Why contact-buffer write would reject (B4.5 deepen pass).

/// Human-readable label for contact-buffer write reject reasons (B4.5 deepen pass).

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.5 deepen pass).
/// Why contact-buffer write would reject (B4.4 guard pass).

/// Human-readable label for contact-buffer write reject reasons (B4.4 guard pass).

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.4 guard pass).
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

bool contact_buffer_write_rejects_for_reason(
    const ContactManifold& manifold,
    ContactBufferWriteRejectReason expected);

struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool outOfRangeSlot = false;
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.4 deepen pass).
bool contactBufferWriteRejectsForReason(

/// Read-only write diagnostics — no mutation (B4.4 deepen pass).
    bool invalidSlot = false;

    bool outOfRange = false;
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.4 deepen guard pass).

/// Read-only write diagnostics — no mutation (B4.4 deepen guard pass).

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.4 deepen pass).


/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.5 deepen pass).

/// Read-only write diagnostics — no mutation (B4.5 deepen pass).
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.4 guard pass).

/// Read-only write diagnostics — no mutation (B4.4 guard pass).
    bool invalidManifold = false;
    bool selfPair = false;

    bool canWrite() const { return reason == ContactBufferWriteRejectReason::None; }

ContactBufferWritePreflight preflight_contact_buffer_write(

/// Why contact-buffer compaction would early-out (B4.4 deepen guard pass).
    AllValid,



bool contact_buffer_compaction_rejects_for_reason(
    ContactBufferCompactionRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }

/// Populate compaction preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.6 deepen pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.6 deepen pass).
bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-basis rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionBasisRejectReason : u8 {
    NoValidManifolds,

/// Human-readable label for contact-buffer friction-basis reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_basis_reject_reason_name(ContactBufferFrictionBasisRejectReason reason);

/// Diagnose why friction-basis rebuild would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_friction_basis_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_basis_rejects_for_reason(
    ContactBufferFrictionBasisRejectReason expected);

/// Read-only friction-basis rebuild diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionBasisPreflight {
    ContactBufferFrictionBasisRejectReason reason = ContactBufferFrictionBasisRejectReason::None;
    bool noValidManifolds = false;

    bool needsRebuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }

/// Populate friction-basis preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_basis(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis skip predicate — inverse of `needsRebuild` (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_basis(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis predicate — mirrors `preflight_contact_buffer_friction_basis` (B4.6 deepen pass).
bool should_run_contact_buffer_friction_basis(const ContactBufferSoA& buffer);
/// Read-only write-slot diagnostics — no mutation (B4.4 deepen follow-up pass).

    bool canWrite() const { return !invalidManifold && !selfPair; }

ContactBufferWritePreflight preflight_contact_buffer_write(const ContactManifold& manifold);

/// Read-only compaction diagnostics — no mutation (B4.4 deepen follow-up pass).
    bool allValid = false;

    bool needsCompaction() const { return !emptyBuffer && !allValid; }


/// Read-only max-capacity clamp diagnostics — no mutation (B4.4 deepen follow-up pass).
struct ContactBufferClampPreflight {
    bool withinCapacity = false;

    bool needsClamp() const { return !emptyBuffer && !withinCapacity; }

ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Read-only friction-basis rebuild diagnostics at the SoA layer (B4.4 deepen follow-up pass).
struct ContactBufferFrictionPreflight {
    u32 staleSlotCount = 0u;
    u32 rebuildSlotCount = 0u;

    bool can_skip_rebuild() const { return emptyBuffer || rebuildSlotCount == 0u; }

ContactBufferFrictionPreflight preflight_contact_buffer_friction_rebuild(
    f32 epsilon = 1e-4f);

/// Non-mutating friction rebuild skip predicate (B4.4 deepen follow-up pass).
bool can_skip_build_friction_tangent_bases(



bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer);

bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.4 deepen guard pass).

ContactBufferWritePreflight preflightContactBufferWrite(




/// Non-mutating write skip predicate — inverse of `canWrite` (B4.4 deepen pass).
bool can_skip_contact_buffer_write(

/// Non-mutating write predicate — mirrors `preflight_contact_buffer_write` (B4.4 deepen pass).
bool should_run_contact_buffer_write(

/// Why contact-buffer compaction would early-out (B4.4 deepen pass).
    AllInvalid,




/// Non-mutating write skip predicate — inverse of `canWrite` (B4.4 deepen guard pass).
bool canSkipContactBufferWrite(

/// Non-mutating write predicate — mirrors `preflightContactBufferWrite` (B4.4 deepen guard pass).
bool shouldRunContactBufferWrite(

enum class ContactBufferCompactRejectReason : u8 {

/// Human-readable label for contact-buffer compaction reject reasons (logging / tests).
const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed.
/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed.
ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.4 deepen pass).
bool contactBufferCompactionRejectsForReason(

/// Read-only compaction diagnostics — no mutation (B4.4 deepen pass).





/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.4 deepen pass).

    bool allInvalid = false;

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.4 deepen guard pass).

/// Read-only compaction diagnostics — no mutation (B4.4 deepen guard pass).



/// Why contact-buffer compaction would early-out (B4.5 deepen pass).

/// Human-readable label for contact-buffer compaction reject reasons (B4.5 deepen pass).

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.5 deepen pass).

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.5 deepen pass).

/// Read-only compaction diagnostics — no mutation (B4.5 deepen pass).


ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.4 deepen pass).

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.4 deepen pass).


/// Why contact-buffer max-capacity clamp would early-out (B4.4 deepen pass).
/// Non-mutating compaction predicate — inverse of `canSkipCompaction` (B4.4 deepen guard pass).



const char* contactBufferCompactRejectReasonName(ContactBufferCompactRejectReason reason);

ContactBufferCompactRejectReason contactBufferCompactRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferCompactRejectReason` matches `expected` (B4.4 deepen guard pass).
bool contactBufferCompactRejectsForReason(
    ContactBufferCompactRejectReason expected);

struct ContactBufferCompactPreflight {
    ContactBufferCompactRejectReason reason = ContactBufferCompactRejectReason::None;

    bool needsCompaction() const { return reason == ContactBufferCompactRejectReason::None; }

ContactBufferCompactPreflight preflightContactBufferCompact(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.4 deepen guard pass).
bool canSkipContactBufferCompact(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompact` (B4.4 deepen guard pass).
bool shouldRunContactBufferCompact(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.5 deepen pass).

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.5 deepen pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    WithinCapacity,

const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

bool contact_buffer_clamp_rejects_for_reason(
    ContactBufferClampRejectReason expected);

    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }


bool canSkipContactBufferClamp(const ContactBufferSoA& buffer);


/// Human-readable label for contact-buffer clamp reject reasons (logging / tests).
const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed.
ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.4 deepen pass).
bool contactBufferClampRejectsForReason(

/// Read-only max-capacity clamp diagnostics — no mutation (B4.4 deepen pass).


/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.4 deepen guard pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.4 deepen guard pass).



ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.4 deepen pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.4 deepen pass).
/// Non-mutating clamp predicate — inverse of `canSkipClamp` (B4.4 deepen guard pass).
bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer);



/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.4 deepen pass).





/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.4 deepen guard pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.4 deepen guard pass).

/// Human-readable label for contact-buffer clamp reject reasons (B4.5 deepen pass).

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.5 deepen pass).

/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.5 deepen pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.5 deepen pass).



/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.5 deepen pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.5 deepen pass).
/// Const preflight for contact-buffer compaction dispatch (B4.4 deepen follow-up pass).
    bool skipped = false;
    bool emptySlots = false;
    bool needsCompaction = false;

    bool can_skip_compaction() const { return skipped || !needsCompaction; }
    bool should_run_compaction() const { return !can_skip_compaction(); }

/// Populate compaction preflight without mutating the buffer (B4.4 deepen follow-up pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer compaction should be skipped (B4.4 deepen follow-up pass).
bool should_skip_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Const preflight for contact-buffer max-capacity clamp dispatch (B4.4 deepen follow-up pass).
    bool needsClamp = false;

    bool can_skip_clamp() const { return skipped || !needsClamp; }
    bool should_run_clamp() const { return !can_skip_clamp(); }

/// Populate clamp preflight without mutating the buffer (B4.4 deepen follow-up pass).

/// Returns true when contact-buffer clamp should be skipped (B4.4 deepen follow-up pass).
bool should_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Const preflight for contact-buffer friction-tangent rebuild dispatch (B4.4 deepen follow-up pass).
    u32 slotCount = 0u;
    u32 needsRebuildCount = 0u;

    bool can_skip_rebuild() const { return skipped || needsRebuildCount == 0u; }
    bool should_run_rebuild() const { return !can_skip_rebuild(); }

/// Populate friction-tangent preflight without mutating the buffer (B4.4 deepen follow-up pass).
ContactBufferFrictionPreflight preflight_contact_buffer_friction_tangents(const ContactBufferSoA& buffer);

/// Returns true when friction-tangent SoA rebuild should be skipped (B4.4 deepen follow-up pass).
bool should_skip_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer);



/// Why contact-buffer compaction would early-out (B4.4 guard pass).

/// Human-readable label for contact-buffer compaction reject reasons (B4.4 guard pass).

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.4 guard pass).

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.4 guard pass).

/// Read-only compaction diagnostics — no mutation (B4.4 guard pass).



/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.4 guard pass).

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.4 guard pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.4 guard pass).

/// Human-readable label for contact-buffer clamp reject reasons (B4.4 guard pass).

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.4 guard pass).

/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.4 guard pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.4 guard pass).



/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.4 guard pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.4 guard pass).
/// Const preflight for contact-buffer slot write (B4.4 deepen pass follow-up).

    bool can_write() const { return !skipped && !outOfRangeSlot && !invalidManifold && !selfPair; }

/// Populate write preflight without mutating buffer slots (B4.4 deepen pass follow-up).

/// Returns true when `writeSlot` / `writeSlotIfValid` would reject (B4.4 deepen pass follow-up).
bool should_skip_contact_buffer_write(

} // namespace fuse::physics::narrowphase
