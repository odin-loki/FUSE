#include <fuse/physics/solver/pbd_solver.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics {
namespace {

bool isStaticOrKinematic(u32 flags) {
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;
}

bool isSleeping(u32 flags) {
    return (flags & RB_SLEEPING) != 0u;
}

f32 effectiveInvMass(const RigidBodySoA& bodies, u32 index) {
    if (isStaticOrKinematic(bodies.flags[index]) || isSleeping(bodies.flags[index])) {
        return 0.f;
    }
    return bodies.invMasses[index];
}

void resolveContactXpbd(RigidBodySoA& bodies,
                        const narrowphase::ContactManifold& contact,
                        f32 dt,
                        f32 compliance) {
    if (!contact.valid) {
        return;
    }

    const u32 a = contact.bodyA;
    const u32 b = contact.bodyB;
    const f32 invMassA = effectiveInvMass(bodies, a);
    const f32 invMassB = effectiveInvMass(bodies, b);
    const f32 weightSum = invMassA + invMassB;
    if (weightSum < 1e-10f) {
        return;
    }

    const vec3 pa = bodies.predictedPositions[a];
    const vec3 pb = bodies.predictedPositions[b];
    const vec3 diff = pa - pb;
    const f32 constraint = diff.dot(contact.contactNormal) - contact.minSeparation;
    if (constraint >= 0.f) {
        return;
    }

    const f32 alpha = compliance / (dt * dt);
    const f32 deltaLambda = -constraint / (weightSum + alpha);
    const vec3 deltaPosition = contact.contactNormal * deltaLambda;

    bodies.predictedPositions[a] += deltaPosition * invMassA;
    bodies.predictedPositions[b] -= deltaPosition * invMassB;

    const vec3 relativeVelocity = (pa - pb) * (1.f / dt);
    const f32 normalVelocity = relativeVelocity.dot(contact.contactNormal);
    vec3 tangentialVelocity = relativeVelocity - contact.contactNormal * normalVelocity;
    const f32 tangentialLength = tangentialVelocity.length();
    if (tangentialLength > 1e-6f) {
        const f32 frictionCoeff = bodies.frictionDynamic[a];
        const f32 frictionCorrection =
            std::min(frictionCoeff * std::fabs(deltaLambda), tangentialLength * weightSum) / weightSum;
        const vec3 tangentialDir = tangentialVelocity * (1.f / tangentialLength);
        bodies.predictedPositions[a] -= tangentialDir * (frictionCorrection * invMassA);
        bodies.predictedPositions[b] += tangentialDir * (frictionCorrection * invMassB);
    }
}

vec3 worldAnchor(const RigidBodySoA& bodies, u32 bodyIndex, const vec3& localAnchor) {
    (void)localAnchor;
    return bodies.predictedPositions[bodyIndex];
}

} // namespace

void PBDSolver::init(u32 maxBodies, u32 maxContacts, u32 maxConstraints) {
    maxBodies_ = maxBodies;
    maxContacts_ = maxContacts;
    distanceConstraints_.clear();
    distanceConstraints_.reserve(maxConstraints);
    lastContactCount_ = 0;
    lastActiveCount_ = 0;
}

void PBDSolver::destroy() {
    distanceConstraints_.clear();
    maxBodies_ = 0;
    maxContacts_ = 0;
    lastContactCount_ = 0;
    lastActiveCount_ = 0;
}

void PBDSolver::setDistanceConstraints(const std::vector<DistanceConstraint>& constraints) {
    distanceConstraints_ = constraints;
}

void PBDSolver::predict(RigidBodySoA& bodies, const SolverParams& params, f32 dt) {
    const u32 bodyCount = bodies.count();
    for (u32 i = 0; i < bodyCount; ++i) {
        bodies.predictedOrientations[i] = bodies.orientations[i];

        if (isStaticOrKinematic(bodies.flags[i]) || isSleeping(bodies.flags[i])) {
            bodies.predictedPositions[i] = bodies.positions[i];
            continue;
        }

        vec3 acceleration{};
        if ((bodies.flags[i] & RB_NO_GRAVITY) == 0u) {
            acceleration += params.gravity;
        }
        if (bodies.invMasses[i] > 0.f) {
            acceleration += bodies.forces[i] * bodies.invMasses[i];
        }

        bodies.linearVelocities[i] += acceleration * dt;
        bodies.predictedPositions[i] = bodies.positions[i] + bodies.linearVelocities[i] * dt;
        bodies.forces[i] = {};
        bodies.torques[i] = {};
    }
}

std::vector<narrowphase::ContactManifold> PBDSolver::generateContacts(
    const RigidBodySoA& bodies,
    const CollisionShapeSoA& shapes,
    const SolverParams& params) const {
    RigidBodySoA collisionBodies = bodies;
    collisionBodies.positions = bodies.predictedPositions;

    broadphase::SpatialHashParams hashParams = params.broadphase;
    hashParams.bodyCount = collisionBodies.count();
    if (hashParams.tableSize == 0) {
        hashParams.tableSize = std::max(1024u, hashParams.bodyCount * 8u);
    }
    if (hashParams.cellSize <= 0.f) {
        hashParams.cellSize = 2.f;
    }

    const std::vector<broadphase::CandidatePair> pairs =
        broadphase::runBroadphase(collisionBodies, shapes, hashParams);
    std::vector<narrowphase::ContactManifold> manifolds =
        narrowphase::runNarrowphase(pairs, collisionBodies, shapes);

    std::vector<narrowphase::ContactManifold> filtered;
    filtered.reserve(manifolds.size());
    for (const narrowphase::ContactManifold& manifold : manifolds) {
        if (!manifold.valid) {
            continue;
        }
        if ((bodies.flags[manifold.bodyA] & RB_TRIGGER) != 0u ||
            (bodies.flags[manifold.bodyB] & RB_TRIGGER) != 0u) {
            continue;
        }
        filtered.push_back(manifold);
    }
    return filtered;
}

void PBDSolver::resolveContacts(RigidBodySoA& bodies,
                                const std::vector<narrowphase::ContactManifold>& manifolds,
                                const SolverParams& params,
                                f32 dt) {
    for (const narrowphase::ContactManifold& manifold : manifolds) {
        resolveContactXpbd(bodies, manifold, dt, params.contactCompliance);
    }
}

void PBDSolver::resolveDistanceConstraints(RigidBodySoA& bodies, f32 dt) {
    for (const DistanceConstraint& constraint : distanceConstraints_) {
        const u32 a = constraint.bodyA;
        const u32 b = constraint.bodyB;
        const f32 invMassA = effectiveInvMass(bodies, a);
        const f32 invMassB = effectiveInvMass(bodies, b);
        const f32 weightSum = invMassA + invMassB;
        if (weightSum < 1e-10f) {
            continue;
        }

        const vec3 anchorA = worldAnchor(bodies, a, constraint.localAnchorA);
        const vec3 anchorB = worldAnchor(bodies, b, constraint.localAnchorB);
        const vec3 diff = anchorA - anchorB;
        const f32 distance = diff.length();
        if (distance < 1e-8f) {
            continue;
        }

        const vec3 normal = diff * (1.f / distance);
        const f32 constraintValue = distance - constraint.restLength;
        const f32 alpha = constraint.compliance / (dt * dt);
        const f32 deltaLambda = -constraintValue / (weightSum + alpha);
        const vec3 deltaPosition = normal * deltaLambda;

        bodies.predictedPositions[a] += deltaPosition * invMassA;
        bodies.predictedPositions[b] -= deltaPosition * invMassB;
    }
}

void PBDSolver::updateVelocities(RigidBodySoA& bodies, f32 dt) {
    const f32 invDt = 1.f / dt;
    for (u32 i = 0; i < bodies.count(); ++i) {
        if (isStaticOrKinematic(bodies.flags[i])) {
            bodies.linearVelocities[i] = {};
            bodies.angularVelocities[i] = {};
            continue;
        }

        if (isSleeping(bodies.flags[i])) {
            continue;
        }

        const vec3 delta = bodies.predictedPositions[i] - bodies.positions[i];
        bodies.linearVelocities[i] = delta * invDt;
        bodies.positions[i] = bodies.predictedPositions[i];
        bodies.orientations[i] = bodies.predictedOrientations[i];
    }
}

void PBDSolver::applyDamping(RigidBodySoA& bodies, const SolverParams& params) {
    for (u32 i = 0; i < bodies.count(); ++i) {
        if (isStaticOrKinematic(bodies.flags[i]) || isSleeping(bodies.flags[i])) {
            continue;
        }
        bodies.linearVelocities[i] = bodies.linearVelocities[i] * params.linearDamping;
        bodies.angularVelocities[i] = bodies.angularVelocities[i] * params.angularDamping;
    }
}

void PBDSolver::detectSleep(RigidBodySoA& bodies, const SolverParams& params, f32 dt) {
    for (u32 i = 0; i < bodies.count(); ++i) {
        if (isStaticOrKinematic(bodies.flags[i])) {
            continue;
        }

        const f32 linearSpeed = bodies.linearVelocities[i].length();
        const f32 angularSpeed = bodies.angularVelocities[i].length();
        if (linearSpeed < params.sleepLinearThreshold && angularSpeed < params.sleepAngularThreshold) {
            bodies.sleepTimers[i] += dt;
            if (bodies.sleepTimers[i] >= params.sleepTimeRequired) {
                bodies.flags[i] |= RB_SLEEPING;
                bodies.linearVelocities[i] = {};
                bodies.angularVelocities[i] = {};
            }
        } else {
            bodies.sleepTimers[i] = 0.f;
            bodies.flags[i] &= ~RB_SLEEPING;
        }
    }
}

void PBDSolver::step(RigidBodySoA& bodies,
                     const CollisionShapeSoA& shapes,
                     const SolverParams& params,
                     f32 dt) {
    if (bodies.count() == 0 || dt <= 0.f) {
        lastContactCount_ = 0;
        lastActiveCount_ = 0;
        return;
    }

    const f32 subDt = dt / static_cast<f32>(std::max(1u, params.substeps));
    lastActiveCount_ = 0;
    lastContactCount_ = 0;

    std::vector<narrowphase::ContactManifold> manifolds;
    manifolds.reserve(maxContacts_);

    for (u32 substep = 0; substep < std::max(1u, params.substeps); ++substep) {
        predict(bodies, params, subDt);
        manifolds = generateContacts(bodies, shapes, params);

        for (u32 iter = 0; iter < params.iterations; ++iter) {
            resolveContacts(bodies, manifolds, params, subDt);
            resolveDistanceConstraints(bodies, subDt);
        }

        updateVelocities(bodies, subDt);
    }

    applyDamping(bodies, params);
    detectSleep(bodies, params, dt);

    lastContactCount_ = static_cast<u32>(manifolds.size());
    for (u32 i = 0; i < bodies.count(); ++i) {
        if (!isStaticOrKinematic(bodies.flags[i]) && !isSleeping(bodies.flags[i])) {
            ++lastActiveCount_;
        }
    }
}

} // namespace fuse::physics
