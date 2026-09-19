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

    /// Returns true when every active slot has a valid orthonormal friction basis (B4.6 deepen pass).
    bool canSkipFrictionRebuild(f32 epsilon = 1e-4f) const;

    /// Rebuild tangent SoA columns only for slots missing or stale bases (B4.6 deepen pass).
    void rebuildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Const preflight for buffer friction-basis rebuild (B4.6 deepen pass).
struct ContactBufferFrictionPreflight {
    u32 activeCount = 0u;
    u32 needsRebuildCount = 0u;
    bool skipped = false;

    bool can_skip_rebuild() const { return skipped || needsRebuildCount == 0u; }
};

/// Populate buffer friction preflight without mutating tangent columns (B4.6 deepen pass).
ContactBufferFrictionPreflight preflight_buffer_friction_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Returns true when buffer friction rebuild should be skipped (B4.6 deepen pass).
bool should_skip_buffer_friction_rebuild(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

} // namespace fuse::physics::narrowphase
