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
    bool hasDroppedContacts() const { return droppedCount > 0u; }
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }

    FUSE_PHYSICS_INLINE bool canAcceptContacts(u32 additionalCount = 1u) const {
        if (additionalCount == 0u) {
            return true;
        }
        if (maxCapacity == 0u) {
            return true;
        }
        return activeCount + additionalCount <= maxCapacity;
    }

    FUSE_PHYSICS_INLINE u32 remainingCapacity() const {
        if (maxCapacity == 0u) {
            return UINT32_MAX;
        }
        return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
    }

    FUSE_PHYSICS_INLINE bool canApplyMaxCapacityClamp() const {
        return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
    }

    FUSE_PHYSICS_INLINE bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }

    FUSE_PHYSICS_INLINE bool canSkipSoAIteration() const {
        return activeCount == 0u && pairSlotCount == 0u;
    }

    FUSE_PHYSICS_INLINE u32 countValidSlots() const {
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

    FUSE_PHYSICS_INLINE bool canSkipCompaction() const {
        if (canSkipSoAIteration()) {
            return true;
        }

        const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
        if (scanCount == 0u) {
            return true;
        }

        u32 lastValidSlot = UINT32_MAX;
        for (u32 slot = 0u; slot < scanCount; ++slot) {
            if (validFlags[slot] != 0u) {
                lastValidSlot = slot;
            }
        }
        if (lastValidSlot == UINT32_MAX) {
            return true;
        }

        for (u32 slot = 0u; slot < lastValidSlot; ++slot) {
            if (validFlags[slot] == 0u) {
                return false;
            }
        }
        return true;
    }

    FUSE_PHYSICS_INLINE bool slotIsValid(u32 slot) const {
        return slot < validFlags.size() && validFlags[slot] != 0u;
    }

    FUSE_PHYSICS_INLINE bool canSkipFrictionTangentBases(f32 epsilon = 1e-4f) const {
        if (canSkipSoAIteration()) {
            return true;
        }

        const u32 scanCount = activeCount > 0u ? activeCount : pairSlotCount;
        bool sawValidSlot = false;
        for (u32 slot = 0u; slot < scanCount; ++slot) {
            if (!slotIsValid(slot)) {
                continue;
            }
            sawValidSlot = true;
            const TangentBasis basis{tangent1[slot], tangent2[slot]};
            if (!isOrthonormalTangentBasis(contactNormals[slot], basis, epsilon)) {
                return false;
            }
        }
        return sawValidSlot;
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

    /// Guarded slot write — returns false when preflight rejects (B4.5 deepen follow-up pass).
    bool tryWriteSlot(u32 slot, const ContactManifold& manifold);

    /// Guarded compaction — returns prior active count when skipped (B4.5 deepen follow-up pass).
    u32 tryCompact();

    /// Guarded max-capacity clamp — returns prior active count when skipped (B4.5 deepen follow-up pass).
    u32 tryApplyMaxCapacityClamp();

    /// Guarded compact-and-clamp — returns prior active count when skipped (B4.5 deepen follow-up pass).
    u32 tryCompactAndClamp();

    /// Guarded export — returns empty when preflight rejects (B4.5 deepen follow-up pass).
    std::vector<ContactManifold> tryToVector() const;

    /// Guarded friction-basis rebuild — returns false when preflight rejects (B4.5 deepen follow-up pass).
    bool tryBuildFrictionTangentBases(f32 epsilon = 1e-4f);

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer slot write would reject (B4.5 deepen follow-up pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfContact,
};

/// Human-readable label for contact-buffer write-slot reject reasons (logging / tests).
const char* contactBufferWriteSlotRejectReasonName(ContactBufferWriteSlotRejectReason reason);

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed.
ContactBufferWriteSlotRejectReason contactBufferWriteSlotRejectReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contactBufferWriteSlotRejectReason` matches `expected`.
bool contactBufferWriteSlotRejectsForReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected);

/// Read-only write-slot diagnostics — no mutation (B4.5 deepen follow-up pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfContact = false;

    bool canWrite() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

ContactBufferWriteSlotPreflight preflightContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

bool canSkipContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

bool shouldRunContactBufferWriteSlot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer compaction would early-out (B4.5 deepen follow-up pass).
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

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen follow-up pass).
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

/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen follow-up pass).
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

    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }
};

ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Why contact-buffer toVector would early-out (B4.5 deepen follow-up pass).
enum class ContactBufferToVectorRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

const char* contactBufferToVectorRejectReasonName(ContactBufferToVectorRejectReason reason);

ContactBufferToVectorRejectReason contactBufferToVectorRejectReason(const ContactBufferSoA& buffer);

bool contactBufferToVectorRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected);

struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool canExport() const { return reason == ContactBufferToVectorRejectReason::None; }
};

ContactBufferToVectorPreflight preflightContactBufferToVector(const ContactBufferSoA& buffer);

bool canSkipContactBufferToVector(const ContactBufferSoA& buffer);

bool shouldRunContactBufferToVector(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-tangent rebuild would early-out (B4.5 deepen follow-up pass).
enum class ContactBufferFrictionTangentRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

const char* contactBufferFrictionTangentRejectReasonName(ContactBufferFrictionTangentRejectReason reason);

ContactBufferFrictionTangentRejectReason contactBufferFrictionTangentRejectReason(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

bool contactBufferFrictionTangentRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionTangentRejectReason expected,
    f32 epsilon = 1e-4f);

struct ContactBufferFrictionTangentPreflight {
    ContactBufferFrictionTangentRejectReason reason = ContactBufferFrictionTangentRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsRebuild() const { return reason == ContactBufferFrictionTangentRejectReason::None; }
};

ContactBufferFrictionTangentPreflight preflightContactBufferFrictionTangentBases(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

bool canSkipContactBufferFrictionTangentBases(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

bool shouldRunContactBufferFrictionTangentBases(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

FUSE_PHYSICS_INLINE const char* contactBufferWriteSlotRejectReasonName(ContactBufferWriteSlotRejectReason reason) {
    switch (reason) {
    case ContactBufferWriteSlotRejectReason::None:
        return "None";
    case ContactBufferWriteSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case ContactBufferWriteSlotRejectReason::InvalidManifold:
        return "InvalidManifold";
    case ContactBufferWriteSlotRejectReason::SelfContact:
        return "SelfContact";
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
        return ContactBufferWriteSlotRejectReason::SelfContact;
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
    preflight.selfContact = preflight.reason == ContactBufferWriteSlotRejectReason::SelfContact;
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

FUSE_PHYSICS_INLINE const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason) {
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

    const u32 validCount = buffer.countValidSlots();
    if (validCount == 0u) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;
    }

    if (buffer.canSkipCompaction() && buffer.activeCount == validCount) {
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

FUSE_PHYSICS_INLINE ContactBufferToVectorPreflight preflightContactBufferToVector(const ContactBufferSoA& buffer) {
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

FUSE_PHYSICS_INLINE const char* contactBufferFrictionTangentRejectReasonName(
    ContactBufferFrictionTangentRejectReason reason) {
    switch (reason) {
    case ContactBufferFrictionTangentRejectReason::None:
        return "None";
    case ContactBufferFrictionTangentRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferFrictionTangentRejectReason::AllValid:
        return "AllValid";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferFrictionTangentRejectReason contactBufferFrictionTangentRejectReason(
    const ContactBufferSoA& buffer,
    f32 epsilon) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferFrictionTangentRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipFrictionTangentBases(epsilon)) {
        return ContactBufferFrictionTangentRejectReason::AllValid;
    }
    return ContactBufferFrictionTangentRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contactBufferFrictionTangentRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionTangentRejectReason expected,
    f32 epsilon) {
    return contactBufferFrictionTangentRejectReason(buffer, epsilon) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferFrictionTangentPreflight preflightContactBufferFrictionTangentBases(
    const ContactBufferSoA& buffer,
    f32 epsilon) {
    ContactBufferFrictionTangentPreflight preflight{};
    preflight.reason = contactBufferFrictionTangentRejectReason(buffer, epsilon);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionTangentRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferFrictionTangentRejectReason::AllValid;
    return preflight;
}

FUSE_PHYSICS_INLINE bool canSkipContactBufferFrictionTangentBases(const ContactBufferSoA& buffer, f32 epsilon) {
    return !preflightContactBufferFrictionTangentBases(buffer, epsilon).needsRebuild();
}

FUSE_PHYSICS_INLINE bool shouldRunContactBufferFrictionTangentBases(const ContactBufferSoA& buffer, f32 epsilon) {
    return preflightContactBufferFrictionTangentBases(buffer, epsilon).needsRebuild();
}

FUSE_PHYSICS_INLINE bool ContactBufferSoA::tryWriteSlot(u32 slot, const ContactManifold& manifold) {
    if (!preflightContactBufferWriteSlot(*this, slot, manifold).canWrite()) {
        return false;
    }
    writeSlot(slot, manifold);
    return true;
}

FUSE_PHYSICS_INLINE u32 ContactBufferSoA::tryCompact() {
    if (!shouldRunContactBufferCompaction(*this)) {
        return activeCount;
    }
    return compact();
}

FUSE_PHYSICS_INLINE u32 ContactBufferSoA::tryApplyMaxCapacityClamp() {
    if (!shouldRunContactBufferClamp(*this)) {
        return activeCount;
    }
    return applyMaxCapacityClamp();
}

FUSE_PHYSICS_INLINE u32 ContactBufferSoA::tryCompactAndClamp() {
    if (!shouldRunContactBufferCompactAndClamp(*this)) {
        return activeCount;
    }
    return compactAndClamp();
}

FUSE_PHYSICS_INLINE std::vector<ContactManifold> ContactBufferSoA::tryToVector() const {
    if (!shouldRunContactBufferToVector(*this)) {
        return {};
    }
    return toVector();
}

FUSE_PHYSICS_INLINE bool ContactBufferSoA::tryBuildFrictionTangentBases(f32 epsilon) {
    if (!shouldRunContactBufferFrictionTangentBases(*this, epsilon)) {
        return false;
    }
    buildFrictionTangentBases();
    return true;
}

} // namespace fuse::physics::narrowphase
