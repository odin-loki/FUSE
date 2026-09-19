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

    /// Write only when `preflight_contact_buffer_write` allows (B4.4 deepen pass follow-up).
    bool writeSlotIfValid(u32 slot, const ContactManifold& manifold);

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Const preflight for contact-buffer slot write (B4.4 deepen pass follow-up).
struct ContactBufferWritePreflight {
    bool skipped = false;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool can_write() const { return !skipped && !outOfRangeSlot && !invalidManifold && !selfPair; }
};

/// Populate write preflight without mutating buffer slots (B4.4 deepen pass follow-up).
ContactBufferWritePreflight preflight_contact_buffer_write(
    u32 slot,
    const ContactBufferSoA& buffer,
    const ContactManifold& manifold);

/// Returns true when `writeSlot` / `writeSlotIfValid` would reject (B4.4 deepen pass follow-up).
bool should_skip_contact_buffer_write(
    u32 slot,
    const ContactBufferSoA& buffer,
    const ContactManifold& manifold);

} // namespace fuse::physics::narrowphase
