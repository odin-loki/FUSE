#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

#include <cstdint>
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
    /// Contact-list guard: true when `additionalCount` contacts fit before `maxCapacity` clamp.
    bool canAcceptContacts(u32 additionalCount = 1u) const;
    /// Remaining storage slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.3 deepen follow-up pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;
    /// True when no valid slots need friction-basis rebuild.
    bool canSkipBuildFrictionTangentBases() const;

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

FUSE_PHYSICS_INLINE u32 ContactBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    }
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
}

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canAcceptContacts(u32 additionalCount) const {
    if (additionalCount == 0u) {
        return true;
    }
    if (maxCapacity == 0u) {
        return true;
    }
    return activeCount + additionalCount <= maxCapacity;
}

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
}

FUSE_PHYSICS_INLINE u32 ContactBufferSoA::countValidSlots() const {
    if (canSkipSoAIteration()) {
        return 0u;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 validCount = 0u;
    for (u32 slot = 0u; slot < scanCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
        }
    }
    return validCount;
}

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canSkipCompaction() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    if (scanCount == 0u) {
        return true;
    }

    for (u32 slot = 0u; slot < scanCount; ++slot) {
        if (validFlags[slot] == 0u) {
            return false;
        }
    }
    return true;
}

FUSE_PHYSICS_INLINE bool ContactBufferSoA::slotIsValid(u32 slot) const {
    return slot < validFlags.size() && validFlags[slot] != 0u;
}

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canSkipBuildFrictionTangentBases() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;
}

/// Why contact-buffer slot write would reject (B4.3 deepen follow-up pass).
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

/// Returns true when `contactBufferWriteSlotRejectReason` matches `expected` (B4.3 deepen follow-up pass).
bool contactBufferWriteSlotRejectsForReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected);

/// Read-only write-slot diagnostics — no mutation (B4.3 deepen follow-up pass).
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

/// Non-mutating write-slot skip predicate — inverse of `canWrite` (B4.3 deepen follow-up pass).
bool canSkipContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot predicate — mirrors `preflightContactBufferWriteSlot` (B4.3 deepen follow-up pass).
bool shouldRunContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer compaction would early-out (B4.3 deepen follow-up pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for contact-buffer compaction reject reasons (logging / tests).
const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed.
ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.3 deepen follow-up pass).
bool contactBufferCompactionRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.3 deepen follow-up pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.3 deepen follow-up pass).
bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.3 deepen follow-up pass).
bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.3 deepen follow-up pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

/// Human-readable label for contact-buffer clamp reject reasons (logging / tests).
const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed.
ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.3 deepen follow-up pass).
bool contactBufferClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected);

/// Read-only max-capacity clamp diagnostics — no mutation (B4.3 deepen follow-up pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }
};

ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.3 deepen follow-up pass).
bool canSkipContactBufferClamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.3 deepen follow-up pass).
bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.3 deepen follow-up pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for contact-buffer compact-and-clamp reject reasons (logging / tests).
const char* contactBufferCompactAndClampRejectReasonName(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed.
ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(
    const ContactBufferSoA& buffer);

/// Returns true when `contactBufferCompactAndClampRejectReason` matches `expected` (B4.3 deepen follow-up pass).
bool contactBufferCompactAndClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.3 deepen follow-up pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }
};

ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.3 deepen follow-up pass).
bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflightContactBufferCompactAndClamp` (B4.3 deepen follow-up pass).
bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Why contact-buffer toVector would early-out (B4.3 deepen follow-up pass).
enum class ContactBufferToVectorRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

/// Human-readable label for contact-buffer toVector reject reasons (logging / tests).
const char* contactBufferToVectorRejectReasonName(ContactBufferToVectorRejectReason reason);

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed.
ContactBufferToVectorRejectReason contactBufferToVectorRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferToVectorRejectReason` matches `expected` (B4.3 deepen follow-up pass).
bool contactBufferToVectorRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected);

/// Read-only toVector diagnostics — no mutation (B4.3 deepen follow-up pass).
struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool canExport() const { return reason == ContactBufferToVectorRejectReason::None; }
};

ContactBufferToVectorPreflight preflightContactBufferToVector(const ContactBufferSoA& buffer);

/// Non-mutating toVector skip predicate — inverse of `canExport` (B4.3 deepen follow-up pass).
bool canSkipContactBufferToVector(const ContactBufferSoA& buffer);

