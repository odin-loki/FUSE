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
    /// True when `maxCapacity` is set and no additional contacts may be stored after clamp.
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Remaining slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.5 deepen follow-up pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const ContactManifold& manifold);
    void invalidateSlot(u32 slot);
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

/// Why contact-buffer write would reject (B4.5 deepen follow-up pass).
enum class ContactBufferWriteRejectReason : u8 {
    None = 0,
    InvalidSlot,
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write reject reasons (B4.5 deepen follow-up pass).
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.5 deepen follow-up pass).
ContactBufferWriteRejectReason contact_buffer_write_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool contact_buffer_write_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteRejectReason expected);

/// Const preflight for contact-buffer slot write (B4.5 deepen follow-up pass).
struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool invalidSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool can_write() const { return reason == ContactBufferWriteRejectReason::None; }
};

/// Populate write preflight without mutating the buffer (B4.5 deepen follow-up pass).
ContactBufferWritePreflight preflight_contact_buffer_write(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when contact-buffer write should be skipped (B4.5 deepen follow-up pass).
bool should_skip_contact_buffer_write(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer compaction would early-out (B4.5 deepen follow-up pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for contact-buffer compaction reject reasons (B4.5 deepen follow-up pass).
const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.5 deepen follow-up pass).
ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

/// Const preflight for contact-buffer compaction (B4.5 deepen follow-up pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needs_compaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

/// Populate compaction preflight without mutating the buffer (B4.5 deepen follow-up pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer compaction should be skipped (B4.5 deepen follow-up pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer compaction should run (B4.5 deepen follow-up pass).
bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen follow-up pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

/// Human-readable label for contact-buffer clamp reject reasons (B4.5 deepen follow-up pass).
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.5 deepen follow-up pass).
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool contact_buffer_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected);

/// Const preflight for contact-buffer max-capacity clamp (B4.5 deepen follow-up pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needs_clamp() const { return reason == ContactBufferClampRejectReason::None; }
};

/// Populate clamp preflight without mutating the buffer (B4.5 deepen follow-up pass).
ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer clamp should be skipped (B4.5 deepen follow-up pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer clamp should run (B4.5 deepen follow-up pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen follow-up pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for compact-and-clamp reject reasons (B4.5 deepen follow-up pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.5 deepen follow-up pass).
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected);

/// Const preflight for contact-buffer compact-and-clamp (B4.5 deepen follow-up pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needs_compact_and_clamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }
};

/// Populate compact-and-clamp preflight without mutating the buffer (B4.5 deepen follow-up pass).
ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer compact-and-clamp should be skipped (B4.5 deepen follow-up pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer compact-and-clamp should run (B4.5 deepen follow-up pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase
