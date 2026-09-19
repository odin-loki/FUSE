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
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when at least one valid contact is present after compaction.
    bool hasValidContacts() const { return activeCount > 0u; }
    /// True when slot storage has no invalid flags (compact is a no-op).
    bool canSkipCompaction() const;
    /// True when compact+clamp would leave the buffer unchanged.
    bool canSkipCompactAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    u32 remainingCapacity() const;
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const;
    bool slotIsValid(u32 slot) const;

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const ContactManifold& manifold);
    void invalidateSlot(u32 slot);
    void applyWarmStartStub(u32 slot, ContactManifold& manifold) const;
    void buildFrictionTangentBases();
    void buildFrictionTangentBasesIfNeeded();
    u32 compact();
    u32 compactIfNeeded();
    u32 applyMaxCapacityClamp();
    u32 applyMaxCapacityClampIfNeeded();
    u32 compactAndClamp();
    u32 compactAndClampIfNeeded();
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Const preflight for contact-buffer compaction and clamp (B4.6 deepen pass).
struct ContactBufferPreflight {
    u32 pairSlotCount = 0u;
    u32 validSlotCount = 0u;
    bool canSkipCompaction = true;
    bool canSkipClamp = true;
    bool canSkipCompactAndClamp = true;

    bool can_iterate() const { return pairSlotCount > 0u || validSlotCount > 0u; }
};

/// Populate contact-buffer preflight without mutating slots (B4.6 deepen pass).
ContactBufferPreflight preflight_contact_buffer(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer iteration can be skipped (B4.6 deepen pass).
bool should_skip_contact_buffer_iteration(const ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase
