#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::narrowphase {

struct ContactBufferSoA;

/// Why contact-buffer compaction would early-out (B4.5 deepen pass).
enum class ContactBufferCompactRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for contact-buffer compact reject reasons (B4.5 deepen pass).
const char* contact_buffer_compact_reject_reason_name(ContactBufferCompactRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.5 deepen pass).
ContactBufferCompactRejectReason contact_buffer_compact_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.5 deepen pass).
/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
    NoWork,

/// Human-readable label for contact-buffer compact reject reasons (B4.6 deepen pass).

/// Diagnose why contact-buffer compact would skip; vacuously succeeds when compact may proceed (B4.6 deepen pass).

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.6 deepen pass).
/// Why contact-buffer slot write would reject (B4.6 deepen follow-up pass).
enum class ContactBufferWriteRejectReason : u8 {
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,

/// Human-readable label for contact-buffer write reject reasons (B4.6 deepen follow-up pass).
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);

/// Diagnose why slot write would skip; vacuously succeeds when write may proceed (B4.6 deepen follow-up pass).
ContactBufferWriteRejectReason contact_buffer_write_reject_reason(
    u32 slot,
    const ContactManifold& manifold,
    u32 pairSlotCount);

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_write_rejects_for_reason(
    u32 pairSlotCount,
    ContactBufferWriteRejectReason expected);

/// Const preflight for contact-buffer slot write (B4.6 deepen follow-up pass).
struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool canWrite = false;

    bool can_write() const { return canWrite && reason == ContactBufferWriteRejectReason::None; }

/// Populate write preflight without mutating buffer slots (B4.6 deepen follow-up pass).
ContactBufferWritePreflight preflight_contact_buffer_write(

/// Why contact-buffer compaction would early-out (B4.6 deepen follow-up pass).
    AllInvalid,

/// Human-readable label for contact-buffer compact reject reasons (B4.6 deepen follow-up pass).

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen follow-up pass).

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_compact_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.5 deepen pass).
struct ContactBufferCompactPreflight {
    ContactBufferCompactRejectReason reason = ContactBufferCompactRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;
/// Read-only compact diagnostics — no mutation (B4.6 deepen pass).
    bool noWork = false;

    bool needsCompaction() const { return reason == ContactBufferCompactRejectReason::None; }
};

ContactBufferCompactPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.5 deepen pass).
bool can_skip_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compact` (B4.5 deepen pass).
bool should_run_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,

/// Human-readable label for contact-buffer clamp reject reasons (B4.5 deepen pass).
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.5 deepen pass).
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.5 deepen pass).
bool contact_buffer_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected);

/// Read-only clamp diagnostics — no mutation (B4.5 deepen pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool withinCapacity = false;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }

ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.5 deepen pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.5 deepen pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen pass).
/// Populate compact preflight without mutating the buffer (B4.6 deepen pass).

/// Non-mutating compact skip predicate — inverse of `needsCompaction` (B4.6 deepen pass).

/// Non-mutating compact predicate — mirrors `preflight_contact_buffer_compact` (B4.6 deepen pass).

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    NoWork,

/// Human-readable label for compact-and-clamp reject reasons (B4.5 deepen pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.5 deepen pass).
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.5 deepen pass).
/// Human-readable label for contact-buffer compact-and-clamp reject reasons (B4.6 deepen pass).

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen pass).

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
    ContactBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.5 deepen pass).
/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;

    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.5 deepen pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.5 deepen pass).
    bool needsCompactAndClamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }

/// Populate compact-and-clamp preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(

/// Non-mutating compact-and-clamp skip predicate (B4.6 deepen pass).

/// Non-mutating compact-and-clamp predicate (B4.6 deepen pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Const preflight for contact-buffer compaction (B4.6 deepen follow-up pass).
    bool canCompact = false;

    bool can_compact() const { return canCompact && reason == ContactBufferCompactRejectReason::None; }

/// Populate compact preflight without mutating buffer slots (B4.6 deepen follow-up pass).

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
    /// True when clamping dropped one or more contacts.
    /// Inverse of `canApplyMaxCapacityClamp` (B4.3 deepen follow-up).
    /// True when both dense and slot storage are empty (safe to skip SoA scans) (B4.6 deepen pass).
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.6 deepen pass).
    /// True when compact+clamp would leave the buffer unchanged (B4.6 deepen pass).
    /// Count valid flags in prepared slot storage before compaction (B4.6 deepen pass).
    /// Remaining slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`) (B4.6 deepen pass).
    /// True when post-pass truncation would drop contacts (B4.6 deepen pass).
    /// True when both dense and slot storage are empty (safe to skip SoA scans, B4.6 deepen pass).
    /// True when slot storage has no invalid flags (compact is a no-op, B4.6 deepen pass).
    /// Remaining write slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`, B4.6 deepen pass).
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    /// Inverse of `canApplyMaxCapacityClamp` (B4.6 deepen pass).
    /// True when compact+clamp would leave the buffer unchanged.
    /// True when at most one valid contact is present (friction-basis rebuild is a no-op).
    bool canSkipFrictionBasisBuild() const;
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`, B4.6 deepen pass).
    /// True when clamping dropped one or more contact manifolds.
    /// True when `maxCapacity` is set and no additional contacts may be retained after clamp.
    /// Remaining slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).

    bool canSkipFrictionTangentBuild(f32 epsilon = 1e-4f) const;
    /// True when `maxCapacity` is set and no additional contacts may be stored.
    /// Contact-list guard: true when `additionalCount` writes fit before `maxCapacity` clamp.
    bool canAcceptWrites(u32 additionalCount = 1u) const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.6 deepen follow-up pass).
    /// True when `maxCapacity` is set and no additional contacts may be stored after compaction.
    /// True when `maxCapacity` is set and no additional contacts may be pushed.
    /// Inverse of `canApplyMaxCapacityClamp` (B4.5 deepen pass).
    /// Inverse of `canApplyMaxCapacityClamp` (B4.3 deepen pass).
    /// True when `maxCapacity` is set and no additional contacts may be written after compact.
    /// Remaining active slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    /// True when `maxCapacity` is set and no additional contacts may be stored after clamp.
    /// Inverse of `canApplyMaxCapacityClamp` (B4.5 deepen follow-up pass).

    /// True when pair slots are empty or already contiguously valid (B4.5 deepen pass).

    /// True when max-capacity clamp would be a no-op after compaction (B4.5 deepen pass).

    /// Count valid pair slots without mutating storage (B4.5 deepen pass).

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const ContactManifold& manifold);
    void invalidateSlot(u32 slot);
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    /// Write only when `preflightContactBufferWrite` passes; no-op otherwise (B4.6 deepen pass).
    /// Write only when preflight allows; returns false when write is skipped (B4.6 deepen pass).
    bool writeSlotIfPreflight(u32 slot, const ContactManifold& manifold);
    void applyWarmStartStub(u32 slot, ContactManifold& manifold) const;
    void buildFrictionTangentBases();
    void buildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    /// Rebuild friction tangents only when orthonormal frames are missing (B4.4 deepen follow-up pass).
    void buildFrictionTangentBasesIfNeeded();
    /// Write only when preflight allows; no-op otherwise (B4.6 deepen pass).
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    /// Rebuild tangent columns only when preflight allows (B4.6 deepen pass).
    bool buildFrictionTangentBasesWithPreflight();
    /// Guarded write using preflight; returns false when write is rejected (B4.6 deepen pass).
    /// Rebuild tangent SoA columns only when preflight reports stale or missing frames (B4.6 deepen pass).
    bool slotFrictionBasisIsStale(u32 slot, f32 epsilon = 1e-4f) const;
    void rebuildFrictionTangentBasisAt(u32 slot, f32 epsilon = 1e-4f);
    void rebuildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    bool writeSlotWithFinalize(u32 slot, ContactManifold& manifold, f32 frictionEpsilon = 1e-4f);
    bool slotNeedsFrictionBasisRebuild(u32 slot, f32 epsilon = 1e-4f) const;
    bool frictionBasisMatchesNormalAt(u32 slot, f32 epsilon = 1e-4f) const;
    bool canSkipBuildFrictionTangentBases(f32 epsilon = 1e-4f) const;
    u32 compact();
    /// Compact only when preflight allows (B4.6 deepen pass).
    u32 compactWithPreflight();
    u32 applyMaxCapacityClamp();
    u32 compactAndClamp();
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.4 deepen follow-up pass).
    bool canSkipCompaction() const;
    /// True when post-pass truncation would drop contacts (B4.4 deepen follow-up pass).
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.4 deepen follow-up pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// Rebuild friction tangents only for slots with missing or stale bases (B4.4 deepen follow-up pass).
    /// Compact and clamp only when preflight allows (B4.6 deepen pass).
    u32 compactAndClampWithPreflight();
    bool canSkipMaxCapacityClamp() const;
    bool canSkipCompactAndClamp() const;
    u32 countValidPairSlots() const;
    u32 compactIfNeeded();
    u32 compactAndClampIfNeeded();
    bool canSkipFrictionTangentRebuild(f32 epsilon = 1e-4f) const;
    bool rebuildFrictionTangentBasesWithPreflight(f32 epsilon = 1e-4f);
    /// Write slot only when preflight allows (B4.6 deepen follow-up pass).
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    /// Compact only when preflight allows (B4.6 deepen follow-up pass).
    u32 compactWithPreflight();
    /// Compact and clamp only when preflight allows compaction (B4.6 deepen follow-up pass).
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
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    /// True when the buffer has no pair slots or active contacts (B4.6 deepen pass).
    bool canSkipSoAIteration() const { return pairSlotCount == 0u; }

    /// True when compaction would be a no-op (B4.6 deepen pass).

    /// True when max-capacity clamp would be a no-op (B4.6 deepen pass).
    /// Returns true when every active slot has a valid orthonormal friction basis (B4.6 deepen pass).
    bool canSkipFrictionRebuild(f32 epsilon = 1e-4f) const;

    /// Rebuild tangent SoA columns only for slots missing or stale bases (B4.6 deepen pass).
    void rebuildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    /// True when the buffer has no pair slots allocated (B4.6 deepen pass).

    /// True when every prepared slot is valid (B4.6 deepen pass).

    /// Count valid flags in prepared slot storage before compaction (B4.6 deepen pass).

    /// True when max-capacity clamp may reduce active contacts (B4.6 deepen pass).
    bool canSkipBuildFrictionTangentBases() const;
    bool shouldRunBuildFrictionTangentBases() const;
    /// True when slot storage has no invalid flags (compact is a no-op, B4.6 deepen follow-up pass).
    /// True when post-pass truncation would not drop contacts (B4.6 deepen follow-up pass).
    /// True when compact+clamp would leave the buffer unchanged (B4.6 deepen follow-up pass).
    bool canSkipCompactAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction (B4.6 deepen follow-up pass).
    /// Returns true when the pair slot holds a valid contact (B4.6 deepen pass).
    bool hasValidPairSlot(u32 slot) const;

    /// Returns true when compact would be a no-op (B4.6 deepen pass).
    bool canSkipBufferCompact() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer write would reject (B4.6 deepen pass).
/// Why contact-buffer write would reject (B4.3 deepen follow-up pass).
enum class ContactBufferWriteRejectReason : u8 {
    None = 0,
    InvalidSlot,
/// Why contact-buffer slot write would early-out (B4.6 deepen pass).
/// Why contact-buffer write-slot would reject (B4.6 deepen pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
/// Why contact-buffer write would reject (B4.6 deepen follow-up pass).
/// Why contact-buffer write would reject (B4.5 deepen follow-up pass).
    OutOfRangeSlot,
/// Why contact-buffer write would reject (B4.5 deepen pass).
/// Why contact-buffer write would reject (B4.3 deepen pass).
/// Why contact-buffer slot write would reject (B4.6 deepen pass).
/// Why contact-buffer write would reject (B4.6 narrowphase deepen pass).
/// Why contact-buffer slot write would reject (B4.5 deepen pass).
/// Why contact-buffer slot write would reject (B4.5 deepen follow-up pass).
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write reject reasons (B4.6 deepen pass).
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.6 deepen pass).
ContactBufferWriteRejectReason contact_buffer_write_reject_reason(

const char* contactBufferWriteRejectReasonName(ContactBufferWriteRejectReason reason);

/// Human-readable label for contact-buffer write reject reasons (B4.3 deepen follow-up pass).

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.3 deepen follow-up pass).
ContactBufferWriteRejectReason contactBufferWriteRejectReason(
/// Diagnose why write would skip; vacuously succeeds when write may proceed (B4.6 deepen pass).



/// Diagnose why slot write would skip; vacuously succeeds when write may proceed (B4.6 deepen pass).
/// Human-readable label for contact-buffer write-slot reject reasons (B4.6 deepen pass).
const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason);

/// Diagnose why write-slot would reject; vacuously succeeds when write may proceed.
ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
/// Human-readable label for contact-buffer write reject reasons (B4.6 deepen follow-up pass).

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.6 deepen follow-up pass).
/// Human-readable label for contact-buffer write reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.5 deepen follow-up pass).

/// Human-readable label for contact-buffer write reject reasons (logging / tests).

/// Diagnose why write would reject; vacuously succeeds when write may proceed.




/// Human-readable label for contact-buffer write reject reasons (B4.5 deepen pass).


/// Diagnose why slot write would reject; vacuously succeeds when write may proceed (B4.6 deepen pass).


/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.5 deepen pass).






    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_write_rejects_for_reason(
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferWriteRejectsForReason(
/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.3 deepen follow-up pass).
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.5 deepen pass).
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.3 deepen pass).
    const ContactManifold& manifold,
    ContactBufferWriteRejectReason expected);

/// Read-only write diagnostics — no mutation (B4.6 deepen pass).

/// Read-only write diagnostics — no mutation (B4.3 deepen follow-up pass).
struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool invalidSlot = false;
/// Const preflight for contact-buffer slot write (B4.6 deepen pass).
/// Read-only slot-write diagnostics — no mutation (B4.6 deepen pass).
/// Returns true when `contact_buffer_write_slot_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_write_slot_rejects_for_reason(
    ContactBufferWriteSlotRejectReason expected);

