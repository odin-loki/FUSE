#include <fuse/physics/solver/solver_work_buffers.hpp>

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

void SolverWorkBuffers::clearPositionDeltas() {
    for (PositionDelta& slot : positionDeltas_) {
        slot.delta = {};
        slot.writeCount = 0;
    }
}

void SolverWorkBuffers::clearPositionDeltasForBodies(u32 bodyA, u32 bodyB) {
    if (bodyA < positionDeltas_.size()) {
        positionDeltas_[bodyA].delta = {};
        positionDeltas_[bodyA].writeCount = 0;
    }
    if (bodyB < positionDeltas_.size()) {
        positionDeltas_[bodyB].delta = {};
        positionDeltas_[bodyB].writeCount = 0;
    }
}

void SolverWorkBuffers::applyPositionDeltas(RigidBodySoA& bodies) const {
    const u32 count = std::min(bodies.count(), static_cast<u32>(positionDeltas_.size()));
    for (u32 i = 0; i < count; ++i) {
        if (positionDeltas_[i].writeCount == 0) {
            continue;
        }
        bodies.predictedPositions[i] += positionDeltas_[i].delta;
    }
}

} // namespace fuse::physics
