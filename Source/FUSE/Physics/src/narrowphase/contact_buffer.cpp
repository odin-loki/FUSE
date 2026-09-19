#include <fuse/physics/narrowphase/contact_buffer.hpp>

#include <algorithm>

namespace fuse::physics::narrowphase {

void ContactBufferSoA::setMaxCapacity(u32 capacity) {
    maxCapacity = capacity;
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

void ContactBufferSoA::writeSlot(u32 slot, const ContactManifold& manifold) {
    if (slot >= pairSlotCount || !manifold.valid || manifold.bodyA == manifold.bodyB) {
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

void ContactBufferSoA::applyWarmStartStub(u32 slot, ContactManifold& manifold) const {
    if (slot >= pairSlotCount || validFlags[slot] == 0u) {
        return;
    }

    manifold.warmNormalImpulse = warmNormalImpulses[slot];
    manifold.warmTangentImpulse = warmTangentImpulses[slot];
    manifold.frictionBasis = tangentBasisAt(slot);
}

void ContactBufferSoA::buildFrictionTangentBases() {
    if (!should_run_contact_buffer_friction_basis(*this)) {
        return;
    }

    for (u32 slot = 0u; slot < activeCount; ++slot) {
        if (validFlags[slot] == 0u) {
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

u32 ContactBufferSoA::compact() {
    u32 writeIndex = 0;
    for (u32 readIndex = 0; readIndex < pairSlotCount; ++readIndex) {
        if (validFlags[readIndex] == 0u) {
            continue;
        }
        if (writeIndex != readIndex) {
            contactPoints[writeIndex] = contactPoints[readIndex];
            contactNormals[writeIndex] = contactNormals[readIndex];
            penetrationDepths[writeIndex] = penetrationDepths[readIndex];
            minSeparations[writeIndex] = minSeparations[readIndex];
            bodyA[writeIndex] = bodyA[readIndex];
            bodyB[writeIndex] = bodyB[readIndex];
            validFlags[writeIndex] = 1u;
            pointCounts[writeIndex] = pointCounts[readIndex];
            warmNormalImpulses[writeIndex] = warmNormalImpulses[readIndex];
            warmTangentImpulses[writeIndex] = warmTangentImpulses[readIndex];
            tangent1[writeIndex] = tangent1[readIndex];
            tangent2[writeIndex] = tangent2[readIndex];

            const u32 readBase = pointSlotBase(readIndex);
            const u32 writeBase = pointSlotBase(writeIndex);
            for (u32 pointIndex = 0u; pointIndex < kMaxContactPointsPerManifold; ++pointIndex) {
                pointSlots[writeBase + pointIndex] = pointSlots[readBase + pointIndex];
                pointPenetrations[writeBase + pointIndex] = pointPenetrations[readBase + pointIndex];
            }
        }
        ++writeIndex;
    }

    activeCount = writeIndex;
    for (u32 i = activeCount; i < pairSlotCount; ++i) {
        validFlags[i] = 0u;
        pointCounts[i] = 0u;
    }
    return activeCount;
}

u32 ContactBufferSoA::applyMaxCapacityClamp() {
    if (maxCapacity == 0u || activeCount <= maxCapacity) {
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
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }
    if (preflight.reason == ContactBufferCompactionRejectReason::NoWork) {
        return activeCount;
    }

    compact();
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
        }
    }
    return writeIndex != buffer.activeCount;
}

bool shouldRunContactBufferCompactionWork(const ContactBufferSoA& buffer) {
    if (buffer.pairSlotCount == 0u) {
        return false;
    }
    return contactBufferHasCompactionGaps(buffer);
}

bool shouldRunContactBufferClamp(const ContactBufferSoA& buffer) {
    return buffer.maxCapacity > 0u && buffer.activeCount > buffer.maxCapacity;
}

} // namespace

const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason) {
    switch (reason) {
    case ContactBufferCompactionRejectReason::None:
        return "None";
    case ContactBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactionRejectReason::NoWork:
        return "NoWork";
    }
    return "Unknown";
}

ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer) {
    if (buffer.pairSlotCount == 0u) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;
    }
    if (!shouldRunContactBufferCompactionWork(buffer) && !shouldRunContactBufferClamp(buffer)) {
        return ContactBufferCompactionRejectReason::NoWork;
    }
    return ContactBufferCompactionRejectReason::None;
}

bool contact_buffer_compaction_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactionRejectReason expected) {
    return contact_buffer_compaction_reject_reason(buffer) == expected;
}

ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    ContactBufferCompactionPreflight preflight{};
    preflight.reason = contact_buffer_compaction_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactionRejectReason::NoWork;
    return preflight;
}

bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compaction(buffer).needsCompaction();
}

bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compaction(buffer).needsCompaction();
}

const char* contact_buffer_friction_basis_reject_reason_name(ContactBufferFrictionBasisRejectReason reason) {
    switch (reason) {
    case ContactBufferFrictionBasisRejectReason::None:
        return "None";
    case ContactBufferFrictionBasisRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferFrictionBasisRejectReason::NoValidManifolds:
        return "NoValidManifolds";
    }
    return "Unknown";
}

ContactBufferFrictionBasisRejectReason contact_buffer_friction_basis_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.activeCount == 0u) {
        return ContactBufferFrictionBasisRejectReason::EmptyBuffer;
    }

    for (u32 slot = 0u; slot < buffer.activeCount; ++slot) {
        if (buffer.validFlags[slot] != 0u) {
            return ContactBufferFrictionBasisRejectReason::None;
        }
    }

    return ContactBufferFrictionBasisRejectReason::NoValidManifolds;
}

bool contact_buffer_friction_basis_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasisRejectReason expected) {
    return contact_buffer_friction_basis_reject_reason(buffer) == expected;
}

ContactBufferFrictionBasisPreflight preflight_contact_buffer_friction_basis(const ContactBufferSoA& buffer) {
    ContactBufferFrictionBasisPreflight preflight{};
    preflight.reason = contact_buffer_friction_basis_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBasisRejectReason::EmptyBuffer;
    preflight.noValidManifolds = preflight.reason == ContactBufferFrictionBasisRejectReason::NoValidManifolds;
    return preflight;
}

bool can_skip_contact_buffer_friction_basis(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_friction_basis(buffer).needsRebuild();
}

bool should_run_contact_buffer_friction_basis(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_friction_basis(buffer).needsRebuild();
}

} // namespace fuse::physics::narrowphase

// --- deepen additive from deepen-b4-narrowphase-guards-914a ---
        const FrictionBasisPreflight preflight = preflight_friction_basis_rebuild(manifold, epsilon);

// --- deepen additive from deepen-b4-narrowphase-guards-7360 ---
        if (should_skip_friction_basis_preflight(manifold)) {

// --- deepen additive from deepen-b4-narrowphase-guards-b463 ---
ContactBufferWritePreflight preflight_contact_buffer_write(const ContactManifold& manifold) {
    ContactBufferWritePreflight preflight{};
ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
ContactBufferFrictionPreflight preflight_contact_buffer_friction_rebuild(
    ContactBufferFrictionPreflight preflight{};

// --- deepen additive from deepen-b4-narrowphase-guard-pass-9852 ---
    const ContactBufferWritePreflight writePreflight = preflight_contact_buffer_write(*this, slot, manifold);
    if (!writePreflight.canWrite()) {
    const ContactBufferCompactionPreflight compactionPreflight = preflight_contact_buffer_compaction(*this);
    if (compactionPreflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer) {
    if (compactionPreflight.reason == ContactBufferCompactionRejectReason::AllValid) {
const char* contact_buffer_write_reject_reason_name(ContactBufferWriteRejectReason reason) {
    case ContactBufferWriteRejectReason::None:
    case ContactBufferWriteRejectReason::OutOfRangeSlot:
    case ContactBufferWriteRejectReason::InvalidManifold:
    case ContactBufferWriteRejectReason::SelfPair:
ContactBufferWriteRejectReason contact_buffer_write_reject_reason(
        return ContactBufferWriteRejectReason::OutOfRangeSlot;
        return ContactBufferWriteRejectReason::InvalidManifold;
        return ContactBufferWriteRejectReason::SelfPair;
    return ContactBufferWriteRejectReason::None;
    ContactBufferWriteRejectReason expected) {
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWriteRejectReason::OutOfRangeSlot;
    preflight.invalidManifold = preflight.reason == ContactBufferWriteRejectReason::InvalidManifold;
    preflight.selfPair = preflight.reason == ContactBufferWriteRejectReason::SelfPair;
    case ContactBufferCompactionRejectReason::AllValid:
        return ContactBufferCompactionRejectReason::AllValid;
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;
const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason) {
    case ContactBufferClampRejectReason::None:
    case ContactBufferClampRejectReason::EmptyBuffer:
    case ContactBufferClampRejectReason::WithinCapacity:
ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer) {
        return ContactBufferClampRejectReason::EmptyBuffer;
        return ContactBufferClampRejectReason::WithinCapacity;
    return ContactBufferClampRejectReason::None;
    ContactBufferClampRejectReason expected) {
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;

// --- deepen additive from b4-narrowphase-deepen-guards-046d ---
const char* contactBufferWriteRejectReasonName(ContactBufferWriteRejectReason reason) {
    case ContactBufferWriteRejectReason::InvalidSlot:
ContactBufferWriteRejectReason contactBufferWriteRejectReason(
        return ContactBufferWriteRejectReason::InvalidSlot;
    return contactBufferWriteRejectReason(buffer, slot, manifold) == expected;
ContactBufferWritePreflight preflightContactBufferWrite(
    preflight.reason = contactBufferWriteRejectReason(buffer, slot, manifold);
    preflight.invalidSlot = preflight.reason == ContactBufferWriteRejectReason::InvalidSlot;
const char* contactBufferCompactionRejectReasonName(ContactBufferCompactionRejectReason reason) {
ContactBufferCompactionRejectReason contactBufferCompactionRejectReason(const ContactBufferSoA& buffer) {
    return contactBufferCompactionRejectReason(buffer) == expected;
ContactBufferCompactionPreflight preflightContactBufferCompaction(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferCompactionRejectReason(buffer);
    return !preflightContactBufferCompaction(buffer).needsCompaction();
    return preflightContactBufferCompaction(buffer).needsCompaction();
const char* contactBufferClampRejectReasonName(ContactBufferClampRejectReason reason) {
ContactBufferClampRejectReason contactBufferClampRejectReason(const ContactBufferSoA& buffer) {
    return contactBufferClampRejectReason(buffer) == expected;
ContactBufferClampPreflight preflightContactBufferClamp(const ContactBufferSoA& buffer) {
    preflight.reason = contactBufferClampRejectReason(buffer);
    return !preflightContactBufferClamp(buffer).needsClamp();
    return preflightContactBufferClamp(buffer).needsClamp();

// --- deepen additive from deepen-b4-narrowphase-guards-bc5b ---
    const ContactBufferWritePreflight preflight = preflightContactBufferWrite(*this, slot, manifold);
    const ContactBufferCompactionPreflight compactionPreflight = preflightContactBufferCompaction(*this);
    if (compactionPreflight.reason == ContactBufferCompactionRejectReason::AllInvalid) {
    const ContactBufferClampPreflight clampPreflight = preflightContactBufferClamp(*this);
    if (!clampPreflight.needsClamp()) {
    case ContactBufferWriteRejectReason::OutOfRange:
        return ContactBufferWriteRejectReason::OutOfRange;
    preflight.outOfRange = preflight.reason == ContactBufferWriteRejectReason::OutOfRange;
    case ContactBufferCompactionRejectReason::AllInvalid:
        return ContactBufferCompactionRejectReason::AllInvalid;
    preflight.allInvalid = preflight.reason == ContactBufferCompactionRejectReason::AllInvalid;

// --- deepen additive from deepen-b4-narrowphase-guards-b130 ---
    return !preflightContactBufferWrite(*this, slot, manifold).canWrite();
    const ContactBufferWritePreflight writePreflight = preflightContactBufferWrite(*this, slot, manifold);
    return contactBufferCompactionRejectReason(*this) != ContactBufferCompactionRejectReason::None;
    return contactBufferClampRejectReason(*this) != ContactBufferClampRejectReason::None;
    if (clampPreflight.reason != ContactBufferClampRejectReason::None) {

// --- deepen additive from b4-narrowphase-deepen-guards-4d64 ---
    if (!preflightContactBufferClamp(*this).needsClamp()) {

// --- deepen additive from b4-narrowphase-guard-pass-0376 ---
    const ContactBufferWritePreflight preflight = preflight_contact_buffer_write(*this, slot, manifold);

// --- deepen additive from deepen-b4-narrowphase-guards-37c2 ---
    if (!preflightContactBufferWrite(*this, slot, manifold).canWrite()) {
    const ContactBufferCompactPreflight compactionPreflight = preflightContactBufferCompact(*this);
    if (compactionPreflight.reason == ContactBufferCompactRejectReason::EmptyBuffer) {
    if (compactionPreflight.reason == ContactBufferCompactRejectReason::AllValid) {
const char* contactBufferCompactRejectReasonName(ContactBufferCompactRejectReason reason) {
    case ContactBufferCompactRejectReason::None:
    case ContactBufferCompactRejectReason::EmptyBuffer:
    case ContactBufferCompactRejectReason::AllValid:
ContactBufferCompactRejectReason contactBufferCompactRejectReason(const ContactBufferSoA& buffer) {
        return ContactBufferCompactRejectReason::EmptyBuffer;
        return ContactBufferCompactRejectReason::AllValid;
    return ContactBufferCompactRejectReason::None;
    ContactBufferCompactRejectReason expected) {
    return contactBufferCompactRejectReason(buffer) == expected;
    return !preflightContactBufferWrite(buffer, slot, manifold).canWrite();
    return preflightContactBufferWrite(buffer, slot, manifold).canWrite();
ContactBufferCompactPreflight preflightContactBufferCompact(const ContactBufferSoA& buffer) {
    ContactBufferCompactPreflight preflight{};
    preflight.reason = contactBufferCompactRejectReason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactRejectReason::EmptyBuffer;
    preflight.allValid = preflight.reason == ContactBufferCompactRejectReason::AllValid;
    return !preflightContactBufferCompact(buffer).needsCompaction();
    return preflightContactBufferCompact(buffer).needsCompaction();

// --- deepen additive from deepen-b4-narrowphase-guards-e6d2 ---
    if (contactBufferWriteRejectReason(*this, slot, manifold) != ContactBufferWriteRejectReason::None) {
    if (contactBufferClampRejectReason(*this) != ContactBufferClampRejectReason::None) {
    return contactBufferCompactionRejectReason(buffer) != ContactBufferCompactionRejectReason::None;
    return contactBufferCompactionRejectReason(buffer) == ContactBufferCompactionRejectReason::None;
    return contactBufferClampRejectReason(buffer) != ContactBufferClampRejectReason::None;
    return contactBufferClampRejectReason(buffer) == ContactBufferClampRejectReason::None;

// --- deepen additive from deepen-b4-narrowphase-guards-26f2 ---
ContactBufferCompactionPreflight preflight_contact_buffer_compact(const ContactBufferSoA& buffer) {
bool should_skip_contact_buffer_compact(const ContactBufferSoA& buffer) {
bool should_skip_contact_buffer_clamp(const ContactBufferSoA& buffer) {
ContactBufferFrictionPreflight preflight_contact_buffer_friction_tangents(const ContactBufferSoA& buffer) {
bool should_skip_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer) {

// --- deepen additive from deepen-b4-narrowphase-guards-c6ee ---
bool should_skip_contact_buffer_write(

// --- deepen additive from deepen-b4-narrowphase-guards-7d67 ---
bool ContactBufferSoA::writeSlotWithPreflight(u32 slot, const ContactManifold& manifold) {
