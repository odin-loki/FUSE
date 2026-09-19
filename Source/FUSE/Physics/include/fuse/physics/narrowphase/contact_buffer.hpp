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

// --- deepen additive from deepen-b4-narrowphase-guard-pass-9852 ---
enum class ContactBufferWriteRejectReason : u8 {
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);
ContactBufferWriteRejectReason contact_buffer_write_reject_reason(
    ContactBufferWriteRejectReason expected);
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool canWrite() const { return reason == ContactBufferWriteRejectReason::None; }
enum class ContactBufferClampRejectReason : u8 {
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);
    ContactBufferClampRejectReason expected);
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }

// --- deepen additive from b4-narrowphase-deepen-guards-046d ---
const char* contactBufferWriteRejectReasonName(ContactBufferWriteRejectReason reason);
ContactBufferWriteRejectReason contactBufferWriteRejectReason(
ContactBufferWritePreflight preflightContactBufferWrite(
const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason);
ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer);
ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer);
const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason);
ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer);
ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer);

// --- deepen additive from deepen-b4-narrowphase-guards-37c2 ---
enum class ContactBufferCompactRejectReason : u8 {
const char* contactBufferCompactRejectReasonName(ContactBufferCompactRejectReason reason);
ContactBufferCompactRejectReason contactBufferCompactRejectReason(const ContactBufferSoA& buffer);
    ContactBufferCompactRejectReason expected);
struct ContactBufferCompactPreflight {
    ContactBufferCompactRejectReason reason = ContactBufferCompactRejectReason::None;
    bool needsCompaction() const { return reason == ContactBufferCompactRejectReason::None; }
ContactBufferCompactPreflight preflightContactBufferCompact(const ContactBufferSoA& buffer);

// --- deepen additive from deepen-b4-narrowphase-guards-26f2 ---
ContactBufferCompactionPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);
bool should_skip_contact_buffer_compact(const ContactBufferSoA& buffer);
bool should_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);
ContactBufferFrictionPreflight preflight_contact_buffer_friction_tangents(const ContactBufferSoA& buffer);
bool should_skip_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer);

// --- deepen additive from deepen-b4-narrowphase-guards-c6ee ---
bool should_skip_contact_buffer_write(

// --- deepen additive from deepen-b4-narrowphase-guards-7d67 ---
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);

// --- deepen additive from b4-narrowphase-deepen-guards-8a17 ---
const char* contact_buffer_compact_reject_reason_name(ContactBufferCompactRejectReason reason);
ContactBufferCompactRejectReason contact_buffer_compact_reject_reason(const ContactBufferSoA& buffer);
ContactBufferCompactPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);
enum class ContactBufferCompactAndClampRejectReason : u8 {
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    ContactBufferCompactAndClampRejectReason expected);
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }
ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);
enum class ContactBufferFrictionBuildRejectReason : u8 {
const char* contact_buffer_friction_build_reject_reason_name(ContactBufferFrictionBuildRejectReason reason);
ContactBufferFrictionBuildRejectReason contact_buffer_friction_build_reject_reason(const ContactBufferSoA& buffer);
    ContactBufferFrictionBuildRejectReason expected);