/// Read-only write-slot diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
/// Read-only write diagnostics — no mutation (B4.6 deepen follow-up pass).
/// Const preflight for contact-buffer write dispatch (B4.5 deepen follow-up pass).
/// Const preflight for contact-buffer slot write (B4.6 deepen follow-up pass).
    bool outOfRangeSlot = false;
/// Read-only write diagnostics — no mutation (B4.5 deepen pass).
/// Read-only write diagnostics — no mutation (B4.3 deepen pass).
    const ContactBufferSoA& buffer,
    u32 slot,


/// Read-only write diagnostics — no mutation (B4.5 deepen follow-up pass).
    bool invalidManifold = false;
    bool selfPair = false;

    bool canWrite() const { return reason == ContactBufferWriteRejectReason::None; }

/// Populate write preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferWritePreflight preflight_contact_buffer_write(


ContactBufferWritePreflight preflightContactBufferWrite(

/// Non-mutating write skip predicate — inverse of `canWrite` (B4.6 deepen pass).
bool canSkipContactBufferWrite(

/// Non-mutating write predicate — mirrors `preflightContactBufferWrite` (B4.6 deepen pass).
bool shouldRunContactBufferWrite(

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    EmptyBuffer,
    NoWork,
    AllValid,




    bool can_write() const { return reason == ContactBufferWriteRejectReason::None; }




/// Returns true when contact-buffer write should be skipped (B4.6 deepen pass).
bool should_skip_contact_buffer_write(



/// Write only when preflight passes; no-op otherwise (B4.6 deepen pass).
void write_contact_buffer_slot_with_preflight(
    ContactBufferSoA& buffer,

    bool canWrite() const { return reason == ContactBufferWriteSlotRejectReason::None; }

ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(

/// Non-mutating write-slot skip predicate — inverse of `canWrite` (B4.6 deepen pass).
bool should_skip_contact_buffer_write_slot(






/// Non-mutating slot-write skip predicate — inverse of `canWrite` (B4.6 deepen pass).

    bool outOfRange = false;


















/// Human-readable label for contact-buffer compaction reject reasons (B4.6 deepen pass).
const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen pass).
ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.6 deepen pass).
/// Why contact-buffer write would reject (B4.4 deepen guard pass).


/// Why contact-buffer write would reject (B4.4 deepen pass).
/// Why contact-buffer slot write would reject (B4.4 deepen pass).
    OutOfRange,






/// Why contact-buffer write would reject (B4.4 guard pass).

/// Human-readable label for contact-buffer write reject reasons (B4.4 guard pass).

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.4 guard pass).


/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.4 deepen pass).

/// Read-only write diagnostics — no mutation (B4.4 deepen pass).

/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.4 deepen guard pass).

/// Read-only write diagnostics — no mutation (B4.4 deepen guard pass).

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.4 deepen pass).



/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.4 guard pass).

