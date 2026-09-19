#include <fuse/physics/narrowphase/contact_buffer.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>

#include <fuse/physics/narrowphase/friction.hpp>

#include <fuse/physics/narrowphase/friction.hpp>

#include <algorithm>
#include <climits>

namespace fuse::physics::narrowphase {

namespace {

void copySlotFields(
    ContactBufferSoA& buffer,
    u32 writeIndex,
    u32 readIndex) {
    buffer.contactPoints[writeIndex] = buffer.contactPoints[readIndex];
    buffer.contactNormals[writeIndex] = buffer.contactNormals[readIndex];
    buffer.penetrationDepths[writeIndex] = buffer.penetrationDepths[readIndex];
    buffer.minSeparations[writeIndex] = buffer.minSeparations[readIndex];
    buffer.bodyA[writeIndex] = buffer.bodyA[readIndex];
    buffer.bodyB[writeIndex] = buffer.bodyB[readIndex];
    buffer.validFlags[writeIndex] = 1u;
    buffer.pointCounts[writeIndex] = buffer.pointCounts[readIndex];
    buffer.warmNormalImpulses[writeIndex] = buffer.warmNormalImpulses[readIndex];
    buffer.warmTangentImpulses[writeIndex] = buffer.warmTangentImpulses[readIndex];
    buffer.tangent1[writeIndex] = buffer.tangent1[readIndex];
    buffer.tangent2[writeIndex] = buffer.tangent2[readIndex];

    const u32 readBase = readIndex * kMaxContactPointsPerManifold;
    const u32 writeBase = writeIndex * kMaxContactPointsPerManifold;
    for (u32 pointIndex = 0u; pointIndex < kMaxContactPointsPerManifold; ++pointIndex) {
        buffer.pointSlots[writeBase + pointIndex] = buffer.pointSlots[readBase + pointIndex];
        buffer.pointPenetrations[writeBase + pointIndex] = buffer.pointPenetrations[readBase + pointIndex];
    }

} // namespace

bool manifoldIsWritable(const ContactManifold& manifold) {
    return manifold.valid && manifold.bodyA != manifold.bodyB;


u32 ContactBufferSoA::countValidSlots() const {
    if (canSkipSoAIteration()) {
        return 0u;

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 validCount = 0u;
    for (u32 slot = 0u; slot < scanCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
    return validCount;

bool ContactBufferSoA::slotIsValid(u32 slot) const {
    if (slot >= validFlags.size()) {
        return false;
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
    return validFlags[slot] != 0u;

bool ContactBufferSoA::canSkipCompaction() const {
        return true;

    if (scanCount == 0u) {

        if (validFlags[slot] == 0u) {

u32 ContactBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    }
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;

bool ContactBufferSoA::canAcceptContacts(u32 additionalCount) const {
    if (additionalCount == 0u) {
        return true;
    return activeCount + additionalCount <= maxCapacity;
bool ContactBufferSoA::canSkipCompaction() const {
    if (canSkipSoAIteration()) {

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    if (scanCount == 0u) {

    for (u32 slot = 0u; slot < scanCount; ++slot) {
        if (validFlags[slot] == 0u) {
            return false;

bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;

    if (pairSlotCount == 0u) {
    return countValidSlots() == pairSlotCount;

u32 ContactBufferSoA::countValidSlots() const {
    u32 validCount = 0u;
    for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
    return validCount;

bool ContactBufferSoA::slotIsValid(u32 slot) const {
    return slot < validFlags.size() && validFlags[slot] != 0u;
}


bool ContactBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;

const char* contactBufferWriteRejectReasonName(ContactBufferWriteRejectReason reason) {
    switch (reason) {
    case ContactBufferWriteRejectReason::None:
        return "None";
    case ContactBufferWriteRejectReason::InvalidSlot:
        return "InvalidSlot";
    case ContactBufferWriteRejectReason::InvalidManifold:
        return "InvalidManifold";
    case ContactBufferWriteRejectReason::SelfPair:
        return "SelfPair";
    return "Unknown";
    }

ContactBufferWriteRejectReason contactBufferWriteRejectReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (slot >= buffer.pairSlotCount) {
        return ContactBufferWriteRejectReason::InvalidSlot;
    if (!manifold.valid) {
        return ContactBufferWriteRejectReason::InvalidManifold;
    if (manifold.bodyA == manifold.bodyB) {
        return ContactBufferWriteRejectReason::SelfPair;
    return ContactBufferWriteRejectReason::None;

bool contactBufferWriteRejectsForReason(
    const ContactManifold& manifold,
    ContactBufferWriteRejectReason expected) {
    return contactBufferWriteRejectReason(buffer, slot, manifold) == expected;

ContactBufferWritePreflight preflightContactBufferWrite(
    }

    const ContactBufferSoA& buffer,
    u32 slot,

    const ContactManifold& manifold) {
    ContactBufferWritePreflight preflight{};
    preflight.reason = contactBufferWriteRejectReason(buffer, slot, manifold);
    preflight.invalidSlot = preflight.reason == ContactBufferWriteRejectReason::InvalidSlot;
    preflight.invalidManifold = preflight.reason == ContactBufferWriteRejectReason::InvalidManifold;
    preflight.selfPair = preflight.reason == ContactBufferWriteRejectReason::SelfPair;
    return preflight;

const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason) {
    case ContactBufferCompactionRejectReason::None:
}

    switch (reason) {
        return "None";
    case ContactBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactionRejectReason::AllValid:
        return "AllValid";
    }
    return "Unknown";

ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;
    if (buffer.canSkipCompaction()) {
        return ContactBufferCompactionRejectReason::AllValid;
    return ContactBufferCompactionRejectReason::None;

bool contactBufferCompactionRejectsForReason(
    ContactBufferCompactionRejectReason expected) {
    return contactBufferCompactionRejectReason(buffer) == expected;
    }

    const ContactBufferSoA& buffer,

ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer) {
    ContactBufferCompactionPreflight preflight{};
    preflight.reason = contactBufferCompactionRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;

bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer) {
    return !preflightContactBufferCompaction(buffer).needsCompaction();

bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer) {
    return preflightContactBufferCompaction(buffer).needsCompaction();

const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason) {
    case ContactBufferClampRejectReason::None:
    case ContactBufferClampRejectReason::EmptyBuffer:
    case ContactBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";

ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer) {
        return ContactBufferClampRejectReason::EmptyBuffer;
    if (buffer.canSkipMaxCapacityClamp()) {
        return ContactBufferClampRejectReason::WithinCapacity;
    return ContactBufferClampRejectReason::None;

bool contactBufferClampRejectsForReason(
    ContactBufferClampRejectReason expected) {
    return contactBufferClampRejectReason(buffer) == expected;
    return preflight;
}



    switch (reason) {
        return "None";
        return "EmptyBuffer";
    return "Unknown";

    if (buffer.canSkipSoAIteration()) {
    if (!buffer.canApplyMaxCapacityClamp()) {

    const ContactBufferSoA& buffer,

ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.reason = contactBufferClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;

bool canSkipContactBufferClamp(const ContactBufferSoA& buffer) {
    return !preflightContactBufferClamp(buffer).needsClamp();

bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer) {
    return preflightContactBufferClamp(buffer).needsClamp();
namespace {

bool contactBufferIsEmpty(const ContactBufferSoA& buffer) {
    return buffer.pairSlotCount == 0u;

bool contactBufferAllInvalid(const ContactBufferSoA& buffer) {
    if (contactBufferIsEmpty(buffer)) {
    for (u32 slot = 0u; slot < buffer.pairSlotCount; ++slot) {
        if (buffer.validFlags[slot] != 0u) {

bool contactBufferNeedsClamp(const ContactBufferSoA& buffer) {
    return buffer.maxCapacity > 0u && buffer.activeCount > buffer.maxCapacity;
#include <cmath>



bool tangentBasisMatchesNormal(vec3 normal, const TangentBasis& basis, f32 epsilon = 1e-4f) {
    const f32 tangent1Length = basis.tangent1.length();
    const f32 tangent2Length = basis.tangent2.length();
    if (std::fabs(tangent1Length - 1.f) > epsilon || std::fabs(tangent2Length - 1.f) > epsilon) {

    const vec3 unitNormal = normal.normalized();
    return std::fabs(basis.tangent1.dot(unitNormal)) <= epsilon &&
           std::fabs(basis.tangent2.dot(unitNormal)) <= epsilon &&
           std::fabs(basis.tangent1.dot(basis.tangent2)) <= epsilon;

} // namespace

bool ContactBufferSoA::canSkipMaxCapacityClamp() const {
    if (maxCapacity == 0u || activeCount == 0u) {
    return activeCount <= maxCapacity;

    u32 valid = 0u;
            ++valid;
    return valid;

    return slot < pairSlotCount && validFlags[slot] != 0u;
}




    for (u32 i = 0u; i < scanCount; ++i) {
        if (validFlags[i] != 0u) {

    if (validCount == 0u) {

    u32 writeIndex = 0u;
    for (u32 readIndex = 0u; readIndex < scanCount; ++readIndex) {
        if (validFlags[readIndex] == 0u) {
            continue;
        if (writeIndex != readIndex) {
        ++writeIndex;

    return writeIndex == validCount;

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : validFlags.size();

    return preflight;



const char* contactBufferCompactAndClampRejectReasonName(ContactBufferCompactAndClampRejectReason reason) {
    switch (reason) {
    case ContactBufferCompactAndClampRejectReason::None:
        return "None";
    case ContactBufferCompactAndClampRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactAndClampRejectReason::NoWork:
        return "NoWork";
    return "Unknown";

ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    if (buffer.canSkipCompactAndClamp()) {
        return ContactBufferCompactAndClampRejectReason::NoWork;
    return ContactBufferCompactAndClampRejectReason::None;

bool contactBufferCompactAndClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected) {
    return contactBufferCompactAndClampRejectReason(buffer) == expected;

ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(
    ContactBufferCompactAndClampPreflight preflight{};
    preflight.reason = contactBufferCompactAndClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork;

bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    return !preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();

bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    return preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();

void writeContactBufferSlotWithPreflight(
    ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (!preflightContactBufferWrite(buffer, slot, manifold).canWrite()) {
        return;
    buffer.writeSlot(slot, manifold);

u32 compactContactBufferWithPreflight(ContactBufferSoA& buffer) {
    if (!preflightContactBufferCompaction(buffer).needsCompaction()) {
        return buffer.activeCount;
    return buffer.compact();

u32 clampContactBufferWithPreflight(ContactBufferSoA& buffer) {
    if (!preflightContactBufferClamp(buffer).needsClamp()) {
    return buffer.applyMaxCapacityClamp();

u32 compactAndClampContactBufferWithPreflight(ContactBufferSoA& buffer) {
    if (!preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp()) {
    return buffer.compactAndClamp();

void ContactBufferSoA::setMaxCapacity(u32 capacity) {
    maxCapacity = capacity;
}

u32 ContactBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    }
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
}

bool ContactBufferSoA::canAcceptContacts(u32 additionalCount) const {
    if (additionalCount == 0u) {
        return true;
    }
    if (maxCapacity == 0u) {
        return true;
    }
    return activeCount + additionalCount <= maxCapacity;
}

bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
}

u32 ContactBufferSoA::countValidSlots() const {
    if (canSkipSoAIteration()) {
        return 0u;
    }

    u32 validCount = 0u;
    for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
        }
    }
    return validCount;
}

bool ContactBufferSoA::canSkipCompaction() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    if (pairSlotCount == 0u) {
        return true;
    }

    for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
        if (validFlags[slot] == 0u) {
            return false;
        }
    }
    return true;
}

bool ContactBufferSoA::slotIsValid(u32 slot) const {
    return slot < validFlags.size() && validFlags[slot] != 0u;
}

void ContactBufferSoA::reserve(u32 capacity) {
    contactPoints.reserve(capacity);
    contactNormals.reserve(capacity);
    penetrationDepths.reserve(capacity);
    minSeparations.reserve(capacity);
    bodyA.reserve(capacity);
    bodyB.reserve(capacity);
    validFlags.reserve(capacity);
    pointCounts.reserve(capacity);
    pointSlots.reserve(capacity * kMaxContactPointsPerManifold);
    pointPenetrations.reserve(capacity * kMaxContactPointsPerManifold);
    warmNormalImpulses.reserve(capacity);
    warmTangentImpulses.reserve(capacity);
    tangent1.reserve(capacity);
    tangent2.reserve(capacity);
}

void ContactBufferSoA::clear() {
    activeCount = 0;
    pairSlotCount = 0;
    droppedCount = 0;
}

bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
bool ContactBufferSoA::slotIsValid(u32 slot) const {
    if (slot >= validFlags.size()) {
        return false;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
    return validFlags[slot] != 0u;
}

u32 ContactBufferSoA::countValidSlots() const {
    if (canSkipSoAIteration()) {
        return 0u;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 validCount = 0u;
    for (u32 slot = 0; slot < scanCount; ++slot) {
    for (u32 slot = 0u; slot < scanCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
        }
    }
    return validCount;
}

bool ContactBufferSoA::canSkipCompaction() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    if (scanCount == 0u) {
        return true;
    }

    for (u32 slot = 0; slot < scanCount; ++slot) {
    for (u32 slot = 0u; slot < scanCount; ++slot) {
        if (validFlags[slot] == 0u) {
            return false;
        }
    }
    return true;
}

bool ContactBufferSoA::slotIsValid(u32 slot) const {
    return slot < validFlags.size() && validFlags[slot] != 0u;
bool ContactBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;
}

u32 ContactBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;

bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
}

void ContactBufferSoA::preparePairSlots(u32 pairCount) {
    pairSlotCount = pairCount;
    activeCount = 0;
    droppedCount = 0;
    contactPoints.assign(pairCount, {});
    contactNormals.assign(pairCount, {});
    penetrationDepths.assign(pairCount, 0.f);
    minSeparations.assign(pairCount, 0.f);
    bodyA.assign(pairCount, 0u);
    bodyB.assign(pairCount, 0u);
    validFlags.assign(pairCount, 0u);
    pointCounts.assign(pairCount, 0u);
    pointSlots.assign(pairCount * kMaxContactPointsPerManifold, {});
    pointPenetrations.assign(pairCount * kMaxContactPointsPerManifold, 0.f);
    warmNormalImpulses.assign(pairCount, 0.f);
    warmTangentImpulses.assign(pairCount, {});
    tangent1.assign(pairCount, {});
    tangent2.assign(pairCount, {});
}

const char* contactBufferWriteRejectReasonName(ContactBufferWriteRejectReason reason) {
    switch (reason) {
    case ContactBufferWriteRejectReason::None:
        return "None";
    case ContactBufferWriteRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case ContactBufferWriteRejectReason::InvalidManifold:
        return "InvalidManifold";
    case ContactBufferWriteRejectReason::SelfPair:
        return "SelfPair";
    }
    return "Unknown";

ContactBufferWriteRejectReason contactBufferWriteRejectReason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (slot >= buffer.pairSlotCount) {
        return ContactBufferWriteRejectReason::OutOfRangeSlot;
    if (!manifold.valid) {
        return ContactBufferWriteRejectReason::InvalidManifold;
    if (manifold.bodyA == manifold.bodyB) {
        return ContactBufferWriteRejectReason::SelfPair;
    return ContactBufferWriteRejectReason::None;

bool contactBufferWriteRejectsForReason(
    const ContactManifold& manifold,
    ContactBufferWriteRejectReason expected) {
    return contactBufferWriteRejectReason(buffer, slot, manifold) == expected;

ContactBufferWritePreflight preflightContactBufferWrite(
    ContactBufferWritePreflight preflight{};
    preflight.reason = contactBufferWriteRejectReason(buffer, slot, manifold);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWriteRejectReason::OutOfRangeSlot;
    preflight.invalidManifold = preflight.reason == ContactBufferWriteRejectReason::InvalidManifold;
    preflight.selfPair = preflight.reason == ContactBufferWriteRejectReason::SelfPair;
    return preflight;

bool ContactBufferSoA::canSkipWrite(u32 slot, const ContactManifold& manifold) const {
    return !preflightContactBufferWrite(*this, slot, manifold).canWrite();
ContactBufferWritePreflight preflight_contact_buffer_write(
        preflight.skipped = true;
        preflight.outOfRangeSlot = true;
        preflight.invalidManifold = true;
        preflight.selfPair = true;

bool should_skip_contact_buffer_write(
    return !preflight_contact_buffer_write(slot, buffer, manifold).can_write();

bool ContactBufferSoA::writeSlotIfValid(u32 slot, const ContactManifold& manifold) {
    if (!preflight_contact_buffer_write(slot, *this, manifold).can_write()) {
        return false;
    writeSlot(slot, manifold);
    return true;
bool ContactBufferSoA::writeSlotWithPreflight(u32 slot, const ContactManifold& manifold) {
    if (!preflight_contact_buffer_write(*this, slot, manifold).canWrite()) {

void ContactBufferSoA::writeSlot(u32 slot, const ContactManifold& manifold) {
    if (slot >= pairSlotCount || !manifold.valid || manifold.bodyA == manifold.bodyB ||
        !can_finalize_contact_manifold(manifold)) {
    const ContactBufferWritePreflight writePreflight = preflight_contact_buffer_write(*this, slot, manifold);
    if (!writePreflight.canWrite()) {
    const ContactBufferWritePreflight preflight = preflightContactBufferWrite(*this, slot, manifold);
    if (!preflight.canWrite()) {
    const ContactBufferWritePreflight writePreflight = preflightContactBufferWrite(*this, slot, manifold);
    const ContactBufferWritePreflight preflight = preflight_contact_buffer_write(*this, slot, manifold);
    if (!preflightContactBufferWrite(*this, slot, manifold).canWrite()) {
bool ContactBufferSoA::canSkipCompaction() const {
    if (pairSlotCount == 0u) {
        return true;
    for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
        if (validFlags[slot] == 0u) {
            return false;

bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return maxCapacity > 0u && activeCount > maxCapacity;

u32 ContactBufferSoA::countValidSlots() const {
    u32 validCount = 0u;
        if (validFlags[slot] != 0u) {
            ++validCount;
    return validCount;

bool ContactBufferSoA::slotIsValid(u32 slot) const {
    return slot < pairSlotCount && validFlags[slot] != 0u;

    if (contactBufferWriteRejectReason(*this, slot, manifold) != ContactBufferWriteRejectReason::None) {
    if (!canWriteSlot(slot, pairSlotCount, manifold)) {
    if (!manifoldIsWritable(manifold) || slot >= pairSlotCount) {
        return;
    }

    contactPoints[slot] = manifold.contactPoint;
    contactNormals[slot] = manifold.contactNormal;
    penetrationDepths[slot] = manifold.penetrationDepth;
    minSeparations[slot] = manifold.minSeparation;
    bodyA[slot] = manifold.bodyA;
    bodyB[slot] = manifold.bodyB;
    validFlags[slot] = 1u;
    warmNormalImpulses[slot] = manifold.warmNormalImpulse;
    warmTangentImpulses[slot] = manifold.warmTangentImpulse;

    const TangentBasis basis = buildTangentBasisForManifold(manifold);
    tangent1[slot] = basis.tangent1;
    tangent2[slot] = basis.tangent2;

    const u32 pointCount = std::min(manifold.pointCount, kMaxContactPointsPerManifold);
    pointCounts[slot] = static_cast<u8>(pointCount);
    const u32 base = pointSlotBase(slot);
    for (u32 pointIndex = 0u; pointIndex < kMaxContactPointsPerManifold; ++pointIndex) {
        if (pointIndex < pointCount) {
            pointSlots[base + pointIndex] = manifold.points[pointIndex].point;
            pointPenetrations[base + pointIndex] = manifold.points[pointIndex].penetration;
        } else {
            pointSlots[base + pointIndex] = {};
            pointPenetrations[base + pointIndex] = 0.f;
        }
    }
}

bool ContactBufferSoA::canWriteSlot(u32 slot, u32 pairSlotCount, const ContactManifold& manifold) {
    return slot < pairSlotCount && manifold.valid && manifold.bodyA != manifold.bodyB;
}

bool ContactBufferSoA::writeSlotIfValid(u32 slot, const ContactManifold& manifold) {
    if (!canWriteSlot(slot, pairSlotCount, manifold)) {
        return false;
    writeSlot(slot, manifold);
    return true;
void ContactBufferSoA::invalidateSlot(u32 slot) {
    if (slot >= validFlags.size()) {
        return;
    validFlags[slot] = 0u;
}

void ContactBufferSoA::applyWarmStartStub(u32 slot, ContactManifold& manifold) const {
    if (slot >= pairSlotCount || validFlags[slot] == 0u) {
        return;
    }

    manifold.warmNormalImpulse = warmNormalImpulses[slot];
    manifold.warmTangentImpulse = warmTangentImpulses[slot];
    manifold.frictionBasis = tangentBasisAt(slot);
}

u32 ContactBufferSoA::countValidSlots() const {
    u32 validCount = 0u;
    for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
        }
    }
    return validCount;
}

bool ContactBufferSoA::canSkipCompaction() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    const u32 validCount = countValidSlots();
    if (validCount == 0u) {
        return true;
    }

    for (u32 slot = 0u; slot < validCount; ++slot) {
        if (validFlags[slot] == 0u) {
            return false;
        }
    }

    return activeCount == validCount;
}

bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return maxCapacity > 0u && activeCount > maxCapacity;
}

void ContactBufferSoA::buildFrictionTangentBases() {
    if (!should_run_contact_buffer_friction_basis(*this)) {
    if (!should_run_contact_buffer_friction_build(*this)) {
        return;
    }

    for (u32 slot = 0u; slot < activeCount; ++slot) {
        if (validFlags[slot] == 0u) {
            continue;
        }

        ContactManifold manifold = manifoldAt(slot);
        if (should_skip_friction_basis_preflight(manifold)) {
            continue;
        }

        compute_friction_tangents_if_needed(manifold);
        tangent1[slot] = manifold.frictionBasis.tangent1;
        tangent2[slot] = manifold.frictionBasis.tangent2;
    }

void ContactBufferSoA::buildFrictionTangentBasesIfNeeded(f32 epsilon) {
    for (u32 slot = 0u; slot < activeCount; ++slot) {
        if (validFlags[slot] == 0u) {
            continue;

        ContactManifold manifold = manifoldAt(slot);
        const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);
        if (preflight.can_reuse()) {
            tangent1[slot] = manifold.frictionBasis.tangent1;
            tangent2[slot] = manifold.frictionBasis.tangent2;

        if (preflight.shouldSkip) {
            tangent1[slot] = {};
            tangent2[slot] = {};

        const TangentBasis basis = buildTangentBasis(contactNormals[slot]);
        tangent1[slot] = basis.tangent1;
        tangent2[slot] = basis.tangent2;

bool ContactBufferSoA::canSkipCompaction() const {
    if (pairSlotCount == 0u) {
        return true;

    for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
            return false;

bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return activeCount > 0u && maxCapacity > 0u && activeCount > maxCapacity;

    if (can_skip_build_friction_tangent_bases(*this, epsilon)) {
        return;


        if (can_skip_friction_basis_rebuild(manifold, epsilon)) {

    }
}

u32 ContactBufferSoA::countValidSlots() const {
    if (canSkipSoAIteration()) {
        return 0u;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 validCount = 0u;
    for (u32 slot = 0u; slot < scanCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
        }
    }
    return validCount;
}

bool ContactBufferSoA::canSkipCompaction() const {
    if (canSkipSoAIteration()) {
        return true;
    }

    if (activeCount > 0u) {
        for (u32 slot = 0u; slot < activeCount; ++slot) {
            if (validFlags[slot] == 0u) {
                return false;
            }
        }
        return true;
    }

    if (pairSlotCount == 0u) {
        return true;
    }

    for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
        if (validFlags[slot] == 0u) {
            return false;
        }
    }
    return true;
}

bool ContactBufferSoA::canSkipMaxCapacityClamp() const {
    if (activeCount == 0u) {
        return true;
    }
    return maxCapacity == 0u || activeCount <= maxCapacity;
}

bool ContactBufferSoA::canSkipCompactAndClamp() const {
    return canSkipCompaction() && canSkipMaxCapacityClamp();
}

void ContactBufferSoA::buildFrictionTangentBasesIfNeeded() {
    for (u32 slot = 0u; slot < activeCount; ++slot) {
        if (validFlags[slot] == 0u) {
            continue;
        }

        const TangentBasis existing{tangent1[slot], tangent2[slot]};
        if (tangentBasisMatchesNormal(contactNormals[slot], existing)) {
            continue;
        }

        const TangentBasis basis = buildTangentBasis(contactNormals[slot]);
        tangent1[slot] = basis.tangent1;
        tangent2[slot] = basis.tangent2;
    }
}

TangentBasis ContactBufferSoA::tangentBasisAt(u32 index) const {
    if (index >= activeCount || validFlags[index] == 0u) {
        return {};
    }
    return {tangent1[index], tangent2[index]};
}

const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason) {
    switch (reason) {
    case ContactBufferCompactionRejectReason::None:
        return "None";
    case ContactBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactionRejectReason::AllValid:
        return "AllValid";
    }
    return "Unknown";

ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer) {
    if (buffer.pairSlotCount == 0u) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;

    bool hasInvalid = false;
    for (u32 i = 0u; i < buffer.pairSlotCount; ++i) {
        if (buffer.validFlags[i] == 0u) {
            hasInvalid = true;
            break;
    if (!hasInvalid) {
        return ContactBufferCompactionRejectReason::AllValid;
    return ContactBufferCompactionRejectReason::None;

bool contactBufferCompactionRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected) {
    return contactBufferCompactionRejectReason(buffer) == expected;

ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer) {
    ContactBufferCompactionPreflight preflight{};
    preflight.reason = contactBufferCompactionRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;
    return preflight;

bool ContactBufferSoA::canSkipCompaction() const {
    return contactBufferCompactionRejectReason(*this) != ContactBufferCompactionRejectReason::None;

bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer) {
    return !buffer.canSkipCompaction();

u32 ContactBufferSoA::compact() {
    if (pairSlotCount == 0u) {
    const ContactBufferCompactionPreflight compactionPreflight = preflight_contact_buffer_compaction(*this);
    if (compactionPreflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer) {
    const ContactBufferCompactPreflight compactionPreflight = preflightContactBufferCompact(*this);
    if (compactionPreflight.reason == ContactBufferCompactRejectReason::EmptyBuffer) {
        activeCount = 0u;
        return activeCount;

    if (canSkipCompaction()) {
        activeCount = pairSlotCount;
        pairSlotCount = 0u;

    if (compactionPreflight.reason == ContactBufferCompactionRejectReason::AllValid) {
        activeCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
        pairSlotCount = activeCount;
    const ContactBufferCompactionPreflight compactionPreflight = preflightContactBufferCompaction(*this);
    if (compactionPreflight.reason == ContactBufferCompactionRejectReason::AllInvalid) {
        for (u32 i = 0u; i < pairSlotCount; ++i) {
            validFlags[i] = 0u;
            pointCounts[i] = 0u;


    if (compactionPreflight.reason == ContactBufferCompactRejectReason::AllValid) {


    return canSkipContactBufferCompaction(*this);

bool ContactBufferSoA::canSkipClamp() const {
    return canSkipContactBufferClamp(*this);

    if (!should_run_contact_buffer_compact(*this)) {
    if (canSkipSoAIteration()) {
        return 0u;
    }


    const u32 validCount = countValidSlots();
    if (validCount == 0u) {

        activeCount = validCount;
    if (pairSlotCount == 0u) {
        return activeCount;
    }

        activeCount = 0u;

    if (canSkipCompaction()) {
        activeCount = pairSlotCount;
    }

    u32 writeIndex = 0;
    for (u32 readIndex = 0; readIndex < pairSlotCount; ++readIndex) {

        if (pairSlotCount > 0u) {
            activeCount = countValidSlots();

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;

    u32 writeIndex = 0u;
    for (u32 readIndex = 0u; readIndex < scanCount; ++readIndex) {
        if (validFlags[readIndex] == 0u) {
            continue;
        }
        if (writeIndex != readIndex) {
            copySlotFields(*this, writeIndex, readIndex);
        }
        ++writeIndex;
    }

    activeCount = writeIndex;
    pairSlotCount = activeCount;
    for (u32 i = activeCount; i < validFlags.size(); ++i) {
    for (u32 i = activeCount; i < scanCount; ++i) {
        validFlags[i] = 0u;
        pointCounts[i] = 0u;
    }
    return activeCount;
}

const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason) {
    switch (reason) {
    case ContactBufferClampRejectReason::None:
        return "None";
    case ContactBufferClampRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";
    }
    return "Unknown";
}

ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer) {
    if (buffer.activeCount == 0u) {
        return ContactBufferClampRejectReason::EmptyBuffer;
    }
    if (buffer.maxCapacity == 0u || buffer.activeCount <= buffer.maxCapacity) {
        return ContactBufferClampRejectReason::WithinCapacity;
    }
    return ContactBufferClampRejectReason::None;
}

bool contactBufferClampRejectsForReason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected) {
    return contactBufferClampRejectReason(buffer) == expected;
}

ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.reason = contactBufferClampRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;
    return preflight;
}

bool ContactBufferSoA::canSkipClamp() const {
    return contactBufferClampRejectReason(*this) != ContactBufferClampRejectReason::None;
}

bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer) {
    return !buffer.canSkipClamp();
}

u32 ContactBufferSoA::applyMaxCapacityClamp() {
    if (!preflight_contact_buffer_clamp(*this).needsClamp()) {
    const ContactBufferClampPreflight clampPreflight = preflightContactBufferClamp(*this);
    if (!clampPreflight.needsClamp()) {
    if (clampPreflight.reason != ContactBufferClampRejectReason::None) {
    if (!preflightContactBufferClamp(*this).needsClamp()) {
    if (contactBufferClampRejectReason(*this) != ContactBufferClampRejectReason::None) {
    if (clampPreflight.reason == ContactBufferClampRejectReason::EmptyBuffer) {
        return activeCount;
    }
    if (clampPreflight.reason == ContactBufferClampRejectReason::WithinCapacity) {
    if (!should_run_contact_buffer_clamp(*this)) {
    if (!canApplyMaxCapacityClamp()) {
        return activeCount;
    }

    if (activeCount > 1u) {
        std::vector<u32> order(activeCount);
        for (u32 i = 0u; i < activeCount; ++i) {
            order[i] = i;
        }

        std::sort(order.begin(), order.end(), [&](u32 lhs, u32 rhs) {
            return penetrationDepths[lhs] > penetrationDepths[rhs];
        });

        std::vector<vec3> sortedContactPoints(activeCount);
        std::vector<vec3> sortedContactNormals(activeCount);
        std::vector<f32> sortedPenetrationDepths(activeCount);
        std::vector<f32> sortedMinSeparations(activeCount);
        std::vector<u32> sortedBodyA(activeCount);
        std::vector<u32> sortedBodyB(activeCount);
        std::vector<u8> sortedValidFlags(activeCount);
        std::vector<u8> sortedPointCounts(activeCount);
        std::vector<f32> sortedWarmNormalImpulses(activeCount);
        std::vector<vec2> sortedWarmTangentImpulses(activeCount);
        std::vector<vec3> sortedTangent1(activeCount);
        std::vector<vec3> sortedTangent2(activeCount);
        std::vector<vec3> sortedPointSlots(activeCount * kMaxContactPointsPerManifold);
        std::vector<f32> sortedPointPenetrations(activeCount * kMaxContactPointsPerManifold);

        for (u32 i = 0u; i < activeCount; ++i) {
            const u32 src = order[i];
            sortedContactPoints[i] = contactPoints[src];
            sortedContactNormals[i] = contactNormals[src];
            sortedPenetrationDepths[i] = penetrationDepths[src];
            sortedMinSeparations[i] = minSeparations[src];
            sortedBodyA[i] = bodyA[src];
            sortedBodyB[i] = bodyB[src];
            sortedValidFlags[i] = validFlags[src];
            sortedPointCounts[i] = pointCounts[src];
            sortedWarmNormalImpulses[i] = warmNormalImpulses[src];
            sortedWarmTangentImpulses[i] = warmTangentImpulses[src];
            sortedTangent1[i] = tangent1[src];
            sortedTangent2[i] = tangent2[src];

            const u32 readBase = pointSlotBase(src);
            const u32 writeBase = i * kMaxContactPointsPerManifold;
            for (u32 pointIndex = 0u; pointIndex < kMaxContactPointsPerManifold; ++pointIndex) {
                sortedPointSlots[writeBase + pointIndex] = pointSlots[readBase + pointIndex];
                sortedPointPenetrations[writeBase + pointIndex] = pointPenetrations[readBase + pointIndex];
            }
        }

        contactPoints.swap(sortedContactPoints);
        contactNormals.swap(sortedContactNormals);
        penetrationDepths.swap(sortedPenetrationDepths);
        minSeparations.swap(sortedMinSeparations);
        bodyA.swap(sortedBodyA);
        bodyB.swap(sortedBodyB);
        validFlags.swap(sortedValidFlags);
        pointCounts.swap(sortedPointCounts);
        warmNormalImpulses.swap(sortedWarmNormalImpulses);
        warmTangentImpulses.swap(sortedWarmTangentImpulses);
        tangent1.swap(sortedTangent1);
        tangent2.swap(sortedTangent2);
        pointSlots.swap(sortedPointSlots);
        pointPenetrations.swap(sortedPointPenetrations);
    }

    const u32 excess = activeCount - maxCapacity;
    droppedCount += excess;
    activeCount = maxCapacity;

    for (u32 i = activeCount; i < pairSlotCount; ++i) {
        validFlags[i] = 0u;
        pointCounts[i] = 0u;
    }

    return activeCount;
}

u32 ContactBufferSoA::compactAndClamp() {
    const ContactBufferCompactionPreflight preflight = preflight_contact_buffer_compaction(*this);
    if (preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer) {
    const ContactBufferCompactAndClampPreflight preflight = preflightContactBufferCompactAndClamp(*this);
    if (preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }
    if (preflight.reason == ContactBufferCompactionRejectReason::NoWork) {
    if (!should_run_contact_buffer_compact_and_clamp(*this)) {
    if (preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork) {

    if (can_skip_contact_buffer_compact_and_clamp(*this)) {
    if (canSkipCompactAndClamp()) {
        return 0u;

    compact();
    if (isEmpty()) {
        return 0u;
    }

    return applyMaxCapacityClamp();
}

ContactManifold ContactBufferSoA::manifoldAt(u32 index) const {
    ContactManifold manifold{};
    if (index >= activeCount || validFlags[index] == 0u) {
        return manifold;
    }

    manifold.contactPoint = contactPoints[index];
    manifold.contactNormal = contactNormals[index];
    manifold.penetrationDepth = penetrationDepths[index];
    manifold.minSeparation = minSeparations[index];
    manifold.bodyA = bodyA[index];
    manifold.bodyB = bodyB[index];
    manifold.warmNormalImpulse = warmNormalImpulses[index];
    manifold.warmTangentImpulse = warmTangentImpulses[index];
    manifold.frictionBasis = tangentBasisAt(index);
    manifold.valid = true;

    const u32 pointCount = std::min(static_cast<u32>(pointCounts[index]), kMaxContactPointsPerManifold);
    manifold.pointCount = pointCount;
    const u32 base = pointSlotBase(index);
    for (u32 pointIndex = 0u; pointIndex < pointCount; ++pointIndex) {
        manifold.points[pointIndex].point = pointSlots[base + pointIndex];
        manifold.points[pointIndex].penetration = pointPenetrations[base + pointIndex];
    }

    return manifold;
}

std::vector<ContactManifold> ContactBufferSoA::toVector() const {
    std::vector<ContactManifold> manifolds;
    manifolds.reserve(activeCount);
    for (u32 i = 0; i < activeCount; ++i) {
        if (validFlags[i] != 0u) {
            manifolds.push_back(manifoldAt(i));
        }
    }
    return manifolds;
}

namespace {

bool contactBufferHasCompactionGaps(const ContactBufferSoA& buffer) {
    u32 writeIndex = 0u;
    for (u32 readIndex = 0u; readIndex < buffer.pairSlotCount; ++readIndex) {
        if (buffer.validFlags[readIndex] != 0u) {
            if (writeIndex != readIndex) {
                return true;
            }
            ++writeIndex;
    return writeIndex != buffer.activeCount;

bool shouldRunContactBufferCompactionWork(const ContactBufferSoA& buffer) {
    if (buffer.pairSlotCount == 0u) {
        return false;
    return contactBufferHasCompactionGaps(buffer);

bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer) {
    return buffer.maxCapacity > 0u && buffer.activeCount > buffer.maxCapacity;

} // namespace
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason) {
bool ContactBufferSoA::canSkipCompaction() const {
    if (pairSlotCount == 0u) {

    for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
        if (validFlags[slot] == 0u) {

bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return maxCapacity > 0u && activeCount > maxCapacity;

const char* contactBufferWriteRejectReasonName(ContactBufferWriteRejectReason reason) {
    switch (reason) {
    case ContactBufferWriteRejectReason::None:
        return "None";
    case ContactBufferWriteRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case ContactBufferWriteRejectReason::OutOfRange:
        return "OutOfRange";
    case ContactBufferWriteRejectReason::InvalidSlot:
        return "InvalidSlot";
    case ContactBufferWriteRejectReason::InvalidManifold:
        return "InvalidManifold";
    case ContactBufferWriteRejectReason::SelfPair:
        return "SelfPair";
    return "Unknown";

ContactBufferWriteRejectReason contact_buffer_write_reject_reason(

ContactBufferWriteRejectReason contactBufferWriteRejectReason(








    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (slot >= buffer.pairSlotCount) {
        return ContactBufferWriteRejectReason::OutOfRangeSlot;
    if (!manifold.valid) {
        return ContactBufferWriteRejectReason::InvalidManifold;
    if (manifold.bodyA == manifold.bodyB) {
        return ContactBufferWriteRejectReason::SelfPair;
    return ContactBufferWriteRejectReason::None;

bool contact_buffer_write_rejects_for_reason(
    const ContactManifold& manifold,
    ContactBufferWriteRejectReason expected) {
    return contact_buffer_write_reject_reason(buffer, slot, manifold) == expected;

ContactBufferWritePreflight preflight_contact_buffer_write(
    ContactBufferWritePreflight preflight{};
    preflight.reason = contact_buffer_write_reject_reason(buffer, slot, manifold);
        return ContactBufferWriteRejectReason::InvalidSlot;

bool contactBufferWriteRejectsForReason(
    return contactBufferWriteRejectReason(buffer, slot, manifold) == expected;

ContactBufferWritePreflight preflightContactBufferWrite(
    preflight.reason = contactBufferWriteRejectReason(buffer, slot, manifold);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWriteRejectReason::OutOfRangeSlot;
    preflight.invalidManifold = preflight.reason == ContactBufferWriteRejectReason::InvalidManifold;
    preflight.selfPair = preflight.reason == ContactBufferWriteRejectReason::SelfPair;
    return preflight;

const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason) {
        return ContactBufferWriteRejectReason::OutOfRange;


    preflight.outOfRange = preflight.reason == ContactBufferWriteRejectReason::OutOfRange;




    preflight.invalidSlot = preflight.reason == ContactBufferWriteRejectReason::InvalidSlot;

const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason) {

bool can_skip_contact_buffer_write(
    return !preflight_contact_buffer_write(buffer, slot, manifold).canWrite();

bool should_run_contact_buffer_write(
    return preflight_contact_buffer_write(buffer, slot, manifold).canWrite();


    case ContactBufferCompactionRejectReason::None:
    case ContactBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactionRejectReason::NoWork:
        return "NoWork";

ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;
    if (!shouldRunContactBufferCompactionWork(buffer) && !shouldRunContactBufferClamp(buffer)) {
        return ContactBufferCompactionRejectReason::NoWork;
    return ContactBufferCompactionRejectReason::None;
    case ContactBufferCompactionRejectReason::AllValid:
        return "AllValid";

    if (buffer.canSkipSoAIteration()) {
    if (buffer.canSkipCompaction()) {
        return ContactBufferCompactionRejectReason::AllValid;
    case ContactBufferCompactionRejectReason::AllInvalid:
        return "AllInvalid";


    for (u32 i = 0u; i < buffer.pairSlotCount; ++i) {
        if (buffer.validFlags[i] != 0u) {
    return ContactBufferCompactionRejectReason::AllInvalid;

bool contact_buffer_compaction_rejects_for_reason(
    ContactBufferCompactionRejectReason expected) {
    return contact_buffer_compaction_reject_reason(buffer) == expected;
ContactBufferWritePreflight preflight_contact_buffer_write(const ContactManifold& manifold) {
    preflight.invalidManifold = !manifold.valid;
    preflight.selfPair = manifold.bodyA == manifold.bodyB;





    }

    const ContactBufferSoA& buffer,
    u32 slot,

    const ContactManifold& manifold) {

bool write_contact_buffer_slot_with_preflight(
    ContactBufferSoA& buffer,
    if (!preflight_contact_buffer_write(buffer, slot, manifold).canWrite()) {
        return false;
    buffer.writeSlot(slot, manifold);
    return true;

    switch (reason) {
        return "None";
    return "Unknown";



ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    ContactBufferCompactionPreflight preflight{};
    preflight.reason = contact_buffer_compaction_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactionRejectReason::NoWork;

bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compaction(buffer).needsCompaction();

bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compaction(buffer).needsCompaction();

const char* contact_buffer_friction_basis_reject_reason_name(ContactBufferFrictionBasisRejectReason reason) {
    case ContactBufferFrictionBasisRejectReason::None:
    case ContactBufferFrictionBasisRejectReason::EmptyBuffer:
    case ContactBufferFrictionBasisRejectReason::NoValidManifolds:
        return "NoValidManifolds";

ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.activeCount == 0u) {
        return ContactBufferFrictionBasisRejectReason::EmptyBuffer;

    for (u32 slot = 0u; slot < buffer.activeCount; ++slot) {
        if (buffer.validFlags[slot] != 0u) {
            return ContactBufferFrictionBasisRejectReason::None;

    return ContactBufferFrictionBasisRejectReason::NoValidManifolds;

bool contact_buffer_friction_basis_rejects_for_reason(
    ContactBufferFrictionBasisRejectReason expected) {
    return contact_buffer_friction_basis_reject_reason(buffer) == expected;

ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_basis(const ContactBufferSoA& buffer) {
    ContactBufferFrictionBasisPreflight preflight{};
    preflight.reason = contact_buffer_friction_basis_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBasisRejectReason::EmptyBuffer;
    preflight.noValidManifolds = preflight.reason == ContactBufferFrictionBasisRejectReason::NoValidManifolds;

bool can_skip_contact_buffer_friction_basis(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_friction_basis(buffer).needsRebuild();

bool should_run_contact_buffer_friction_basis(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_friction_basis(buffer).needsRebuild();
    preflight.emptyBuffer = buffer.pairSlotCount == 0u;
    preflight.allValid = !preflight.emptyBuffer && buffer.canSkipCompaction();

ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.emptyBuffer = buffer.activeCount == 0u;
    preflight.withinCapacity = preflight.emptyBuffer || buffer.canSkipMaxCapacityClamp();

ContactBufferFrictionPreflight preflight_contact_buffer_friction_rebuild(
    f32 epsilon) {
    ContactBufferFrictionPreflight preflight{};
    if (preflight.emptyBuffer) {

        if (buffer.validFlags[slot] == 0u) {
            continue;

        const ContactManifold manifold = buffer.manifoldAt(slot);
        if (friction_basis_is_stale(manifold, epsilon)) {
            ++preflight.staleSlotCount;
        if (needs_friction_basis_refresh(manifold, epsilon)) {
            ++preflight.rebuildSlotCount;

bool can_skip_build_friction_tangent_bases(
    return preflight_contact_buffer_friction_rebuild(buffer, epsilon).can_skip_rebuild();
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;

bool canSkipContactBufferCompaction(const ContactBufferSoA& buffer) {

bool shouldRunContactBufferCompaction(const ContactBufferSoA& buffer) {

const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason) {
    case ContactBufferClampRejectReason::None:
    case ContactBufferClampRejectReason::EmptyBuffer:
    case ContactBufferClampRejectReason::WithinCapacity:
        return "WithinCapacity";

ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer) {
        return ContactBufferClampRejectReason::EmptyBuffer;
    if (buffer.canSkipMaxCapacityClamp()) {
        return ContactBufferClampRejectReason::WithinCapacity;
    return ContactBufferClampRejectReason::None;

bool contact_buffer_clamp_rejects_for_reason(
    ContactBufferClampRejectReason expected) {
    return contact_buffer_clamp_reject_reason(buffer) == expected;

    preflight.reason = contact_buffer_clamp_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;

bool canSkipContactBufferClamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_clamp(buffer).needsClamp();

    return preflight_contact_buffer_clamp(buffer).needsClamp();

ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer) {
    if (contactBufferIsEmpty(buffer)) {
    if (contactBufferAllInvalid(buffer)) {

bool contactBufferCompactionRejectsForReason(
    return contactBufferCompactionRejectReason(buffer) == expected;

ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferCompactionRejectReason(buffer);
    preflight.allInvalid = preflight.reason == ContactBufferCompactionRejectReason::AllInvalid;

    return !preflightContactBufferCompaction(buffer).needsCompaction();

    return preflightContactBufferCompaction(buffer).needsCompaction();

const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason) {

ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer) {
    if (!contactBufferNeedsClamp(buffer)) {

bool contactBufferClampRejectsForReason(
    return contactBufferClampRejectReason(buffer) == expected;

ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferClampRejectReason(buffer);

    return !preflightContactBufferClamp(buffer).needsClamp();








    if (!buffer.canApplyMaxCapacityClamp()) {




    return preflightContactBufferClamp(buffer).needsClamp();



const char* contactBufferCompactRejectReasonName(ContactBufferCompactRejectReason reason) {
    case ContactBufferCompactRejectReason::None:
    case ContactBufferCompactRejectReason::EmptyBuffer:
    case ContactBufferCompactRejectReason::AllValid:

ContactBufferCompactRejectReason contactBufferCompactRejectReason(const ContactBufferSoA& buffer) {
        return ContactBufferCompactRejectReason::EmptyBuffer;
        return ContactBufferCompactRejectReason::AllValid;
    return ContactBufferCompactRejectReason::None;

bool contactBufferCompactRejectsForReason(
    ContactBufferCompactRejectReason expected) {
    return contactBufferCompactRejectReason(buffer) == expected;





bool canSkipContactBufferWrite(
    return !preflightContactBufferWrite(buffer, slot, manifold).canWrite();

bool shouldRunContactBufferWrite(
    return preflightContactBufferWrite(buffer, slot, manifold).canWrite();

ContactBufferCompactPreflight preflightContactBufferCompact(const ContactBufferSoA& buffer) {
    ContactBufferCompactPreflight preflight{};
    preflight.reason = contactBufferCompactRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferCompactRejectReason::AllValid;

bool canSkipContactBufferCompact(const ContactBufferSoA& buffer) {
    return !preflightContactBufferCompact(buffer).needsCompaction();

bool shouldRunContactBufferCompact(const ContactBufferSoA& buffer) {
    return preflightContactBufferCompact(buffer).needsCompaction();







    return contactBufferCompactionRejectReason(buffer) != ContactBufferCompactionRejectReason::None;

    return contactBufferCompactionRejectReason(buffer) == ContactBufferCompactionRejectReason::None;





    return contactBufferClampRejectReason(buffer) != ContactBufferClampRejectReason::None;

    return contactBufferClampRejectReason(buffer) == ContactBufferClampRejectReason::None;
ContactBufferCompactionPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer) {
        preflight.skipped = true;
        preflight.emptySlots = true;

    const u32 scanCount = buffer.pairSlotCount > 0u ? buffer.pairSlotCount : buffer.activeCount;
    if (scanCount == 0u) {

    preflight.allValid = buffer.canSkipCompaction();
    preflight.needsCompaction = !preflight.allValid;

bool should_skip_contact_buffer_compact(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compact(buffer).can_skip_compaction();

        preflight.emptyBuffer = true;

    preflight.withinCapacity = buffer.canSkipMaxCapacityClamp();
    preflight.needsClamp = !preflight.withinCapacity;

bool should_skip_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_clamp(buffer).can_skip_clamp();

ContactBufferFrictionPreflight preflight_contact_buffer_friction_tangents(const ContactBufferSoA& buffer) {

    preflight.slotCount = buffer.activeCount;

        const TangentBasis existing{buffer.tangent1[slot], buffer.tangent2[slot]};
        if (!tangentBasisMatchesNormal(buffer.contactNormals[slot], existing)) {
            ++preflight.needsRebuildCount;

bool should_skip_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_friction_tangents(buffer).can_skip_rebuild();


















bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer) {






    bool anyValid = false;
            anyValid = true;
            break;
    if (!anyValid) {






    if (buffer.maxCapacity == 0u || buffer.activeCount <= buffer.maxCapacity) {




const char* contact_buffer_compact_reject_reason_name(ContactBufferCompactRejectReason reason) {

ContactBufferCompactRejectReason contact_buffer_compact_reject_reason(const ContactBufferSoA& buffer) {

bool contact_buffer_compact_rejects_for_reason(
    return contact_buffer_compact_reject_reason(buffer) == expected;

ContactBufferCompactPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer) {
    preflight.reason = contact_buffer_compact_reject_reason(buffer);

bool can_skip_contact_buffer_compact(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compact(buffer).needsCompaction();

bool should_run_contact_buffer_compact(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compact(buffer).needsCompaction();






bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer) {

const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason) {
    case ContactBufferCompactAndClampRejectReason::None:
    case ContactBufferCompactAndClampRejectReason::EmptyBuffer:
    case ContactBufferCompactAndClampRejectReason::NoWork:

ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
        return ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    if (!should_run_contact_buffer_compact(buffer) && !should_run_contact_buffer_clamp(buffer)) {
        const u32 validCount = buffer.countValidSlots();
        if (buffer.pairSlotCount > 0u && buffer.activeCount != validCount) {
            return ContactBufferCompactAndClampRejectReason::None;
        if (buffer.activeCount != validCount) {
        return ContactBufferCompactAndClampRejectReason::NoWork;

bool contact_buffer_compact_and_clamp_rejects_for_reason(
    ContactBufferCompactAndClampRejectReason expected) {
    return contact_buffer_compact_and_clamp_reject_reason(buffer) == expected;
    return preflight;
}



    switch (reason) {
        return "None";
        return "EmptyBuffer";
    return "Unknown";

    if (buffer.canSkipSoAIteration()) {

    const ContactBufferSoA& buffer,




        return "NoWork";

    if (!should_run_contact_buffer_compaction(buffer) && !should_run_contact_buffer_clamp(buffer)) {


ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    ContactBufferCompactAndClampPreflight preflight{};
    preflight.reason = contact_buffer_compact_and_clamp_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork;

bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compact_and_clamp(buffer).needsCompactAndClamp();

bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compact_and_clamp(buffer).needsCompactAndClamp();

const char* contact_buffer_friction_build_reject_reason_name(ContactBufferFrictionBuildRejectReason reason) {
    case ContactBufferFrictionBuildRejectReason::None:
    case ContactBufferFrictionBuildRejectReason::EmptyBuffer:
    case ContactBufferFrictionBuildRejectReason::NoValidSlots:
        return "NoValidSlots";

ContactBufferFrictionBuildRejectReason contact_buffer_friction_build_reject_reason(const ContactBufferSoA& buffer) {
        return ContactBufferFrictionBuildRejectReason::EmptyBuffer;
    if (buffer.countValidSlots() == 0u) {
        return ContactBufferFrictionBuildRejectReason::NoValidSlots;
    return ContactBufferFrictionBuildRejectReason::None;

bool contact_buffer_friction_build_rejects_for_reason(
    ContactBufferFrictionBuildRejectReason expected) {
    return contact_buffer_friction_build_reject_reason(buffer) == expected;

ContactBufferFrictionBuildPreflight preflight_contact_buffer_friction_build(const ContactBufferSoA& buffer) {
    ContactBufferFrictionBuildPreflight preflight{};
    preflight.reason = contact_buffer_friction_build_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBuildRejectReason::EmptyBuffer;
    preflight.noValidSlots = preflight.reason == ContactBufferFrictionBuildRejectReason::NoValidSlots;

bool can_skip_contact_buffer_friction_build(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_friction_build(buffer).needsFrictionBuild();

bool should_run_contact_buffer_friction_build(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_friction_build(buffer).needsFrictionBuild();
    }

    const ContactBufferSoA& buffer,
    u32 slot,

    const ContactManifold& manifold) {

    switch (reason) {
        return "None";
    return "Unknown";











bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer) {

const char* contactBufferCompactAndClampRejectReasonName(ContactBufferCompactAndClampRejectReason reason) {

ContactBufferCompactAndClampRejectReason contactBufferCompactAndClampRejectReason(
    if (!shouldRunContactBufferCompaction(buffer) && !shouldRunContactBufferClamp(buffer)) {

bool contactBufferCompactAndClampRejectsForReason(
    return contactBufferCompactAndClampRejectReason(buffer) == expected;

ContactBufferCompactAndClampPreflight preflightContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferCompactAndClampRejectReason(buffer);

bool canSkipContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    return !preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();

bool shouldRunContactBufferCompactAndClamp(const ContactBufferSoA& buffer) {
    return preflightContactBufferCompactAndClamp(buffer).needsCompactAndClamp();
    return preflight;



u32 compact_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    const ContactBufferCompactionPreflight preflight = preflight_contact_buffer_compaction(buffer);
    if (preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer) {
        buffer.activeCount = 0u;
        return buffer.activeCount;
    if (!preflight.needsCompaction()) {
    return buffer.compact();

u32 compact_and_clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    if (can_skip_contact_buffer_compact_and_clamp(buffer)) {
    return buffer.compactAndClamp();
}

} // namespace fuse::physics::narrowphase
