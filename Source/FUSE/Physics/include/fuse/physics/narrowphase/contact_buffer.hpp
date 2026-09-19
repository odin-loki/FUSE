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
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.4 deepen follow-up pass).
    bool canSkipCompaction() const;
    /// True when post-pass truncation would drop contacts (B4.4 deepen follow-up pass).
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.4 deepen follow-up pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// Rebuild friction tangents only for slots with missing or stale bases (B4.4 deepen follow-up pass).
    void buildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Read-only write-slot diagnostics — no mutation (B4.4 deepen follow-up pass).
struct ContactBufferWritePreflight {
    bool invalidManifold = false;
    bool selfPair = false;

    bool canWrite() const { return !invalidManifold && !selfPair; }
};

ContactBufferWritePreflight preflight_contact_buffer_write(const ContactManifold& manifold);

/// Read-only compaction diagnostics — no mutation (B4.4 deepen follow-up pass).
struct ContactBufferCompactionPreflight {
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsCompaction() const { return !emptyBuffer && !allValid; }
};

ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Read-only max-capacity clamp diagnostics — no mutation (B4.4 deepen follow-up pass).
struct ContactBufferClampPreflight {
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needsClamp() const { return !emptyBuffer && !withinCapacity; }
};

ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Read-only friction-basis rebuild diagnostics at the SoA layer (B4.4 deepen follow-up pass).
struct ContactBufferFrictionPreflight {
    bool emptyBuffer = false;
    u32 staleSlotCount = 0u;
    u32 rebuildSlotCount = 0u;

    bool can_skip_rebuild() const { return emptyBuffer || rebuildSlotCount == 0u; }
};

ContactBufferFrictionPreflight preflight_contact_buffer_friction_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Non-mutating friction rebuild skip predicate (B4.4 deepen follow-up pass).
bool can_skip_build_friction_tangent_bases(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

} // namespace fuse::physics::narrowphase
