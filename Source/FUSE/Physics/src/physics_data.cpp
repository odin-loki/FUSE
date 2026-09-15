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
    predictedPositions.reserve(bodyCapacity);
    predictedOrientations.reserve(bodyCapacity);
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
    predictedPositions.clear();
    predictedOrientations.clear();
}

u32 RigidBodySoA::addBody(vec3 position, f32 invMass, u32 bodyFlags) {
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
    predictedPositions.push_back(position);
    predictedOrientations.push_back({});
    return index;
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
