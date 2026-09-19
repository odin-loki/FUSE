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
    /// True when all active contacts already have orthonormal tangent frames (B4.6 deepen pass).
    bool canSkipFrictionTangentRebuild(f32 epsilon = 1e-4f) const;
    /// Contact-list guard: true when `additionalCount` contacts fit before `maxCapacity` clamp.
    bool canAcceptContacts(u32 additionalCount = 1u) const;
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
    u32 compact();
    u32 applyMaxCapacityClamp();
    u32 compactAndClamp();
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer writeSlot would reject (B4.6 deepen pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
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

/// Read-only write-slot diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;

    bool can_write() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

/// Populate write-slot preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot skip predicate — inverse of `can_write` (B4.6 deepen pass).
bool can_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot predicate — mirrors `preflight_contact_buffer_write_slot` (B4.6 deepen pass).
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

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed (B4.6 deepen pass).
ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needs_compaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

/// Populate compaction preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needs_compaction` (B4.6 deepen pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.6 deepen pass).
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

/// Read-only max-capacity clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needs_clamp() const { return reason == ContactBufferClampRejectReason::None; }
};

/// Populate clamp preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needs_clamp` (B4.6 deepen pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.6 deepen pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for compact-and-clamp reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen pass).
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needs_compact_and_clamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }
};

/// Populate compact-and-clamp preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needs_compact_and_clamp` (B4.6 deepen pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.6 deepen pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-tangent rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionTangentRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for friction-tangent rebuild reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_tangent_reject_reason_name(ContactBufferFrictionTangentRejectReason reason);

/// Diagnose why friction-tangent rebuild would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Returns true when `contact_buffer_friction_tangent_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_tangent_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionTangentRejectReason expected,
    f32 epsilon = 1e-4f);

/// Read-only friction-tangent rebuild diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionTangentPreflight {
    ContactBufferFrictionTangentRejectReason reason = ContactBufferFrictionTangentRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needs_rebuild() const { return reason == ContactBufferFrictionTangentRejectReason::None; }
};

/// Populate friction-tangent rebuild preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Non-mutating friction-tangent rebuild skip predicate (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_tangent_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Non-mutating friction-tangent rebuild predicate (B4.6 deepen pass).
bool should_run_contact_buffer_friction_tangent_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

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

/// Read-only toVector diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool can_export() const { return reason == ContactBufferToVectorRejectReason::None; }
};

/// Populate toVector preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Non-mutating toVector skip predicate — inverse of `can_export` (B4.6 deepen pass).
bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Non-mutating toVector predicate — mirrors `preflight_contact_buffer_to_vector` (B4.6 deepen pass).
bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Write slot only when preflight allows; no-op otherwise (B4.6 deepen pass).
void write_contact_buffer_slot_with_preflight(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Compact only when preflight allows; returns active count (B4.6 deepen pass).
u32 compact_contact_buffer_with_preflight(ContactBufferSoA& buffer);

/// Clamp only when preflight allows; returns active count (B4.6 deepen pass).
u32 clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer);

/// Compact and clamp only when preflight allows; returns active count (B4.6 deepen pass).
u32 compact_and_clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer);

/// Rebuild friction tangents only when preflight allows (B4.6 deepen pass).
void build_contact_buffer_friction_tangents_with_preflight(
    ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

// --- Inline guard implementations (B4.6 deepen pass) ---

FUSE_PHYSICS_INLINE bool ContactBufferSoA::slotIsValid(u32 slot) const {
    return slot < validFlags.size() && validFlags[slot] != 0u;
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

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canSkipCompactAndClamp() const {
    return !should_run_contact_buffer_compaction(*this) && !should_run_contact_buffer_clamp(*this) &&
           countValidSlots() == activeCount;
}

FUSE_PHYSICS_INLINE const char* contact_buffer_write_slot_reject_reason_name(
    ContactBufferWriteSlotRejectReason reason) {
    switch (reason) {
    case ContactBufferWriteSlotRejectReason::None:
        return "None";
    case ContactBufferWriteSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case ContactBufferWriteSlotRejectReason::InvalidManifold:
        return "InvalidManifold";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (slot >= buffer.pairSlotCount) {
        return ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    }
    if (!manifold.valid || manifold.bodyA == manifold.bodyB) {
        return ContactBufferWriteSlotRejectReason::InvalidManifold;
    }
    return ContactBufferWriteSlotRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contact_buffer_write_slot_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected) {
    return contact_buffer_write_slot_reject_reason(buffer, slot, manifold) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    ContactBufferWriteSlotPreflight preflight{};
    preflight.reason = contact_buffer_write_slot_reject_reason(buffer, slot, manifold);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    preflight.invalidManifold = preflight.reason == ContactBufferWriteSlotRejectReason::InvalidManifold;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    return !preflight_contact_buffer_write_slot(buffer, slot, manifold).can_write();
}

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    return preflight_contact_buffer_write_slot(buffer, slot, manifold).can_write();
}

FUSE_PHYSICS_INLINE const char* contact_buffer_compaction_reject_reason_name(
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

FUSE_PHYSICS_INLINE ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipCompaction()) {
        return ContactBufferCompactionRejectReason::AllValid;
    }
    return ContactBufferCompactionRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected) {
    return contact_buffer_compaction_reject_reason(buffer) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferCompactionPreflight preflight_contact_buffer_compaction(
    const ContactBufferSoA& buffer) {
    ContactBufferCompactionPreflight preflight{};
    preflight.reason = contact_buffer_compaction_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compaction(buffer).needs_compaction();
}

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compaction(buffer).needs_compaction();
}

FUSE_PHYSICS_INLINE const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason) {
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

FUSE_PHYSICS_INLINE ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferClampRejectReason::EmptyBuffer;
    }
    if (!buffer.canApplyMaxCapacityClamp()) {
        return ContactBufferClampRejectReason::WithinCapacity;
    }
    return ContactBufferClampRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contact_buffer_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected) {
    return contact_buffer_clamp_reject_reason(buffer) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.reason = contact_buffer_clamp_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_clamp(buffer).needs_clamp();
}

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_clamp(buffer).needs_clamp();
}

