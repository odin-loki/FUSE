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

    u32 countValidSlots() const;

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

/// Why contact-buffer slot write would reject (B4.6 deepen follow-up pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write-slot reject reasons (B4.6 deepen follow-up pass).
const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason);

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed (B4.6 deepen follow-up pass).
ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contact_buffer_write_slot_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_write_slot_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected);

/// Read-only write-slot diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool can_write() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot skip predicate — inverse of `can_write` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Non-mutating write-slot predicate — mirrors `preflight_contact_buffer_write_slot` (B4.6 deepen follow-up pass).
bool should_run_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Why contact-buffer toVector would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferToVectorRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

/// Human-readable label for contact-buffer toVector reject reasons (B4.6 deepen follow-up pass).
const char* contact_buffer_to_vector_reject_reason_name(ContactBufferToVectorRejectReason reason);

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed (B4.6 deepen follow-up pass).
ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_to_vector_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_to_vector_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected);

/// Read-only toVector diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool emptyBuffer = false;

    bool can_export() const { return reason == ContactBufferToVectorRejectReason::None; }
};

ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Non-mutating toVector skip predicate — inverse of `can_export` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Non-mutating toVector predicate — mirrors `preflight_contact_buffer_to_vector` (B4.6 deepen follow-up pass).
bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for compact-and-clamp reject reasons (B4.6 deepen follow-up pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compactAndClamp would skip; vacuously succeeds when work may proceed (B4.6 deepen follow-up pass).
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needs_compact_and_clamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }
};

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
    const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needs_compact_and_clamp` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors preflight (B4.6 deepen follow-up pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer warm-start stub would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferWarmStartRejectReason : u8 {
    None = 0,
    OutOfRangeSlot,
    InvalidSlot,
};

/// Human-readable label for warm-start reject reasons (B4.6 deepen follow-up pass).
const char* contact_buffer_warm_start_reject_reason_name(ContactBufferWarmStartRejectReason reason);

/// Diagnose why applyWarmStartStub would skip; vacuously succeeds when warm-start may proceed (B4.6 deepen follow-up pass).
ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot);

/// Returns true when `contact_buffer_warm_start_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_warm_start_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    ContactBufferWarmStartRejectReason expected);

/// Read-only warm-start diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferWarmStartPreflight {
    ContactBufferWarmStartRejectReason reason = ContactBufferWarmStartRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidSlot = false;

    bool can_apply() const { return reason == ContactBufferWarmStartRejectReason::None; }
};

ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    const ContactBufferSoA& buffer,
    u32 slot);

/// Non-mutating warm-start skip predicate — inverse of `can_apply` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

/// Non-mutating warm-start predicate — mirrors preflight (B4.6 deepen follow-up pass).
bool should_run_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

/// Why contact-buffer friction-basis rebuild would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferBuildFrictionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
};

/// Human-readable label for friction-basis rebuild reject reasons (B4.6 deepen follow-up pass).
const char* contact_buffer_build_friction_reject_reason_name(ContactBufferBuildFrictionRejectReason reason);

/// Diagnose why buildFrictionTangentBases would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen follow-up pass).
ContactBufferBuildFrictionRejectReason contact_buffer_build_friction_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_build_friction_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_build_friction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferBuildFrictionRejectReason expected);

/// Read-only friction-basis rebuild diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferBuildFrictionPreflight {
    ContactBufferBuildFrictionRejectReason reason = ContactBufferBuildFrictionRejectReason::None;
    bool emptyBuffer = false;

    bool can_build() const { return reason == ContactBufferBuildFrictionRejectReason::None; }
};

ContactBufferBuildFrictionPreflight preflight_contact_buffer_build_friction(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis rebuild skip predicate — inverse of `can_build` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_build_friction(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis rebuild predicate — mirrors preflight (B4.6 deepen follow-up pass).
bool should_run_contact_buffer_build_friction(const ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase
