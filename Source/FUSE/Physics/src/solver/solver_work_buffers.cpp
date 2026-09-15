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
}

void SolverWorkBuffers::clearPositionDeltas() {
    for (PositionDelta& slot : positionDeltas_) {
        slot.delta = {};
        slot.writeCount = 0;
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