/// Non-mutating toVector predicate — mirrors `preflightContactBufferToVector` (B4.3 deepen follow-up pass).
bool shouldRunContactBufferToVector(const ContactBufferSoA& buffer);

/// Why contact-buffer warm-start stub would reject (B4.3 deepen follow-up pass).
enum class ContactBufferWarmStartRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidSlot,
};

/// Human-readable label for contact-buffer warm-start reject reasons (logging / tests).
const char* contactBufferWarmStartRejectReasonName(ContactBufferWarmStartRejectReason reason);

/// Diagnose why applyWarmStartStub would reject; vacuously succeeds when warm-start may proceed.
ContactBufferWarmStartRejectReason contactBufferWarmStartRejectReason(
    const ContactBufferSoA& buffer,
    u32 slot);

/// Returns true when `contactBufferWarmStartRejectReason` matches `expected` (B4.3 deepen follow-up pass).
bool contactBufferWarmStartRejectsForReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactBufferWarmStartRejectReason expected);

/// Read-only warm-start diagnostics — no mutation (B4.3 deepen follow-up pass).
struct ContactBufferWarmStartPreflight {
    ContactBufferWarmStartRejectReason reason = ContactBufferWarmStartRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidSlot = false;

    bool canApply() const { return reason == ContactBufferWarmStartRejectReason::None; }
};

ContactBufferWarmStartPreflight preflightContactBufferWarmStart(
    const ContactBufferSoA& buffer,
    u32 slot);

/// Non-mutating warm-start skip predicate — inverse of `canApply` (B4.3 deepen follow-up pass).
bool canSkipContactBufferWarmStart(const ContactBufferSoA& buffer, u32 slot);

/// Non-mutating warm-start predicate — mirrors `preflightContactBufferWarmStart` (B4.3 deepen follow-up pass).
bool shouldRunContactBufferWarmStart(const ContactBufferSoA& buffer, u32 slot);

/// Why contact-buffer friction-basis rebuild would early-out (B4.3 deepen follow-up pass).
enum class ContactBufferFrictionBasesRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoValidSlots,
};

/// Human-readable label for contact-buffer friction-basis reject reasons (logging / tests).
const char* contactBufferFrictionBasesRejectReasonName(ContactBufferFrictionBasesRejectReason reason);