FUSE_PHYSICS_INLINE const char* contact_buffer_compact_and_clamp_reject_reason_name(
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

FUSE_PHYSICS_INLINE ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    }
    if (!should_run_contact_buffer_compaction(buffer) && !should_run_contact_buffer_clamp(buffer)) {
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

FUSE_PHYSICS_INLINE bool contact_buffer_compact_and_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected) {
    return contact_buffer_compact_and_clamp_reject_reason(buffer) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
    const ContactBufferSoA& buffer) {
    ContactBufferCompactAndClampPreflight preflight{};
    preflight.reason = contact_buffer_compact_and_clamp_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compact_and_clamp(buffer).needs_compact_and_clamp();
}

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compact_and_clamp(buffer).needs_compact_and_clamp();
}

FUSE_PHYSICS_INLINE const char* contact_buffer_friction_tangent_reject_reason_name(
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

FUSE_PHYSICS_INLINE ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(
    const ContactBufferSoA& buffer,
    f32 epsilon) {
    if (buffer.canSkipSoAIteration() || buffer.activeCount == 0u) {
        return ContactBufferFrictionTangentRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipFrictionTangentRebuild(epsilon)) {
        return ContactBufferFrictionTangentRejectReason::AllValid;
    }
    return ContactBufferFrictionTangentRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contact_buffer_friction_tangent_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionTangentRejectReason expected,
    f32 epsilon) {
    return contact_buffer_friction_tangent_reject_reason(buffer, epsilon) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon) {
    ContactBufferFrictionTangentPreflight preflight{};
    preflight.reason = contact_buffer_friction_tangent_reject_reason(buffer, epsilon);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionTangentRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferFrictionTangentRejectReason::AllValid;
    return preflight;
}

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canSkipFrictionTangentRebuild(f32 epsilon) const {
    if (canSkipSoAIteration() || activeCount == 0u) {
        return true;
    }

    for (u32 slot = 0u; slot < activeCount; ++slot) {
        if (validFlags[slot] == 0u) {
            continue;
        }
        if (!isOrthonormalTangentBasis(contactNormals[slot], {tangent1[slot], tangent2[slot]}, epsilon)) {
            return false;
        }
    }
    return activeCount > 0u;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_friction_tangent_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon) {
    const ContactBufferFrictionTangentRejectReason reason =
        contact_buffer_friction_tangent_reject_reason(buffer, epsilon);
    return reason != ContactBufferFrictionTangentRejectReason::None;
}

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_friction_tangent_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon) {
    return preflight_contact_buffer_friction_tangent_rebuild(buffer, epsilon).needs_rebuild();
}

FUSE_PHYSICS_INLINE const char* contact_buffer_to_vector_reject_reason_name(
    ContactBufferToVectorRejectReason reason) {
    switch (reason) {
    case ContactBufferToVectorRejectReason::None:
        return "None";
    case ContactBufferToVectorRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferToVectorRejectReason::EmptyBuffer;
    }
    return ContactBufferToVectorRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contact_buffer_to_vector_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected) {
    return contact_buffer_to_vector_reject_reason(buffer) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(
    const ContactBufferSoA& buffer) {
    ContactBufferToVectorPreflight preflight{};
    preflight.reason = contact_buffer_to_vector_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferToVectorRejectReason::EmptyBuffer;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_to_vector(buffer).can_export();
}

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_to_vector(buffer).can_export();
}

FUSE_PHYSICS_INLINE void write_contact_buffer_slot_with_preflight(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (!should_run_contact_buffer_write_slot(buffer, slot, manifold)) {
        return;
    }
    buffer.writeSlot(slot, manifold);
}

FUSE_PHYSICS_INLINE u32 compact_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    const ContactBufferCompactionPreflight preflight = preflight_contact_buffer_compaction(buffer);
    if (preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer) {
        buffer.activeCount = 0u;
        buffer.pairSlotCount = 0u;
        return buffer.activeCount;
    }
    if (preflight.reason == ContactBufferCompactionRejectReason::AllValid) {
        buffer.activeCount = buffer.pairSlotCount > 0u ? buffer.pairSlotCount : buffer.activeCount;
        buffer.pairSlotCount = buffer.activeCount;
        return buffer.activeCount;
    }
    return buffer.compact();
}

FUSE_PHYSICS_INLINE u32 clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    if (!should_run_contact_buffer_clamp(buffer)) {
        return buffer.activeCount;
    }
    return buffer.applyMaxCapacityClamp();
}

FUSE_PHYSICS_INLINE u32 compact_and_clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    const ContactBufferCompactAndClampPreflight preflight = preflight_contact_buffer_compact_and_clamp(buffer);
    if (preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer) {
        buffer.activeCount = 0u;
        buffer.pairSlotCount = 0u;
        return buffer.activeCount;
    }
    if (preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork) {
        return buffer.activeCount;
    }
    return buffer.compactAndClamp();
}

FUSE_PHYSICS_INLINE void build_contact_buffer_friction_tangents_with_preflight(
    ContactBufferSoA& buffer,
    f32 epsilon) {
    if (!should_run_contact_buffer_friction_tangent_rebuild(buffer, epsilon)) {
        return;
    }
    buffer.buildFrictionTangentBases();
}

} // namespace fuse::physics::narrowphase
