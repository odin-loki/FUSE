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
    /// True when clamping dropped one or more finalized contacts.
    bool hasDroppedContacts() const { return droppedCount > 0u; }
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.5 deepen pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when compact+clamp would leave the buffer unchanged.
    bool canSkipCompactAndClamp() const;
    /// True when no valid contacts need tangent rebuild (B4.5 deepen pass).
    bool canSkipFrictionTangentBuild() const;
    /// True when `maxCapacity` is set and no additional contacts may be retained.
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
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
    /// Write only when preflight allows; no-op otherwise (B4.5 deepen pass).
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    void applyWarmStartStub(u32 slot, ContactManifold& manifold) const;
    void buildFrictionTangentBases();
    /// Rebuild tangents only when preflight allows (B4.5 deepen pass).
    void buildFrictionTangentBasesIfNeeded();
    u32 compact();
    /// Compact only when preflight allows (B4.5 deepen pass).
    u32 compactIfNeeded();
    u32 applyMaxCapacityClamp();
    /// Clamp only when preflight allows (B4.5 deepen pass).
    u32 applyMaxCapacityClampIfNeeded();
    u32 compactAndClamp();
    /// Compact and clamp only when preflight allows (B4.5 deepen pass).
    u32 compactAndClampIfNeeded();
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer slot write would reject (B4.5 deepen pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
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

/// Read-only write-slot diagnostics — no mutation (B4.5 deepen pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;

    bool can_write() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot skip predicate — inverse of `can_write` (B4.5 deepen pass).
bool can_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot predicate — mirrors `preflight_contact_buffer_write_slot` (B4.5 deepen pass).
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

const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needs_compaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

bool contact_buffer_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected);

struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needs_clamp() const { return reason == ContactBufferClampRejectReason::None; }
};

ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

bool contact_buffer_compact_and_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected);

struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needs_compact_and_clamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }
};

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
    const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-basis rebuild would early-out (B4.5 deepen pass).
enum class ContactBufferFrictionBasisRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoValidContacts,
};

const char* contact_buffer_friction_basis_reject_reason_name(ContactBufferFrictionBasisRejectReason reason);

ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(
    const ContactBufferSoA& buffer);

bool contact_buffer_friction_basis_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasisRejectReason expected);

struct ContactBufferFrictionBasisPreflight {
    ContactBufferFrictionBasisRejectReason reason = ContactBufferFrictionBasisRejectReason::None;
    bool emptyBuffer = false;
    bool noValidContacts = false;

    bool needs_friction_basis_build() const {
        return reason == ContactBufferFrictionBasisRejectReason::None;
    }
};

ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_basis(
    const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_friction_basis_build(const ContactBufferSoA& buffer);

bool should_run_contact_buffer_friction_basis_build(const ContactBufferSoA& buffer);

/// Why contact-buffer toVector would early-out (B4.5 deepen pass).
enum class ContactBufferToVectorRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

const char* contact_buffer_to_vector_reject_reason_name(ContactBufferToVectorRejectReason reason);

ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(const ContactBufferSoA& buffer);

bool contact_buffer_to_vector_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected);

struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool can_export() const { return reason == ContactBufferToVectorRejectReason::None; }
};

ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer);

bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer);

inline bool ContactBufferSoA::slotIsValid(u32 slot) const {
    if (slot >= validFlags.size()) {
        return false;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return false;
    }
    return validFlags[slot] != 0u;
}

inline u32 ContactBufferSoA::countValidSlots() const {
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

inline bool ContactBufferSoA::canSkipCompaction() const {
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

inline u32 ContactBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    }
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
}

inline bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
}

inline bool ContactBufferSoA::canSkipCompactAndClamp() const {
    if (canSkipSoAIteration()) {
        return true;
    }
    if (countValidSlots() == 0u) {
        return true;
    }
    return canSkipCompaction() && canSkipMaxCapacityClamp();
}

inline bool ContactBufferSoA::canSkipFrictionTangentBuild() const {
    if (canSkipSoAIteration()) {
        return true;
    }
    return countValidSlots() == 0u;
}

inline bool ContactBufferSoA::writeSlotWithPreflight(u32 slot, const ContactManifold& manifold) {
    if (!preflight_contact_buffer_write_slot(*this, slot, manifold).can_write()) {
        return false;
    }
    writeSlot(slot, manifold);
    return true;
}

inline void ContactBufferSoA::buildFrictionTangentBasesIfNeeded() {
    if (can_skip_contact_buffer_friction_basis_build(*this)) {
        return;
    }
    buildFrictionTangentBases();
}

inline u32 ContactBufferSoA::compactIfNeeded() {
    if (can_skip_contact_buffer_compaction(*this)) {
        return activeCount;
    }
    return compact();
}

inline u32 ContactBufferSoA::applyMaxCapacityClampIfNeeded() {
    if (can_skip_contact_buffer_clamp(*this)) {
        return activeCount;
    }
    return applyMaxCapacityClamp();
}

inline u32 ContactBufferSoA::compactAndClampIfNeeded() {
    if (can_skip_contact_buffer_compact_and_clamp(*this)) {
        return activeCount;
    }
    return compactAndClamp();
}

inline const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason) {
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

inline ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
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
    if (buffer.countValidSlots() == 0u) {
        return ContactBufferCompactAndClampRejectReason::NoWork;
    }
    if (buffer.canSkipCompaction() && buffer.canSkipMaxCapacityClamp()) {
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

inline const char* contact_buffer_friction_basis_reject_reason_name(
    ContactBufferFrictionBasisRejectReason reason) {
    switch (reason) {
    case ContactBufferFrictionBasisRejectReason::None:
        return "None";
    case ContactBufferFrictionBasisRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferFrictionBasisRejectReason::NoValidContacts:
        return "NoValidContacts";
    }
    return "Unknown";
}

inline ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferFrictionBasisRejectReason::EmptyBuffer;
    }
    if (buffer.countValidSlots() == 0u) {
        return ContactBufferFrictionBasisRejectReason::NoValidContacts;
    }
    return ContactBufferFrictionBasisRejectReason::None;
}

inline bool contact_buffer_friction_basis_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasisRejectReason expected) {
    return contact_buffer_friction_basis_reject_reason(buffer) == expected;
}

inline ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_basis(
    const ContactBufferSoA& buffer) {
    ContactBufferFrictionBasisPreflight preflight{};
    preflight.reason = contact_buffer_friction_basis_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBasisRejectReason::EmptyBuffer;
    preflight.noValidContacts = preflight.reason == ContactBufferFrictionBasisRejectReason::NoValidContacts;
    return preflight;
}

inline bool can_skip_contact_buffer_friction_basis_build(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_friction_basis(buffer).needs_friction_basis_build();
}

inline bool should_run_contact_buffer_friction_basis_build(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_friction_basis(buffer).needs_friction_basis_build();
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

inline ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
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
