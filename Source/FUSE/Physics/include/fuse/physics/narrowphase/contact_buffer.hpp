#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

#include <climits>
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
    /// True when `maxCapacity` is set and no additional contacts may be stored.
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Remaining storage slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    FUSE_PHYSICS_INLINE u32 remainingCapacity() const {
        if (maxCapacity == 0u) {
            return UINT32_MAX;
        }
        return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
    }
    /// True when post-pass truncation would drop contacts.
    FUSE_PHYSICS_INLINE bool canApplyMaxCapacityClamp() const {
        return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
    }
    /// Inverse of `canApplyMaxCapacityClamp` (B4.6 deepen pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    FUSE_PHYSICS_INLINE bool canSkipCompaction() const {
        if (canSkipSoAIteration()) {
            return true;
        }

        const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
        if (scanCount == 0u) {
            return true;
        }

        for (u32 slot = 0; slot < scanCount; ++slot) {
            if (validFlags[slot] == 0u) {
                return false;
            }
        }
        return true;
    }
    /// Count valid flags in prepared slot storage before compaction.
    FUSE_PHYSICS_INLINE u32 countValidSlots() const {
        if (canSkipSoAIteration()) {
            return 0u;
        }

        const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
        u32 validCount = 0u;
        for (u32 slot = 0; slot < scanCount; ++slot) {
            if (validFlags[slot] != 0u) {
                ++validCount;
            }
        }
        return validCount;
    }
    FUSE_PHYSICS_INLINE bool slotIsValid(u32 slot) const {
        return slot < validFlags.size() && validFlags[slot] != 0u;
    }

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
const char* contactBufferWriteSlotRejectReasonName(ContactBufferWriteSlotRejectReason reason);

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed (B4.6 deepen pass).
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

/// Human-readable label for contact-buffer compaction reject reasons (B4.6 deepen pass).
const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed (B4.6 deepen pass).
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

/// Human-readable label for contact-buffer clamp reject reasons (B4.6 deepen pass).
const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.6 deepen pass).
ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected);

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

/// Human-readable label for compact-and-clamp reject reasons (B4.6 deepen pass).
const char* contactBufferCompactAndClampRejectReasonName(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen pass).
ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(
    const ContactBufferSoA& buffer);

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

/// Human-readable label for contact-buffer toVector reject reasons (B4.6 deepen pass).
const char* contactBufferToVectorRejectReasonName(ContactBufferToVectorRejectReason reason);

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed (B4.6 deepen pass).
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

/// Compact only when `preflightContactBufferCompaction` allows; returns active count (B4.6 deepen pass).
u32 compactContactBufferWithPreflight(ContactBufferSoA& buffer);

/// Clamp only when `preflightContactBufferClamp` allows; returns active count (B4.6 deepen pass).
u32 applyMaxCapacityClampWithPreflight(ContactBufferSoA& buffer);

/// Compact and clamp only when `preflightContactBufferCompactAndClamp` allows (B4.6 deepen pass).
u32 compactAndClampContactBufferWithPreflight(ContactBufferSoA& buffer);

/// Write slot only when `preflightContactBufferWriteSlot` allows; returns false when skipped (B4.6 deepen pass).
bool writeContactBufferSlotWithPreflight(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Guarded slot write — same guards as `writeContactBufferSlotWithPreflight` (B4.6 deepen pass).
bool tryWriteContactBufferSlot(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

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
    if (buffer.canSkipMaxCapacityClamp()) {
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
    if (!shouldRunContactBufferCompaction(buffer) && !shouldRunContactBufferClamp(buffer)) {
        const u32 validCount = buffer.countValidSlots();
        if (buffer.pairSlotCount > 0u && buffer.activeCount != validCount) {
            return ContactBufferCompactAndClampRejectReason::None;
        }
        if (buffer.activeCount != validCount) {
            return ContactBufferCompactAndClampRejectReason::None;
        }
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

FUSE_PHYSICS_INLINE const char* contactBufferToVectorRejectReasonName(ContactBufferToVectorRejectReason reason) {
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
    if (buffer.canSkipSoAIteration()) {
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

FUSE_PHYSICS_INLINE u32 compactContactBufferWithPreflight(ContactBufferSoA& buffer) {
    if (canSkipContactBufferCompaction(buffer)) {
        return buffer.activeCount;
    }
    return buffer.compact();
}

FUSE_PHYSICS_INLINE u32 applyMaxCapacityClampWithPreflight(ContactBufferSoA& buffer) {
    if (canSkipContactBufferClamp(buffer)) {
        return buffer.activeCount;
    }
    return buffer.applyMaxCapacityClamp();
}

FUSE_PHYSICS_INLINE u32 compactAndClampContactBufferWithPreflight(ContactBufferSoA& buffer) {
    if (canSkipContactBufferCompactAndClamp(buffer)) {
        return buffer.activeCount;
    }
    return buffer.compactAndClamp();
}

FUSE_PHYSICS_INLINE bool writeContactBufferSlotWithPreflight(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (!shouldRunContactBufferWriteSlot(buffer, slot, manifold)) {
        return false;
    }
    buffer.writeSlot(slot, manifold);
    return true;
}

FUSE_PHYSICS_INLINE bool tryWriteContactBufferSlot(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    return writeContactBufferSlotWithPreflight(buffer, slot, manifold);
}

} // namespace fuse::physics::narrowphase