/// Read-only write diagnostics — no mutation (B4.4 guard pass).



/// Why contact-buffer compaction would early-out (B4.4 deepen guard pass).



bool contact_buffer_compaction_rejects_for_reason(
    AllInvalid,

const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason);





/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed (B4.6 deepen pass).











bool shouldSkipContactBufferWrite(


/// Human-readable label for contact-buffer compaction reject reasons (logging / tests).





/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed.









ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferCompactionRejectsForReason(






/// Why contact-buffer compaction would early-out (B4.6 narrowphase deepen pass).







    ContactBufferCompactionRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }
    bool allValid = false;


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












/// Write only when preflight allows; returns false when skipped (B4.3 deepen pass).
bool writeContactBufferSlotWithPreflight(

/// Why contact-buffer compaction would early-out (B4.3 deepen pass).






ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer);

bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer);

bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.4 deepen guard pass).





/// Non-mutating write skip predicate — inverse of `canWrite` (B4.4 deepen pass).
bool can_skip_contact_buffer_write(

/// Non-mutating write predicate — mirrors `preflight_contact_buffer_write` (B4.4 deepen pass).
bool should_run_contact_buffer_write(

/// Why contact-buffer compaction would early-out (B4.4 deepen pass).




/// Non-mutating write skip predicate — inverse of `canWrite` (B4.4 deepen guard pass).

/// Non-mutating write predicate — mirrors `preflightContactBufferWrite` (B4.4 deepen guard pass).

enum class ContactBufferCompactRejectReason : u8 {


/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed.

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.4 deepen pass).

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
/// Why contact-buffer max-capacity clamp would early-out (B4.6 narrowphase deepen pass).


};

    const ContactManifold& manifold);

    None = 0,





    bool needs_compaction() const { return reason == ContactBufferCompactionRejectReason::None; }




const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

bool contact_buffer_clamp_rejects_for_reason(
    ContactBufferClampRejectReason expected);

    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }


bool canSkipContactBufferClamp(const ContactBufferSoA& buffer);


/// Human-readable label for contact-buffer clamp reject reasons (logging / tests).


/// Why contact-buffer compaction would early-out (B4.3 deepen follow-up pass).

/// Human-readable label for contact-buffer compaction reject reasons (B4.3 deepen follow-up pass).


/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.3 deepen follow-up pass).

/// Read-only compaction diagnostics — no mutation (B4.3 deepen follow-up pass).



/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.3 deepen follow-up pass).

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.3 deepen follow-up pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.3 deepen follow-up pass).

/// Human-readable label for contact-buffer clamp reject reasons (B4.3 deepen follow-up pass).


/// Non-mutating write predicate — mirrors `preflightContactBufferWrite` (B4.3 deepen follow-up pass).
bool canWriteContactBufferSlot(

/// Write only when preflight passes; no-op otherwise (B4.3 deepen follow-up pass).















/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.6 deepen pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen pass).









/// Write only when preflight allows; returns false when skipped (B4.5 deepen pass).
bool writeContactSlotWithPreflight(














/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed (B4.5 deepen pass).








/// Human-readable label for contact-buffer clamp reject reasons (B4.5 deepen pass).
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
/// Non-mutating compaction skip predicate (B4.6 deepen pass).


/// Human-readable label for contact-buffer clamp reject reasons (B4.6 deepen pass).

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.6 deepen pass).

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.6 deepen pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.6 deepen pass).


/// Populate clamp preflight without mutating the buffer (B4.6 deepen pass).

/// Non-mutating clamp skip predicate (B4.6 deepen pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);








/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.6 deepen pass).




/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.6 deepen pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.6 deepen pass).

/// Human-readable label for contact-buffer compact reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_reject_reason_name(ContactBufferCompactRejectReason reason);

ContactBufferCompactRejectReason contact_buffer_compact_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_rejects_for_reason(



ContactBufferCompactPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compact` (B4.6 deepen pass).
bool should_run_contact_buffer_compact(const ContactBufferSoA& buffer);



















































/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.6 deepen pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
/// Why contact-buffer friction-basis build would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionBasesRejectReason : u8 {
    AllBuilt,



const char* contact_buffer_friction_bases_reject_reason_name(ContactBufferFrictionBasesRejectReason reason);

/// Diagnose why friction-basis build would skip; vacuously succeeds when build may proceed.
ContactBufferFrictionBasesRejectReason contact_buffer_friction_bases_reject_reason(

/// Returns true when `contact_buffer_friction_bases_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_bases_rejects_for_reason(



    ContactBufferFrictionBasesRejectReason expected);

/// Read-only friction-basis build diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionBasesPreflight {
    ContactBufferFrictionBasesRejectReason reason = ContactBufferFrictionBasesRejectReason::None;
    bool allBuilt = false;

    bool needsBuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }




