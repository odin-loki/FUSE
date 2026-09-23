#include <fuse/physics/solver/pbd_solver.hpp>
#include <fuse/physics/solver/constraint_accumulation.hpp>
#include <fuse/physics/solver/pbd_island_solve.hpp>
#include <fuse/physics/rotation.hpp>

#include <fuse/jobs/parallel_for.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics {
namespace {

constexpr u32 kIslandGrainSize = 1u;
constexpr usize kParallelConstraintThreshold = 8192u;

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

constexpr u32 kNoShape = 0xFFFFFFFFu;
constexpr u32 kNoSlot = 0xFFFFFFFFu;

/// Penetrating (not merely speculative) at detection: what counts as a contact for events.
bool isTouching(const narrowphase::ContactManifold& manifold) {
    return manifold.maxPenetration() >= -1e-6f;
}

/// Conservative bounds of a shape at `position`; planes are unbounded (handled separately).
aabb shapeBoundsAt(const CollisionShapeSoA& shapes, u32 shapeIndex, vec3 position, const quat& orientation) {
    const vec3 p = shapes.params[shapeIndex];
    switch (static_cast<CollisionShapeType>(shapes.types[shapeIndex])) {
    case CollisionShapeType::Box:
        return broadphase::aabbFromBox(position, orientedBoxHalfExtents(orientation, p));
    case CollisionShapeType::Capsule:
        return broadphase::aabbFromBox(position, orientedCapsuleHalfExtents(orientation, p));
    default:
        return broadphase::aabbFromSphere(position, p.x);
    }
}

aabb sweptBounds(const aabb& start, vec3 motion) {
    aabb out = start;
    out.min.x += std::min(motion.x, 0.f);
    out.min.y += std::min(motion.y, 0.f);
    out.min.z += std::min(motion.z, 0.f);
    out.max.x += std::max(motion.x, 0.f);
    out.max.y += std::max(motion.y, 0.f);
    out.max.z += std::max(motion.z, 0.f);
    return out;
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

void PBDSolver::mapBodyShapes_(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    const u32 bodyCount = bodies.count();
    bodyShape_.assign(bodyCount, kNoShape);
    for (u32 shapeIndex = 0; shapeIndex < shapes.count(); ++shapeIndex) {
        const u32 body = shapes.bodyIndices[shapeIndex];
        if (body < bodyCount && bodyShape_[body] == kNoShape) {
            bodyShape_[body] = shapeIndex;
        }
    }
}

void PBDSolver::computeInverseInertia_(const RigidBodySoA& bodies, const CollisionShapeSoA& shapes) {
    std::vector<vec3>& inertia = workBuffers_.bodyInvInertia();
    inertia.resize(bodies.count());
    for (u32 i = 0; i < bodies.count(); ++i) {
        const u32 shape = bodyShape_[i];
        if (shape == kNoShape || (bodies.flags[i] & RB_FIXED_ROTATION) != 0u) {
            inertia[i] = {};
            continue;
        }
        inertia[i] = shapeInverseInertia(static_cast<CollisionShapeType>(shapes.types[shape]), shapes.params[shape],
                                         bodies.invMasses[i]);
    }
}

u32 PBDSolver::slotForFrameContact_(const narrowphase::ContactManifold& manifold, bool trigger) {
    const u32 lo = std::min(manifold.bodyA, manifold.bodyB);
    const u32 hi = std::max(manifold.bodyA, manifold.bodyB);
    const u64 key = (static_cast<u64>(lo) << 32u) | hi;
    const auto [it, inserted] = frameContactSlot_.try_emplace(key, static_cast<u32>(frameContacts_.size()));
    if (inserted) {
        frameContacts_.push_back({manifold.bodyA, manifold.bodyB, manifold.contactNormal, manifold.contactPoint, 0.f,
                                  trigger});
    }
    return it->second;
}

void PBDSolver::recordFrameContacts_(RigidBodySoA& bodies, const SolverParams& params) {
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers_.contactManifolds();
    substepContactSlot_.resize(contacts.size());
    for (u32 i = 0; i < contacts.size(); ++i) {
        // Speculative contacts (not yet touching) get a slot only if they end up pushing.
        if (!isTouching(contacts[i])) {
            substepContactSlot_[i] = kNoSlot;
            continue;
        }
        substepContactSlot_[i] = slotForFrameContact_(contacts[i], false);
        // A moving body wakes a sleeping one it runs into. Bodies already settling (non-zero
        // sleep timer) only creep under gravity and must not wake their resting neighbours.
        const u32 a = contacts[i].bodyA;
        const u32 b = contacts[i].bodyB;
        const auto wake = [&](u32 sleeper, u32 other) {
            if (isSleeping(bodies.flags[sleeper]) && !isSleeping(bodies.flags[other]) &&
                (bodies.flags[other] & RB_STATIC) == 0u && bodies.sleepTimers[other] == 0.f &&
                (bodies.linearVelocities[other].length() > params.sleepLinearThreshold ||
                 bodies.angularVelocities[other].length() > params.sleepAngularThreshold)) {
                bodies.flags[sleeper] &= ~RB_SLEEPING;
                bodies.sleepTimers[sleeper] = 0.f;
            }
        };
        wake(a, b);
        wake(b, a);
    }
    for (const narrowphase::ContactManifold& manifold : triggerManifolds_) {
        slotForFrameContact_(manifold, true);
    }
}

u32 PBDSolver::applyContinuousCollision(RigidBodySoA& bodies, const CollisionShapeSoA& shapes, f32 dt) {
    lastCcdHitCount_ = 0;
    ccdBuffer_.clear();
    const u32 bodyCount = bodies.count();
    if (bodyCount == 0u || dt <= 0.f) {
        return 0u;
    }
    mapBodyShapes_(bodies, shapes);

    // Candidates: every shape a CCD body's swept bounds touch (planes always).
    sweptBounds_.resize(bodyCount);
    for (u32 body = 0; body < bodyCount; ++body) {
        if (bodyShape_[body] != kNoShape) {
            sweptBounds_[body] = sweptBounds(
                shapeBoundsAt(shapes, bodyShape_[body], bodies.positions[body], bodies.orientations[body]),
                bodies.linearVelocities[body] * dt);
        }
    }
    ccdPairs_.clear();
    for (u32 body = 0; body < bodyCount; ++body) {
        const u32 flags = bodies.flags[body];
        if ((flags & RB_CCD) == 0u || isStaticOrKinematic(flags) || isSleeping(flags) || bodyShape_[body] == kNoShape) {
            continue;
        }
        const aabb& swept = sweptBounds_[body];
        for (u32 other = 0; other < bodyCount; ++other) {
            const u32 otherShape = bodyShape_[other];
            if (other == body || otherShape == kNoShape) {
                continue;
            }
            if (!collisionLayersCollide(bodies.collisionLayers[body], bodies.collisionMasks[body],
                                        bodies.collisionLayers[other], bodies.collisionMasks[other]) ||
                (bodies.flags[other] & RB_TRIGGER) != 0u) {
                continue;
            }
            if (static_cast<CollisionShapeType>(shapes.types[otherShape]) != CollisionShapeType::Plane) {
                if (!broadphase::aabbOverlap(swept, sweptBounds_[other])) {
                    continue;
                }
                // Both CCD bodies: keep one ordering of the pair.
                if ((bodies.flags[other] & RB_CCD) != 0u && other < body) {
                    continue;
                }
            }
            ccdPairs_.push_back({body, other});
        }
    }
    if (ccdPairs_.empty()) {
        return 0u;
    }
    runCcdIntoBuffer(ccdPairs_, bodies, shapes, dt, ccdBuffer_);

    // Earliest impact per body (buffer is sorted by TOI); TOI 0 is an existing contact
    // the discrete solver already handles.
    ccdHandled_.assign(bodyCount, 0u);
    for (u32 i = 0; i < ccdBuffer_.activeCount; ++i) {
        const f32 toi = ccdBuffer_.toiValues[i];
        u32 a = ccdBuffer_.bodyA[i];
        u32 b = ccdBuffer_.bodyB[i];
        vec3 n = ccdBuffer_.contactNormals[i]; // points towards bodyA
        if (toi <= 1e-6f || ccdHandled_[a] != 0u || ccdHandled_[b] != 0u) {
            continue;
        }
        if ((bodies.flags[a] & RB_CCD) == 0u) {
            std::swap(a, b);
            n = n * -1.f;
        }
        const f32 invMassA = effectiveInvMass(bodies, a);
        const f32 invMassB = effectiveInvMass(bodies, b);
        const f32 weightSum = invMassA + invMassB;
        if (weightSum < 1e-10f) {
            continue;
        }
        bodies.positions[a] += bodies.linearVelocities[a] * (dt * toi);
        bodies.positions[b] += bodies.linearVelocities[b] * (dt * toi);
        const f32 approach = (bodies.linearVelocities[a] - bodies.linearVelocities[b]).dot(n);
        if (approach < 0.f) {
            const f32 restitution = bodies.restitutions[a] * bodies.restitutions[b];
            const f32 impulse = -(1.f + restitution) * approach / weightSum;
            bodies.linearVelocities[a] += n * (impulse * invMassA);
            bodies.linearVelocities[b] -= n * (impulse * invMassB);
        }
        ccdHandled_[a] = 1u;
        ccdHandled_[b] = 1u;
        ++lastCcdHitCount_;
    }
    return lastCcdHitCount_;
}

void PBDSolver::predict(RigidBodySoA& bodies, const SolverParams& params, f32 dt) {
    const u32 bodyCount = bodies.count();
    const std::vector<vec3>& invInertia = workBuffers_.bodyInvInertia();
    for (u32 i = 0; i < bodyCount; ++i) {
        if ((bodies.flags[i] & RB_KINEMATIC) != 0u) {
            // Driven along its programmed velocity; infinite mass so it pushes but is never pushed.
            bodies.predictedPositions[i] = bodies.positions[i] + bodies.linearVelocities[i] * dt;
            bodies.predictedOrientations[i] = applyRotationVector(bodies.orientations[i], bodies.angularVelocities[i] * dt);
            continue;
        }
        if (isStaticOrKinematic(bodies.flags[i]) || isSleeping(bodies.flags[i])) {
            bodies.predictedPositions[i] = bodies.positions[i];
            bodies.predictedOrientations[i] = bodies.orientations[i];
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

        const vec3 bodyInvInertia = i < invInertia.size() ? invInertia[i] : vec3{};
        if (bodyInvInertia.x <= 0.f || bodyInvInertia.y <= 0.f || bodyInvInertia.z <= 0.f) {
            bodies.angularVelocities[i] = {}; // no rotational freedom
            bodies.predictedOrientations[i] = bodies.orientations[i];
            continue;
        }
        const quat q = bodies.orientations[i];
        vec3& omega = bodies.angularVelocities[i];
        omega += applyInverseInertia(q, bodyInvInertia, bodies.torques[i]) * dt;
        omega = implicitGyroscopicStep(q, bodyInvInertia, omega, dt);
        bodies.predictedOrientations[i] = applyRotationVector(q, omega * dt);
    }
}

void PBDSolver::generateContacts(RigidBodySoA& bodies,
                                 const CollisionShapeSoA& shapes,
                                 const SolverParams& params) {
    // Collide at the predicted positions (swapped in place instead of copying the SoA).
    std::swap(bodies.positions, bodies.predictedPositions);
    std::swap(bodies.orientations, bodies.predictedOrientations);
    const f32 cellSize = params.broadphase.cellSize > 0.f ? params.broadphase.cellSize : 2.f;
    const f32 margin = std::max(params.contactMargin, 0.f);
    grid_.findPairs(bodies, shapes, cellSize, candidatePairs_, margin);
    narrowphase::collidePairs(candidatePairs_, bodies, shapes, narrowManifolds_, margin);
    std::swap(bodies.positions, bodies.predictedPositions);
    std::swap(bodies.orientations, bodies.predictedOrientations);

    std::vector<narrowphase::ContactManifold>& filtered = workBuffers_.contactManifolds();
    filtered.clear();
    triggerManifolds_.clear();
    for (const narrowphase::ContactManifold& manifold : narrowManifolds_) {
        if (!manifold.valid) {
            continue;
        }
        if ((bodies.flags[manifold.bodyA] & RB_TRIGGER) != 0u ||
            (bodies.flags[manifold.bodyB] & RB_TRIGGER) != 0u) {
            if (isTouching(manifold)) {
                triggerManifolds_.push_back(manifold); // reported, never resolved
            }
            continue;
        }
        filtered.push_back(manifold);
    }
}

f32 PBDSolver::measureConstraintResidual_(RigidBodySoA& bodies) const {
    return measureConstraintResidual(
        bodies,
        workBuffers_,
        workBuffers_.contactManifolds(),
        distanceConstraints_,
        [](const RigidBodySoA& bodySoA, u32 index) { return effectiveInvMass(bodySoA, index); });
}

void PBDSolver::resolveIslandConstraints(RigidBodySoA& bodies,
                                         const ContactIslandGraph::Island& island,
                                         const SolverParams& params,
                                         f32 dt) {
    solve_island_job(bodies,
                     island,
                     workBuffers_,
                     distanceConstraints_,
                     dt,
                     params.contactCompliance,
                     [](const RigidBodySoA& bodySoA, u32 index) {
                         return effectiveInvMass(bodySoA, index);
                     });
}

void PBDSolver::runConstraintIterations(RigidBodySoA& bodies, const SolverParams& params, f32 dt) {
    const u32 maxIterations = std::max(1u, params.iterations);
    lastIterationCount_ = 0;
    lastConstraintResidual_ = 0.f;

    islandGraph_.build(bodies.count(), workBuffers_.contactManifolds(), distanceConstraints_);
    workBuffers_.ensureLambdaCapacity(static_cast<u32>(workBuffers_.contactManifolds().size()),
                                      static_cast<u32>(distanceConstraints_.size()));

    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers_.contactManifolds();
    for (u32 contactIndex = 0; contactIndex < contacts.size(); ++contactIndex) {
        workBuffers_.seedContactLambdaFromImpulse(contactIndex, contacts[contactIndex].warmNormalImpulse, dt);
    }

    // Job dispatch costs more than it saves until there is a lot of constraint work: measured
    // on 1000 bodies / ~500 contacts, the serial loop is ~4x faster than per-iteration jobs.
    const bool parallelIslands =
        contacts.size() + distanceConstraints_.size() >= kParallelConstraintThreshold && islandGraph_.islandCount() > 1u;
    for (u32 iter = 0; iter < maxIterations; ++iter) {
        const u32 islandCount = islandGraph_.islandCount();
        if (islandCount == 0) {
            for (u32 distanceIndex = 0; distanceIndex < distanceConstraints_.size(); ++distanceIndex) {
                const DistanceConstraint& constraint = distanceConstraints_[distanceIndex];
                workBuffers_.clearPositionDeltasForBodies(constraint.bodyA, constraint.bodyB);
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
                workBuffers_.applyPositionDeltasForBodies(bodies, constraint.bodyA, constraint.bodyB);
            }
        } else if (!parallelIslands) {
            for (u32 islandIndex = 0; islandIndex < islandCount; ++islandIndex) {
                dispatch_solve_island(bodies,
                                      islandGraph_,
                                      islandIndex,
                                      workBuffers_,
                                      distanceConstraints_,
                                      dt,
                                      params.contactCompliance,
                                      [](const RigidBodySoA& bodySoA, u32 index) {
                                          return effectiveInvMass(bodySoA, index);
                                      });
            }
        } else if (has_dispatchable_islands(islandGraph_)) {
            // Batch small islands so each job carries real work (islands touch disjoint bodies).
            const u32 grain = std::max(kIslandGrainSize, islandCount / 32u);
            fuse::jobs::parallel_for(0, islandCount, grain, [&](u32 islandIndex) {
                dispatch_solve_island(bodies,
                                      islandGraph_,
                                      islandIndex,
                                      workBuffers_,
                                      distanceConstraints_,
                                      dt,
                                      params.contactCompliance,
                                      [](const RigidBodySoA& bodySoA, u32 index) {
                                          return effectiveInvMass(bodySoA, index);
                                      });
            });
        }

        ++lastIterationCount_;
        // The residual costs a full pass over the contact points: only measure it when it can end
        // the loop early, and once after the final iteration for diagnostics.
        const bool finalIteration = iter + 1u == maxIterations;
        if (params.residualTolerance > 0.f || finalIteration) {
            lastConstraintResidual_ = measureConstraintResidual_(bodies);
        }

        if (params.residualTolerance > 0.f && lastConstraintResidual_ <= params.residualTolerance) {
            break;
        }
    }
}

void PBDSolver::updateVelocities(RigidBodySoA& bodies, f32 dt) {
    const f32 invDt = 1.f / dt;
    for (u32 i = 0; i < bodies.count(); ++i) {
        if ((bodies.flags[i] & RB_KINEMATIC) != 0u) {
            bodies.positions[i] = bodies.predictedPositions[i];
            bodies.orientations[i] = bodies.predictedOrientations[i];
            continue;
        }
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
        bodies.angularVelocities[i] =
            angularVelocityBetween(bodies.orientations[i], bodies.predictedOrientations[i], dt);
        bodies.positions[i] = bodies.predictedPositions[i];
        bodies.orientations[i] = bodies.predictedOrientations[i];
    }
}

void PBDSolver::solveVelocities(RigidBodySoA& bodies, const SolverParams& params, f32 dt) {
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers_.contactManifolds();
    const std::vector<f32>& lambdas = workBuffers_.contactLambdas();
    const f32 restingSpeed = 2.f * params.gravity.length() * dt;
    for (u32 contactIndex = 0; contactIndex < contacts.size(); ++contactIndex) {
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid || contactIndex >= lambdas.size()) {
            continue;
        }
        const f32 start = contactIndex < substepLambdaStart_.size() ? substepLambdaStart_[contactIndex] : 0.f;
        const f32 normalLambda = lambdas[contactIndex] - start;
        if (normalLambda <= 0.f) {
            continue; // contact did not push this substep
        }
        if (contactIndex < substepContactSlot_.size()) {
            u32& slot = substepContactSlot_[contactIndex];
            if (slot == kNoSlot) {
                slot = slotForFrameContact_(contact, false); // a speculative contact that pushed
            }
            frameContacts_[slot].impulse += normalLambda / dt;
        }
        const u32 a = contact.bodyA;
        const u32 b = contact.bodyB;
        const f32 invMassA = effectiveInvMass(bodies, a);
        const f32 invMassB = effectiveInvMass(bodies, b);
        if (invMassA + invMassB < 1e-10f) {
            continue;
        }
        const vec3 invInertiaA = workBuffers_.effectiveInvInertia(a, invMassA);
        const vec3 invInertiaB = workBuffers_.effectiveInvInertia(b, invMassB);
        const quat qA = bodies.orientations[a];
        const quat qB = bodies.orientations[b];
        const vec3 n = contact.contactNormal;
        const f32 mu = std::sqrt(bodies.frictionDynamic[a] * bodies.frictionDynamic[b]);
        const f32 restitutionCoeff = bodies.restitutions[a] * bodies.restitutions[b];
        const bool anchored = workBuffers_.hasContactAnchors(contactIndex);

        // Points that pushed this substep, with their arms at the solved pose.
        constexpr u32 kSlots = narrowphase::kMaxContactPointsPerManifold;
        f32 pointImpulse[kSlots]{}; // position-solve normal impulse per point
        vec3 armA[kSlots]{};
        vec3 armB[kSlots]{};
        vec3 spinA[kSlots]{}; // I_A^-1 (rA x n)
        vec3 spinB[kSlots]{};
        f32 target[kSlots]{};
        f32 step[kSlots]{};
        u32 count = 0;
        for (u32 k = 0; k < contact.pointCount && k < kSlots && (anchored || k == 0u); ++k) {
            const f32 pointLambda = anchored ? workBuffers_.contactPointLambdaValue(contactIndex, k) : normalLambda;
            if (pointLambda <= 0.f) {
                continue;
            }
            vec3 rA{};
            vec3 rB{};
            if (anchored) {
                const ContactAnchor& anchor = workBuffers_.contactAnchor(contactIndex, k);
                rA = rotate(qA, anchor.localA);
                rB = rotate(qB, anchor.localB);
            }
            const vec3 relative = (bodies.linearVelocities[a] + bodies.angularVelocities[a].cross(rA)) -
                                  (bodies.linearVelocities[b] + bodies.angularVelocities[b].cross(rB));
            // Restitution against the approach speed before the position solve.
            const vec3 preRelative = (preSolveVelocities_[a] + preSolveAngular_[a].cross(rA)) -
                                     (preSolveVelocities_[b] + preSolveAngular_[b].cross(rB));
            const f32 preNormalSpeed = preRelative.dot(n);
            const f32 restitution = std::fabs(preNormalSpeed) <= restingSpeed ? 0.f : restitutionCoeff;
            spinA[count] = applyInverseInertia(qA, invInertiaA, rA.cross(n));
            spinB[count] = applyInverseInertia(qB, invInertiaB, rB.cross(n));
            const f32 w = invMassA + invMassB + rA.cross(n).dot(spinA[count]) + rB.cross(n).dot(spinB[count]);
            pointImpulse[count] = pointLambda / dt;
            armA[count] = rA;
            armB[count] = rB;
            target[count] = -relative.dot(n) + std::max(-restitution * preNormalSpeed, 0.f);
            step[count] = w > 1e-10f ? target[count] / w : 0.f;
            ++count;
        }
        if (count == 0u) {
            continue;
        }

        // Normal velocity: one block over the points, scaled by the least-squares factor that
        // best meets every point's target (see solveContactConstraint for why not point by point).
        f32 scale = 1.f;
        if (count > 1u) {
            f32 targetDotMoved = 0.f;
            f32 movedSq = 0.f;
            for (u32 i = 0; i < count; ++i) {
                const vec3 ci = armA[i].cross(n);
                const vec3 di = armB[i].cross(n);
                f32 moved = 0.f;
                for (u32 j = 0; j < count; ++j) {
                    moved += step[j] * (invMassA + invMassB + ci.dot(spinA[j]) + di.dot(spinB[j]));
                }
                targetDotMoved += target[i] * moved;
                movedSq += moved * moved;
            }
            scale = movedSq > 1e-20f ? std::clamp(targetDotMoved / movedSq, 0.f, static_cast<f32>(count)) : 0.f;
        }
        f32 normalImpulse = 0.f;
        vec3 turnA{};
        vec3 turnB{};
        for (u32 i = 0; i < count; ++i) {
            step[i] *= scale;
            normalImpulse += step[i];
            turnA += spinA[i] * step[i];
            turnB += spinB[i] * step[i];
        }
        bodies.linearVelocities[a] += n * (normalImpulse * invMassA);
        bodies.linearVelocities[b] -= n * (normalImpulse * invMassB);
        bodies.angularVelocities[a] += turnA;
        bodies.angularVelocities[b] -= turnB;

        // Dynamic friction per point, bounded by mu times the point's net normal impulse this
        // substep (position-solve lambda / dt plus the velocity-level normal correction).
        for (u32 i = 0; i < count; ++i) {
            const f32 netNormalImpulse = std::max(pointImpulse[i] + step[i], 0.f);
            if (netNormalImpulse <= 0.f) {
                continue;
            }
            const vec3 rA = armA[i];
            const vec3 rB = armB[i];
            const vec3 relative = (bodies.linearVelocities[a] + bodies.angularVelocities[a].cross(rA)) -
                                  (bodies.linearVelocities[b] + bodies.angularVelocities[b].cross(rB));
            const vec3 tangential = relative - n * relative.dot(n);
            const f32 tangentialSpeed = tangential.length();
            if (tangentialSpeed <= 1e-9f) {
                continue;
            }
            const vec3 t = tangential * (1.f / tangentialSpeed);
            const f32 wTangent = generalizedInverseMass(invMassA, qA, invInertiaA, rA, t) +
                                 generalizedInverseMass(invMassB, qB, invInertiaB, rB, t);
            if (wTangent < 1e-10f) {
                continue;
            }
            const vec3 impulse = t * (-std::min(mu * netNormalImpulse * wTangent, tangentialSpeed) / wTangent);
            bodies.linearVelocities[a] += impulse * invMassA;
            bodies.linearVelocities[b] -= impulse * invMassB;
            bodies.angularVelocities[a] += applyInverseInertia(qA, invInertiaA, rA.cross(impulse));
            bodies.angularVelocities[b] -= applyInverseInertia(qB, invInertiaB, rB.cross(impulse));
        }
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

    // Nothing awake or driven: only the sleep bookkeeping runs. Nothing moves, so the last
    // step's contacts (and contact count) still describe the scene.
    bool anyActive = false;
    for (u32 i = 0; i < bodies.count(); ++i) {
        u32& flags = bodies.flags[i];
        if (isSleeping(flags) &&
            (bodies.forces[i].dot(bodies.forces[i]) > 0.f || bodies.torques[i].dot(bodies.torques[i]) > 0.f)) {
            flags &= ~RB_SLEEPING; // an applied force wakes the body
            bodies.sleepTimers[i] = 0.f;
        }
        anyActive = anyActive || (flags & RB_KINEMATIC) != 0u || ((flags & RB_STATIC) == 0u && !isSleeping(flags));
    }
    if (!anyActive) {
        lastActiveCount_ = 0;
        lastIterationCount_ = 0;
        lastConstraintResidual_ = 0.f;
        lastCcdHitCount_ = 0;
        for (u32 i = 0; i < bodies.count(); ++i) {
            bodies.forces[i] = {};
            bodies.torques[i] = {};
        }
        return;
    }

    frameContacts_.clear();
    frameContactSlot_.clear();
    mapBodyShapes_(bodies, shapes);
    computeInverseInertia_(bodies, shapes);

    if (params.enableCcd) {
        applyContinuousCollision(bodies, shapes, dt);
    } else {
        lastCcdHitCount_ = 0;
    }

    const f32 subDt = dt / static_cast<f32>(std::max(1u, params.substeps));
    lastActiveCount_ = 0;
    lastContactCount_ = 0;
    const std::vector<f32> priorDistanceLambdas = workBuffers_.distanceLambdas();
    const std::vector<f32> priorContactLambdas = workBuffers_.contactLambdas();

    for (u32 substep = 0; substep < std::max(1u, params.substeps); ++substep) {
        predict(bodies, params, subDt);
        preSolveVelocities_ = bodies.linearVelocities;
        preSolveAngular_ = bodies.angularVelocities;
        generateContacts(bodies, shapes, params);
        if (substep == 0u) {
            frame_lambda_warm_start(workBuffers_,
                                    distanceConstraints_,
                                    priorDistanceLambdas,
                                    priorContactLambdas);
        }
        recordFrameContacts_(bodies, params);
        workBuffers_.prepareContactPoints(bodies);
        substepLambdaStart_ = workBuffers_.contactLambdas();
        runConstraintIterations(bodies, params, subDt);
        updateVelocities(bodies, subDt);
        solveVelocities(bodies, params, subDt);
    }

    for (u32 i = 0; i < bodies.count(); ++i) {
        bodies.forces[i] = {};
        bodies.torques[i] = {};
    }
    applyDamping(bodies, params);
    detectSleep(bodies, params, dt);

    lastContactCount_ = 0;
    for (const narrowphase::ContactManifold& manifold : workBuffers_.contactManifolds()) {
        lastContactCount_ += isTouching(manifold) ? 1u : 0u;
    }
    for (u32 i = 0; i < bodies.count(); ++i) {
        if (!isStaticOrKinematic(bodies.flags[i]) && !isSleeping(bodies.flags[i])) {
            ++lastActiveCount_;
        }
    }
}

} // namespace fuse::physics
