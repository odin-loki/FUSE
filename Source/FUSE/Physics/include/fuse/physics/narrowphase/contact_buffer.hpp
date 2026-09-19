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
    /// True when `maxCapacity` is set and no additional contacts may be written.
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Remaining write slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.6 deepen pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// True when compact+clamp would leave the buffer unchanged.
    bool canSkipCompactAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const ContactManifold& manifold);
    /// Guarded write using preflight; returns false when write is rejected (B4.6 deepen pass).
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    void invalidateSlot(u32 slot);
    void applyWarmStartStub(u32 slot, ContactManifold& manifold) const;
    void buildFrictionTangentBases();
    /// Rebuild tangent SoA columns only when preflight reports stale or missing frames (B4.6 deepen pass).
    void buildFrictionTangentBasesIfNeeded();
    u32 compact();
    u32 applyMaxCapacityClamp();
    u32 compactAndClamp();
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer write would reject (B4.6 deepen pass).
enum class ContactBufferWriteRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write reject reasons (B4.6 deepen pass).
const char* contactBufferWriteRejectReasonName(ContactBufferWriteRejectReason reason);

/// Diagnose why write would reject; vacuously succeeds when write may proceed.
ContactBufferWriteRejectReason contactBufferWriteRejectReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferWriteRejectsForReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteRejectReason expected);

/// Read-only write diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool canWrite() const { return reason == ContactBufferWriteRejectReason::None; }
};

ContactBufferWritePreflight preflightContactBufferWrite(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write skip predicate — inverse of `canWrite` (B4.6 deepen pass).
bool canSkipContactBufferWrite(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason);

ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer);

bool contactBufferCompactionRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer);

bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer);

bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason);

ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer);

bool contactBufferClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected);

struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }
};

ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer);

bool canSkipContactBufferClamp(const ContactBufferSoA& buffer);

bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

const char* contactBufferCompactAndClampRejectReasonName(ContactBufferCompactAndClampRejectReason reason);

ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(
    const ContactBufferSoA& buffer);

bool contactBufferCompactAndClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected);

struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompactAndClamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }
};

ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-basis rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionRebuildRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllOrthonormal,
};

const char* contactBufferFrictionRebuildRejectReasonName(ContactBufferFrictionRebuildRejectReason reason);

ContactBufferFrictionRebuildRejectReason contactBufferFrictionRebuildRejectReason(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

bool contactBufferFrictionRebuildRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionRebuildRejectReason expected,
    f32 epsilon = 1e-4f);

struct ContactBufferFrictionRebuildPreflight {
    ContactBufferFrictionRebuildRejectReason reason = ContactBufferFrictionRebuildRejectReason::None;
    bool emptyBuffer = false;
    bool allOrthonormal = false;

    bool needsRebuild() const { return reason == ContactBufferFrictionRebuildRejectReason::None; }
};

ContactBufferFrictionRebuildPreflight preflightContactBufferFrictionRebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

bool canSkipContactBufferFrictionRebuild(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

bool shouldRunContactBufferFrictionRebuild(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

} // namespace fuse::physics::narrowphase
