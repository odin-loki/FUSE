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

    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    bool slotIsValid(u32 slot) const;
    u32 countValidSlots() const;
    bool canSkipCompaction() const;
    bool canApplyMaxCapacityClamp() const;
    bool canSkipFrictionTangentBuild(f32 epsilon = 1e-4f) const;

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

/// Why contact-buffer slot write would early-out (B4.6 deepen pass).
enum class ContactBufferWriteRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write reject reasons (B4.6 deepen pass).
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);

/// Diagnose why slot write would skip; vacuously succeeds when write may proceed (B4.6 deepen pass).
ContactBufferWriteRejectReason contact_buffer_write_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_write_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteRejectReason expected);

/// Read-only slot-write diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool canWrite() const { return reason == ContactBufferWriteRejectReason::None; }
};

/// Populate write preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferWritePreflight preflight_contact_buffer_write(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Write only when preflight passes; no-op otherwise (B4.6 deepen pass).
void write_contact_buffer_slot_with_preflight(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
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
    bool allValid = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

/// Populate compaction preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.6 deepen pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.6 deepen pass).
bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

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

/// Populate clamp preflight without mutating the buffer (B4.6 deepen pass).
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

/// Human-readable label for compact-and-clamp reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(
    ContactBufferCompactAndClampRejectReason reason);

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

/// Populate compact-and-clamp preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
    const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.6 deepen pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors preflight (B4.6 deepen pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-tangent rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionTangentRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for friction-tangent reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_tangent_reject_reason_name(
    ContactBufferFrictionTangentRejectReason reason);

/// Diagnose why friction-tangent rebuild would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Returns true when `contact_buffer_friction_tangent_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_tangent_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionTangentRejectReason expected,
    f32 epsilon = 1e-4f);

/// Read-only friction-tangent diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionTangentPreflight {
    ContactBufferFrictionTangentRejectReason reason = ContactBufferFrictionTangentRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsFrictionTangentBuild() const {
        return reason == ContactBufferFrictionTangentRejectReason::None;
    }
};

/// Populate friction-tangent preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangents(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Non-mutating friction-tangent skip predicate (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_tangent_build(
    const ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

/// Compact only when preflight allows; returns active count (B4.6 deepen pass).
u32 compact_contact_buffer_with_preflight(ContactBufferSoA& buffer);

/// Clamp only when preflight allows; returns active count (B4.6 deepen pass).
u32 apply_contact_buffer_max_capacity_clamp_with_preflight(ContactBufferSoA& buffer);

/// Compact-and-clamp only when preflight allows; returns active count (B4.6 deepen pass).
u32 compact_and_clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer);

/// Build friction tangents only when preflight allows (B4.6 deepen pass).
void build_contact_buffer_friction_tangents_with_preflight(
    ContactBufferSoA& buffer,
    f32 epsilon = 1e-4f);

} // namespace fuse::physics::narrowphase
