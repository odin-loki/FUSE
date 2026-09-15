#include <fuse/physics/solver/pbd_solver.hpp>
#include <fuse/physics/solver/constraint_accumulation.hpp>

#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics {
namespace {

constexpr u32 kIslandGrainSize = 1u;

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

} // namespace

void PBDSolver::init(u32 maxBodies, u32 maxContacts, u32 maxConstraints) {
    maxBodies_ = maxBodies;
    maxContacts_ = maxContacts;
    distanceConstraints_.clear();
    distanceConstraints_.reserve(maxConstraints);
    workBuffers_.init(maxBodies, maxContacts, maxConstraints);
    islandGraph_.clear();
    lastContactCount_ = 0;
    lastActiveCount_ = 0;
    lastIterationCount_ = 0;
    lastConstraintResidual_ = 0.f;
}

void PBDSolver::destroy() {
    distanceConstraints_.clear();
    workBuffers_.clear();
    islandGraph_.clear();
    maxBodies_ = 0;
    maxContacts_ = 0;
    lastContactCount_ = 0;
    lastActiveCount_ = 0;
    lastIterationCount_ = 0;
    lastConstraintResidual_ = 0.f;
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

void PBDSolver::generateContacts(RigidBodySoA& bodies,
                                 const CollisionShapeSoA& shapes,
                                 const SolverParams& params) {
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

    std::vector<narrowphase::ContactManifold>& filtered = workBuffers_.contactManifolds();
    filtered.clear();
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
}

f32 PBDSolver::measureConstraintResidual_(RigidBodySoA& bodies) const {
    return measureConstraintResidual(
        bodies,
        workBuffers_.contactManifolds(),
        distanceConstraints_,
        [](const RigidBodySoA& bodySoA, u32 index) { return effectiveInvMass(bodySoA, index); });
}

void PBDSolver::resolveIslandConstraints(RigidBodySoA& bodies,
                                         const ContactIslandGraph::Island& island,
                                         const SolverParams& params,
                                         f32 dt) {
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers_.contactManifolds();
    std::vector<PositionDelta>& positionDeltas = workBuffers_.positionDeltas();

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        workBuffers_.clearPositionDeltas();
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        const f32 invMassA = effectiveInvMass(bodies, contact.bodyA);
        const f32 invMassB = effectiveInvMass(bodies, contact.bodyB);
        f32& lambda = workBuffers_.contactLambda(contactIndex);
        accumulateContactCorrection(bodies,
                                  contact,
                                  invMassA,
                                  invMassB,
                                  dt,
                                  params.contactCompliance,
                                  lambda,
                                  positionDeltas);
        workBuffers_.applyPositionDeltas(bodies);
    }

    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceConstraints_.size()) {
            continue;
        }
        workBuffers_.clearPositionDeltas();
        const DistanceConstraint& constraint = distanceConstraints_[distanceIndex];
        const f32 invMassA = effectiveInvMass(bodies, constraint.bodyA);
        const f32 invMassB = effectiveInvMass(bodies, constraint.bodyB);
        f32& lambda = workBuffers_.distanceLambda(distanceIndex);
        accumulateDistanceSpringCorrection(bodies,
                                           constraint,
                                           invMassA,
                                           invMassB,
                                           dt,
                                           lambda,
                                           positionDeltas);
        workBuffers_.applyPositionDeltas(bodies);
    }
}

void PBDSolver::runConstraintIterations(RigidBodySoA& bodies, const SolverParams& params, f32 dt) {
    const u32 maxIterations = std::max(1u, params.iterations);
    lastIterationCount_ = 0;
    lastConstraintResidual_ = 0.f;

    islandGraph_.build(bodies.count(), workBuffers_.contactManifolds(), distanceConstraints_);
    workBuffers_.ensureLambdaCapacity(static_cast<u32>(workBuffers_.contactManifolds().size()),
                                      static_cast<u32>(distanceConstraints_.size()));

    for (u32 iter = 0; iter < maxIterations; ++iter) {
        const u32 islandCount = islandGraph_.islandCount();
        if (islandCount == 0) {
            for (u32 distanceIndex = 0; distanceIndex < distanceConstraints_.size(); ++distanceIndex) {
                workBuffers_.clearPositionDeltas();
                const DistanceConstraint& constraint = distanceConstraints_[distanceIndex];
                const f32 invMassA = effectiveInvMass(bodies, constraint.bodyA);
                const f32 invMassB = effectiveInvMass(bodies, constraint.bodyB);
                f32& lambda = workBuffers_.distanceLambda(distanceIndex);
                accumulateDistanceSpringCorrection(bodies,
                                                   constraint,
                                                   invMassA,
                                                   invMassB,
                                                   dt,
                                                   lambda,
                                                   workBuffers_.positionDeltas());
                workBuffers_.applyPositionDeltas(bodies);
            }
        } else {
            fuse::jobs::parallel_for(0, islandCount, kIslandGrainSize, [&](u32 islandIndex) {
                resolveIslandConstraints(bodies, islandGraph_.island(islandIndex), params, dt);
            });
        }

        lastConstraintResidual_ = measureConstraintResidual_(bodies);
        ++lastIterationCount_;

        if (params.residualTolerance > 0.f && lastConstraintResidual_ <= params.residualTolerance) {
            break;
        }
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
        lastIterationCount_ = 0;
        lastConstraintResidual_ = 0.f;
        return;
    }

    const f32 subDt = dt / static_cast<f32>(std::max(1u, params.substeps));
    lastActiveCount_ = 0;
    lastContactCount_ = 0;

    for (u32 substep = 0; substep < std::max(1u, params.substeps); ++substep) {
        predict(bodies, params, subDt);
        generateContacts(bodies, shapes, params);
        if (substep == 0u) {
            workBuffers_.clearLambdas();
        }
        runConstraintIterations(bodies, params, subDt);
        updateVelocities(bodies, subDt);
    }

    applyDamping(bodies, params);
    detectSleep(bodies, params, dt);

    lastContactCount_ = static_cast<u32>(workBuffers_.contactManifolds().size());
    for (u32 i = 0; i < bodies.count(); ++i) {
        if (!isStaticOrKinematic(bodies.flags[i]) && !isSleeping(bodies.flags[i])) {
            ++lastActiveCount_;
        }
    }
}

} // namespace fuse::physics
