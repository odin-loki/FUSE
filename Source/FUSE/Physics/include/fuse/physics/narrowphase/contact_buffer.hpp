#pragma once

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
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.5 deepen pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when all active tangent frames are orthonormal with their normals (B4.5 deepen pass).
    bool canSkipFrictionTangentRebuild(f32 epsilon = 1e-4f) const;
    /// True when compact+clamp would leave the buffer unchanged (B4.5 deepen pass).
    bool canSkipCompactAndClamp() const;
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const ContactManifold& manifold);
    void applyWarmStartStub(u32 slot, ContactManifold& manifold) const;
    void buildFrictionTangentBases();
    /// Rebuild tangent SoA columns only when `canSkipFrictionTangentRebuild` is false (B4.5 deepen pass).
    void buildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    void invalidateSlot(u32 slot);
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
        if (slot < validFlags.size() && validFlags[slot] != 0u) {
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
        if (slot >= validFlags.size() || validFlags[slot] == 0u) {
            return false;
        }
    }
    return true;
}

inline u32 ContactBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    }
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
}

inline bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
}

inline bool ContactBufferSoA::canSkipFrictionTangentRebuild(f32 epsilon) const {
    if (canSkipSoAIteration() || activeCount == 0u) {
        return true;
    }

    for (u32 slot = 0u; slot < activeCount; ++slot) {
        if (!slotIsValid(slot)) {
            continue;
        }
        if (!isOrthonormalTangentBasis(contactNormals[slot], {tangent1[slot], tangent2[slot]}, epsilon)) {
            return false;
        }
    }
    return true;
}

inline bool ContactBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || (canSkipCompaction() && canSkipMaxCapacityClamp());
}

inline bool ContactBufferSoA::slotIsValid(u32 slot) const {
    if (slot >= validFlags.size()) {
        return false;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return false;
    }
    return validFlags[slot] != 0u;
}

inline void ContactBufferSoA::invalidateSlot(u32 slot) {
    if (slot >= validFlags.size()) {
        return;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return;
    }
    validFlags[slot] = 0u;
}

inline void ContactBufferSoA::buildFrictionTangentBasesIfNeeded(f32 epsilon) {
    if (canSkipFrictionTangentRebuild(epsilon)) {
        return;
    }
    buildFrictionTangentBases();
}

/// Why contact-buffer write-slot would reject (B4.5 deepen pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write-slot reject reasons (B4.5 deepen pass).
const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason);

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed (B4.5 deepen pass).
ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contact_buffer_write_slot_reject_reason` matches `expected` (B4.5 deepen pass).
bool contact_buffer_write_slot_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected);

/// Const preflight for contact-buffer write-slot dispatch (B4.5 deepen pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool can_write() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

/// Populate write-slot preflight without mutating the buffer (B4.5 deepen pass).
ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when write-slot should be skipped (B4.5 deepen pass).
bool can_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when write-slot may proceed (B4.5 deepen pass).
bool should_run_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer compaction would early-out (B4.5 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for contact-buffer compaction reject reasons (B4.5 deepen pass).
const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.5 deepen pass).
ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.5 deepen pass).
bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

/// Const preflight for contact-buffer compaction dispatch (B4.5 deepen pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needs_compaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

/// Populate compaction preflight without mutating the buffer (B4.5 deepen pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Returns true when compaction should be skipped (B4.5 deepen pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Returns true when compaction may proceed (B4.5 deepen pass).
bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

/// Human-readable label for contact-buffer clamp reject reasons (B4.5 deepen pass).
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.5 deepen pass).
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.5 deepen pass).
bool contact_buffer_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected);

/// Const preflight for contact-buffer max-capacity clamp dispatch (B4.5 deepen pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needs_clamp() const { return reason == ContactBufferClampRejectReason::None; }
};

/// Populate clamp preflight without mutating the buffer (B4.5 deepen pass).
ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Returns true when max-capacity clamp should be skipped (B4.5 deepen pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Returns true when max-capacity clamp may proceed (B4.5 deepen pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-tangent rebuild would early-out (B4.5 deepen pass).
enum class ContactBufferFrictionTangentRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllOrthonormal,
};

/// Human-readable label for contact-buffer friction-tangent reject reasons (B4.5 deepen pass).
const char* contact_buffer_friction_tangent_reject_reason_name(ContactBufferFrictionTangentRejectReason reason);

/// Diagnose why friction-tangent rebuild would skip; vacuously succeeds when rebuild may proceed (B4.5 deepen pass).
ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Returns true when `contact_buffer_friction_tangent_reject_reason` matches `expected` (B4.5 deepen pass).
bool contact_buffer_friction_tangent_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionTangentRejectReason expected,
    f32 epsilon = 1e-4f);

/// Const preflight for contact-buffer friction-tangent rebuild dispatch (B4.5 deepen pass).
struct ContactBufferFrictionTangentPreflight {
    ContactBufferFrictionTangentRejectReason reason = ContactBufferFrictionTangentRejectReason::None;
    bool emptyBuffer = false;
    bool allOrthonormal = false;

    bool needs_rebuild() const { return reason == ContactBufferFrictionTangentRejectReason::None; }
};

/// Populate friction-tangent preflight without mutating the buffer (B4.5 deepen pass).
ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Returns true when friction-tangent rebuild should be skipped (B4.5 deepen pass).
bool can_skip_contact_buffer_friction_tangent(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

/// Returns true when friction-tangent rebuild may proceed (B4.5 deepen pass).
bool should_run_contact_buffer_friction_tangent(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for contact-buffer compact-and-clamp reject reasons (B4.5 deepen pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.5 deepen pass).
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.5 deepen pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected);

/// Const preflight for contact-buffer compact-and-clamp dispatch (B4.5 deepen pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needs_compact_and_clamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }
};

/// Populate compact-and-clamp preflight without mutating the buffer (B4.5 deepen pass).
ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
    const ContactBufferSoA& buffer);

/// Returns true when compact-and-clamp should be skipped (B4.5 deepen pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Returns true when compact-and-clamp may proceed (B4.5 deepen pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer warm-start apply would reject (B4.5 deepen pass).
enum class ContactBufferWarmStartRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidSlot,
};

/// Human-readable label for contact-buffer warm-start reject reasons (B4.5 deepen pass).
const char* contact_buffer_warm_start_reject_reason_name(ContactBufferWarmStartRejectReason reason);

/// Diagnose why warm-start apply would reject; vacuously succeeds when apply may proceed (B4.5 deepen pass).
ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot);

/// Returns true when `contact_buffer_warm_start_reject_reason` matches `expected` (B4.5 deepen pass).
bool contact_buffer_warm_start_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactBufferWarmStartRejectReason expected);

/// Const preflight for contact-buffer warm-start apply dispatch (B4.5 deepen pass).
struct ContactBufferWarmStartPreflight {
    ContactBufferWarmStartRejectReason reason = ContactBufferWarmStartRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidSlot = false;

    bool can_apply() const { return reason == ContactBufferWarmStartRejectReason::None; }
};

/// Populate warm-start preflight without mutating the manifold (B4.5 deepen pass).
ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    const ContactBufferSoA& buffer,
    u32 slot);

/// Returns true when warm-start apply should be skipped (B4.5 deepen pass).
bool can_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

/// Returns true when warm-start apply may proceed (B4.5 deepen pass).
bool should_run_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

/// Why contact-buffer toVector would early-out (B4.5 deepen pass).
enum class ContactBufferToVectorRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

/// Human-readable label for contact-buffer toVector reject reasons (B4.5 deepen pass).
const char* contact_buffer_to_vector_reject_reason_name(ContactBufferToVectorRejectReason reason);

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed (B4.5 deepen pass).
ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_to_vector_reject_reason` matches `expected` (B4.5 deepen pass).
bool contact_buffer_to_vector_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected);

/// Const preflight for contact-buffer toVector dispatch (B4.5 deepen pass).
struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool can_export() const { return reason == ContactBufferToVectorRejectReason::None; }
};

/// Populate toVector preflight without mutating the buffer (B4.5 deepen pass).
ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Returns true when toVector should be skipped (B4.5 deepen pass).
bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Returns true when toVector may proceed (B4.5 deepen pass).
bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer);

// --- Inline guard implementations (B4.5 deepen pass) ---

inline const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason) {
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

inline ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
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

inline bool contact_buffer_write_slot_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected) {
    return contact_buffer_write_slot_reject_reason(buffer, slot, manifold) == expected;
}

inline ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    ContactBufferWriteSlotPreflight preflight{};
    preflight.reason = contact_buffer_write_slot_reject_reason(buffer, slot, manifold);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    preflight.invalidManifold = preflight.reason == ContactBufferWriteSlotRejectReason::InvalidManifold;
    preflight.selfPair = preflight.reason == ContactBufferWriteSlotRejectReason::SelfPair;
    return preflight;
}

inline bool can_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    return !preflight_contact_buffer_write_slot(buffer, slot, manifold).can_write();
}

inline bool should_run_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    return preflight_contact_buffer_write_slot(buffer, slot, manifold).can_write();
}

inline const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason) {
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

inline ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipCompaction()) {
        return ContactBufferCompactionRejectReason::AllValid;
    }
    return ContactBufferCompactionRejectReason::None;
}

inline bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected) {
    return contact_buffer_compaction_reject_reason(buffer) == expected;
}

inline ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    ContactBufferCompactionPreflight preflight{};
    preflight.reason = contact_buffer_compaction_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;
    return preflight;
}

inline bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compaction(buffer).needs_compaction();
}

inline bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compaction(buffer).needs_compaction();
}

inline const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason) {
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

inline ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferClampRejectReason::EmptyBuffer;
    }
    if (!buffer.canApplyMaxCapacityClamp()) {
        return ContactBufferClampRejectReason::WithinCapacity;
    }
    return ContactBufferClampRejectReason::None;
}

inline bool contact_buffer_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected) {
    return contact_buffer_clamp_reject_reason(buffer) == expected;
}

inline ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.reason = contact_buffer_clamp_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;
    return preflight;
}

inline bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_clamp(buffer).needs_clamp();
}

inline bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_clamp(buffer).needs_clamp();
}

inline const char* contact_buffer_friction_tangent_reject_reason_name(
    ContactBufferFrictionTangentRejectReason reason) {
    switch (reason) {
    case ContactBufferFrictionTangentRejectReason::None:
        return "None";
    case ContactBufferFrictionTangentRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferFrictionTangentRejectReason::AllOrthonormal:
        return "AllOrthonormal";
    }
    return "Unknown";
}

inline ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(
    const ContactBufferSoA& buffer,
    f32 epsilon) {
    if (buffer.canSkipSoAIteration() || buffer.activeCount == 0u) {
        return ContactBufferFrictionTangentRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipFrictionTangentRebuild(epsilon)) {
        return ContactBufferFrictionTangentRejectReason::AllOrthonormal;
    }
    return ContactBufferFrictionTangentRejectReason::None;
}

inline bool contact_buffer_friction_tangent_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionTangentRejectReason expected,
    f32 epsilon) {
    return contact_buffer_friction_tangent_reject_reason(buffer, epsilon) == expected;
}

inline ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent(
    const ContactBufferSoA& buffer,
    f32 epsilon) {
    ContactBufferFrictionTangentPreflight preflight{};
    preflight.reason = contact_buffer_friction_tangent_reject_reason(buffer, epsilon);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionTangentRejectReason::EmptyBuffer;
    preflight.allOrthonormal = preflight.reason == ContactBufferFrictionTangentRejectReason::AllOrthonormal;
    return preflight;
}

inline bool can_skip_contact_buffer_friction_tangent(const ContactBufferSoA& buffer, f32 epsilon) {
    return !preflight_contact_buffer_friction_tangent(buffer, epsilon).needs_rebuild();
}

inline bool should_run_contact_buffer_friction_tangent(const ContactBufferSoA& buffer, f32 epsilon) {
    return preflight_contact_buffer_friction_tangent(buffer, epsilon).needs_rebuild();
}

inline const char* contact_buffer_compact_and_clamp_reject_reason_name(
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

inline ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipCompactAndClamp()) {
        return ContactBufferCompactAndClampRejectReason::NoWork;
    }
    return ContactBufferCompactAndClampRejectReason::None;
}

inline bool contact_buffer_compact_and_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected) {
    return contact_buffer_compact_and_clamp_reject_reason(buffer) == expected;
}

inline ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
    const ContactBufferSoA& buffer) {
    ContactBufferCompactAndClampPreflight preflight{};
    preflight.reason = contact_buffer_compact_and_clamp_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork;
    return preflight;
}

inline bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compact_and_clamp(buffer).needs_compact_and_clamp();
}

inline bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compact_and_clamp(buffer).needs_compact_and_clamp();
}

inline const char* contact_buffer_warm_start_reject_reason_name(ContactBufferWarmStartRejectReason reason) {
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

inline ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
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

inline bool contact_buffer_warm_start_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactBufferWarmStartRejectReason expected) {
    return contact_buffer_warm_start_reject_reason(buffer, slot) == expected;
}

inline ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    const ContactBufferSoA& buffer,
    u32 slot) {
    ContactBufferWarmStartPreflight preflight{};
    preflight.reason = contact_buffer_warm_start_reject_reason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWarmStartRejectReason::OutOfRangeSlot;
    preflight.invalidSlot = preflight.reason == ContactBufferWarmStartRejectReason::InvalidSlot;
    return preflight;
}

inline bool can_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot) {
    return !preflight_contact_buffer_warm_start(buffer, slot).can_apply();
}

inline bool should_run_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot) {
    return preflight_contact_buffer_warm_start(buffer, slot).can_apply();
}

inline const char* contact_buffer_to_vector_reject_reason_name(ContactBufferToVectorRejectReason reason) {
    switch (reason) {
    case ContactBufferToVectorRejectReason::None:
        return "None";
    case ContactBufferToVectorRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    }
    return "Unknown";
}

inline ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration() || !buffer.hasValidContacts()) {
        return ContactBufferToVectorRejectReason::EmptyBuffer;
    }
    return ContactBufferToVectorRejectReason::None;
}

inline bool contact_buffer_to_vector_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected) {
    return contact_buffer_to_vector_reject_reason(buffer) == expected;
}

inline ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    ContactBufferToVectorPreflight preflight{};
    preflight.reason = contact_buffer_to_vector_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferToVectorRejectReason::EmptyBuffer;
    return preflight;
}

inline bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_to_vector(buffer).can_export();
}

inline bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_to_vector(buffer).can_export();
}

} // namespace fuse::physics::narrowphase
