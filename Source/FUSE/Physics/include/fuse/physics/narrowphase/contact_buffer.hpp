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
    /// True when both dense and slot storage are empty (safe to skip SoA scans) (B4.4 deepen follow-up pass).
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.4 deepen follow-up pass).
    bool canSkipCompaction() const;
    /// True when active count is within `maxCapacity` (clamp is a no-op) (B4.4 deepen follow-up pass).
    bool canSkipMaxCapacityClamp() const;
    /// True when compact+clamp would leave the buffer unchanged (B4.4 deepen follow-up pass).
    bool canSkipCompactAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction (B4.4 deepen follow-up pass).
    u32 countValidSlots() const;

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const ContactManifold& manifold);
    void applyWarmStartStub(u32 slot, ContactManifold& manifold) const;
    void buildFrictionTangentBases();
    /// Rebuild friction tangents only when orthonormal frames are missing (B4.4 deepen follow-up pass).
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

/// Const preflight for contact-buffer compaction dispatch (B4.4 deepen follow-up pass).
struct ContactBufferCompactionPreflight {
    bool skipped = false;
    bool emptySlots = false;
    bool allValid = false;
    bool needsCompaction = false;

    bool can_skip_compaction() const { return skipped || !needsCompaction; }
    bool should_run_compaction() const { return !can_skip_compaction(); }
};

/// Populate compaction preflight without mutating the buffer (B4.4 deepen follow-up pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer compaction should be skipped (B4.4 deepen follow-up pass).
bool should_skip_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Const preflight for contact-buffer max-capacity clamp dispatch (B4.4 deepen follow-up pass).
struct ContactBufferClampPreflight {
    bool skipped = false;
    bool emptyBuffer = false;
    bool withinCapacity = false;
    bool needsClamp = false;

    bool can_skip_clamp() const { return skipped || !needsClamp; }
    bool should_run_clamp() const { return !can_skip_clamp(); }
};

/// Populate clamp preflight without mutating the buffer (B4.4 deepen follow-up pass).
ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer clamp should be skipped (B4.4 deepen follow-up pass).
bool should_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Const preflight for contact-buffer friction-tangent rebuild dispatch (B4.4 deepen follow-up pass).
struct ContactBufferFrictionPreflight {
    bool skipped = false;
    u32 slotCount = 0u;
    u32 needsRebuildCount = 0u;

    bool can_skip_rebuild() const { return skipped || needsRebuildCount == 0u; }
    bool should_run_rebuild() const { return !can_skip_rebuild(); }
};

/// Populate friction-tangent preflight without mutating the buffer (B4.4 deepen follow-up pass).
ContactBufferFrictionPreflight preflight_contact_buffer_friction_tangents(const ContactBufferSoA& buffer);

/// Returns true when friction-tangent SoA rebuild should be skipped (B4.4 deepen follow-up pass).
bool should_skip_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase
