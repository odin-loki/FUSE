#include <fuse/physics/narrowphase/contact_buffer.hpp>

#include <algorithm>
#include <climits>

namespace fuse::physics::narrowphase {

namespace {

bool tangentBasisIsBuilt(vec3 normal, vec3 tangent1, vec3 tangent2) {
    if (normal.length() < 1e-8f) {
        return false;
    }
    return isOrthonormalTangentBasis(normal, {tangent1, tangent2});
}

} // namespace

u32 ContactBufferSoA::remainingCapacity() const {
    if (maxCapacity == 0u) {
        return UINT32_MAX;
    }
    return activeCount < maxCapacity ? maxCapacity - activeCount : 0u;
}

bool ContactBufferSoA::canApplyMaxCapacityClamp() const {
    return !canSkipSoAIteration() && maxCapacity > 0u && activeCount > maxCapacity;
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
        if (validFlags[slot] == 0u) {
            return false;
        }
    }
    return true;
}

bool ContactBufferSoA::canSkipCompactAndClamp() const {
    return canSkipSoAIteration() || countValidSlots() == 0u;
}

u32 ContactBufferSoA::countValidSlots() const {
    if (canSkipSoAIteration()) {
        return 0u;
    }

    const u32 scanCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
    u32 validCount = 0u;
    for (u32 slot = 0; slot < scanCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
        }
    }
    return validCount;
}

bool ContactBufferSoA::slotIsValid(u32 slot) const {
    if (canSkipSoAIteration()) {
        return false;
    }
    if (pairSlotCount > 0u && slot >= pairSlotCount) {
        return false;
    }
    return validFlags[slot] != 0u;
}

const char* contact_buffer_write_slot_reject_reason_name(ContactBufferWriteSlotRejectReason reason) {
    switch (reason) {
    case ContactBufferWriteSlotRejectReason::None:
        return "None";
    case ContactBufferWriteSlotRejectReason::OutOfRangeSlot:
        return "OutOfRangeSlot";
    case ContactBufferWriteSlotRejectReason::InvalidManifold:
        return "InvalidManifold";
    case ContactBufferWriteSlotRejectReason::SelfPair:
        return "SelfPair";
    }
    return "Unknown";
}

ContactBufferWriteSlotRejectReason contact_buffer_write_slot_reject_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    if (buffer.pairSlotCount == 0u || slot >= buffer.pairSlotCount) {
        return ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    }
    if (!manifold.valid) {
        return ContactBufferWriteSlotRejectReason::InvalidManifold;
    }
    if (manifold.bodyA == manifold.bodyB) {
        return ContactBufferWriteSlotRejectReason::SelfPair;
    }
    return ContactBufferWriteSlotRejectReason::None;
}

bool contact_buffer_write_slot_rejects_for_reason(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold,
    ContactBufferWriteSlotRejectReason expected) {
    return contact_buffer_write_slot_reject_reason(buffer, slot, manifold) == expected;
}

ContactBufferWriteSlotPreflight preflight_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    ContactBufferWriteSlotPreflight preflight{};
    preflight.reason = contact_buffer_write_slot_reject_reason(buffer, slot, manifold);
    preflight.outOfRangeSlot = preflight.reason == ContactBufferWriteSlotRejectReason::OutOfRangeSlot;
    preflight.invalidManifold = preflight.reason == ContactBufferWriteSlotRejectReason::InvalidManifold;
    preflight.selfPair = preflight.reason == ContactBufferWriteSlotRejectReason::SelfPair;
    return preflight;
}

bool should_skip_contact_buffer_write_slot(
    const ContactBufferSoA& buffer,
    u32 slot,
    const ContactManifold& manifold) {
    return !preflight_contact_buffer_write_slot(buffer, slot, manifold).canWrite();
}

const char* contact_buffer_compaction_reject_reason_name(ContactBufferCompactionRejectReason reason) {
    switch (reason) {
    case ContactBufferCompactionRejectReason::None:
        return "None";
    case ContactBufferCompactionRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactionRejectReason::AllValid:
        return "AllValid";
    }
    return "Unknown";
}

ContactBufferCompactionRejectReason contact_buffer_compaction_reject_reason(const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactionRejectReason::EmptyBuffer;
    }
    if (buffer.canSkipCompaction()) {
        return ContactBufferCompactionRejectReason::AllValid;
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
    preflight.allValid = preflight.reason == ContactBufferCompactionRejectReason::AllValid;
    return preflight;
}

bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compaction(buffer).needsCompaction();
}

bool should_run_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compaction(buffer).needsCompaction();
}

const char* contact_buffer_clamp_reject_reason_name(ContactBufferClampRejectReason reason) {
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

ContactBufferClampRejectReason contact_buffer_clamp_reject_reason(const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferClampRejectReason::EmptyBuffer;
    }
    if (!buffer.canApplyMaxCapacityClamp()) {
        return ContactBufferClampRejectReason::WithinCapacity;
    }
    return ContactBufferClampRejectReason::None;
}

bool contact_buffer_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferClampRejectReason expected) {
    return contact_buffer_clamp_reject_reason(buffer) == expected;
}

ContactBufferClampPreflight preflight_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    ContactBufferClampPreflight preflight{};
    preflight.reason = contact_buffer_clamp_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferClampRejectReason::EmptyBuffer;
    preflight.withinCapacity = preflight.reason == ContactBufferClampRejectReason::WithinCapacity;
    return preflight;
}

bool can_skip_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_clamp(buffer).needsClamp();
}

bool should_run_contact_buffer_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_clamp(buffer).needsClamp();
}

const char* contact_buffer_friction_bases_reject_reason_name(ContactBufferFrictionBasesRejectReason reason) {
    switch (reason) {
    case ContactBufferFrictionBasesRejectReason::None:
        return "None";
    case ContactBufferFrictionBasesRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferFrictionBasesRejectReason::AllBuilt:
        return "AllBuilt";
    }
    return "Unknown";
}

ContactBufferFrictionBasesRejectReason contact_buffer_friction_bases_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration() || buffer.activeCount == 0u) {
        return ContactBufferFrictionBasesRejectReason::EmptyBuffer;
    }

    for (u32 slot = 0u; slot < buffer.activeCount; ++slot) {
        if (buffer.validFlags[slot] == 0u) {
            continue;
        }
        if (!tangentBasisIsBuilt(
                buffer.contactNormals[slot],
                buffer.tangent1[slot],
                buffer.tangent2[slot])) {
            return ContactBufferFrictionBasesRejectReason::None;
        }
    }

    return ContactBufferFrictionBasesRejectReason::AllBuilt;
}

bool contact_buffer_friction_bases_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferFrictionBasesRejectReason expected) {
    return contact_buffer_friction_bases_reject_reason(buffer) == expected;
}

ContactBufferFrictionBasesPreflight preflight_contact_buffer_friction_bases(const ContactBufferSoA& buffer) {
    ContactBufferFrictionBasesPreflight preflight{};
    preflight.reason = contact_buffer_friction_bases_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferFrictionBasesRejectReason::EmptyBuffer;
    preflight.allBuilt = preflight.reason == ContactBufferFrictionBasesRejectReason::AllBuilt;
    return preflight;
}

bool can_skip_contact_buffer_friction_bases(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_friction_bases(buffer).needsBuild();
}

bool should_run_contact_buffer_friction_bases(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_friction_bases(buffer).needsBuild();
}

const char* contact_buffer_compact_and_clamp_reject_reason_name(ContactBufferCompactAndClampRejectReason reason) {
    switch (reason) {
    case ContactBufferCompactAndClampRejectReason::None:
        return "None";
    case ContactBufferCompactAndClampRejectReason::EmptyBuffer:
        return "EmptyBuffer";
    case ContactBufferCompactAndClampRejectReason::NoWork:
        return "NoWork";
    }
    return "Unknown";
}

ContactBufferCompactAndClampRejectReason contact_buffer_compact_and_clamp_reject_reason(
    const ContactBufferSoA& buffer) {
    if (buffer.canSkipSoAIteration()) {
        return ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    }
    if (!should_run_contact_buffer_compaction(buffer) && !should_run_contact_buffer_clamp(buffer)) {
        const u32 validCount = buffer.countValidSlots();
        if (buffer.pairSlotCount > 0u && buffer.activeCount != validCount) {
            return ContactBufferCompactAndClampRejectReason::None;
        }
        if (buffer.activeCount != validCount) {
            return ContactBufferCompactAndClampRejectReason::None;
        }
        return ContactBufferCompactAndClampRejectReason::NoWork;
    }
    return ContactBufferCompactAndClampRejectReason::None;
}

bool contact_buffer_compact_and_clamp_rejects_for_reason(
    const ContactBufferSoA& buffer,
    ContactBufferCompactAndClampRejectReason expected) {
    return contact_buffer_compact_and_clamp_reject_reason(buffer) == expected;
}

ContactBufferCompactAndClampPreflight preflight_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    ContactBufferCompactAndClampPreflight preflight{};
    preflight.reason = contact_buffer_compact_and_clamp_reject_reason(buffer);
    preflight.emptyBuffer = preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer;
    preflight.noWork = preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork;
    return preflight;
}

bool can_skip_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return !preflight_contact_buffer_compact_and_clamp(buffer).needsCompactAndClamp();
}

bool should_run_contact_buffer_compact_and_clamp(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compact_and_clamp(buffer).needsCompactAndClamp();
}

u32 compact_and_clamp_contact_buffer_with_preflight(ContactBufferSoA& buffer) {
    if (can_skip_contact_buffer_compact_and_clamp(buffer)) {
        return buffer.activeCount;
    }
    return buffer.compactAndClamp();
}

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
    if (should_skip_contact_buffer_write_slot(*this, slot, manifold)) {
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
    if (can_skip_contact_buffer_friction_bases(*this)) {
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
    const ContactBufferCompactionPreflight compactionPreflight = preflight_contact_buffer_compaction(*this);
    if (compactionPreflight.reason == ContactBufferCompactionRejectReason::EmptyBuffer) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }

    if (compactionPreflight.reason == ContactBufferCompactionRejectReason::AllValid) {
        activeCount = pairSlotCount > 0u ? pairSlotCount : activeCount;
        pairSlotCount = activeCount;
        return activeCount;
    }

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
    pairSlotCount = activeCount;
    for (u32 i = activeCount; i < validFlags.size(); ++i) {
        validFlags[i] = 0u;
        pointCounts[i] = 0u;
    }
    return activeCount;
}

u32 ContactBufferSoA::applyMaxCapacityClamp() {
    if (can_skip_contact_buffer_clamp(*this)) {
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
    const ContactBufferCompactAndClampPreflight preflight = preflight_contact_buffer_compact_and_clamp(*this);
    if (preflight.reason == ContactBufferCompactAndClampRejectReason::EmptyBuffer) {
        activeCount = 0u;
        pairSlotCount = 0u;
        return activeCount;
    }
    if (preflight.reason == ContactBufferCompactAndClampRejectReason::NoWork) {
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

} // namespace fuse::physics::narrowphase
