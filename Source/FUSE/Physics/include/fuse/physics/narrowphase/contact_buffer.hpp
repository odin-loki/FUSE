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

    bool canSkipSoAIteration() const { return pairSlotCount == 0u; }
    bool canSkipCompaction() const;
    bool canApplyMaxCapacityClamp() const;
    u32 countValidSlots() const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for contact-buffer compact reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_reject_reason_name(ContactBufferCompactRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen pass).
ContactBufferCompactRejectReason contact_buffer_compact_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactPreflight {
    ContactBufferCompactRejectReason reason = ContactBufferCompactRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsCompaction() const { return reason == ContactBufferCompactRejectReason::None; }
};

ContactBufferCompactPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.6 deepen pass).
bool can_skip_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compact` (B4.6 deepen pass).
bool should_run_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,
};

/// Human-readable label for contact-buffer clamp reject reasons (B4.6 deepen pass).
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.6 deepen pass).
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected);

/// Read-only max-capacity clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool emptyBuffer = false;
    bool withinCapacity = false;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }
};

ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.6 deepen pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.6 deepen pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for contact-buffer compact-and-clamp reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen pass).
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }
};

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.6 deepen pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.6 deepen pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-basis build would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionBuildRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoValidSlots,
};

/// Human-readable label for contact-buffer friction-build reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_build_reject_reason_name(ContactBufferFrictionBuildRejectReason reason);

/// Diagnose why friction-basis build would skip; vacuously succeeds when build may proceed (B4.6 deepen pass).
ContactBufferFrictionBuildRejectReason contact_buffer_friction_build_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_friction_build_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_build_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBuildRejectReason expected);

/// Read-only friction-basis build diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionBuildPreflight {
    ContactBufferFrictionBuildRejectReason reason = ContactBufferFrictionBuildRejectReason::None;
    bool emptyBuffer = false;
    bool noValidSlots = false;

    bool needsFrictionBuild() const { return reason == ContactBufferFrictionBuildRejectReason::None; }
};

ContactBufferFrictionBuildPreflight preflight_contact_buffer_friction_build(const ContactBufferSoA& buffer);

/// Non-mutating friction-build skip predicate — inverse of `needsFrictionBuild` (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_build(const ContactBufferSoA& buffer);

/// Non-mutating friction-build predicate — mirrors `preflight_contact_buffer_friction_build` (B4.6 deepen pass).
bool should_run_contact_buffer_friction_build(const ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase
