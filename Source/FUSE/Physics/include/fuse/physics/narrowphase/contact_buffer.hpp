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
    bool hasDroppedContacts() const { return droppedCount > 0u; }
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    u32 remainingCapacity() const;
    bool canAcceptContacts(u32 additionalCount = 1u) const;
    bool canApplyMaxCapacityClamp() const;
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    bool canSkipCompaction() const;
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const ContactManifold& manifold);
    void applyWarmStartStub(u32 slot, ContactManifold& manifold) const;
    void buildFrictionTangentBases();
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

/// Why contact-buffer slot write would reject (B4.5 deepen pass).
enum class ContactBufferWriteRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,
};

const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);

ContactBufferWriteRejectReason contact_buffer_write_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

bool contact_buffer_write_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteRejectReason expected);

struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool can_write() const { return reason == ContactBufferWriteRejectReason::None; }
};

ContactBufferWritePreflight preflight_contact_buffer_write(
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

/// Why contact-buffer friction-basis rebuild would early-out (B4.5 deepen pass).
enum class ContactBufferFrictionRebuildRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoValidContacts,
};

const char* contact_buffer_friction_rebuild_reject_reason_name(ContactBufferFrictionRebuildRejectReason reason);

ContactBufferFrictionRebuildRejectReason contact_buffer_friction_rebuild_reject_reason(
    const ContactBufferSoA& buffer);

bool contact_buffer_friction_rebuild_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionRebuildRejectReason expected);

struct ContactBufferFrictionRebuildPreflight {
    ContactBufferFrictionRebuildRejectReason reason = ContactBufferFrictionRebuildRejectReason::None;
    bool emptyBuffer = false;
    bool noValidContacts = false;

    bool can_rebuild() const { return reason == ContactBufferFrictionRebuildRejectReason::None; }
};

ContactBufferFrictionRebuildPreflight preflight_contact_buffer_friction_rebuild(
    const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer);
bool should_run_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

const char* contact_buffer_compact_and_clamp_reject_reason_name(
    ContactBufferCompactAndClampRejectReason reason);

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

} // namespace fuse::physics::narrowphase
