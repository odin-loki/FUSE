#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics::narrowphase {

struct ContactBufferSoA;

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    NoWork,
};

/// Human-readable label for contact-buffer compact reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_reject_reason_name(ContactBufferCompactRejectReason reason);

/// Diagnose why contact-buffer compact would skip; vacuously succeeds when compact may proceed (B4.6 deepen pass).
ContactBufferCompactRejectReason contact_buffer_compact_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactRejectReason expected);

/// Read-only compact diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactPreflight {
    ContactBufferCompactRejectReason reason = ContactBufferCompactRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompaction() const { return reason == ContactBufferCompactRejectReason::None; }
};

/// Populate compact preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Non-mutating compact skip predicate — inverse of `needsCompaction` (B4.6 deepen pass).
bool can_skip_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Non-mutating compact predicate — mirrors `preflight_contact_buffer_compact` (B4.6 deepen pass).
bool should_run_contact_buffer_compact(const ContactBufferSoA& buffer);

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

    bool needsCompactAndClamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }
};

/// Populate compact-and-clamp preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
    const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate (B4.6 deepen pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate (B4.6 deepen pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

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

} // namespace fuse::physics::narrowphase
