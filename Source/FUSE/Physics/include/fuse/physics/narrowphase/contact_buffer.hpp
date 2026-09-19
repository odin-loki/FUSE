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
    /// True when clamping dropped one or more contacts.
    bool hasDroppedContacts() const { return droppedCount > 0u; }
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.6 deepen pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when compact+clamp would leave the buffer unchanged.
    bool canSkipCompactAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const ContactManifold& manifold);
    void applyWarmStartStub(u32 slot, ContactManifold& manifold) const;
    void buildFrictionTangentBases();
    u32 compact();
    u32 applyMaxCapacityClamp();
    u32 compactAndClamp();
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer slot write would reject (B4.6 deepen pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write-slot reject reasons (B4.6 deepen pass).
const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason);

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed (B4.6 deepen pass).
ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contact_buffer_write_slot_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_write_slot_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected);

/// Const preflight for contact-buffer slot write (B4.6 deepen pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool can_write() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

/// Populate write-slot preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when writeSlot should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when writeSlot may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for contact-buffer compaction reject reasons (B4.6 deepen pass).
const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen pass).
ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

/// Const preflight for contact-buffer compaction (B4.6 deepen pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needs_compaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

/// Populate compaction preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Returns true when compaction should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Returns true when compaction may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

/// Human-readable label for contact-buffer clamp reject reasons (B4.6 deepen pass).
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.6 deepen pass).
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected);

/// Const preflight for contact-buffer max-capacity clamp (B4.6 deepen pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needs_clamp() const { return reason == ContactBufferClampRejectReason::None; }
};

/// Populate clamp preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Returns true when clamp should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Returns true when clamp may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for contact-buffer compact-and-clamp reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen pass).
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected);

/// Const preflight for contact-buffer compact-and-clamp (B4.6 deepen pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needs_compact_and_clamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }
};

/// Populate compact-and-clamp preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
    const ContactBufferSoA& buffer);

/// Returns true when compact-and-clamp should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Returns true when compact-and-clamp may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer toVector would early-out (B4.6 deepen pass).
enum class ContactBufferToVectorRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

/// Human-readable label for contact-buffer toVector reject reasons (B4.6 deepen pass).
const char* contact_buffer_to_vector_reject_reason_name(ContactBufferToVectorRejectReason reason);

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed (B4.6 deepen pass).
ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_to_vector_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_to_vector_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected);

/// Const preflight for contact-buffer toVector (B4.6 deepen pass).
struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool can_export() const { return reason == ContactBufferToVectorRejectReason::None; }
};

/// Populate toVector preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Returns true when toVector should return empty (B4.6 deepen pass).
bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Returns true when toVector may export contacts (B4.6 deepen pass).
bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Why contact-buffer warm-start stub would early-out (B4.6 deepen pass).
enum class ContactBufferWarmStartRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidSlot,
};

/// Human-readable label for contact-buffer warm-start reject reasons (B4.6 deepen pass).
const char* contact_buffer_warm_start_reject_reason_name(ContactBufferWarmStartRejectReason reason);

/// Diagnose why applyWarmStartStub would skip; vacuously succeeds when copy may proceed (B4.6 deepen pass).
ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot);

/// Returns true when `contact_buffer_warm_start_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_warm_start_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactBufferWarmStartRejectReason expected);

/// Const preflight for contact-buffer warm-start stub (B4.6 deepen pass).
struct ContactBufferWarmStartPreflight {
    ContactBufferWarmStartRejectReason reason = ContactBufferWarmStartRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidSlot = false;

    bool can_apply() const { return reason == ContactBufferWarmStartRejectReason::None; }
};

/// Populate warm-start preflight without mutating the manifold (B4.6 deepen pass).
ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    const ContactBufferSoA& buffer,
    u32 slot);

/// Returns true when warm-start stub should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

/// Returns true when warm-start stub may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

/// Why contact-buffer friction-basis rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionBasesRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoValidSlots,
};

/// Human-readable label for contact-buffer friction-bases reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_bases_reject_reason_name(ContactBufferFrictionBasesRejectReason reason);

/// Diagnose why buildFrictionTangentBases would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
ContactBufferFrictionBasesRejectReason contact_buffer_friction_bases_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_friction_bases_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_bases_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasesRejectReason expected);

/// Const preflight for contact-buffer friction-basis rebuild (B4.6 deepen pass).
struct ContactBufferFrictionBasesPreflight {
    ContactBufferFrictionBasesRejectReason reason = ContactBufferFrictionBasesRejectReason::None;
    bool emptyBuffer = false;
    bool noValidSlots = false;

    bool needs_rebuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }
};

/// Populate friction-bases preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionBasesPreflight preflight_contact_buffer_friction_bases(
    const ContactBufferSoA& buffer);

/// Returns true when buildFrictionTangentBases should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_bases(const ContactBufferSoA& buffer);

/// Returns true when buildFrictionTangentBases may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_friction_bases(const ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase
