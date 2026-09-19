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
    void buildFrictionTangentBasesIfNeeded();
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    u32 compact();
    u32 applyMaxCapacityClamp();
    u32 compactAndClamp();
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

    bool canSkipBuildFrictionTangentBases() const;
    bool shouldRunBuildFrictionTangentBases() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer slot write would early-out (B4.6 deepen pass).
enum class ContactBufferWriteRejectReason : u8 {
    None = 0,
    InvalidSlot,
    SelfPair,
    InvalidManifold,
};

/// Human-readable label for contact-buffer write reject reasons (B4.6 deepen pass).
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);

/// Diagnose why slot write would skip; vacuously succeeds when write may proceed (B4.6 deepen pass).
ContactBufferWriteRejectReason contact_buffer_write_reject_reason(
    u32 slot,
    const ContactManifold& manifold,
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_write_rejects_for_reason(
    u32 slot,
    const ContactManifold& manifold,
    const ContactBufferSoA& buffer,
    ContactBufferWriteRejectReason expected);

/// Read-only slot-write diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool rejected = false;

    bool can_write() const { return !rejected; }
};

/// Populate slot-write preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferWritePreflight preflight_contact_buffer_write(
    u32 slot,
    const ContactManifold& manifold,
    const ContactBufferSoA& buffer);

/// Why friction-basis SoA rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionBuildRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllBasesValid,
};

/// Human-readable label for friction-basis SoA rebuild reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_build_reject_reason_name(ContactBufferFrictionBuildRejectReason reason);

/// Diagnose why friction-basis SoA rebuild would skip (B4.6 deepen pass).
ContactBufferFrictionBuildRejectReason contact_buffer_friction_build_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_friction_build_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_build_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBuildRejectReason expected);

/// Read-only friction-basis SoA rebuild diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionBuildPreflight {
    ContactBufferFrictionBuildRejectReason reason = ContactBufferFrictionBuildRejectReason::None;
    bool emptyBuffer = false;
    bool allBasesValid = false;

    bool needs_build() const { return reason == ContactBufferFrictionBuildRejectReason::None; }
};

/// Populate friction-basis SoA rebuild preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionBuildPreflight preflight_contact_buffer_friction_build(const ContactBufferSoA& buffer);

/// Non-mutating skip predicate — inverse of `needs_build` (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_build(const ContactBufferSoA& buffer);

/// Non-mutating rebuild predicate — mirrors `preflight_contact_buffer_friction_build` (B4.6 deepen pass).
bool should_run_contact_buffer_friction_build(const ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase
