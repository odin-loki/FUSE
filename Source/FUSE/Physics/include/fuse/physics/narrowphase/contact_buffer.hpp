#pragma once

#include <fuse/physics/config.hpp>
#include <fuse/physics/math.hpp>

#include <cstdint>
#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

#include <climits>
#include <vector>

namespace fuse::physics::narrowphase {

struct ContactBufferSoA;

/// Why contact-buffer compaction would early-out (B4.5 deepen pass).
enum class ContactBufferCompactRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,
};

/// Human-readable label for contact-buffer compact reject reasons (B4.5 deepen pass).
const char* contact_buffer_compact_reject_reason_name(ContactBufferCompactRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.5 deepen pass).
ContactBufferCompactRejectReason contact_buffer_compact_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.5 deepen pass).
/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
    NoWork,

/// Human-readable label for contact-buffer compact reject reasons (B4.6 deepen pass).

/// Diagnose why contact-buffer compact would skip; vacuously succeeds when compact may proceed (B4.6 deepen pass).

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.6 deepen pass).
/// Why contact-buffer slot write would reject (B4.6 deepen follow-up pass).
enum class ContactBufferWriteRejectReason : u8 {
    OutOfRangeSlot,
    InvalidManifold,
    SelfPair,

/// Human-readable label for contact-buffer write reject reasons (B4.6 deepen follow-up pass).
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);

/// Diagnose why slot write would skip; vacuously succeeds when write may proceed (B4.6 deepen follow-up pass).
ContactBufferWriteRejectReason contact_buffer_write_reject_reason(
    u32 slot,
    const ContactManifold& manifold,
    u32 pairSlotCount);

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_write_rejects_for_reason(
    u32 pairSlotCount,
    ContactBufferWriteRejectReason expected);

/// Const preflight for contact-buffer slot write (B4.6 deepen follow-up pass).
struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool canWrite = false;

    bool can_write() const { return canWrite && reason == ContactBufferWriteRejectReason::None; }

/// Populate write preflight without mutating buffer slots (B4.6 deepen follow-up pass).
ContactBufferWritePreflight preflight_contact_buffer_write(

/// Why contact-buffer compaction would early-out (B4.6 deepen follow-up pass).
    AllInvalid,

/// Human-readable label for contact-buffer compact reject reasons (B4.6 deepen follow-up pass).

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen follow-up pass).

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
bool contact_buffer_compact_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.5 deepen pass).
struct ContactBufferCompactPreflight {
    ContactBufferCompactRejectReason reason = ContactBufferCompactRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;
/// Read-only compact diagnostics — no mutation (B4.6 deepen pass).
    bool noWork = false;

    bool needsCompaction() const { return reason == ContactBufferCompactRejectReason::None; }
};

ContactBufferCompactPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.5 deepen pass).
bool can_skip_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compact` (B4.5 deepen pass).
bool should_run_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,

/// Human-readable label for contact-buffer clamp reject reasons (B4.5 deepen pass).
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.5 deepen pass).
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.5 deepen pass).
bool contact_buffer_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected);

/// Read-only clamp diagnostics — no mutation (B4.5 deepen pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool withinCapacity = false;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }

ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.5 deepen pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.5 deepen pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen pass).
/// Populate compact preflight without mutating the buffer (B4.6 deepen pass).

/// Non-mutating compact skip predicate — inverse of `needsCompaction` (B4.6 deepen pass).

/// Non-mutating compact predicate — mirrors `preflight_contact_buffer_compact` (B4.6 deepen pass).

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    NoWork,

/// Human-readable label for compact-and-clamp reject reasons (B4.5 deepen pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.5 deepen pass).
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.5 deepen pass).
/// Human-readable label for contact-buffer compact-and-clamp reject reasons (B4.6 deepen pass).

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen pass).

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
    ContactBufferCompactAndClampRejectReason expected);

/// Read-only compact-and-clamp diagnostics — no mutation (B4.5 deepen pass).
/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;

    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.5 deepen pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.5 deepen pass).
    bool needsCompactAndClamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }

/// Populate compact-and-clamp preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(

/// Non-mutating compact-and-clamp skip predicate (B4.6 deepen pass).

/// Non-mutating compact-and-clamp predicate (B4.6 deepen pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Const preflight for contact-buffer compaction (B4.6 deepen follow-up pass).
    bool canCompact = false;

    bool can_compact() const { return canCompact && reason == ContactBufferCompactRejectReason::None; }

/// Populate compact preflight without mutating buffer slots (B4.6 deepen follow-up pass).

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
    bool hasValidContacts() const { return activeCount > 0u; }
    bool hasDroppedContacts() const { return droppedCount > 0u; }
    bool isFull() const { return maxCapacity > 0u && activeCount >= maxCapacity; }
    bool canAcceptContacts(u32 additionalCount = 1u) const;
    u32 remainingCapacity() const;
    bool canApplyMaxCapacityClamp() const;
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    bool canSkipSoAIteration() const { return activeCount == 0u && pairSlotCount == 0u; }
    bool canSkipCompaction() const;
    /// True when clamping dropped one or more finalized contacts.
    /// True when both dense and slot storage are empty (safe to skip SoA scans).
    /// True when slot storage has no invalid flags (compact is a no-op).
    /// True when post-pass truncation would drop contacts.
    /// Inverse of `canApplyMaxCapacityClamp` (B4.4 deepen pass).
    /// True when `maxCapacity` is set and no additional contacts may be written.
    /// Remaining write slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    /// Count valid flags in prepared slot storage before compaction.
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;
    /// True when both dense and slot storage are empty (safe to skip SoA scans) (B4.4 deepen follow-up pass).
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.4 deepen follow-up pass).
    /// True when active count is within `maxCapacity` (clamp is a no-op) (B4.4 deepen follow-up pass).
    bool canSkipMaxCapacityClamp() const;
    /// True when compact+clamp would leave the buffer unchanged (B4.4 deepen follow-up pass).
    bool canSkipCompactAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction (B4.4 deepen follow-up pass).
    /// True when clamping dropped one or more contacts.
    /// Inverse of `canApplyMaxCapacityClamp` (B4.3 deepen follow-up).
    /// True when both dense and slot storage are empty (safe to skip SoA scans) (B4.6 deepen pass).
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.6 deepen pass).
    /// True when compact+clamp would leave the buffer unchanged (B4.6 deepen pass).
    /// Count valid flags in prepared slot storage before compaction (B4.6 deepen pass).
    /// Remaining slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`) (B4.6 deepen pass).
    /// True when post-pass truncation would drop contacts (B4.6 deepen pass).
    /// True when both dense and slot storage are empty (safe to skip SoA scans, B4.6 deepen pass).
    /// True when slot storage has no invalid flags (compact is a no-op, B4.6 deepen pass).
    /// Remaining write slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`, B4.6 deepen pass).
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    /// Inverse of `canApplyMaxCapacityClamp` (B4.6 deepen pass).
    /// True when compact+clamp would leave the buffer unchanged.
    /// True when at most one valid contact is present (friction-basis rebuild is a no-op).
    bool canSkipFrictionBasisBuild() const;
    /// Remaining push slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`, B4.6 deepen pass).
    /// True when clamping dropped one or more contact manifolds.
    /// True when `maxCapacity` is set and no additional contacts may be retained after clamp.
    /// Remaining slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).

    bool canSkipFrictionTangentBuild(f32 epsilon = 1e-4f) const;
    /// True when `maxCapacity` is set and no additional contacts may be stored.
    /// Contact-list guard: true when `additionalCount` writes fit before `maxCapacity` clamp.
    bool canAcceptWrites(u32 additionalCount = 1u) const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.6 deepen follow-up pass).
    /// True when `maxCapacity` is set and no additional contacts may be stored after compaction.
    /// True when `maxCapacity` is set and no additional contacts may be pushed.
    /// Inverse of `canApplyMaxCapacityClamp` (B4.5 deepen pass).
    /// Inverse of `canApplyMaxCapacityClamp` (B4.3 deepen pass).
    /// True when `maxCapacity` is set and no additional contacts may be written after compact.
    /// Remaining active slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    /// True when `maxCapacity` is set and no additional contacts may be stored after clamp.
    /// Inverse of `canApplyMaxCapacityClamp` (B4.5 deepen follow-up pass).

    /// True when pair slots are empty or already contiguously valid (B4.5 deepen pass).

    /// True when max-capacity clamp would be a no-op after compaction (B4.5 deepen pass).

    /// Count valid pair slots without mutating storage (B4.5 deepen pass).
    /// True when at least one valid contact is present after compaction.
    /// True when all active contacts already carry orthonormal tangent frames.
    /// Remaining storage slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    /// Contact-list guard: true when `additionalCount` contacts fit before `maxCapacity` clamp.
    FUSE_PHYSICS_INLINE bool canAcceptContacts(u32 additionalCount = 1u) const;
    FUSE_PHYSICS_INLINE u32 remainingCapacity() const;
    FUSE_PHYSICS_INLINE bool canApplyMaxCapacityClamp() const;
    FUSE_PHYSICS_INLINE bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    FUSE_PHYSICS_INLINE bool canSkipSoAIteration() const {
        return activeCount == 0u && pairSlotCount == 0u;
    }
    FUSE_PHYSICS_INLINE bool canSkipCompaction() const;
    FUSE_PHYSICS_INLINE u32 countValidSlots() const;
    FUSE_PHYSICS_INLINE bool slotIsValid(u32 slot) const;
    /// True when every active contact already has an orthonormal tangent frame.
    FUSE_PHYSICS_INLINE bool canSkipFrictionTangentRebuild() const;
    /// True when at least one active slot needs tangent-frame rebuild.
    FUSE_PHYSICS_INLINE bool needsFrictionTangentRebuild() const {
        return !canSkipFrictionTangentRebuild();
    /// True when all active contacts already have orthonormal tangent frames (B4.6 deepen pass).
    bool canSkipFrictionTangentRebuild(f32 epsilon = 1e-4f) const;
    FUSE_PHYSICS_INLINE u32 remainingCapacity() const {
        if (maxCapacity == 0u) {
            return UINT32_MAX;
        return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
    FUSE_PHYSICS_INLINE bool canApplyMaxCapacityClamp() const {
        return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
    FUSE_PHYSICS_INLINE bool canSkipCompaction() const {
        if (canSkipSoAIteration()) {
            return true;

        const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
        if (scanCount == 0u) {

        for (u32 slot = 0u; slot < scanCount; ++slot) {
            if (validFlags[slot] == 0u) {
                return false;
    FUSE_PHYSICS_INLINE bool canSkipCompactAndClamp() const {
        return canSkipSoAIteration() || countValidSlots() == 0u;
    FUSE_PHYSICS_INLINE u32 countValidSlots() const {
            return 0u;

        u32 validCount = 0u;
            if (validFlags[slot] != 0u) {
                ++validCount;
        return validCount;
    FUSE_PHYSICS_INLINE bool slotIsValid(u32 slot) const {
        if (slot >= validFlags.size()) {
        if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return validFlags[slot] != 0u;
    bool canApplyMaxCapacityClamp() const {
        return maxCapacity > 0u && activeCount > maxCapacity;
    /// True when every prepared slot is valid (compact is a no-op).
    bool canSkipCompaction() const {
        if (pairSlotCount == 0u) {
        for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
    u32 countValidSlots() const {
    bool slotIsValid(u32 slot) const {
        return slot < pairSlotCount && validFlags[slot] != 0u;
    /// True when at least one compacted contact is present (B4.6 deepen pass).
    /// True when max-capacity clamp dropped one or more contacts (B4.6 deepen pass).
    /// Remaining retention slots before `maxCapacity` clamp (unlimited when `maxCapacity == 0`).
    /// True when every valid slot already stores an orthonormal friction basis (B4.6 deepen pass).
    bool canSkipFrictionTangentBases(f32 epsilon = 1e-4f) const;

    void reserve(u32 capacity);
    void setMaxCapacity(u32 capacity);
    void clear();
    void preparePairSlots(u32 pairCount);
    void writeSlot(u32 slot, const ContactManifold& manifold);
    void invalidateSlot(u32 slot);
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    /// Write only when `preflightContactBufferWrite` passes; no-op otherwise (B4.6 deepen pass).
    /// Write only when preflight allows; returns false when write is skipped (B4.6 deepen pass).
    bool writeSlotIfPreflight(u32 slot, const ContactManifold& manifold);
    FUSE_PHYSICS_INLINE void invalidateSlot(u32 slot) {
        if (slot >= validFlags.size()) {
            return;
        }
        if (pairSlotCount > 0u && slot >= pairSlotCount) {
        validFlags[slot] = 0u;
        pointCounts[slot] = 0u;
    void applyWarmStartStub(u32 slot, ContactManifold& manifold) const;
    void buildFrictionTangentBases();
    void buildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    /// Rebuild friction tangents only when orthonormal frames are missing (B4.4 deepen follow-up pass).
    void buildFrictionTangentBasesIfNeeded();
    /// Write only when preflight allows; no-op otherwise (B4.6 deepen pass).
    /// Rebuild tangent columns only when preflight allows (B4.6 deepen pass).
    bool buildFrictionTangentBasesWithPreflight();
    /// Guarded write using preflight; returns false when write is rejected (B4.6 deepen pass).
    /// Rebuild tangent SoA columns only when preflight reports stale or missing frames (B4.6 deepen pass).
    bool slotFrictionBasisIsStale(u32 slot, f32 epsilon = 1e-4f) const;
    void rebuildFrictionTangentBasisAt(u32 slot, f32 epsilon = 1e-4f);
    void rebuildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    bool writeSlotWithFinalize(u32 slot, ContactManifold& manifold, f32 frictionEpsilon = 1e-4f);
    bool slotNeedsFrictionBasisRebuild(u32 slot, f32 epsilon = 1e-4f) const;
    bool frictionBasisMatchesNormalAt(u32 slot, f32 epsilon = 1e-4f) const;
    bool canSkipBuildFrictionTangentBases(f32 epsilon = 1e-4f) const;
    u32 compact();
    /// Compact only when preflight allows (B4.6 deepen pass).
    u32 compactWithPreflight();
    u32 compactIfNeeded();
    u32 applyMaxCapacityClamp();
    u32 applyMaxCapacityClampIfNeeded();
    u32 compactAndClamp();
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.4 deepen follow-up pass).
    bool canSkipCompaction() const;
    /// True when post-pass truncation would drop contacts (B4.4 deepen follow-up pass).
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.4 deepen follow-up pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// Rebuild friction tangents only for slots with missing or stale bases (B4.4 deepen follow-up pass).
    /// Compact and clamp only when preflight allows (B4.6 deepen pass).
    u32 compactAndClampWithPreflight();
    bool canSkipMaxCapacityClamp() const;
    bool canSkipCompactAndClamp() const;
    u32 countValidPairSlots() const;
    u32 compactIfNeeded();
    u32 compactAndClampIfNeeded();
    bool canSkipFrictionTangentRebuild(f32 epsilon = 1e-4f) const;
    bool rebuildFrictionTangentBasesWithPreflight(f32 epsilon = 1e-4f);
    /// Write slot only when preflight allows (B4.6 deepen follow-up pass).
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    /// Compact only when preflight allows (B4.6 deepen follow-up pass).
    u32 compactWithPreflight();
    /// Compact and clamp only when preflight allows compaction (B4.6 deepen follow-up pass).
    TangentBasis tangentBasisAt(u32 index) const;
    ContactManifold manifoldAt(u32 index) const;
    std::vector<ContactManifold> toVector() const;

    /// Non-mutating write skip predicate — mirrors `preflightContactBufferWrite` (B4.4 deepen guard pass).
    bool canSkipWrite(u32 slot, const ContactManifold& manifold) const;

    /// Non-mutating compaction skip predicate — inverse of `shouldRunCompaction` (B4.4 deepen guard pass).
    bool canSkipCompaction() const;

    /// Non-mutating clamp skip predicate — inverse of `shouldRunClamp` (B4.4 deepen guard pass).
    bool canSkipClamp() const;
    /// True when slot storage has no valid flags (compact is a no-op).
    /// True when post-pass truncation would drop contacts.
    bool canApplyMaxCapacityClamp() const;
    /// Inverse of `canApplyMaxCapacityClamp` (B4.4 deepen guard pass).
    bool canSkipMaxCapacityClamp() const { return !canApplyMaxCapacityClamp(); }
    /// True when slot storage has no invalid flags (compact is a no-op) (B4.5 deepen pass).
    /// True when post-pass truncation would drop contacts (B4.5 deepen pass).
    /// Inverse of `canApplyMaxCapacityClamp` (B4.5 deepen pass).
    /// Count valid flags in prepared slot storage before compaction (B4.5 deepen pass).
    u32 countValidSlots() const;
    bool slotIsValid(u32 slot) const;
    /// Returns true when slot is in range and manifold is valid with distinct bodies (B4.5 deepen pass).
    static bool canWriteSlot(u32 slot, u32 pairSlotCount, const ContactManifold& manifold);

    /// Write slot only when buffer preflight passes (B4.5 deepen pass).
    /// Write only when `preflight_contact_buffer_write` allows (B4.4 deepen pass follow-up).
    bool writeSlotIfValid(u32 slot, const ContactManifold& manifold);
    bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold);
    /// True when the buffer has no pair slots or active contacts (B4.6 deepen pass).
    bool canSkipSoAIteration() const { return pairSlotCount == 0u; }

    /// True when compaction would be a no-op (B4.6 deepen pass).

    /// True when max-capacity clamp would be a no-op (B4.6 deepen pass).
    /// Returns true when every active slot has a valid orthonormal friction basis (B4.6 deepen pass).
    bool canSkipFrictionRebuild(f32 epsilon = 1e-4f) const;

    /// Rebuild tangent SoA columns only for slots missing or stale bases (B4.6 deepen pass).
    void rebuildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    /// True when the buffer has no pair slots allocated (B4.6 deepen pass).

    /// True when every prepared slot is valid (B4.6 deepen pass).

    /// Count valid flags in prepared slot storage before compaction (B4.6 deepen pass).

    /// True when max-capacity clamp may reduce active contacts (B4.6 deepen pass).
    bool canSkipBuildFrictionTangentBases() const;
    bool shouldRunBuildFrictionTangentBases() const;
    /// True when slot storage has no invalid flags (compact is a no-op, B4.6 deepen follow-up pass).
    /// True when post-pass truncation would not drop contacts (B4.6 deepen follow-up pass).
    /// True when compact+clamp would leave the buffer unchanged (B4.6 deepen follow-up pass).
    bool canSkipCompactAndClamp() const;
    /// Count valid flags in prepared slot storage before compaction (B4.6 deepen follow-up pass).
    /// Returns true when the pair slot holds a valid contact (B4.6 deepen pass).
    bool hasValidPairSlot(u32 slot) const;

    /// Returns true when compact would be a no-op (B4.6 deepen pass).
    bool canSkipBufferCompact() const;
    /// Write only when preflight allows; returns false when skipped (B4.6 deepen pass).
    /// Compact only when preflight allows; returns active count (B4.6 deepen pass).
    u32 compactWithPreflight();
    /// Clamp only when preflight allows; returns active count (B4.6 deepen pass).
    u32 applyMaxCapacityClampWithPreflight();
    /// Compact-and-clamp only when preflight allows; returns active count (B4.6 deepen pass).
    u32 compactAndClampWithPreflight();
    /// Rebuild friction tangents only when preflight allows (B4.6 deepen pass).
    void buildFrictionTangentBasesIfNeeded(f32 epsilon = 1e-4f);
    /// Apply warm-start stub only when preflight allows; returns false when skipped (B4.6 deepen pass).
    bool applyWarmStartStubWithPreflight(u32 slot, ContactManifold& manifold) const;

private:
    u32 pointSlotBase(u32 slot) const { return slot * kMaxContactPointsPerManifold; }
};

/// Why contact-buffer write would reject (B4.6 deepen pass).
/// Why contact-buffer write would reject (B4.3 deepen follow-up pass).
enum class ContactBufferWriteRejectReason : u8 {
    None = 0,
    InvalidSlot,
/// Why contact-buffer slot write would early-out (B4.6 deepen pass).
/// Why contact-buffer write-slot would reject (B4.6 deepen pass).
enum class ContactBufferWriteSlotRejectReason : u8 {
/// Why contact-buffer write would reject (B4.6 deepen follow-up pass).
/// Why contact-buffer write would reject (B4.5 deepen follow-up pass).
    OutOfRangeSlot,
/// Why contact-buffer write would reject (B4.5 deepen pass).
/// Why contact-buffer write would reject (B4.3 deepen pass).
/// Why contact-buffer slot write would reject (B4.6 deepen pass).
/// Why contact-buffer write would reject (B4.6 narrowphase deepen pass).
/// Why contact-buffer slot write would reject (B4.5 deepen pass).
/// Why contact-buffer slot write would reject (B4.5 deepen follow-up pass).
/// Why contact-buffer writeSlot would reject (B4.5 deepen follow-up pass).
/// Why contact-buffer slot write would reject (B4.6 deepen follow-up pass).
    InvalidManifold,
    SelfPair,
};

/// Human-readable label for contact-buffer write reject reasons (B4.6 deepen pass).
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason);

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.6 deepen pass).
ContactBufferWriteRejectReason contact_buffer_write_reject_reason(

const char* contactBufferWriteRejectReasonName(ContactBufferWriteRejectReason reason);

/// Human-readable label for contact-buffer write reject reasons (B4.3 deepen follow-up pass).

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.3 deepen follow-up pass).
ContactBufferWriteRejectReason contactBufferWriteRejectReason(
/// Diagnose why write would skip; vacuously succeeds when write may proceed (B4.6 deepen pass).



/// Diagnose why slot write would skip; vacuously succeeds when write may proceed (B4.6 deepen pass).
/// Human-readable label for contact-buffer write-slot reject reasons (B4.6 deepen pass).
const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason);

/// Diagnose why write-slot would reject; vacuously succeeds when write may proceed.
ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
/// Human-readable label for contact-buffer write reject reasons (B4.6 deepen follow-up pass).

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.6 deepen follow-up pass).
/// Human-readable label for contact-buffer write reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.5 deepen follow-up pass).

/// Human-readable label for contact-buffer write reject reasons (logging / tests).

/// Diagnose why write would reject; vacuously succeeds when write may proceed.




/// Human-readable label for contact-buffer write reject reasons (B4.5 deepen pass).


/// Diagnose why slot write would reject; vacuously succeeds when write may proceed (B4.6 deepen pass).


/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.5 deepen pass).







const char* contactBufferWriteSlotRejectReasonName(ContactBufferWriteSlotRejectReason reason);

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed (B4.6 deepen pass).
ContactBufferWriteSlotRejectReason contactBufferWriteSlotRejectReason(

/// Human-readable label for contact-buffer write-slot reject reasons (logging / tests).

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed.
/// Human-readable label for contact-buffer write-slot reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed (B4.5 deepen follow-up pass).

FUSE_PHYSICS_INLINE const char* contact_buffer_write_slot_reject_reason_name(
    ContactBufferWriteSlotRejectReason reason);

FUSE_PHYSICS_INLINE ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
/// Why contact-buffer writeSlot would reject (B4.6 deepen pass).




    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold);

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_write_rejects_for_reason(
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferWriteRejectsForReason(
/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.5 deepen follow-up pass).
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.3 deepen follow-up pass).
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.5 deepen pass).
/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.3 deepen pass).
    const ContactManifold& manifold,
    ContactBufferWriteRejectReason expected);

/// Read-only write diagnostics — no mutation (B4.6 deepen pass).

/// Read-only write diagnostics — no mutation (B4.3 deepen follow-up pass).
struct ContactBufferWritePreflight {
    ContactBufferWriteRejectReason reason = ContactBufferWriteRejectReason::None;
    bool invalidSlot = false;
/// Const preflight for contact-buffer slot write (B4.6 deepen pass).
/// Read-only slot-write diagnostics — no mutation (B4.6 deepen pass).
/// Returns true when `contact_buffer_write_slot_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_write_slot_rejects_for_reason(
/// Returns true when `contactBufferWriteSlotRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferWriteSlotRejectsForReason(
FUSE_PHYSICS_INLINE bool contact_buffer_write_slot_rejects_for_reason(
    ContactBufferWriteSlotRejectReason expected);

/// Read-only write-slot diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferWriteSlotPreflight {
    ContactBufferWriteSlotRejectReason reason = ContactBufferWriteSlotRejectReason::None;
/// Read-only write diagnostics — no mutation (B4.6 deepen follow-up pass).
/// Const preflight for contact-buffer write dispatch (B4.5 deepen follow-up pass).
/// Const preflight for contact-buffer slot write (B4.6 deepen follow-up pass).
    bool outOfRangeSlot = false;
/// Read-only write diagnostics — no mutation (B4.5 deepen pass).
/// Read-only write diagnostics — no mutation (B4.3 deepen pass).


/// Read-only write diagnostics — no mutation (B4.5 deepen follow-up pass).
    bool outOfRange = false;
    bool invalidManifold = false;
    bool selfPair = false;

    bool canWrite() const { return reason == ContactBufferWriteRejectReason::None; }

/// Populate write preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferWritePreflight preflight_contact_buffer_write(


ContactBufferWritePreflight preflightContactBufferWrite(

/// Non-mutating write skip predicate — inverse of `canWrite` (B4.6 deepen pass).
bool canSkipContactBufferWrite(

/// Non-mutating write predicate — mirrors `preflightContactBufferWrite` (B4.6 deepen pass).
bool shouldRunContactBufferWrite(

/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    EmptyBuffer,
    NoWork,
    AllValid,




    bool can_write() const { return reason == ContactBufferWriteRejectReason::None; }




/// Returns true when contact-buffer write should be skipped (B4.6 deepen pass).
bool should_skip_contact_buffer_write(



/// Write only when preflight passes; no-op otherwise (B4.6 deepen pass).
void write_contact_buffer_slot_with_preflight(
    ContactBufferSoA& buffer,

    bool canWrite() const { return reason == ContactBufferWriteSlotRejectReason::None; }

ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(

/// Non-mutating write-slot skip predicate — inverse of `canWrite` (B4.6 deepen pass).
bool should_skip_contact_buffer_write_slot(






/// Non-mutating slot-write skip predicate — inverse of `canWrite` (B4.6 deepen pass).
























    bool can_write() const { return reason == ContactBufferWriteSlotRejectReason::None; }
};

/// Populate write-slot preflight without mutating the buffer (B4.6 deepen pass).


FUSE_PHYSICS_INLINE ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(

/// Returns true when writeSlot should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_write_slot(

    None = 0,
/// Non-mutating write-slot skip predicate — inverse of `can_write` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_write_slot(

/// Returns true when writeSlot may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_write_slot(

/// Non-mutating write-slot predicate — mirrors `preflight_contact_buffer_write_slot` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool should_run_contact_buffer_write_slot(



/// Human-readable label for contact-buffer compaction reject reasons (B4.6 deepen pass).
const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen pass).
ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.6 deepen pass).
/// Why contact-buffer write would reject (B4.4 deepen guard pass).


/// Why contact-buffer write would reject (B4.4 deepen pass).
/// Why contact-buffer slot write would reject (B4.4 deepen pass).
    OutOfRange,






/// Why contact-buffer write would reject (B4.4 guard pass).

/// Human-readable label for contact-buffer write reject reasons (B4.4 guard pass).

/// Diagnose why write would reject; vacuously succeeds when write may proceed (B4.4 guard pass).


/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.4 deepen pass).

/// Read-only write diagnostics — no mutation (B4.4 deepen pass).

/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.4 deepen guard pass).

/// Read-only write diagnostics — no mutation (B4.4 deepen guard pass).

/// Returns true when `contact_buffer_write_reject_reason` matches `expected` (B4.4 deepen pass).



/// Returns true when `contactBufferWriteRejectReason` matches `expected` (B4.4 guard pass).

/// Read-only write diagnostics — no mutation (B4.4 guard pass).



/// Why contact-buffer compaction would early-out (B4.4 deepen guard pass).



bool contact_buffer_compaction_rejects_for_reason(
    AllInvalid,

const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason);





/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed (B4.6 deepen pass).











bool shouldSkipContactBufferWrite(


/// Human-readable label for contact-buffer compaction reject reasons (logging / tests).





/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed.














ContactBufferWriteSlotPreflight preflightContactBufferWriteSlot(



bool canSkipContactBufferWriteSlot(

/// Non-mutating write-slot predicate — mirrors `preflightContactBufferWriteSlot` (B4.6 deepen pass).
bool shouldRunContactBufferWriteSlot(







ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferCompactionRejectsForReason(






/// Why contact-buffer compaction would early-out (B4.6 narrowphase deepen pass).







FUSE_PHYSICS_INLINE const char* contact_buffer_compaction_reject_reason_name(
    ContactBufferCompactionRejectReason reason);

FUSE_PHYSICS_INLINE ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(
    const ContactBufferSoA& buffer);

FUSE_PHYSICS_INLINE bool contact_buffer_compaction_rejects_for_reason(

    ContactBufferCompactionRejectReason expected);

/// Read-only compaction diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool noWork = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }
    bool allValid = false;


/// Populate compaction preflight without mutating the buffer (B4.6 deepen pass).














ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.6 deepen pass).

    bool needs_compaction() const { return reason == ContactBufferCompactionRejectReason::None; }


/// Non-mutating compaction skip predicate — inverse of `needs_compaction` (B4.6 deepen pass).
bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.6 deepen pass).
bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-basis rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionBasisRejectReason : u8 {
    NoValidManifolds,

/// Const preflight for contact-buffer compaction (B4.6 deepen pass).



/// Returns true when compaction should be skipped (B4.6 deepen pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    WithinCapacity,





/// Returns true when compaction may proceed (B4.6 deepen pass).


/// Human-readable label for contact-buffer clamp reject reasons (B4.6 deepen pass).
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.6 deepen pass).
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_clamp_rejects_for_reason(
    ContactBufferClampRejectReason expected);

/// Const preflight for contact-buffer max-capacity clamp (B4.6 deepen pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool withinCapacity = false;

    bool needs_clamp() const { return reason == ContactBufferClampRejectReason::None; }


/// Read-only max-capacity clamp diagnostics — no mutation (B4.6 deepen pass).


/// Populate clamp preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Returns true when max-capacity clamp should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
/// Returns true when clamp should be skipped (B4.6 deepen pass).

/// Returns true when clamp may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);



FUSE_PHYSICS_INLINE ContactBufferCompactionPreflight preflight_contact_buffer_compaction(

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);


FUSE_PHYSICS_INLINE const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

FUSE_PHYSICS_INLINE ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(

FUSE_PHYSICS_INLINE bool contact_buffer_clamp_rejects_for_reason(



FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needs_clamp` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-tangent rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionTangentRejectReason : u8 {
    AllOrthonormal,

/// Human-readable label for contact-buffer friction-tangent reject reasons (B4.6 deepen pass).
FUSE_PHYSICS_INLINE const char* contact_buffer_friction_tangent_reject_reason_name(
    ContactBufferFrictionTangentRejectReason reason);

/// Diagnose why friction-tangent rebuild would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
FUSE_PHYSICS_INLINE ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(

/// Returns true when `contact_buffer_friction_tangent_reject_reason` matches `expected` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool contact_buffer_friction_tangent_rejects_for_reason(
    ContactBufferFrictionTangentRejectReason expected);

/// Read-only friction-tangent rebuild diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionTangentPreflight {
    ContactBufferFrictionTangentRejectReason reason = ContactBufferFrictionTangentRejectReason::None;
    bool allOrthonormal = false;

    bool needs_rebuild() const { return reason == ContactBufferFrictionTangentRejectReason::None; }

FUSE_PHYSICS_INLINE ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent_rebuild(

/// Non-mutating friction-tangent rebuild skip predicate (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_friction_tangent_rebuild(const ContactBufferSoA& buffer);

/// Non-mutating friction-tangent rebuild predicate (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool should_run_contact_buffer_friction_tangent_rebuild(const ContactBufferSoA& buffer);




/// Human-readable label for contact-buffer compact-and-clamp reject reasons (B4.6 deepen pass).
/// Human-readable label for compact-and-clamp reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason);

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen pass).
ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_and_clamp_rejects_for_reason(
    ContactBufferCompactAndClampRejectReason expected);

/// Const preflight for contact-buffer compact-and-clamp (B4.6 deepen pass).
FUSE_PHYSICS_INLINE const char* contact_buffer_compact_and_clamp_reject_reason_name(
    ContactBufferCompactAndClampRejectReason reason);

FUSE_PHYSICS_INLINE ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(

FUSE_PHYSICS_INLINE bool contact_buffer_compact_and_clamp_rejects_for_reason(

/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
/// Returns true when `contact_buffer_write_slot_reject_reason` matches `expected` (B4.6 deepen follow-up pass).
    const ContactBufferSoA& buffer,
    u32 slot,

/// Read-only write-slot diagnostics — no mutation (B4.6 deepen follow-up pass).


    const ContactManifold& manifold);

/// Non-mutating write-slot skip predicate — inverse of `can_write` (B4.6 deepen follow-up pass).

/// Non-mutating write-slot predicate — mirrors `preflight_contact_buffer_write_slot` (B4.6 deepen follow-up pass).

/// Write only when preflight allows; returns false when skipped (B4.6 deepen follow-up pass).
bool write_contact_buffer_slot_with_preflight(

/// Why contact-buffer compaction would early-out (B4.6 deepen follow-up pass).


/// Diagnose why compact would early-out; vacuously succeeds when compaction may proceed.

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.6 deepen follow-up pass).

/// Read-only compaction diagnostics — no mutation (B4.6 deepen follow-up pass).



/// Non-mutating compaction skip predicate — inverse of `needs_compaction` (B4.6 deepen follow-up pass).

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.6 deepen follow-up pass).

/// Compact only when preflight allows; returns active count or zero when skipped (B4.6 deepen follow-up pass).
u32 compact_contact_buffer_with_preflight(ContactBufferSoA& buffer);

/// Why contact-buffer clamp would early-out (B4.6 deepen follow-up pass).
    NoCapacityLimit,

/// Human-readable label for contact-buffer clamp reject reasons (logging / tests).

/// Diagnose why applyMaxCapacityClamp would early-out; vacuously succeeds when clamp may proceed.

/// Read-only clamp diagnostics — no mutation (B4.6 deepen follow-up pass).
    bool noCapacityLimit = false;



/// Non-mutating clamp skip predicate — inverse of `needs_clamp` (B4.6 deepen follow-up pass).

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.6 deepen follow-up pass).

/// Clamp only when preflight allows; returns active count or prior count when skipped (B4.6 deepen follow-up pass).
u32 apply_contact_buffer_max_capacity_clamp_with_preflight(ContactBufferSoA& buffer);

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen follow-up pass).

/// Human-readable label for contact-buffer compact-and-clamp reject reasons (logging / tests).

/// Diagnose why compactAndClamp would early-out; vacuously succeeds when pass may proceed.

/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen follow-up pass).

    bool needs_compact_and_clamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;
    }

/// Populate compact-and-clamp preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(


/// Returns true when compact-and-clamp should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer toVector would early-out (B4.6 deepen pass).
enum class ContactBufferToVectorRejectReason : u8 {
/// Returns true when compact-and-clamp may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);


FUSE_PHYSICS_INLINE ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(

/// Non-mutating compact-and-clamp skip predicate (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

    bool needs_compact_and_clamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needs_compact_and_clamp` (B4.6 deepen pass).

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.6 deepen pass).


/// Human-readable label for friction-tangent rebuild reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_tangent_reject_reason_name(ContactBufferFrictionTangentRejectReason reason);

ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(
    f32 epsilon = 1e-4f);

bool contact_buffer_friction_tangent_rejects_for_reason(
    ContactBufferFrictionTangentRejectReason expected,



/// Populate friction-tangent rebuild preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent_rebuild(

bool can_skip_contact_buffer_friction_tangent_rebuild(

bool should_run_contact_buffer_friction_tangent_rebuild(


/// Human-readable label for contact-buffer toVector reject reasons (B4.6 deepen pass).
const char* contact_buffer_to_vector_reject_reason_name(ContactBufferToVectorRejectReason reason);

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed (B4.6 deepen pass).
ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_to_vector_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_to_vector_rejects_for_reason(
    ContactBufferToVectorRejectReason expected);

/// Const preflight for contact-buffer export (B4.6 deepen pass).
struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;

    bool can_export() const { return reason == ContactBufferToVectorRejectReason::None; }

/// Const preflight for contact-buffer toVector (B4.6 deepen pass).


/// Read-only toVector diagnostics — no mutation (B4.6 deepen pass).


/// Populate toVector preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Returns true when toVector should return empty (B4.6 deepen pass).
bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Why contact-buffer friction-basis build would early-out (B4.6 deepen pass).
    NoValidContacts,

/// Human-readable label for contact-buffer friction-basis reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_basis_reject_reason_name(ContactBufferFrictionBasisRejectReason reason);

/// Diagnose why friction-basis rebuild would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
/// Diagnose why buildFrictionTangentBases would skip; vacuously succeeds when build may proceed (B4.6 deepen pass).
ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(

/// Returns true when `contact_buffer_friction_basis_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_basis_rejects_for_reason(
    ContactBufferFrictionBasisRejectReason expected);

/// Read-only friction-basis rebuild diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionBasisPreflight {
    ContactBufferFrictionBasisRejectReason reason = ContactBufferFrictionBasisRejectReason::None;
    bool noValidManifolds = false;

    bool needsRebuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }

/// Populate friction-basis preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_basis(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis skip predicate — inverse of `needsRebuild` (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_basis(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis predicate — mirrors `preflight_contact_buffer_friction_basis` (B4.6 deepen pass).
bool should_run_contact_buffer_friction_basis(const ContactBufferSoA& buffer);
/// Read-only write-slot diagnostics — no mutation (B4.4 deepen follow-up pass).

    bool canWrite() const { return !invalidManifold && !selfPair; }

ContactBufferWritePreflight preflight_contact_buffer_write(const ContactManifold& manifold);

/// Read-only compaction diagnostics — no mutation (B4.4 deepen follow-up pass).

    bool needsCompaction() const { return !emptyBuffer && !allValid; }


/// Read-only max-capacity clamp diagnostics — no mutation (B4.4 deepen follow-up pass).

    bool needsClamp() const { return !emptyBuffer && !withinCapacity; }


/// Read-only friction-basis rebuild diagnostics at the SoA layer (B4.4 deepen follow-up pass).
struct ContactBufferFrictionPreflight {
    u32 staleSlotCount = 0u;
    u32 rebuildSlotCount = 0u;

    bool can_skip_rebuild() const { return emptyBuffer || rebuildSlotCount == 0u; }

ContactBufferFrictionPreflight preflight_contact_buffer_friction_rebuild(

/// Non-mutating friction rebuild skip predicate (B4.4 deepen follow-up pass).
bool can_skip_build_friction_tangent_bases(












/// Write only when preflight allows; returns false when skipped (B4.3 deepen pass).
bool writeContactBufferSlotWithPreflight(

/// Why contact-buffer compaction would early-out (B4.3 deepen pass).






ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer);

bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer);

bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.4 deepen guard pass).





/// Non-mutating write skip predicate — inverse of `canWrite` (B4.4 deepen pass).
bool can_skip_contact_buffer_write(

/// Non-mutating write predicate — mirrors `preflight_contact_buffer_write` (B4.4 deepen pass).
bool should_run_contact_buffer_write(

/// Why contact-buffer compaction would early-out (B4.4 deepen pass).




/// Non-mutating write skip predicate — inverse of `canWrite` (B4.4 deepen guard pass).

/// Non-mutating write predicate — mirrors `preflightContactBufferWrite` (B4.4 deepen guard pass).

enum class ContactBufferCompactRejectReason : u8 {


/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed.

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.4 deepen pass).

/// Read-only compaction diagnostics — no mutation (B4.4 deepen pass).





/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.4 deepen pass).

    bool allInvalid = false;

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.4 deepen guard pass).

/// Read-only compaction diagnostics — no mutation (B4.4 deepen guard pass).



/// Why contact-buffer compaction would early-out (B4.5 deepen pass).

/// Human-readable label for contact-buffer compaction reject reasons (B4.5 deepen pass).

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.5 deepen pass).

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.5 deepen pass).

/// Read-only compaction diagnostics — no mutation (B4.5 deepen pass).



/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.4 deepen pass).

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.4 deepen pass).


/// Why contact-buffer max-capacity clamp would early-out (B4.4 deepen pass).
/// Non-mutating compaction predicate — inverse of `canSkipCompaction` (B4.4 deepen guard pass).



const char* contactBufferCompactRejectReasonName(ContactBufferCompactRejectReason reason);

ContactBufferCompactRejectReason contactBufferCompactRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferCompactRejectReason` matches `expected` (B4.4 deepen guard pass).
bool contactBufferCompactRejectsForReason(
    ContactBufferCompactRejectReason expected);

struct ContactBufferCompactPreflight {
    ContactBufferCompactRejectReason reason = ContactBufferCompactRejectReason::None;

    bool needsCompaction() const { return reason == ContactBufferCompactRejectReason::None; }

ContactBufferCompactPreflight preflightContactBufferCompact(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.4 deepen guard pass).
bool canSkipContactBufferCompact(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompact` (B4.4 deepen guard pass).
bool shouldRunContactBufferCompact(const ContactBufferSoA& buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.5 deepen pass).

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.5 deepen pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen pass).
/// Why contact-buffer max-capacity clamp would early-out (B4.6 narrowphase deepen pass).

















    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }


bool canSkipContactBufferClamp(const ContactBufferSoA& buffer);




/// Why contact-buffer compaction would early-out (B4.3 deepen follow-up pass).

/// Human-readable label for contact-buffer compaction reject reasons (B4.3 deepen follow-up pass).


/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.3 deepen follow-up pass).

/// Read-only compaction diagnostics — no mutation (B4.3 deepen follow-up pass).



/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.3 deepen follow-up pass).

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.3 deepen follow-up pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.3 deepen follow-up pass).

/// Human-readable label for contact-buffer clamp reject reasons (B4.3 deepen follow-up pass).


/// Non-mutating write predicate — mirrors `preflightContactBufferWrite` (B4.3 deepen follow-up pass).
bool canWriteContactBufferSlot(

/// Write only when preflight passes; no-op otherwise (B4.3 deepen follow-up pass).















/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.6 deepen pass).










/// Write only when preflight allows; returns false when skipped (B4.5 deepen pass).
bool writeContactSlotWithPreflight(














/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed (B4.5 deepen pass).








/// Human-readable label for contact-buffer clamp reject reasons (B4.5 deepen pass).

















const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason);

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed.
ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.4 deepen pass).
bool contactBufferClampRejectsForReason(

/// Read-only max-capacity clamp diagnostics — no mutation (B4.4 deepen pass).


/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.4 deepen guard pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.4 deepen guard pass).



ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer);

/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.4 deepen pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.4 deepen pass).
/// Non-mutating clamp predicate — inverse of `canSkipClamp` (B4.4 deepen guard pass).
bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer);



/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.4 deepen pass).





/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.4 deepen guard pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.4 deepen guard pass).


/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.5 deepen pass).

/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.5 deepen pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.5 deepen pass).



/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.5 deepen pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.5 deepen pass).
/// Const preflight for contact-buffer compaction dispatch (B4.4 deepen follow-up pass).
    bool skipped = false;
    bool emptySlots = false;
    bool needsCompaction = false;

    bool can_skip_compaction() const { return skipped || !needsCompaction; }
    bool should_run_compaction() const { return !can_skip_compaction(); }

/// Populate compaction preflight without mutating the buffer (B4.4 deepen follow-up pass).
ContactBufferCompactionPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer compaction should be skipped (B4.4 deepen follow-up pass).
bool should_skip_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Const preflight for contact-buffer max-capacity clamp dispatch (B4.4 deepen follow-up pass).
    bool needsClamp = false;

    bool can_skip_clamp() const { return skipped || !needsClamp; }
    bool should_run_clamp() const { return !can_skip_clamp(); }

/// Populate clamp preflight without mutating the buffer (B4.4 deepen follow-up pass).

/// Returns true when contact-buffer clamp should be skipped (B4.4 deepen follow-up pass).
bool should_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

/// Const preflight for contact-buffer friction-tangent rebuild dispatch (B4.4 deepen follow-up pass).
    u32 slotCount = 0u;
    u32 needsRebuildCount = 0u;

    bool can_skip_rebuild() const { return skipped || needsRebuildCount == 0u; }
    bool should_run_rebuild() const { return !can_skip_rebuild(); }

/// Populate friction-tangent preflight without mutating the buffer (B4.4 deepen follow-up pass).
ContactBufferFrictionPreflight preflight_contact_buffer_friction_tangents(const ContactBufferSoA& buffer);

/// Returns true when friction-tangent SoA rebuild should be skipped (B4.4 deepen follow-up pass).
bool should_skip_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer);



/// Why contact-buffer compaction would early-out (B4.4 guard pass).

/// Human-readable label for contact-buffer compaction reject reasons (B4.4 guard pass).

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.4 guard pass).

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.4 guard pass).

/// Read-only compaction diagnostics — no mutation (B4.4 guard pass).



/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.4 guard pass).

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.4 guard pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.4 guard pass).

/// Human-readable label for contact-buffer clamp reject reasons (B4.4 guard pass).

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.4 guard pass).

/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.4 guard pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.4 guard pass).



/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.4 guard pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.4 guard pass).
/// Const preflight for contact-buffer slot write (B4.4 deepen pass follow-up).

    bool can_write() const { return !skipped && !outOfRangeSlot && !invalidManifold && !selfPair; }

/// Populate write preflight without mutating buffer slots (B4.4 deepen pass follow-up).

/// Returns true when `writeSlot` / `writeSlotIfValid` would reject (B4.4 deepen pass follow-up).
/// Non-mutating compaction skip predicate (B4.6 deepen pass).








/// Non-mutating clamp skip predicate (B4.6 deepen pass).








/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.6 deepen pass).




/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.6 deepen pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.6 deepen pass).

/// Human-readable label for contact-buffer compact reject reasons (B4.6 deepen pass).
const char* contact_buffer_compact_reject_reason_name(ContactBufferCompactRejectReason reason);

ContactBufferCompactRejectReason contact_buffer_compact_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_compact_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_compact_rejects_for_reason(



ContactBufferCompactPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_compact(const ContactBufferSoA& buffer);

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compact` (B4.6 deepen pass).
bool should_run_contact_buffer_compact(const ContactBufferSoA& buffer);



























































enum class ContactBufferFrictionBasesRejectReason : u8 {
    AllBuilt,



const char* contact_buffer_friction_bases_reject_reason_name(ContactBufferFrictionBasesRejectReason reason);

/// Diagnose why friction-basis build would skip; vacuously succeeds when build may proceed.
ContactBufferFrictionBasesRejectReason contact_buffer_friction_bases_reject_reason(

/// Returns true when `contact_buffer_friction_bases_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_bases_rejects_for_reason(



    ContactBufferFrictionBasesRejectReason expected);

/// Read-only friction-basis build diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionBasesPreflight {
    ContactBufferFrictionBasesRejectReason reason = ContactBufferFrictionBasesRejectReason::None;
    bool allBuilt = false;

    bool needsBuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }




ContactBufferFrictionBasesPreflight preflight_contact_buffer_friction_bases(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis build skip predicate (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_bases(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis build predicate (B4.6 deepen pass).
bool should_run_contact_buffer_friction_bases(const ContactBufferSoA& buffer);







/// Why contact-buffer compact-and-clamp would early-out (B4.6 narrowphase deepen pass).




/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed.


const char* contact_buffer_compact_and_clamp_reject_reason_name(

















const char* contactBufferCompactAndClampRejectReasonName(ContactBufferCompactAndClampRejectReason reason);

ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(

/// Returns true when `contactBufferCompactAndClampRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferCompactAndClampRejectsForReason(

/// Human-readable label for compact-and-clamp reject reasons (logging / tests).





















bool contactBufferClampRejectsForReason(const ContactBufferSoA& buffer, ContactBufferClampRejectReason expected);








ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(const ContactBufferSoA& buffer);



    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }



    bool needsCompactAndClamp() const {









/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.6 deepen pass).


enum class ContactBufferFrictionBuildRejectReason : u8 {
    NoValidSlots,

/// Human-readable label for contact-buffer friction-build reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_build_reject_reason_name(ContactBufferFrictionBuildRejectReason reason);

/// Diagnose why friction-basis build would skip; vacuously succeeds when build may proceed (B4.6 deepen pass).
ContactBufferFrictionBuildRejectReason contact_buffer_friction_build_reject_reason(const ContactBufferSoA& buffer);

/// Returns true when `contact_buffer_friction_build_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_friction_build_rejects_for_reason(
    ContactBufferFrictionBuildRejectReason expected);

struct ContactBufferFrictionBuildPreflight {
    ContactBufferFrictionBuildRejectReason reason = ContactBufferFrictionBuildRejectReason::None;
    bool noValidSlots = false;

    bool needsFrictionBuild() const { return reason == ContactBufferFrictionBuildRejectReason::None; }

ContactBufferFrictionBuildPreflight preflight_contact_buffer_friction_build(const ContactBufferSoA& buffer);

/// Non-mutating friction-build skip predicate — inverse of `needsFrictionBuild` (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_build(const ContactBufferSoA& buffer);

/// Non-mutating friction-build predicate — mirrors `preflight_contact_buffer_friction_build` (B4.6 deepen pass).
bool should_run_contact_buffer_friction_build(const ContactBufferSoA& buffer);
/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.3 deepen follow-up pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.3 deepen follow-up pass).


































/// Returns true when contact-buffer compaction should be skipped (B4.6 deepen pass).

/// Returns true when contact-buffer compaction should run (B4.6 deepen pass).



















/// Populate write preflight without mutating the buffer (B4.6 deepen follow-up pass).

/// Non-mutating write skip predicate — inverse of `canWrite` (B4.6 deepen follow-up pass).



/// Human-readable label for contact-buffer compaction reject reasons (B4.6 deepen follow-up pass).

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed (B4.6 deepen follow-up pass).




/// Populate compaction preflight without mutating the buffer (B4.6 deepen follow-up pass).

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.6 deepen follow-up pass).


/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen follow-up pass).

/// Human-readable label for contact-buffer clamp reject reasons (B4.6 deepen follow-up pass).

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.6 deepen follow-up pass).

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.6 deepen follow-up pass).

/// Read-only max-capacity clamp diagnostics — no mutation (B4.6 deepen follow-up pass).

/// Populate write preflight without mutating the buffer (B4.5 deepen follow-up pass).

/// Returns true when contact-buffer write should be skipped (B4.5 deepen follow-up pass).

/// Write only when preflight passes; no-op otherwise (B4.5 deepen follow-up pass).

/// Why contact-buffer compaction would early-out (B4.5 deepen follow-up pass).

/// Human-readable label for contact-buffer compaction reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.5 deepen follow-up pass).

/// Returns true when `contact_buffer_compaction_reject_reason` matches `expected` (B4.5 deepen follow-up pass).

/// Const preflight for contact-buffer compaction dispatch (B4.5 deepen follow-up pass).


/// Populate compaction preflight without mutating the buffer (B4.5 deepen follow-up pass).

/// Non-mutating compaction skip predicate — inverse of `needs_compaction` (B4.5 deepen follow-up pass).

/// Non-mutating compaction predicate — mirrors `preflight_contact_buffer_compaction` (B4.5 deepen follow-up pass).

/// Why contact-buffer max-capacity clamp would early-out (B4.5 deepen follow-up pass).

/// Human-readable label for contact-buffer clamp reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.5 deepen follow-up pass).

/// Returns true when `contact_buffer_clamp_reject_reason` matches `expected` (B4.5 deepen follow-up pass).

/// Const preflight for contact-buffer clamp dispatch (B4.5 deepen follow-up pass).


/// Returns true when contact-buffer write should be skipped (B4.6 deepen follow-up pass).



/// Diagnose why compaction would skip; vacuously succeeds when compaction may proceed (B4.6 deepen follow-up pass).


/// Const preflight for contact-buffer compaction (B4.6 deepen follow-up pass).



/// Non-mutating compaction skip predicate (B4.6 deepen follow-up pass).

/// Non-mutating compaction predicate (B4.6 deepen follow-up pass).





/// Const preflight for contact-buffer max-capacity clamp (B4.6 deepen follow-up pass).
















/// Why contact-buffer max-capacity clamp would early-out (B4.3 deepen pass).










/// Const preflight for contact-buffer slot write (B4.5 deepen follow-up pass).








/// Const preflight for contact-buffer compaction (B4.5 deepen follow-up pass).



/// Returns true when contact-buffer compaction should be skipped (B4.5 deepen follow-up pass).

/// Returns true when contact-buffer compaction should run (B4.5 deepen follow-up pass).





/// Const preflight for contact-buffer max-capacity clamp (B4.5 deepen follow-up pass).





/// Returns true when compaction should run (B4.6 deepen pass).











/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.3 deepen follow-up pass).

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.3 deepen follow-up pass).

/// Why contact-buffer compact-and-clamp would early-out (B4.3 deepen follow-up pass).

/// Human-readable label for compact-and-clamp reject reasons (B4.3 deepen follow-up pass).










/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen pass).



/// Human-readable label for compact-and-clamp reject reasons (B4.5 deepen pass).


/// Returns true when `contactBufferCompactAndClampRejectReason` matches `expected` (B4.3 deepen follow-up pass).

/// Read-only compact-and-clamp diagnostics — no mutation (B4.3 deepen follow-up pass).








/// Why contact-buffer compact-and-clamp would early-out (B4.3 deepen pass).























ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.3 deepen follow-up pass).
bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer);

/// Non-mutating compact-and-clamp predicate — mirrors `preflightContactBufferCompactAndClamp` (B4.3 deepen follow-up pass).
bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer);




/// Returns true when contact-buffer clamp should be skipped (B4.6 deepen pass).

/// Returns true when contact-buffer clamp should run (B4.6 deepen pass).





























/// Non-mutating compact-and-clamp predicate — mirrors preflight (B4.6 deepen pass).

/// Write only when preflight allows; no-op otherwise (B4.6 deepen pass).











/// Populate clamp preflight without mutating the buffer (B4.5 deepen follow-up pass).

/// Non-mutating clamp skip predicate — inverse of `needs_clamp` (B4.5 deepen follow-up pass).

/// Non-mutating clamp predicate — mirrors `preflight_contact_buffer_clamp` (B4.5 deepen follow-up pass).

/// Why contact-buffer compact-and-clamp would early-out (B4.5 deepen follow-up pass).

/// Human-readable label for compact-and-clamp reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.5 deepen follow-up pass).

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.5 deepen follow-up pass).

/// Const preflight for contact-buffer compact-and-clamp dispatch (B4.5 deepen follow-up pass).

/// Populate clamp preflight without mutating the buffer (B4.6 deepen follow-up pass).

/// Non-mutating clamp skip predicate (B4.6 deepen follow-up pass).

/// Non-mutating clamp predicate (B4.6 deepen follow-up pass).


/// Human-readable label for contact-buffer compact-and-clamp reject reasons (B4.6 deepen follow-up pass).

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen follow-up pass).

/// Returns true when `contact_buffer_compact_and_clamp_reject_reason` matches `expected` (B4.6 deepen follow-up pass).

/// Const preflight for contact-buffer compact-and-clamp (B4.6 deepen follow-up pass).


ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(








/// Non-mutating compact-and-clamp predicate — mirrors `preflightContactBufferCompactAndClamp` (B4.6 deepen pass).

void writeContactBufferSlotWithPreflight(



/// Returns true when clamp should run (B4.6 deepen pass).









/// Returns true when compact-and-clamp should run (B4.6 deepen pass).

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.5 deepen pass).

/// Returns true when `contactBufferCompactAndClampRejectReason` matches `expected` (B4.5 deepen pass).

/// Read-only compact-and-clamp diagnostics — no mutation (B4.5 deepen pass).



/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.5 deepen pass).

/// Non-mutating compact-and-clamp predicate — mirrors `preflightContactBufferCompactAndClamp` (B4.5 deepen pass).


/// Compact only when preflight allows work; returns active count (B4.6 deepen pass).

/// Compact-and-clamp only when preflight allows work; returns active count (B4.6 deepen pass).
u32 compact_and_clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer);
u32 compactContactBufferWithPreflight(ContactBufferSoA& buffer);

/// Clamp only when preflight allows work; returns active count (B4.6 deepen pass).
u32 clampContactBufferWithPreflight(ContactBufferSoA& buffer);

/// Compact and clamp only when preflight allows work; returns active count (B4.6 deepen pass).
u32 compactAndClampContactBufferWithPreflight(ContactBufferSoA& buffer);



/// Human-readable label for friction-build reject reasons (B4.6 deepen pass).
const char* contactBufferFrictionBuildRejectReasonName(ContactBufferFrictionBuildRejectReason reason);






    AllCached,

/// Human-readable label for friction-basis rebuild reject reasons (B4.6 deepen pass).

/// Diagnose why buildFrictionTangentBases would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
ContactBufferFrictionBuildRejectReason contactBufferFrictionBuildRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferFrictionBuildRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferFrictionBuildRejectsForReason(

/// Read-only friction-build diagnostics — no mutation (B4.6 deepen pass).
    bool noValidContacts = false;

    bool canBuild() const { return reason == ContactBufferFrictionBuildRejectReason::None; }

/// Populate friction-build preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionBuildPreflight preflightContactBufferFrictionBuild(const ContactBufferSoA& buffer);

/// Non-mutating friction-build skip predicate — inverse of `canBuild` (B4.6 deepen pass).
bool canSkipContactBufferFrictionBuild(const ContactBufferSoA& buffer);

/// Build friction tangent bases only when preflight allows (B4.6 deepen pass).
void buildContactBufferFrictionTangentBasesWithPreflight(ContactBufferSoA& buffer);
const char* contact_buffer_friction_basis_reject_reason_name(
    ContactBufferFrictionBasisRejectReason reason);

/// Diagnose why friction-basis rebuild would skip (B4.6 deepen pass).


/// Const preflight for contact-buffer friction-basis rebuild (B4.6 deepen pass).

    bool can_rebuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }

ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_basis(

/// Non-mutating friction-basis skip predicate (B4.6 deepen pass).

/// Returns true when contact-buffer compact-and-clamp should be skipped (B4.6 deepen pass).

/// Returns true when contact-buffer compact-and-clamp should run (B4.6 deepen pass).






/// Human-readable label for friction-bases reject reasons (B4.6 deepen pass).
const char* contactBufferFrictionBasesRejectReasonName(ContactBufferFrictionBasesRejectReason reason);

/// Diagnose why `buildFrictionTangentBases` would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
ContactBufferFrictionBasesRejectReason contactBufferFrictionBasesRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferFrictionBasesRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferFrictionBasesRejectsForReason(

/// Read-only friction-bases diagnostics — no mutation (B4.6 deepen pass).

    bool needsRebuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }



ContactBufferFrictionBasesPreflight preflightContactBufferFrictionBases(const ContactBufferSoA& buffer);

/// Non-mutating friction-bases skip predicate — inverse of `needsRebuild` (B4.6 deepen pass).
bool canSkipContactBufferFrictionBases(const ContactBufferSoA& buffer);

/// Non-mutating friction-bases predicate — mirrors `preflightContactBufferFrictionBases` (B4.6 deepen pass).
bool shouldRunContactBufferFrictionBases(const ContactBufferSoA& buffer);

/// Rebuild friction tangents only when preflight allows; returns false when skipped (B4.6 deepen pass).
bool buildContactBufferFrictionBasesWithPreflight(ContactBufferSoA& buffer);

/// Human-readable label for friction-tangent reject reasons (B4.6 deepen pass).
const char* contact_buffer_friction_tangent_reject_reason_name(



/// Read-only friction-tangent diagnostics — no mutation (B4.6 deepen pass).

    bool needsFrictionTangentBuild() const {
        return reason == ContactBufferFrictionTangentRejectReason::None;

/// Populate friction-tangent preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangents(

/// Non-mutating friction-tangent skip predicate (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_tangent_build(

/// Compact only when preflight allows; returns active count (B4.6 deepen pass).

/// Clamp only when preflight allows; returns active count (B4.6 deepen pass).

/// Compact-and-clamp only when preflight allows; returns active count (B4.6 deepen pass).

/// Build friction tangents only when preflight allows (B4.6 deepen pass).
void build_contact_buffer_friction_tangents_with_preflight(
/// Compact and clamp only when preflight allows; returns active count (B4.6 deepen pass).


/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.6 deepen follow-up pass).



/// Human-readable label for compact-and-clamp reject reasons (B4.6 deepen follow-up pass).





/// Populate compact-and-clamp preflight without mutating the buffer (B4.6 deepen follow-up pass).

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.6 deepen follow-up pass).

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.6 deepen follow-up pass).

/// Compact only when preflight allows; returns active count (B4.6 deepen follow-up pass).

/// Compact and clamp only when preflight allows; returns active count (B4.6 deepen follow-up pass).

/// Populate compact-and-clamp preflight without mutating the buffer (B4.5 deepen follow-up pass).

/// Non-mutating compact-and-clamp skip predicate — inverse of `needs_compact_and_clamp` (B4.5 deepen follow-up pass).

/// Non-mutating compact-and-clamp predicate — mirrors `preflight_contact_buffer_compact_and_clamp` (B4.5 deepen follow-up pass).


/// Non-mutating compact-and-clamp skip predicate (B4.6 deepen follow-up pass).

/// Non-mutating compact-and-clamp predicate (B4.6 deepen follow-up pass).








enum class ContactBufferFrictionRebuildRejectReason : u8 {

const char* contactBufferFrictionRebuildRejectReasonName(ContactBufferFrictionRebuildRejectReason reason);

ContactBufferFrictionRebuildRejectReason contactBufferFrictionRebuildRejectReason(

bool contactBufferFrictionRebuildRejectsForReason(
    ContactBufferFrictionRebuildRejectReason expected,

struct ContactBufferFrictionRebuildPreflight {
    ContactBufferFrictionRebuildRejectReason reason = ContactBufferFrictionRebuildRejectReason::None;

    bool needsRebuild() const { return reason == ContactBufferFrictionRebuildRejectReason::None; }

ContactBufferFrictionRebuildPreflight preflightContactBufferFrictionRebuild(

bool canSkipContactBufferFrictionRebuild(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

bool shouldRunContactBufferFrictionRebuild(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);














/// Returns true when contact-buffer clamp should be skipped (B4.5 deepen follow-up pass).

/// Returns true when contact-buffer clamp should run (B4.5 deepen follow-up pass).





/// Const preflight for contact-buffer compact-and-clamp (B4.5 deepen follow-up pass).



/// Returns true when contact-buffer compact-and-clamp should be skipped (B4.5 deepen follow-up pass).

/// Returns true when contact-buffer compact-and-clamp should run (B4.5 deepen follow-up pass).

u32 clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer);


/// Compact only when preflight allows; returns active count (B4.5 deepen pass).

/// Clamp only when preflight allows; returns active count (B4.5 deepen pass).

/// Compact and clamp only when preflight allows; returns active count (B4.5 deepen pass).








/// Const preflight for contact-buffer friction tangent rebuild (B4.6 deepen pass).
    u32 activeCount = 0u;
    u32 staleCount = 0u;
    u32 rebuildCount = 0u;

    bool can_skip_rebuild() const { return activeCount == 0u || rebuildCount == 0u; }

/// Populate buffer friction preflight without mutating slots (B4.6 deepen pass).
    u32 validSlotCount = 0u;

    bool can_skip_compact_and_clamp() const { return skipped || (!needsCompaction && !needsClamp); }


/// Returns true when compaction would be a no-op (B4.6 deepen pass).

    FrictionBasisRejectReason reason = FrictionBasisRejectReason::None;
    u32 slotsNeedingRebuild = 0u;
    u32 slotsWithStaleBasis = 0u;

    bool can_skip_rebuild() const {
        return skipped || reason != FrictionBasisRejectReason::None || slotsNeedingRebuild == 0u;

/// Populate friction rebuild preflight without mutating the buffer (B4.6 deepen pass).

/// Returns true when buffer friction rebuild should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_friction_rebuild(
/// Const preflight for buffer friction-basis rebuild (B4.6 deepen pass).


/// Populate buffer friction preflight without mutating tangent columns (B4.6 deepen pass).
ContactBufferFrictionPreflight preflight_buffer_friction_rebuild(

bool should_skip_buffer_friction_rebuild(const ContactBufferSoA& buffer, f32 epsilon = 1e-4f);

/// Human-readable label for friction-basis rebuild reject reasons (logging / tests).

/// Diagnose why friction-basis rebuild would skip; vacuously succeeds when rebuild may proceed.
ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(const ContactBufferSoA& buffer);





/// Non-mutating friction-basis rebuild skip predicate — inverse of `needsRebuild` (B4.6 deepen pass).

/// Non-mutating friction-basis rebuild predicate — mirrors `preflight_contact_buffer_friction_basis` (B4.6 deepen pass).










    bool rejected = false;

    bool can_write() const { return !rejected; }

/// Populate slot-write preflight without mutating the buffer (B4.6 deepen pass).

/// Why friction-basis SoA rebuild would early-out (B4.6 deepen pass).
    AllBasesValid,

/// Human-readable label for friction-basis SoA rebuild reject reasons (B4.6 deepen pass).

/// Diagnose why friction-basis SoA rebuild would skip (B4.6 deepen pass).
ContactBufferFrictionBuildRejectReason contact_buffer_friction_build_reject_reason(


/// Read-only friction-basis SoA rebuild diagnostics — no mutation (B4.6 deepen pass).
    bool allBasesValid = false;

    bool needs_build() const { return reason == ContactBufferFrictionBuildRejectReason::None; }

/// Populate friction-basis SoA rebuild preflight without mutating the buffer (B4.6 deepen pass).

/// Non-mutating skip predicate — inverse of `needs_build` (B4.6 deepen pass).

/// Non-mutating rebuild predicate — mirrors `preflight_contact_buffer_friction_build` (B4.6 deepen pass).

    bool can_skip_rebuild() const { return staleCount == 0u; }
    bool needs_rebuild() const { return staleCount > 0u; }

/// Populate friction tangent rebuild preflight without mutating slots (B4.6 deepen pass).

/// Returns true when all active slots already store orthonormal tangent frames (B4.6 deepen pass).



/// Write only when preflight allows; no-op otherwise (B4.6 deepen follow-up pass).
bool writeSlotWithPreflight(u32 slot, const ContactManifold& manifold, ContactBufferSoA& buffer);









/// Compaction only when preflight allows; returns active count (B4.6 deepen follow-up pass).
u32 compactWithPreflight(ContactBufferSoA& buffer);









/// Clamp only when preflight allows; returns active count (B4.6 deepen follow-up pass).
u32 applyMaxCapacityClampWithPreflight(ContactBufferSoA& buffer);









u32 compactAndClampWithPreflight(ContactBufferSoA& buffer);





/// Why contact-buffer friction-basis rebuild would early-out (B4.5 deepen pass).

const char* contact_buffer_friction_rebuild_reject_reason_name(ContactBufferFrictionRebuildRejectReason reason);

ContactBufferFrictionRebuildRejectReason contact_buffer_friction_rebuild_reject_reason(

bool contact_buffer_friction_rebuild_rejects_for_reason(
    ContactBufferFrictionRebuildRejectReason expected);


    bool can_rebuild() const { return reason == ContactBufferFrictionRebuildRejectReason::None; }

ContactBufferFrictionRebuildPreflight preflight_contact_buffer_friction_rebuild(

bool can_skip_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer);
bool should_run_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer);












/// Read-only compaction diagnostics — no mutation (B4.5 deepen follow-up pass).



/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.5 deepen follow-up pass).






/// Read-only max-capacity clamp diagnostics — no mutation (B4.5 deepen follow-up pass).



/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.5 deepen follow-up pass).






/// Read-only compact-and-clamp diagnostics — no mutation (B4.5 deepen follow-up pass).



/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.5 deepen follow-up pass).







/// Compact only when preflight reports compaction is needed (B4.6 deepen pass).
u32 compact_contact_buffer_if_needed(ContactBufferSoA& buffer);

/// Compact and clamp only when preflight reports work is needed (B4.6 deepen pass).
u32 compact_and_clamp_contact_buffer_if_needed(ContactBufferSoA& buffer);
/// Const preflight for contact-buffer compaction and clamp (B4.6 deepen pass).
struct ContactBufferPreflight {
    u32 pairSlotCount = 0u;
    bool canSkipCompaction = true;
    bool canSkipClamp = true;
    bool canSkipCompactAndClamp = true;

    bool can_iterate() const { return pairSlotCount > 0u || validSlotCount > 0u; }

/// Populate contact-buffer preflight without mutating slots (B4.6 deepen pass).
ContactBufferPreflight preflight_contact_buffer(const ContactBufferSoA& buffer);

/// Returns true when contact-buffer iteration can be skipped (B4.6 deepen pass).
bool should_skip_contact_buffer_iteration(const ContactBufferSoA& buffer);






/// Human-readable label for friction-basis build reject reasons (B4.6 deepen pass).
const char* contactBufferFrictionBasisRejectReasonName(ContactBufferFrictionBasisRejectReason reason);

ContactBufferFrictionBasisRejectReason contactBufferFrictionBasisRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferFrictionBasisRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferFrictionBasisRejectsForReason(


    bool needsFrictionBasisBuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }

ContactBufferFrictionBasisPreflight preflightContactBufferFrictionBasis(const ContactBufferSoA& buffer);

bool canSkipContactBufferFrictionBasisBuild(const ContactBufferSoA& buffer);

bool shouldRunContactBufferFrictionBasisBuild(const ContactBufferSoA& buffer);

/// Build friction tangents only when preflight allows; no-op otherwise (B4.6 deepen pass).
void buildFrictionTangentBasesWithPreflight(ContactBufferSoA& buffer);

    bool allCached = false;




/// Non-mutating friction-build predicate — mirrors `preflightContactBufferFrictionBuild` (B4.6 deepen pass).
bool shouldRunContactBufferFrictionBuild(const ContactBufferSoA& buffer);

/// Why contact-buffer warm-start stub would early-out (B4.6 deepen pass).
enum class ContactBufferWarmStartRejectReason : u8 {
    OutOfRangeSlot,
    InvalidSlot,

/// Human-readable label for warm-start reject reasons (B4.6 deepen pass).
const char* contactBufferWarmStartRejectReasonName(ContactBufferWarmStartRejectReason reason);

/// Diagnose why applyWarmStartStub would skip; vacuously succeeds when warm-start may proceed (B4.6 deepen pass).
ContactBufferWarmStartRejectReason contactBufferWarmStartRejectReason(
    u32 slot);

/// Returns true when `contactBufferWarmStartRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferWarmStartRejectsForReason(
    ContactBufferWarmStartRejectReason expected);

/// Read-only warm-start diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferWarmStartPreflight {
    ContactBufferWarmStartRejectReason reason = ContactBufferWarmStartRejectReason::None;

    bool canWarmStart() const { return reason == ContactBufferWarmStartRejectReason::None; }

ContactBufferWarmStartPreflight preflightContactBufferWarmStart(

/// Non-mutating warm-start skip predicate — inverse of `canWarmStart` (B4.6 deepen pass).
bool canSkipContactBufferWarmStart(const ContactBufferSoA& buffer, u32 slot);

/// Non-mutating warm-start predicate — mirrors `preflightContactBufferWarmStart` (B4.6 deepen pass).
bool shouldRunContactBufferWarmStart(const ContactBufferSoA& buffer, u32 slot);


const char* contactBufferToVectorRejectReasonName(ContactBufferToVectorRejectReason reason);







/// Human-readable label for contact-buffer toVector reject reasons (logging / tests).

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed.
ContactBufferToVectorRejectReason contactBufferToVectorRejectReason(const ContactBufferSoA& buffer);

/// Returns true when `contactBufferToVectorRejectReason` matches `expected` (B4.6 deepen pass).
bool contactBufferToVectorRejectsForReason(


    bool canExport() const { return reason == ContactBufferToVectorRejectReason::None; }


ContactBufferToVectorPreflight preflightContactBufferToVector(const ContactBufferSoA& buffer);

/// Non-mutating toVector skip predicate — inverse of `canExport` (B4.6 deepen pass).
bool canSkipContactBufferToVector(const ContactBufferSoA& buffer);

/// Non-mutating toVector predicate — mirrors `preflightContactBufferToVector` (B4.6 deepen pass).
bool shouldRunContactBufferToVector(const ContactBufferSoA& buffer);

/// Const preflight for contact-buffer friction-basis build (B4.6 deepen pass).

    bool needs_friction_basis_build() const {
        return reason == ContactBufferFrictionBasisRejectReason::None;


/// Returns true when buildFrictionTangentBases should be skipped (B4.6 deepen pass).

/// Human-readable label for friction-basis build reject reasons (logging / tests).

/// Diagnose why buildFrictionTangentBases would skip; vacuously succeeds when build may proceed.



    bool canBuild() const { return reason == ContactBufferFrictionBasisRejectReason::None; }


/// Non-mutating friction-basis build skip predicate — inverse of `canBuild` (B4.6 deepen pass).
bool canSkipContactBufferFrictionBasis(const ContactBufferSoA& buffer);

/// Non-mutating friction-basis build predicate — mirrors `preflightContactBufferFrictionBasis` (B4.6 deepen pass).
bool shouldRunContactBufferFrictionBasis(const ContactBufferSoA& buffer);

/// Write slot only when preflight allows; returns false when skipped (B4.6 deepen pass).


void buildContactBufferFrictionBasesWithPreflight(ContactBufferSoA& buffer);
/// Returns true when `contact_buffer_write_slot_reject_reason` matches `expected` (B4.5 deepen follow-up pass).

/// Const preflight for contact-buffer write-slot dispatch (B4.5 deepen follow-up pass).


/// Populate write-slot preflight without mutating the buffer (B4.5 deepen follow-up pass).

/// Non-mutating write-slot skip predicate — inverse of `canWrite` (B4.5 deepen follow-up pass).

/// Non-mutating write-slot predicate — mirrors `preflight_contact_buffer_write_slot` (B4.5 deepen follow-up pass).



/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed (B4.5 deepen follow-up pass).











/// Const preflight for contact-buffer max-capacity clamp dispatch (B4.5 deepen follow-up pass).






/// Human-readable label for contact-buffer compact-and-clamp reject reasons (B4.5 deepen follow-up pass).








/// Why contact-buffer toVector would early-out (B4.5 deepen follow-up pass).

/// Human-readable label for contact-buffer toVector reject reasons (B4.5 deepen follow-up pass).

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed (B4.5 deepen follow-up pass).

/// Returns true when `contact_buffer_to_vector_reject_reason` matches `expected` (B4.5 deepen follow-up pass).

/// Const preflight for contact-buffer export dispatch (B4.5 deepen follow-up pass).


/// Populate toVector preflight without mutating the buffer (B4.5 deepen follow-up pass).

/// Non-mutating toVector skip predicate — inverse of `canExport` (B4.5 deepen follow-up pass).

/// Non-mutating toVector predicate — mirrors `preflight_contact_buffer_to_vector` (B4.5 deepen follow-up pass).
bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer);
/// Returns true when toVector may export contacts (B4.6 deepen pass).


/// Human-readable label for contact-buffer warm-start reject reasons (B4.6 deepen pass).
const char* contact_buffer_warm_start_reject_reason_name(ContactBufferWarmStartRejectReason reason);

/// Diagnose why applyWarmStartStub would skip; vacuously succeeds when copy may proceed (B4.6 deepen pass).
ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(

/// Returns true when `contact_buffer_warm_start_reject_reason` matches `expected` (B4.6 deepen pass).
bool contact_buffer_warm_start_rejects_for_reason(

/// Const preflight for contact-buffer warm-start stub (B4.6 deepen pass).

    bool can_apply() const { return reason == ContactBufferWarmStartRejectReason::None; }

/// Populate warm-start preflight without mutating the manifold (B4.6 deepen pass).
ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(

/// Returns true when warm-start stub should be skipped (B4.6 deepen pass).
bool can_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

/// Returns true when warm-start stub may proceed (B4.6 deepen pass).
bool should_run_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);


/// Human-readable label for contact-buffer friction-bases reject reasons (B4.6 deepen pass).




    bool needs_rebuild() const { return reason == ContactBufferFrictionBasesRejectReason::None; }

/// Populate friction-bases preflight without mutating the buffer (B4.6 deepen pass).
ContactBufferFrictionBasesPreflight preflight_contact_buffer_friction_bases(


/// Returns true when buildFrictionTangentBases may proceed (B4.6 deepen pass).
FUSE_PHYSICS_INLINE const char* contact_buffer_to_vector_reject_reason_name(
    ContactBufferToVectorRejectReason reason);

FUSE_PHYSICS_INLINE ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(

FUSE_PHYSICS_INLINE bool contact_buffer_to_vector_rejects_for_reason(



FUSE_PHYSICS_INLINE ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(

/// Non-mutating toVector skip predicate (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer);

/// Non-mutating toVector predicate (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer);
/// Non-mutating toVector skip predicate — inverse of `can_export` (B4.6 deepen pass).

/// Non-mutating toVector predicate — mirrors `preflight_contact_buffer_to_vector` (B4.6 deepen pass).

/// Write slot only when preflight allows; no-op otherwise (B4.6 deepen pass).




/// Rebuild friction tangents only when preflight allows (B4.6 deepen pass).

// --- Inline guard implementations (B4.6 deepen pass) ---

FUSE_PHYSICS_INLINE bool ContactBufferSoA::slotIsValid(u32 slot) const {
    return slot < validFlags.size() && validFlags[slot] != 0u;

FUSE_PHYSICS_INLINE u32 ContactBufferSoA::countValidSlots() const {
    if (canSkipSoAIteration()) {
        return 0u;

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 validCount = 0u;
    for (u32 slot = 0u; slot < scanCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
    return validCount;

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canSkipCompaction() const {
        return true;

    if (scanCount == 0u) {

        if (validFlags[slot] == 0u) {
            return false;

FUSE_PHYSICS_INLINE u32 ContactBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canAcceptContacts(u32 additionalCount) const {
    if (additionalCount == 0u) {
    return activeCount + additionalCount <= maxCapacity;

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;


        if (slot < validFlags.size() && validFlags[slot] != 0u) {


    const u32 scanCount =
        activeCount > 0u ? activeCount : (pairSlotCount > 0u ? pairSlotCount : 0u);

        if (slot >= validFlags.size() || validFlags[slot] == 0u) {

    if (slot >= validFlags.size()) {
    return validFlags[slot] != 0u;

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canSkipFrictionTangentRebuild() const {

    const u32 scanCount = activeCount > 0u ? activeCount : pairSlotCount;
        if (!slotIsValid(slot)) {
            continue;
        if (slot >= contactNormals.size()) {
        const TangentBasis basis{tangent1[slot], tangent2[slot]};
        if (!isOrthonormalTangentBasis(contactNormals[slot], basis)) {


FUSE_PHYSICS_INLINE bool ContactBufferSoA::canSkipCompactAndClamp() const {
    return !should_run_contact_buffer_compaction(*this) && !should_run_contact_buffer_clamp(*this) &&
           countValidSlots() == activeCount;

    ContactBufferWriteSlotRejectReason reason) {
FUSE_PHYSICS_INLINE const char* contactBufferWriteSlotRejectReasonName(ContactBufferWriteSlotRejectReason reason) {


/// Non-mutating compact-and-clamp skip predicate — inverse of `needs_compact_and_clamp` (B4.6 deepen follow-up pass).


/// Compact and clamp only when preflight allows; returns active count or zero when skipped (B4.6 deepen follow-up pass).

/// Why contact-buffer toVector would early-out (B4.6 deepen follow-up pass).



/// Returns true when `contact_buffer_to_vector_reject_reason` matches `expected` (B4.6 deepen follow-up pass).

/// Read-only toVector diagnostics — no mutation (B4.6 deepen follow-up pass).



/// Non-mutating toVector skip predicate — inverse of `can_export` (B4.6 deepen follow-up pass).

/// Non-mutating toVector predicate — mirrors `preflight_contact_buffer_to_vector` (B4.6 deepen follow-up pass).

/// Export only when preflight allows; returns empty vector when skipped (B4.6 deepen follow-up pass).
std::vector<ContactManifold> to_vector_contact_buffer_with_preflight(const ContactBufferSoA& buffer);

/// Why contact-buffer manifoldAt would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferManifoldAtRejectReason : u8 {
    OutOfRangeIndex,

/// Human-readable label for contact-buffer manifoldAt reject reasons (logging / tests).
const char* contact_buffer_manifold_at_reject_reason_name(ContactBufferManifoldAtRejectReason reason);

/// Diagnose why manifoldAt would return invalid; vacuously succeeds when read may proceed.
ContactBufferManifoldAtRejectReason contact_buffer_manifold_at_reject_reason(
    u32 index);

/// Read-only manifoldAt diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferManifoldAtPreflight {
    ContactBufferManifoldAtRejectReason reason = ContactBufferManifoldAtRejectReason::None;
    bool outOfRangeIndex = false;

    bool can_read() const { return reason == ContactBufferManifoldAtRejectReason::None; }

ContactBufferManifoldAtPreflight preflight_contact_buffer_manifold_at(

/// Non-mutating manifoldAt skip predicate — inverse of `can_read` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_manifold_at(const ContactBufferSoA& buffer, u32 index);

/// Why contact-buffer warm-start apply would early-out (B4.6 deepen follow-up pass).

/// Human-readable label for contact-buffer warm-start reject reasons (logging / tests).

/// Diagnose why applyWarmStartStub would no-op; vacuously succeeds when apply may proceed.

/// Read-only warm-start diagnostics — no mutation (B4.6 deepen follow-up pass).



/// Non-mutating warm-start skip predicate — inverse of `can_apply` (B4.6 deepen follow-up pass).

/// Apply warm-start only when preflight allows; returns false when skipped (B4.6 deepen follow-up pass).
bool apply_contact_buffer_warm_start_with_preflight(
    ContactManifold& manifold);

/// Why contact-buffer tangent-basis build would early-out (B4.6 deepen follow-up pass).
enum class ContactBufferTangentBasisRejectReason : u8 {

/// Human-readable label for contact-buffer tangent-basis reject reasons (logging / tests).
const char* contact_buffer_tangent_basis_reject_reason_name(ContactBufferTangentBasisRejectReason reason);

/// Diagnose why buildFrictionTangentBases would no-op; vacuously succeeds when build may proceed.
ContactBufferTangentBasisRejectReason contact_buffer_tangent_basis_reject_reason(const ContactBufferSoA& buffer);

/// Read-only tangent-basis diagnostics — no mutation (B4.6 deepen follow-up pass).
struct ContactBufferTangentBasisPreflight {
    ContactBufferTangentBasisRejectReason reason = ContactBufferTangentBasisRejectReason::None;

    bool can_build() const { return reason == ContactBufferTangentBasisRejectReason::None; }

ContactBufferTangentBasisPreflight preflight_contact_buffer_tangent_basis(const ContactBufferSoA& buffer);

/// Non-mutating tangent-basis skip predicate — inverse of `can_build` (B4.6 deepen follow-up pass).
bool can_skip_contact_buffer_build_friction_tangent_bases(const ContactBufferSoA& buffer);

/// Build tangent bases only when preflight allows; returns false when skipped (B4.6 deepen follow-up pass).
bool build_contact_buffer_friction_tangent_bases_with_preflight(ContactBufferSoA& buffer);

FUSE_PHYSICS_INLINE const char* contact_buffer_write_slot_reject_reason_name(
    switch (reason) {
    case ContactBufferWriteSlotRejectReason::None:
        return "None";
    case ContactBufferWriteSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case ContactBufferWriteSlotRejectReason::InvalidManifold:
        return "InvalidManifold";
    case ContactBufferWriteSlotRejectReason::SelfPair:
        return "SelfPair";
    return "Unknown";

FUSE_PHYSICS_INLINE ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
    const ContactManifold& manifold) {
    if (slot >= buffer.pairSlotCount) {
        return ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    if (!manifold.valid) {
        return ContactBufferWriteSlotRejectReason::InvalidManifold;
    if (manifold.bodyA == manifold.bodyB) {
        return ContactBufferWriteSlotRejectReason::SelfPair;
    return ContactBufferWriteSlotRejectReason::None;

    ContactBufferWriteSlotRejectReason expected) {
    return contact_buffer_write_slot_reject_reason(buffer, slot, manifold) == expected;

    ContactBufferWriteSlotPreflight preflight{};
    preflight.reason = contact_buffer_write_slot_reject_reason(buffer, slot, manifold);
    preflight.outOfRangeSlot =
        preflight.reason == ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    preflight.invalidManifold =
        preflight.reason == ContactBufferWriteSlotRejectReason::InvalidManifold;
    preflight.selfPair = preflight.reason == ContactBufferWriteSlotRejectReason::SelfPair;
    return preflight;

    return !preflight_contact_buffer_write_slot(buffer, slot, manifold).can_write();

    return preflight_contact_buffer_write_slot(buffer, slot, manifold).can_write();

    ContactBufferCompactionRejectReason reason) {
    case ContactBufferCompactionRejectReason::None:

    if (!manifold.valid || manifold.bodyA == manifold.bodyB) {


    preflight.outOfRangeSlot = preflight.reason == ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    preflight.invalidManifold = preflight.reason == ContactBufferWriteSlotRejectReason::InvalidManifold;




/// Diagnose why writeSlot would reject; vacuously succeeds when write may proceed (B4.6 deepen pass).
FUSE_PHYSICS_INLINE ContactBufferWriteSlotRejectReason contactBufferWriteSlotRejectReason(

FUSE_PHYSICS_INLINE bool contactBufferWriteSlotRejectsForReason(
    return contactBufferWriteSlotRejectReason(buffer, slot, manifold) == expected;



FUSE_PHYSICS_INLINE ContactBufferWriteSlotPreflight preflightContactBufferWriteSlot(
    preflight.reason = contactBufferWriteSlotRejectReason(buffer, slot, manifold);

FUSE_PHYSICS_INLINE bool canSkipContactBufferWriteSlot(
    return !preflightContactBufferWriteSlot(buffer, slot, manifold).canWrite();

FUSE_PHYSICS_INLINE bool shouldRunContactBufferWriteSlot(
    return preflightContactBufferWriteSlot(buffer, slot, manifold).canWrite();






/// Why contact-buffer compaction would early-out (B4.6 deepen pass).
enum class ContactBufferCompactionRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    AllValid,

/// Human-readable label for contact-buffer compaction reject reasons (B4.6 deepen pass).
FUSE_PHYSICS_INLINE const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason) {






FUSE_PHYSICS_INLINE bool write_contact_buffer_slot_with_preflight(
    ContactBufferSoA& buffer,
    if (!should_run_contact_buffer_write_slot(buffer, slot, manifold)) {
        return false;
    buffer.writeSlot(slot, manifold);
    return true;

        return "None";
    case ContactBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactionRejectReason::AllValid:
        return "AllValid";
    }
    return "Unknown";

FUSE_PHYSICS_INLINE ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;
    if (buffer.canSkipCompaction()) {
        return ContactBufferCompactionRejectReason::AllValid;
    return ContactBufferCompactionRejectReason::None;

FUSE_PHYSICS_INLINE bool contact_buffer_compaction_rejects_for_reason(
    ContactBufferCompactionRejectReason expected) {
    return contact_buffer_compaction_reject_reason(buffer) == expected;

FUSE_PHYSICS_INLINE ContactBufferCompactionPreflight preflight_contact_buffer_compaction(

    const ContactBufferSoA& buffer,

    if (buffer.countValidSlots() == 0u) {


    ContactBufferCompactionPreflight preflight{};
    preflight.reason = contact_buffer_compaction_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compaction(buffer).needs_compaction();

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compaction(buffer).needs_compaction();

FUSE_PHYSICS_INLINE const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason) {
    case ContactBufferClampRejectReason::None:
    case ContactBufferClampRejectReason::EmptyBuffer:
    case ContactBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";

FUSE_PHYSICS_INLINE ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(
        return ContactBufferClampRejectReason::EmptyBuffer;
    if (!buffer.canApplyMaxCapacityClamp()) {
        return ContactBufferClampRejectReason::WithinCapacity;
    return ContactBufferClampRejectReason::None;

FUSE_PHYSICS_INLINE bool contact_buffer_clamp_rejects_for_reason(
    ContactBufferClampRejectReason expected) {
    return contact_buffer_clamp_reject_reason(buffer) == expected;
    return preflight;



    switch (reason) {



FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.reason = contact_buffer_clamp_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_clamp(buffer).needs_clamp();

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_clamp(buffer).needs_clamp();

FUSE_PHYSICS_INLINE const char* contact_buffer_friction_tangent_reject_reason_name(
    ContactBufferFrictionTangentRejectReason reason) {
    case ContactBufferFrictionTangentRejectReason::None:
    case ContactBufferFrictionTangentRejectReason::EmptyBuffer:
    case ContactBufferFrictionTangentRejectReason::AllOrthonormal:
        return "AllOrthonormal";

FUSE_PHYSICS_INLINE ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(
        return ContactBufferFrictionTangentRejectReason::EmptyBuffer;
    if (buffer.canSkipFrictionTangentRebuild()) {
        return ContactBufferFrictionTangentRejectReason::AllOrthonormal;
    return ContactBufferFrictionTangentRejectReason::None;

FUSE_PHYSICS_INLINE bool contact_buffer_friction_tangent_rejects_for_reason(
    ContactBufferFrictionTangentRejectReason expected) {
    return contact_buffer_friction_tangent_reject_reason(buffer) == expected;

FUSE_PHYSICS_INLINE ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent_rebuild(
    ContactBufferFrictionTangentPreflight preflight{};
    preflight.reason = contact_buffer_friction_tangent_reject_reason(buffer);
    preflight.emptyBuffer =
        preflight.reason == ContactBufferFrictionTangentRejectReason::EmptyBuffer;
    preflight.allOrthonormal =
        preflight.reason == ContactBufferFrictionTangentRejectReason::AllOrthonormal;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_friction_tangent_rebuild(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_friction_tangent_rebuild(buffer).needs_rebuild();

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_friction_tangent_rebuild(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_friction_tangent_rebuild(buffer).needs_rebuild();

FUSE_PHYSICS_INLINE const char* contact_buffer_compact_and_clamp_reject_reason_name(
    ContactBufferCompactAndClampRejectReason reason) {
    case ContactBufferCompactAndClampRejectReason::None:
    case ContactBufferCompactAndClampRejectReason::EmptyBuffer:
    case ContactBufferCompactAndClampRejectReason::NoWork:
        return "NoWork";

FUSE_PHYSICS_INLINE ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
        return ContactBufferCompactAndClampRejectReason::EmptyBuffer;




    if (!should_run_contact_buffer_compaction(buffer) && !should_run_contact_buffer_clamp(buffer)) {
        const u32 validCount = buffer.countValidSlots();
        if (buffer.pairSlotCount > 0u && buffer.activeCount != validCount) {
            return ContactBufferCompactAndClampRejectReason::None;
        if (buffer.activeCount != validCount) {
        return ContactBufferCompactAndClampRejectReason::NoWork;

FUSE_PHYSICS_INLINE bool contact_buffer_compact_and_clamp_rejects_for_reason(
    ContactBufferCompactAndClampRejectReason expected) {
    return contact_buffer_compact_and_clamp_reject_reason(buffer) == expected;

FUSE_PHYSICS_INLINE ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
    ContactBufferCompactAndClampPreflight preflight{};
    preflight.reason = contact_buffer_compact_and_clamp_reject_reason(buffer);
        preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compact_and_clamp(buffer).needs_compact_and_clamp();

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compact_and_clamp(buffer).needs_compact_and_clamp();

    ContactBufferToVectorRejectReason reason) {
    case ContactBufferToVectorRejectReason::None:
    case ContactBufferToVectorRejectReason::EmptyBuffer:

        return ContactBufferToVectorRejectReason::EmptyBuffer;
    return ContactBufferToVectorRejectReason::None;

    ContactBufferToVectorRejectReason expected) {
    return contact_buffer_to_vector_reject_reason(buffer) == expected;

    ContactBufferToVectorPreflight preflight{};
    preflight.reason = contact_buffer_to_vector_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferToVectorRejectReason::EmptyBuffer;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_to_vector(buffer).can_export();

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_to_vector(buffer).can_export();


    preflight.emptyBuffer = preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;



    case ContactBufferFrictionTangentRejectReason::AllValid:

    f32 epsilon) {
    if (buffer.canSkipSoAIteration() || buffer.activeCount == 0u) {
    if (buffer.canSkipFrictionTangentRebuild(epsilon)) {
        return ContactBufferFrictionTangentRejectReason::AllValid;

    ContactBufferFrictionTangentRejectReason expected,
    return contact_buffer_friction_tangent_reject_reason(buffer, epsilon) == expected;

    preflight.reason = contact_buffer_friction_tangent_reject_reason(buffer, epsilon);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionTangentRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferFrictionTangentRejectReason::AllValid;

FUSE_PHYSICS_INLINE bool ContactBufferSoA::canSkipFrictionTangentRebuild(f32 epsilon) const {
    if (canSkipSoAIteration() || activeCount == 0u) {

    for (u32 slot = 0u; slot < activeCount; ++slot) {
        if (validFlags[slot] == 0u) {
            continue;
        if (!isOrthonormalTangentBasis(contactNormals[slot], {tangent1[slot], tangent2[slot]}, epsilon)) {
    return activeCount > 0u;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_friction_tangent_rebuild(
    const ContactBufferFrictionTangentRejectReason reason =
        contact_buffer_friction_tangent_reject_reason(buffer, epsilon);
    return reason != ContactBufferFrictionTangentRejectReason::None;

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_friction_tangent_rebuild(
    return preflight_contact_buffer_friction_tangent_rebuild(buffer, epsilon).needs_rebuild();

FUSE_PHYSICS_INLINE const char* contact_buffer_to_vector_reject_reason_name(

FUSE_PHYSICS_INLINE ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(

FUSE_PHYSICS_INLINE bool contact_buffer_to_vector_rejects_for_reason(

FUSE_PHYSICS_INLINE ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(



FUSE_PHYSICS_INLINE void write_contact_buffer_slot_with_preflight(
    u32 slot,
    const ContactManifold& manifold) {
        return;



FUSE_PHYSICS_INLINE u32 compact_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    const ContactBufferCompactionPreflight preflight = preflight_contact_buffer_compaction(buffer);
    if (preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer) {
        buffer.activeCount = 0u;
        buffer.pairSlotCount = 0u;
        return buffer.activeCount;
    if (preflight.reason == ContactBufferCompactionRejectReason::AllValid) {
        buffer.activeCount = buffer.pairSlotCount > 0u ? buffer.pairSlotCount : buffer.activeCount;
        buffer.pairSlotCount = buffer.activeCount;
    return buffer.compact();

FUSE_PHYSICS_INLINE u32 clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    if (!should_run_contact_buffer_clamp(buffer)) {
    return buffer.applyMaxCapacityClamp();

FUSE_PHYSICS_INLINE u32 compact_and_clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    const ContactBufferCompactAndClampPreflight preflight = preflight_contact_buffer_compact_and_clamp(buffer);
    if (preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer) {
    if (preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork) {
    return buffer.compactAndClamp();

FUSE_PHYSICS_INLINE void build_contact_buffer_friction_tangents_with_preflight(
    if (!should_run_contact_buffer_friction_tangent_rebuild(buffer, epsilon)) {
    buffer.buildFrictionTangentBases();

/// Diagnose why compaction would skip its scan loop; vacuously succeeds when compaction may proceed (B4.6 deepen pass).
FUSE_PHYSICS_INLINE ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(

/// Returns true when `contactBufferCompactionRejectReason` matches `expected` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool contactBufferCompactionRejectsForReason(
    return contactBufferCompactionRejectReason(buffer) == expected;

/// Read-only compaction diagnostics — no mutation (B4.6 deepen pass).
};

const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason);

ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer);

bool contact_buffer_compaction_rejects_for_reason(
    ContactBufferCompactionRejectReason expected);

struct ContactBufferCompactionPreflight {
    ContactBufferCompactionRejectReason reason = ContactBufferCompactionRejectReason::None;
    bool emptyBuffer = false;
    bool allValid = false;

    bool needsCompaction() const { return reason == ContactBufferCompactionRejectReason::None; }
};

FUSE_PHYSICS_INLINE ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferCompactionRejectReason(buffer);

/// Non-mutating compaction skip predicate — inverse of `needsCompaction` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer) {
    return !preflightContactBufferCompaction(buffer).needsCompaction();

/// Non-mutating compaction predicate — mirrors `preflightContactBufferCompaction` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer) {
    return preflightContactBufferCompaction(buffer).needsCompaction();
    bool needs_compaction() const { return reason == ContactBufferCompactionRejectReason::None; }

ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer);

bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer);

/// Why contact-buffer max-capacity clamp would early-out (B4.6 deepen pass).
enum class ContactBufferClampRejectReason : u8 {
    None = 0,
    EmptyBuffer,
    WithinCapacity,

/// Human-readable label for contact-buffer clamp reject reasons (B4.6 deepen pass).
FUSE_PHYSICS_INLINE const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason) {

/// Diagnose why clamp would skip; vacuously succeeds when clamp may proceed (B4.6 deepen pass).
FUSE_PHYSICS_INLINE ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer) {

/// Returns true when `contactBufferClampRejectReason` matches `expected` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool contactBufferClampRejectsForReason(
    return contactBufferClampRejectReason(buffer) == expected;

/// Read-only max-capacity clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferClampPreflight {
    ContactBufferClampRejectReason reason = ContactBufferClampRejectReason::None;
    bool withinCapacity = false;

    bool needsClamp() const { return reason == ContactBufferClampRejectReason::None; }

FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferClampRejectReason(buffer);

/// Non-mutating clamp skip predicate — inverse of `needsClamp` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipContactBufferClamp(const ContactBufferSoA& buffer) {
    return !preflightContactBufferClamp(buffer).needsClamp();

/// Non-mutating clamp predicate — mirrors `preflightContactBufferClamp` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer) {
    return preflightContactBufferClamp(buffer).needsClamp();

namespace detail {

FUSE_PHYSICS_INLINE bool contactBufferSlotHasCachedFrictionBasis(const ContactBufferSoA& buffer, u32 slot) {
    if (slot >= buffer.activeCount || buffer.validFlags[slot] == 0u) {
    const TangentBasis basis{buffer.tangent1[slot], buffer.tangent2[slot]};
    return isOrthonormalTangentBasis(buffer.contactNormals[slot], basis);

FUSE_PHYSICS_INLINE bool contactBufferNeedsFrictionBuild(const ContactBufferSoA& buffer) {
    for (u32 slot = 0u; slot < buffer.activeCount; ++slot) {
        if (buffer.validFlags[slot] == 0u) {
        if (!contactBufferSlotHasCachedFrictionBasis(buffer, slot)) {

} // namespace detail

/// Why contact-buffer compact-and-clamp would early-out (B4.6 deepen pass).
enum class ContactBufferCompactAndClampRejectReason : u8 {
    NoWork,

/// Human-readable label for compact-and-clamp reject reasons (B4.6 deepen pass).
FUSE_PHYSICS_INLINE const char* contactBufferCompactAndClampRejectReasonName(

/// Diagnose why compact-and-clamp would skip; vacuously succeeds when work may proceed (B4.6 deepen pass).
FUSE_PHYSICS_INLINE ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(
    if (!shouldRunContactBufferCompaction(buffer) && !shouldRunContactBufferClamp(buffer)) {

/// Returns true when `contactBufferCompactAndClampRejectReason` matches `expected` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool contactBufferCompactAndClampRejectsForReason(
    return contactBufferCompactAndClampRejectReason(buffer) == expected;

/// Read-only compact-and-clamp diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferCompactAndClampPreflight {
    ContactBufferCompactAndClampRejectReason reason = ContactBufferCompactAndClampRejectReason::None;
    bool noWork = false;

    bool needsCompactAndClamp() const { return reason == ContactBufferCompactAndClampRejectReason::None; }

FUSE_PHYSICS_INLINE ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(
    preflight.reason = contactBufferCompactAndClampRejectReason(buffer);

/// Non-mutating compact-and-clamp skip predicate — inverse of `needsCompactAndClamp` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    return !preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();

/// Non-mutating compact-and-clamp predicate — mirrors `preflightContactBufferCompactAndClamp` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    return preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();

/// Why contact-buffer friction-basis rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionBuildRejectReason : u8 {
    AllCached,

/// Human-readable label for friction-basis rebuild reject reasons (B4.6 deepen pass).
FUSE_PHYSICS_INLINE const char* contactBufferFrictionBuildRejectReasonName(
    ContactBufferFrictionBuildRejectReason reason) {
    case ContactBufferFrictionBuildRejectReason::None:
    case ContactBufferFrictionBuildRejectReason::EmptyBuffer:
    case ContactBufferFrictionBuildRejectReason::AllCached:
        return "AllCached";

/// Diagnose why buildFrictionTangentBases would skip; vacuously succeeds when rebuild may proceed (B4.6 deepen pass).
FUSE_PHYSICS_INLINE ContactBufferFrictionBuildRejectReason contactBufferFrictionBuildRejectReason(
        return ContactBufferFrictionBuildRejectReason::EmptyBuffer;
    if (!detail::contactBufferNeedsFrictionBuild(buffer)) {
        return ContactBufferFrictionBuildRejectReason::AllCached;
    return ContactBufferFrictionBuildRejectReason::None;

/// Returns true when `contactBufferFrictionBuildRejectReason` matches `expected` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool contactBufferFrictionBuildRejectsForReason(
    ContactBufferFrictionBuildRejectReason expected) {
    return contactBufferFrictionBuildRejectReason(buffer) == expected;

/// Read-only friction-basis rebuild diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferFrictionBuildPreflight {
    ContactBufferFrictionBuildRejectReason reason = ContactBufferFrictionBuildRejectReason::None;
    bool allCached = false;

    bool needsFrictionBuild() const { return reason == ContactBufferFrictionBuildRejectReason::None; }

FUSE_PHYSICS_INLINE ContactBufferFrictionBuildPreflight preflightContactBufferFrictionBuild(
    ContactBufferFrictionBuildPreflight preflight{};
    preflight.reason = contactBufferFrictionBuildRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBuildRejectReason::EmptyBuffer;
    preflight.allCached = preflight.reason == ContactBufferFrictionBuildRejectReason::AllCached;

/// Non-mutating friction-build skip predicate — inverse of `needsFrictionBuild` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipContactBufferFrictionBuild(const ContactBufferSoA& buffer) {
    return !preflightContactBufferFrictionBuild(buffer).needsFrictionBuild();

/// Non-mutating friction-build predicate — mirrors `preflightContactBufferFrictionBuild` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunContactBufferFrictionBuild(const ContactBufferSoA& buffer) {
    return preflightContactBufferFrictionBuild(buffer).needsFrictionBuild();

/// Why contact-buffer warm-start stub would early-out (B4.6 deepen pass).
enum class ContactBufferWarmStartRejectReason : u8 {
    OutOfRangeSlot,
    InvalidSlot,

/// Human-readable label for warm-start reject reasons (B4.6 deepen pass).
FUSE_PHYSICS_INLINE const char* contactBufferWarmStartRejectReasonName(ContactBufferWarmStartRejectReason reason) {
    case ContactBufferWarmStartRejectReason::None:
    }
        buffer.activeCount = buffer.countValidSlots();

FUSE_PHYSICS_INLINE const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason) {
    switch (reason) {
    case ContactBufferClampRejectReason::None:
        return "None";
    case ContactBufferClampRejectReason::NoCapacityLimit:
        return "NoCapacityLimit";
    case ContactBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";
    return "Unknown";

FUSE_PHYSICS_INLINE ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.maxCapacity == 0u) {
        return ContactBufferClampRejectReason::NoCapacityLimit;
    if (buffer.activeCount <= buffer.maxCapacity) {
        return ContactBufferClampRejectReason::WithinCapacity;
    return ContactBufferClampRejectReason::None;

FUSE_PHYSICS_INLINE ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.reason = contact_buffer_clamp_reject_reason(buffer);
    preflight.noCapacityLimit = preflight.reason == ContactBufferClampRejectReason::NoCapacityLimit;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;
    return preflight;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_clamp(buffer).needs_clamp();

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_clamp(buffer).needs_clamp();

FUSE_PHYSICS_INLINE u32 apply_contact_buffer_max_capacity_clamp_with_preflight(ContactBufferSoA& buffer) {

FUSE_PHYSICS_INLINE const char* contact_buffer_compact_and_clamp_reject_reason_name(
    ContactBufferCompactAndClampRejectReason reason) {
    case ContactBufferCompactAndClampRejectReason::None:
    case ContactBufferCompactAndClampRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactAndClampRejectReason::NoWork:
        return "NoWork";

FUSE_PHYSICS_INLINE ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    if (buffer.canSkipCompaction() && buffer.canSkipMaxCapacityClamp()) {
        return ContactBufferCompactAndClampRejectReason::NoWork;
    return ContactBufferCompactAndClampRejectReason::None;

FUSE_PHYSICS_INLINE ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(
    ContactBufferCompactAndClampPreflight preflight{};
    preflight.reason = contact_buffer_compact_and_clamp_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compact_and_clamp(buffer).needs_compact_and_clamp();

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compact_and_clamp(buffer).needs_compact_and_clamp();

    if (!should_run_contact_buffer_compact_and_clamp(buffer)) {

FUSE_PHYSICS_INLINE const char* contact_buffer_to_vector_reject_reason_name(
    ContactBufferToVectorRejectReason reason) {
    case ContactBufferToVectorRejectReason::None:
    case ContactBufferToVectorRejectReason::EmptyBuffer:

FUSE_PHYSICS_INLINE ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(
        return ContactBufferToVectorRejectReason::EmptyBuffer;
    return ContactBufferToVectorRejectReason::None;

FUSE_PHYSICS_INLINE bool contact_buffer_to_vector_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferToVectorRejectReason expected) {
    return contact_buffer_to_vector_reject_reason(buffer) == expected;

FUSE_PHYSICS_INLINE ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(
    ContactBufferToVectorPreflight preflight{};
    preflight.reason = contact_buffer_to_vector_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferToVectorRejectReason::EmptyBuffer;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_to_vector(buffer).can_export();

FUSE_PHYSICS_INLINE bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_to_vector(buffer).can_export();

FUSE_PHYSICS_INLINE std::vector<ContactManifold> to_vector_contact_buffer_with_preflight(
    if (!should_run_contact_buffer_to_vector(buffer)) {
        return {};
    return buffer.toVector();

FUSE_PHYSICS_INLINE const char* contact_buffer_manifold_at_reject_reason_name(
    ContactBufferManifoldAtRejectReason reason) {
    case ContactBufferManifoldAtRejectReason::None:
    case ContactBufferManifoldAtRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case ContactBufferManifoldAtRejectReason::InvalidSlot:
        return "InvalidSlot";

FUSE_PHYSICS_INLINE ContactBufferManifoldAtRejectReason contact_buffer_manifold_at_reject_reason(
    u32 index) {
    if (index >= buffer.activeCount) {
        return ContactBufferManifoldAtRejectReason::OutOfRangeIndex;
    if (buffer.validFlags[index] == 0u) {
        return ContactBufferManifoldAtRejectReason::InvalidSlot;
    return ContactBufferManifoldAtRejectReason::None;

FUSE_PHYSICS_INLINE ContactBufferManifoldAtPreflight preflight_contact_buffer_manifold_at(
    ContactBufferManifoldAtPreflight preflight{};
    preflight.reason = contact_buffer_manifold_at_reject_reason(buffer, index);
    preflight.outOfRangeIndex = preflight.reason == ContactBufferManifoldAtRejectReason::OutOfRangeIndex;
    preflight.invalidSlot = preflight.reason == ContactBufferManifoldAtRejectReason::InvalidSlot;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_manifold_at(const ContactBufferSoA& buffer, u32 index) {
    return !preflight_contact_buffer_manifold_at(buffer, index).can_read();

FUSE_PHYSICS_INLINE const char* contact_buffer_warm_start_reject_reason_name(
    ContactBufferWarmStartRejectReason reason) {
    case ContactBufferWarmStartRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case ContactBufferWarmStartRejectReason::InvalidSlot:

/// Diagnose why applyWarmStartStub would skip; vacuously succeeds when warm-start may proceed (B4.6 deepen pass).
FUSE_PHYSICS_INLINE ContactBufferWarmStartRejectReason contactBufferWarmStartRejectReason(
    u32 slot) {
    if (slot >= buffer.pairSlotCount) {
        return ContactBufferWarmStartRejectReason::OutOfRangeSlot;
        return ContactBufferWarmStartRejectReason::InvalidSlot;
    return ContactBufferWarmStartRejectReason::None;

/// Returns true when `contactBufferWarmStartRejectReason` matches `expected` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool contactBufferWarmStartRejectsForReason(
    ContactBufferWarmStartRejectReason expected) {
    return contactBufferWarmStartRejectReason(buffer, slot) == expected;

/// Read-only warm-start diagnostics — no mutation (B4.6 deepen pass).
};

const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason);

ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer);

bool contact_buffer_clamp_rejects_for_reason(
    ContactBufferClampRejectReason expected);

    bool emptyBuffer = false;

    bool needs_clamp() const { return reason == ContactBufferClampRejectReason::None; }

ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer);

bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer);

    None = 0,
    EmptyBuffer,

const char* contact_buffer_compact_and_clamp_reject_reason_name(
    ContactBufferCompactAndClampRejectReason reason);

ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer);

bool contact_buffer_compact_and_clamp_rejects_for_reason(
    ContactBufferCompactAndClampRejectReason expected);


    bool needs_compact_and_clamp() const {
        return reason == ContactBufferCompactAndClampRejectReason::None;

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(

bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer);

/// Why contact-buffer friction tangent rebuild would early-out (B4.6 deepen pass).
enum class ContactBufferFrictionTangentRejectReason : u8 {
    AllOrthonormal,

const char* contact_buffer_friction_tangent_reject_reason_name(
    ContactBufferFrictionTangentRejectReason reason);

ContactBufferFrictionTangentRejectReason contact_buffer_friction_tangent_reject_reason(
    f32 epsilon = 1e-4f);

bool contact_buffer_friction_tangent_rejects_for_reason(
    ContactBufferFrictionTangentRejectReason expected,

struct ContactBufferFrictionTangentPreflight {
    ContactBufferFrictionTangentRejectReason reason = ContactBufferFrictionTangentRejectReason::None;
    bool allOrthonormal = false;

    bool needs_rebuild() const { return reason == ContactBufferFrictionTangentRejectReason::None; }

ContactBufferFrictionTangentPreflight preflight_contact_buffer_friction_tangent_bases(

bool can_skip_contact_buffer_friction_tangent_bases(

bool should_run_contact_buffer_friction_tangent_bases(

/// Why contact-buffer warm-start stub apply would early-out (B4.6 deepen pass).

const char* contact_buffer_warm_start_reject_reason_name(ContactBufferWarmStartRejectReason reason);

ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
    u32 slot);

bool contact_buffer_warm_start_rejects_for_reason(
    u32 slot,
    ContactBufferWarmStartRejectReason expected);

struct ContactBufferWarmStartPreflight {
    ContactBufferWarmStartRejectReason reason = ContactBufferWarmStartRejectReason::None;
    bool outOfRangeSlot = false;
    bool invalidSlot = false;

    bool canWarmStart() const { return reason == ContactBufferWarmStartRejectReason::None; }

FUSE_PHYSICS_INLINE ContactBufferWarmStartPreflight preflightContactBufferWarmStart(
    ContactBufferWarmStartPreflight preflight{};
    preflight.reason = contactBufferWarmStartRejectReason(buffer, slot);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWarmStartRejectReason::OutOfRangeSlot;
    preflight.invalidSlot = preflight.reason == ContactBufferWarmStartRejectReason::InvalidSlot;

/// Non-mutating warm-start skip predicate — inverse of `canWarmStart` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipContactBufferWarmStart(const ContactBufferSoA& buffer, u32 slot) {
    return !preflightContactBufferWarmStart(buffer, slot).canWarmStart();

/// Non-mutating warm-start predicate — mirrors `preflightContactBufferWarmStart` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunContactBufferWarmStart(const ContactBufferSoA& buffer, u32 slot) {
    return preflightContactBufferWarmStart(buffer, slot).canWarmStart();

/// Why contact-buffer toVector would early-out (B4.6 deepen pass).
enum class ContactBufferToVectorRejectReason : u8 {

/// Human-readable label for contact-buffer toVector reject reasons (B4.6 deepen pass).
FUSE_PHYSICS_INLINE const char* contactBufferToVectorRejectReasonName(ContactBufferToVectorRejectReason reason) {

/// Diagnose why toVector would return empty; vacuously succeeds when export may proceed (B4.6 deepen pass).
FUSE_PHYSICS_INLINE ContactBufferToVectorRejectReason contactBufferToVectorRejectReason(

/// Returns true when `contactBufferToVectorRejectReason` matches `expected` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool contactBufferToVectorRejectsForReason(
    return contactBufferToVectorRejectReason(buffer) == expected;

/// Read-only toVector diagnostics — no mutation (B4.6 deepen pass).
struct ContactBufferToVectorPreflight {
    ContactBufferToVectorRejectReason reason = ContactBufferToVectorRejectReason::None;

    bool canExport() const { return reason == ContactBufferToVectorRejectReason::None; }

FUSE_PHYSICS_INLINE ContactBufferToVectorPreflight preflightContactBufferToVector(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferToVectorRejectReason(buffer);

/// Non-mutating toVector skip predicate — inverse of `canExport` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool canSkipContactBufferToVector(const ContactBufferSoA& buffer) {
    return !preflightContactBufferToVector(buffer).canExport();

/// Non-mutating toVector predicate — mirrors `preflightContactBufferToVector` (B4.6 deepen pass).
FUSE_PHYSICS_INLINE bool shouldRunContactBufferToVector(const ContactBufferSoA& buffer) {
    return preflightContactBufferToVector(buffer).canExport();
    }
    return "Unknown";

FUSE_PHYSICS_INLINE ContactBufferWarmStartRejectReason contact_buffer_warm_start_reject_reason(
    const ContactBufferSoA& buffer,
    if (buffer.validFlags[slot] == 0u) {

FUSE_PHYSICS_INLINE ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    preflight.reason = contact_buffer_warm_start_reject_reason(buffer, slot);
    return preflight;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot) {
    return !preflight_contact_buffer_warm_start(buffer, slot).can_apply();

FUSE_PHYSICS_INLINE bool apply_contact_buffer_warm_start_with_preflight(
    u32 slot,
    ContactManifold& manifold) {
    if (!preflight_contact_buffer_warm_start(buffer, slot).can_apply()) {
        return false;
    buffer.applyWarmStartStub(slot, manifold);
    return true;

FUSE_PHYSICS_INLINE const char* contact_buffer_tangent_basis_reject_reason_name(
    ContactBufferTangentBasisRejectReason reason) {
    switch (reason) {
    case ContactBufferTangentBasisRejectReason::None:
        return "None";
    case ContactBufferTangentBasisRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferTangentBasisRejectReason::NoValidSlots:
        return "NoValidSlots";

FUSE_PHYSICS_INLINE ContactBufferTangentBasisRejectReason contact_buffer_tangent_basis_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.activeCount == 0u) {
        return ContactBufferTangentBasisRejectReason::EmptyBuffer;
    for (u32 slot = 0u; slot < buffer.activeCount; ++slot) {
        if (buffer.validFlags[slot] != 0u) {
            return ContactBufferTangentBasisRejectReason::None;
    return ContactBufferTangentBasisRejectReason::NoValidSlots;

FUSE_PHYSICS_INLINE ContactBufferTangentBasisPreflight preflight_contact_buffer_tangent_basis(
    ContactBufferTangentBasisPreflight preflight{};
    preflight.reason = contact_buffer_tangent_basis_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferTangentBasisRejectReason::EmptyBuffer;
    preflight.noValidSlots = preflight.reason == ContactBufferTangentBasisRejectReason::NoValidSlots;

FUSE_PHYSICS_INLINE bool can_skip_contact_buffer_build_friction_tangent_bases(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_tangent_basis(buffer).can_build();

FUSE_PHYSICS_INLINE bool build_contact_buffer_friction_tangent_bases_with_preflight(ContactBufferSoA& buffer) {
    if (!preflight_contact_buffer_tangent_basis(buffer).can_build()) {
    buffer.buildFrictionTangentBases();
    bool can_apply() const { return reason == ContactBufferWarmStartRejectReason::None; }
};

ContactBufferWarmStartPreflight preflight_contact_buffer_warm_start(
    u32 slot);

bool can_skip_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

bool should_run_contact_buffer_warm_start(const ContactBufferSoA& buffer, u32 slot);

    None = 0,
    EmptyBuffer,

const char* contact_buffer_to_vector_reject_reason_name(ContactBufferToVectorRejectReason reason);

ContactBufferToVectorRejectReason contact_buffer_to_vector_reject_reason(const ContactBufferSoA& buffer);

bool contact_buffer_to_vector_rejects_for_reason(
    ContactBufferToVectorRejectReason expected);

    bool emptyBuffer = false;

    bool can_export() const { return reason == ContactBufferToVectorRejectReason::None; }

ContactBufferToVectorPreflight preflight_contact_buffer_to_vector(const ContactBufferSoA& buffer);

bool can_skip_contact_buffer_to_vector(const ContactBufferSoA& buffer);

bool should_run_contact_buffer_to_vector(const ContactBufferSoA& buffer);

} // namespace fuse::physics::narrowphase
