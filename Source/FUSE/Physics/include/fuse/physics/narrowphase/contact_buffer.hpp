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

    /// True when both dense and slot storage are empty (safe to skip SoA scans, B4.6 deepen pass).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }

    /// True when slot storage has no invalid flags (compact is a no-op, B4.6 deepen pass).
    bool canSkipCompaction() const;

    /// True when post-pass truncation would drop contacts (B4.6 deepen pass).
    bool canApplyMaxCapacityClamp() const {
        return maxCapacity > 0u && activeCount > maxCapacity;
    }

    /// True when max-capacity clamp would leave the buffer unchanged (B4.6 deepen pass).
    bool canSkipClamp() const { return !canApplyMaxCapacityClamp(); }

    /// True when compact+clamp would leave the buffer unchanged (B4.6 deepen pass).
    bool canSkipCompactAndClamp() const;

    /// Count valid flags in prepared slot storage before compaction (B4.6 deepen pass).
    u32 countValidSlots() const;

    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }

    /// Remaining write slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`, B4.6 deepen pass).
    u32 remainingCapacity() const {
        if (maxCapacity == 0u) {
            return ~0u;
        }
        return activeCount >= maxCapacity ? 0u : maxCapacity - activeCount;
    }

    bool slotIsValid(u32 slot) const {
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

inline u32 ContactBufferSoA::countValidSlots() const {
    if (canSkipSoAIteration()) {
        return 0u;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 validCount = 0u;
    for (u32 slot = 0u; slot < scanCount; ++slot) {
        if (slotIsValid(slot)) {
            ++validCount;
        }
    }
    return validCount;
}

inline bool ContactBufferSoA::canSkipCompaction() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    if (scanCount == 0u) {
        return true;
    }

    for (u32 slot = 0u; slot < scanCount; ++slot) {
        if (!slotIsValid(slot)) {
            return false;
        }
    }
    return true;
}

inline bool ContactBufferSoA::canSkipCompactAndClamp() const {
    if (canSkipSoAIteration()) {
        return true;
    }
    if (!canSkipCompaction()) {
        return false;
    }
    if (!canSkipClamp()) {
        return false;
    }
    return activeCount == countValidSlots();
}

/// Why contact-buffer slot write would reject (B4.6 deepen pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write-slot reject reasons (B4.6 deepen pass).
inline const char* contactBufferWriteSlotRejectReasonName(ContactBufferWriteSlotRejectReason reason) {
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

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed (B4.6 deepen pass).
inline ContactBufferWriteSlotRejectReason contactBufferWriteSlotRejectReason(
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

/// Returns true when `contactBufferWriteSlotRejectReason` matches `expected` (B4.6 deepen pass).
inline bool contactBufferWriteSlotRejectsForReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected) {
    return contactBufferWriteSlotRejectReason(buffer, slot, manifold) == expected;
}

/// Read-only write-slot diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool canWrite() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

inline ContactBufferWriteSlotPreflight preflightContactBufferWriteSlot(
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

/// Non-mutating write-slot skip predicate — inverse of `canWrite` (B4.6 deepen pass).
inline bool canSkipContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    return !preflightContactBufferWriteSlot(buffer, slot, manifold).canWrite();
}

/// Non-mutating write-slot predicate — mirrors `preflightContactBufferWriteSlot` (B4.6 deepen pass).
inline bool shouldRunContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    return preflightContactBufferWriteSlot(buffer, slot, manifold).canWrite();
}

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

inline const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason) {
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

inline ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipCompaction()) {
        return ContactBufferCompactionRejectReason::AllValid;
    }
    return ContactBufferCompactionRejectReason::None;
}

inline bool contactBufferCompactionRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected) {
    return contactBufferCompactionRejectReason(buffer) == expected;
}

struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

inline ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer) {
    ContactBufferCompactionPreflight preflight{};
    preflight.reason = contactBufferCompactionRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;
    return preflight;
}

inline bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer) {
    return !preflightContactBufferCompaction(buffer).needsCompaction();
}

inline bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer) {
    return preflightContactBufferCompaction(buffer).needsCompaction();
}

/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

inline const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason) {
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

inline ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferClampRejectReason::EmptyBuffer;
    }
    if (!buffer.canApplyMaxCapacityClamp()) {
        return ContactBufferClampRejectReason::WithinCapacity;
    }
    return ContactBufferClampRejectReason::None;
}

inline bool contactBufferClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected) {
    return contactBufferClampRejectReason(buffer) == expected;
}

struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }
};

inline ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.reason = contactBufferClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;
    return preflight;
}

inline bool canSkipContactBufferClamp(const ContactBufferSoA& buffer) {
    return !preflightContactBufferClamp(buffer).needsClamp();
}

inline bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer) {
    return preflightContactBufferClamp(buffer).needsClamp();
}

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

inline const char* contactBufferCompactAndClampRejectReasonName(
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

inline ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(
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

inline bool contactBufferCompactAndClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected) {
    return contactBufferCompactAndClampRejectReason(buffer) == expected;
}

struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompactAndClamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }
};

inline ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(
    const ContactBufferSoA& buffer) {
    ContactBufferCompactAndClampPreflight preflight{};
    preflight.reason = contactBufferCompactAndClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork;
    return preflight;
}

inline bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    return !preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();
}

inline bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    return preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();
}

/// Why contact-buffer toVector would early-out (B4.6 deepen pass).
enum class ContactBufferToVectorRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

inline const char* contactBufferToVectorRejectReasonName(ContactBufferToVectorRejectReason reason) {
    switch (reason) {
    case ContactBufferToVectorRejectReason::None:
        return "None";
    case ContactBufferToVectorRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    }
    return "Unknown";
}

inline ContactBufferToVectorRejectReason contactBufferToVectorRejectReason(const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferToVectorRejectReason::EmptyBuffer;
    }
    return ContactBufferToVectorRejectReason::None;
}

inline bool contactBufferToVectorRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected) {
    return contactBufferToVectorRejectReason(buffer) == expected;
}

struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool canExport() const { return reason == ContactBufferToVectorRejectReason::None; }
};

inline ContactBufferToVectorPreflight preflightContactBufferToVector(const ContactBufferSoA& buffer) {
    ContactBufferToVectorPreflight preflight{};
    preflight.reason = contactBufferToVectorRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferToVectorRejectReason::EmptyBuffer;
    return preflight;
}

inline bool canSkipContactBufferToVector(const ContactBufferSoA& buffer) {
    return !preflightContactBufferToVector(buffer).canExport();
}

inline bool shouldRunContactBufferToVector(const ContactBufferSoA& buffer) {
    return preflightContactBufferToVector(buffer).canExport();
}

/// Compact only when preflight allows; returns active count (B4.6 deepen pass).
inline u32 compactContactBufferWithPreflight(ContactBufferSoA& buffer) {
    if (!preflightContactBufferCompaction(buffer).needsCompaction()) {
        return buffer.activeCount;
    }
    return buffer.compact();
}

/// Compact and clamp only when preflight allows; returns active count (B4.6 deepen pass).
inline u32 compactAndClampContactBufferWithPreflight(ContactBufferSoA& buffer) {
    if (!preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp()) {
        return buffer.activeCount;
    }
    return buffer.compactAndClamp();
}

} // namespace fuse::physics::narrowphase
