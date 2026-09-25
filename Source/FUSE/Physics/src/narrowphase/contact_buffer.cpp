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

void ContactBufferSoA::moveSlot(u32 dst, u32 src) {
    contactPoints[dst] = contactPoints[src];
    contactNormals[dst] = contactNormals[src];
    penetrationDepths[dst] = penetrationDepths[src];
    minSeparations[dst] = minSeparations[src];
    bodyA[dst] = bodyA[src];
    bodyB[dst] = bodyB[src];
    validFlags[dst] = validFlags[src];
    pointCounts[dst] = pointCounts[src];
    warmNormalImpulses[dst] = warmNormalImpulses[src];
    warmTangentImpulses[dst] = warmTangentImpulses[src];
    tangent1[dst] = tangent1[src];
    tangent2[dst] = tangent2[src];
    const u32 readBase = pointSlotBase(src);
    const u32 writeBase = pointSlotBase(dst);
    for (u32 pointIndex = 0u; pointIndex < kMaxContactPointsPerManifold; ++pointIndex) {
        pointSlots[writeBase + pointIndex] = pointSlots[readBase + pointIndex];
        pointPenetrations[writeBase + pointIndex] = pointPenetrations[readBase + pointIndex];
    }
}

u32 ContactBufferSoA::applyMaxCapacityClamp() {
    if (maxCapacity == 0u || activeCount <= maxCapacity) {
        return activeCount;
    }

    if (activeCount > 1u) {
        // Deepest contacts first. The permutation is applied in place (cycle-following with one
        // row held in locals), so every SoA array keeps its original size (>= pairSlotCount) and
        // the tail loop below stays in bounds. Same std::sort over the same input as before, so
        // the resulting order (including ties) is unchanged.
        std::vector<u32>& order = m_clampOrder;
        if (order.capacity() < activeCount) {
            order.reserve(static_cast<usize>(activeCount) + activeCount / 2u);
        }
        order.resize(activeCount);
        for (u32 i = 0u; i < activeCount; ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](u32 lhs, u32 rhs) {
            return penetrationDepths[lhs] > penetrationDepths[rhs];
        });

        // Slot i must receive old slot order[i]. Follow each cycle holding its first row in locals.
        for (u32 start = 0u; start < activeCount; ++start) {
            if (order[start] == start) {
                continue;
            }
            const vec3 tmpContactPoint = contactPoints[start];
            const vec3 tmpContactNormal = contactNormals[start];
            const f32 tmpPenetrationDepth = penetrationDepths[start];
            const f32 tmpMinSeparation = minSeparations[start];
            const u32 tmpBodyA = bodyA[start];
            const u32 tmpBodyB = bodyB[start];
            const u8 tmpValidFlag = validFlags[start];
            const u8 tmpPointCount = pointCounts[start];
            const f32 tmpWarmNormalImpulse = warmNormalImpulses[start];
            const vec2 tmpWarmTangentImpulse = warmTangentImpulses[start];
            const vec3 tmpTangent1 = tangent1[start];
            const vec3 tmpTangent2 = tangent2[start];
            vec3 tmpPointSlots[kMaxContactPointsPerManifold];
            f32 tmpPointPenetrations[kMaxContactPointsPerManifold];
            for (u32 pointIndex = 0u; pointIndex < kMaxContactPointsPerManifold; ++pointIndex) {
                tmpPointSlots[pointIndex] = pointSlots[pointSlotBase(start) + pointIndex];
                tmpPointPenetrations[pointIndex] = pointPenetrations[pointSlotBase(start) + pointIndex];
            }

            u32 dst = start;
            while (true) {
                const u32 src = order[dst];
                order[dst] = dst; // mark placed
                if (src == start) {
                    contactPoints[dst] = tmpContactPoint;
                    contactNormals[dst] = tmpContactNormal;
                    penetrationDepths[dst] = tmpPenetrationDepth;
                    minSeparations[dst] = tmpMinSeparation;
                    bodyA[dst] = tmpBodyA;
                    bodyB[dst] = tmpBodyB;
                    validFlags[dst] = tmpValidFlag;
                    pointCounts[dst] = tmpPointCount;
                    warmNormalImpulses[dst] = tmpWarmNormalImpulse;
                    warmTangentImpulses[dst] = tmpWarmTangentImpulse;
                    tangent1[dst] = tmpTangent1;
                    tangent2[dst] = tmpTangent2;
                    for (u32 pointIndex = 0u; pointIndex < kMaxContactPointsPerManifold; ++pointIndex) {
                        pointSlots[pointSlotBase(dst) + pointIndex] = tmpPointSlots[pointIndex];
                        pointPenetrations[pointSlotBase(dst) + pointIndex] = tmpPointPenetrations[pointIndex];
                    }
                    break;
                }
                moveSlot(dst, src);
                dst = src;
            }
        }
    }

    const u32 excess = activeCount - maxCapacity;
    droppedCount += excess;
    activeCount = maxCapacity;

    const u32 slotEnd = std::min<u32>(pairSlotCount, static_cast<u32>(validFlags.size()));
    for (u32 i = activeCount; i < slotEnd; ++i) {
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

void ContactBufferSoA::copyTo(std::vector<ContactManifold>& out) const {
    out.clear();
    if (out.capacity() < activeCount) {
        out.reserve(activeCount);
    }
    for (u32 i = 0; i < activeCount; ++i) {
        if (validFlags[i] != 0u) {
            out.push_back(manifoldAt(i));
        }
    }
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
