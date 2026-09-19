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
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;

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

/// Const preflight for contact-buffer slot write (B4.6 deepen pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool can_write() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

/// Populate write-slot preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when writeSlot should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when writeSlot may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot skip predicate with optional reject-reason output (B4.6 deepen pass).
bool would_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason* reason = nullptr);

/// Guarded writeSlot — returns false when preflight rejects (B4.6 deepen pass).
bool try_write_contact_buffer_slot(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
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
bool would_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen pass).
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
bool would_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
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
bool would_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Guarded compactAndClamp — returns active count or 0 when preflight skips (B4.6 deepen pass).
u32 try_compact_and_clamp_contact_buffer(ContactBufferSoA& buffer);

/// Why contact-buffer toVector would early-out (B4.6 deepen pass).
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
bool would_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Why contact-buffer warm-start stub would early-out (B4.6 deepen pass).
enum class ContactBufferWarmStartRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidSlot,
};

const char* contact_buffer_warm_start_reject_reason_name(ContactBufferWarmStartRejectReason reason);
ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot);
bool contact_buffer_warm_start_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactBufferWarmStartRejectReason expected);

struct ContactBufferWarmStartPreflight {
    ContactBufferWarmStartRejectReason reason = ContactBufferWarmStartRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidSlot = false;

    bool can_apply() const { return reason == ContactBufferWarmStartRejectReason::None; }
};

ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    const ContactBufferSoA& buffer,
    u32 slot);
bool can_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);
bool should_run_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);
bool would_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);
bool try_apply_contact_buffer_warm_start_stub(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactManifold& manifold);

/// Why contact-buffer friction-basis rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionBasesRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoValidSlots,
};

const char* contact_buffer_friction_bases_reject_reason_name(ContactBufferFrictionBasesRejectReason reason);
ContactBufferFrictionBasesRejectReason contact_buffer_friction_bases_reject_reason(
    const ContactBufferSoA& buffer);
bool contact_buffer_friction_bases_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasesRejectReason expected);

struct ContactBufferFrictionBasesPreflight {
    ContactBufferFrictionBasesRejectReason reason = ContactBufferFrictionBasesRejectReason::None;
    bool emptyBuffer = false;
    bool noValidSlots = false;

    bool needs_rebuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }
};

ContactBufferFrictionBasesPreflight preflight_contact_buffer_friction_bases(
    const ContactBufferSoA& buffer);
bool can_skip_contact_buffer_friction_bases(const ContactBufferSoA& buffer);
bool should_run_contact_buffer_friction_bases(const ContactBufferSoA& buffer);
bool would_skip_contact_buffer_friction_bases(const ContactBufferSoA& buffer);
bool try_build_contact_buffer_friction_tangent_bases(ContactBufferSoA& buffer);

// --- inline implementations (B4.6 deepen pass) ---

FUSE_PHYSICS_INLINE u32 ContactBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    }
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
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

FUSE_PHYSICS_INLINE bool would_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason* reason) {
    const ContactBufferWriteSlotRejectReason rejectReason =
        contact_buffer_write_slot_reject_reason(buffer, slot, manifold);
    if (reason != nullptr) {
        *reason = rejectReason;
    }
    return rejectReason != ContactBufferWriteSlotRejectReason::None;
}

FUSE_PHYSICS_INLINE bool try_write_contact_buffer_slot(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (would_skip_contact_buffer_write_slot(buffer, slot, manifold)) {
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

FUSE_PHYSICS_INLINE bool would_skip_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return can_skip_contact_buffer_compaction(buffer);
}

FUSE_PHYSICS_INLINE const char* contact_buffer_clamp_reject_reason_name(
    ContactBufferClampRejectReason reason) {
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

FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflight_contact_buffer_clamp(
    const ContactBufferSoA& buffer) {
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

FUSE_PHYSICS_INLINE bool would_skip_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return can_skip_contact_buffer_clamp(buffer);
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

FUSE_PHYSICS_INLINE bool would_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return can_skip_contact_buffer_compact_and_clamp(buffer);
}

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canSkipCompactAndClamp() const {
    return can_skip_contact_buffer_compact_and_clamp(*this);
}

FUSE_PHYSICS_INLINE u32 try_compact_and_clamp_contact_buffer(ContactBufferSoA& buffer) {
    if (would_skip_contact_buffer_compact_and_clamp(buffer)) {
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

FUSE_PHYSICS_INLINE bool would_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    return can_skip_contact_buffer_to_vector(buffer);
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

FUSE_PHYSICS_INLINE bool contact_buffer_warm_start_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactBufferWarmStartRejectReason expected) {
    return contact_buffer_warm_start_reject_reason(buffer, slot) == expected;
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

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot) {
    return preflight_contact_buffer_warm_start(buffer, slot).can_apply();
}

FUSE_PHYSICS_INLINE bool would_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot) {
    return can_skip_contact_buffer_warm_start(buffer, slot);
}

FUSE_PHYSICS_INLINE bool try_apply_contact_buffer_warm_start_stub(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactManifold& manifold) {
    if (would_skip_contact_buffer_warm_start(buffer, slot)) {
        return false;
    }
    buffer.applyWarmStartStub(slot, manifold);
    return true;
}

FUSE_PHYSICS_INLINE const char* contact_buffer_friction_bases_reject_reason_name(
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

FUSE_PHYSICS_INLINE ContactBufferFrictionBasesRejectReason contact_buffer_friction_bases_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferFrictionBasesRejectReason::EmptyBuffer;
    }
    if (buffer.countValidSlots() == 0u) {
        return ContactBufferFrictionBasesRejectReason::NoValidSlots;
    }
    return ContactBufferFrictionBasesRejectReason::None;
}

FUSE_PHYSICS_INLINE bool contact_buffer_friction_bases_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasesRejectReason expected) {
    return contact_buffer_friction_bases_reject_reason(buffer) == expected;
}

FUSE_PHYSICS_INLINE ContactBufferFrictionBasesPreflight preflight_contact_buffer_friction_bases(
    const ContactBufferSoA& buffer) {
    ContactBufferFrictionBasesPreflight preflight{};
    preflight.reason = contact_buffer_friction_bases_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBasesRejectReason::EmptyBuffer;
    preflight.noValidSlots = preflight.reason == ContactBufferFrictionBasesRejectReason::NoValidSlots;
    return preflight;
}

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_friction_bases(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_friction_bases(buffer).needs_rebuild();
}

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_friction_bases(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_friction_bases(buffer).needs_rebuild();
}

FUSE_PHYSICS_INLINE bool would_skip_contact_buffer_friction_bases(const ContactBufferSoA& buffer) {
    return can_skip_contact_buffer_friction_bases(buffer);
}

FUSE_PHYSICS_INLINE bool try_build_contact_buffer_friction_tangent_bases(ContactBufferSoA& buffer) {
    if (would_skip_contact_buffer_friction_bases(buffer)) {
        return false;
    }
    buffer.buildFrictionTangentBases();
    return true;
}

} // namespace fuse::physics::narrowphase