ContactBufferFrictionBasesPreflight preflight_contact_buffer_friction_bases(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis build skip predicate (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_bases(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis build predicate (B4.6 deepen pass).
bool should_run_contact_buffer_friction_bases(const ContactBufferSoA& buffer);







/// Why contact-buffer compact-and-clamp would early-out (B4.6 narrowphase deepen pass).

/// Human-readable label for contact-buffer compact-and-clamp reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen pass).
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed.


/// Human-readable label for compact-and-clamp reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(
    ContactBufferCompactAndClampRejectReason reason);

















const char* contactBufferCompactAndClampRejectReasonName(ContactBufferCompactAndClampRejectReason reason);

ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(

/// Returns true when `contactBufferCompactAndClampRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferCompactAndClampRejectsForReason(

/// Human-readable label for compact-and-clamp reject reasons (logging / tests).



    ContactBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;

    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }


ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

    bool needsCompactAndClamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(








/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.6 deepen pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.6 deepen pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

enum class ContactBufferFrictionBuildRejectReason : u8 {
    NoValidSlots,

/// Human-readable label for contact-buffer friction-build reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_build_reject_reason_name(ContactBufferFrictionBuildRejectReason reason);

/// Diagnose why friction-basis build would skip; vacuously succeeds when build may proceed (B4.6 deepen pass).
ContactBufferFrictionBuildRejectReason contact_buffer_friction_build_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_friction_build_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_build_rejects_for_reason(
    ContactBufferFrictionBuildRejectReason expected);

struct ContactBufferFrictionBuildPreflight {
    ContactBufferFrictionBuildRejectReason reason = ContactBufferFrictionBuildRejectReason::None;
    bool noValidSlots = false;

    bool needsFrictionBuild() const { return reason == ContactBufferFrictionBuildRejectReason::None; }

ContactBufferFrictionBuildPreflight preflight_contact_buffer_friction_build(const ContactBufferSoA& buffer);

/// Non-mutating friction-build skip predicate — inverse of `needsFrictionBuild` (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_build(const ContactBufferSoA& buffer);

/// Non-mutating friction-build predicate — mirrors `preflight_contact_buffer_friction_build` (B4.6 deepen pass).
bool should_run_contact_buffer_friction_build(const ContactBufferSoA& buffer);
/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.3 deepen follow-up pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.3 deepen follow-up pass).























/// Const preflight for contact-buffer compaction (B4.6 deepen pass).



/// Non-mutating compaction skip predicate — inverse of `needs_compaction` (B4.6 deepen pass).








/// Returns true when contact-buffer compaction should be skipped (B4.6 deepen pass).

/// Returns true when contact-buffer compaction should run (B4.6 deepen pass).





/// Const preflight for contact-buffer max-capacity clamp (B4.6 deepen pass).














/// Populate write preflight without mutating the buffer (B4.6 deepen follow-up pass).

/// Non-mutating write skip predicate — inverse of `canWrite` (B4.6 deepen follow-up pass).

/// Write only when preflight allows; returns false when skipped (B4.6 deepen follow-up pass).
bool write_contact_buffer_slot_with_preflight(

/// Why contact-buffer compaction would early-out (B4.6 deepen follow-up pass).

/// Human-readable label for contact-buffer compaction reject reasons (B4.6 deepen follow-up pass).

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed (B4.6 deepen follow-up pass).

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.6 deepen follow-up pass).

/// Read-only compaction diagnostics — no mutation (B4.6 deepen follow-up pass).


/// Populate compaction preflight without mutating the buffer (B4.6 deepen follow-up pass).

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.6 deepen follow-up pass).

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.6 deepen follow-up pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen follow-up pass).

/// Human-readable label for contact-buffer clamp reject reasons (B4.6 deepen follow-up pass).

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.6 deepen follow-up pass).

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.6 deepen follow-up pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.6 deepen follow-up pass).

/// Populate write preflight without mutating the buffer (B4.5 deepen follow-up pass).

/// Returns true when contact-buffer write should be skipped (B4.5 deepen follow-up pass).

/// Write only when preflight passes; no-op otherwise (B4.5 deepen follow-up pass).

/// Why contact-buffer compaction would early-out (B4.5 deepen follow-up pass).

/// Human-readable label for contact-buffer compaction reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.5 deepen follow-up pass).

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.5 deepen follow-up pass).

/// Const preflight for contact-buffer compaction dispatch (B4.5 deepen follow-up pass).


/// Populate compaction preflight without mutating the buffer (B4.5 deepen follow-up pass).

/// Non-mutating compaction skip predicate — inverse of `needs_compaction` (B4.5 deepen follow-up pass).

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.5 deepen follow-up pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen follow-up pass).

/// Human-readable label for contact-buffer clamp reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.5 deepen follow-up pass).

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.5 deepen follow-up pass).

/// Const preflight for contact-buffer clamp dispatch (B4.5 deepen follow-up pass).


/// Returns true when contact-buffer write should be skipped (B4.6 deepen follow-up pass).



/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen follow-up pass).


/// Const preflight for contact-buffer compaction (B4.6 deepen follow-up pass).



/// Non-mutating compaction skip predicate (B4.6 deepen follow-up pass).

/// Non-mutating compaction predicate (B4.6 deepen follow-up pass).





/// Const preflight for contact-buffer max-capacity clamp (B4.6 deepen follow-up pass).
















/// Why contact-buffer max-capacity clamp would early-out (B4.3 deepen pass).










/// Const preflight for contact-buffer slot write (B4.5 deepen follow-up pass).








/// Const preflight for contact-buffer compaction (B4.5 deepen follow-up pass).



/// Returns true when contact-buffer compaction should be skipped (B4.5 deepen follow-up pass).

/// Returns true when contact-buffer compaction should run (B4.5 deepen follow-up pass).





/// Const preflight for contact-buffer max-capacity clamp (B4.5 deepen follow-up pass).




/// Returns true when compaction should be skipped (B4.6 deepen pass).

/// Returns true when compaction should run (B4.6 deepen pass).











/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.3 deepen follow-up pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.3 deepen follow-up pass).

/// Why contact-buffer compact-and-clamp would early-out (B4.3 deepen follow-up pass).

