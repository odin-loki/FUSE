#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::narrowphase {

struct ContactBufferSoA;

/// Why contact-buffer slot write would reject (B4.6 deepen follow-up pass).
enum class ContactBufferWriteRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write reject reasons (B4.6 deepen follow-up pass).
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);

/// Diagnose why slot write would skip; vacuously succeeds when write may proceed (B4.6 deepen follow-up pass).
ContactBufferWriteRejectReason contact_buffer_write_reject_reason(
    u32 slot,
    const ContactManifold& manifold,
    u32 pairSlotCount);

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_write_rejects_for_reason(
    u32 slot,
    const ContactManifold& manifold,
    u32 pairSlotCount,
    ContactBufferWriteRejectReason expected);

/// Const preflight for contact-buffer slot write (B4.6 deepen follow-up pass).
struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool canWrite = false;

    bool can_write() const { return canWrite && reason == ContactBufferWriteRejectReason::None; }
};

/// Populate write preflight without mutating buffer slots (B4.6 deepen follow-up pass).
ContactBufferWritePreflight preflight_contact_buffer_write(
    u32 slot,
    const ContactManifold& manifold,
    u32 pairSlotCount);

/// Why contact-buffer compaction would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferCompactRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllInvalid,
};

/// Human-readable label for contact-buffer compact reject reasons (B4.6 deepen follow-up pass).
const char* contact_buffer_compact_reject_reason_name(ContactBufferCompactRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen follow-up pass).
ContactBufferCompactRejectReason contact_buffer_compact_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_compact_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactRejectReason expected);

/// Const preflight for contact-buffer compaction (B4.6 deepen follow-up pass).
struct ContactBufferCompactPreflight {
    ContactBufferCompactRejectReason reason = ContactBufferCompactRejectReason::None;
    bool canCompact = false;

    bool can_compact() const { return canCompact && reason == ContactBufferCompactRejectReason::None; }
};

/// Populate compact preflight without mutating buffer slots (B4.6 deepen follow-up pass).
ContactBufferCompactPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);

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
    /// Write slot only when preflight allows (B4.6 deepen follow-up pass).
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    /// Compact only when preflight allows (B4.6 deepen follow-up pass).
    u32 compactWithPreflight();
    /// Compact and clamp only when preflight allows compaction (B4.6 deepen follow-up pass).
    u32 compactAndClampWithPreflight();
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

} // namespace fuse::physics::narrowphase
