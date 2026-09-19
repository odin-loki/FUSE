#pragma once

#include <fuse/physics/config.hpp>
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
    /// True when clamping dropped one or more contact manifolds.
    bool hasDroppedContacts() const { return droppedCount > 0u; }
    /// True when `maxCapacity` is set and no additional contacts may be retained after clamp.
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const {
        return maxCapacity > 0u && activeCount > maxCapacity;
    }
    /// Inverse of `canApplyMaxCapacityClamp` (B4.6 deepen follow-up pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when every prepared slot is valid (compact is a no-op).
    bool canSkipCompaction() const {
        if (pairSlotCount == 0u) {
            return true;
        }
        for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
            if (validFlags[slot] == 0u) {
                return false;
            }
        }
        return true;
    }
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const {
        u32 validCount = 0u;
        for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
            if (validFlags[slot] != 0u) {
                ++validCount;
            }
        }
        return validCount;
    }
    bool slotIsValid(u32 slot) const {
        return slot < pairSlotCount && validFlags[slot] != 0u;
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

/// Why contact-buffer slot write would reject (B4.6 deepen follow-up pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write-slot reject reasons (logging / tests).
const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason);

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed.
ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contact_buffer_write_slot_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_write_slot_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected);

/// Read-only write-slot diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool can_write() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot skip predicate — inverse of `can_write` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot predicate — mirrors `preflight_contact_buffer_write_slot` (B4.6 deepen follow-up pass).
bool should_run_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Write only when preflight allows; returns false when skipped (B4.6 deepen follow-up pass).
bool write_contact_buffer_slot_with_preflight(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer compaction would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for contact-buffer compaction reject reasons (logging / tests).
const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

/// Diagnose why compact would early-out; vacuously succeeds when compaction may proceed.
ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needs_compaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needs_compaction` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.6 deepen follow-up pass).
bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Compact only when preflight allows; returns active count or zero when skipped (B4.6 deepen follow-up pass).
u32 compact_contact_buffer_with_preflight(ContactBufferSoA& buffer);

/// Why contact-buffer clamp would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    NoCapacityLimit,
    WithinCapacity,
};

/// Human-readable label for contact-buffer clamp reject reasons (logging / tests).
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

/// Diagnose why applyMaxCapacityClamp would early-out; vacuously succeeds when clamp may proceed.
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

/// Read-only clamp diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool noCapacityLimit = false;
    bool withinCapacity = false;

    bool needs_clamp() const { return reason == ContactBufferClampRejectReason::None; }
};

ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needs_clamp` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.6 deepen follow-up pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Clamp only when preflight allows; returns active count or prior count when skipped (B4.6 deepen follow-up pass).
u32 apply_contact_buffer_max_capacity_clamp_with_preflight(ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for contact-buffer compact-and-clamp reject reasons (logging / tests).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compactAndClamp would early-out; vacuously succeeds when pass may proceed.
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needs_compact_and_clamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }
};

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needs_compact_and_clamp` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.6 deepen follow-up pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Compact and clamp only when preflight allows; returns active count or zero when skipped (B4.6 deepen follow-up pass).
u32 compact_and_clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer);

/// Why contact-buffer toVector would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferToVectorRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

/// Human-readable label for contact-buffer toVector reject reasons (logging / tests).
const char* contact_buffer_to_vector_reject_reason_name(ContactBufferToVectorRejectReason reason);

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed.
ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_to_vector_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_to_vector_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected);

/// Read-only toVector diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool can_export() const { return reason == ContactBufferToVectorRejectReason::None; }
};

ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Non-mutating toVector skip predicate — inverse of `can_export` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Non-mutating toVector predicate — mirrors `preflight_contact_buffer_to_vector` (B4.6 deepen follow-up pass).
bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Export only when preflight allows; returns empty vector when skipped (B4.6 deepen follow-up pass).
std::vector<ContactManifold> to_vector_contact_buffer_with_preflight(const ContactBufferSoA& buffer);

/// Why contact-buffer manifoldAt would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferManifoldAtRejectReason : u8 {
    None = 0,
    OutOfRangeIndex,
    InvalidSlot,
};

/// Human-readable label for contact-buffer manifoldAt reject reasons (logging / tests).
const char* contact_buffer_manifold_at_reject_reason_name(ContactBufferManifoldAtRejectReason reason);

/// Diagnose why manifoldAt would return invalid; vacuously succeeds when read may proceed.
ContactBufferManifoldAtRejectReason contact_buffer_manifold_at_reject_reason(
    const ContactBufferSoA& buffer,
    u32 index);

/// Read-only manifoldAt diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferManifoldAtPreflight {
    ContactBufferManifoldAtRejectReason reason = ContactBufferManifoldAtRejectReason::None;
    bool outOfRangeIndex = false;
    bool invalidSlot = false;

    bool can_read() const { return reason == ContactBufferManifoldAtRejectReason::None; }
};

ContactBufferManifoldAtPreflight preflight_contact_buffer_manifold_at(
    const ContactBufferSoA& buffer,
    u32 index);

/// Non-mutating manifoldAt skip predicate — inverse of `can_read` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_manifold_at(const ContactBufferSoA& buffer, u32 index);

/// Why contact-buffer warm-start apply would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferWarmStartRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidSlot,
};

/// Human-readable label for contact-buffer warm-start reject reasons (logging / tests).
const char* contact_buffer_warm_start_reject_reason_name(ContactBufferWarmStartRejectReason reason);

/// Diagnose why applyWarmStartStub would no-op; vacuously succeeds when apply may proceed.
ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot);

/// Read-only warm-start diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferWarmStartPreflight {
    ContactBufferWarmStartRejectReason reason = ContactBufferWarmStartRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidSlot = false;

    bool can_apply() const { return reason == ContactBufferWarmStartRejectReason::None; }
};

ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    const ContactBufferSoA& buffer,
    u32 slot);

/// Non-mutating warm-start skip predicate — inverse of `can_apply` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

/// Apply warm-start only when preflight allows; returns false when skipped (B4.6 deepen follow-up pass).
bool apply_contact_buffer_warm_start_with_preflight(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactManifold& manifold);

/// Why contact-buffer tangent-basis build would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferTangentBasisRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoValidSlots,
};

/// Human-readable label for contact-buffer tangent-basis reject reasons (logging / tests).
const char* contact_buffer_tangent_basis_reject_reason_name(ContactBufferTangentBasisRejectReason reason);

/// Diagnose why buildFrictionTangentBases would no-op; vacuously succeeds when build may proceed.
ContactBufferTangentBasisRejectReason contact_buffer_tangent_basis_reject_reason(const ContactBufferSoA& buffer);

/// Read-only tangent-basis diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferTangentBasisPreflight {
    ContactBufferTangentBasisRejectReason reason = ContactBufferTangentBasisRejectReason::None;
    bool emptyBuffer = false;
    bool noValidSlots = false;

    bool can_build() const { return reason == ContactBufferTangentBasisRejectReason::None; }
};

ContactBufferTangentBasisPreflight preflight_contact_buffer_tangent_basis(const ContactBufferSoA& buffer);

/// Non-mutating tangent-basis skip predicate — inverse of `can_build` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_build_friction_tangent_bases(const ContactBufferSoA& buffer);

/// Build tangent bases only when preflight allows; returns false when skipped (B4.6 deepen follow-up pass).
bool build_contact_buffer_friction_tangent_bases_with_preflight(ContactBufferSoA& buffer);

FUSE_PHYSICS_INLINE const char* contact_buffer_write_slot_reject_reason_name(
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

FUSE_PHYSICS_INLINE ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
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
    preflight.selfPair = preflight.reason == ContactBufferWriteSlotRejectReason::SelfPair;
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

FUSE_PHYSICS_INLINE bool write_contact_buffer_slot_with_preflight(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (!should_run_contact_buffer_write_slot(buffer, slot, manifold)) {
        return false;
    }
    buffer.writeSlot(slot, manifold);
    return true;
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
    if (buffer.countValidSlots() == 0u) {
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

FUSE_PHYSICS_INLINE u32 compact_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    const ContactBufferCompactionPreflight preflight = preflight_contact_buffer_compaction(buffer);
    if (preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer) {
        return buffer.activeCount;
    }
    if (preflight.reason == ContactBufferCompactionRejectReason::AllValid) {
        buffer.activeCount = buffer.countValidSlots();
        return buffer.activeCount;
    }
    return buffer.compact();
}

FUSE_PHYSICS_INLINE const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason) {
    switch (reason) {
    case ContactBufferClampRejectReason::None:
        return "None";
    case ContactBufferClampRejectReason::NoCapacityLimit:
        return "NoCapacityLimit";
    case ContactBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.maxCapacity == 0u) {
        return ContactBufferClampRejectReason::NoCapacityLimit;
    }
    if (buffer.activeCount <= buffer.maxCapacity) {
        return ContactBufferClampRejectReason::WithinCapacity;
    }
    return ContactBufferClampRejectReason::None;
}

FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.reason = contact_buffer_clamp_reject_reason(buffer);
    preflight.noCapacityLimit = preflight.reason == ContactBufferClampRejectReason::NoCapacityLimit;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_clamp(buffer).needs_clamp();
}

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_clamp(buffer).needs_clamp();
}

FUSE_PHYSICS_INLINE u32 apply_contact_buffer_max_capacity_clamp_with_preflight(ContactBufferSoA& buffer) {
    if (!should_run_contact_buffer_clamp(buffer)) {
        return buffer.activeCount;
    }
    return buffer.applyMaxCapacityClamp();
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
    if (buffer.canSkipCompaction() && buffer.canSkipMaxCapacityClamp()) {
        return ContactBufferCompactAndClampRejectReason::NoWork;
    }
    return ContactBufferCompactAndClampRejectReason::None;
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

FUSE_PHYSICS_INLINE u32 compact_and_clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    if (!should_run_contact_buffer_compact_and_clamp(buffer)) {
        return buffer.activeCount;
    }
    return buffer.compactAndClamp();
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

FUSE_PHYSICS_INLINE std::vector<ContactManifold> to_vector_contact_buffer_with_preflight(
    const ContactBufferSoA& buffer) {
    if (!should_run_contact_buffer_to_vector(buffer)) {
        return {};
    }
    return buffer.toVector();
}

FUSE_PHYSICS_INLINE const char* contact_buffer_manifold_at_reject_reason_name(
    ContactBufferManifoldAtRejectReason reason) {
    switch (reason) {
    case ContactBufferManifoldAtRejectReason::None:
        return "None";
    case ContactBufferManifoldAtRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case ContactBufferManifoldAtRejectReason::InvalidSlot:
        return "InvalidSlot";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferManifoldAtRejectReason contact_buffer_manifold_at_reject_reason(
    const ContactBufferSoA& buffer,
    u32 index) {
    if (index >= buffer.activeCount) {
        return ContactBufferManifoldAtRejectReason::OutOfRangeIndex;
    }
    if (buffer.validFlags[index] == 0u) {
        return ContactBufferManifoldAtRejectReason::InvalidSlot;
    }
    return ContactBufferManifoldAtRejectReason::None;
}

FUSE_PHYSICS_INLINE ContactBufferManifoldAtPreflight preflight_contact_buffer_manifold_at(
    const ContactBufferSoA& buffer,
    u32 index) {
    ContactBufferManifoldAtPreflight preflight{};
    preflight.reason = contact_buffer_manifold_at_reject_reason(buffer, index);
    preflight.outOfRangeIndex = preflight.reason == ContactBufferManifoldAtRejectReason::OutOfRangeIndex;
    preflight.invalidSlot = preflight.reason == ContactBufferManifoldAtRejectReason::InvalidSlot;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_manifold_at(const ContactBufferSoA& buffer, u32 index) {
    return !preflight_contact_buffer_manifold_at(buffer, index).can_read();
}

FUSE_PHYSICS_INLINE const char* contact_buffer_warm_start_reject_reason_name(
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

FUSE_PHYSICS_INLINE ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot) {
    if (slot >= buffer.pairSlotCount) {
        return ContactBufferWarmStartRejectReason::OutOfRangeSlot;
    }
    if (buffer.validFlags[slot] == 0u) {
        return ContactBufferWarmStartRejectReason::InvalidSlot;
    }
    return ContactBufferWarmStartRejectReason::None;
}

FUSE_PHYSICS_INLINE ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    const ContactBufferSoA& buffer,
    u32 slot) {
    ContactBufferWarmStartPreflight preflight{};
    preflight.reason = contact_buffer_warm_start_reject_reason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWarmStartRejectReason::OutOfRangeSlot;
    preflight.invalidSlot = preflight.reason == ContactBufferWarmStartRejectReason::InvalidSlot;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot) {
    return !preflight_contact_buffer_warm_start(buffer, slot).can_apply();
}

FUSE_PHYSICS_INLINE bool apply_contact_buffer_warm_start_with_preflight(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactManifold& manifold) {
    if (!preflight_contact_buffer_warm_start(buffer, slot).can_apply()) {
        return false;
    }
    buffer.applyWarmStartStub(slot, manifold);
    return true;
}

FUSE_PHYSICS_INLINE const char* contact_buffer_tangent_basis_reject_reason_name(
    ContactBufferTangentBasisRejectReason reason) {
    switch (reason) {
    case ContactBufferTangentBasisRejectReason::None:
        return "None";
    case ContactBufferTangentBasisRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferTangentBasisRejectReason::NoValidSlots:
        return "NoValidSlots";
    }
    return "Unknown";
}

FUSE_PHYSICS_INLINE ContactBufferTangentBasisRejectReason contact_buffer_tangent_basis_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.activeCount == 0u) {
        return ContactBufferTangentBasisRejectReason::EmptyBuffer;
    }
    for (u32 slot = 0u; slot < buffer.activeCount; ++slot) {
        if (buffer.validFlags[slot] != 0u) {
            return ContactBufferTangentBasisRejectReason::None;
        }
    }
    return ContactBufferTangentBasisRejectReason::NoValidSlots;
}

FUSE_PHYSICS_INLINE ContactBufferTangentBasisPreflight preflight_contact_buffer_tangent_basis(
    const ContactBufferSoA& buffer) {
    ContactBufferTangentBasisPreflight preflight{};
    preflight.reason = contact_buffer_tangent_basis_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferTangentBasisRejectReason::EmptyBuffer;
    preflight.noValidSlots = preflight.reason == ContactBufferTangentBasisRejectReason::NoValidSlots;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_build_friction_tangent_bases(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_tangent_basis(buffer).can_build();
}

FUSE_PHYSICS_INLINE bool build_contact_buffer_friction_tangent_bases_with_preflight(ContactBufferSoA& buffer) {
    if (!preflight_contact_buffer_tangent_basis(buffer).can_build()) {
        return false;
    }
    buffer.buildFrictionTangentBases();
    return true;
}

} // namespace fuse::physics::narrowphase