/// Diagnose why buildFrictionTangentBases would skip; vacuously succeeds when rebuild may proceed.
ContactBufferFrictionBasesRejectReason contactBufferFrictionBasesRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferFrictionBasesRejectReason` matches `expected` (B4.3 deepen follow-up pass).
bool contactBufferFrictionBasesRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasesRejectReason expected);

/// Read-only friction-basis rebuild diagnostics — no mutation (B4.3 deepen follow-up pass).
struct ContactBufferFrictionBasesPreflight {
    ContactBufferFrictionBasesRejectReason reason = ContactBufferFrictionBasesRejectReason::None;
    bool emptyBuffer = false;
    bool noValidSlots = false;

    bool needsRebuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }
};

ContactBufferFrictionBasesPreflight preflightContactBufferFrictionBases(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis skip predicate — inverse of `needsRebuild` (B4.3 deepen follow-up pass).
bool canSkipContactBufferFrictionBases(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis predicate — mirrors `preflightContactBufferFrictionBases` (B4.3 deepen follow-up pass).
bool shouldRunContactBufferFrictionBases(const ContactBufferSoA& buffer);

FUSE_PHYSICS_INLINE const char* contactBufferWriteSlotRejectReasonName(
    ContactBufferWriteSlotRejectReason reason) {
    switch (reason) {
    case ContactBufferWriteSlotRejectReason::None:
        return "None";
    case ContactBufferWriteSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case ContactBufferWriteSlotRejectReason::InvalidManifold:
        return "InvalidManifold";
    case ContactBufferWriteSlotRejectReason::SelfPair:
        return "SelfPair";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferWriteSlotRejectReason contactBufferWriteSlotRejectReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (slot >= buffer.pairSlotCount) {
        return ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    }
    if (!manifold.valid) {
        return ContactBufferWriteSlotRejectReason::InvalidManifold;
    }
    if (manifold.bodyA == manifold.bodyB) {
        return ContactBufferWriteSlotRejectReason::SelfPair;
    }
    return ContactBufferWriteSlotRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contactBufferWriteSlotRejectsForReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected) {
    return contactBufferWriteSlotRejectReason(buffer, slot, manifold) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferWriteSlotPreflight preflightContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    ContactBufferWriteSlotPreflight preflight{};
    preflight.reason = contactBufferWriteSlotRejectReason(buffer, slot, manifold);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    preflight.invalidManifold = preflight.reason == ContactBufferWriteSlotRejectReason::InvalidManifold;
    preflight.selfPair = preflight.reason == ContactBufferWriteSlotRejectReason::SelfPair;
    return preflight;
}

FUSE_PHYSICS_INLINE bool canSkipContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    return !preflightContactBufferWriteSlot(buffer, slot, manifold).canWrite();
}

FUSE_PHYSICS_INLINE bool shouldRunContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    return preflightContactBufferWriteSlot(buffer, slot, manifold).canWrite();
}

FUSE_PHYSICS_INLINE const char* contactBufferCompactionRejectReasonName(
    ContactBufferCompactionRejectReason reason) {
    switch (reason) {
    case ContactBufferCompactionRejectReason::None:
        return "None";
    case ContactBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactionRejectReason::AllValid:
        return "AllValid";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipCompaction()) {
        return ContactBufferCompactionRejectReason::AllValid;
    }
    return ContactBufferCompactionRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contactBufferCompactionRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected) {
    return contactBufferCompactionRejectReason(buffer) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferCompactionPreflight preflightContactBufferCompaction(
    const ContactBufferSoA& buffer) {
    ContactBufferCompactionPreflight preflight{};
    preflight.reason = contactBufferCompactionRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;
    return preflight;
}

FUSE_PHYSICS_INLINE bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer) {
    return !preflightContactBufferCompaction(buffer).needsCompaction();
}

FUSE_PHYSICS_INLINE bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer) {
    return preflightContactBufferCompaction(buffer).needsCompaction();
}

FUSE_PHYSICS_INLINE const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason) {
    switch (reason) {
    case ContactBufferClampRejectReason::None:
        return "None";
    case ContactBufferClampRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferClampRejectReason contactBufferClampRejectReason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferClampRejectReason::EmptyBuffer;
    }
    if (!buffer.canApplyMaxCapacityClamp()) {
        return ContactBufferClampRejectReason::WithinCapacity;
    }
    return ContactBufferClampRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contactBufferClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected) {
    return contactBufferClampRejectReason(buffer) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.reason = contactBufferClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;
    return preflight;
}

FUSE_PHYSICS_INLINE bool canSkipContactBufferClamp(const ContactBufferSoA& buffer) {
    return !preflightContactBufferClamp(buffer).needsClamp();
}

FUSE_PHYSICS_INLINE bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer) {
    return preflightContactBufferClamp(buffer).needsClamp();
}

FUSE_PHYSICS_INLINE const char* contactBufferCompactAndClampRejectReasonName(
    ContactBufferCompactAndClampRejectReason reason) {
    switch (reason) {
    case ContactBufferCompactAndClampRejectReason::None:
        return "None";
    case ContactBufferCompactAndClampRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactAndClampRejectReason::NoWork:
        return "NoWork";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipCompaction() && buffer.canSkipMaxCapacityClamp()) {
        return ContactBufferCompactAndClampRejectReason::NoWork;
    }
    return ContactBufferCompactAndClampRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contactBufferCompactAndClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected) {
    return contactBufferCompactAndClampRejectReason(buffer) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(
    const ContactBufferSoA& buffer) {
    ContactBufferCompactAndClampPreflight preflight{};
    preflight.reason = contactBufferCompactAndClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork;
    return preflight;
}

FUSE_PHYSICS_INLINE bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    return !preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();
}

FUSE_PHYSICS_INLINE bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    return preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();
}

FUSE_PHYSICS_INLINE const char* contactBufferToVectorRejectReasonName(
    ContactBufferToVectorRejectReason reason) {
    switch (reason) {
    case ContactBufferToVectorRejectReason::None:
        return "None";
    case ContactBufferToVectorRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferToVectorRejectReason contactBufferToVectorRejectReason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration() || buffer.isEmpty()) {
        return ContactBufferToVectorRejectReason::EmptyBuffer;
    }
    return ContactBufferToVectorRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contactBufferToVectorRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected) {
    return contactBufferToVectorRejectReason(buffer) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferToVectorPreflight preflightContactBufferToVector(
    const ContactBufferSoA& buffer) {
    ContactBufferToVectorPreflight preflight{};
    preflight.reason = contactBufferToVectorRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferToVectorRejectReason::EmptyBuffer;
    return preflight;
}

FUSE_PHYSICS_INLINE bool canSkipContactBufferToVector(const ContactBufferSoA& buffer) {
    return !preflightContactBufferToVector(buffer).canExport();
}

FUSE_PHYSICS_INLINE bool shouldRunContactBufferToVector(const ContactBufferSoA& buffer) {
    return preflightContactBufferToVector(buffer).canExport();
}

FUSE_PHYSICS_INLINE const char* contactBufferWarmStartRejectReasonName(
    ContactBufferWarmStartRejectReason reason) {
    switch (reason) {
    case ContactBufferWarmStartRejectReason::None:
        return "None";
    case ContactBufferWarmStartRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case ContactBufferWarmStartRejectReason::InvalidSlot:
        return "InvalidSlot";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferWarmStartRejectReason contactBufferWarmStartRejectReason(
    const ContactBufferSoA& buffer,
    u32 slot) {
    if (slot >= buffer.pairSlotCount) {
        return ContactBufferWarmStartRejectReason::OutOfRangeSlot;
    }
    if (!buffer.slotIsValid(slot)) {
        return ContactBufferWarmStartRejectReason::InvalidSlot;
    }
    return ContactBufferWarmStartRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contactBufferWarmStartRejectsForReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactBufferWarmStartRejectReason expected) {
    return contactBufferWarmStartRejectReason(buffer, slot) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferWarmStartPreflight preflightContactBufferWarmStart(
    const ContactBufferSoA& buffer,
    u32 slot) {
    ContactBufferWarmStartPreflight preflight{};
    preflight.reason = contactBufferWarmStartRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWarmStartRejectReason::OutOfRangeSlot;
    preflight.invalidSlot = preflight.reason == ContactBufferWarmStartRejectReason::InvalidSlot;
    return preflight;
}

FUSE_PHYSICS_INLINE bool canSkipContactBufferWarmStart(const ContactBufferSoA& buffer, u32 slot) {
    return !preflightContactBufferWarmStart(buffer, slot).canApply();
}

FUSE_PHYSICS_INLINE bool shouldRunContactBufferWarmStart(const ContactBufferSoA& buffer, u32 slot) {
    return preflightContactBufferWarmStart(buffer, slot).canApply();
}

FUSE_PHYSICS_INLINE const char* contactBufferFrictionBasesRejectReasonName(
    ContactBufferFrictionBasesRejectReason reason) {
    switch (reason) {
    case ContactBufferFrictionBasesRejectReason::None:
        return "None";
    case ContactBufferFrictionBasesRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferFrictionBasesRejectReason::NoValidSlots:
        return "NoValidSlots";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferFrictionBasesRejectReason contactBufferFrictionBasesRejectReason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferFrictionBasesRejectReason::EmptyBuffer;
    }
    if (buffer.countValidSlots() == 0u) {
        return ContactBufferFrictionBasesRejectReason::NoValidSlots;
    }
    return ContactBufferFrictionBasesRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contactBufferFrictionBasesRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasesRejectReason expected) {
    return contactBufferFrictionBasesRejectReason(buffer) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferFrictionBasesPreflight preflightContactBufferFrictionBases(
    const ContactBufferSoA& buffer) {
    ContactBufferFrictionBasesPreflight preflight{};
    preflight.reason = contactBufferFrictionBasesRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBasesRejectReason::EmptyBuffer;
    preflight.noValidSlots = preflight.reason == ContactBufferFrictionBasesRejectReason::NoValidSlots;
    return preflight;
}

FUSE_PHYSICS_INLINE bool canSkipContactBufferFrictionBases(const ContactBufferSoA& buffer) {
    return !preflightContactBufferFrictionBases(buffer).needsRebuild();
}

FUSE_PHYSICS_INLINE bool shouldRunContactBufferFrictionBases(const ContactBufferSoA& buffer) {
    return preflightContactBufferFrictionBases(buffer).needsRebuild();
}

} // namespace fuse::physics::narrowphase
