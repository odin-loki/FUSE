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

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for contact-buffer compaction reject reasons (B4.6 deepen pass).
const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen pass).
ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

/// Populate compaction preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.6 deepen pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.6 deepen pass).
bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-basis rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionBasisRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoValidManifolds,
};

/// Human-readable label for contact-buffer friction-basis reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_basis_reject_reason_name(ContactBufferFrictionBasisRejectReason reason);

/// Diagnose why friction-basis rebuild would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_friction_basis_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_basis_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasisRejectReason expected);

/// Read-only friction-basis rebuild diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionBasisPreflight {
    ContactBufferFrictionBasisRejectReason reason = ContactBufferFrictionBasisRejectReason::None;
    bool emptyBuffer = false;
    bool noValidManifolds = false;

    bool needsRebuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }
};

/// Populate friction-basis preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_basis(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis skip predicate — inverse of `needsRebuild` (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_basis(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis predicate — mirrors `preflight_contact_buffer_friction_basis` (B4.6 deepen pass).
bool should_run_contact_buffer_friction_basis(const ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase

// --- deepen additive from deepen-b4-narrowphase-guards-b463 ---
struct ContactBufferWritePreflight {
ContactBufferWritePreflight preflight_contact_buffer_write(const ContactManifold& manifold);
struct ContactBufferClampPreflight {
ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);
struct ContactBufferFrictionPreflight {
ContactBufferFrictionPreflight preflight_contact_buffer_friction_rebuild(