/// Human-readable label for compact-and-clamp reject reasons (B4.3 deepen follow-up pass).










/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen pass).



/// Human-readable label for compact-and-clamp reject reasons (B4.5 deepen pass).


/// Returns true when `contactBufferCompactAndClampRejectReason` matches `expected` (B4.3 deepen follow-up pass).

/// Read-only compact-and-clamp diagnostics — no mutation (B4.3 deepen follow-up pass).








/// Why contact-buffer compact-and-clamp would early-out (B4.3 deepen pass).























ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.3 deepen follow-up pass).
bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflightContactBufferCompactAndClamp` (B4.3 deepen follow-up pass).
bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer);


    bool needs_clamp() const { return reason == ContactBufferClampRejectReason::None; }


/// Returns true when contact-buffer clamp should be skipped (B4.6 deepen pass).

/// Returns true when contact-buffer clamp should run (B4.6 deepen pass).



























/// Populate compact-and-clamp preflight without mutating the buffer (B4.6 deepen pass).


/// Non-mutating compact-and-clamp predicate — mirrors preflight (B4.6 deepen pass).

/// Write only when preflight allows; no-op otherwise (B4.6 deepen pass).



/// Non-mutating clamp skip predicate — inverse of `needs_clamp` (B4.6 deepen pass).





/// Const preflight for contact-buffer compact-and-clamp (B4.6 deepen pass).



/// Populate clamp preflight without mutating the buffer (B4.5 deepen follow-up pass).

/// Non-mutating clamp skip predicate — inverse of `needs_clamp` (B4.5 deepen follow-up pass).

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.5 deepen follow-up pass).

/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen follow-up pass).

/// Human-readable label for compact-and-clamp reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.5 deepen follow-up pass).

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.5 deepen follow-up pass).

/// Const preflight for contact-buffer compact-and-clamp dispatch (B4.5 deepen follow-up pass).

/// Populate clamp preflight without mutating the buffer (B4.6 deepen follow-up pass).

/// Non-mutating clamp skip predicate (B4.6 deepen follow-up pass).

/// Non-mutating clamp predicate (B4.6 deepen follow-up pass).

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen follow-up pass).

/// Human-readable label for contact-buffer compact-and-clamp reject reasons (B4.6 deepen follow-up pass).

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen follow-up pass).

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.6 deepen follow-up pass).

/// Const preflight for contact-buffer compact-and-clamp (B4.6 deepen follow-up pass).

    bool needs_compact_and_clamp() const {

ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(








/// Non-mutating compact-and-clamp predicate — mirrors `preflightContactBufferCompactAndClamp` (B4.6 deepen pass).

void writeContactBufferSlotWithPreflight(


/// Returns true when clamp should be skipped (B4.6 deepen pass).

/// Returns true when clamp should run (B4.6 deepen pass).








/// Returns true when compact-and-clamp should be skipped (B4.6 deepen pass).

/// Returns true when compact-and-clamp should run (B4.6 deepen pass).

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.5 deepen pass).

/// Returns true when `contactBufferCompactAndClampRejectReason` matches `expected` (B4.5 deepen pass).

/// Read-only compact-and-clamp diagnostics — no mutation (B4.5 deepen pass).



/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.5 deepen pass).

/// Non-mutating compact-and-clamp predicate — mirrors `preflightContactBufferCompactAndClamp` (B4.5 deepen pass).


/// Compact only when preflight allows work; returns active count (B4.6 deepen pass).
u32 compact_contact_buffer_with_preflight(ContactBufferSoA& buffer);

/// Compact-and-clamp only when preflight allows work; returns active count (B4.6 deepen pass).
u32 compact_and_clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer);
u32 compactContactBufferWithPreflight(ContactBufferSoA& buffer);

/// Clamp only when preflight allows work; returns active count (B4.6 deepen pass).
u32 clampContactBufferWithPreflight(ContactBufferSoA& buffer);

/// Compact and clamp only when preflight allows work; returns active count (B4.6 deepen pass).
u32 compactAndClampContactBufferWithPreflight(ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate (B4.6 deepen pass).

    NoValidContacts,

/// Human-readable label for friction-build reject reasons (B4.6 deepen pass).
const char* contactBufferFrictionBuildRejectReasonName(ContactBufferFrictionBuildRejectReason reason);

ContactBufferFrictionBuildRejectReason contactBufferFrictionBuildRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferFrictionBuildRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferFrictionBuildRejectsForReason(

/// Read-only friction-build diagnostics — no mutation (B4.6 deepen pass).
    bool noValidContacts = false;

    bool canBuild() const { return reason == ContactBufferFrictionBuildRejectReason::None; }

/// Populate friction-build preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionBuildPreflight preflightContactBufferFrictionBuild(const ContactBufferSoA& buffer);

/// Non-mutating friction-build skip predicate — inverse of `canBuild` (B4.6 deepen pass).
bool canSkipContactBufferFrictionBuild(const ContactBufferSoA& buffer);

/// Build friction tangent bases only when preflight allows (B4.6 deepen pass).
void buildContactBufferFrictionTangentBasesWithPreflight(ContactBufferSoA& buffer);
const char* contact_buffer_friction_basis_reject_reason_name(
    ContactBufferFrictionBasisRejectReason reason);

/// Diagnose why friction-basis rebuild would skip (B4.6 deepen pass).


/// Const preflight for contact-buffer friction-basis rebuild (B4.6 deepen pass).

    bool can_rebuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }

ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_basis(

/// Non-mutating friction-basis skip predicate (B4.6 deepen pass).

/// Returns true when contact-buffer compact-and-clamp should be skipped (B4.6 deepen pass).

/// Returns true when contact-buffer compact-and-clamp should run (B4.6 deepen pass).






/// Human-readable label for friction-bases reject reasons (B4.6 deepen pass).
const char* contactBufferFrictionBasesRejectReasonName(ContactBufferFrictionBasesRejectReason reason);

/// Diagnose why `buildFrictionTangentBases` would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
ContactBufferFrictionBasesRejectReason contactBufferFrictionBasesRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferFrictionBasesRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferFrictionBasesRejectsForReason(

/// Read-only friction-bases diagnostics — no mutation (B4.6 deepen pass).

    bool needsRebuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }



ContactBufferFrictionBasesPreflight preflightContactBufferFrictionBases(const ContactBufferSoA& buffer);

/// Non-mutating friction-bases skip predicate — inverse of `needsRebuild` (B4.6 deepen pass).
bool canSkipContactBufferFrictionBases(const ContactBufferSoA& buffer);

/// Non-mutating friction-bases predicate — mirrors `preflightContactBufferFrictionBases` (B4.6 deepen pass).
bool shouldRunContactBufferFrictionBases(const ContactBufferSoA& buffer);

/// Rebuild friction tangents only when preflight allows; returns false when skipped (B4.6 deepen pass).
bool buildContactBufferFrictionBasesWithPreflight(ContactBufferSoA& buffer);
/// Why contact-buffer friction-tangent rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionTangentRejectReason : u8 {

/// Human-readable label for friction-tangent reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_tangent_reject_reason_name(
    ContactBufferFrictionTangentRejectReason reason);

/// Diagnose why friction-tangent rebuild would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(

/// Returns true when `contact_buffer_friction_tangent_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_tangent_rejects_for_reason(
    ContactBufferFrictionTangentRejectReason expected,

/// Read-only friction-tangent diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionTangentPreflight {
    ContactBufferFrictionTangentRejectReason reason = ContactBufferFrictionTangentRejectReason::None;

    bool needsFrictionTangentBuild() const {
        return reason == ContactBufferFrictionTangentRejectReason::None;

/// Populate friction-tangent preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangents(

/// Non-mutating friction-tangent skip predicate (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_tangent_build(

/// Compact only when preflight allows; returns active count (B4.6 deepen pass).

/// Clamp only when preflight allows; returns active count (B4.6 deepen pass).
u32 apply_contact_buffer_max_capacity_clamp_with_preflight(ContactBufferSoA& buffer);

/// Compact-and-clamp only when preflight allows; returns active count (B4.6 deepen pass).

/// Build friction tangents only when preflight allows (B4.6 deepen pass).
void build_contact_buffer_friction_tangents_with_preflight(
/// Compact and clamp only when preflight allows; returns active count (B4.6 deepen pass).


/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.6 deepen follow-up pass).

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.6 deepen follow-up pass).


/// Human-readable label for compact-and-clamp reject reasons (B4.6 deepen follow-up pass).



/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen follow-up pass).


/// Populate compact-and-clamp preflight without mutating the buffer (B4.6 deepen follow-up pass).

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.6 deepen follow-up pass).

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.6 deepen follow-up pass).

/// Compact only when preflight allows; returns active count (B4.6 deepen follow-up pass).

/// Compact and clamp only when preflight allows; returns active count (B4.6 deepen follow-up pass).

/// Populate compact-and-clamp preflight without mutating the buffer (B4.5 deepen follow-up pass).

/// Non-mutating compact-and-clamp skip predicate — inverse of `needs_compact_and_clamp` (B4.5 deepen follow-up pass).

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.5 deepen follow-up pass).


/// Non-mutating compact-and-clamp skip predicate (B4.6 deepen follow-up pass).

/// Non-mutating compact-and-clamp predicate (B4.6 deepen follow-up pass).








enum class ContactBufferFrictionRebuildRejectReason : u8 {
    AllOrthonormal,

const char* contactBufferFrictionRebuildRejectReasonName(ContactBufferFrictionRebuildRejectReason reason);

ContactBufferFrictionRebuildRejectReason contactBufferFrictionRebuildRejectReason(

bool contactBufferFrictionRebuildRejectsForReason(
    ContactBufferFrictionRebuildRejectReason expected,

struct ContactBufferFrictionRebuildPreflight {
    ContactBufferFrictionRebuildRejectReason reason = ContactBufferFrictionRebuildRejectReason::None;
    bool allOrthonormal = false;

    bool needsRebuild() const { return reason == ContactBufferFrictionRebuildRejectReason::None; }

ContactBufferFrictionRebuildPreflight preflightContactBufferFrictionRebuild(

bool canSkipContactBufferFrictionRebuild(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

bool shouldRunContactBufferFrictionRebuild(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);














/// Returns true when contact-buffer clamp should be skipped (B4.5 deepen follow-up pass).

/// Returns true when contact-buffer clamp should run (B4.5 deepen follow-up pass).





/// Const preflight for contact-buffer compact-and-clamp (B4.5 deepen follow-up pass).

    bool needs_compact_and_clamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }


/// Returns true when contact-buffer compact-and-clamp should be skipped (B4.5 deepen follow-up pass).

/// Returns true when contact-buffer compact-and-clamp should run (B4.5 deepen follow-up pass).

u32 clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer);


/// Compact only when preflight allows; returns active count (B4.5 deepen pass).

/// Clamp only when preflight allows; returns active count (B4.5 deepen pass).

/// Compact and clamp only when preflight allows; returns active count (B4.5 deepen pass).








/// Const preflight for contact-buffer friction tangent rebuild (B4.6 deepen pass).
    u32 activeCount = 0u;
    u32 staleCount = 0u;
    u32 rebuildCount = 0u;

    bool can_skip_rebuild() const { return activeCount == 0u || rebuildCount == 0u; }

/// Populate buffer friction preflight without mutating slots (B4.6 deepen pass).
    u32 validSlotCount = 0u;

    bool can_skip_compact_and_clamp() const { return skipped || (!needsCompaction && !needsClamp); }


/// Returns true when compaction would be a no-op (B4.6 deepen pass).

    FrictionBasisRejectReason reason = FrictionBasisRejectReason::None;
    u32 slotsNeedingRebuild = 0u;
    u32 slotsWithStaleBasis = 0u;

    bool can_skip_rebuild() const {
        return skipped || reason != FrictionBasisRejectReason::None || slotsNeedingRebuild == 0u;

/// Populate friction rebuild preflight without mutating the buffer (B4.6 deepen pass).

/// Returns true when buffer friction rebuild should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_rebuild(
/// Const preflight for buffer friction-basis rebuild (B4.6 deepen pass).


/// Populate buffer friction preflight without mutating tangent columns (B4.6 deepen pass).
ContactBufferFrictionPreflight preflight_buffer_friction_rebuild(

bool should_skip_buffer_friction_rebuild(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

/// Human-readable label for friction-basis rebuild reject reasons (logging / tests).

/// Diagnose why friction-basis rebuild would skip; vacuously succeeds when rebuild may proceed.
ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(const ContactBufferSoA& buffer);





/// Non-mutating friction-basis rebuild skip predicate — inverse of `needsRebuild` (B4.6 deepen pass).

/// Non-mutating friction-basis rebuild predicate — mirrors `preflight_contact_buffer_friction_basis` (B4.6 deepen pass).










    bool rejected = false;

    bool can_write() const { return !rejected; }

/// Populate slot-write preflight without mutating the buffer (B4.6 deepen pass).

/// Why friction-basis SoA rebuild would early-out (B4.6 deepen pass).
    AllBasesValid,

/// Human-readable label for friction-basis SoA rebuild reject reasons (B4.6 deepen pass).

/// Diagnose why friction-basis SoA rebuild would skip (B4.6 deepen pass).
ContactBufferFrictionBuildRejectReason contact_buffer_friction_build_reject_reason(


/// Read-only friction-basis SoA rebuild diagnostics — no mutation (B4.6 deepen pass).
    bool allBasesValid = false;

    bool needs_build() const { return reason == ContactBufferFrictionBuildRejectReason::None; }

/// Populate friction-basis SoA rebuild preflight without mutating the buffer (B4.6 deepen pass).

/// Non-mutating skip predicate — inverse of `needs_build` (B4.6 deepen pass).

/// Non-mutating rebuild predicate — mirrors `preflight_contact_buffer_friction_build` (B4.6 deepen pass).

    bool can_skip_rebuild() const { return staleCount == 0u; }
    bool needs_rebuild() const { return staleCount > 0u; }

/// Populate friction tangent rebuild preflight without mutating slots (B4.6 deepen pass).

/// Returns true when all active slots already store orthonormal tangent frames (B4.6 deepen pass).


    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Write only when preflight allows; no-op otherwise (B4.6 deepen follow-up pass).
bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold, ContactBufferSoA& buffer);

    None = 0,








/// Compaction only when preflight allows; returns active count (B4.6 deepen follow-up pass).
u32 compactWithPreflight(ContactBufferSoA& buffer);









/// Clamp only when preflight allows; returns active count (B4.6 deepen follow-up pass).
u32 applyMaxCapacityClampWithPreflight(ContactBufferSoA& buffer);









u32 compactAndClampWithPreflight(ContactBufferSoA& buffer);

struct ContactBufferClampPreflight {
    bool emptyBuffer = false;
    bool withinCapacity = false;


ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);


/// Why contact-buffer friction-basis rebuild would early-out (B4.5 deepen pass).
    EmptyBuffer,

const char* contact_buffer_friction_rebuild_reject_reason_name(ContactBufferFrictionRebuildRejectReason reason);

ContactBufferFrictionRebuildRejectReason contact_buffer_friction_rebuild_reject_reason(
    const ContactBufferSoA& buffer);

bool contact_buffer_friction_rebuild_rejects_for_reason(
    ContactBufferFrictionRebuildRejectReason expected);


    bool can_rebuild() const { return reason == ContactBufferFrictionRebuildRejectReason::None; }

ContactBufferFrictionRebuildPreflight preflight_contact_buffer_friction_rebuild(

bool can_skip_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer);
bool should_run_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer);

    NoWork,




    bool noWork = false;



/// Why contact-buffer compaction would early-out (B4.5 deepen follow-up pass).
enum class ContactBufferCompactionRejectReason : u8 {
    AllValid,
};

/// Human-readable label for contact-buffer compaction reject reasons (logging / tests).
const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed.
ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.5 deepen follow-up pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool allValid = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }

ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.5 deepen follow-up pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.5 deepen follow-up pass).
bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen follow-up pass).
enum class ContactBufferClampRejectReason : u8 {
    WithinCapacity,

/// Human-readable label for contact-buffer clamp reject reasons (logging / tests).
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed.
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool contact_buffer_clamp_rejects_for_reason(
    ContactBufferClampRejectReason expected);

/// Read-only max-capacity clamp diagnostics — no mutation (B4.5 deepen follow-up pass).
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }


/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.5 deepen follow-up pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.5 deepen follow-up pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen follow-up pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {

/// Human-readable label for compact-and-clamp reject reasons (logging / tests).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed.
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
    ContactBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.5 deepen follow-up pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;

    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.5 deepen follow-up pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.5 deepen follow-up pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase
