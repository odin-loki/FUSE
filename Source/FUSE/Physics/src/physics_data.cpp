#include <fuse/physics/physics_data.hpp>

namespace fuse::physics {

void RigidBodySoA::reserve(u32 bodyCapacity) {
    positions.reserve(bodyCapacity);
    orientations.reserve(bodyCapacity);
    linearVelocities.reserve(bodyCapacity);
    angularVelocities.reserve(bodyCapacity);
    invMasses.reserve(bodyCapacity);
    restitutions.reserve(bodyCapacity);
    frictionStatic.reserve(bodyCapacity);
    frictionDynamic.reserve(bodyCapacity);
    flags.reserve(bodyCapacity);
    collisionLayers.reserve(bodyCapacity);
    collisionMasks.reserve(bodyCapacity);
    predictedPositions.reserve(bodyCapacity);
    predictedOrientations.reserve(bodyCapacity);
    forces.reserve(bodyCapacity);
    torques.reserve(bodyCapacity);
    sleepTimers.reserve(bodyCapacity);
}

void RigidBodySoA::clear() {
    positions.clear();
    orientations.clear();
    linearVelocities.clear();
    angularVelocities.clear();
    invMasses.clear();
    restitutions.clear();
    frictionStatic.clear();
    frictionDynamic.clear();
    flags.clear();
    collisionLayers.clear();
    collisionMasks.clear();
    predictedPositions.clear();
    predictedOrientations.clear();
    forces.clear();
    torques.clear();
    sleepTimers.clear();
}

u32 RigidBodySoA::addBody(vec3 position, f32 invMass, u32 bodyFlags, u32 collisionLayer,
                            u32 collisionMask) {
    const u32 index = count();
    positions.push_back(position);
    orientations.push_back({});
    linearVelocities.push_back({});
    angularVelocities.push_back({});
    invMasses.push_back(invMass);
    restitutions.push_back(0.2f);
    frictionStatic.push_back(0.5f);
    frictionDynamic.push_back(0.3f);
    flags.push_back(bodyFlags);
    collisionLayers.push_back(collisionLayer);
    collisionMasks.push_back(collisionMask);
    predictedPositions.push_back(position);
    predictedOrientations.push_back({});
    forces.push_back({});
    torques.push_back({});
    sleepTimers.push_back(0.f);
    return index;
}

namespace {

template <typename T>
void swapRemove(std::vector<T>& values, u32 index) {
    values[index] = values.back();
    values.pop_back();
}

} // namespace

void RigidBodySoA::removeBodySwap(u32 index) {
    if (index >= count()) {
        return;
    }
    swapRemove(positions, index);
    swapRemove(orientations, index);
    swapRemove(linearVelocities, index);
    swapRemove(angularVelocities, index);
    swapRemove(invMasses, index);
    swapRemove(restitutions, index);
    swapRemove(frictionStatic, index);
    swapRemove(frictionDynamic, index);
    swapRemove(flags, index);
    swapRemove(collisionLayers, index);
    swapRemove(collisionMasks, index);
    swapRemove(predictedPositions, index);
    swapRemove(predictedOrientations, index);
    swapRemove(forces, index);
    swapRemove(torques, index);
    swapRemove(sleepTimers, index);
}

void CollisionShapeSoA::removeShapeSwap(u32 index) {
    if (index >= count()) {
        return;
    }
    swapRemove(types, index);
    swapRemove(params, index);
    swapRemove(scalars, index);
    swapRemove(bodyIndices, index);
}

void CollisionShapeSoA::clear() {
    types.clear();
    params.clear();
    scalars.clear();
    bodyIndices.clear();
}

u32 CollisionShapeSoA::addShape(CollisionShapeType type, u32 bodyIndex, vec3 shapeParams, f32 scalarParam) {
    const u32 index = count();
    types.push_back(static_cast<u32>(type));
    params.push_back(shapeParams);
    scalars.push_back(scalarParam);
    bodyIndices.push_back(bodyIndex);
    return index;
}

} // namespace fuse::physics
