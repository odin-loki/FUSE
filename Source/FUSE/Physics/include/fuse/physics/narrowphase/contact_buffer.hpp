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
    /// True when compact+clamp would leave the buffer unchanged.
    bool canSkipCompactAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.6 deepen pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }

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

/// Why contact-buffer write-slot would reject (B4.6 deepen pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write-slot reject reasons (logging / tests).
const char* contactBufferWriteSlotRejectReasonName(ContactBufferWriteSlotRejectReason reason);

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed.
ContactBufferWriteSlotRejectReason contactBufferWriteSlotRejectReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contactBufferWriteSlotRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferWriteSlotRejectsForReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected);

/// Read-only write-slot diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool canWrite() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

ContactBufferWriteSlotPreflight preflightContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot skip predicate — inverse of `canWrite` (B4.6 deepen pass).
bool canSkipContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot predicate — mirrors `preflightContactBufferWriteSlot` (B4.6 deepen pass).
bool shouldRunContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for contact-buffer compaction reject reasons (logging / tests).
const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed.
ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferCompactionRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.6 deepen pass).
bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.6 deepen pass).
bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

/// Human-readable label for contact-buffer clamp reject reasons (logging / tests).
const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed.
ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferClampRejectsForReason(const ContactBufferSoA& buffer, ContactBufferClampRejectReason expected);

/// Read-only max-capacity clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }
};

ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.6 deepen pass).
bool canSkipContactBufferClamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.6 deepen pass).
bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for compact-and-clamp reject reasons (logging / tests).
const char* contactBufferCompactAndClampRejectReasonName(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed.
ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferCompactAndClampRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferCompactAndClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }
};

ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.6 deepen pass).
bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflightContactBufferCompactAndClamp` (B4.6 deepen pass).
bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Why contact-buffer toVector would early-out (B4.6 deepen pass).
enum class ContactBufferToVectorRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

/// Human-readable label for contact-buffer toVector reject reasons (logging / tests).
const char* contactBufferToVectorRejectReasonName(ContactBufferToVectorRejectReason reason);

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed.
ContactBufferToVectorRejectReason contactBufferToVectorRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferToVectorRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferToVectorRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected);

/// Read-only toVector diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool canExport() const { return reason == ContactBufferToVectorRejectReason::None; }
};

ContactBufferToVectorPreflight preflightContactBufferToVector(const ContactBufferSoA& buffer);

/// Non-mutating toVector skip predicate — inverse of `canExport` (B4.6 deepen pass).
bool canSkipContactBufferToVector(const ContactBufferSoA& buffer);

/// Non-mutating toVector predicate — mirrors `preflightContactBufferToVector` (B4.6 deepen pass).
bool shouldRunContactBufferToVector(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-basis build would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionBasisRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoValidContacts,
};

/// Human-readable label for friction-basis build reject reasons (logging / tests).
const char* contactBufferFrictionBasisRejectReasonName(ContactBufferFrictionBasisRejectReason reason);

/// Diagnose why buildFrictionTangentBases would skip; vacuously succeeds when build may proceed.
ContactBufferFrictionBasisRejectReason contactBufferFrictionBasisRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferFrictionBasisRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferFrictionBasisRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasisRejectReason expected);

/// Read-only friction-basis build diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionBasisPreflight {
    ContactBufferFrictionBasisRejectReason reason = ContactBufferFrictionBasisRejectReason::None;
    bool emptyBuffer = false;
    bool noValidContacts = false;

    bool canBuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }
};

ContactBufferFrictionBasisPreflight preflightContactBufferFrictionBasis(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis build skip predicate — inverse of `canBuild` (B4.6 deepen pass).
bool canSkipContactBufferFrictionBasis(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis build predicate — mirrors `preflightContactBufferFrictionBasis` (B4.6 deepen pass).
bool shouldRunContactBufferFrictionBasis(const ContactBufferSoA& buffer);

/// Write slot only when preflight allows; returns false when skipped (B4.6 deepen pass).
bool writeContactBufferSlotWithPreflight(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Compact only when preflight allows; returns active count (B4.6 deepen pass).
u32 compactContactBufferWithPreflight(ContactBufferSoA& buffer);

/// Build friction tangents only when preflight allows (B4.6 deepen pass).
void buildContactBufferFrictionBasesWithPreflight(ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase
