#include <fuse/physics/solver/solver_work_buffers.hpp>

#include <fuse/physics/rotation.hpp>

#include <algorithm>

namespace fuse::physics {

void SolverWorkBuffers::init(u32 maxBodies, u32 maxContacts, u32 maxConstraints) {
    positionDeltas_.assign(maxBodies, PositionDelta{});
    contactManifolds_.clear();
    contactManifolds_.reserve(maxContacts);
    maxConstraints_ = maxConstraints;
}

void SolverWorkBuffers::clear() {
    clearPositionDeltas();
    contactManifolds_.clear();
    contactLambdas_.clear();
    distanceLambdas_.clear();
    bodyInvInertia_.clear();
    contactAnchors_.clear();
    contactPointLambdas_.clear();
}

void SolverWorkBuffers::prepareContactPoints(const RigidBodySoA& bodies) {
    constexpr u32 kSlots = narrowphase::kMaxContactPointsPerManifold;
    const usize slotCount = contactManifolds_.size() * kSlots;
    contactAnchors_.resize(slotCount);
    contactPointLambdas_.assign(slotCount, 0.f);
    for (usize c = 0; c < contactManifolds_.size(); ++c) {
        const narrowphase::ContactManifold& contact = contactManifolds_[c];
        const vec3 n = contact.contactNormal;
        const vec3 xA = bodies.predictedPositions[contact.bodyA];
        const vec3 xB = bodies.predictedPositions[contact.bodyB];
        const quat qA = bodies.predictedOrientations[contact.bodyA];
        const quat qB = bodies.predictedOrientations[contact.bodyB];
        const vec3 startA = bodies.positions[contact.bodyA];
        const vec3 startB = bodies.positions[contact.bodyB];
        const quat startQA = bodies.orientations[contact.bodyA];
        const quat startQB = bodies.orientations[contact.bodyB];
        for (u32 k = 0; k < contact.pointCount && k < kSlots; ++k) {
            // Split the penetration about the reported point so (pA - pB) . n = -penetration.
            const f32 halfDepth = 0.5f * contact.points[k].penetration;
            const vec3 pA = contact.points[k].point - n * halfDepth;
            const vec3 pB = contact.points[k].point + n * halfDepth;
            ContactAnchor& anchor = contactAnchors_[c * kSlots + k];
            anchor.localA = inverseRotate(qA, pA - xA);
            anchor.localB = inverseRotate(qB, pB - xB);
            anchor.startSeparation =
                (startA + rotate(startQA, anchor.localA)) - (startB + rotate(startQB, anchor.localB));
        }
    }
}

f32& SolverWorkBuffers::contactPointLambda(u32 contactIndex, u32 pointIndex) {
    const usize slot = static_cast<usize>(contactIndex) * narrowphase::kMaxContactPointsPerManifold + pointIndex;
    if (slot >= contactPointLambdas_.size()) {
        contactPointLambdas_.resize(slot + 1u, 0.f);
    }
    return contactPointLambdas_[slot];
}

void SolverWorkBuffers::ensureLambdaCapacity(u32 contactCount, u32 distanceCount) {
    if (contactLambdas_.size() < contactCount) {
        contactLambdas_.resize(contactCount, 0.f);
    }
    if (distanceLambdas_.size() < distanceCount) {
        distanceLambdas_.resize(distanceCount, 0.f);
    }
}

void SolverWorkBuffers::clearLambdas() {
    std::fill(contactLambdas_.begin(), contactLambdas_.end(), 0.f);
    std::fill(distanceLambdas_.begin(), distanceLambdas_.end(), 0.f);
}

f32& SolverWorkBuffers::contactLambda(u32 contactIndex) {
    if (contactIndex >= contactLambdas_.size()) {
        contactLambdas_.resize(contactIndex + 1u, 0.f);
    }
    return contactLambdas_[contactIndex];
}

f32& SolverWorkBuffers::distanceLambda(u32 distanceIndex) {
    if (distanceIndex >= distanceLambdas_.size()) {
        distanceLambdas_.resize(distanceIndex + 1u, 0.f);
    }
    return distanceLambdas_[distanceIndex];
}

void SolverWorkBuffers::seedContactLambdaFromImpulse(u32 contactIndex,
                                                     f32 warmNormalImpulse,
                                                     f32 dt) {
    if (warmNormalImpulse == 0.f || dt <= 0.f) {
        return;
    }
    f32& lambda = contactLambda(contactIndex);
    if (lambda == 0.f) {
        lambda = warmNormalImpulse * dt;
    }
}

void SolverWorkBuffers::seedDistanceLambda(u32 distanceIndex, f32 priorLambda) {
    if (priorLambda == 0.f) {
        return;
    }
    f32& lambda = distanceLambda(distanceIndex);
    if (lambda == 0.f) {
        lambda = priorLambda;
    }
}

void SolverWorkBuffers::clearPositionDeltas() {
    for (PositionDelta& slot : positionDeltas_) {
        slot.delta = {};
        slot.writeCount = 0;
    }
}

void SolverWorkBuffers::clearPositionDeltaForBody(u32 bodyIndex) {
    if (bodyIndex < positionDeltas_.size()) {
        positionDeltas_[bodyIndex].delta = {};
        positionDeltas_[bodyIndex].writeCount = 0;
    }
}

void SolverWorkBuffers::clearPositionDeltasForBodies(u32 bodyA, u32 bodyB) {
    clearPositionDeltaForBody(bodyA);
    clearPositionDeltaForBody(bodyB);
}

void SolverWorkBuffers::clearPositionDeltasForIslandBodies(const std::vector<u32>& bodyIndices) {
    for (u32 bodyIndex : bodyIndices) {
        clearPositionDeltaForBody(bodyIndex);
    }
}

void SolverWorkBuffers::applyPositionDeltas(RigidBodySoA& bodies) {
    const u32 count = std::min(bodies.count(), static_cast<u32>(positionDeltas_.size()));
    for (u32 i = 0; i < count; ++i) {
        if (positionDeltas_[i].writeCount == 0) {
            continue;
        }
        bodies.predictedPositions[i] += positionDeltas_[i].delta;
        positionDeltas_[i].delta = {};
        positionDeltas_[i].writeCount = 0;
    }
}

void SolverWorkBuffers::applyPositionDeltasForBodies(RigidBodySoA& bodies, u32 bodyA, u32 bodyB) {
    const auto apply = [&](u32 i) {
        if (i >= bodies.count() || i >= positionDeltas_.size() || positionDeltas_[i].writeCount == 0) {
            return;
        }
        bodies.predictedPositions[i] += positionDeltas_[i].delta;
        positionDeltas_[i].delta = {};
        positionDeltas_[i].writeCount = 0;
    };
    apply(bodyA);
    if (bodyB != bodyA) {
        apply(bodyB);
    }
}

} // namespace fuse::physics
