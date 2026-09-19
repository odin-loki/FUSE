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
    bool canSkipCompaction() const;
    bool canSkipMaxCapacityClamp() const;
    bool canSkipCompactAndClamp() const;
    u32 countValidPairSlots() const;
    u32 compact();
    u32 compactIfNeeded();
    u32 applyMaxCapacityClamp();
    u32 compactAndClamp();
    u32 compactAndClampIfNeeded();
    bool canSkipFrictionTangentRebuild(f32 epsilon = 1e-4f) const;
    void buildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    bool rebuildFrictionTangentBasesWithPreflight(f32 epsilon = 1e-4f);
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Const preflight for contact-buffer compaction (B4.6 deepen pass).
struct ContactBufferCompactionPreflight {
    bool skipped = false;
    bool needsCompaction = false;
    bool needsClamp = false;
    u32 validSlotCount = 0u;
    u32 activeCount = 0u;

    bool can_skip_compaction() const { return skipped || !needsCompaction; }
    bool can_skip_compact_and_clamp() const { return skipped || (!needsCompaction && !needsClamp); }
};

/// Populate compaction preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Returns true when compaction would be a no-op (B4.6 deepen pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Const preflight for contact-buffer friction tangent rebuild (B4.6 deepen pass).
struct ContactBufferFrictionPreflight {
    FrictionBasisRejectReason reason = FrictionBasisRejectReason::None;
    bool skipped = false;
    u32 slotsNeedingRebuild = 0u;
    u32 slotsWithStaleBasis = 0u;

    bool can_skip_rebuild() const {
        return skipped || reason != FrictionBasisRejectReason::None || slotsNeedingRebuild == 0u;
    }
};

/// Populate friction rebuild preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionPreflight preflight_contact_buffer_friction_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Returns true when buffer friction rebuild should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

} // namespace fuse::physics::narrowphase