struct ContactBufferFrictionBuildPreflight {
    ContactBufferFrictionBuildRejectReason reason = ContactBufferFrictionBuildRejectReason::None;
    bool needsFrictionBuild() const { return reason == ContactBufferFrictionBuildRejectReason::None; }
ContactBufferFrictionBuildPreflight preflight_contact_buffer_friction_build(const ContactBufferSoA& buffer);

// --- deepen additive from b4-narrowphase-deepen-deea ---
const char* contactBufferCompactAndClampRejectReasonName(ContactBufferCompactAndClampRejectReason reason);
ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(
ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

// --- deepen additive from deepen-b4-narrowphase-b46-8196 ---
void writeContactBufferSlotWithPreflight(
u32 compactContactBufferWithPreflight(ContactBufferSoA& buffer);
u32 clampContactBufferWithPreflight(ContactBufferSoA& buffer);
u32 compactAndClampContactBufferWithPreflight(ContactBufferSoA& buffer);

// --- deepen additive from deepen-b4-narrowphase-6c66 ---
const char* contactBufferFrictionBuildRejectReasonName(ContactBufferFrictionBuildRejectReason reason);
ContactBufferFrictionBuildRejectReason contactBufferFrictionBuildRejectReason(const ContactBufferSoA& buffer);
    bool canBuild() const { return reason == ContactBufferFrictionBuildRejectReason::None; }
ContactBufferFrictionBuildPreflight preflightContactBufferFrictionBuild(const ContactBufferSoA& buffer);
void buildContactBufferFrictionTangentBasesWithPreflight(ContactBufferSoA& buffer);

// --- deepen additive from b4-narrowphase-deepen-pass6-949f ---
    bool buildFrictionTangentBasesWithPreflight();
    u32 compactWithPreflight();
    u32 compactAndClampWithPreflight();
    bool can_write() const { return reason == ContactBufferWriteRejectReason::None; }
    bool needs_compaction() const { return reason == ContactBufferCompactionRejectReason::None; }
    bool needs_clamp() const { return reason == ContactBufferClampRejectReason::None; }
    bool can_rebuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }

// --- deepen additive from b4-narrowphase-deepen-e124 ---
enum class ContactBufferFrictionBasesRejectReason : u8 {
const char* contactBufferFrictionBasesRejectReasonName(ContactBufferFrictionBasesRejectReason reason);
ContactBufferFrictionBasesRejectReason contactBufferFrictionBasesRejectReason(const ContactBufferSoA& buffer);
    ContactBufferFrictionBasesRejectReason expected);
struct ContactBufferFrictionBasesPreflight {
    ContactBufferFrictionBasesRejectReason reason = ContactBufferFrictionBasesRejectReason::None;
    bool needsRebuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }
ContactBufferFrictionBasesPreflight preflightContactBufferFrictionBases(const ContactBufferSoA& buffer);
bool buildContactBufferFrictionBasesWithPreflight(ContactBufferSoA& buffer);

// --- deepen additive from deepen-b4-narrowphase-guards-1644 ---
enum class ContactBufferFrictionTangentRejectReason : u8 {
    ContactBufferFrictionTangentRejectReason reason);
ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(
    ContactBufferFrictionTangentRejectReason expected,
struct ContactBufferFrictionTangentPreflight {
    ContactBufferFrictionTangentRejectReason reason = ContactBufferFrictionTangentRejectReason::None;
        return reason == ContactBufferFrictionTangentRejectReason::None;
ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangents(

// --- deepen additive from b4-narrowphase-deepen-d3be ---
enum class ContactBufferWriteSlotRejectReason : u8 {
const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason);
ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
    ContactBufferWriteSlotRejectReason expected);
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
    bool canWrite() const { return reason == ContactBufferWriteSlotRejectReason::None; }
ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
bool should_skip_contact_buffer_write_slot(
const char* contact_buffer_friction_bases_reject_reason_name(ContactBufferFrictionBasesRejectReason reason);
ContactBufferFrictionBasesRejectReason contact_buffer_friction_bases_reject_reason(
    bool needsBuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }
ContactBufferFrictionBasesPreflight preflight_contact_buffer_friction_bases(const ContactBufferSoA& buffer);

// --- deepen additive from deepen-b4-narrowphase-guards-ef6e ---
bool writeContactBufferSlotWithPreflight(

// --- deepen additive from deepen-b4-narrowphase-guards-b135 ---
enum class ContactBufferFrictionRebuildRejectReason : u8 {
const char* contactBufferFrictionRebuildRejectReasonName(ContactBufferFrictionRebuildRejectReason reason);
ContactBufferFrictionRebuildRejectReason contactBufferFrictionRebuildRejectReason(
    ContactBufferFrictionRebuildRejectReason expected,
struct ContactBufferFrictionRebuildPreflight {
    ContactBufferFrictionRebuildRejectReason reason = ContactBufferFrictionRebuildRejectReason::None;
    bool needsRebuild() const { return reason == ContactBufferFrictionRebuildRejectReason::None; }
ContactBufferFrictionRebuildPreflight preflightContactBufferFrictionRebuild(

// --- deepen additive from deepen-b4-narrowphase-guards-e78a ---
bool writeContactSlotWithPreflight(

// --- deepen additive from deepen-b4-narrowphase-guards-f881 ---
    bool writeSlotIfPreflight(u32 slot, const ContactManifold& manifold);

// --- deepen additive from deepen-b4-narrowphase-2570 ---
    bool needs_compact_and_clamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }

// --- deepen additive from b4-narrowphase-deepen-pass-6859 ---
ContactBufferFrictionPreflight preflight_buffer_friction_rebuild(
bool should_skip_buffer_friction_rebuild(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

// --- deepen additive from b4-narrowphase-deepen-guards-01df ---
    bool can_write() const { return canWrite && reason == ContactBufferWriteRejectReason::None; }
    bool can_compact() const { return canCompact && reason == ContactBufferCompactRejectReason::None; }

// --- deepen additive from deepen-b4-narrowphase-guards-5ef6 ---
ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(const ContactBufferSoA& buffer);

// --- deepen additive from b4-narrowphase-deepen-guards-9857 ---
    bool needs_build() const { return reason == ContactBufferFrictionBuildRejectReason::None; }

// --- deepen additive from deepen-b4-narrowphase-guards-fd4e ---
bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold, ContactBufferSoA& buffer);
u32 compactWithPreflight(ContactBufferSoA& buffer);
u32 applyMaxCapacityClampWithPreflight(ContactBufferSoA& buffer);
u32 compactAndClampWithPreflight(ContactBufferSoA& buffer);

// --- deepen additive from b4-narrowphase-guards-91b3 ---
const char* contact_buffer_friction_rebuild_reject_reason_name(ContactBufferFrictionRebuildRejectReason reason);
ContactBufferFrictionRebuildRejectReason contact_buffer_friction_rebuild_reject_reason(
    ContactBufferFrictionRebuildRejectReason expected);
    bool can_rebuild() const { return reason == ContactBufferFrictionRebuildRejectReason::None; }
ContactBufferFrictionRebuildPreflight preflight_contact_buffer_friction_rebuild(

// --- deepen additive from b4-narrowphase-deepen-ffe6 ---
struct ContactBufferPreflight {
ContactBufferPreflight preflight_contact_buffer(const ContactBufferSoA& buffer);
bool should_skip_contact_buffer_iteration(const ContactBufferSoA& buffer);

// --- deepen additive from b4-narrowphase-deepen-guards-69fc ---
const char* contactBufferFrictionBasisRejectReasonName(ContactBufferFrictionBasisRejectReason reason);
ContactBufferFrictionBasisRejectReason contactBufferFrictionBasisRejectReason(const ContactBufferSoA& buffer);
    bool needsFrictionBasisBuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }
ContactBufferFrictionBasisPreflight preflightContactBufferFrictionBasis(const ContactBufferSoA& buffer);
void buildFrictionTangentBasesWithPreflight(ContactBufferSoA& buffer);

// --- deepen additive from b4-narrowphase-buffer-guards-0bb9 ---
const char* contactBufferWriteSlotRejectReasonName(ContactBufferWriteSlotRejectReason reason);
ContactBufferWriteSlotRejectReason contactBufferWriteSlotRejectReason(
ContactBufferWriteSlotPreflight preflightContactBufferWriteSlot(
enum class ContactBufferWarmStartRejectReason : u8 {
const char* contactBufferWarmStartRejectReasonName(ContactBufferWarmStartRejectReason reason);
ContactBufferWarmStartRejectReason contactBufferWarmStartRejectReason(
    ContactBufferWarmStartRejectReason expected);
struct ContactBufferWarmStartPreflight {
    ContactBufferWarmStartRejectReason reason = ContactBufferWarmStartRejectReason::None;
    bool canWarmStart() const { return reason == ContactBufferWarmStartRejectReason::None; }
ContactBufferWarmStartPreflight preflightContactBufferWarmStart(
enum class ContactBufferToVectorRejectReason : u8 {
const char* contactBufferToVectorRejectReasonName(ContactBufferToVectorRejectReason reason);
ContactBufferToVectorRejectReason contactBufferToVectorRejectReason(const ContactBufferSoA& buffer);
    ContactBufferToVectorRejectReason expected);
struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;
    bool canExport() const { return reason == ContactBufferToVectorRejectReason::None; }
ContactBufferToVectorPreflight preflightContactBufferToVector(const ContactBufferSoA& buffer);

// --- deepen additive from deepen-narrowphase-b4-guards-5b57 ---
    bool can_write() const { return reason == ContactBufferWriteSlotRejectReason::None; }
const char* contact_buffer_to_vector_reject_reason_name(ContactBufferToVectorRejectReason reason);
ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(const ContactBufferSoA& buffer);
    bool can_export() const { return reason == ContactBufferToVectorRejectReason::None; }
ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer);

// --- deepen additive from deepen-b4-narrowphase-guards-d1c7 ---
bool contactBufferClampRejectsForReason(const ContactBufferSoA& buffer, ContactBufferClampRejectReason expected);
ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(const ContactBufferSoA& buffer);
    bool canBuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }
void buildContactBufferFrictionBasesWithPreflight(ContactBufferSoA& buffer);

// --- deepen additive from b4-narrowphase-deepen-guards-bee6 ---
const char* contact_buffer_warm_start_reject_reason_name(ContactBufferWarmStartRejectReason reason);
ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
    bool can_apply() const { return reason == ContactBufferWarmStartRejectReason::None; }
ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    bool needs_rebuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }

// --- deepen additive from b4-narrowphase-deepen-c201 ---
FUSE_PHYSICS_INLINE ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
FUSE_PHYSICS_INLINE ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
FUSE_PHYSICS_INLINE ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(
FUSE_PHYSICS_INLINE ContactBufferCompactionPreflight preflight_contact_buffer_compaction(
FUSE_PHYSICS_INLINE const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);
FUSE_PHYSICS_INLINE ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(
FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);
FUSE_PHYSICS_INLINE ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(
    ContactBufferFrictionTangentRejectReason expected);
    bool needs_rebuild() const { return reason == ContactBufferFrictionTangentRejectReason::None; }
FUSE_PHYSICS_INLINE ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent_rebuild(
FUSE_PHYSICS_INLINE ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
FUSE_PHYSICS_INLINE ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
FUSE_PHYSICS_INLINE ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(
FUSE_PHYSICS_INLINE ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(
    ContactBufferWriteSlotRejectReason reason) {
    case ContactBufferWriteSlotRejectReason::None:
    case ContactBufferWriteSlotRejectReason::OutOfRangeSlot:
    case ContactBufferWriteSlotRejectReason::InvalidManifold:
    case ContactBufferWriteSlotRejectReason::SelfPair:
        return ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
        return ContactBufferWriteSlotRejectReason::InvalidManifold;
        return ContactBufferWriteSlotRejectReason::SelfPair;
    return ContactBufferWriteSlotRejectReason::None;
    ContactBufferWriteSlotRejectReason expected) {
    ContactBufferWriteSlotPreflight preflight{};
        preflight.reason == ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
        preflight.reason == ContactBufferWriteSlotRejectReason::InvalidManifold;
    preflight.selfPair = preflight.reason == ContactBufferWriteSlotRejectReason::SelfPair;
    ContactBufferCompactionRejectReason reason) {
    case ContactBufferCompactionRejectReason::None:
    case ContactBufferCompactionRejectReason::EmptyBuffer:
    case ContactBufferCompactionRejectReason::AllValid:
        return ContactBufferCompactionRejectReason::EmptyBuffer;
        return ContactBufferCompactionRejectReason::AllValid;
    return ContactBufferCompactionRejectReason::None;
    ContactBufferCompactionRejectReason expected) {
    ContactBufferCompactionPreflight preflight{};
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;
FUSE_PHYSICS_INLINE const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason) {
    case ContactBufferClampRejectReason::None:
    case ContactBufferClampRejectReason::EmptyBuffer:
    case ContactBufferClampRejectReason::WithinCapacity:
        return ContactBufferClampRejectReason::EmptyBuffer;
        return ContactBufferClampRejectReason::WithinCapacity;
    return ContactBufferClampRejectReason::None;
    ContactBufferClampRejectReason expected) {
FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;
    ContactBufferFrictionTangentRejectReason reason) {
    case ContactBufferFrictionTangentRejectReason::None:
    case ContactBufferFrictionTangentRejectReason::EmptyBuffer:
    case ContactBufferFrictionTangentRejectReason::AllOrthonormal:
        return ContactBufferFrictionTangentRejectReason::EmptyBuffer;
        return ContactBufferFrictionTangentRejectReason::AllOrthonormal;
    return ContactBufferFrictionTangentRejectReason::None;
    ContactBufferFrictionTangentRejectReason expected) {
    ContactBufferFrictionTangentPreflight preflight{};
        preflight.reason == ContactBufferFrictionTangentRejectReason::EmptyBuffer;
        preflight.reason == ContactBufferFrictionTangentRejectReason::AllOrthonormal;
    ContactBufferCompactAndClampRejectReason reason) {
    case ContactBufferCompactAndClampRejectReason::None:
    case ContactBufferCompactAndClampRejectReason::EmptyBuffer:
    case ContactBufferCompactAndClampRejectReason::NoWork:
        return ContactBufferCompactAndClampRejectReason::EmptyBuffer;
            return ContactBufferCompactAndClampRejectReason::None;
        return ContactBufferCompactAndClampRejectReason::NoWork;
    ContactBufferCompactAndClampRejectReason expected) {
    ContactBufferCompactAndClampPreflight preflight{};
        preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork;
    ContactBufferToVectorRejectReason reason) {
    case ContactBufferToVectorRejectReason::None:
    case ContactBufferToVectorRejectReason::EmptyBuffer:
        return ContactBufferToVectorRejectReason::EmptyBuffer;
    return ContactBufferToVectorRejectReason::None;
    ContactBufferToVectorRejectReason expected) {
    ContactBufferToVectorPreflight preflight{};
    preflight.emptyBuffer = preflight.reason == ContactBufferToVectorRejectReason::EmptyBuffer;

// --- deepen additive from b4-narrowphase-deepen-8324 ---
const char* contact_buffer_friction_tangent_reject_reason_name(ContactBufferFrictionTangentRejectReason reason);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    preflight.invalidManifold = preflight.reason == ContactBufferWriteSlotRejectReason::InvalidManifold;
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    case ContactBufferFrictionTangentRejectReason::AllValid:
        return ContactBufferFrictionTangentRejectReason::AllValid;
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionTangentRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferFrictionTangentRejectReason::AllValid;
    const ContactBufferFrictionTangentRejectReason reason =
    return reason != ContactBufferFrictionTangentRejectReason::None;
    const ContactBufferCompactionPreflight preflight = preflight_contact_buffer_compaction(buffer);
    if (preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer) {
    if (preflight.reason == ContactBufferCompactionRejectReason::AllValid) {
    const ContactBufferCompactAndClampPreflight preflight = preflight_contact_buffer_compact_and_clamp(buffer);
    if (preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer) {
    if (preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork) {

// --- deepen additive from b4-narrowphase-deepen-guards-1595 ---
FUSE_PHYSICS_INLINE const char* contactBufferWriteSlotRejectReasonName(ContactBufferWriteSlotRejectReason reason) {
FUSE_PHYSICS_INLINE ContactBufferWriteSlotRejectReason contactBufferWriteSlotRejectReason(
    return contactBufferWriteSlotRejectReason(buffer, slot, manifold) == expected;
FUSE_PHYSICS_INLINE ContactBufferWriteSlotPreflight preflightContactBufferWriteSlot(
    preflight.reason = contactBufferWriteSlotRejectReason(buffer, slot, manifold);
    return !preflightContactBufferWriteSlot(buffer, slot, manifold).canWrite();
    return preflightContactBufferWriteSlot(buffer, slot, manifold).canWrite();
FUSE_PHYSICS_INLINE const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason) {
FUSE_PHYSICS_INLINE ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(
    return contactBufferCompactionRejectReason(buffer) == expected;
FUSE_PHYSICS_INLINE ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferCompactionRejectReason(buffer);
    return !preflightContactBufferCompaction(buffer).needsCompaction();
    return preflightContactBufferCompaction(buffer).needsCompaction();
FUSE_PHYSICS_INLINE const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason) {
FUSE_PHYSICS_INLINE ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer) {
    return contactBufferClampRejectReason(buffer) == expected;
FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferClampRejectReason(buffer);
    return !preflightContactBufferClamp(buffer).needsClamp();
    return preflightContactBufferClamp(buffer).needsClamp();
FUSE_PHYSICS_INLINE const char* contactBufferCompactAndClampRejectReasonName(
FUSE_PHYSICS_INLINE ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(
    return contactBufferCompactAndClampRejectReason(buffer) == expected;
FUSE_PHYSICS_INLINE ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(
    preflight.reason = contactBufferCompactAndClampRejectReason(buffer);
    return !preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();
    return preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();
FUSE_PHYSICS_INLINE const char* contactBufferFrictionBuildRejectReasonName(
    ContactBufferFrictionBuildRejectReason reason) {
    case ContactBufferFrictionBuildRejectReason::None:
    case ContactBufferFrictionBuildRejectReason::EmptyBuffer:
    case ContactBufferFrictionBuildRejectReason::AllCached:
FUSE_PHYSICS_INLINE ContactBufferFrictionBuildRejectReason contactBufferFrictionBuildRejectReason(
        return ContactBufferFrictionBuildRejectReason::EmptyBuffer;
        return ContactBufferFrictionBuildRejectReason::AllCached;
    return ContactBufferFrictionBuildRejectReason::None;
    ContactBufferFrictionBuildRejectReason expected) {
    return contactBufferFrictionBuildRejectReason(buffer) == expected;
FUSE_PHYSICS_INLINE ContactBufferFrictionBuildPreflight preflightContactBufferFrictionBuild(
    ContactBufferFrictionBuildPreflight preflight{};
    preflight.reason = contactBufferFrictionBuildRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBuildRejectReason::EmptyBuffer;
    preflight.allCached = preflight.reason == ContactBufferFrictionBuildRejectReason::AllCached;
    return !preflightContactBufferFrictionBuild(buffer).needsFrictionBuild();
    return preflightContactBufferFrictionBuild(buffer).needsFrictionBuild();
FUSE_PHYSICS_INLINE const char* contactBufferWarmStartRejectReasonName(ContactBufferWarmStartRejectReason reason) {
    case ContactBufferWarmStartRejectReason::None:
    case ContactBufferWarmStartRejectReason::OutOfRangeSlot:
    case ContactBufferWarmStartRejectReason::InvalidSlot:
FUSE_PHYSICS_INLINE ContactBufferWarmStartRejectReason contactBufferWarmStartRejectReason(
        return ContactBufferWarmStartRejectReason::OutOfRangeSlot;
        return ContactBufferWarmStartRejectReason::InvalidSlot;
    return ContactBufferWarmStartRejectReason::None;
    ContactBufferWarmStartRejectReason expected) {
    return contactBufferWarmStartRejectReason(buffer, slot) == expected;
FUSE_PHYSICS_INLINE ContactBufferWarmStartPreflight preflightContactBufferWarmStart(
    ContactBufferWarmStartPreflight preflight{};
    preflight.reason = contactBufferWarmStartRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWarmStartRejectReason::OutOfRangeSlot;
    preflight.invalidSlot = preflight.reason == ContactBufferWarmStartRejectReason::InvalidSlot;
    return !preflightContactBufferWarmStart(buffer, slot).canWarmStart();
    return preflightContactBufferWarmStart(buffer, slot).canWarmStart();
FUSE_PHYSICS_INLINE const char* contactBufferToVectorRejectReasonName(ContactBufferToVectorRejectReason reason) {
FUSE_PHYSICS_INLINE ContactBufferToVectorRejectReason contactBufferToVectorRejectReason(
    return contactBufferToVectorRejectReason(buffer) == expected;
FUSE_PHYSICS_INLINE ContactBufferToVectorPreflight preflightContactBufferToVector(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferToVectorRejectReason(buffer);
    return !preflightContactBufferToVector(buffer).canExport();
    return preflightContactBufferToVector(buffer).canExport();

// --- deepen additive from b4-narrowphase-deepen-guards-af6c ---
enum class ContactBufferManifoldAtRejectReason : u8 {
const char* contact_buffer_manifold_at_reject_reason_name(ContactBufferManifoldAtRejectReason reason);
ContactBufferManifoldAtRejectReason contact_buffer_manifold_at_reject_reason(
struct ContactBufferManifoldAtPreflight {
    ContactBufferManifoldAtRejectReason reason = ContactBufferManifoldAtRejectReason::None;
    bool can_read() const { return reason == ContactBufferManifoldAtRejectReason::None; }
ContactBufferManifoldAtPreflight preflight_contact_buffer_manifold_at(
enum class ContactBufferTangentBasisRejectReason : u8 {
const char* contact_buffer_tangent_basis_reject_reason_name(ContactBufferTangentBasisRejectReason reason);
ContactBufferTangentBasisRejectReason contact_buffer_tangent_basis_reject_reason(const ContactBufferSoA& buffer);
struct ContactBufferTangentBasisPreflight {
    ContactBufferTangentBasisRejectReason reason = ContactBufferTangentBasisRejectReason::None;
    bool can_build() const { return reason == ContactBufferTangentBasisRejectReason::None; }
ContactBufferTangentBasisPreflight preflight_contact_buffer_tangent_basis(const ContactBufferSoA& buffer);
    case ContactBufferClampRejectReason::NoCapacityLimit:
        return ContactBufferClampRejectReason::NoCapacityLimit;
    preflight.noCapacityLimit = preflight.reason == ContactBufferClampRejectReason::NoCapacityLimit;
    ContactBufferManifoldAtRejectReason reason) {
    case ContactBufferManifoldAtRejectReason::None:
    case ContactBufferManifoldAtRejectReason::OutOfRangeIndex:
    case ContactBufferManifoldAtRejectReason::InvalidSlot:
FUSE_PHYSICS_INLINE ContactBufferManifoldAtRejectReason contact_buffer_manifold_at_reject_reason(
        return ContactBufferManifoldAtRejectReason::OutOfRangeIndex;
        return ContactBufferManifoldAtRejectReason::InvalidSlot;
    return ContactBufferManifoldAtRejectReason::None;
FUSE_PHYSICS_INLINE ContactBufferManifoldAtPreflight preflight_contact_buffer_manifold_at(
    ContactBufferManifoldAtPreflight preflight{};
    preflight.outOfRangeIndex = preflight.reason == ContactBufferManifoldAtRejectReason::OutOfRangeIndex;
    preflight.invalidSlot = preflight.reason == ContactBufferManifoldAtRejectReason::InvalidSlot;
FUSE_PHYSICS_INLINE ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
FUSE_PHYSICS_INLINE ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    ContactBufferTangentBasisRejectReason reason) {
    case ContactBufferTangentBasisRejectReason::None:
    case ContactBufferTangentBasisRejectReason::EmptyBuffer:
    case ContactBufferTangentBasisRejectReason::NoValidSlots:
FUSE_PHYSICS_INLINE ContactBufferTangentBasisRejectReason contact_buffer_tangent_basis_reject_reason(
        return ContactBufferTangentBasisRejectReason::EmptyBuffer;
            return ContactBufferTangentBasisRejectReason::None;
    return ContactBufferTangentBasisRejectReason::NoValidSlots;
FUSE_PHYSICS_INLINE ContactBufferTangentBasisPreflight preflight_contact_buffer_tangent_basis(
    ContactBufferTangentBasisPreflight preflight{};
    preflight.emptyBuffer = preflight.reason == ContactBufferTangentBasisRejectReason::EmptyBuffer;
    preflight.noValidSlots = preflight.reason == ContactBufferTangentBasisRejectReason::NoValidSlots;

// --- deepen additive from b4-narrowphase-deepen-guards-ea87 ---
    u32 applyMaxCapacityClampWithPreflight();
    bool applyWarmStartStubWithPreflight(u32 slot, ContactManifold& manifold) const;
ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent_bases(

// --- deepen additive from deepen-b4-narrowphase-guards-bfb5 ---
enum class ContactBufferBuildFrictionRejectReason : u8 {
const char* contact_buffer_build_friction_reject_reason_name(ContactBufferBuildFrictionRejectReason reason);
ContactBufferBuildFrictionRejectReason contact_buffer_build_friction_reject_reason(
    ContactBufferBuildFrictionRejectReason expected);
struct ContactBufferBuildFrictionPreflight {
    ContactBufferBuildFrictionRejectReason reason = ContactBufferBuildFrictionRejectReason::None;
    bool can_build() const { return reason == ContactBufferBuildFrictionRejectReason::None; }
ContactBufferBuildFrictionPreflight preflight_contact_buffer_build_friction(const ContactBufferSoA& buffer);

// --- deepen additive from b4-narrowphase-deepen-guards-04be ---
    ContactBufferFrictionBasisRejectReason expected,
ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_bases(
inline const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason) {
inline const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason) {
inline ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer) {
inline ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer) {
inline const char* contact_buffer_to_vector_reject_reason_name(ContactBufferToVectorRejectReason reason) {
inline ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(const ContactBufferSoA& buffer) {
inline ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    ContactBufferFrictionBasisRejectReason reason) {
    case ContactBufferFrictionBasisRejectReason::None:
    case ContactBufferFrictionBasisRejectReason::EmptyBuffer:
    case ContactBufferFrictionBasisRejectReason::AllValid:
inline ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(
        return ContactBufferFrictionBasisRejectReason::EmptyBuffer;
            return ContactBufferFrictionBasisRejectReason::None;
    return ContactBufferFrictionBasisRejectReason::AllValid;
inline ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_bases(
    ContactBufferFrictionBasisPreflight preflight{};
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBasisRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferFrictionBasisRejectReason::AllValid;

// --- deepen additive from deepen-b4-narrowphase-guards-950d ---
inline ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer) {
inline ContactBufferToVectorRejectReason contactBufferToVectorRejectReason(const ContactBufferSoA& buffer) {
inline u32 compactContactBufferWithPreflight(ContactBufferSoA& buffer) {
    if (!preflightContactBufferCompaction(buffer).needsCompaction()) {
inline u32 compactAndClampContactBufferWithPreflight(ContactBufferSoA& buffer) {
    if (!preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp()) {

// --- deepen additive from deepen-b4-narrowphase-guards-5f30 ---
    bool canApply() const { return reason == ContactBufferWarmStartRejectReason::None; }
ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

// --- deepen additive from b4-narrowphase-deepen-guards-c120 ---
    bool canRebuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }

// --- deepen additive from b4-narrowphase-deeper-guards-3864 ---
inline bool ContactBufferSoA::writeSlotWithPreflight(u32 slot, const ContactManifold& manifold) {
    case ContactBufferFrictionBasisRejectReason::NoValidContacts:
        return ContactBufferFrictionBasisRejectReason::NoValidContacts;
    ContactBufferFrictionBasisRejectReason expected) {
inline ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_basis(
    preflight.noValidContacts = preflight.reason == ContactBufferFrictionBasisRejectReason::NoValidContacts;

// --- deepen additive from b4-narrowphase-deepen-guards-c379 ---
ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent(
inline ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent(
    preflight.allOrthonormal = preflight.reason == ContactBufferFrictionTangentRejectReason::AllOrthonormal;
inline const char* contact_buffer_warm_start_reject_reason_name(ContactBufferWarmStartRejectReason reason) {

// --- deepen additive from deepen-b4-narrowphase-guards-5907 ---
bool tryWriteContactBufferSlot(ContactBufferSoA& buffer, u32 slot, const ContactManifold& manifold);
inline bool tryWriteContactBufferSlot(ContactBufferSoA& buffer, u32 slot, const ContactManifold& manifold) {

// --- deepen additive from b4-narrowphase-deepen-guards-3dcc ---
    bool tryWriteSlot(u32 slot, const ContactManifold& manifold);
    u32 tryCompact();
    u32 tryApplyMaxCapacityClamp();
    u32 tryCompactAndClamp();
    std::vector<ContactManifold> tryToVector() const;
    bool tryBuildFrictionTangentBases(f32 epsilon = 1e-4f);
const char* contactBufferFrictionTangentRejectReasonName(ContactBufferFrictionTangentRejectReason reason);
ContactBufferFrictionTangentRejectReason contactBufferFrictionTangentRejectReason(
    bool needsRebuild() const { return reason == ContactBufferFrictionTangentRejectReason::None; }
ContactBufferFrictionTangentPreflight preflightContactBufferFrictionTangentBases(
    case ContactBufferWriteSlotRejectReason::SelfContact:
        return ContactBufferWriteSlotRejectReason::SelfContact;
    preflight.selfContact = preflight.reason == ContactBufferWriteSlotRejectReason::SelfContact;
FUSE_PHYSICS_INLINE const char* contactBufferFrictionTangentRejectReasonName(
FUSE_PHYSICS_INLINE ContactBufferFrictionTangentRejectReason contactBufferFrictionTangentRejectReason(
    return contactBufferFrictionTangentRejectReason(buffer, epsilon) == expected;
FUSE_PHYSICS_INLINE ContactBufferFrictionTangentPreflight preflightContactBufferFrictionTangentBases(
    preflight.reason = contactBufferFrictionTangentRejectReason(buffer, epsilon);
    return !preflightContactBufferFrictionTangentBases(buffer, epsilon).needsRebuild();
    return preflightContactBufferFrictionTangentBases(buffer, epsilon).needsRebuild();
FUSE_PHYSICS_INLINE bool ContactBufferSoA::tryWriteSlot(u32 slot, const ContactManifold& manifold) {
    if (!preflightContactBufferWriteSlot(*this, slot, manifold).canWrite()) {
FUSE_PHYSICS_INLINE u32 ContactBufferSoA::tryCompact() {
FUSE_PHYSICS_INLINE u32 ContactBufferSoA::tryApplyMaxCapacityClamp() {
FUSE_PHYSICS_INLINE u32 ContactBufferSoA::tryCompactAndClamp() {
FUSE_PHYSICS_INLINE std::vector<ContactManifold> ContactBufferSoA::tryToVector() const {
FUSE_PHYSICS_INLINE bool ContactBufferSoA::tryBuildFrictionTangentBases(f32 epsilon) {

// --- deepen additive from b4-narrowphase-deepen-guards-27c6 ---
inline const char* contactBufferCompactAndClampRejectReasonName(ContactBufferCompactAndClampRejectReason reason) {
inline ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
inline bool tryContactBufferWriteSlot(

// --- deepen additive from b4-narrowphase-guards-18a7 ---
    bool canBuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }
FUSE_PHYSICS_INLINE const char* contactBufferFrictionBasesRejectReasonName(
    ContactBufferFrictionBasesRejectReason reason) {
    case ContactBufferFrictionBasesRejectReason::None:
    case ContactBufferFrictionBasesRejectReason::EmptyBuffer:
    case ContactBufferFrictionBasesRejectReason::NoValidContacts:
FUSE_PHYSICS_INLINE ContactBufferFrictionBasesRejectReason contactBufferFrictionBasesRejectReason(
        return ContactBufferFrictionBasesRejectReason::EmptyBuffer;
        return ContactBufferFrictionBasesRejectReason::NoValidContacts;
    return ContactBufferFrictionBasesRejectReason::None;
    ContactBufferFrictionBasesRejectReason expected) {
    return contactBufferFrictionBasesRejectReason(buffer) == expected;
FUSE_PHYSICS_INLINE ContactBufferFrictionBasesPreflight preflightContactBufferFrictionBases(
    ContactBufferFrictionBasesPreflight preflight{};
    preflight.reason = contactBufferFrictionBasesRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBasesRejectReason::EmptyBuffer;
    preflight.noValidContacts = preflight.reason == ContactBufferFrictionBasesRejectReason::NoValidContacts;
    return !preflightContactBufferFrictionBases(buffer).canBuild();
    return preflightContactBufferFrictionBases(buffer).canBuild();

// --- deepen additive from deepen-b4-narrowphase-guards-6ef0 ---
u32 applyContactBufferMaxCapacityClampWithPreflight(ContactBufferSoA& buffer);
bool buildContactBufferFrictionTangentBasesWithPreflight(ContactBufferSoA& buffer);
FUSE_PHYSICS_INLINE bool tryWriteContactBufferSlot(
FUSE_PHYSICS_INLINE u32 compactContactBufferWithPreflight(ContactBufferSoA& buffer) {
FUSE_PHYSICS_INLINE u32 applyContactBufferMaxCapacityClampWithPreflight(ContactBufferSoA& buffer) {
FUSE_PHYSICS_INLINE u32 compactAndClampContactBufferWithPreflight(ContactBufferSoA& buffer) {
    const ContactBufferCompactAndClampPreflight preflight = preflightContactBufferCompactAndClamp(buffer);
FUSE_PHYSICS_INLINE const char* contactBufferFrictionBasisRejectReasonName(
    case ContactBufferFrictionBasisRejectReason::NoValidSlots:
FUSE_PHYSICS_INLINE ContactBufferFrictionBasisRejectReason contactBufferFrictionBasisRejectReason(
        return ContactBufferFrictionBasisRejectReason::NoValidSlots;
    return contactBufferFrictionBasisRejectReason(buffer) == expected;
FUSE_PHYSICS_INLINE ContactBufferFrictionBasisPreflight preflightContactBufferFrictionBasis(
    preflight.reason = contactBufferFrictionBasisRejectReason(buffer);
    preflight.noValidSlots = preflight.reason == ContactBufferFrictionBasisRejectReason::NoValidSlots;
    return !preflightContactBufferFrictionBasis(buffer).needsRebuild();
    return preflightContactBufferFrictionBasis(buffer).needsRebuild();
FUSE_PHYSICS_INLINE bool buildContactBufferFrictionTangentBasesWithPreflight(ContactBufferSoA& buffer) {
