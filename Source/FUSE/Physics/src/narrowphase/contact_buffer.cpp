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
    for (u32 slot = 0u; slot < activeCount; ++slot) {
        if (validFlags[slot] == 0u) {
            continue;
        }
        const TangentBasis basis = buildTangentBasis(contactNormals[slot]);
        tangent1[slot] = basis.tangent1;
        tangent2[slot] = basis.tangent2;
    }
}

bool ContactBufferSoA::canSkipCompaction() const {
    if (pairSlotCount == 0u) {
        return true;
    }

    bool sawInvalid = false;
    for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
        if (validFlags[slot] == 0u) {
            sawInvalid = true;
            continue;
        }
        if (sawInvalid) {
            return false;
        }
    }
    return true;
}

bool ContactBufferSoA::canSkipMaxCapacityClamp() const {
    return maxCapacity == 0u || activeCount <= maxCapacity;
}

bool ContactBufferSoA::canSkipCompactAndClamp() const {
    return canSkipCompaction() && canSkipMaxCapacityClamp();
}

u32 ContactBufferSoA::countValidPairSlots() const {
    u32 validCount = 0u;
    for (u32 slot = 0u; slot < pairSlotCount; ++slot) {
        if (validFlags[slot] != 0u) {
            ++validCount;
        }
    }
    return validCount;
}

u32 ContactBufferSoA::compactIfNeeded() {
    if (canSkipCompaction()) {
        activeCount = countValidPairSlots();
        return activeCount;
    }
    return compact();
}

bool ContactBufferSoA::canSkipFrictionTangentRebuild(f32 epsilon) const {
    return preflight_contact_buffer_friction_rebuild(*this, epsilon).can_skip_rebuild();
}

void ContactBufferSoA::buildFrictionTangentBasesIfNeeded(f32 epsilon) {
    if (canSkipFrictionTangentRebuild(epsilon)) {
        return;
    }
    buildFrictionTangentBases();
}

bool ContactBufferSoA::rebuildFrictionTangentBasesWithPreflight(f32 epsilon) {
    const ContactBufferFrictionPreflight preflight = preflight_contact_buffer_friction_rebuild(*this, epsilon);
    if (preflight.reason != FrictionBasisRejectReason::None) {
        return false;
    }
    if (preflight.can_skip_rebuild()) {
        return activeCount > 0u;
    }
    buildFrictionTangentBases();
    return true;
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

u32 ContactBufferSoA::compactAndClampIfNeeded() {
    compactIfNeeded();
    if (canSkipMaxCapacityClamp()) {
        return activeCount;
    }
    return applyMaxCapacityClamp();
}

u32 ContactBufferSoA::compactAndClamp() {
    compact();
    return applyMaxCapacityClamp();
}

ContactBufferCompactionPreflight preflight_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    ContactBufferCompactionPreflight preflight{};
    if (buffer.pairSlotCount == 0u) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.validSlotCount = buffer.countValidPairSlots();
    preflight.activeCount = buffer.activeCount;
    preflight.needsCompaction = !buffer.canSkipCompaction();
    preflight.needsClamp = !buffer.canSkipMaxCapacityClamp();
    return preflight;
}

bool can_skip_contact_buffer_compaction(const ContactBufferSoA& buffer) {
    return preflight_contact_buffer_compaction(buffer).can_skip_compaction();
}

ContactBufferFrictionPreflight preflight_contact_buffer_friction_rebuild(
    const ContactBufferSoA& buffer,
    f32 epsilon) {
    ContactBufferFrictionPreflight preflight{};
    if (buffer.activeCount == 0u) {
        preflight.skipped = true;
        return preflight;
    }

    for (u32 slot = 0u; slot < buffer.activeCount; ++slot) {
        if (buffer.validFlags[slot] == 0u) {
            continue;
        }

        ContactManifold manifold = buffer.manifoldAt(slot);
        const FrictionBasisRejectReason slotReason = friction_basis_reject_reason(manifold);
        if (slotReason != FrictionBasisRejectReason::None) {
            preflight.reason = slotReason;
            preflight.skipped = true;
            return preflight;
        }

        if (friction_basis_is_stale(manifold, epsilon)) {
            ++preflight.slotsWithStaleBasis;
            ++preflight.slotsNeedingRebuild;
            continue;
        }

        if (!can_skip_friction_basis_rebuild(manifold, epsilon)) {
            ++preflight.slotsNeedingRebuild;
        }
    }

    return preflight;
}

bool can_skip_contact_buffer_friction_rebuild(const ContactBufferSoA& buffer, f32 epsilon) {
    return preflight_contact_buffer_friction_rebuild(buffer, epsilon).can_skip_rebuild();
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
