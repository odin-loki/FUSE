#include <fuse/physics/solver/pbd_island_solve.hpp>

#include <fuse/physics/solver/constraint_accumulation.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fuse::physics {
namespace {

bool isStaticOrKinematic(u32 flags) {
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;
}

bool isSleeping(u32 flags) {
    return (flags & RB_SLEEPING) != 0u;
}

} // namespace

bool is_body_sleeping(u32 flags) {
    return isSleeping(flags);
}

bool is_body_static_or_kinematic(u32 flags) {
    return isStaticOrKinematic(flags);
}

bool is_sleep_candidate_body(const RigidBodySoA& bodies,
                             u32 index,
                             f32 sleepLinearThreshold,
                             f32 sleepAngularThreshold) {
    if (index >= bodies.count()) {
        return false;
    }
    if (isStaticOrKinematic(bodies.flags[index]) || isSleeping(bodies.flags[index])) {
        return false;
    }
    const f32 linearSpeed = bodies.linearVelocities[index].length();
    const f32 angularSpeed = bodies.angularVelocities[index].length();
    return linearSpeed < sleepLinearThreshold && angularSpeed < sleepAngularThreshold;
}

bool is_wake_candidate_body(const RigidBodySoA& bodies,
                            u32 index,
                            f32 sleepLinearThreshold,
                            f32 sleepAngularThreshold) {
    if (index >= bodies.count()) {
        return false;
    }
    if (!isSleeping(bodies.flags[index]) || isStaticOrKinematic(bodies.flags[index])) {
        return false;
    }
    const f32 linearSpeed = bodies.linearVelocities[index].length();
    const f32 angularSpeed = bodies.angularVelocities[index].length();
    return linearSpeed >= sleepLinearThreshold || angularSpeed >= sleepAngularThreshold;
}

u32 count_sleeping_bodies(const RigidBodySoA& bodies) {
    u32 count = 0;
    for (u32 index = 0; index < bodies.count(); ++index) {
        if (isSleeping(bodies.flags[index])) {
            ++count;
        }
    }
    return count;
}

SleepPassPreflight preflight_sleep_pass(const RigidBodySoA& bodies,
                                        f32 sleepLinearThreshold,
                                        f32 sleepAngularThreshold) {
    SleepPassPreflight preflight{};
    preflight.stats.totalBodies = bodies.count();
    for (u32 index = 0; index < bodies.count(); ++index) {
        if (isStaticOrKinematic(bodies.flags[index])) {
            ++preflight.stats.staticCount;
            continue;
        }
        if (isSleeping(bodies.flags[index])) {
            ++preflight.stats.sleepingCount;
            continue;
        }
        ++preflight.stats.activeCount;
        if (is_sleep_candidate_body(bodies, index, sleepLinearThreshold, sleepAngularThreshold)) {
            ++preflight.stats.sleepCandidateCount;
        }
    }
    preflight.skipped = preflight.stats.activeCount == 0u;
    return preflight;
}

bool should_skip_sleep_pass(const RigidBodySoA& bodies,
                            f32 sleepLinearThreshold,
                            f32 sleepAngularThreshold) {
    return !preflight_sleep_pass(bodies, sleepLinearThreshold, sleepAngularThreshold).can_sleep_pass();
}

WakePreflight preflight_wake_candidates(const RigidBodySoA& bodies,
                                        f32 sleepLinearThreshold,
                                        f32 sleepAngularThreshold) {
    WakePreflight preflight{};
    for (u32 index = 0; index < bodies.count(); ++index) {
        if (!isSleeping(bodies.flags[index]) || isStaticOrKinematic(bodies.flags[index])) {
            continue;
        }
        ++preflight.stats.sleepingCount;
        if (is_wake_candidate_body(bodies, index, sleepLinearThreshold, sleepAngularThreshold)) {
            ++preflight.stats.wakeCandidateCount;
        } else {
            ++preflight.stats.restingSleepingCount;
        }
    }
    preflight.skipped = preflight.stats.sleepingCount == 0u;
    return preflight;
}

bool is_valid_constraint_body_pair(const RigidBodySoA& bodies, u32 bodyA, u32 bodyB) {
    return bodyA < bodies.count() && bodyB < bodies.count();
}

bool island_all_bodies_inactive(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    if (island.bodyIndices.empty()) {
        return true;
    }
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (!isStaticOrKinematic(bodies.flags[bodyIndex]) && !isSleeping(bodies.flags[bodyIndex])) {
            return false;
        }
    }
    return true;
}

bool island_has_active_bodies(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    return !island_all_bodies_inactive(bodies, island);
}

bool should_skip_island_solve_all_inactive(const RigidBodySoA& bodies,
                                           const ContactIslandGraph::Island& island) {
    return island_all_bodies_inactive(bodies, island);
}

IslandSolveWorkPreflight preflight_island_solve_work(const RigidBodySoA& bodies,
                                                     const SolverWorkBuffers& workBuffers) {
    IslandSolveWorkPreflight preflight{};
    preflight.bodyCount = bodies.count();
    preflight.bufferCapacity = workBuffers.bodyCapacity();
    preflight.insufficientBufferCapacity = preflight.bufferCapacity < preflight.bodyCount;
    preflight.skipped = preflight.bodyCount == 0u;
    return preflight;
}

IslandConstraintIndexPreflight preflight_island_constraint_indices(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    u32 contactCount,
    u32 distanceCount) {
    IslandConstraintIndexPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contactCount) {
            ++preflight.oobContactIndexCount;
        }
    }
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceCount) {
            ++preflight.oobDistanceIndexCount;
        }
    }
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            ++preflight.oobBodyRefCount;
        }
    }
    return preflight;
}

IslandConstraintIndexPreflight preflight_island_constraint_indices_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    u32 contactCount,
    u32 distanceCount) {
    IslandConstraintIndexPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_constraint_indices(graph.island(islandIndex), bodies, contactCount, distanceCount);
}

namespace {

constexpr f32 kImmovableInvMassEpsilon = 1e-10f;

bool bodyHasPendingForces(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    }
    const vec3& force = bodies.forces[bodyIndex];
    return force.x != 0.f || force.y != 0.f || force.z != 0.f;
}

} // namespace

namespace {

bool body_index_in_range(u32 bodyIndex, u32 bodyCount) {
    return bodyIndex < bodyCount;
}

} // namespace

bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (!body_index_in_range(bodyIndex, bodies.count())) {
        return false;
    }
    return (bodies.flags[bodyIndex] & RB_SLEEPING) != 0u;
}

bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (!body_index_in_range(bodyIndex, bodies.count())) {
        return false;
    }
    const u32 flags = bodies.flags[bodyIndex];
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;
}

bool body_has_effective_mass(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (!body_index_in_range(bodyIndex, bodies.count())) {
        return false;
    }
    if (is_body_static_or_kinematic(bodies, bodyIndex)) {
        return false;
    }
    if (is_body_sleeping(bodies, bodyIndex)) {
        return false;
    }
    return bodies.invMasses[bodyIndex] > 0.f;
}

bool island_all_bodies_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return false;
    }

    bool hasDynamicBody = false;
    for (u32 bodyIndex : island.bodyIndices) {
        if (!body_index_in_range(bodyIndex, bodies.count())) {
            continue;
        }
        if (is_body_static_or_kinematic(bodies, bodyIndex)) {
            continue;
        }
        hasDynamicBody = true;
        if (!is_body_sleeping(bodies, bodyIndex)) {
            return false;
        }
    }
    return hasDynamicBody;
}

bool island_has_movable_bodies(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    for (u32 bodyIndex : island.bodyIndices) {
        if (body_has_effective_mass(bodies, bodyIndex)) {
            return true;
        }
    }
    return false;
}

bool island_has_awake_dynamic_bodies(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    for (u32 bodyIndex : island.bodyIndices) {
        if (!body_index_in_range(bodyIndex, bodies.count())) {
            continue;
        }
        if (is_body_static_or_kinematic(bodies, bodyIndex)) {
            continue;
        }
        if (!is_body_sleeping(bodies, bodyIndex)) {
            return true;
        }
    }
    return false;
}

IslandConstraintIndexPreflight preflight_island_constraint_indices(
    const ContactIslandGraph::Island& island,
    u32 contactCount,
    u32 distanceCount) {
    IslandConstraintIndexPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contactCount) {
            ++preflight.orphanedContactIndexCount;
        }
    }
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceCount) {
            ++preflight.orphanedDistanceIndexCount;
        }
    }
    return preflight;
}

IslandSleepPreflight preflight_sleeping_island(const ContactIslandGraph::Island& island,
                                               const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    for (u32 bodyIndex : island.bodyIndices) {
        if (!body_index_in_range(bodyIndex, bodies.count())) {
            continue;
        }
        ++preflight.bodyCount;
        if (is_body_static_or_kinematic(bodies, bodyIndex)) {
            ++preflight.staticBodyCount;
            continue;
        }
        if (is_body_sleeping(bodies, bodyIndex)) {
            ++preflight.sleepingBodyCount;
        }
        if (body_has_effective_mass(bodies, bodyIndex)) {
            ++preflight.movableBodyCount;
        }
    }

    const u32 dynamicBodyCount = preflight.bodyCount - preflight.staticBodyCount;
    preflight.allSleeping = dynamicBodyCount > 0u && preflight.sleepingBodyCount == dynamicBodyCount;
    preflight.noMovableBodies = preflight.movableBodyCount == 0u;
    return preflight;
}

IslandSleepPreflight preflight_sleeping_island_by_index(const ContactIslandGraph& graph,
                                                        u32 islandIndex,
                                                        const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_sleeping_island(graph.island(islandIndex), bodies);
}

bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies) {
    const IslandSleepPreflight preflight = preflight_sleeping_island(island, bodies);
    return preflight.skipped || !preflight.can_solve();
}

bool should_skip_sleeping_island_solve_job(const IslandSolveJob& job,
                                           const RigidBodySoA& bodies) {
    if (should_skip_island_solve_job(job)) {
        return true;
    }
    if (job.island == nullptr) {
        return true;
    }
    return should_skip_sleeping_island_solve(*job.island, bodies);
}

WakeOnImpulsePreflight preflight_wake_on_impulse(const RigidBodySoA& bodies,
                                                 u32 bodyIndex,
                                                 f32 wakeEpsilonSq) {
    WakeOnImpulsePreflight preflight{};
    if (!body_index_in_range(bodyIndex, bodies.count())) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.sleeping = is_body_sleeping(bodies, bodyIndex);
    preflight.forceMagnitudeSq = bodies.forces[bodyIndex].dot(bodies.forces[bodyIndex]);
    preflight.torqueMagnitudeSq = bodies.torques[bodyIndex].dot(bodies.torques[bodyIndex]);
    preflight.shouldWake =
        preflight.sleeping &&
        (preflight.forceMagnitudeSq > wakeEpsilonSq || preflight.torqueMagnitudeSq > wakeEpsilonSq);
    return preflight;
}

bool should_wake_body_on_impulse(const RigidBodySoA& bodies,
                                 u32 bodyIndex,
                                 f32 wakeEpsilonSq) {
    return preflight_wake_on_impulse(bodies, bodyIndex, wakeEpsilonSq).can_wake();
}

IslandBodyPartitionPreflight preflight_island_body_partition(const ContactIslandGraph& graph) {
    IslandBodyPartitionPreflight preflight{};
    preflight.totalIslands = graph.islandCount();
    if (preflight.totalIslands == 0u) {
        preflight.skipped = true;
        return preflight;
    }

    std::vector<bool> seen;
    for (u32 islandIndex = 0; islandIndex < preflight.totalIslands; ++islandIndex) {
        for (u32 bodyIndex : graph.island(islandIndex).bodyIndices) {
            ++preflight.totalBodySlots;
            if (bodyIndex >= seen.size()) {
                seen.resize(bodyIndex + 1u, false);
            }
            if (seen[bodyIndex]) {
                ++preflight.duplicateBodyCount;
                preflight.partitionValid = false;
            } else {
                seen[bodyIndex] = true;
            }
        }
    }
    return preflight;
}

ConstraintIterationPreflight preflight_constraint_iterations(const SolverParams& params) {
    ConstraintIterationPreflight preflight{};
    preflight.iterations = params.iterations;
    preflight.residualTolerance = params.residualTolerance;
    preflight.zeroIterations = params.iterations == 0u;
    preflight.skipped = preflight.zeroIterations;
    return preflight;
}

bool should_skip_constraint_iterations(const SolverParams& params) {
    return !preflight_constraint_iterations(params).can_iterate();
}

namespace {

bool bodyIndexInRange(u32 bodyIndex, u32 bodyCount) {
    return bodyIndex < bodyCount;
}

bool contactReferencesValidBodies(const narrowphase::ContactManifold& contact, u32 bodyCount) {
    return bodyIndexInRange(contact.bodyA, bodyCount) && bodyIndexInRange(contact.bodyB, bodyCount);
}

bool distanceConstraintReferencesValidBodies(const DistanceConstraint& constraint, u32 bodyCount) {
    return bodyIndexInRange(constraint.bodyA, bodyCount) && bodyIndexInRange(constraint.bodyB, bodyCount);
}

} // namespace

const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandBuildRejectReason::OutOfRangeContact:
        return "OutOfRangeContact";
    case IslandBuildRejectReason::OutOfRangeDistance:
        return "OutOfRangeDistance";
    }
    return "Unknown";
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    case IslandConstraintSolveRejectReason::InvalidDt:
        return "InvalidDt";
    }
    return "Unknown";
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";
    }
    return "Unknown";
}

bool is_body_sleeping(u32 flags);
bool is_body_static_or_kinematic(u32 flags);
bool is_body_movable(const RigidBodySoA& bodies, u32 bodyIndex);

namespace {

IslandBuildRejectReason diagnose_island_build_reject(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;

    for (const narrowphase::ContactManifold& contact : contacts) {
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++inRangeContactCount;
        } else if (contact.valid) {
            ++outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            ++inRangeDistanceCount;
        } else {
            ++outOfRangeDistanceBodyCount;
        }
    }

    if (bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return IslandBuildRejectReason::EmptyInput;
    }
    if (outOfRangeContactBodyCount > 0u || outOfRangeDistanceBodyCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeRefs;
    }
    return IslandBuildRejectReason::None;
}

IslandDispatchRejectReason diagnose_island_dispatch_reject(const ContactIslandGraph& graph, f32 dt) {
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::EmptyGraph;
    }
    if (!is_valid_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!std::isfinite(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    return IslandDispatchRejectReason::None;
}

IslandSolveJobRejectReason diagnose_island_solve_job_reject(const IslandSolveJob& job, f32 dt) {
    if (job.island == nullptr || job.islandIndex == ContactIslandGraph::invalidIsland) {
        return IslandSolveJobRejectReason::OutOfRangeIndex;
    }
    if (job.empty || job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::EmptyIsland;
    }
    if (!is_valid_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (!std::isfinite(dt)) {
        return IslandSolveJobRejectReason::NonFiniteDt;
    }
    return IslandSolveJobRejectReason::None;
}

IslandConstraintSolveRejectReason diagnose_island_constraint_solve_reject(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }

    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex < contacts.size()) {
            ++inRangeContactCount;
        }
    }
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex < distanceConstraints.size()) {
            ++inRangeDistanceCount;
        }
    }
    if (inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }

    u32 movableCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (is_body_movable(bodies, bodyIndex)) {
            ++movableCount;
        }
    }
    if (movableCount == 0u) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

IslandSleepRejectReason diagnose_island_sleep_reject(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandWakeRejectReason diagnose_island_wake_reject(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    const bool hasMixedSleepState = sleepingCount > 0u && activeDynamicCount > 0u;
    if (!hasMixedSleepState || activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoWakeTarget;
    }
    return IslandWakeRejectReason::None;
}

} // namespace

const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandBuildRejectReason::OutOfRangeRefs:
        return "OutOfRangeRefs";
    default:
        return "Unknown";
    }
}

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return diagnose_island_build_reject(bodyCount, contacts, distanceConstraints);
}

bool island_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::EmptyGraph:
        return "EmptyGraph";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    return diagnose_island_dispatch_reject(graph, dt);
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandSolveJobRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    return diagnose_island_solve_job_reject(job, dt);
}

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                         f32 dt,
                                         IslandSolveJobRejectReason expected) {
    return island_solve_job_reject_reason(job, dt) == expected;
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    default:
        return "Unknown";
    }
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return diagnose_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints);
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies) {
    return diagnose_island_sleep_reject(island, bodies);
}

bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected) {
    return island_sleep_reject_reason(island, bodies) == expected;
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoWakeTarget:
        return "NoWakeTarget";
    default:
        return "Unknown";
    }
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    return diagnose_island_wake_reject(island, bodies);
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

namespace {

IslandBuildRejectReason classifyIslandBuildRejectReason(const IslandBuildPreflight& preflight) {
    if (preflight.skipped) {
        return IslandBuildRejectReason::EmptyInput;
    }
    if (preflight.stats.outOfRangeContactBodyCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeContactBody;
    }
    if (preflight.stats.outOfRangeDistanceBodyCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeDistanceBody;
    }
    return IslandBuildRejectReason::None;
}

IslandConstraintSolveRejectReason classifyIslandConstraintSolveRejectReason(
    const IslandConstraintSolvePreflight& preflight) {
    if (preflight.skipped) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (!preflight.refs.can_solve()) {
        if (preflight.refs.inRangeContactCount == 0u && preflight.refs.ownedContactCount > 0u) {
            return IslandConstraintSolveRejectReason::StaleContactRefs;
        }
        if (preflight.refs.inRangeDistanceCount == 0u && preflight.refs.ownedDistanceCount > 0u) {
            return IslandConstraintSolveRejectReason::StaleDistanceRefs;
        }
        return IslandConstraintSolveRejectReason::StaleContactRefs;
    }
    if (!preflight.bodies.can_solve()) {
        if (preflight.bodies.movableCount == 0u && preflight.bodies.sleepingCount > 0u) {
            return IslandConstraintSolveRejectReason::AllSleeping;
        }
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

IslandDispatchRejectReason classifyIslandDispatchRejectReason(const IslandSolveJobPreflight& jobPreflight,
                                                              f32 dt) {
    if (dt <= 0.f) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!std::isfinite(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    if (jobPreflight.skipped) {
        return jobPreflight.constraintCount == 0u ? IslandDispatchRejectReason::NoConstraints
                                                  : IslandDispatchRejectReason::EmptyJob;
    }
    return IslandDispatchRejectReason::None;
}

IslandDispatchRejectReason classifyIslandDispatchRejectReason(const IslandDispatchJobPreflight& preflight) {
    if (preflight.reason != IslandDispatchRejectReason::None) {
        return preflight.reason;
    }
    return IslandDispatchRejectReason::None;
}

} // namespace

const char* islandBuildRejectReasonName(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandBuildRejectReason::OutOfRangeContactBody:
        return "OutOfRangeContactBody";
    case IslandBuildRejectReason::OutOfRangeDistanceBody:
        return "OutOfRangeDistanceBody";
    default:
        return "Unknown";
    }
}

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::StaleContactRefs:
        return "StaleContactRefs";
    case IslandConstraintSolveRejectReason::StaleDistanceRefs:
        return "StaleDistanceRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    case IslandConstraintSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandDispatchRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandDispatchRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandDispatchRejectReason::NoConstraints:
        return "NoConstraints";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::AllSleeping:
        return "AllSleeping";
    case IslandDispatchRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    case IslandDispatchRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    default:
        return "Unknown";
    }
}

const char* islandWarmStartRejectReasonName(IslandWarmStartRejectReason reason) {
    switch (reason) {
    case IslandWarmStartRejectReason::None:
        return "None";
    case IslandWarmStartRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWarmStartRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandWarmStartRejectReason::NoPriorData:
        return "NoPriorData";
    case IslandWarmStartRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandWarmStartRejectReason::NoImpulses:
        return "NoImpulses";
    default:
        return "Unknown";
    }
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    }
    return "Unknown";
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    }
    return "Unknown";
}

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (should_skip_island_solve_job(job)) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    return IslandSolveJobRejectReason::None;
}

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                         f32 dt,
                                         IslandSolveJobRejectReason expected) {
    return island_solve_job_reject_reason(job, dt) == expected;
}

const char* island_constraint_refs_reject_reason_name(IslandConstraintRefsRejectReason reason) {
    switch (reason) {
    case IslandConstraintRefsRejectReason::None:
        return "None";
    case IslandConstraintRefsRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintRefsRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    }
    return "Unknown";
}

IslandConstraintRefsRejectReason island_constraint_refs_reject_reason(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintRefsRejectReason::EmptyIsland;
    }

    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex < contacts.size()) {
            ++inRangeContactCount;
        }
    }
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex < distanceConstraints.size()) {
            ++inRangeDistanceCount;
        }
    }

    if (inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return IslandConstraintRefsRejectReason::NoInRangeRefs;
    }
    return IslandConstraintRefsRejectReason::None;
}

bool island_constraint_refs_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintRefsRejectReason expected) {
    return island_constraint_refs_reject_reason(island, contacts, distanceConstraints) == expected;
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    const IslandConstraintRefsRejectReason refsReason =
        island_constraint_refs_reject_reason(island, contacts, distanceConstraints);
    if (refsReason == IslandConstraintRefsRejectReason::EmptyIsland) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (refsReason == IslandConstraintRefsRejectReason::NoInRangeRefs) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }

    u32 movableCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (is_body_movable(bodies, bodyIndex)) {
            ++movableCount;
        }
    }
    if (movableCount == 0u) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandSleepRejectReason island_sleep_reject_reason_by_index(const ContactIslandGraph& graph,
                                                            u32 islandIndex,
                                                            const RigidBodySoA& bodies) {
    if (!island_index_valid(graph, islandIndex)) {
        return IslandSleepRejectReason::OutOfRangeIndex;
    }
    return island_sleep_reject_reason(graph.island(islandIndex), bodies);
}

bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected) {
    return island_sleep_reject_reason(island, bodies) == expected;
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    }
    return "Unknown";
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    const IslandSleepGraphStats stats = compute_island_sleep_stats(graph, bodies);
    if (stats.fullyActiveCount == 0u && stats.mixedSleepCount == 0u) {
        return IslandSleepGraphRejectReason::AllIslandsSleeping;
    }
    return IslandSleepGraphRejectReason::None;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandWakeRejectReason::NoWakeTarget:
        return "NoWakeTarget";
    }
    return "Unknown";
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (sleepingCount == 0u || activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoWakeTarget;
    }
    return IslandWakeRejectReason::None;
}

IslandWakeRejectReason island_wake_reject_reason_by_index(const ContactIslandGraph& graph,
                                                          u32 islandIndex,
                                                          const RigidBodySoA& bodies) {
    if (!island_index_valid(graph, islandIndex)) {
        return IslandWakeRejectReason::OutOfRangeIndex;
    }
    return island_wake_reject_reason(graph.island(islandIndex), bodies);
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    if (compute_island_wake_stats(graph, bodies).wakeableCount == 0u) {
        return IslandWakeGraphRejectReason::NoWakeableIslands;
    }
    return IslandWakeGraphRejectReason::None;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    }
    return "Unknown";
}

const char* islandSolveRejectReasonName(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "None";
    case IslandSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

const char* islandSleepRejectReasonName(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    case IslandSleepRejectReason::NoSolveableIslands:
        return "NoSolveableIslands";
    }
    return "Unknown";
}

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandWakeRejectReason::NoWakeTarget:
        return "NoWakeTarget";
    case IslandWakeRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

IslandDispatchRejectReason islandDispatchRejectReason(const ContactIslandGraph& graph, f32 dt);
bool islandDispatchRejectsForReason(const ContactIslandGraph& graph,
                                    f32 dt,
                                    IslandDispatchRejectReason expected);
IslandSolveRejectReason islandSolveJobRejectReason(const IslandSolveJob& job, f32 dt);
IslandSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);
bool islandSolveRejectsForReason(const IslandSolveJob& job,
                                 f32 dt,
                                 IslandSolveRejectReason expected);
bool islandConstraintSolveRejectsForReason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           const std::vector<narrowphase::ContactManifold>& contacts,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                           IslandSolveRejectReason expected);
IslandSleepRejectReason islandSleepRejectReason(const ContactIslandGraph::Island& island,
                                                const RigidBodySoA& bodies);
IslandSleepRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies);
bool islandSleepRejectsForReason(const ContactIslandGraph::Island& island,
                                 const RigidBodySoA& bodies,
                                 IslandSleepRejectReason expected);
IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,
                                              const RigidBodySoA& bodies);
IslandWakeRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,
                                                   const RigidBodySoA& bodies);
bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies,
                                IslandWakeRejectReason expected);

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    }
    return "Unknown";
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    }
    return "Unknown";
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::NoSolveableIslands:
        return "NoSolveableIslands";
    }
    return "Unknown";
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    }
    return "Unknown";
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    }
    return "Unknown";
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    }
    return "Unknown";
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeConstraints:
        return "NoInRangeConstraints";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::NoSolveableIslands:
        return "NoSolveableIslands";
    }
    return "Unknown";
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandWakeRejectReason::NoMixedState:
        return "NoMixedState";
    }
    return "Unknown";
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (job.empty || job.island == nullptr || job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    return IslandSolveJobRejectReason::None;
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (should_skip_island_constraint_refs(island, contacts, distanceConstraints)) {
        return IslandConstraintSolveRejectReason::NoInRangeConstraints;
    }
    if (should_skip_island_solve_bodies(island, bodies)) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    const IslandSleepGraphStats stats = compute_island_sleep_stats(graph, bodies);
    if (stats.fullyActiveCount == 0u && stats.mixedSleepCount == 0u) {
        return IslandSleepGraphRejectReason::NoSolveableIslands;
    }
    return IslandSleepGraphRejectReason::None;
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (sleepingCount == 0u || activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoMixedState;
    }
    return IslandWakeRejectReason::None;
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    if (compute_island_wake_stats(graph, bodies).wakeableCount == 0u) {
        return IslandWakeGraphRejectReason::NoWakeableIslands;
    }
    return IslandWakeGraphRejectReason::None;
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                         f32 dt,
                                         IslandSolveJobRejectReason expected) {
    return island_solve_job_reject_reason(job, dt) == expected;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected) {
    return island_sleep_reject_reason(island, bodies) == expected;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    }
    return "Unknown";
}

const char* islandSolveJobRejectReasonName(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    }
    return "Unknown";
}

const char* islandSolveRejectReasonName(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "None";
    case IslandSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveRejectReason::OutOfRangeRefs:
        return "OutOfRangeRefs";
    case IslandSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

const char* islandSleepRejectReasonName(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::AllSleepingOrEmpty:
        return "AllSleepingOrEmpty";
    }
    return "Unknown";
}

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoWakeTarget:
        return "NoWakeTarget";
    }
    return "Unknown";
}

const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

IslandGraphBuildRejectReason classifyIslandGraphBuildReject(const IslandBuildPreflight& preflight) {
    if (preflight.skipped) {
        return IslandGraphBuildRejectReason::EmptyInput;
    }
    if (preflight.stats.outOfRangeContactBodyCount > 0u) {
        return IslandGraphBuildRejectReason::OutOfRangeContactRefs;
    }
    if (preflight.stats.outOfRangeDistanceBodyCount > 0u) {
        return IslandGraphBuildRejectReason::OutOfRangeDistanceRefs;
    }
    return IslandGraphBuildRejectReason::None;
}

IslandDispatchRejectReason classifyIslandDispatchReject(const IslandDispatchPreflight& preflight) {
    if (preflight.invalidDt) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (preflight.solve.skipped || preflight.solve.stats.dispatchableCount == 0u) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

IslandSolveJobRejectReason classifyIslandSolveJobReject(const IslandSolveJobPreflight& preflight) {
    if (preflight.invalidDt) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (preflight.skipped) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    if (preflight.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    }
    return IslandSolveJobRejectReason::None;
}

IslandSolveRejectReason classifyIslandConstraintSolveReject(const IslandConstraintSolvePreflight& preflight) {
    if (preflight.skipped) {
        return IslandSolveRejectReason::EmptyIsland;
    }
    if (!preflight.refs.can_solve()) {
        return IslandSolveRejectReason::OutOfRangeRefs;
    }
    if (!preflight.bodies.can_solve()) {
        return IslandSolveRejectReason::NoMovableBodies;
    }
    return IslandSolveRejectReason::None;
}

IslandSleepRejectReason classifyIslandSleepReject(const IslandSleepPreflight& preflight) {
    if (preflight.skipped) {
        return IslandSleepRejectReason::EmptyIsland;
    }
    if (preflight.allSleeping) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandSleepGraphRejectReason classifyIslandSleepGraphReject(const IslandSleepGraphPreflight& preflight) {
    if (preflight.skipped || !preflight.has_solveable_islands()) {
        return IslandSleepGraphRejectReason::AllSleepingOrEmpty;
    }
    return IslandSleepGraphRejectReason::None;
}

IslandWakeRejectReason classifyIslandWakeReject(const IslandWakePreflight& preflight) {
    if (preflight.skipped) {
        return IslandWakeRejectReason::EmptyIsland;
    }
    if (!preflight.should_wake_sleepers()) {
        return IslandWakeRejectReason::NoWakeTarget;
    }
    return IslandWakeRejectReason::None;
}

IslandWakeGraphRejectReason classifyIslandWakeGraphReject(const IslandWakeGraphPreflight& preflight) {
    if (preflight.skipped || !preflight.can_wake()) {
        return IslandWakeGraphRejectReason::NoWakeableIslands;
    }
    return IslandWakeGraphRejectReason::None;
}

bool tryPreflightIslandBuild(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints,
                             IslandGraphBuildRejectReason& reason) {
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    reason = preflight.reason;
    return preflight.can_build();
}

namespace {

IslandDispatchRejectReason classify_dispatch_dt_reject(f32 dt) {
    if (dt <= 0.f) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!std::isfinite(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    return IslandDispatchRejectReason::None;
}

IslandSolveRejectReason classify_solve_dt_reject(f32 dt) {
    if (dt <= 0.f) {
        return IslandSolveRejectReason::InvalidDt;
    }
    if (!std::isfinite(dt)) {
        return IslandSolveRejectReason::NonFiniteDt;
    }
    return IslandSolveRejectReason::None;
}

} // namespace

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandDispatchRejectReason::NothingDispatchable:
        return "NothingDispatchable";
    }
    return "Unknown";
}

const char* island_solve_reject_reason_name(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "None";
    case IslandSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveRejectReason::OutOfRangeIslandIndex:
        return "OutOfRangeIslandIndex";
    case IslandSolveRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandSolveRejectReason::NoInRangeConstraints:
        return "NoInRangeConstraints";
    case IslandSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIslandIndex:
        return "OutOfRangeIslandIndex";
    case IslandSleepRejectReason::NotAllSleeping:
        return "NotAllSleeping";
    }
    return "Unknown";
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIslandIndex:
        return "OutOfRangeIslandIndex";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    }
    return "Unknown";
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::NoSolveableIslands:
        return "NoSolveableIslands";
    }
    return "Unknown";
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

const char* islandSolveRejectReasonName(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "none";
    case IslandSolveRejectReason::NoDispatchableIslands:
        return "no_dispatchable_islands";
    }
    return "unknown";
}

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "none";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "no_dispatchable_islands";
    case IslandDispatchRejectReason::InvalidDt:
        return "invalid_dt";
    }
    return "unknown";
}

const char* islandSolveJobRejectReasonName(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "none";
    case IslandSolveJobRejectReason::EmptyJob:
        return "empty_job";
    case IslandSolveJobRejectReason::InvalidDt:
        return "invalid_dt";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "zero_constraints";
    }
    return "unknown";
}

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "none";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "empty_island";
    case IslandConstraintSolveRejectReason::NoInRangeConstraints:
        return "no_in_range_constraints";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "no_movable_bodies";
    }
    return "unknown";
}

const char* islandSleepRejectReasonName(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "none";
    case IslandSleepRejectReason::EmptyIsland:
        return "empty_island";
    case IslandSleepRejectReason::OutOfRangeIndex:
        return "out_of_range_index";
    case IslandSleepRejectReason::AllSleeping:
        return "all_sleeping";
    }
    return "unknown";
}

const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "none";
    case IslandSleepGraphRejectReason::NoSolveableIslands:
        return "no_solveable_islands";
    }
    return "unknown";
}

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "none";
    case IslandWakeRejectReason::EmptyIsland:
        return "empty_island";
    case IslandWakeRejectReason::OutOfRangeIndex:
        return "out_of_range_index";
    case IslandWakeRejectReason::NoWakeTarget:
        return "no_wake_target";
    }
    return "unknown";
}

const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "none";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "no_wakeable_islands";
    }
    return "unknown";
}

IslandSolveRejectReason islandSolveRejectReason(const IslandSolvePreflight& preflight) {
    if (preflight.stats.dispatchableCount == 0u) {
        return IslandSolveRejectReason::NoDispatchableIslands;
    }
    return IslandSolveRejectReason::None;
}

IslandDispatchRejectReason islandDispatchRejectReason(const IslandDispatchPreflight& preflight) {
    if (preflight.invalidDt) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (preflight.solve.skipped) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

IslandSolveJobRejectReason islandSolveJobRejectReason(const IslandSolveJobPreflight& preflight) {
    if (preflight.skipped) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    if (preflight.invalidDt) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (preflight.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    }
    return IslandSolveJobRejectReason::None;
}

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    const IslandConstraintSolvePreflight& preflight) {
    if (preflight.skipped) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (!preflight.refs.can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeConstraints;
    }
    if (!preflight.bodies.can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

IslandSleepRejectReason islandSleepRejectReason(const IslandSleepPreflight& preflight) {
    if (preflight.skipped) {
        return IslandSleepRejectReason::EmptyIsland;
    }
    if (preflight.allSleeping) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandSleepGraphRejectReason islandSleepGraphRejectReason(const IslandSleepGraphPreflight& preflight) {
    if (preflight.skipped) {
        return IslandSleepGraphRejectReason::NoSolveableIslands;
    }
    return IslandSleepGraphRejectReason::None;
}

IslandWakeRejectReason islandWakeRejectReason(const IslandWakePreflight& preflight) {
    if (preflight.skipped) {
        return IslandWakeRejectReason::EmptyIsland;
    }
    if (!preflight.should_wake_sleepers()) {
        return IslandWakeRejectReason::NoWakeTarget;
    }
    return IslandWakeRejectReason::None;
}

IslandWakeGraphRejectReason islandWakeGraphRejectReason(const IslandWakeGraphPreflight& preflight) {
    if (preflight.skipped) {
        return IslandWakeGraphRejectReason::NoWakeableIslands;
    }
    return IslandWakeGraphRejectReason::None;
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    }
    return "Unknown";
}

const char* island_solve_reject_reason_name(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "None";
    case IslandSolveRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveRejectReason::NoConstraints:
        return "NoConstraints";
    case IslandSolveRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    case IslandSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::NotAllSleeping:
        return "NotAllSleeping";
    }
    return "Unknown";
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::UniformSleepState:
        return "UniformSleepState";
    }
    return "Unknown";
}

IslandGraphBuildRejectReason classify_island_build_reject(const IslandBuildPreflight& preflight) {
    return preflight.reason;
}

IslandDispatchRejectReason classify_island_dispatch_reject(const IslandDispatchPreflight& preflight) {
    if (preflight.reason != IslandDispatchRejectReason::None) {
        return preflight.reason;
    }
    if (preflight.invalidDt) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (preflight.solve.skipped) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

IslandSolveRejectReason classify_island_solve_job_reject(const IslandSolveJobPreflight& preflight) {
    if (preflight.reason != IslandSolveRejectReason::None) {
        return preflight.reason;
    }
    if (preflight.invalidDt) {
        return IslandSolveRejectReason::InvalidDt;
    }
    if (preflight.skipped) {
        return preflight.constraintCount == 0u ? IslandSolveRejectReason::NoConstraints
                                             : IslandSolveRejectReason::EmptyIsland;
    }
    return IslandSolveRejectReason::None;
}

IslandSolveRejectReason classify_island_constraint_solve_reject(const IslandConstraintSolvePreflight& preflight) {
    if (preflight.reason != IslandSolveRejectReason::None) {
        return preflight.reason;
    }
    if (preflight.skipped) {
        return IslandSolveRejectReason::EmptyIsland;
    }
    if (!preflight.refs.can_solve()) {
        return IslandSolveRejectReason::StaleConstraintRefs;
    }
    if (!preflight.bodies.can_solve()) {
        return IslandSolveRejectReason::NoMovableBodies;
    }
    return IslandSolveRejectReason::None;
}

IslandSleepRejectReason classify_island_sleep_reject(const IslandSleepPreflight& preflight) {
    if (preflight.reason != IslandSleepRejectReason::None) {
        return preflight.reason;
    }
    if (preflight.skipped) {
        return IslandSleepRejectReason::EmptyIsland;
    }
    if (!preflight.can_skip_solve()) {
        return IslandSleepRejectReason::NotAllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandSleepRejectReason classify_island_sleep_graph_reject(const IslandSleepGraphPreflight& preflight) {
    if (preflight.reason != IslandSleepRejectReason::None) {
        return preflight.reason;
    }
    if (preflight.skipped) {
        return IslandSleepRejectReason::NotAllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandWakeRejectReason classify_island_wake_reject(const IslandWakePreflight& preflight) {
    if (preflight.reason != IslandWakeRejectReason::None) {
        return preflight.reason;
    }
    if (preflight.skipped) {
        return IslandWakeRejectReason::EmptyIsland;
    }
    if (!preflight.should_wake_sleepers()) {
        return IslandWakeRejectReason::UniformSleepState;
    }
    return IslandWakeRejectReason::None;
}

IslandWakeRejectReason classify_island_wake_graph_reject(const IslandWakeGraphPreflight& preflight) {
    if (preflight.reason != IslandWakeRejectReason::None) {
        return preflight.reason;
    }
    if (preflight.skipped) {
        return IslandWakeRejectReason::UniformSleepState;
    }
    return IslandWakeRejectReason::None;
}

bool preflight_island_build_ready(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandGraphBuildRejectReason* reason) {
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_build();
}

bool try_preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandGraphBuildRejectReason& reason) {
    return preflight_island_build_ready(bodyCount, contacts, distanceConstraints, &reason);
}

bool preflight_island_dispatch_ready(const ContactIslandGraph& graph,
                                     f32 dt,
                                     IslandDispatchRejectReason* reason) {
    const IslandDispatchPreflight preflight = preflight_island_dispatch(graph, dt);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_dispatch();
}

bool try_preflight_island_dispatch(const ContactIslandGraph& graph,
                                   f32 dt,
                                   IslandDispatchRejectReason& reason) {
    return preflight_island_dispatch_ready(graph, dt, &reason);
}

bool preflight_solve_island_job_ready(const IslandSolveJob& job,
                                      f32 dt,
                                      IslandSolveRejectReason* reason) {
    const IslandSolveJobPreflight preflight = preflight_solve_island_job(job, dt);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_dispatch();
}

bool try_preflight_solve_island_job(const IslandSolveJob& job,
                                    f32 dt,
                                    IslandSolveRejectReason& reason) {
    return preflight_solve_island_job_ready(job, dt, &reason);
}

bool preflight_island_sleep_ready(const ContactIslandGraph::Island& island,
                                  const RigidBodySoA& bodies,
                                  IslandSleepRejectReason* reason) {
    const IslandSleepPreflight preflight = preflight_island_sleep(island, bodies);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.can_skip_solve();
}

bool try_preflight_island_sleep(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies,
                                IslandSleepRejectReason& reason) {
    return preflight_island_sleep_ready(island, bodies, &reason);
}

bool preflight_island_wake_ready(const ContactIslandGraph::Island& island,
                                 const RigidBodySoA& bodies,
                                 IslandWakeRejectReason* reason) {
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies);
    if (reason != nullptr) {
        *reason = preflight.reason;
    }
    return preflight.should_wake_sleepers();
}

bool try_preflight_island_wake(const ContactIslandGraph::Island& island,
                               const RigidBodySoA& bodies,
                               IslandWakeRejectReason& reason) {
    return preflight_island_wake_ready(island, bodies, &reason);
}

const char* island_solve_reject_reason_name(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "None";
    case IslandSolveRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    }
    return "Unknown";
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    }
    return "Unknown";
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    }
    return "Unknown";
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    }
    return "Unknown";
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    }
    return "Unknown";
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::NoSolveableIslands:
        return "NoSolveableIslands";
    }
    return "Unknown";
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::EmptyGraph:
        return "EmptyGraph";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    }
    return "Unknown";
}

const char* island_solve_reject_reason_name(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "None";
    case IslandSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandSolveRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveRejectReason::OutOfRangeRefs:
        return "OutOfRangeRefs";
    case IslandSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandWakeRejectReason::NoWakeTarget:
        return "NoWakeTarget";
    }
    return "Unknown";
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandDispatchRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandDispatchRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandDispatchRejectReason::NullIslandJob:
        return "NullIslandJob";
    case IslandDispatchRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    default:
        return "Unknown";
    }
}

const char* island_solve_reject_reason_name(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "None";
    case IslandSolveRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    case IslandSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandSleepRejectReason::NoSolveableIslands:
        return "NoSolveableIslands";
    default:
        return "Unknown";
    }
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    default:
        return "Unknown";
    }
}

IslandDispatchRejectReason island_dispatch_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.f) {
        return std::isfinite(dt) ? IslandDispatchRejectReason::InvalidDt
                                 : IslandDispatchRejectReason::NonFiniteDt;
    }
    if (job.island == nullptr) {
        return IslandDispatchRejectReason::NullIslandJob;
    }
    if (job.empty) {
        return IslandDispatchRejectReason::EmptyIsland;
    }
    if (job.constraintCount == 0u) {
        return IslandDispatchRejectReason::ZeroConstraints;
    }
    return IslandDispatchRejectReason::None;
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.f) {
        return std::isfinite(dt) ? IslandDispatchRejectReason::InvalidDt
                                 : IslandDispatchRejectReason::NonFiniteDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

IslandSolveRejectReason island_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   const std::vector<DistanceConstraint>& distanceConstraints,
                                                   f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveRejectReason::InvalidDt;
    }
    if (!island_has_constraints(island)) {
        return IslandSolveRejectReason::EmptyIsland;
    }
    if (should_skip_island_constraint_refs(island, contacts, distanceConstraints)) {
        return IslandSolveRejectReason::NoInRangeRefs;
    }
    if (should_skip_island_solve_bodies(island, bodies)) {
        const IslandSleepPreflight sleepPreflight = preflight_island_sleep(island, bodies);
        if (sleepPreflight.can_skip_solve()) {
            return IslandSolveRejectReason::AllSleeping;
        }
        return IslandSolveRejectReason::NoMovableBodies;
    }
    return IslandSolveRejectReason::None;
}

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepRejectReason::EmptyIsland;
    }
    return IslandSleepRejectReason::None;
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (sleepingCount == 0u || activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoMixedSleepState;
    }
    return IslandWakeRejectReason::None;
}

bool island_dispatch_rejects_for_reason(const IslandSolveJob& job,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(job, dt) == expected;
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

bool island_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     f32 dt,
                                     IslandSolveRejectReason expected) {
    return island_solve_reject_reason(island, bodies, contacts, distanceConstraints, dt) == expected;
}

bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected) {
    return island_sleep_reject_reason(island, bodies) == expected;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandDispatchRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    default:
        return "Unknown";
    }
}

const char* islandSolveRejectReasonName(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "None";
    case IslandSolveRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveRejectReason::NoInRangeConstraints:
        return "NoInRangeConstraints";
    case IslandSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    case IslandSolveRejectReason::InvalidDt:
        return "InvalidDt";
    default:
        return "Unknown";
    }
}

const char* islandSleepRejectReasonName(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    case IslandSleepRejectReason::NoSolveableIslands:
        return "NoSolveableIslands";
    default:
        return "Unknown";
    }
}

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandWakeRejectReason::NoWakeTarget:
        return "NoWakeTarget";
    case IslandWakeRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    default:
        return "Unknown";
    }
}

bool island_index_valid(const ContactIslandGraph& graph, u32 islandIndex) {
    return islandIndex < graph.islandCount();
}

bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    }
    return (bodies.flags[bodyIndex] & RB_SLEEPING) != 0u;
}

bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    }
    const u32 flags = bodies.flags[bodyIndex];
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;
}

bool is_body_dynamic(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    }
    return !is_body_static_or_kinematic(bodies, bodyIndex);
}

IslandSolveJobPreflight preflight_solve_island_job(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    const RigidBodySoA& bodies,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandSolveJobPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.contactCount = static_cast<u32>(island.contactIndices.size());
    preflight.distanceCount = static_cast<u32>(island.distanceIndices.size());

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            ++preflight.outOfRangeContacts;
            continue;
        }
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        const f32 invMassA = invMassFn(bodies, contact.bodyA);
        const f32 invMassB = invMassFn(bodies, contact.bodyB);
        if (invMassA + invMassB < kImmovableInvMassEpsilon) {
            ++preflight.immovableConstraintPairs;
        } else {
            ++preflight.solvableConstraintPairs;
        }
    }

    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceConstraints.size()) {
            ++preflight.outOfRangeDistances;
            continue;
        }
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        const f32 invMassA = invMassFn(bodies, constraint.bodyA);
        const f32 invMassB = invMassFn(bodies, constraint.bodyB);
        if (invMassA + invMassB < kImmovableInvMassEpsilon) {
            ++preflight.immovableConstraintPairs;
        } else {
            ++preflight.solvableConstraintPairs;
        }
    }

    return preflight;
}

IslandSolveJobPreflight preflight_solve_island_job_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    const RigidBodySoA& bodies,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandSolveJobPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_solve_island_job(graph.island(islandIndex),
                                      contacts,
                                      distanceConstraints,
                                      bodies,
                                      invMassFn);
}

bool should_skip_solve_island_all_sleeping(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies) {
    const IslandSleepPreflight sleepPreflight = preflight_island_sleep_state(island, bodies);
    return !sleepPreflight.skipped && sleepPreflight.all_dynamic_sleeping();
}

bool should_skip_solve_island_all_static(const ContactIslandGraph::Island& island,
                                         const RigidBodySoA& bodies) {
    const IslandSleepPreflight sleepPreflight = preflight_island_sleep_state(island, bodies);
    return !sleepPreflight.skipped && sleepPreflight.all_static();
}

bool should_skip_solve_island_job_preflight(const IslandSolveJobPreflight& preflight) {
    return preflight.skipped || !preflight.can_solve();
}

IslandSleepPreflight preflight_island_sleep_state(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
    if (island.bodyIndices.empty()) {
        preflight.skipped = true;
        return preflight;
    }

    for (u32 bodyIndex : island.bodyIndices) {
        if (is_body_static_or_kinematic(bodies, bodyIndex)) {
            ++preflight.staticBodyCount;
            continue;
        }

        ++preflight.dynamicBodyCount;
        if (is_body_sleeping(bodies, bodyIndex)) {
            ++preflight.sleepingBodyCount;
        }
        if (bodyHasPendingForces(bodies, bodyIndex)) {
            ++preflight.bodiesWithForces;
        }
        if ((bodies.flags[bodyIndex] & RB_CCD) != 0u) {
            ++preflight.bodiesWithCcd;
        }
    }

    return preflight;
}

IslandSleepPreflight preflight_island_sleep_state_by_index(const ContactIslandGraph& graph,
                                                           u32 islandIndex,
                                                           const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_sleep_state(graph.island(islandIndex), bodies);
}

bool should_skip_sleep_detection_for_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (!is_body_dynamic(bodies, bodyIndex)) {
        return true;
    }
    if (bodyHasPendingForces(bodies, bodyIndex)) {
        return true;
    }
    return (bodies.flags[bodyIndex] & RB_CCD) != 0u;
}

bool should_wake_island_on_contact(const ContactIslandGraph::Island& island,
                                   const RigidBodySoA& bodies,
                                   const std::vector<narrowphase::ContactManifold>& contacts) {
    return preflight_island_sleep_wake(island, bodies, contacts).should_wake;
}

IslandSleepWakePreflight preflight_island_sleep_wake(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts) {
    IslandSleepWakePreflight preflight{};
    preflight.sleep = preflight_island_sleep_state(island, bodies);
    if (preflight.sleep.skipped) {
        preflight.skipped = true;
        return preflight;
    }

    if (island.isEmpty()) {
        return preflight;
    }

    bool hasAwakeDynamic = false;
    for (u32 bodyIndex : island.bodyIndices) {
        if (is_body_dynamic(bodies, bodyIndex) && !is_body_sleeping(bodies, bodyIndex)) {
            hasAwakeDynamic = true;
            break;
        }
    }

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid) {
            continue;
        }

        ++preflight.activeContactCount;
        const bool sleepingA = is_body_sleeping(bodies, contact.bodyA);
        const bool sleepingB = is_body_sleeping(bodies, contact.bodyB);
        if (sleepingA != sleepingB) {
            ++preflight.contactsTouchingSleepingBody;
        }
    }

    preflight.should_wake =
        preflight.contactsTouchingSleepingBody > 0u ||
        (preflight.sleep.sleepingBodyCount > 0u && hasAwakeDynamic && preflight.activeContactCount > 0u);
    return preflight;
}

IslandSleepWakePreflight preflight_island_sleep_wake_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts) {
    IslandSleepWakePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_sleep_wake(graph.island(islandIndex), bodies, contacts);
}

bool island_has_constraints(const ContactIslandGraph::Island& island) {
    return !island.isEmpty();
}

bool is_valid_island_build_body_index(u32 bodyIndex, u32 bodyCount) {
    return bodyIndexInRange(bodyIndex, bodyCount);
}

bool is_valid_island_build_body_count(u32 bodyCount) {
    return bodyCount > 0u;
}

bool is_rigid_body_sleeping(u32 bodyFlags) {
    return (bodyFlags & RB_SLEEPING) != 0u;
}

bool is_rigid_body_static_or_kinematic(u32 bodyFlags) {
    return (bodyFlags & RB_STATIC) != 0u || (bodyFlags & RB_KINEMATIC) != 0u;
}

bool is_rigid_body_dynamic_awake(u32 bodyFlags) {
    return !is_rigid_body_static_or_kinematic(bodyFlags) && !is_rigid_body_sleeping(bodyFlags);
}

bool is_island_all_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return false;
    }

    bool hasDynamicBody = false;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (is_rigid_body_static_or_kinematic(bodies.flags[bodyIndex])) {
            continue;
        }
        hasDynamicBody = true;
        if (!is_rigid_body_sleeping(bodies.flags[bodyIndex])) {
            return false;
        }
    }
    return hasDynamicBody;
}

bool island_has_wake_candidates(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies,
                                const std::vector<narrowphase::ContactManifold>& contacts) {
    if (!island_has_constraints(island)) {
        return false;
    }

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid) {
            continue;
        }
        if (contact.bodyA >= bodies.count() || contact.bodyB >= bodies.count()) {
            continue;
        }

        const bool sleepingA = is_rigid_body_sleeping(bodies.flags[contact.bodyA]);
        const bool sleepingB = is_rigid_body_sleeping(bodies.flags[contact.bodyB]);
        const bool awakeA = is_rigid_body_dynamic_awake(bodies.flags[contact.bodyA]);
        const bool awakeB = is_rigid_body_dynamic_awake(bodies.flags[contact.bodyB]);
        if ((sleepingA && awakeB) || (sleepingB && awakeA)) {
            return true;
        }
    }
    return false;
}

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    if (!is_valid_island_build_body_count(bodyCount)) {
        preflight.skipped = true;
        return preflight;
    }

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        if (contactReferencesValidBodies(contact, bodyCount)) {
            ++preflight.validContactCount;
        } else {
            ++preflight.invalidContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (distanceConstraintReferencesValidBodies(constraint, bodyCount)) {
            ++preflight.validDistanceCount;
        } else {
            ++preflight.invalidDistanceCount;
        }
    }

    return preflight;
}

bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (is_rigid_body_static_or_kinematic(bodies.flags[bodyIndex])) {
            continue;
        }
        ++preflight.dynamicBodyCount;
        if (is_rigid_body_sleeping(bodies.flags[bodyIndex])) {
            ++preflight.sleepingBodyCount;
        } else {
            ++preflight.awakeBodyCount;
        }
    }

    preflight.allSleeping =
        preflight.dynamicBodyCount > 0u && preflight.sleepingBodyCount == preflight.dynamicBodyCount;
    return preflight;
}

IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
                                                     u32 islandIndex,
                                                     const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_sleep(graph.island(islandIndex), bodies);
}

IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    IslandSleepStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island_has_constraints(island)) {
            ++stats.emptyCount;
            continue;
        }
        const IslandSleepPreflight preflight = preflight_island_sleep(island, bodies);
        if (preflight.allSleeping) {
            ++stats.allSleepingCount;
        } else if (preflight.dynamicBodyCount > 0u) {
            ++stats.partiallyAwakeCount;
        }
    }
    return stats;
}

u32 count_all_sleeping_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return compute_island_sleep_stats(graph, bodies).allSleepingCount;
}

bool has_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    const IslandSleepStats stats = compute_island_sleep_stats(graph, bodies);
    return stats.partiallyAwakeCount > 0u;
}

IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies) {
    IslandSleepGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_stats(graph, bodies);
    preflight.skipped = preflight.stats.totalIslands == 0u;
    return preflight;
}

bool should_skip_island_solve_all_sleeping(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    const IslandSleepGraphPreflight preflight = preflight_island_sleep_graph(graph, bodies);
    if (preflight.skipped) {
        return true;
    }
    return !preflight.has_awake_islands();
}

bool should_skip_sleeping_island(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_sleep(island, bodies).can_solve();
}

bool should_skip_sleeping_island_index(const ContactIslandGraph& graph,
                                       u32 islandIndex,
                                       const RigidBodySoA& bodies) {
    const IslandSleepPreflight preflight = preflight_island_sleep_by_index(graph, islandIndex, bodies);
    return preflight.skipped || !preflight.can_solve();
}

std::vector<u32> collect_island_wake_body_indices(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies,
                                                  const std::vector<narrowphase::ContactManifold>& contacts) {
    std::vector<u32> wakeBodies;
    if (!island_has_constraints(island)) {
        return wakeBodies;
    }

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid) {
            continue;
        }
        if (contact.bodyA >= bodies.count() || contact.bodyB >= bodies.count()) {
            continue;
        }

        const u32 candidates[2] = {contact.bodyA, contact.bodyB};
        for (u32 bodyIndex : candidates) {
            if (!is_rigid_body_sleeping(bodies.flags[bodyIndex])) {
                continue;
            }
            const u32 neighbor = bodyIndex == contact.bodyA ? contact.bodyB : contact.bodyA;
            if (!is_rigid_body_dynamic_awake(bodies.flags[neighbor])) {
                continue;
            }
            if (std::find(wakeBodies.begin(), wakeBodies.end(), bodyIndex) == wakeBodies.end()) {
                wakeBodies.push_back(bodyIndex);
            }
        }
    }
    return wakeBodies;
}

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts) {
    IslandWakePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (is_rigid_body_sleeping(bodies.flags[bodyIndex])) {
            ++preflight.sleepingBodyCount;
        } else if (is_rigid_body_dynamic_awake(bodies.flags[bodyIndex])) {
            ++preflight.awakeNeighborCount;
        }
    }

    const std::vector<u32> wakeBodies = collect_island_wake_body_indices(island, bodies, contacts);
    preflight.wakeCandidateCount = static_cast<u32>(wakeBodies.size());
    preflight.needsWake = preflight.wakeCandidateCount > 0u;
    return preflight;
}

IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   const RigidBodySoA& bodies,
                                                   const std::vector<narrowphase::ContactManifold>& contacts) {
    IslandWakePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_wake(graph.island(islandIndex), bodies, contacts);
}

IslandWakeStats compute_island_wake_stats(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts) {
    IslandWakeStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island_has_constraints(island)) {
            ++stats.emptyCount;
            continue;
        }
        const IslandWakePreflight preflight = preflight_island_wake(island, bodies, contacts);
        if (preflight.needsWake) {
            ++stats.wakeCandidateCount;
        } else {
            ++stats.noWakeCount;
        }
    }
    return stats;
}

u32 count_wake_candidate_islands(const ContactIslandGraph& graph,
                                 const RigidBodySoA& bodies,
                                 const std::vector<narrowphase::ContactManifold>& contacts) {
    return compute_island_wake_stats(graph, bodies, contacts).wakeCandidateCount;
}

bool has_wake_candidate_islands(const ContactIslandGraph& graph,
                                const RigidBodySoA& bodies,
                                const std::vector<narrowphase::ContactManifold>& contacts) {
    return count_wake_candidate_islands(graph, bodies, contacts) > 0u;
}

IslandWakeGraphPreflight preflight_island_wake_graph(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies,
                                                     const std::vector<narrowphase::ContactManifold>& contacts) {
    IslandWakeGraphPreflight preflight{};
    preflight.stats = compute_island_wake_stats(graph, bodies, contacts);
    preflight.skipped = preflight.stats.totalIslands == 0u;
    return preflight;
}

bool should_skip_island_wake_graph(const ContactIslandGraph& graph,
                                   const RigidBodySoA& bodies,
                                   const std::vector<narrowphase::ContactManifold>& contacts) {
    return !preflight_island_wake_graph(graph, bodies, contacts).has_wake_candidates();
}

u32 island_constraint_count(const ContactIslandGraph::Island& island) {
    return static_cast<u32>(island.contactIndices.size() + island.distanceIndices.size());
}

bool should_solve_island(const IslandSolveJob& job) {
    return !job.empty && job.island != nullptr && job.constraintCount > 0u;
}

bool should_skip_island_solve_job(const IslandSolveJob& job) {
    return !should_solve_island(job);
}

bool should_dispatch_island_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_solve_island(extract_island(graph, islandIndex));

bool should_skip_island_solve(const IslandSolveJob& job) {

IslandSolveJob extract_island(const ContactIslandGraph& graph, u32 islandIndex) {
    IslandSolveJob job{};
    if (islandIndex >= graph.islandCount()) {
        return job;
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    job.islandIndex = islandIndex;
    job.island = &island;
    job.constraintCount = island_constraint_count(island);
    job.empty = job.constraintCount == 0u;
    return job;
}

bool is_dispatchable_island_index(const ContactIslandGraph& graph, u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return false;
    }
    return should_solve_island(extract_island(graph, islandIndex));
}

std::vector<IslandSolveJob> extract_island_jobs(const ContactIslandGraph& graph) {
    const u32 count = graph.islandCount();
    std::vector<IslandSolveJob> jobs;
    jobs.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        jobs.push_back(extract_island(graph, islandIndex));
    }
    return jobs;
}

std::vector<IslandSolveJob> filter_dispatchable_jobs(const std::vector<IslandSolveJob>& jobs) {
    std::vector<IslandSolveJob> dispatchable;
    dispatchable.reserve(jobs.size());
    for (const IslandSolveJob& job : jobs) {
        if (should_solve_island(job)) {
            dispatchable.push_back(job);
        }
    }
    return dispatchable;
}

IslandSolveStats compute_island_solve_stats(const ContactIslandGraph& graph) {
    IslandSolveStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (job.empty) {
            ++stats.emptyCount;
        } else {
            ++stats.constrainedCount;
        }
        if (should_solve_island(job)) {
            ++stats.dispatchableCount;
        }
    }
    return stats;
}

u32 count_dispatchable_islands(const ContactIslandGraph& graph) {
    return compute_island_solve_stats(graph).dispatchableCount;
}

bool has_dispatchable_islands(const ContactIslandGraph& graph) {
    return count_dispatchable_islands(graph) > 0u;
}

bool all_islands_empty(const ContactIslandGraph& graph) {
    const IslandSolveStats stats = compute_island_solve_stats(graph);
    return stats.totalIslands == 0u || stats.emptyCount == stats.totalIslands;
}

u32 empty_island_count(const ContactIslandGraph& graph) {
    return compute_island_solve_stats(graph).emptyCount;
}

bool graph_has_empty_islands(const ContactIslandGraph& graph) {
    return empty_island_count(graph) > 0u;

bool graph_has_constrained_islands(const ContactIslandGraph& graph) {
    return graph.constrainedIslandCount() > 0u;
const char* islandSolveRejectReasonName(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "None";
    case IslandSolveRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    return "Unknown";

IslandSolveRejectReason islandSolveRejectReason(const ContactIslandGraph& graph) {
    if (has_dispatchable_islands(graph)) {
        return IslandSolveRejectReason::None;
    return IslandSolveRejectReason::NoDispatchableIslands;

bool islandSolveRejectsForReason(const ContactIslandGraph& graph, IslandSolveRejectReason expected) {
    return islandSolveRejectReason(graph) == expected;
}

bool is_valid_island_solve_dt(f32 dt) {
    return dt > 0.f;

bool is_valid_warm_start_dt(f32 dt) {
    return is_valid_island_solve_dt(dt);

bool is_finite_island_solve_dt(f32 dt) {
    return is_valid_island_solve_dt(dt) && std::isfinite(dt);

bool is_finite_warm_start_dt(f32 dt) {
    return is_finite_island_solve_dt(dt);

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    }
    return "Unknown";
}

IslandDispatchRejectReason islandDispatchRejectReason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool islandDispatchRejectsForReason(const ContactIslandGraph& graph,
                                    f32 dt,
                                    IslandDispatchRejectReason expected) {
    return islandDispatchRejectReason(graph, dt) == expected;
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

IslandDispatchRejectReason island_dispatch_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (job.island == nullptr) {
        return IslandDispatchRejectReason::OutOfRangeIsland;
    }
    if (job.empty || job.constraintCount == 0u) {
        return IslandDispatchRejectReason::EmptyIsland;
    }
    return IslandDispatchRejectReason::None;
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

IslandSolveRejectReason island_solve_reject_reason(const ContactIslandGraph& graph) {
    if (!has_dispatchable_islands(graph)) {
        return IslandSolveRejectReason::NoDispatchableIslands;
    }
    return IslandSolveRejectReason::None;
}

IslandSolveRejectReason island_constraint_refs_reject_reason(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandSolveRejectReason::EmptyIsland;
    }

    u32 inRangeContactCount = 0u;
    u32 inRangeDistanceCount = 0u;
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex < contacts.size()) {
            ++inRangeContactCount;
        }
    }
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex < distanceConstraints.size()) {
            ++inRangeDistanceCount;
        }
    }
    if (inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return IslandSolveRejectReason::NoInRangeConstraints;
    }
    return IslandSolveRejectReason::None;
}

IslandSolveJobPreflight preflight_solve_island_job(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobPreflight preflight{};
    preflight.reason = diagnose_island_solve_job_reject(job, dt);
    preflight.invalidDt = !is_finite_island_solve_dt(dt);
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.nonFiniteDt = is_valid_island_solve_dt(dt) && !std::isfinite(dt);
    preflight.reason = island_job_dispatch_reject_reason(job, dt);
    preflight.invalidDt = preflight.reason == IslandJobDispatchRejectReason::InvalidDt;
    preflight.reason = island_dispatch_reject_reason(job, dt);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = preflight.reason != IslandJobDispatchRejectReason::None;
    preflight.reason = island_solve_job_reject_reason(job, dt);
    preflight.invalidDt = preflight.reason == IslandSolveJobRejectReason::InvalidDt;
    preflight.skipped = preflight.reason == IslandSolveJobRejectReason::EmptyJob;
    preflight.reason = islandSolveJobRejectReason(job, dt);
    preflight.invalidDt = preflight.reason == IslandSolveRejectReason::InvalidDt;
    preflight.skipped = preflight.reason != IslandSolveRejectReason::None;
    preflight.skipped = should_skip_island_solve_job(job);
    if (!std::isfinite(dt)) {
        preflight.reason = IslandSolveJobRejectReason::NonFiniteDt;
        preflight.invalidDt = true;
    } else if (!is_valid_island_solve_dt(dt)) {
        preflight.reason = IslandSolveJobRejectReason::InvalidDt;
    } else if (preflight.skipped) {
        preflight.reason = IslandSolveJobRejectReason::EmptyJob;
    }
    preflight.reason = classifyIslandSolveJobReject(preflight);
    preflight.invalidDt = preflight.reason == IslandSolveJobRejectReason::InvalidDt ||
                          preflight.reason == IslandSolveJobRejectReason::NonFiniteDt;
    preflight.skipped = preflight.reason != IslandSolveJobRejectReason::None;
    preflight.reason = classify_solve_dt_reject(dt);
    preflight.invalidDt = preflight.reason != IslandSolveRejectReason::None;
    if (preflight.reason == IslandSolveRejectReason::None && should_skip_island_solve_job(job)) {
        preflight.reason = IslandSolveRejectReason::EmptyIsland;
        preflight.skipped = true;
    preflight.reason = islandSolveJobRejectReason(preflight);
    if (preflight.invalidDt) {
        preflight.reason = IslandSolveRejectReason::InvalidDt;
        preflight.reason = job.constraintCount == 0u ? IslandSolveRejectReason::NoConstraints
                                                     : IslandSolveRejectReason::EmptyIsland;
        return preflight;
    if (should_skip_island_solve_job(job)) {
        preflight.reason = IslandSolveJobRejectReason::EmptyIsland;
    preflight.reason = island_dispatch_job_reject_reason(job, dt);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt;
    preflight.skipped = preflight.reason != IslandDispatchRejectReason::None;
    return preflight;

bool should_skip_solve_island_job(const IslandSolveJob& job, f32 dt) {
    return !preflight_solve_island_job(job, dt).can_dispatch();

IslandDispatchRejectReason islandDispatchRejectReason(const IslandSolveJob& job, f32 dt) {
    return classifyIslandDispatchRejectReason(preflight_solve_island_job(job, dt), dt);
}

IslandConstraintRefsPreflight preflight_island_constraint_refs(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_constraint_refs(island, contacts, distanceConstraints, 0u);
}

IslandConstraintRefsPreflight preflight_island_constraint_refs(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    u32 bodyCount) {
    IslandConstraintRefsPreflight preflight{};
    preflight.reason = island_constraint_refs_reject_reason(island, contacts, distanceConstraints);
    if (!island_has_constraints(island)) {
        preflight.reason = IslandSolveRejectReason::EmptyIsland;
        preflight.skipped = true;
    preflight.reason = island_constraint_refs_reject_reason(island, contacts, distanceConstraints);
    preflight.skipped = preflight.reason == IslandConstraintRefsRejectReason::EmptyIsland;

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            ++preflight.outOfRangeContactCount;
            continue;
        ++preflight.inRangeContactCount;
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (contact.valid) {
            ++preflight.validContactCount;

    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex < distanceConstraints.size()) {
            ++preflight.inRangeDistanceCount;
        } else {
            ++preflight.outOfRangeDistanceCount;
        }
        if (bodyCount > 0u) {
            if (contact.bodyA < bodyCount && contact.bodyB < bodyCount) {
                ++preflight.inRangeContactBodyCount;
            } else {
                ++preflight.outOfRangeContactBodyCount;

        if (distanceIndex >= distanceConstraints.size()) {
            continue;
            const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
            if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
                ++preflight.inRangeDistanceBodyCount;
                ++preflight.outOfRangeDistanceBodyCount;

    if (preflight.inRangeContactCount == 0u && preflight.inRangeDistanceCount == 0u) {
        preflight.reason = IslandSolveRejectReason::NoInRangeConstraints;
    }
        preflight.reason = IslandSolveRejectReason::OutOfRangeRefs;

    return preflight;

bool should_skip_island_constraint_refs(const ContactIslandGraph::Island& island,
    return !preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve();

bool is_valid_warm_start_dt(f32 dt) {
    return dt > 0.f;
}

bool is_body_sleeping(u32 flags) {
    return (flags & RB_SLEEPING) != 0u;
}

bool is_body_static_or_kinematic(u32 flags) {
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;

IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island) {
    IslandSleepStats stats{};
    stats.totalBodies = static_cast<u32>(island.bodyIndices.size());

bool is_body_dynamic_awake(const RigidBodySoA& bodies, u32 bodyIndex) {
bool is_sleeping_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    return (bodies.flags[bodyIndex] & RB_SLEEPING) != 0u;

bool is_dynamic_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    const u32 flags = bodies.flags[bodyIndex];
    return !is_body_static_or_kinematic(flags) && !is_body_sleeping(flags);

bool island_all_dynamic_bodies_sleeping(const RigidBodySoA& bodies,
    u32 dynamicCount = 0;
    return (flags & RB_STATIC) == 0u && (flags & RB_KINEMATIC) == 0u;

bool is_awake_dynamic_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    return is_dynamic_body(bodies, bodyIndex) && !is_sleeping_body(bodies, bodyIndex);

bool island_all_bodies_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    if (island.bodyIndices.empty()) {
    for (u32 bodyIndex : island.bodyIndices) {
        if (!is_sleeping_body(bodies, bodyIndex)) {
    return true;

bool island_has_awake_dynamic_body(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
        if (is_awake_dynamic_body(bodies, bodyIndex)) {

IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
        preflight.skipped = true;
        return preflight;
bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex) {

bool is_body_awake(const RigidBodySoA& bodies, u32 bodyIndex) {
    return (bodies.flags[bodyIndex] & RB_SLEEPING) == 0u;

bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex) {

u32 count_sleeping_bodies_in_island(const ContactIslandGraph::Island& island,
    u32 count = 0;
        if (is_body_sleeping(bodies, bodyIndex)) {
            ++count;
    return count;

bool island_all_bodies_sleeping(const ContactIslandGraph::Island& island,
            continue;
        if (is_body_static_or_kinematic(bodies, bodyIndex)) {
        if (!is_body_sleeping(bodies, bodyIndex)) {

IslandSleepPreflight preflight_island_sleep_for_solve(const ContactIslandGraph::Island& island,
    if (!island_has_constraints(island)) {

    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
        if (is_body_static_or_kinematic(flags)) {
            ++stats.staticCount;
        if (is_body_sleeping(flags)) {
            ++stats.sleepingCount;
        } else {
            ++stats.activeCount;
    return stats;

bool is_island_fully_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {

    const IslandSleepStats stats = compute_island_sleep_stats(bodies, island);
    return stats.activeCount == 0u && stats.sleepingCount > 0u;

IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
        if (is_body_static_or_kinematic(bodies.flags[bodyIndex])) {
        ++dynamicCount;
        if (!is_body_sleeping(bodies.flags[bodyIndex])) {
    return dynamicCount > 0u;

bool island_has_awake_dynamic_bodies(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
        if (is_body_dynamic_awake(bodies, bodyIndex)) {

u32 count_island_awake_dynamic_bodies(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {

IslandConstraintSolvePreflight preflight_island_constraint_solve(const RigidBodySoA& bodies,
                                                                 const ContactIslandGraph::Island& island,
                                                                 f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
        if ((flags & RB_STATIC) != 0u) {
            ++preflight.staticCount;
        } else if ((flags & RB_KINEMATIC) != 0u) {
            ++preflight.kinematicCount;
        } else if ((flags & RB_SLEEPING) != 0u) {
            ++preflight.sleepingCount;
            ++preflight.awakeDynamicCount;

IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
                                                     u32 islandIndex,
    if (!island_index_valid(graph, islandIndex)) {
    return preflight_island_sleep(graph.island(islandIndex), bodies);

IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
            ++stats.emptyCount;
        const IslandSleepPreflight preflight = preflight_island_sleep(island, bodies);
        if (preflight.skipped) {
        } else if (preflight.can_solve()) {
            ++stats.solvableCount;
        } else if (preflight.all_sleeping()) {
            ++stats.allSleepingCount;

u32 count_solvable_sleep_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return compute_island_sleep_stats(graph, bodies).solvableCount;

bool has_solvable_sleep_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_solvable_sleep_islands(graph, bodies) > 0u;

bool should_skip_island_solve_sleeping(const ContactIslandGraph::Island& island,
    return !preflight.can_solve();

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts) {
            ++preflight.outOfRangeCount;
            ++preflight.awakeCount;

    preflight.allSleeping = preflight.awakeCount == 0u;

IslandSleepPreflight preflight_island_sleep_for_solve_by_index(const ContactIslandGraph& graph,
    return preflight_island_sleep_for_solve(graph.island(islandIndex), bodies);

IslandSleepGraphStats compute_island_sleep_graph_stats(const ContactIslandGraph& graph,
    IslandSleepGraphStats stats{};
        const IslandSleepPreflight preflight = preflight_island_sleep_for_solve(island, bodies);
        if (preflight.allSleeping) {
            ++stats.solveableCount;

IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
    IslandSleepGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_graph_stats(graph, bodies);
    preflight.skipped = preflight.stats.solveableCount == 0u;

bool should_skip_island_solve_for_sleep(const ContactIslandGraph& graph,
    return !preflight_island_sleep_graph(graph, bodies).can_dispatch();

bool should_skip_solve_sleeping_island(const ContactIslandGraph::Island& island,
    return !preflight_island_sleep_for_solve(island, bodies).can_solve();

                                          f32 linearSleepThreshold,
                                          f32 angularSleepThreshold) {
bool body_index_in_range(const RigidBodySoA& bodies, u32 bodyIndex) {
    return bodyIndex < bodies.count();

bool is_sleeping_body(u32 bodyFlags) {
    return (bodyFlags & RB_SLEEPING) != 0u;

bool is_static_or_kinematic_body(u32 bodyFlags) {
    return (bodyFlags & RB_STATIC) != 0u || (bodyFlags & RB_KINEMATIC) != 0u;

bool is_active_dynamic_body(u32 bodyFlags) {
    return !is_static_or_kinematic_body(bodyFlags) && !is_sleeping_body(bodyFlags);

IslandBodyRefsPreflight preflight_island_body_refs(const ContactIslandGraph::Island& island,
    IslandBodyRefsPreflight preflight{};

    preflight.ownedBodyCount = static_cast<u32>(island.bodyIndices.size());
        if (body_index_in_range(bodies, bodyIndex)) {
            ++preflight.inRangeBodyCount;
            ++preflight.outOfRangeBodyCount;

bool should_skip_island_body_refs(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_body_refs(island, bodies).can_solve();

IslandSleepPreflight preflight_island_sleep_state(const ContactIslandGraph::Island& island,

    u32 dynamicBodyCount = 0;
        if (!body_index_in_range(bodies, bodyIndex)) {
        if (is_static_or_kinematic_body(flags)) {
            ++preflight.staticOrKinematicCount;
        ++dynamicBodyCount;
        if (is_sleeping_body(flags)) {
            ++preflight.activeCount;

    preflight.allSleeping = dynamicBodyCount > 0u && preflight.activeCount == 0u;

bool is_island_all_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep_state(island, bodies).allSleeping;

bool is_island_wake_candidate(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    const IslandSleepPreflight preflight = preflight_island_sleep_state(island, bodies);
    return !preflight.skipped && preflight.activeCount > 0u;

bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_state(island, bodies).can_solve();

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    IslandWakePreflight preflight{};
bool island_body_is_sleeping(const RigidBodySoA& bodies, u32 bodyIndex) {

bool island_body_is_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex) {

bool island_body_is_awake_dynamic(const RigidBodySoA& bodies, u32 bodyIndex) {
    return !island_body_is_static_or_kinematic(bodies, bodyIndex) && !island_body_is_sleeping(bodies, bodyIndex);

IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
bool is_island_body_index_valid(u32 bodyIndex, u32 bodyCount) {
    return bodyIndex < bodyCount;

bool is_island_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex) {

bool is_island_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex) {

IslandSolveBodyRefsPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
    IslandSolveBodyRefsPreflight preflight{};

        if (!is_island_body_index_valid(bodyIndex, bodies.count())) {
        if (is_island_body_sleeping(bodies, bodyIndex)) {
            ++preflight.sleepingBodyCount;
        if (is_island_body_static_or_kinematic(bodies, bodyIndex)) {
            ++preflight.staticBodyCount;

bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_bodies(island, bodies).can_solve();

    IslandSleepWakePreflight preflight{};
namespace {

bool is_static_or_kinematic(u32 flags) {

} // namespace


bool is_body_solve_immobile(const RigidBodySoA& bodies, u32 bodyIndex) {
    return is_static_or_kinematic(flags) || (flags & RB_SLEEPING) != 0u;

IslandSolveBodiesPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
    IslandSolveBodiesPreflight preflight{};

bool is_static_or_kinematic_body(const RigidBodySoA& bodies, u32 bodyIndex) {

    if (is_static_or_kinematic_body(bodies, bodyIndex)) {
    return !is_sleeping_body(bodies, bodyIndex);



    if (!island_has_constraints(island) && island.bodyIndices.empty()) {

        ++preflight.bodyCount;
        } else if (is_body_sleeping(bodies, bodyIndex)) {

    if (preflight.bodyCount == 0u) {

    return preflight_island_sleep(bodies, graph.island(islandIndex));

    return preflight_island_sleep(bodies, island).is_fully_sleeping();

bool should_skip_solve_fully_sleeping_island(const RigidBodySoA& bodies,
    return !preflight_island_sleep(bodies, island).can_solve();


    return bodyIndex < bodies.count() && !is_body_static_or_kinematic(bodies, bodyIndex) &&
           !is_body_sleeping(bodies, bodyIndex);

bool is_sleeping_dynamic_body(const RigidBodySoA& bodies, u32 bodyIndex) {
           is_body_sleeping(bodies, bodyIndex);


IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
    if (should_skip_warm_start_island(island)) {

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size() || !contacts[contactIndex].valid) {
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        const bool awakeA = is_awake_dynamic_body(bodies, contact.bodyA);
        const bool awakeB = is_awake_dynamic_body(bodies, contact.bodyB);
        const bool sleepingA = is_sleeping_dynamic_body(bodies, contact.bodyA);
        const bool sleepingB = is_sleeping_dynamic_body(bodies, contact.bodyB);

        if (awakeA) {
            ++preflight.awakeParticipantCount;
        if (awakeB) {
        if ((awakeA && sleepingB) || (awakeB && sleepingA)) {
            ++preflight.wakeCandidateCount;

IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
    return preflight_island_wake(bodies, graph.island(islandIndex), contacts);

bool should_wake_island_bodies(const RigidBodySoA& bodies,
    return preflight_island_wake(bodies, island, contacts).can_wake();

u32 wake_island_bodies_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    u32 wokenCount = 0;
        if (is_body_static_or_kinematic(bodies, bodyIndex) || !is_body_sleeping(bodies, bodyIndex)) {
        bodies.flags[bodyIndex] &= ~RB_SLEEPING;
        bodies.sleepTimers[bodyIndex] = 0.f;
        ++wokenCount;
    return wokenCount;

IslandSolveSleepPreflight preflight_solve_island_with_sleep(
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandSolveSleepPreflight preflight{};


bool is_solver_inactive_body(u32 bodyFlags) {
    return is_static_or_kinematic_body(bodyFlags) || is_sleeping_body(bodyFlags);

        if (bodyIndex >= bodies.flags.size() || !is_sleeping_body(bodies.flags[bodyIndex])) {


        if (bodyIndex >= bodies.flags.size()) {
        const u32 bodyFlags = bodies.flags[bodyIndex];
        if (is_static_or_kinematic_body(bodyFlags)) {
        if (is_sleeping_body(bodyFlags)) {
        ++preflight.wakeableCount;

bool is_island_wakeable(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep_wake(island, bodies).has_wakeable_body();

IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
    return preflight_island_sleep_wake(graph.island(islandIndex), bodies);

bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island,
    return !preflight_island_sleep_wake(island, bodies).can_solve();

bool should_skip_sleeping_island_solve_index(const ContactIslandGraph& graph,
    return should_skip_sleeping_island_solve(graph.island(islandIndex), bodies);

IslandSleepWakeStats compute_island_sleep_wake_stats(const ContactIslandGraph& graph,
    IslandSleepWakeStats stats{};
        const IslandSleepWakePreflight preflight = preflight_island_sleep_wake(island, bodies);
        if (preflight.skipped || island.isEmpty()) {
        } else if (preflight.is_all_sleeping()) {
        } else if (preflight.has_wakeable_body()) {
            ++stats.wakeableCount;
            ++stats.inactiveCount;

u32 count_wakeable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return compute_island_sleep_wake_stats(graph, bodies).wakeableCount;

bool has_wakeable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_wakeable_islands(graph, bodies) > 0u;

IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,
    IslandSleepWakeGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_wake_stats(graph, bodies);
    preflight.skipped = preflight.stats.wakeableCount == 0u;

bool should_skip_sleeping_island_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !has_wakeable_islands(graph, bodies);

std::vector<u32> collect_wakeable_island_indices(const ContactIslandGraph& graph,

bool is_inactive_solver_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u || (flags & RB_SLEEPING) != 0u;

bool is_fully_inactive_constraint_pair(const RigidBodySoA& bodies, u32 bodyA, u32 bodyB) {
    return is_inactive_solver_body(bodies, bodyA) && is_inactive_solver_body(bodies, bodyB);


bool is_island_all_inactive(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
        if (!is_inactive_solver_body(bodies, bodyIndex)) {


        if (is_sleeping_body(bodies, bodyIndex)) {
        if (is_inactive_solver_body(bodies, bodyIndex)) {
            ++preflight.inactiveCount;
            ++preflight.activeDynamicCount;

    preflight.allSleeping = preflight.ownedBodyCount > 0u &&
                            preflight.sleepingCount + preflight.outOfRangeBodyCount == preflight.ownedBodyCount;
    preflight.allInactive = preflight.ownedBodyCount > 0u &&
                            preflight.inactiveCount + preflight.outOfRangeBodyCount == preflight.ownedBodyCount;


        const IslandSleepWakePreflight preflight = preflight_island_sleep_wake(graph.island(islandIndex), bodies);
        if (!island_has_constraints(graph.island(islandIndex))) {
        } else if (preflight.allSleeping) {
        } else if (preflight.allInactive) {
            ++stats.allInactiveCount;

u32 count_active_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return compute_island_sleep_wake_stats(graph, bodies).activeCount;

bool has_active_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_active_islands(graph, bodies) > 0u;

    preflight.skipped = preflight.stats.activeCount == 0u;

bool should_skip_island_sleep_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !has_active_islands(graph, bodies);

std::vector<u32> collect_active_island_indices(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandSleepWakePreflight preflight =
            preflight_island_sleep_wake(graph.island(islandIndex), bodies);
        if (preflight.can_solve()) {
            indices.push_back(islandIndex);
    return indices;

IslandConstraintSolvePreflight preflight_island_constraint_solve(

bool is_body_dynamic(u32 flags) {
    return !is_body_static_or_kinematic(flags);

bool is_body_dynamic_awake(u32 flags) {
    return is_body_dynamic(flags) && !is_body_sleeping(flags);

bool island_all_dynamic_bodies_sleeping(const ContactIslandGraph::Island& island,
    bool foundDynamic = false;
        if (!is_body_dynamic(bodies.flags[bodyIndex])) {
        foundDynamic = true;
    return foundDynamic;

        if (is_body_dynamic_awake(bodies.flags[bodyIndex])) {

IslandSleepSolvePreflight preflight_island_sleep_solve(const ContactIslandGraph::Island& island,
    IslandSleepSolvePreflight preflight{};

bool should_skip_inactive_island_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {

bool should_skip_inactive_island_solve_index(const ContactIslandGraph& graph,
    return should_skip_inactive_island_solve(graph.island(islandIndex), bodies);

IslandConstraintRefsPreflight preflight_island_solvable_constraint_refs(
    IslandConstraintRefsPreflight preflight = preflight_island_constraint_refs(island, contacts, distanceConstraints);

        if (contactIndex >= contacts.size()) {
        if (!contact.valid) {
        if (!is_fully_inactive_constraint_pair(bodies, contact.bodyA, contact.bodyB)) {
            ++preflight.solvableContactCount;

    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceConstraints.size()) {
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        if (!is_fully_inactive_constraint_pair(bodies, constraint.bodyA, constraint.bodyB)) {
            ++preflight.solvableDistanceCount;


bool should_skip_island_solvable_constraint_refs(const ContactIslandGraph::Island& island,
    return !preflight_island_solvable_constraint_refs(island, bodies, contacts, distanceConstraints)
                .has_solvable_constraints();

IslandSolveBodiesPreflight preflight_island_solve_bodies(


    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.stats = compute_island_sleep_stats(bodies, island);
    preflight.fullySleeping = is_island_fully_sleeping(bodies, island);
    preflight.constraintCount = island_constraint_count(island);
    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
    preflight.ownedBodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            ++preflight.immobileCount;
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_static_or_kinematic(flags)) {
        } else if ((flags & RB_SLEEPING) != 0u) {
            ++preflight.sleepingCount;
        } else {
            ++preflight.awakeDynamicCount;
    return preflight;

IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
                                                                  u32 islandIndex,
                                                                  const RigidBodySoA& bodies) {
    IslandSolveBodiesPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
    return preflight_island_solve_bodies(graph.island(islandIndex), bodies);

bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_bodies(island, bodies).can_solve();

IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
    IslandSleepPreflight preflight{};
    if (!island_has_constraints(island)) {

        if (is_body_static_or_kinematic(bodies.flags[bodyIndex])) {
        ++preflight.dynamicBodyCount;
        if (is_body_sleeping(bodies.flags[bodyIndex])) {
            ++preflight.sleepingDynamicCount;

    preflight.allDynamicSleeping =
        preflight.dynamicBodyCount > 0u && preflight.sleepingDynamicCount == preflight.dynamicBodyCount;

IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(const RigidBodySoA& bodies,
                                                                            const ContactIslandGraph& graph,
                                                                            f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    return preflight_island_constraint_solve(bodies, graph.island(islandIndex), dt);

bool should_skip_island_constraint_solve(const RigidBodySoA& bodies,
                                         const ContactIslandGraph::Island& island,
    return !preflight_island_constraint_solve(bodies, island, dt).can_solve();

bool should_skip_island_constraint_solve_index(const RigidBodySoA& bodies,
        return true;
    return should_skip_island_constraint_solve(bodies, graph.island(islandIndex), dt);

IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
                                            f32 sleepLinearThreshold,
                                            f32 sleepAngularThreshold) {
    if (should_skip_warm_start_island(island)) {


            ++preflight.sleepingBodyCount;

        const f32 linearSpeed = bodies.linearVelocities[bodyIndex].length();
        const f32 angularSpeed = bodies.angularVelocities[bodyIndex].length();
        if (linearSpeed < sleepLinearThreshold && angularSpeed < sleepAngularThreshold) {
            ++preflight.belowThresholdCount;

    if (preflight.dynamicBodyCount == 0u) {

IslandSleepPreflight preflight_island_sleep_by_index(const RigidBodySoA& bodies,
    return preflight_island_sleep(bodies,
                                  graph.island(islandIndex),
                                  sleepLinearThreshold,
                                  sleepAngularThreshold);

bool should_skip_island_sleep_check(const ContactIslandGraph::Island& island) {
    return !island_has_constraints(island);

bool should_skip_island_sleep_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_island_sleep_check(graph.island(islandIndex));

IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    IslandWakePreflight preflight{};

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());


            ++preflight.awakeBodyCount;

        if (bodies.forces[bodyIndex].length() > 0.f) {
            preflight.hasExternalForces = true;


IslandWakePreflight preflight_island_wake_by_index(const RigidBodySoA& bodies,
                                                   u32 islandIndex) {
    return preflight_island_wake(bodies, graph.island(islandIndex));

bool should_skip_island_wake_check(const ContactIslandGraph::Island& island) {

bool should_skip_island_wake_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_island_wake_check(graph.island(islandIndex));

IslandSleepWakeStats compute_island_sleep_wake_stats(const RigidBodySoA& bodies,
    IslandSleepWakeStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
            ++stats.emptyCount;

        const IslandSleepPreflight sleepPreflight =
            preflight_island_sleep(bodies, island, sleepLinearThreshold, sleepAngularThreshold);
        if (sleepPreflight.can_consider_sleep()) {
            ++stats.sleepCandidateCount;
        if (sleepPreflight.all_dynamic_sleeping()) {
            ++stats.allSleepingCount;

        const IslandWakePreflight wakePreflight = preflight_island_wake(bodies, island);
        if (wakePreflight.should_wake()) {
            ++stats.wakeCandidateCount;
    return stats;
    const bool hasAwakeDynamic = island_has_awake_dynamic_body(island, bodies);
        if (!is_sleeping_body(bodies, bodyIndex)) {
        if (hasAwakeDynamic) {
            ++preflight.awakeNeighborCount;
        if (bodyIndex < bodies.forces.size()) {
            const vec3 force = bodies.forces[bodyIndex];
            if (force.x != 0.f || force.y != 0.f || force.z != 0.f) {
                ++preflight.externalForceCount;

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid) {
        const bool sleepingA = is_sleeping_body(bodies, contact.bodyA);
        const bool sleepingB = is_sleeping_body(bodies, contact.bodyB);
        const bool awakeA = is_awake_dynamic_body(bodies, contact.bodyA);
        const bool awakeB = is_awake_dynamic_body(bodies, contact.bodyB);
        if ((sleepingA && awakeB) || (sleepingB && awakeA)) {
            ++preflight.contactWakeCount;


bool should_skip_island_wake(const ContactIslandGraph::Island& island,
                             const RigidBodySoA& bodies,
                             const std::vector<narrowphase::ContactManifold>& contacts) {
    return !preflight_island_wake(island, bodies, contacts).should_wake();

u32 wake_island_bodies_guarded(RigidBodySoA& bodies,
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies, contacts);
    if (!preflight.should_wake()) {
        return 0u;

    u32 wokenCount = 0u;
        bool shouldWake = false;
        if (island_has_awake_dynamic_body(island, bodies)) {
            shouldWake = true;
        if (!shouldWake && bodyIndex < bodies.forces.size()) {
        if (!shouldWake) {
                if (contact.bodyA == bodyIndex && is_awake_dynamic_body(bodies, contact.bodyB)) {
                    break;
                if (contact.bodyB == bodyIndex && is_awake_dynamic_body(bodies, contact.bodyA)) {
        bodies.flags[bodyIndex] &= ~RB_SLEEPING;
        if (bodyIndex < bodies.sleepTimers.size()) {
            bodies.sleepTimers[bodyIndex] = 0.f;
        ++wokenCount;
    return wokenCount;

IslandSolveJobSleepPreflight preflight_solve_island_job_with_sleep(const IslandSolveJob& job,
                                                                   f32 dt,
    IslandSolveJobSleepPreflight preflight{};
    preflight.job = preflight_solve_island_job(job, dt);
    if (job.island != nullptr) {
        preflight.sleep = preflight_island_sleep(*job.island, bodies);
        preflight.sleep.skipped = true;
    preflight.skipped = !preflight.job.can_dispatch() || !preflight.sleep.can_solve();

bool should_skip_solve_island_job_with_sleep(const IslandSolveJob& job,
    return !preflight_solve_island_job_with_sleep(job, dt, bodies).can_dispatch();

IslandSleepDispatchPreflight preflight_island_dispatch_with_sleep(const ContactIslandGraph& graph,
    IslandSleepDispatchPreflight preflight{};
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    const IslandSleepStats stats = compute_island_sleep_stats(graph, bodies);
    preflight.solvableIslandCount = stats.solvableCount;
    preflight.allSleepingIslandCount = stats.allSleepingCount;
    preflight.skipped = preflight.dispatch.skipped || stats.solvableCount == 0u;

bool should_skip_island_dispatch_with_sleep(const ContactIslandGraph& graph,
    return !preflight_island_dispatch_with_sleep(graph, dt, bodies).can_dispatch();

std::vector<u32> collect_solvable_sleep_island_indices(const ContactIslandGraph& graph,
    preflight.totalBodies = static_cast<u32>(island.bodyIndices.size());
        if (!is_island_body_index_valid(bodyIndex, bodies.count())) {
        if (is_island_body_static_or_kinematic(bodies, bodyIndex)) {
            ++preflight.staticBodyCount;
        if (is_island_body_sleeping(bodies, bodyIndex)) {

IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
    IslandSleepWakePreflight preflight{};
    return preflight_island_sleep_wake(graph.island(islandIndex), bodies);

bool should_skip_island_solve_for_sleep(const ContactIslandGraph::Island& island,
    return !preflight_island_sleep_wake(island, bodies).can_solve();

bool should_skip_island_solve_for_sleep_index(const ContactIslandGraph& graph,
    return should_skip_island_solve_for_sleep(graph.island(islandIndex), bodies);

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,

    preflight.hasConstraints = true;
    preflight.constrainedBodyCount = static_cast<u32>(island.bodyIndices.size());

bool should_wake_island(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_wake(island, bodies).can_wake();

IslandSleepWakeStats compute_island_sleep_wake_stats(const ContactIslandGraph& graph,
        const IslandSleepWakePreflight preflight =
            preflight_island_sleep_wake(graph.island(islandIndex), bodies);
        if (preflight.skipped) {
        } else if (preflight.can_solve()) {
            ++stats.solvableCount;
        } else if (preflight.all_dynamic_sleeping()) {

IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,
    IslandSleepWakeGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_wake_stats(graph, bodies);
    preflight.skipped = preflight.stats.solvableCount == 0u;

bool should_skip_island_sleep_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_wake_graph(graph, bodies).can_solve_any();

std::vector<u32> collect_solvable_island_indices(const ContactIslandGraph& graph,
        ++preflight.inRangeBodyCount;
        if (!is_static_or_kinematic_body(bodies, bodyIndex) && !is_sleeping_body(bodies, bodyIndex)) {

bool should_skip_island_body_refs(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_body_refs(island, bodies).can_solve();

IslandSolveRefsPreflight preflight_island_solve_refs(
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandSolveRefsPreflight preflight{};
        preflight.constraints.skipped = true;
        preflight.bodies.skipped = true;

    preflight.constraints = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_body_refs(island, bodies);

bool should_skip_island_solve_refs(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_refs(island, bodies, contacts, distanceConstraints).can_solve();

bool is_sleeping_body(const RigidBodySoA& bodies, u32 bodyIndex) {
        return false;
    return (bodies.flags[bodyIndex] & RB_SLEEPING) != 0u;

bool is_static_or_kinematic_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;

bool is_dynamic_awake_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    return !is_static_or_kinematic_body(bodies, bodyIndex) && !is_sleeping_body(bodies, bodyIndex);

bool body_exceeds_wake_threshold(const RigidBodySoA& bodies,
                                 u32 bodyIndex,
                                 f32 linearThreshold,
                                 f32 angularThreshold) {
    if (bodyIndex >= bodies.count() || is_static_or_kinematic_body(bodies, bodyIndex)) {

    return linearSpeed >= linearThreshold || angularSpeed >= angularThreshold;


        if (is_static_or_kinematic_body(bodies, bodyIndex)) {
            ++preflight.staticOrKinematicCount;
        if (is_sleeping_body(bodies, bodyIndex)) {
            ++preflight.dynamicAwakeCount;

IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
    return preflight_island_sleep(graph.island(islandIndex), bodies);


        if (body_exceeds_wake_threshold(bodies, bodyIndex, linearThreshold, angularThreshold)) {
            ++preflight.wakeCandidateCount;
        if (bodies.forces[bodyIndex].length() > 0.f || bodies.torques[bodyIndex].length() > 0.f) {

        if (contact.valid && contact.maxPenetration() > 0.f) {
            ++preflight.penetratingContactCount;

IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
    return preflight_island_wake(graph.island(islandIndex), bodies, contacts, linearThreshold, angularThreshold);

IslandSleepGraphStats compute_island_sleep_graph_stats(const ContactIslandGraph& graph,
    IslandSleepGraphStats stats{};
        const IslandSleepPreflight preflight = preflight_island_sleep(island, bodies);
            ++stats.fullySleepingCount;
            ++stats.partiallyAwakeCount;

IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
    IslandSleepGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_graph_stats(graph, bodies);
    preflight.skipped = preflight.stats.totalIslands == 0u;

IslandWakeGraphPreflight preflight_island_wake_graph(const ContactIslandGraph& graph,
    IslandWakeGraphPreflight preflight{};
    preflight.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < preflight.totalIslands; ++islandIndex) {
        const IslandWakePreflight islandPreflight =
            preflight_island_wake(graph.island(islandIndex), bodies, contacts, linearThreshold, angularThreshold);
        if (islandPreflight.should_wake()) {
            ++preflight.wakeableIslandCount;
    preflight.skipped = preflight.totalIslands == 0u;

    return preflight_island_sleep(island, bodies).can_skip_solve();


bool should_wake_island(const ContactIslandGraph::Island& island,
    return preflight_island_wake(island, bodies, contacts, linearThreshold, angularThreshold).should_wake();

std::vector<u32> collect_fully_sleeping_island_indices(const ContactIslandGraph& graph,
        if (bodyIndex < bodies.count()) {

bool should_skip_island_body_refs(const ContactIslandGraph::Island& island,

IslandSleepPreflight preflight_island_sleep_state(const ContactIslandGraph::Island& island,

        } else if (is_sleeping_body(bodies, bodyIndex)) {

IslandSleepPreflight preflight_island_sleep_state_by_index(const ContactIslandGraph& graph,
    return preflight_island_sleep_state(graph.island(islandIndex), bodies);

bool should_skip_solve_sleeping_island(const ContactIslandGraph::Island& island,
    return !preflight_island_sleep_state(island, bodies).can_solve_awake();


    const IslandSleepPreflight sleep = preflight_island_sleep_state(island, bodies);
    preflight.sleepingCount = sleep.sleepingCount;
    preflight.awakeDynamicCount = sleep.awakeDynamicCount;

    return preflight_island_wake(island, bodies).needs_wake();

IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph& graph,
    IslandSleepStats stats{};

        if (sleep.all_dynamic_sleeping()) {
        } else if (sleep.awakeDynamicCount > 0u) {
            ++stats.awakeCount;

u32 count_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return compute_island_sleep_stats(graph, bodies).awakeCount;

bool has_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_awake_islands(graph, bodies) > 0u;

    preflight.stats = compute_island_sleep_stats(graph, bodies);
    preflight.skipped = preflight.stats.awakeCount == 0u;

bool should_skip_island_dispatch_for_sleep(const ContactIslandGraph& graph,
    return !preflight_island_sleep_graph(graph, bodies).can_dispatch_awake();

std::vector<u32> collect_awake_island_indices(const ContactIslandGraph& graph,
        if (is_body_static_or_kinematic(bodies, bodyIndex)) {
        if (is_body_sleeping(bodies, bodyIndex)) {

        preflight.awakeDynamicCount == 0u && preflight.sleepingCount > 0u;




        } else if (preflight.can_sleep()) {
            ++stats.sleepingCount;

u32 count_solvable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return compute_island_sleep_wake_stats(graph, bodies).solvableCount;

bool has_solvable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_solvable_islands(graph, bodies) > 0u;


bool should_skip_island_dispatch_for_sleep(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_wake_graph(graph, bodies).can_solve();

    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        if (preflight.can_solve()) {
            indices.push_back(islandIndex);
    return indices;
        if (is_body_static_or_kinematic(bodies, bodyIndex)) {
            ++preflight.staticCount;

        ++preflight.candidateCount;
        if (is_body_sleeping(bodies, bodyIndex)) {
            if (linearSpeed >= linearSleepThreshold || angularSpeed >= angularSleepThreshold) {
            ++preflight.alreadyAwakeCount;


                                                   f32 linearSleepThreshold,
                                                   f32 angularSleepThreshold) {
    return preflight_island_wake(graph.island(islandIndex),
                                 bodies,
                                 linearSleepThreshold,
                                 angularSleepThreshold);

bool should_skip_island_wake_check(const ContactIslandGraph::Island& island,
    return preflight_island_wake(island, bodies, 0.f, 0.f).can_skip_wake_check();

IslandSolveCombinedPreflight preflight_island_solve_combined(
    const IslandSolveJob& job,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandSolveCombinedPreflight preflight{};
        preflight.constraintRefs =
            preflight_island_constraint_refs(*job.island, contacts, distanceConstraints);
        preflight.sleep = preflight_island_sleep_for_solve(*job.island, bodies);
        preflight.constraintRefs.skipped = true;
    preflight.skipped = !preflight.can_dispatch();

bool should_skip_island_solve_combined(const IslandSolveJob& job,
    return !preflight_island_solve_combined(job, bodies, contacts, distanceConstraints, dt)
                .can_dispatch();
        if (!body_index_in_range(bodies, bodyIndex)) {
        if (!is_sleeping_body(flags) || is_static_or_kinematic_body(flags)) {
        if (bodies.forces[bodyIndex].dot(bodies.forces[bodyIndex]) > 0.f) {
            ++preflight.forcedWakeCount;

bool should_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_wake(island, bodies).can_wake();

IslandConstraintSolvePreflight preflight_island_constraint_solve(
    preflight.invalidDt = !is_finite_island_solve_dt(dt);
    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.sleep = preflight_island_sleep_state(island, bodies);
    preflight.skipped = preflight.refs.skipped || preflight.bodies.skipped || preflight.sleep.skipped;

bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
    return !preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints, dt).can_solve();

bool solve_island_job_guarded(RigidBodySoA& bodies,
                              SolverWorkBuffers& workBuffers,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints, dt)) {
    preflight.sleep = preflight_island_sleep(bodies, island);

bool should_skip_solve_island_with_sleep(const RigidBodySoA& bodies,
    return !preflight_solve_island_with_sleep(bodies, island, contacts, distanceConstraints).can_solve();

IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
        const IslandSleepPreflight preflight = preflight_island_sleep(bodies, graph.island(islandIndex));
        } else if (preflight.is_fully_sleeping()) {

u32 count_solvable_sleep_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return compute_island_sleep_stats(graph, bodies).solvableCount;

bool has_solvable_sleep_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_solvable_sleep_islands(graph, bodies) > 0u;

IslandWakeStats compute_island_wake_stats(const ContactIslandGraph& graph,
    IslandWakeStats stats{};
        const IslandWakePreflight preflight =
            preflight_island_wake(bodies, graph.island(islandIndex), contacts);
        } else if (preflight.can_wake()) {
            ++stats.wakeableCount;

u32 count_wakeable_islands(const ContactIslandGraph& graph,
    return compute_island_wake_stats(graph, bodies, contacts).wakeableCount;

u32 wake_all_islands_guarded(RigidBodySoA& bodies,
    u32 wokenCount = 0;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        if (!should_wake_island_bodies(bodies, island, contacts)) {
        wokenCount += wake_island_bodies_guarded(bodies, island);

    if (should_skip_solve_island_with_sleep(bodies, island, contacts, distanceConstraints)) {
    return solve_island_job(bodies,
                            island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);
        if (island_body_is_static_or_kinematic(bodies, bodyIndex)) {
        } else if (island_body_is_sleeping(bodies, bodyIndex)) {



    preflight.sleepWake = preflight_island_sleep_wake(island, bodies);

    return !preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints).can_solve();

        const IslandSleepWakePreflight preflight = preflight_island_sleep_wake(graph.island(islandIndex), bodies);
        } else if (preflight.should_remain_asleep()) {

u32 count_solvable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return compute_island_sleep_wake_stats(graph, bodies).solvableCount;

bool has_solvable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_solvable_islands(graph, bodies) > 0u;


bool should_skip_island_solve_sleep_wake(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !has_solvable_islands(graph, bodies);

    if (should_skip_island_solve_job(job)) {

    preflight.refs = preflight_island_constraint_refs(*job.island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(*job.island, bodies);
    preflight.sleepWake = preflight_island_sleep_wake(*job.island, bodies);

bool should_skip_island_constraint_solve(const IslandSolveJob& job,
    return !preflight_island_constraint_solve(job, bodies, contacts, distanceConstraints, dt).can_solve();
        const IslandSleepPreflight preflight = preflight_island_sleep(graph.island(islandIndex), bodies);
        if (preflight.all_dynamic_sleeping()) {

std::vector<u32> collect_wakeable_island_indices(const ContactIslandGraph& graph,
        if (preflight.should_wake()) {
        if (is_static_or_kinematic(bodies.flags[bodyIndex])) {

    preflight.allSleeping =
        preflight.dynamicBodyCount > 0u && preflight.sleepingBodyCount == preflight.dynamicBodyCount;


bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island,

bool should_skip_sleeping_island_solve_index(const ContactIslandGraph& graph,
    return should_skip_sleeping_island_solve(graph.island(islandIndex), bodies);

            ++preflight.emptyCount;
        const IslandSleepPreflight islandSleep = preflight_island_sleep(island, bodies);
        if (islandSleep.can_skip_solve()) {
            ++preflight.allSleepingCount;
            ++preflight.awakeCount;

    return preflight_island_sleep_graph(graph, bodies).awakeCount;


        if (!should_skip_sleeping_island_solve(island, bodies)) {



    preflight.needsWake = preflight.sleepingDynamicCount > 0u && preflight.awakeDynamicCount > 0u;

    return preflight_island_wake(graph.island(islandIndex), bodies);

    return !preflight_island_wake(island, bodies).should_wake();

u32 wake_island_bodies(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    u32 awakenedCount = 0;
        if (!is_body_sleeping(bodies, bodyIndex)) {
        ++awakenedCount;
    return awakenedCount;

bool wake_island_bodies_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies);
    return wake_island_bodies(bodies, island) > 0u;

bool wake_island_bodies_by_index_guarded(RigidBodySoA& bodies,
    return wake_island_bodies_guarded(bodies, graph.island(islandIndex));

IslandSolvePreflightCombined preflight_island_solve_combined(
    IslandSolvePreflightCombined preflight{};

    preflight.bodies = preflight_island_solve_bodies(island, bodies);
    preflight.sleep = preflight_island_sleep(island, bodies);

bool dispatch_solve_island_skip_sleeping(RigidBodySoA& bodies,
    if (should_skip_sleeping_island_solve(island, bodies) ||
        should_skip_island_solve_bodies(island, bodies)) {
    return dispatch_solve_island(bodies,
                                 graph,
                                 islandIndex,
        if (preflight_island_sleep_state(island, bodies).can_solve_awake()) {

IslandSolvePassPreflight preflight_island_solve_pass(
    IslandSolvePassPreflight preflight{};

    preflight.constraintRefs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodyRefs = preflight_island_body_refs(island, bodies);

bool should_skip_island_solve_pass(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_pass(island, bodies, contacts, distanceConstraints).can_solve();

        if (!is_body_dynamic(bodies.flags[bodyIndex])) {

IslandSleepSolvePreflight preflight_island_sleep_solve_by_index(const ContactIslandGraph& graph,
    IslandSleepSolvePreflight preflight{};
    return preflight_island_sleep_solve(graph.island(islandIndex), bodies);

    const IslandSleepSolvePreflight preflight = preflight_island_sleep_solve(island, bodies);
    return preflight.skipped || (preflight.dynamicBodyCount > 0u && preflight.awakeBodyCount == 0u);

        const IslandSleepSolvePreflight sleepPreflight = preflight_island_sleep_solve(island, bodies);
        if (sleepPreflight.awakeBodyCount > 0u) {
        } else if (sleepPreflight.dynamicBodyCount > 0u) {
            ++preflight.sleepingOnlyCount;
    preflight.skipped = preflight.awakeCount == 0u;



bool should_skip_solve_all_sleeping_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_graph(graph, bodies).can_dispatch();



        if (contacts[contactIndex].warmNormalImpulse != 0.f) {
            ++preflight.nonZeroImpulseCount;


bool should_skip_wake_island(const ContactIslandGraph::Island& island,

void wake_island_bodies(const ContactIslandGraph::Island& island, RigidBodySoA& bodies) {

bool wake_island_bodies_guarded(const ContactIslandGraph::Island& island,
                              RigidBodySoA& bodies,
    wake_island_bodies(island, bodies);

IslandSolveBodyPreflight preflight_solve_island_with_bodies(
    IslandSolveBodyPreflight preflight{};
    preflight.sleep = preflight_island_sleep_solve(island, bodies);
    preflight.skipped = preflight.refs.skipped || preflight.sleep.skipped;

bool should_skip_solve_island_with_bodies(const ContactIslandGraph::Island& island,
    return !preflight_solve_island_with_bodies(island, bodies, contacts, distanceConstraints).can_solve();
        preflight_island_solvable_constraint_refs(island, bodies, contacts, distanceConstraints);

    return !preflight_island_solve_bodies(island, bodies, contacts, distanceConstraints).can_solve();

IslandDispatchBodiesPreflight preflight_island_dispatch_with_bodies(const ContactIslandGraph& graph,
    IslandDispatchBodiesPreflight preflight{};
    preflight.sleepWake = preflight_island_sleep_wake_graph(graph, bodies);
    preflight.skipped = preflight.dispatch.skipped || preflight.sleepWake.skipped;

bool should_skip_island_dispatch_with_bodies(const ContactIslandGraph& graph,
    return !preflight_island_dispatch_with_bodies(graph, bodies, dt).can_dispatch();
        const IslandSleepWakePreflight preflight =
            preflight_island_sleep_wake(graph.island(islandIndex), bodies);
        }

    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.skipped = preflight.refs.skipped || preflight.sleepWake.skipped;
    return preflight;


IslandSolvePreflight preflight_island_solve(const ContactIslandGraph& graph) {
    IslandSolvePreflight preflight{};
    preflight.reason = island_solve_reject_reason(graph);
    preflight.stats = compute_island_solve_stats(graph);
    preflight.reason = islandSolveRejectReason(graph);
    preflight.noDispatchableIslands = preflight.reason == IslandSolveRejectReason::NoDispatchableIslands;
    preflight.skipped = preflight.noDispatchableIslands;
    preflight.skipped = preflight.stats.dispatchableCount == 0u;
    if (preflight.skipped) {
        preflight.reason = IslandDispatchRejectReason::NoDispatchableIslands;
    if (preflight.stats.dispatchableCount == 0u) {
        preflight.reason = IslandDispatchRejectReason::NothingDispatchable;
        preflight.skipped = true;
    }
    preflight.reason = islandSolveRejectReason(preflight);
        preflight.reason = IslandSolveRejectReason::NoDispatchableIslands;
        preflight.reason = IslandDispatchRejectReason::EmptyGraph;
        preflight.reason = IslandSolveRejectReason::EmptyIsland;
    return preflight;
}

IslandSleepPreflight preflight_island_sleep_by_index(const RigidBodySoA& bodies,
                                                    const ContactIslandGraph& graph,
                                                    u32 islandIndex) {
    IslandSleepPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_sleep(bodies, graph.island(islandIndex));

bool should_skip_solve_sleeping_island(const RigidBodySoA& bodies,
                                       const ContactIslandGraph::Island& island) {
    return !preflight_island_sleep(bodies, island).can_solve();

bool should_skip_solve_sleeping_island_index(const RigidBodySoA& bodies,
        return true;
    return should_skip_solve_sleeping_island(bodies, graph.island(islandIndex));

IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
                                          const ContactIslandGraph::Island& island,
                                          f32 linearWakeThreshold,
                                          f32 angularWakeThreshold) {
    IslandWakePreflight preflight{};
    if (!island_has_constraints(island)) {

    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        if (is_body_static_or_kinematic(bodies.flags[bodyIndex])) {

        if (!is_body_sleeping(bodies.flags[bodyIndex])) {

        const vec3 force = bodies.forces[bodyIndex];
        const vec3 torque = bodies.torques[bodyIndex];
        if (force.dot(force) > 0.f || torque.dot(torque) > 0.f) {
            preflight.hasExternalForce = true;

        const f32 linearSpeed = bodies.linearVelocities[bodyIndex].length();
        const f32 angularSpeed = bodies.angularVelocities[bodyIndex].length();
        if (linearSpeed >= linearWakeThreshold || angularSpeed >= angularWakeThreshold) {
            preflight.hasVelocityWake = true;
IslandDispatchPreflight preflight_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchPreflight preflight{};
    preflight.reason = diagnose_island_dispatch_reject(graph, dt);
    preflight.solve = preflight_island_solve(graph);
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.nonFiniteDt = is_valid_island_solve_dt(dt) && !std::isfinite(dt);
    preflight.reason = island_dispatch_reject_reason(graph, dt);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt;
    preflight.reason = islandDispatchRejectReason(graph, dt);
    preflight.noDispatchableIslands = preflight.reason == IslandDispatchRejectReason::NoDispatchableIslands;
    if (!std::isfinite(dt)) {
        preflight.reason = IslandDispatchRejectReason::NonFiniteDt;
        preflight.invalidDt = true;
    } else if (!is_valid_island_solve_dt(dt)) {
    preflight.invalidDt = !is_finite_island_solve_dt(dt);
    if (preflight.invalidDt) {
        preflight.reason = IslandDispatchRejectReason::InvalidDt;
    } else if (preflight.solve.skipped) {
        preflight.reason = preflight.solve.reason;
    }
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.skipped = preflight.solve.skipped;
    preflight.skipped = preflight.reason == IslandDispatchRejectReason::NoDispatchableIslands;
    preflight.skipped = preflight.reason != IslandDispatchRejectReason::None;
    preflight.reason = classifyIslandDispatchReject(preflight);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.reason = classify_dispatch_dt_reject(dt);
    preflight.invalidDt = preflight.reason != IslandDispatchRejectReason::None;
    if (preflight.reason == IslandDispatchRejectReason::None && preflight.solve.skipped) {
        preflight.skipped = true;
    preflight.reason = islandDispatchRejectReason(preflight);
    if (preflight.invalidDt) {
        preflight.reason = IslandDispatchRejectReason::NoDispatchableIslands;
    preflight.invalidDt = !is_finite_island_solve_dt(dt);
        return preflight;
    if (preflight.solve.skipped) {
    }
    return preflight;
}

IslandWakePreflight preflight_island_wake_by_index(const RigidBodySoA& bodies,
                                                   const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   f32 linearWakeThreshold,
                                                   f32 angularWakeThreshold) {
    IslandWakePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_wake(bodies,
                               graph.island(islandIndex),
                               linearWakeThreshold,
                               angularWakeThreshold);
IslandDispatchSleepPreflight preflight_island_dispatch_sleep(const ContactIslandGraph& graph,
                                                             const RigidBodySoA& bodies,
                                                             f32 dt) {
    IslandDispatchSleepPreflight preflight{};
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.skipped = preflight.dispatch.skipped || preflight.sleep.skipped;

bool should_skip_island_dispatch_sleep(const ContactIslandGraph& graph,
    return !preflight_island_dispatch_sleep(graph, bodies, dt).can_dispatch();

IslandBuiltGraphPreflight preflight_built_island_graph(const ContactIslandGraph& graph,
                                                       u32 bodyCount,
                                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                                       const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuiltGraphPreflight preflight{};
    preflight.stats.totalIslands = graph.islandCount();
    if (preflight.stats.totalIslands == 0u) {

    for (u32 islandIndex = 0; islandIndex < preflight.stats.totalIslands; ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (island.isEmpty()) {
            ++preflight.stats.emptyCount;
        } else {
            ++preflight.stats.constrainedCount;

        for (u32 bodyIndex : island.bodyIndices) {
            if (bodyIndex >= bodyCount) {
                ++preflight.stats.outOfRangeBodyIndexCount;
        for (u32 contactIndex : island.contactIndices) {
            if (contactIndex >= contacts.size()) {
                ++preflight.stats.outOfRangeContactRefCount;
                continue;
            const narrowphase::ContactManifold& contact = contacts[contactIndex];
            if (contact.bodyA >= bodyCount || contact.bodyB >= bodyCount) {
        for (u32 distanceIndex : island.distanceIndices) {
            if (distanceIndex >= distanceConstraints.size()) {
                ++preflight.stats.outOfRangeDistanceRefCount;
            const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
            if (constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount) {

bool should_skip_built_island_graph(const ContactIslandGraph& graph,
    return !preflight_built_island_graph(graph, bodyCount, contacts, distanceConstraints).is_consistent();

bool should_skip_island_solve(const ContactIslandGraph& graph) {
    return !has_dispatchable_islands(graph);
}

bool should_wake_island(const RigidBodySoA& bodies,
                        const ContactIslandGraph::Island& island,
                        f32 linearWakeThreshold,
                        f32 angularWakeThreshold) {
    return preflight_island_wake(bodies, island, linearWakeThreshold, angularWakeThreshold).can_wake();
}

IslandConstraintSolvePreflight preflight_island_constraint_solve(const RigidBodySoA& bodies,
                                                                 const ContactIslandGraph::Island& island,
                                                                 f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.emptyIsland = !island_has_constraints(island);
    preflight.constraintCount = island_constraint_count(island);
    preflight.sleep = preflight_island_sleep(bodies, island);
    preflight.skipped = preflight.emptyIsland || preflight.sleep.skipped;
    return preflight;
}

IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(const RigidBodySoA& bodies,
                                                                            const ContactIslandGraph& graph,
                                                                            u32 islandIndex,
                                                                            f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_constraint_solve(bodies, graph.island(islandIndex), dt);
}

bool should_skip_island_constraint_solve(const RigidBodySoA& bodies,
                                         const ContactIslandGraph::Island& island,
                                         f32 dt) {
    return !preflight_island_constraint_solve(bodies, island, dt).can_solve();
}

IslandSleepGraphPreflight preflight_island_sleep_graph(const RigidBodySoA& bodies,
                                                       const ContactIslandGraph& graph) {
    IslandSleepGraphPreflight preflight{};
    preflight.totalIslands = graph.islandCount();

    for (u32 islandIndex = 0; islandIndex < preflight.totalIslands; ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island_has_constraints(island)) {
            continue;
        }

        const IslandSleepPreflight islandPreflight = preflight_island_sleep(bodies, island);
        if (islandPreflight.fullySleeping) {
            ++preflight.fullySleepingIslandCount;
        } else if (islandPreflight.can_solve()) {
            ++preflight.activeIslandCount;
        }
    }

    preflight.skipped = preflight.activeIslandCount == 0u;
    return preflight;
}

bool should_skip_island_sleep_dispatch(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return !preflight_island_sleep_graph(bodies, graph).has_active_islands();
}

std::vector<u32> collect_awake_island_indices(const RigidBodySoA& bodies,
                                              const ContactIslandGraph& graph) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (!island_has_constraints(island)) {
            continue;
        }
        if (!should_skip_solve_sleeping_island(bodies, island)) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

IslandSolvePreflight preflight_island_solve(const ContactIslandGraph& graph) {
    IslandSolvePreflight preflight{};
    preflight.stats = compute_island_solve_stats(graph);
    preflight.skipped = preflight.stats.dispatchableCount == 0u;

IslandDispatchPreflight preflight_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchPreflight preflight{};
    preflight.solve = preflight_island_solve(graph);
    preflight.skipped = preflight.solve.skipped;

IslandJobDispatchPreflight preflight_dispatch_island_index(const ContactIslandGraph& graph,
                                                           u32 islandIndex,
                                                           f32 dt) {
    IslandJobDispatchPreflight preflight{};
    preflight.job = extract_island(graph, islandIndex);
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.skipped = !should_solve_island(preflight.job);
    return preflight;
}

bool should_skip_dispatch_island_index(const ContactIslandGraph& graph, u32 islandIndex, f32 dt) {
    const IslandJobDispatchPreflight preflight = preflight_dispatch_island_index(graph, islandIndex, dt);
    return !preflight.can_dispatch();
IslandSolveStats compute_island_solve_stats_from_jobs(const std::vector<IslandSolveJob>& jobs) {
    IslandSolveStats stats{};
    stats.totalIslands = static_cast<u32>(jobs.size());
    for (const IslandSolveJob& job : jobs) {
        if (job.empty) {
            ++stats.emptyCount;
        } else {
            ++stats.constrainedCount;
        if (should_solve_island(job)) {
            ++stats.dispatchableCount;
    return stats;

u32 count_dispatchable_jobs(const std::vector<IslandSolveJob>& jobs) {
    return compute_island_solve_stats_from_jobs(jobs).dispatchableCount;

bool has_dispatchable_jobs(const std::vector<IslandSolveJob>& jobs) {
    return count_dispatchable_jobs(jobs) > 0u;

IslandDispatchJobPreflight preflight_island_dispatch_from_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt) {
    IslandDispatchJobPreflight preflight{};
    preflight.stats = compute_island_solve_stats_from_jobs(jobs);

bool should_skip_island_dispatch_from_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt) {
    return !preflight_island_dispatch_from_jobs(jobs, dt).can_dispatch();

bool should_skip_island_solve(const ContactIslandGraph& graph) {
    return !has_dispatchable_islands(graph);

bool should_skip_island_solve_index(const ContactIslandGraph& graph, u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    return should_skip_island_solve_job(extract_island(graph, islandIndex));

bool should_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    const IslandDispatchPreflight preflight = preflight_island_dispatch(graph, dt);

IslandDispatchJobPreflight preflight_dispatch_island_job(const IslandSolveJob& job, f32 dt) {
    preflight.emptyJob = should_skip_island_solve_job(job);
    preflight.skipped = preflight.invalidDt || preflight.emptyJob;

bool should_skip_dispatch_island_job(const IslandSolveJob& job, f32 dt) {
    return !preflight_dispatch_island_job(job, dt).can_dispatch();

std::vector<u32> collect_dispatchable_island_indices(const ContactIslandGraph& graph) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    return graph.islandCount() == 0u || graph.constrainedIslandCount() == 0u;

bool island_solve_stats_has_work(const IslandSolveStats& stats) {
    return stats.dispatchableCount > 0u;

bool should_skip_all_island_solves(const ContactIslandGraph& graph) {

    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
            indices.push_back(islandIndex);
    return indices;
std::vector<u32> collect_awake_island_indices(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
        if (!should_solve_island(job)) {
            continue;
        if (!should_skip_sleeping_island(*job.island, bodies)) {

std::vector<IslandSolveJob> collect_dispatchable_island_jobs(const ContactIslandGraph& graph) {
    return filter_dispatchable_jobs(extract_island_jobs(graph));
bool preflight_island_solve(const ContactIslandGraph& graph, u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return false;
    return should_solve_island(extract_island(graph, islandIndex));
}

std::vector<IslandSolveJob> filter_dispatchable_jobs(const std::vector<IslandSolveJob>& jobs) {
    std::vector<IslandSolveJob> dispatchable;
    dispatchable.reserve(jobs.size());
    for (const IslandSolveJob& job : jobs) {
    const std::vector<IslandSolveJob> allJobs = extract_island_jobs(graph);
    dispatchable.reserve(allJobs.size());
    for (const IslandSolveJob& job : allJobs) {
        if (should_solve_island(job)) {
            dispatchable.push_back(job);
        }
    }
    return dispatchable;
}

u32 dispatch_solve_all_islands(RigidBodySoA& bodies,
                               const ContactIslandGraph& graph,
bool island_index_dispatchable(const ContactIslandGraph& graph, u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return false;
    }
    return should_solve_island(extract_island(graph, islandIndex));

IslandSolveDispatchPlan build_island_solve_dispatch_plan(const ContactIslandGraph& graph) {
    IslandSolveDispatchPlan plan{};
    plan.preflight = preflight_island_solve(graph);
    if (plan.preflight.can_dispatch()) {
        plan.dispatchIndices = collect_dispatchable_island_indices(graph);
    return plan;

u32 dispatch_islands_from_plan(RigidBodySoA& bodies,
                               const IslandSolveDispatchPlan& plan,
bool should_skip_island_dispatch_job(const IslandSolveJob& job, f32 dt) {
    return !is_valid_island_solve_dt(dt) || should_skip_island_solve_job(job);

bool dispatch_solve_island_job(RigidBodySoA& bodies,
                               const IslandSolveJob& job,
                               SolverWorkBuffers& workBuffers,
                               const std::vector<DistanceConstraint>& distanceConstraints,
                               f32 dt,
                               f32 contactCompliance,
                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (!has_dispatchable_islands(graph)) {
    if (!plan.can_dispatch()) {
        return 0u;

    u32 solvedCount = 0u;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        if (dispatch_solve_island(bodies,
                                  graph,
                                  islandIndex,
                                  workBuffers,
                                  distanceConstraints,
                                  dt,
                                  contactCompliance,
                                  invMassFn)) {
            ++solvedCount;
    return solvedCount;

bool should_warm_start_island(const ContactIslandGraph::Island& island) {
    return island_has_constraints(island);

bool has_prior_lambda_warm_start(const std::vector<f32>& priorDistanceLambdas,
                                 const std::vector<f32>& priorContactLambdas) {
    return !priorDistanceLambdas.empty() || !priorContactLambdas.empty();

bool preflight_frame_lambda_warm_start(const std::vector<DistanceConstraint>& distanceConstraints,
                                       const std::vector<f32>& priorDistanceLambdas,
    if (!has_prior_lambda_warm_start(priorDistanceLambdas, priorContactLambdas)) {
    return !distanceConstraints.empty() || !priorContactLambdas.empty();
    for (u32 islandIndex : plan.dispatchIndices) {
        const IslandDispatchResult result = dispatch_solve_island_result(bodies,
                                                                         invMassFn);
        if (result.solved) {
    if (!is_valid_island_solve_dt(dt) || !should_solve_island(job)) {
    return solve_island_job(bodies,
                            *job.island,
    if (should_skip_island_dispatch_job(job, dt)) {

IslandDispatchResult dispatch_solve_island_job_result(RigidBodySoA& bodies,
    IslandDispatchResult result{};
    result.islandIndex = job.islandIndex;
        result.skipped = true;
        return result;
    result.solved = dispatch_solve_island_job(bodies,
                                              job,
    result.skipped = !result.solved;

IslandBatchDispatchResult dispatch_dispatchable_jobs(
    RigidBodySoA& bodies,
    const std::vector<IslandSolveJob>& jobs,
    IslandBatchDispatchResult result{};
    if (!is_valid_island_solve_dt(dt)) {

    const std::vector<IslandSolveJob> dispatchable = filter_dispatchable_jobs(jobs);
    result.dispatchableCount = static_cast<u32>(dispatchable.size());
    if (dispatchable.empty()) {

    for (const IslandSolveJob& job : dispatchable) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_job_result(bodies,
IslandDispatchJobBatchPreflight preflight_dispatchable_island_jobs(const ContactIslandGraph& graph,
                                                                   f32 dt) {
    IslandDispatchJobBatchPreflight preflight{};
    preflight.graph = preflight_island_dispatch(graph, dt);
    preflight.jobCount = static_cast<u32>(jobs.size());
    preflight.skipped = preflight.jobCount == 0u || !preflight.graph.can_dispatch();
    return preflight;

bool should_skip_dispatchable_island_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt) {
    if (!is_valid_island_solve_dt(dt) || jobs.empty()) {
        return true;
    for (const IslandSolveJob& job : jobs) {
        if (should_solve_island(job)) {

u32 dispatch_dispatchable_island_jobs(RigidBodySoA& bodies,
    return dispatch_dispatchable_island_jobs_result(bodies,
                                                    jobs,
                                                    invMassFn)
        .solvedCount;

IslandBatchDispatchResult dispatch_dispatchable_island_jobs_result(
    result.dispatchableCount = static_cast<u32>(jobs.size());
    if (should_skip_dispatchable_island_jobs(jobs, dt)) {
        result.skippedCount = result.dispatchableCount;

        if (dispatchResult.solved) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;
IslandConstraintIndexValidation validate_island_constraint_indices(
    const ContactIslandGraph::Island& island,
    u32 contactCount,
    u32 distanceConstraintCount) {
    IslandConstraintIndexValidation validation{};
    validation.valid = true;

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contactCount) {
            ++validation.invalidContactIndices;
            validation.valid = false;
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceConstraintCount) {
            ++validation.invalidDistanceIndices;
    return validation;

IslandSolveJobPreflight preflight_island_solve_job(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
    IslandSolveJobPreflight preflight{};
    preflight.job = extract_island(graph, islandIndex);
        preflight.skipped = true;
    if (!should_solve_island(preflight.job)) {
    preflight.indices =
        validate_island_constraint_indices(*preflight.job.island, contactCount, distanceConstraintCount);

IslandSolveJobPreflight preflight_island_solve_by_index(const ContactIslandGraph& graph,
    return preflight_island_solve_job(graph, islandIndex, contactCount, distanceConstraintCount);

bool should_skip_island_solve_invalid_indices(const IslandSolveJob& job,
    if (!should_solve_island(job) || job.island == nullptr) {
    return validate_island_constraint_indices(*job.island, contactCount, distanceConstraintCount)
        .has_invalid_indices();

bool should_skip_island_solve_index(const ContactIslandGraph& graph,
    const IslandSolveJobPreflight preflight =
        preflight_island_solve_by_index(graph, islandIndex, contactCount, distanceConstraintCount);
    return !preflight.can_dispatch();
IslandSolveJobPreflight preflight_island_solve_job(const IslandSolveJob& job) {
    if (job.island == nullptr) {
        preflight.outOfRange = true;
    preflight.empty = job.empty;
    preflight.dispatchable = should_solve_island(job);
    preflight.skipped = !preflight.dispatchable;
IslandSolveJobPreflight preflight_solve_island_job(const IslandSolveJob& job,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   const std::vector<DistanceConstraint>& distanceConstraints) {

    for (u32 contactIndex : job.island->contactIndices) {
        if (contactIndex >= contacts.size()) {
            ++preflight.staleContactCount;
            ++preflight.validContactCount;
    for (u32 distanceIndex : job.island->distanceIndices) {
        if (distanceIndex >= distanceConstraints.size()) {
            ++preflight.staleDistanceCount;
            ++preflight.validDistanceCount;

    preflight.hasStaleIndices = preflight.staleContactCount > 0u || preflight.staleDistanceCount > 0u;

IslandSolveJobPreflight preflight_solve_island_job_by_index(const ContactIslandGraph& graph,
    return preflight_solve_island_job(extract_island(graph, islandIndex), contacts, distanceConstraints);

bool has_stale_island_indices(const IslandSolveJob& job,
    return preflight_solve_island_job(job, contacts, distanceConstraints).hasStaleIndices;

bool should_skip_solve_island_job_stale(const IslandSolveJob& job,
    const IslandSolveJobPreflight preflight = preflight_solve_island_job(job, contacts, distanceConstraints);
    return preflight.skipped || preflight.hasStaleIndices;

IslandDispatchIndexPreflight preflight_dispatch_island_by_index(const ContactIslandGraph& graph,
    IslandDispatchIndexPreflight preflight{};
    preflight.invalidDt = !is_valid_island_solve_dt(dt);

bool should_skip_dispatch_island_index(const ContactIslandGraph& graph, u32 islandIndex, f32 dt) {
    return !preflight_dispatch_island_by_index(graph, islandIndex, dt).can_dispatch();
bool dispatch_solve_island_sleep_guarded(RigidBodySoA& bodies,
    const IslandSolveJob job = extract_island(graph, islandIndex);
    if (should_skip_sleeping_island_solve_job(job, bodies)) {
    return dispatch_solve_island(bodies,

bool dispatch_solve_island(RigidBodySoA& bodies,
                           const ContactIslandGraph& graph,
                           u32 islandIndex,
                           SolverWorkBuffers& workBuffers,
                           const std::vector<DistanceConstraint>& distanceConstraints,
                           f32 dt,
                           f32 contactCompliance,
                           const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (!is_finite_island_solve_dt(dt)) {
        return false;
    }
    const IslandSolveJob job = extract_island(graph, islandIndex);
    if (!should_solve_island(job)) {
        return false;
    }
    return solve_island_job(bodies,
                            *job.island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);
}

bool dispatch_solve_island_job(RigidBodySoA& bodies,
                             const IslandSolveJob& job,
                             SolverWorkBuffers& workBuffers,
                             const std::vector<DistanceConstraint>& distanceConstraints,
                             f32 dt,
                             f32 contactCompliance,
                             const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (!is_valid_island_solve_dt(dt) || !should_solve_island(job) || job.island == nullptr) {
    if (!is_valid_island_solve_dt(dt) || !should_solve_island(job)) {
        return false;
    }
    return solve_island_job(bodies,
                            *job.island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);

bool dispatch_solve_island_combined_guarded(RigidBodySoA& bodies,
                                            const IslandSolveJob& job,
                                            SolverWorkBuffers& workBuffers,
                                            const std::vector<DistanceConstraint>& distanceConstraints,
                                            f32 dt,
                                            f32 contactCompliance,
                                            const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const IslandSolveCombinedPreflight preflight = preflight_island_solve_combined(
        job,
        bodies,
        workBuffers.contactManifolds(),
        dt);
    if (!preflight.can_dispatch() || job.island == nullptr) {
    if (!is_finite_island_solve_dt(dt) || !should_solve_island(job) || job.island == nullptr) {
        return false;
    }
    return solve_island_job(bodies,
                            *job.island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);
}

IslandDispatchResult dispatch_solve_island_job_result(RigidBodySoA& bodies,
                                                      const IslandSolveJob& job,
                                                      SolverWorkBuffers& workBuffers,
                                                      const std::vector<DistanceConstraint>& distanceConstraints,
                                                      f32 dt,
                                                      f32 contactCompliance,
                                                      const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandDispatchResult result{};
    result.islandIndex = job.islandIndex;
    if (!is_finite_island_solve_dt(dt)) {
    result.reason = islandDispatchRejectReason(job, dt);
    if (result.reason != IslandDispatchRejectReason::None) {
        result.skipped = true;
        return result;
    }
    if (!should_solve_island(job) || job.island == nullptr) {
        result.skipped = true;
        result.reason = job.constraintCount == 0u ? IslandDispatchRejectReason::NoConstraints
                                                  : IslandDispatchRejectReason::EmptyJob;
        return result;
    }
    result.solved = dispatch_solve_island_job(bodies,
                                              job,
                                              workBuffers,
                                              distanceConstraints,
                                              dt,
                                              contactCompliance,
                                              invMassFn);
    result.skipped = !result.solved;
}

IslandDispatchResult dispatch_solve_island_result(RigidBodySoA& bodies,
                                                  const ContactIslandGraph& graph,
                                                  u32 islandIndex,
                                                  SolverWorkBuffers& workBuffers,
                                                  const std::vector<DistanceConstraint>& distanceConstraints,
                                                  f32 dt,
                                                  f32 contactCompliance,
                                                  const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandDispatchResult result{};
    result.islandIndex = job.islandIndex;
    if (!is_valid_island_solve_dt(dt)) {
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        result.reason = IslandDispatchRejectReason::OutOfRangeIsland;
        return result;
    if (!should_solve_island(job) || job.island == nullptr) {
    result.solved = dispatch_solve_island_job(bodies,
                                              job,
    result.skipped = !result.solved;

IslandDispatchResult dispatch_solve_island_result(RigidBodySoA& bodies,
                                                  const ContactIslandGraph& graph,
                                                  u32 islandIndex,
    result.islandIndex = islandIndex;
    const IslandSolveJob job = extract_island(graph, islandIndex);
    if (!should_solve_island(job)) {
    result.solved = dispatch_solve_island(bodies,
                                          graph,
                                          islandIndex,

bool dispatch_solve_awake_island(RigidBodySoA& bodies,
                                 const ContactIslandGraph& graph,
                                 u32 islandIndex,
                                 SolverWorkBuffers& workBuffers,
                                 const std::vector<DistanceConstraint>& distanceConstraints,
                                 f32 dt,
                                 f32 contactCompliance,
                                 const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_solve_awake_island_result(bodies,
                                              graph,
                                              islandIndex,
                                              workBuffers,
                                              distanceConstraints,
                                              dt,
                                              contactCompliance,
                                              invMassFn)
        .solved;
}

IslandDispatchResult dispatch_solve_awake_island_result(RigidBodySoA& bodies,
                                                          const ContactIslandGraph& graph,
                                                          u32 islandIndex,
                                                          SolverWorkBuffers& workBuffers,
                                                          const std::vector<DistanceConstraint>& distanceConstraints,
                                                          f32 dt,
                                                          f32 contactCompliance,
                                                          const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandDispatchResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve(bodies, island, dt);
    if (!preflight.can_solve()) {
        result.skipped = true;
        return result;
    }

    return dispatch_solve_island_result(bodies,
                                          graph,
                                          islandIndex,
                                          workBuffers,
                                          distanceConstraints,
                                          dt,
                                          contactCompliance,
                                          invMassFn);
}

IslandDispatchResult dispatch_solve_island_sleep_guarded_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandDispatchResult result{};
    result.islandIndex = islandIndex;
    if (!is_finite_island_solve_dt(dt)) {
        result.skipped = true;
        return result;
    }
    const IslandSolveJob job = extract_island(graph, islandIndex);
    if (!should_solve_island(job) || job.island == nullptr) {
        result.skipped = true;
        return result;
    }
    if (should_skip_sleeping_island(*job.island, bodies)) {
    result.reason = islandDispatchRejectReason(job, dt);
    if (result.reason != IslandDispatchRejectReason::None) {
    if (!should_solve_island(job)) {
        result.skipped = true;
        result.reason = job.constraintCount == 0u ? IslandDispatchRejectReason::NoConstraints
                                                : IslandDispatchRejectReason::EmptyJob;
        return result;
    }
    result.solved = dispatch_solve_island(bodies,
                                          graph,
                                          islandIndex,
                                          workBuffers,
                                          distanceConstraints,
                                          dt,
                                          contactCompliance,
                                          invMassFn);
    result.skipped = !result.solved;
    return result;
}

u32 dispatch_all_awake_islands(RigidBodySoA& bodies,
                               const ContactIslandGraph& graph,
                               SolverWorkBuffers& workBuffers,
                               const std::vector<DistanceConstraint>& distanceConstraints,
                               f32 dt,
                               f32 contactCompliance,
                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_all_awake_islands_result(bodies,
                                             graph,
                                             workBuffers,
                                             distanceConstraints,
                                             dt,
                                             contactCompliance,
                                             invMassFn)
        .solvedCount;
}

IslandBatchDispatchResult dispatch_all_awake_islands_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandDispatchPreflight preflight = preflight_island_dispatch(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    for (u32 islandIndex : collect_awake_island_indices(graph, bodies)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_sleep_guarded_result(
            bodies,
            graph,
            islandIndex,
            workBuffers,
            distanceConstraints,
            dt,
            contactCompliance,
            invMassFn);
        if (dispatchResult.solved) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;
        }
    }
    return result;
}

u32 dispatch_all_islands(RigidBodySoA& bodies,
    return dispatch_all_islands_result(bodies,
                                       invMassFn)
        .solvedCount;

IslandBatchDispatchResult dispatch_all_islands_result(
    RigidBodySoA& bodies,
    IslandBatchDispatchResult result{};
    const IslandDispatchPreflight preflight = preflight_island_dispatch(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skippedCount = result.dispatchableCount;

    for (u32 islandIndex : collect_dispatchable_island_indices(graph)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_result(bodies,
        if (dispatchResult.solved) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;
IslandSolveDispatchResult dispatch_solve_island_result(RigidBodySoA& bodies,
    IslandSolveDispatchResult result{};

    if (should_skip_island_solve(job)) {

    result.skipped = false;
    result.solved = solve_island_job(bodies,
    return result;
                         const ContactIslandGraph& graph,
                         SolverWorkBuffers& workBuffers,
                         const std::vector<DistanceConstraint>& distanceConstraints,
                         f32 dt,
                         f32 contactCompliance,
                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const IslandSolveDispatchPlan plan = build_island_solve_dispatch_plan(graph);
    return dispatch_islands_from_plan(bodies,
                                      graph,
                                      plan,
    return dispatch_all_islands_batch(bodies,
                                      workBuffers,
                                      distanceConstraints,
                                      dt,
                                      contactCompliance,
                                      invMassFn);
}

IslandDispatchBatchResult dispatch_all_islands_batch(RigidBodySoA& bodies,
    IslandDispatchBatchResult batch{};
    const IslandSolvePreflight preflight = preflight_island_solve(graph);
    const std::vector<IslandSolveJob> jobs = extract_island_jobs(graph);
    return dispatch_island_jobs_result(bodies,
                                       jobs,

IslandBatchDispatchResult dispatch_island_jobs_result(
    const std::vector<IslandSolveJob>& jobs,
    result.invalidDt = preflight.invalidDt;
        return batch;

        const IslandDispatchResult result = dispatch_solve_island_result(bodies,
                                                                         islandIndex,
        if (result.solved) {
            ++batch.solvedCount;
        } else if (result.skipped) {
            ++batch.skippedCount;
    const std::vector<IslandSolveJob> dispatchableJobs = filter_dispatchable_jobs(jobs);
    for (const IslandSolveJob& job : dispatchableJobs) {
        if (dispatch_solve_island_job(bodies,
                                      job,
                                      invMassFn)) {

IslandBatchDispatchResult dispatch_islands_at_indices_result(
    const std::vector<u32>& islandIndices,
IslandSolveableStats compute_island_solveable_stats(
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandSolveableStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (job.empty) {
            ++stats.emptyCount;
            continue;
        if (should_skip_island_sleep_solve(island, bodies)) {
            ++stats.allSleepingCount;
        const IslandConstraintSolvePreflight solvePreflight =
            preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);
        if (!solvePreflight.refs.can_solve()) {
            ++stats.staleRefsCount;
        if (!solvePreflight.bodies.can_solve()) {
            ++stats.noMovableBodiesCount;
        ++stats.solveableCount;
    return stats;

IslandSolveableGraphPreflight preflight_island_solveable_graph(
    IslandSolveableGraphPreflight preflight{};
    preflight.stats = compute_island_solveable_stats(graph, bodies, contacts, distanceConstraints);
    preflight.skipped = preflight.stats.solveableCount == 0u;
    return preflight;

bool should_skip_island_solveable_graph(const ContactIslandGraph& graph,
    return !preflight_island_solveable_graph(graph, bodies, contacts, distanceConstraints).has_solveable();

std::vector<u32> collect_solveable_island_indices(
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        if (!should_solve_island(job)) {
        if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints)) {
        indices.push_back(islandIndex);
    return indices;

IslandBatchDispatchResult dispatch_all_solveable_islands_result(
bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,
                                             u32 islandIndex,
    return dispatch_solve_island_with_wake_result(bodies,
        .solved;

IslandDispatchResult dispatch_solve_island_with_wake_result(RigidBodySoA& bodies,
    IslandDispatchResult result{};
    result.islandIndex = islandIndex;
    if (!is_valid_island_solve_dt(dt)) {
        result.skipped = true;

    if (!should_solve_island(job) || job.island == nullptr) {

    wake_island_sleepers_guarded(bodies, *job.island);
    result.solved = solve_island_job_guarded(bodies,
                                             *job.island,
    result.skipped = !result.solved;

    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    if (!is_valid_island_solve_dt(dt)) {
        result.skipped = true;
        return result;
    }

    for (u32 islandIndex : islandIndices) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!should_solve_island(job)) {
            ++result.skippedCount;
            continue;
        ++result.dispatchableCount;
        if (dispatch_solve_island_job(bodies,
                                      job,
                                      workBuffers,
                                      distanceConstraints,
                                      dt,
                                      contactCompliance,
                                      invMassFn)) {
    const IslandDispatchSleepPreflight preflight = preflight_island_dispatch_sleep(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skippedCount = result.dispatchableCount;

    wake_all_island_sleepers_guarded(bodies, graph);

    for (u32 islandIndex : collect_solveable_island_indices(
             graph, bodies, workBuffers.contactManifolds(), distanceConstraints)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_result(bodies,
                                                                               graph,
                                                                               islandIndex,
                                                                               invMassFn);
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    const IslandSolveableGraphPreflight preflight =
        preflight_island_solveable_graph(graph, bodies, contacts, distanceConstraints, dt);
    result.dispatchableCount = preflight.stats.solveableCount;

    for (u32 islandIndex :
         collect_solveable_island_indices(graph, bodies, contacts, distanceConstraints)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_with_wake_result(
            bodies,
        if (dispatchResult.solved) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;
        }
    }
    return result;
}

u32 dispatch_islands_at_indices(RigidBodySoA& bodies,
                                const ContactIslandGraph& graph,
                                const std::vector<u32>& islandIndices,
                                SolverWorkBuffers& workBuffers,
                                const std::vector<DistanceConstraint>& distanceConstraints,
                                f32 dt,
                                f32 contactCompliance,
                                const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_islands_at_indices_result(bodies,
                                              graph,
                                              islandIndices,
                                              workBuffers,
                                              distanceConstraints,
                                              dt,
                                              contactCompliance,
                                              invMassFn)
        .solvedCount;
u32 dispatch_island_jobs(RigidBodySoA& bodies,
                         const std::vector<IslandSolveJob>& jobs,
    if (!is_valid_island_solve_dt(dt)) {
        return 0u;
    }

    u32 solvedCount = 0u;
    for (const IslandSolveJob& job : jobs) {
        if (!should_solve_island(job)) {
            continue;
        if (dispatch_solve_island(bodies,
                                  job.islandIndex,
                                  invMassFn)) {
            ++solvedCount;
    return solvedCount;
bool dispatch_solve_island_job(RigidBodySoA& bodies,
                             const IslandSolveJob& job,
    return dispatch_solve_island_job_result(bodies,
                                            job,
        .solved;

IslandDispatchResult dispatch_solve_island_job_result(RigidBodySoA& bodies,
    IslandDispatchResult result{};
    result.islandIndex = job.islandIndex;
    const IslandDispatchJobPreflight preflight = preflight_dispatch_island_job(job, dt);
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        return result;
    result.solved = solve_island_job(bodies,
                                     *job.island,
                                     invMassFn);
    result.skipped = !result.solved;
u32 dispatch_all_island_jobs(RigidBodySoA& bodies,
    return dispatch_all_island_jobs_result(bodies,
                                           jobs,

IslandBatchDispatchResult dispatch_all_island_jobs_result(
    RigidBodySoA& bodies,
    IslandBatchDispatchResult result{};

        ++result.dispatchableCount;
        const IslandDispatchResult dispatchResult = dispatch_solve_island_job_result(bodies,
        if (dispatchResult.solved) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;

    if (result.dispatchableCount == 0u) {
u32 dispatch_all_solveable_islands_guarded(
    return dispatch_all_solveable_islands_result(bodies,

void per_pair_delta_application(RigidBodySoA& bodies,
                                SolverWorkBuffers& workBuffers,
                                u32 bodyA,
                                u32 bodyB,
                                f32 invMassA,
                                f32 invMassB,
                                const narrowphase::ContactManifold& contact,
                                f32 dt,
                                f32 contactCompliance,
                                f32& lambda) {
    workBuffers.clearPositionDeltasForBodies(bodyA, bodyB);
    accumulateContactCorrection(bodies,
                                contact,
                                invMassA,
                                invMassB,
                                dt,
                                contactCompliance,
                                lambda,
                                workBuffers.positionDeltas());
    workBuffers.applyPositionDeltas(bodies);
}

void per_pair_delta_application(RigidBodySoA& bodies,
                                SolverWorkBuffers& workBuffers,
                                u32 bodyA,
                                u32 bodyB,
                                f32 invMassA,
                                f32 invMassB,
                                const DistanceConstraint& constraint,
                                f32 dt,
                                f32& lambda) {
    workBuffers.clearPositionDeltasForBodies(bodyA, bodyB);
    accumulateDistanceSpringCorrection(bodies,
                                       constraint,
                                       invMassA,
                                       invMassB,
                                       dt,
                                       lambda,
                                       workBuffers.positionDeltas());
    workBuffers.applyPositionDeltas(bodies);
}

bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (!is_finite_island_solve_dt(dt)) {
    if (!is_valid_island_solve_dt(dt) || !island_has_constraints(island)) {
        return false;
    }

    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints)) {
        return false;
    }
    if (should_skip_island_sleep_solve(island, bodies)) {
        return false;
    }

    return solve_island_job(bodies,
                            island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);
}

bool solve_island_job(RigidBodySoA& bodies,
                      const ContactIslandGraph::Island& island,
                      SolverWorkBuffers& workBuffers,
                      const std::vector<DistanceConstraint>& distanceConstraints,
                      f32 dt,
                      f32 contactCompliance,
                      const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (!is_finite_island_solve_dt(dt) || !island_has_constraints(island)) {
        return false;
    }

    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints)) {
        return false;
    }
    if (should_skip_island_solve_sleeping(island, bodies)) {
    if (should_skip_island_body_refs(island, bodies)) {
    if (should_skip_island_solve_for_sleep(island, bodies)) {
    if (should_skip_solve_sleeping_island(island, bodies)) {
        return false;
    }

    workBuffers.clearPositionDeltasForIslandBodies(island.bodyIndices);

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid) {
            continue;
        }
        f32& lambda = workBuffers.contactLambda(contactIndex);
        per_pair_delta_application(bodies,
                                 workBuffers,
                                 contact.bodyA,
                                 contact.bodyB,
                                 invMassFn(bodies, contact.bodyA),
                                 invMassFn(bodies, contact.bodyB),
                                 contact,
                                 dt,
                                 contactCompliance,
                                 lambda);
    }

    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceConstraints.size()) {
            continue;
        }
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        f32& lambda = workBuffers.distanceLambda(distanceIndex);
        per_pair_delta_application(bodies,
                                 workBuffers,
                                 constraint.bodyA,
                                 constraint.bodyB,
                                 invMassFn(bodies, constraint.bodyA),
                                 invMassFn(bodies, constraint.bodyB),
                                 constraint,
                                 dt,
                                 lambda);
    }

    return true;
}

bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve(bodies, island, dt);
    if (!preflight.can_solve()) {
bool solve_island_job_sleep_guarded(RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    if (should_skip_island_solve_pass(island, bodies, contacts, distanceConstraints)) {
    if (!is_valid_island_solve_dt(dt)) {
        return false;
    }
    if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints)) {
    const IslandExtendedSolvePreflight preflight =
        preflight_island_extended_solve(island, bodies, contacts, distanceConstraints, dt);
        return false;
    }
    return solve_island_job(bodies,
                            island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);
}

bool dispatch_solve_island_sleep_guarded(RigidBodySoA& bodies,
                                         const ContactIslandGraph& graph,
                                         u32 islandIndex,
                                         SolverWorkBuffers& workBuffers,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         f32 dt,
                                         f32 contactCompliance,
                                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_solve_island_sleep_guarded_result(bodies,
                                                      graph,
                                                      islandIndex,
                                                      workBuffers,
                                                      distanceConstraints,
                                                      dt,
                                                      contactCompliance,
                                                      invMassFn)
        .solved;
}

IslandDispatchResult dispatch_solve_island_sleep_guarded_result(
    RigidBodySoA& bodies,
    IslandDispatchResult result{};
    result.islandIndex = islandIndex;
    if (!is_valid_island_solve_dt(dt)) {
        result.skipped = true;
        return result;
    const IslandSolveJob job = extract_island(graph, islandIndex);
    if (!should_solve_island(job) || job.island == nullptr) {
    if (should_skip_solve_sleeping_island(*job.island, bodies)) {
    result.solved = dispatch_solve_island(bodies,
                                          invMassFn);
    result.skipped = !result.solved;

bool solve_island_job_with_wake_guarded(RigidBodySoA& bodies,
                                        const ContactIslandGraph::Island& island,
    wake_island_sleepers_guarded(bodies, island);
    return solve_island_job_guarded(bodies,
                                    island,

void frame_lambda_warm_start(SolverWorkBuffers& workBuffers,
                             const std::vector<DistanceConstraint>& distanceConstraints,
                             const std::vector<f32>& priorDistanceLambdas,
                             const std::vector<f32>& priorContactLambdas) {
    if (!preflight_frame_lambda_warm_start(distanceConstraints, priorDistanceLambdas, priorContactLambdas)) {
        return;
    }

    workBuffers.clearLambdas();
    for (u32 distanceIndex = 0; distanceIndex < distanceConstraints.size(); ++distanceIndex) {
        if (distanceIndex < priorDistanceLambdas.size()) {
            workBuffers.seedDistanceLambda(distanceIndex, priorDistanceLambdas[distanceIndex]);
        }
    }
    for (u32 contactIndex = 0; contactIndex < priorContactLambdas.size(); ++contactIndex) {
        const f32 prior = priorContactLambdas[contactIndex];
        if (prior == 0.f) {
            continue;
        }
        f32& lambda = workBuffers.contactLambda(contactIndex);
        if (lambda == 0.f) {
            lambda = prior;
        }
    }
}

FrameWarmStartPreflight preflight_frame_lambda_warm_start(
    const std::vector<DistanceConstraint>& distanceConstraints,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    FrameWarmStartPreflight preflight{};
    preflight.distanceSlotCount = static_cast<u32>(distanceConstraints.size());
    preflight.contactSlotCount = static_cast<u32>(priorContactLambdas.size());

    for (u32 distanceIndex = 0; distanceIndex < distanceConstraints.size(); ++distanceIndex) {
        if (distanceIndex < priorDistanceLambdas.size()) {
            ++preflight.priorDistanceCoverage;
        }
    }
    for (u32 contactIndex = 0; contactIndex < priorContactLambdas.size(); ++contactIndex) {
        if (priorContactLambdas[contactIndex] != 0.f) {
            ++preflight.priorContactCoverage;
        }
    }

    preflight.skipped = preflight.priorDistanceCoverage == 0u && preflight.priorContactCoverage == 0u;
    return preflight;
}

bool should_skip_frame_warm_start(const std::vector<f32>& priorDistanceLambdas,
                                  const std::vector<f32>& priorContactLambdas) {
    return preflight_frame_lambda_warm_start({}, priorDistanceLambdas, priorContactLambdas).skipped;
}

bool frame_lambda_warm_start_guarded(SolverWorkBuffers& workBuffers,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     const std::vector<f32>& priorDistanceLambdas,
                                     const std::vector<f32>& priorContactLambdas) {
    const FrameWarmStartPreflight preflight =
        preflight_frame_lambda_warm_start(distanceConstraints, priorDistanceLambdas, priorContactLambdas);
    if (!preflight.can_warm_start()) {
        workBuffers.clearLambdas();
        return false;
    }
    frame_lambda_warm_start(workBuffers, distanceConstraints, priorDistanceLambdas, priorContactLambdas);
    return true;
}

IslandWarmStartPreflight preflight_warm_start_island(const ContactIslandGraph::Island& island,
                                                     const std::vector<f32>& priorDistanceLambdas,
                                                     const std::vector<f32>& priorContactLambdas) {
    IslandWarmStartPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());
    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());

    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex < priorDistanceLambdas.size()) {
            ++preflight.priorDistanceCoverage;
        }
    }
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex < priorContactLambdas.size() && priorContactLambdas[contactIndex] != 0.f) {
            ++preflight.priorContactCoverage;
        }
    }
    return preflight;
}

IslandWarmStartPreflight preflight_warm_start_island_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const std::vector<f32>& priorDistanceLambdas,
                                                              const std::vector<f32>& priorContactLambdas) {
    IslandWarmStartPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_warm_start_island(graph.island(islandIndex), priorDistanceLambdas, priorContactLambdas);
}

IslandWarmStartStats compute_island_warm_start_stats(const ContactIslandGraph& graph,
                                                     const std::vector<f32>& priorDistanceLambdas,
                                                     const std::vector<f32>& priorContactLambdas) {
    IslandWarmStartStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandWarmStartPreflight preflight =
            preflight_warm_start_island(graph.island(islandIndex), priorDistanceLambdas, priorContactLambdas);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;
        } else {
            ++stats.noPriorDataCount;
        }
    }
    return stats;
}

u32 count_warm_startable_islands(const ContactIslandGraph& graph,
                                 const std::vector<f32>& priorDistanceLambdas,
                                 const std::vector<f32>& priorContactLambdas) {
    return compute_island_warm_start_stats(graph, priorDistanceLambdas, priorContactLambdas).warmStartableCount;
}

bool has_warm_startable_islands(const ContactIslandGraph& graph,
                                const std::vector<f32>& priorDistanceLambdas,
                                const std::vector<f32>& priorContactLambdas) {
    return count_warm_startable_islands(graph, priorDistanceLambdas, priorContactLambdas) > 0u;
}

IslandWarmStartGraphPreflight preflight_warm_start_graph(const ContactIslandGraph& graph,
                                                         const std::vector<f32>& priorDistanceLambdas,
                                                         const std::vector<f32>& priorContactLambdas) {
    IslandWarmStartGraphPreflight preflight{};
    preflight.stats = compute_island_warm_start_stats(graph, priorDistanceLambdas, priorContactLambdas);
    preflight.skipped = preflight.stats.warmStartableCount == 0u;
    return preflight;
}

bool should_skip_warm_start_graph(const ContactIslandGraph& graph,
                                  const std::vector<f32>& priorDistanceLambdas,
                                  const std::vector<f32>& priorContactLambdas) {
    return !has_warm_startable_islands(graph, priorDistanceLambdas, priorContactLambdas);
}

std::vector<u32> collect_warm_startable_island_indices(const ContactIslandGraph& graph,
                                                       const std::vector<f32>& priorDistanceLambdas,
                                                       const std::vector<f32>& priorContactLambdas) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandWarmStartPreflight preflight =
            preflight_warm_start_island(graph.island(islandIndex), priorDistanceLambdas, priorContactLambdas);
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

IslandWarmStartJob extract_warm_start_job(const ContactIslandGraph& graph,
                                          u32 islandIndex,
                                          const std::vector<f32>& priorDistanceLambdas,
                                          const std::vector<f32>& priorContactLambdas) {
    IslandWarmStartJob job{};
    if (!island_index_valid(graph, islandIndex)) {
        return job;
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    const IslandWarmStartPreflight preflight =
        preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);

    job.islandIndex = islandIndex;
    job.island = &island;
    job.empty = preflight.skipped;
    job.ownedDistanceCount = preflight.ownedDistanceCount;
    job.ownedContactCount = preflight.ownedContactCount;
    job.canWarmStart = preflight.can_warm_start();
    return job;
}

std::vector<IslandWarmStartJob> extract_warm_start_jobs(const ContactIslandGraph& graph,
                                                        const std::vector<f32>& priorDistanceLambdas,
                                                        const std::vector<f32>& priorContactLambdas) {
    const u32 count = graph.islandCount();
    std::vector<IslandWarmStartJob> jobs;
    jobs.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        jobs.push_back(extract_warm_start_job(graph, islandIndex, priorDistanceLambdas, priorContactLambdas));
    }
    return jobs;
}

bool should_warm_start_island(const IslandWarmStartJob& job) {
    return !job.empty && job.island != nullptr && job.canWarmStart;
}

bool should_skip_warm_start_island_job(const IslandWarmStartJob& job) {
    return !should_warm_start_island(job);
}

std::vector<IslandWarmStartJob> filter_warm_startable_jobs(const std::vector<IslandWarmStartJob>& jobs) {
    std::vector<IslandWarmStartJob> warmStartable;
    warmStartable.reserve(jobs.size());
    for (const IslandWarmStartJob& job : jobs) {
        if (should_warm_start_island(job)) {
            warmStartable.push_back(job);
        }
    }
    return warmStartable;
}

bool should_skip_warm_start_island(const ContactIslandGraph::Island& island) {
    return !island_has_constraints(island);
}

bool should_skip_warm_start_island_index(const ContactIslandGraph& graph, u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    return should_skip_warm_start_island(graph.island(islandIndex));
}

bool warm_start_island_lambdas_guarded(SolverWorkBuffers& workBuffers,
                                       const ContactIslandGraph::Island& island,
                                       const std::vector<f32>& priorDistanceLambdas,
                                       const std::vector<f32>& priorContactLambdas) {
    const IslandWarmStartPreflight preflight =
        preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    if (!preflight.can_warm_start()) {
        return false;
    }
    warm_start_island_lambdas(workBuffers, island, priorDistanceLambdas, priorContactLambdas);
    return true;
}

bool warm_start_island_lambdas_by_index_guarded(SolverWorkBuffers& workBuffers,
                                                const ContactIslandGraph& graph,
                                                u32 islandIndex,
                                                const std::vector<f32>& priorDistanceLambdas,
                                                const std::vector<f32>& priorContactLambdas) {
    return warm_start_island_lambdas_result(workBuffers,
                                            graph,
                                            islandIndex,
                                            priorDistanceLambdas,
                                            priorContactLambdas)
        .warmed;
}

IslandWarmStartResult warm_start_island_lambdas_result(SolverWorkBuffers& workBuffers,
                                                       const ContactIslandGraph& graph,
                                                       u32 islandIndex,
                                                       const std::vector<f32>& priorDistanceLambdas,
                                                       const std::vector<f32>& priorContactLambdas) {
    IslandWarmStartResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    const IslandWarmStartPreflight preflight =
        preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        return result;
    }

    warm_start_island_lambdas(workBuffers, island, priorDistanceLambdas, priorContactLambdas);
    result.warmed = true;
    return result;
}

u32 warm_start_graph_lambdas_guarded(SolverWorkBuffers& workBuffers,
                                     const ContactIslandGraph& graph,
                                     const std::vector<f32>& priorDistanceLambdas,
                                     const std::vector<f32>& priorContactLambdas) {
    return warm_start_all_islands_guarded(workBuffers, graph, priorDistanceLambdas, priorContactLambdas);
}

bool warm_start_island_lambdas_by_index_guarded(SolverWorkBuffers& workBuffers,
                                                const ContactIslandGraph& graph,
                                                u32 islandIndex,
                                                const std::vector<f32>& priorDistanceLambdas,
                                                const std::vector<f32>& priorContactLambdas) {
    return warm_start_island_lambdas_result(workBuffers,
                                            graph,
                                            islandIndex,
                                            priorDistanceLambdas,
                                            priorContactLambdas)
        .warmed;

IslandWarmStartResult warm_start_island_lambdas_result(SolverWorkBuffers& workBuffers,
    IslandWarmStartResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;
    return warm_start_all_islands_result(workBuffers, graph, priorDistanceLambdas, priorContactLambdas)
        .warmedCount;

GraphWarmStartPreflight preflight_graph_warm_start(const ContactIslandGraph& graph,
    GraphWarmStartPreflight preflight{};
    const IslandSolveStats stats = compute_island_solve_stats(graph);
    preflight.dispatchableCount = stats.dispatchableCount;
    preflight.skippedEmptyCount = stats.emptyCount;

    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandWarmStartPreflight islandPreflight =
            preflight_warm_start_island(graph.island(islandIndex), priorDistanceLambdas, priorContactLambdas);
        if (islandPreflight.skipped) {
            continue;
        preflight.priorDistanceCoverage += islandPreflight.priorDistanceCoverage;
        preflight.priorContactCoverage += islandPreflight.priorContactCoverage;

    preflight.skipped = should_skip_frame_warm_start(priorDistanceLambdas, priorContactLambdas);
    return preflight;

bool should_skip_graph_warm_start(const std::vector<f32>& priorDistanceLambdas,
    return should_skip_frame_warm_start(priorDistanceLambdas, priorContactLambdas);

IslandWarmStartBatchResult warm_start_all_islands_result(
    SolverWorkBuffers& workBuffers,
    IslandWarmStartBatchResult result{};
    const GraphWarmStartPreflight preflight =
        preflight_graph_warm_start(graph, priorDistanceLambdas, priorContactLambdas);
    result.skippedEmptyCount = preflight.skippedEmptyCount;
    if (!preflight.can_warm_start()) {
        result.skippedNoPriorCount = preflight.dispatchableCount;

        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (should_skip_warm_start_island(island)) {
        if (warm_start_island_lambdas_guarded(workBuffers,
                                              island,
                                              priorContactLambdas)) {
            ++result.warmedCount;
        } else {
            ++result.skippedNoPriorCount;

u32 warm_start_dispatchable_islands_guarded(SolverWorkBuffers& workBuffers,
    u32 warmedCount = 0u;
    for (u32 islandIndex : collect_dispatchable_island_indices(graph)) {
                                              graph.island(islandIndex),
            ++warmedCount;
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    const IslandWarmStartPreflight preflight =
        preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        return result;
    }

    warm_start_island_lambdas(workBuffers, island, priorDistanceLambdas, priorContactLambdas);
    result.warmed = true;
    return result;
}

u32 warm_start_all_islands_guarded(SolverWorkBuffers& workBuffers,
                                   const ContactIslandGraph& graph,
                                   const std::vector<f32>& priorDistanceLambdas,
                                   const std::vector<f32>& priorContactLambdas) {
    return warm_start_all_islands_result(workBuffers, graph, priorDistanceLambdas, priorContactLambdas).warmedCount;
}

IslandBatchWarmStartResult warm_start_all_islands_result(SolverWorkBuffers& workBuffers,
                                                         const ContactIslandGraph& graph,
                                                         const std::vector<f32>& priorDistanceLambdas,
                                                         const std::vector<f32>& priorContactLambdas) {
    IslandBatchWarmStartResult result{};
IslandWarmStartBatchResult warm_start_all_islands_result(SolverWorkBuffers& workBuffers,
IslandWarmStartBatchResult warm_start_all_islands_result(
    SolverWorkBuffers& workBuffers,
    return warm_start_all_islands_result(workBuffers, graph, priorDistanceLambdas, priorContactLambdas)
        .warmedCount;


    IslandWarmStartBatchResult result{};
    const IslandWarmStartGraphPreflight preflight =
        preflight_warm_start_graph(graph, priorDistanceLambdas, priorContactLambdas);
    result.warmStartableCount = preflight.stats.warmStartableCount;
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        result.skippedCount = result.warmStartableCount;
        return result;
        result.skippedCount = preflight.stats.totalIslands;
    }

    for (u32 islandIndex : collect_warm_startable_island_indices(graph, priorDistanceLambdas, priorContactLambdas)) {
        const IslandWarmStartResult warmResult =
            warm_start_island_lambdas_result(workBuffers, graph, islandIndex, priorDistanceLambdas, priorContactLambdas);
            warm_start_island_lambdas_result(workBuffers,
                                            graph,
                                            islandIndex,
                                            priorDistanceLambdas,
                                            priorContactLambdas);
        if (warmResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;
    return result;

IslandContactImpulseWarmStartPreflight preflight_warm_start_contact_impulses(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartPreflight preflight{};
    preflight.invalidDt = !is_valid_warm_start_dt(dt);
    if (should_skip_warm_start_island(island)) {
        preflight.skipped = true;
        return preflight;

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        if (contacts[contactIndex].warmNormalImpulse != 0.f) {
            ++preflight.nonZeroImpulseCount;

IslandContactImpulseWarmStartPreflight preflight_warm_start_contact_impulses_by_index(
    u32 islandIndex,
    if (!island_index_valid(graph, islandIndex)) {
    return preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);

IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandContactImpulseWarmStartPreflight preflight =
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;
        } else {
            ++stats.zeroImpulseCount;
        }
    }
    return stats;
}

u32 count_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,
                                                 const std::vector<narrowphase::ContactManifold>& contacts,
                                                 f32 dt) {
    return compute_island_contact_impulse_warm_start_stats(graph, contacts, dt).warmStartableCount;
}

bool has_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,
                                                const std::vector<narrowphase::ContactManifold>& contacts,
                                                f32 dt) {
    return count_warm_startable_contact_impulse_islands(graph, contacts, dt) > 0u;
}

IslandContactImpulseWarmStartGraphPreflight preflight_warm_start_contact_impulses_graph(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartGraphPreflight preflight{};
    preflight.invalidDt = !is_valid_warm_start_dt(dt);
    preflight.stats = compute_island_contact_impulse_warm_start_stats(graph, contacts, dt);
    preflight.skipped = preflight.stats.warmStartableCount == 0u;
    return preflight;
}

bool should_skip_warm_start_contact_impulses_graph(const ContactIslandGraph& graph,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   f32 dt) {
    return !has_warm_startable_contact_impulse_islands(graph, contacts, dt) || !is_valid_warm_start_dt(dt);
}

std::vector<u32> collect_warm_startable_contact_impulse_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandContactImpulseWarmStartPreflight preflight =
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandContactImpulseWarmStartPreflight preflight =
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;
        } else {
            ++stats.noImpulseCount;
        }
    }
    return stats;
}

u32 count_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,
                                                 const std::vector<narrowphase::ContactManifold>& contacts,
                                                 f32 dt) {
    return compute_island_contact_impulse_warm_start_stats(graph, contacts, dt).warmStartableCount;
}

bool has_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,
                                                const std::vector<narrowphase::ContactManifold>& contacts,
                                                f32 dt) {
    return count_warm_startable_contact_impulse_islands(graph, contacts, dt) > 0u;
}

IslandContactImpulseWarmStartGraphPreflight preflight_warm_start_contact_impulses_graph(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartGraphPreflight preflight{};
    preflight.invalidDt = !is_valid_warm_start_dt(dt);
    preflight.stats = compute_island_contact_impulse_warm_start_stats(graph, contacts, dt);
    preflight.skipped = preflight.invalidDt || preflight.stats.warmStartableCount == 0u;
    return preflight;
}

bool should_skip_warm_start_contact_impulses_graph(const ContactIslandGraph& graph,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   f32 dt) {
    return !preflight_warm_start_contact_impulses_graph(graph, contacts, dt).can_warm_start();
}

std::vector<u32> collect_contact_impulse_warm_startable_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandContactImpulseWarmStartPreflight preflight =
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island, f32 dt) {
    if (!is_valid_warm_start_dt(dt) || should_skip_warm_start_island(island)) {
        return true;
    return island.contactIndices.empty();

bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island,
    return !preflight_warm_start_contact_impulses(island, contacts, dt).can_warm_start();

IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(
    IslandContactImpulseWarmStartStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandContactImpulseWarmStartPreflight preflight =
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;
            ++stats.noImpulseCount;
    return stats;

u32 count_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,
    return compute_island_contact_impulse_warm_start_stats(graph, contacts, dt).warmStartableCount;

bool has_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,
    return count_warm_startable_contact_impulse_islands(graph, contacts, dt) > 0u;

IslandContactImpulseWarmStartGraphPreflight preflight_warm_start_contact_impulses_graph(
    IslandContactImpulseWarmStartGraphPreflight preflight{};
    preflight.stats = compute_island_contact_impulse_warm_start_stats(graph, contacts, dt);
    preflight.skipped = preflight.stats.warmStartableCount == 0u;

bool should_skip_warm_start_contact_impulses_graph(const ContactIslandGraph& graph,
    return !preflight_warm_start_contact_impulses_graph(graph, contacts, dt).can_warm_start();

std::vector<u32> collect_warm_startable_contact_impulse_island_indices(
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
    return indices;

bool should_skip_warm_start_contact_impulses_index(const ContactIslandGraph& graph,
    return should_skip_warm_start_contact_impulses(graph.island(islandIndex), dt);

IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandContactImpulseWarmStartPreflight preflight =
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;
        } else {
            ++stats.noImpulseCount;
        }
    }
    return stats;
}

u32 count_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,
                                                 const std::vector<narrowphase::ContactManifold>& contacts,
                                                 f32 dt) {
    return compute_island_contact_impulse_warm_start_stats(graph, contacts, dt).warmStartableCount;
}

bool has_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,
                                                const std::vector<narrowphase::ContactManifold>& contacts,
                                                f32 dt) {
    return count_warm_startable_contact_impulse_islands(graph, contacts, dt) > 0u;
}

IslandContactImpulseWarmStartGraphPreflight preflight_warm_start_contact_impulses_graph(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartGraphPreflight preflight{};
    preflight.invalidDt = !is_valid_warm_start_dt(dt);
    preflight.stats = compute_island_contact_impulse_warm_start_stats(graph, contacts, dt);
    preflight.skipped = preflight.stats.warmStartableCount == 0u;
    return preflight;
}

bool should_skip_warm_start_contact_impulses_graph(const ContactIslandGraph& graph,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   f32 dt) {
    return !preflight_warm_start_contact_impulses_graph(graph, contacts, dt).can_warm_start();
}

std::vector<u32> collect_warm_startable_contact_impulse_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandContactImpulseWarmStartPreflight preflight =
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

IslandCombinedWarmStartPreflight preflight_warm_start_combined_island(
    f32 dt,
    IslandCombinedWarmStartPreflight preflight{};

    preflight.lambdas = preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    preflight.impulses = preflight_warm_start_contact_impulses(island, contacts, dt);


    return preflight_warm_start_contact_impulses(island, {}, dt).skipped ||
           !is_valid_warm_start_dt(dt);

bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,
                                                         const ContactIslandGraph& graph,
        return false;
    return warm_start_island_contact_impulses_guarded(workBuffers, graph.island(islandIndex), contacts, dt);

u32 warm_start_graph_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
    if (!is_valid_warm_start_dt(dt)) {
        return 0u;

    u32 warmedCount = 0u;
        const IslandWarmStartResult result =
        if (result.warmed) {
            ++warmedCount;
    return warmedCount;

u32 warm_start_all_islands_guarded(SolverWorkBuffers& workBuffers,
                                   const std::vector<f32>& priorDistanceLambdas,
                                   const std::vector<f32>& priorContactLambdas) {
    return warm_start_all_islands_result(workBuffers, graph, priorDistanceLambdas, priorContactLambdas).warmedCount;

IslandWarmStartBatchResult warm_start_all_islands_result(
    SolverWorkBuffers& workBuffers,
    IslandWarmStartBatchResult result{};
    const IslandWarmStartGraphPreflight preflight =
        preflight_warm_start_graph(graph, priorDistanceLambdas, priorContactLambdas);
    result.warmStartableCount = preflight.stats.warmStartableCount;
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        result.skippedCount = result.warmStartableCount;

        const IslandWarmStartResult islandResult =
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (warm_start_island_contact_impulses_guarded(workBuffers, island, contacts, dt)) {

IslandWarmStartBatchResult warm_start_all_islands_result(SolverWorkBuffers& workBuffers,


        if (islandResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;
        }

bool warm_start_island_job_guarded(SolverWorkBuffers& workBuffers,
                                   const IslandWarmStartJob& job,
    if (!should_warm_start_island(job)) {
    warm_start_island_lambdas(workBuffers, *job.island, priorDistanceLambdas, priorContactLambdas);
IslandContactImpulseWarmStartPreflight preflight_contact_impulse_warm_start_island(
    const std::vector<narrowphase::ContactManifold>& contacts) {
    if (!island_has_constraints(island)) {

        if (contactIndex < contacts.size() && contacts[contactIndex].warmNormalImpulse != 0.f) {
            ++preflight.nonZeroImpulseCoverage;

IslandContactImpulseWarmStartPreflight preflight_contact_impulse_warm_start_island_by_index(
    return preflight_contact_impulse_warm_start_island(graph.island(islandIndex), contacts);

bool should_skip_contact_impulse_warm_start_island(const ContactIslandGraph::Island& island) {
    return should_skip_warm_start_island(island);

bool should_skip_contact_impulse_warm_start_island_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_contact_impulse_warm_start_island(graph.island(islandIndex));

IslandCombinedWarmStartPreflight preflight_warm_start_island_combined(
        preflight.lambdas.skipped = true;
        preflight.impulses.skipped = true;

    preflight.impulses = preflight_contact_impulse_warm_start_island(island, contacts);

            preflight_contact_impulse_warm_start_island(graph.island(islandIndex), contacts);
            ++stats.noImpulseDataCount;

u32 count_contact_impulse_warm_startable_islands(
    return compute_island_contact_impulse_warm_start_stats(graph, contacts).warmStartableCount;

bool has_contact_impulse_warm_startable_islands(
    return count_contact_impulse_warm_startable_islands(graph, contacts) > 0u;

IslandContactImpulseWarmStartGraphPreflight preflight_contact_impulse_warm_start_graph(
    preflight.stats = compute_island_contact_impulse_warm_start_stats(graph, contacts);

IslandContactImpulseDispatchPreflight preflight_contact_impulse_dispatch(
    IslandContactImpulseDispatchPreflight preflight{};
    preflight.impulses = preflight_contact_impulse_warm_start_graph(graph, contacts);
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.skipped = preflight.impulses.skipped;

bool should_skip_contact_impulse_warm_start_graph(
    return !has_contact_impulse_warm_startable_islands(graph, contacts);

bool should_skip_contact_impulse_dispatch(const ContactIslandGraph& graph,
    const IslandContactImpulseDispatchPreflight preflight = preflight_contact_impulse_dispatch(graph, contacts, dt);
    return !preflight.can_warm_start();





IslandCombinedWarmStartPreflight preflight_warm_start_island_combined_by_index(
    return preflight_warm_start_island_combined(graph.island(islandIndex),
                                                contacts,
                                                priorDistanceLambdas,
                                                priorContactLambdas);

    preflight.lambda = preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    preflight.impulse = preflight_warm_start_island_contact_impulses(island, contacts, dt);
}

IslandCombinedWarmStartPreflight preflight_warm_start_combined_island_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    IslandCombinedWarmStartPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_warm_start_combined_island(graph.island(islandIndex),
                                                contacts,
                                                dt,
                                                priorDistanceLambdas,
                                                priorContactLambdas);
}

bool should_skip_warm_start_combined_island_index(const ContactIslandGraph& graph,
                                                  u32 islandIndex,
                                                  f32 dt) {
    if (!island_index_valid(graph, islandIndex) || !is_valid_warm_start_dt(dt)) {
        return true;
    }
    return should_skip_warm_start_island(graph.island(islandIndex));
}

IslandCombinedWarmStartStats compute_island_combined_warm_start_stats(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    IslandCombinedWarmStartStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandCombinedWarmStartPreflight preflight = preflight_warm_start_combined_island(
            graph.island(islandIndex), contacts, dt, priorDistanceLambdas, priorContactLambdas);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;
        } else {
            ++stats.noPriorDataCount;
        }
    }
    return stats;
}

u32 count_warm_startable_combined_islands(const ContactIslandGraph& graph,
                                          const std::vector<narrowphase::ContactManifold>& contacts,
                                          f32 dt,
                                          const std::vector<f32>& priorDistanceLambdas,
                                          const std::vector<f32>& priorContactLambdas) {
    return compute_island_combined_warm_start_stats(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas)
        .warmStartableCount;
}

bool has_warm_startable_combined_islands(const ContactIslandGraph& graph,
                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                         f32 dt,
                                         const std::vector<f32>& priorDistanceLambdas,
                                         const std::vector<f32>& priorContactLambdas) {
    return count_warm_startable_combined_islands(
               graph, contacts, dt, priorDistanceLambdas, priorContactLambdas) > 0u;
}

IslandCombinedWarmStartGraphPreflight preflight_warm_start_combined_graph(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    IslandCombinedWarmStartGraphPreflight preflight{};
    preflight.invalidDt = !is_valid_warm_start_dt(dt);
    preflight.stats =
        compute_island_combined_warm_start_stats(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas);
    preflight.skipped = preflight.stats.warmStartableCount == 0u;
    return preflight;
}

IslandCombinedWarmStartPreflight preflight_warm_start_combined_island_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    IslandCombinedWarmStartPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_warm_start_combined_island(graph.island(islandIndex),
                                                contacts,
                                                dt,
                                                priorDistanceLambdas,
                                                priorContactLambdas);

IslandCombinedWarmStartStats compute_island_combined_warm_start_stats(
}

    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    IslandCombinedWarmStartStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandCombinedWarmStartPreflight preflight = preflight_warm_start_combined_island(
            graph.island(islandIndex), contacts, dt, priorDistanceLambdas, priorContactLambdas);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;
        } else {
            ++stats.noDataCount;
    return stats;

u32 count_warm_startable_combined_islands(const ContactIslandGraph& graph,
    return compute_island_combined_warm_start_stats(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas)
        .warmStartableCount;

bool has_warm_startable_combined_islands(const ContactIslandGraph& graph,
    return count_warm_startable_combined_islands(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas) > 0u;

IslandCombinedWarmStartGraphPreflight preflight_warm_start_combined_graph(
    IslandCombinedWarmStartGraphPreflight preflight{};
    preflight.invalidDt = !is_valid_warm_start_dt(dt);
    preflight.stats = compute_island_combined_warm_start_stats(graph,
    preflight.skipped = preflight.stats.warmStartableCount == 0u;
            ++stats.noPriorDataCount;
        }

u32 count_combined_warm_startable_islands(const ContactIslandGraph& graph,
                                          const std::vector<narrowphase::ContactManifold>& contacts,
                                          f32 dt,
                                          const std::vector<f32>& priorDistanceLambdas,
                                          const std::vector<f32>& priorContactLambdas) {

bool has_combined_warm_startable_islands(const ContactIslandGraph& graph,
    return count_combined_warm_startable_islands(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas) >
           0u;

    const ContactIslandGraph& graph,
    preflight.stats =
        compute_island_combined_warm_start_stats(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas);
    preflight.skipped = preflight.invalidDt || preflight.stats.warmStartableCount == 0u;
    return preflight;

bool should_skip_warm_start_combined_graph(const ContactIslandGraph& graph,
                                           const std::vector<narrowphase::ContactManifold>& contacts,
                                           f32 dt,
                                           const std::vector<f32>& priorDistanceLambdas,
                                           const std::vector<f32>& priorContactLambdas) {
    return !preflight_warm_start_combined_graph(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas)
        .can_warm_start();
}

std::vector<u32> collect_warm_startable_combined_island_indices(

std::vector<u32> collect_combined_warm_startable_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandCombinedWarmStartPreflight preflight = preflight_warm_start_combined_island(
            graph.island(islandIndex), contacts, dt, priorDistanceLambdas, priorContactLambdas);
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

bool should_skip_warm_start_combined_island_index(const ContactIslandGraph& graph,
                                                  u32 islandIndex,
                                                  f32 dt) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    if (!is_valid_warm_start_dt(dt)) {
    return should_skip_warm_start_island(graph.island(islandIndex));

bool warm_start_island_combined_guarded(SolverWorkBuffers& workBuffers,
                                        const ContactIslandGraph::Island& island,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        f32 dt,
                                        const std::vector<f32>& priorDistanceLambdas,
                                        const std::vector<f32>& priorContactLambdas) {
    const IslandCombinedWarmStartPreflight preflight =
        preflight_warm_start_combined_island(island, contacts, dt, priorDistanceLambdas, priorContactLambdas);
        preflight_warm_start_island_combined(island, contacts, priorDistanceLambdas, priorContactLambdas);
    if (!preflight.can_warm_start()) {
        return false;
    }

    if (preflight.lambdas.can_warm_start()) {
        warm_start_island_lambdas(workBuffers, island, priorDistanceLambdas, priorContactLambdas);
    if (preflight.impulses.can_warm_start()) {
    if (should_skip_warm_start_island(island)) {


    const IslandWarmStartPreflight preflight =
        preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    const bool hasPriorLambdas = preflight.can_warm_start();
    const bool hasContactImpulses = !island.contactIndices.empty();
    if (!hasPriorLambdas && !hasContactImpulses) {

    if (hasPriorLambdas) {
        return false;
    }

    if (hasContactImpulses) {
        warm_start_island_contact_impulses(workBuffers, island, contacts, dt);
    }
    return true;
}

void warm_start_island_lambdas(SolverWorkBuffers& workBuffers,
                               const ContactIslandGraph::Island& island,
                               const std::vector<f32>& priorDistanceLambdas,
                               const std::vector<f32>& priorContactLambdas) {
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex < priorDistanceLambdas.size()) {
            workBuffers.seedDistanceLambda(distanceIndex, priorDistanceLambdas[distanceIndex]);
        }
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= priorContactLambdas.size()) {
            continue;
        const f32 prior = priorContactLambdas[contactIndex];
        if (prior == 0.f) {
        f32& lambda = workBuffers.contactLambda(contactIndex);
        if (lambda == 0.f) {
            lambda = prior;

void warm_start_island_contact_impulses(SolverWorkBuffers& workBuffers,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        f32 dt) {
        if (contactIndex >= contacts.size()) {
        workBuffers.seedContactLambdaFromImpulse(contactIndex,
                                                 contacts[contactIndex].warmNormalImpulse,
                                                 dt);

bool warm_start_island_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
    const IslandContactImpulseWarmStartPreflight preflight =
        preflight_warm_start_contact_impulses(island, contacts, dt);
    if (!preflight.can_warm_start()) {
        return false;
    warm_start_island_contact_impulses(workBuffers, island, contacts, dt);
    return true;

IslandWarmStartResult warm_start_island_contact_impulses_result(SolverWorkBuffers& workBuffers,
                                                                const ContactIslandGraph& graph,
                                                                u32 islandIndex,
    IslandWarmStartResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;

    const ContactIslandGraph::Island& island = graph.island(islandIndex);

    result.warmed = true;

bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,
    return warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt).warmed;

u32 warm_start_all_islands_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
    return warm_start_all_islands_contact_impulses_result(workBuffers, graph, contacts, dt).warmedCount;

IslandBatchWarmStartResult warm_start_all_islands_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    IslandBatchWarmStartResult result{};
    const IslandContactImpulseWarmStartGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    result.warmStartableCount = preflight.stats.warmStartableCount;

    for (u32 islandIndex : collect_warm_startable_contact_impulse_island_indices(graph, contacts, dt)) {
        const IslandWarmStartResult warmResult =
            warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt);
        if (warmResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;

IslandWarmStartResult warm_start_island_combined_result(SolverWorkBuffers& workBuffers,
                                                        const ContactIslandGraph& graph,
                                                        u32 islandIndex,
                                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                                        f32 dt,
                                                        const std::vector<f32>& priorDistanceLambdas,
                                                        const std::vector<f32>& priorContactLambdas) {
    IslandWarmStartResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    if (!warm_start_island_combined_guarded(workBuffers,
                                            island,
                                            contacts,
                                            dt,
                                            priorDistanceLambdas,
                                            priorContactLambdas)) {
        result.skipped = true;
        return result;
    }

    result.warmed = true;
    return result;
}

void warm_start_island_lambdas(SolverWorkBuffers& workBuffers,
                               const ContactIslandGraph::Island& island,
                               const std::vector<f32>& priorDistanceLambdas,
                               const std::vector<f32>& priorContactLambdas) {
    if (!should_warm_start_island(island)) {
        return;
    }

    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex < priorDistanceLambdas.size()) {
            workBuffers.seedDistanceLambda(distanceIndex, priorDistanceLambdas[distanceIndex]);
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= priorContactLambdas.size()) {
            continue;
        const f32 prior = priorContactLambdas[contactIndex];
        if (prior == 0.f) {
        f32& lambda = workBuffers.contactLambda(contactIndex);
        if (lambda == 0.f) {
            lambda = prior;

void warm_start_island_contact_impulses(SolverWorkBuffers& workBuffers,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        f32 dt) {

        if (contactIndex >= contacts.size()) {
        workBuffers.seedContactLambdaFromImpulse(contactIndex,
                                                 contacts[contactIndex].warmNormalImpulse,
                                                 dt);

bool warm_start_island_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
    const IslandContactImpulseWarmStartPreflight preflight =
        preflight_warm_start_contact_impulses(island, contacts, dt);
        preflight_contact_impulse_warm_start_island(island, contacts);
    if (!preflight.can_warm_start()) {
        return false;
    warm_start_island_contact_impulses(workBuffers, island, contacts, dt);
    return true;

IslandWarmStartResult warm_start_island_contact_impulses_result(SolverWorkBuffers& workBuffers,
                                                                const ContactIslandGraph& graph,
                                                                u32 islandIndex,
    IslandWarmStartResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;

    const ContactIslandGraph::Island& island = graph.island(islandIndex);

    result.warmed = true;

bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,
    return warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt).warmed;

u32 warm_start_all_islands_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
    return warm_start_all_islands_contact_impulses_result(workBuffers, graph, contacts, dt).warmedCount;

IslandBatchWarmStartResult warm_start_all_islands_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    IslandBatchWarmStartResult result{};
    if (!is_valid_warm_start_dt(dt)) {

    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        ++result.warmStartableCount;
        const IslandWarmStartResult warmResult =
            warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt);
        if (warmResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;

    if (result.warmStartableCount == 0u) {

IslandWarmStartResult warm_start_island_combined_result(SolverWorkBuffers& workBuffers,
                                                        f32 dt,

    if (!warm_start_island_combined_guarded(workBuffers,
                                            island,
                                            contacts,
                                            dt,
                                            priorDistanceLambdas,
                                            priorContactLambdas)) {


u32 warm_start_all_islands_combined_guarded(SolverWorkBuffers& workBuffers,
    return warm_start_all_islands_combined_result(workBuffers,
                                                  graph,
                                                  priorContactLambdas)
        .warmedCount;

IslandBatchWarmStartResult warm_start_all_islands_combined_result(

        const IslandCombinedWarmStartPreflight preflight = preflight_warm_start_combined_island(
            graph.island(islandIndex), contacts, dt, priorDistanceLambdas, priorContactLambdas);
        const IslandWarmStartResult warmResult = warm_start_island_combined_result(workBuffers,
                                                                                   islandIndex,
                                                                                   priorContactLambdas);


bool is_body_sleeping(u32 flags) {
    return (flags & RB_SLEEPING) != 0u;

bool is_body_static_or_kinematic(u32 flags) {
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;

bool is_body_movable(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
    if (is_body_static_or_kinematic(bodies.flags[bodyIndex])) {
    if (is_body_sleeping(bodies.flags[bodyIndex])) {
    return bodies.invMasses[bodyIndex] > 0.f;

const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandBuildRejectReason::OutOfRangeContactRefs:
        return "OutOfRangeContactRefs";
    case IslandBuildRejectReason::OutOfRangeDistanceRefs:
        return "OutOfRangeDistanceRefs";
    }
    return "Unknown";
}

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (bodyCount == 0u) {
        bool hasInRangeContact = false;
        for (const narrowphase::ContactManifold& contact : contacts) {
            if (contact.bodyA < bodyCount && contact.bodyB < bodyCount) {
                hasInRangeContact = true;
                break;
            }
        }
        bool hasInRangeDistance = false;
        for (const DistanceConstraint& constraint : distanceConstraints) {
            if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
                hasInRangeDistance = true;
                break;
            }
        }
        if (!hasInRangeContact && !hasInRangeDistance) {
            return IslandBuildRejectReason::EmptyInput;
        }
    }

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid && (contact.bodyA >= bodyCount || contact.bodyB >= bodyCount)) {
            return IslandBuildRejectReason::OutOfRangeContactRefs;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount) {
            return IslandBuildRejectReason::OutOfRangeDistanceRefs;
        }
    }

    return IslandBuildRejectReason::None;
}

bool island_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::OutOfRangeIslandIndex:
        return "OutOfRangeIslandIndex";
    case IslandConstraintSolveRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (!preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve()) {
        return IslandConstraintSolveRejectReason::StaleConstraintRefs;
    }
    if (!preflight_island_solve_bodies(island, bodies).can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_index_valid(graph, islandIndex)) {
        return IslandConstraintSolveRejectReason::OutOfRangeIslandIndex;
    }
    return island_constraint_solve_reject_reason(
        graph.island(islandIndex), bodies, contacts, distanceConstraints);
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIslandIndex:
        return "OutOfRangeIslandIndex";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandSleepRejectReason island_sleep_reject_reason_by_index(const ContactIslandGraph& graph,
                                                            u32 islandIndex,
                                                            const RigidBodySoA& bodies) {
    if (!island_index_valid(graph, islandIndex)) {
        return IslandSleepRejectReason::OutOfRangeIslandIndex;
    }
    return island_sleep_reject_reason(graph.island(islandIndex), bodies);
}

bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected) {
    return island_sleep_reject_reason(island, bodies) == expected;
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::NoConstrainedIslands:
        return "NoConstrainedIslands";
    case IslandSleepGraphRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    }
    return "Unknown";
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    const IslandSleepGraphStats stats = compute_island_sleep_stats(graph, bodies);
    if (stats.totalIslands == 0u || stats.emptyCount == stats.totalIslands) {
        return IslandSleepGraphRejectReason::NoConstrainedIslands;
    }
    if (stats.fullyActiveCount == 0u && stats.mixedSleepCount == 0u) {
        return IslandSleepGraphRejectReason::AllIslandsSleeping;
    }
    return IslandSleepGraphRejectReason::None;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIslandIndex:
        return "OutOfRangeIslandIndex";
    case IslandWakeRejectReason::NoSleepingBodies:
        return "NoSleepingBodies";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";
    }
    return "Unknown";
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (sleepingCount == 0u) {
        return IslandWakeRejectReason::NoSleepingBodies;
    }
    if (activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;
    }
    return IslandWakeRejectReason::None;
}

IslandWakeRejectReason island_wake_reject_reason_by_index(const ContactIslandGraph& graph,
                                                          u32 islandIndex,
                                                          const RigidBodySoA& bodies) {
    if (!island_index_valid(graph, islandIndex)) {
        return IslandWakeRejectReason::OutOfRangeIslandIndex;
    }
    return island_wake_reject_reason(graph.island(islandIndex), bodies);
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    if (compute_island_wake_stats(graph, bodies).wakeableCount == 0u) {
        return IslandWakeGraphRejectReason::NoWakeableIslands;
    }
    return IslandWakeGraphRejectReason::None;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandBuildRejectReason::OutOfRangeRefs:
        return "OutOfRangeRefs";
    }
    return "Unknown";
}

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (is_in_range_island_contact(contact, bodyCount)) {
            ++inRangeContactCount;
        } else if (contact.valid) {
            ++outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (is_in_range_island_distance(constraint, bodyCount)) {
            ++inRangeDistanceCount;
        } else {
            ++outOfRangeDistanceBodyCount;
        }
    }

    if (outOfRangeContactBodyCount > 0u || outOfRangeDistanceBodyCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeRefs;
    }
    if (bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return IslandBuildRejectReason::EmptyInput;
    }
    return IslandBuildRejectReason::None;
}

bool island_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }

    const IslandConstraintRefsPreflight refs =
        preflight_island_constraint_refs(island, contacts, distanceConstraints);
    if (!refs.can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }

    const IslandSolveBodiesPreflight bodyPreflight = preflight_island_solve_bodies(island, bodies);
    if (!bodyPreflight.can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }

    return IslandConstraintSolveRejectReason::None;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected) {
    return island_sleep_reject_reason(island, bodies) == expected;
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoWakeTarget:
        return "NoWakeTarget";
    }
    return "Unknown";
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (sleepingCount == 0u || activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoWakeTarget;
    }
    return IslandWakeRejectReason::None;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::EmptyGraph:
        return "EmptyGraph";
    case IslandSleepGraphRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    }
    return "Unknown";
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    if (graph.islandCount() == 0u) {
        return IslandSleepGraphRejectReason::EmptyGraph;
    }

    const IslandSleepGraphStats stats = compute_island_sleep_stats(graph, bodies);
    if (stats.fullyActiveCount == 0u && stats.mixedSleepCount == 0u) {
        return IslandSleepGraphRejectReason::AllIslandsSleeping;
    }
    return IslandSleepGraphRejectReason::None;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::EmptyGraph:
        return "EmptyGraph";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    if (graph.islandCount() == 0u) {
        return IslandWakeGraphRejectReason::EmptyGraph;
    }

    const IslandWakeGraphStats stats = compute_island_wake_stats(graph, bodies);
    if (stats.wakeableCount == 0u) {
        return IslandWakeGraphRejectReason::NoWakeableIslands;
    }
    return IslandWakeGraphRejectReason::None;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandBuildRejectReason::OutOfRangeRefs:
        return "OutOfRangeRefs";
    case IslandBuildRejectReason::DegenerateRefs:
        return "DegenerateRefs";
    }
    return "Unknown";
}

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;
    u32 selfContactCount = 0;
    u32 selfDistanceCount = 0;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (is_in_range_island_contact(contact, bodyCount)) {
            ++inRangeContactCount;
            if (contact.valid && constraint_pair_is_degenerate(contact.bodyA, contact.bodyB)) {
                ++selfContactCount;
            }
        } else if (contact.valid) {
            ++outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (is_in_range_island_distance(constraint, bodyCount)) {
            ++inRangeDistanceCount;
            if (constraint_pair_is_degenerate(constraint.bodyA, constraint.bodyB)) {
                ++selfDistanceCount;
            }
        } else {
            ++outOfRangeDistanceBodyCount;
        }
    }

    if (outOfRangeContactBodyCount > 0u || outOfRangeDistanceBodyCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeRefs;
    }
    if (selfContactCount > 0u || selfDistanceCount > 0u) {
        return IslandBuildRejectReason::DegenerateRefs;
    }
    if (bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return IslandBuildRejectReason::EmptyInput;
    }
    return IslandBuildRejectReason::None;
}

bool island_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected) {
    return island_sleep_reject_reason(island, bodies) == expected;
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoWakeTarget:
        return "NoWakeTarget";
    }
    return "Unknown";
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (sleepingCount == 0u || activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoWakeTarget;
    }
    return IslandWakeRejectReason::None;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    default:
        return "Unknown";
    }
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (should_skip_island_constraint_refs(island, contacts, distanceConstraints)) {
        return IslandConstraintSolveRejectReason::StaleConstraintRefs;
    }
    if (should_skip_island_solve_bodies(island, bodies)) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason) {
    switch (reason) {
    case IslandSleepSolveRejectReason::None:
        return "None";
    case IslandSleepSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepSolveRejectReason::HasActiveDynamics:
        return "HasActiveDynamics";
    default:
        return "Unknown";
    }
}

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepSolveRejectReason::EmptyIsland;
    }

    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (!is_body_sleeping(flags)) {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount > 0u) {
        return IslandSleepSolveRejectReason::HasActiveDynamics;
    }
    return IslandSleepSolveRejectReason::None;
}

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected) {
    return island_sleep_solve_reject_reason(island, bodies) == expected;
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoActiveDynamics:
        return "NoActiveDynamics";
    default:
        return "Unknown";
    }
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (sleepingCount == 0u) {
        return IslandWakeRejectReason::NoMixedSleepState;
    }
    if (activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamics;
    }
    return IslandWakeRejectReason::None;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

IslandSolveRejectReason island_solve_bodies_reject_reason(const ContactIslandGraph::Island& island,
                                                          const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSolveRejectReason::EmptyIsland;
    }

    u32 movableCount = 0u;
    for (u32 bodyIndex : island.bodyIndices) {
        if (is_body_movable(bodies, bodyIndex)) {
            ++movableCount;
        }
    }
    if (movableCount == 0u) {
        return IslandSolveRejectReason::NoMovableBodies;
    }
    return IslandSolveRejectReason::None;
}

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0u;
    u32 activeDynamicCount = 0u;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0u;
    u32 activeDynamicCount = 0u;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (sleepingCount == 0u || activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoWakeTarget;
    }
    return IslandWakeRejectReason::None;
}

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    const IslandGraphBuildPreflight graphPreflight =
        preflight_island_graph_build(bodyCount, contacts, distanceConstraints);
    IslandBuildPreflight preflight{};
    const ContactIslandGraphBuildPreflight graphPreflight =
        preflightContactIslandGraphBuild(bodyCount, contacts, distanceConstraints);
    const IslandGraphBuildPreflight graphPreflight =
        preflight_island_graph_build(bodyCount, contacts, distanceConstraints);
    preflight.reason = graphPreflight.reason;
    preflight.skipped = graphPreflight.skipped;
    preflight.stats.bodyCount = graphPreflight.bodyCount;
    preflight.stats.contactSlotCount = graphPreflight.contactSlotCount;
    preflight.stats.distanceSlotCount = graphPreflight.distanceSlotCount;
    preflight.stats.validContactCount = graphPreflight.validContactCount;
    preflight.stats.inRangeContactCount = graphPreflight.inRangeContactCount;
    preflight.stats.inRangeDistanceCount = graphPreflight.inRangeDistanceCount;
    preflight.stats.outOfRangeContactBodyCount = graphPreflight.outOfRangeContactBodyCount;
    preflight.stats.outOfRangeDistanceBodyCount = graphPreflight.outOfRangeDistanceBodyCount;
    preflight.reason = island_build_reject_reason(bodyCount, contacts, distanceConstraints);
    preflight.reason = island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints);
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.stats.validContactCount;
        } else {
            ++preflight.stats.invalidContactCount;
        }
        if (!contact.valid) {
            continue;
        if (contact.bodyA == contact.bodyB) {
            ++preflight.stats.selfContactCount;
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (ContactIslandGraph::is_self_contact(contact.bodyA, contact.bodyB)) {
        const bool inRange = contact_body_indices_in_range(contact, bodyCount);
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount && contact.bodyA != contact.bodyB;
        const bool inRange = body_pair_in_range(bodyCount, contact.bodyA, contact.bodyB);
        const bool inRange = island_body_index_in_range(contact.bodyA, bodyCount) &&
                             island_body_index_in_range(contact.bodyB, bodyCount);
        if (inRange) {
        if (is_in_range_island_contact(contact, bodyCount)) {
        if (contact_in_island_build_range(bodyCount, contact)) {
            ++preflight.stats.inRangeContactCount;
            if (contact.valid && constraint_pair_is_degenerate(contact.bodyA, contact.bodyB)) {
            if (contact.valid && contact.bodyA == contact.bodyB) {
                ++preflight.stats.selfReferentialContactCount;
        } else if (contact.valid) {
            ++preflight.stats.outOfRangeContactBodyCount;
        const bool inRange = ContactIslandGraph::bodies_in_range(contact.bodyA, contact.bodyB, bodyCount);

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA == constraint.bodyB) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
        if (is_in_range_island_distance(constraint, bodyCount)) {
        if (ContactIslandGraph::bodies_in_range(constraint.bodyA, constraint.bodyB, bodyCount)) {
        if (body_pair_in_range(bodyCount, constraint.bodyA, constraint.bodyB)) {
        if (distance_in_island_build_range(bodyCount, constraint)) {
        if (island_body_index_in_range(constraint.bodyA, bodyCount) &&
            island_body_index_in_range(constraint.bodyB, bodyCount)) {
            ++preflight.stats.inRangeDistanceCount;
        if (distance_body_indices_in_range(constraint, bodyCount)) {
            if (constraint_pair_is_degenerate(constraint.bodyA, constraint.bodyB)) {
                ++preflight.stats.selfDistanceCount;
                ++preflight.stats.selfReferentialDistanceCount;
            ++preflight.stats.outOfRangeDistanceBodyCount;

    preflight.reason = island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints);
    preflight.skipped = bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                        preflight.stats.inRangeDistanceCount == 0u;

    const ContactIslandBuildInputStats inputStats =
        compute_contact_island_build_input_stats(bodyCount, contacts, distanceConstraints);
    const IslandBuildInputStats inputStats =
        scan_island_build_inputs(bodyCount, contacts, distanceConstraints);
    preflight.stats.bodyCount = inputStats.bodyCount;
    preflight.stats.contactSlotCount = inputStats.contactSlotCount;
    preflight.stats.distanceSlotCount = inputStats.distanceSlotCount;
    preflight.stats.validContactCount = inputStats.validContactCount;
    preflight.stats.inRangeContactCount = inputStats.inRangeContactCount;
    preflight.stats.inRangeDistanceCount = inputStats.inRangeDistanceCount;
    preflight.stats.outOfRangeContactBodyCount = inputStats.outOfRangeContactBodyCount;
    preflight.stats.outOfRangeDistanceBodyCount = inputStats.outOfRangeDistanceBodyCount;

    preflight.skipped = preflight.reason == IslandBuildRejectReason::EmptyInput;
    preflight.stats.bodyCount = graphPreflight.stats.bodyCount;
    preflight.stats.contactSlotCount = graphPreflight.stats.contactSlotCount;
    preflight.stats.distanceSlotCount = graphPreflight.stats.distanceSlotCount;
    preflight.stats.validContactCount = graphPreflight.stats.validContactCount;
    preflight.stats.inRangeContactCount = graphPreflight.stats.inRangeContactCount;
    preflight.stats.inRangeDistanceCount = graphPreflight.stats.inRangeDistanceCount;
    preflight.stats.selfPairContactCount = graphPreflight.stats.selfPairContactCount;
    preflight.stats.selfPairDistanceCount = graphPreflight.stats.selfPairDistanceCount;
    preflight.stats.outOfRangeContactBodyCount = graphPreflight.stats.outOfRangeContactBodyCount;
    preflight.stats.outOfRangeDistanceBodyCount = graphPreflight.stats.outOfRangeDistanceBodyCount;
    preflight.skipped = bodyCount == 0u && inputStats.inRangeContactCount == 0u &&
                        inputStats.inRangeDistanceCount == 0u;
    if (preflight.skipped) {
        preflight.reason = IslandBuildRejectReason::EmptyInput;
    } else if (preflight.stats.outOfRangeContactBodyCount > 0u) {
        preflight.reason = IslandBuildRejectReason::OutOfRangeContactRefs;
    } else if (preflight.stats.outOfRangeDistanceBodyCount > 0u) {
        preflight.reason = IslandBuildRejectReason::OutOfRangeDistanceRefs;
        preflight.reason = IslandBuildRejectReason::OutOfRangeContact;
        preflight.reason = IslandBuildRejectReason::OutOfRangeDistance;
    const ContactIslandBuildPreflight contactPreflight =
        preflight_contact_island_build(bodyCount, contacts, distanceConstraints);
    preflight.stats.bodyCount = contactPreflight.stats.bodyCount;
    preflight.stats.contactSlotCount = contactPreflight.stats.contactSlotCount;
    preflight.stats.distanceSlotCount = contactPreflight.stats.distanceSlotCount;
    preflight.stats.validContactCount = contactPreflight.stats.validContactCount;
    preflight.stats.inRangeContactCount = contactPreflight.stats.inRangeContactCount;
    preflight.stats.inRangeDistanceCount = contactPreflight.stats.inRangeDistanceCount;
    preflight.stats.outOfRangeContactBodyCount = contactPreflight.stats.outOfRangeContactBodyCount;
    preflight.stats.outOfRangeDistanceBodyCount = contactPreflight.stats.outOfRangeDistanceBodyCount;
    preflight.skipped = contactPreflight.skipped;
    const IslandBuildInputCoverage coverage =
        ContactIslandGraph::scanBuildInputs(bodyCount, contacts, distanceConstraints);
    preflight.stats.bodyCount = coverage.bodyCount;
    preflight.stats.contactSlotCount = coverage.contactSlotCount;
    preflight.stats.distanceSlotCount = coverage.distanceSlotCount;
    preflight.stats.validContactCount = coverage.validContactCount;
    preflight.stats.inRangeContactCount = coverage.inRangeContactCount;
    preflight.stats.inRangeDistanceCount = coverage.inRangeDistanceCount;
    preflight.stats.outOfRangeContactBodyCount = coverage.outOfRangeContactCount;
    preflight.stats.outOfRangeDistanceBodyCount = coverage.outOfRangeDistanceCount;
    preflight.skipped = coverage.isEmptyInput();
    preflight.stats = scan_island_build_inputs(bodyCount, contacts, distanceConstraints);
    preflight.skipped = preflight.stats.is_empty();
    preflight.skipped = preflight.reason == IslandGraphBuildRejectReason::EmptyInputs ||
                        (bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                         preflight.stats.inRangeDistanceCount == 0u);
    preflight.reason = diagnose_island_build_reject(bodyCount, contacts, distanceConstraints);
    preflight.skipped = preflight.reason == IslandGraphBuildRejectReason::EmptyInput;
    preflight.reason = classifyIslandBuildRejectReason(preflight);
    const IslandGraphBuildPreflight graphPreflight =
        preflight_island_graph_build(bodyCount, contacts, distanceConstraints);
    preflight.reason = islandGraphBuildRejectReason(bodyCount, contacts, distanceConstraints);
    preflight.emptyInput = preflight.reason == IslandGraphBuildRejectReason::EmptyInput;
    preflight.outOfRangeRefs = preflight.reason == IslandGraphBuildRejectReason::OutOfRangeRefs;
    preflight.skipped = preflight.emptyInput;
    preflight.reason = classifyIslandGraphBuildReject(preflight);
    preflight.reason = island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints);
    return preflight;

const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandBuildRejectReason::OutOfRangeContactRefs:
        return "OutOfRangeContactRefs";
    case IslandBuildRejectReason::OutOfRangeDistanceRefs:
        return "OutOfRangeDistanceRefs";
    return "Unknown";

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).reason;

bool island_build_rejects_for_reason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;

bool has_in_range_constraints(u32 bodyCount,
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    return preflight.stats.inRangeContactCount > 0u || preflight.stats.inRangeDistanceCount > 0u;





const char* islandBuildRejectReasonName(IslandBuildRejectReason reason) {
    case IslandBuildRejectReason::OutOfRangeContactRef:
        return "OutOfRangeContactRef";
    case IslandBuildRejectReason::OutOfRangeDistanceRef:
        return "OutOfRangeDistanceRef";
    default:

IslandBuildRejectReason islandBuildRejectReason(
        return IslandBuildRejectReason::EmptyInput;
    if (preflight.stats.outOfRangeContactBodyCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeContactRef;
    if (preflight.stats.outOfRangeDistanceBodyCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeDistanceRef;
    return IslandBuildRejectReason::None;

bool islandBuildRejectsForReason(u32 bodyCount,
    return islandBuildRejectReason(bodyCount, contacts, distanceConstraints) == expected;

IslandBuildRejectPreflight preflight_island_build_reject(
    IslandBuildRejectPreflight preflight{};
    preflight.build = preflight_island_build(bodyCount, contacts, distanceConstraints);
    preflight.reason = islandBuildRejectReason(bodyCount, contacts, distanceConstraints);
    preflight.emptyInput = preflight.reason == IslandBuildRejectReason::EmptyInput;
    preflight.unsafeContactRefs = preflight.reason == IslandBuildRejectReason::OutOfRangeContactRef;
    preflight.unsafeDistanceRefs = preflight.reason == IslandBuildRejectReason::OutOfRangeDistanceRef;
    return ContactIslandGraph::preflightBuildInputs(bodyCount, contacts, distanceConstraints);

bool canSkipIslandBuild(u32 bodyCount,
    return !preflight_island_build_reject(bodyCount, contacts, distanceConstraints).can_build();

bool shouldRunIslandBuild(u32 bodyCount,
    return preflight_island_build_reject(bodyCount, contacts, distanceConstraints).can_build();

    return preflight_island_graph_build(bodyCount, contacts, distanceConstraints);
}

bool should_skip_island_build(u32 bodyCount,
    return canSkipIslandBuild(bodyCount, contacts, distanceConstraints);

bool can_skip_island_build(u32 bodyCount,
                           const std::vector<narrowphase::ContactManifold>& contacts,
                           const std::vector<DistanceConstraint>& distanceConstraints) {
    return can_skip_contact_island_build(bodyCount, contacts, distanceConstraints);
}

bool should_run_island_build(u32 bodyCount,
    return should_run_contact_island_build(bodyCount, contacts, distanceConstraints);

IslandBuildResult build_island_graph_guarded_result(ContactIslandGraph& graph,
                                                  u32 bodyCount,
    IslandBuildResult result{};
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    result.reason = preflight.reason;
    if (!preflight.can_build()) {
        result.skipped = true;
        graph.clear();
        return result;

    graph.build(bodyCount, contacts, distanceConstraints);
    result.built = true;
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints) {
    return !should_skip_island_build(bodyCount, contacts, distanceConstraints);
}

bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints) {
    return !should_skip_island_build(bodyCount, contacts, distanceConstraints);
}

bool has_in_range_constraints(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    return preflight.stats.inRangeContactCount > 0u || preflight.stats.inRangeDistanceCount > 0u;
    const IslandBuildInputScan scan = scan_island_build_inputs(bodyCount, contacts, distanceConstraints);
    return !can_partition_island_build_inputs(bodyCount, scan);
    return ContactIslandGraph::shouldSkipBuild(bodyCount, contacts, distanceConstraints);
}

bool can_skip_island_build(u32 bodyCount,
                           const std::vector<narrowphase::ContactManifold>& contacts,
                           const std::vector<DistanceConstraint>& distanceConstraints) {
    return should_skip_island_build(bodyCount, contacts, distanceConstraints);

bool should_run_island_build(u32 bodyCount,
    return !can_skip_island_build(bodyCount, contacts, distanceConstraints);
    return should_skip_island_graph_build(bodyCount, contacts, distanceConstraints);
    return canSkipIslandBuild(bodyCount, contacts, distanceConstraints);
}

bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool should_run_island_build(u32 bodyCount,
                             const std::vector<narrowphase::ContactManifold>& contacts,
                             const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool build_island_graph_guarded(ContactIslandGraph& graph,
    return graph.buildGuarded(bodyCount, contacts, distanceConstraints);

bool canSkipIslandBuild(u32 bodyCount,
    return canSkipContactIslandGraphBuild(bodyCount, contacts, distanceConstraints);

bool shouldRunIslandBuild(u32 bodyCount,
    return shouldRunContactIslandGraphBuild(bodyCount, contacts, distanceConstraints);
    return build_island_graph_guarded_result(graph, bodyCount, contacts, distanceConstraints).built;
    return should_skip_island_graph_build(bodyCount, contacts, distanceConstraints);
                                const std::vector<narrowphase::ContactManifold>& contacts,
                                const std::vector<DistanceConstraint>& distanceConstraints) {
}

bool build_island_graph_integrity_guarded(ContactIslandGraph& graph,
                                          u32 bodyCount,
                                          const std::vector<narrowphase::ContactManifold>& contacts,
    if (!build_island_graph_guarded(graph, bodyCount, contacts, distanceConstraints)) {
    IslandBuildPreflight preflight{};
    return build_island_graph_with_preflight(graph, bodyCount, contacts, distanceConstraints, preflight);

bool build_island_graph_with_preflight(ContactIslandGraph& graph,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       IslandBuildPreflight& outPreflight) {
    outPreflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    if (!outPreflight.can_build()) {
    const IslandBuildInputScan scan = scan_island_build_inputs(bodyCount, contacts, distanceConstraints);
    if (!can_partition_island_build_inputs(bodyCount, scan)) {
        graph.clear();
        return false;
    if (should_skip_island_graph_integrity(graph, bodyCount, contacts, distanceConstraints)) {
    return true;
    return graph.buildGuarded(bodyCount, contacts, distanceConstraints);

bool dispatch_solve_island_guarded(RigidBodySoA& bodies,
                                    const ContactIslandGraph& graph,
                                    u32 islandIndex,
                                    SolverWorkBuffers& workBuffers,
                                    f32 dt,
                                    f32 contactCompliance,
                                    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const IslandSolveJob job = extract_island(graph, islandIndex);
    return dispatch_solve_island_job_guarded(bodies,
                                             job,
                                             workBuffers,
                                             distanceConstraints,
                                             dt,
                                             contactCompliance,
                                             invMassFn);

bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
                                       const IslandSolveJob& job,
    if (!preflight_solve_island_job(job, dt).can_dispatch() || job.island == nullptr) {
    return solve_island_job_guarded(bodies,
                                  *job.island,

    return build_island_graph_guarded(graph, bodyCount, contacts, distanceConstraints);
    return build_island_graph_result(graph, bodyCount, contacts, distanceConstraints).built;

IslandBuildResult build_island_graph_result(ContactIslandGraph& graph,




    IslandBuildResult result{};
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    result.stats = preflight.stats;
    result.skipped = preflight.skipped;
    result.unsafeRefs = preflight.has_unsafe_refs();
    if (!preflight.can_build()) {
        return result;
    graph.build(bodyCount, contacts, distanceConstraints);
    result.built = true;
    result.degenerateRefs = preflight.has_degenerate_refs();


    result.preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
    result.unsafeRefs = result.preflight.has_unsafe_refs();
    if (!result.preflight.can_build()) {
        result.skipped = true;





bool build_island_graph_in_range_guarded(ContactIslandGraph& graph,
                                         ContactIslandGraphBuildStats* outStats) {
    if (preflight.skipped) {
        if (outStats != nullptr) {
            *outStats = {};
    graph.buildInRange(bodyCount, contacts, distanceConstraints, outStats);
    return build_island_graph_guarded_result(graph, bodyCount, contacts, distanceConstraints).built;

IslandBuildResult build_island_graph_guarded_result(ContactIslandGraph& graph,
    return graph.build_guarded(bodyCount, contacts, distanceConstraints);



IslandBuildOutcome build_island_graph_result(ContactIslandGraph& graph,
                                IslandGraphBuildRejectReason* reason) {
    return graph.build_guarded(bodyCount, contacts, distanceConstraints, reason);

bool canSkipIslandBuild(u32 bodyCount,
                        const std::vector<DistanceConstraint>& distanceConstraints) {
    return can_skip_island_graph_build(bodyCount, contacts, distanceConstraints);

bool shouldRunIslandBuild(u32 bodyCount,


    return should_run_island_graph_build(bodyCount, contacts, distanceConstraints);
}

IslandSolveBodiesPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
                                                         const RigidBodySoA& bodies) {
    IslandSolveBodiesPreflight preflight{};
    preflight.reason = island_solve_bodies_reject_reason(island, bodies);
    if (!island_has_constraints(island)) {
        preflight.reason = IslandSolveRejectReason::EmptyIsland;
        preflight.skipped = true;

    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            ++preflight.outOfRangeBodyCount;
            continue;
        }
        ++preflight.inRangeBodyCount;
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            ++preflight.staticOrKinematicCount;
        if (is_body_sleeping(flags)) {
            ++preflight.sleepingCount;
        if (is_body_movable(bodies, bodyIndex)) {
            ++preflight.movableCount;

bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_bodies(island, bodies).can_solve();

IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const RigidBodySoA& bodies,
    IslandConstraintSolvePreflight preflight{};

    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(island, bodies);

bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
    return !preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints).can_solve();

IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_constraint_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_island_constraint_solve_index(const ContactIslandGraph& graph,
    return !preflight_island_constraint_solve_by_index(graph, islandIndex, bodies, contacts, distanceConstraints)
                .can_solve();

bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints)) {
        return false;
    return solve_island_job(bodies,
                          island,
                          workBuffers,
                          distanceConstraints,
                          dt,
                          contactCompliance,
                          invMassFn);

IslandConstraintSolveResult solve_island_job_result(RigidBodySoA& bodies,
    IslandConstraintSolveResult result{};
    result.islandIndex = islandIndex;
        result.skipped = true;
        return result;

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    result.solved = solve_island_job_guarded(bodies,
    result.skipped = !result.solved;

IslandSolvePipelinePreflight preflight_island_solve_pipeline(const ContactIslandGraph& graph,
                                                             f32 dt) {
    IslandSolvePipelinePreflight preflight{};
    preflight.reason = island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    if (preflight.reason == IslandConstraintSolveRejectReason::EmptyIsland) {

    const IslandSolveJob job = extract_island(graph, islandIndex);
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.constraintSolve =
        preflight_island_constraint_solve(*job.island, bodies, contacts, distanceConstraints);
    preflight.sleep = preflight_island_sleep(*job.island, bodies);
    preflight.skipped = !should_solve_island(job);
    preflight.refs =
        preflight_island_constraint_refs(island, contacts, distanceConstraints, bodies.count());

bool dispatch_solve_island_with_solve_guards(RigidBodySoA& bodies,
    return dispatch_solve_island_with_solve_guards_result(bodies,
                                                          graph,
                                                          islandIndex,
                                                          invMassFn)
        .solved;

IslandDispatchResult dispatch_solve_island_with_solve_guards_result(
    RigidBodySoA& bodies,
    IslandDispatchResult result{};

    const IslandSolvePipelinePreflight preflight =
        preflight_island_solve_pipeline(graph, islandIndex, bodies, contacts, distanceConstraints, dt);
    if (!preflight.can_solve()) {

    wake_island_sleepers_by_index_guarded(bodies, graph, islandIndex);

                                             *job.island,


std::vector<u32> collect_constraint_solveable_island_indices(
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandConstraintSolvePreflight preflight =
            preflight_island_constraint_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);
        if (preflight.can_solve()) {
            indices.push_back(islandIndex);
    return indices;


bool should_skip_island_constraint_solve_by_index(const ContactIslandGraph& graph,
    return !preflight_island_constraint_solve_by_index(graph,
                                                       bodies,
                                                       contacts,
                                                       distanceConstraints)

    if (!is_valid_island_solve_dt(dt) || !island_has_constraints(island)) {



bool dispatch_solve_island_constraint_guarded(RigidBodySoA& bodies,
    return solve_island_job_guarded(bodies,
                                    graph.island(islandIndex),

IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
    IslandSleepPreflight preflight{};
    preflight.reason = island_sleep_reject_reason(island, bodies);
    if (preflight.reason == IslandSleepRejectReason::EmptyIsland) {
        preflight.skipped = true;
        preflight.reason = IslandSleepSolveRejectReason::EmptyIsland;
        return preflight;
    }

            ++preflight.activeDynamicCount;

    preflight.allSleeping = preflight.activeDynamicCount == 0u && preflight.sleepingCount > 0u;

IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
    preflight.allSleeping = preflight.reason == IslandSleepRejectReason::AllSleeping;
    if (preflight.allSleeping) {
        preflight.reason = IslandSleepSolveRejectReason::AllSleeping;
    }

    if (preflight.movableCount == 0u) {
        preflight.reason = IslandSolveRejectReason::NoMovableBodies;
    }

    if (preflight.movableCount == 0u) {
        preflight.reason = IslandSolveRejectReason::NoMovableBodies;
    }

    return preflight;
}

                                                     u32 islandIndex,
                                                     const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        preflight.reason = IslandSleepRejectReason::OutOfRangeIslandIndex;
    return preflight_island_sleep(graph.island(islandIndex), bodies);

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
    IslandWakePreflight preflight{};
    preflight.reason = island_wake_reject_reason(island, bodies);
    if (preflight.reason == IslandWakeRejectReason::EmptyIsland) {
        preflight.skipped = true;
        preflight.reason = IslandWakeRejectReason::EmptyIsland;
        return preflight;
    }


    preflight.hasMixedSleepState =
        preflight.sleepingCount > 0u && preflight.activeDynamicCount > 0u;

IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
        preflight.reason == IslandWakeRejectReason::None && preflight.sleepingCount > 0u &&
        preflight.activeDynamicCount > 0u;
    if (!preflight.hasMixedSleepState || preflight.activeDynamicCount == 0u) {
        preflight.reason = IslandWakeRejectReason::NoMixedSleepState;
    }
    return preflight;
}

                                                   u32 islandIndex,
                                                   const RigidBodySoA& bodies) {
    IslandWakePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        preflight.reason = IslandWakeRejectReason::OutOfRangeIslandIndex;
    return preflight_island_wake(graph.island(islandIndex), bodies);

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason) {
    switch (reason) {
    case IslandSleepSolveRejectReason::None:
        return "None";
    case IslandSleepSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies) {
    return preflight_island_sleep(island, bodies).reason;
}

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected) {
    return island_sleep_solve_reject_reason(island, bodies) == expected;
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    }
    return "Unknown";
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    return preflight_island_wake(island, bodies).reason;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
                                                                  u32 islandIndex,
                                                                  const RigidBodySoA& bodies) {
    IslandSolveBodiesPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_solve_bodies(graph.island(islandIndex), bodies);
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }

    const IslandConstraintRefsPreflight refs =
        preflight_island_constraint_refs(island, contacts, distanceConstraints);
    if (!refs.can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }

    const IslandSolveBodiesPreflight bodyPreflight = preflight_island_solve_bodies(island, bodies);
    if (!bodyPreflight.can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }

    return IslandConstraintSolveRejectReason::None;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (should_skip_island_constraint_refs(island, contacts, distanceConstraints)) {
        return IslandConstraintSolveRejectReason::StaleConstraintRefs;
    }
    if (should_skip_island_solve_bodies(island, bodies)) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

bool islandConstraintSolveRejectsForReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints) == expected;
}

IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolvePreflight preflight{};
    preflight.reason = island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    preflight.reason =
        island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    if (!island_has_constraints(island)) {
    if (preflight.reason == IslandConstraintSolveRejectReason::EmptyIsland) {
        islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints);
    if (preflight.reason == IslandSolveRejectReason::EmptyIsland) {
        preflight.reason = IslandSolveRejectReason::EmptyIsland;
        preflight.reason = IslandConstraintSolveRejectReason::EmptyIsland;
        preflight.skipped = true;
        preflight.emptyIsland = true;
        preflight.reason = IslandConstraintSolveRejectReason::EmptyIsland;
        preflight.reason = classifyIslandConstraintSolveReject(preflight);
        preflight.reason = IslandSolveRejectReason::EmptyIsland;
        return preflight;
    }

    preflight.skipped = preflight.reason == IslandConstraintSolveRejectReason::EmptyIsland;
    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(island, bodies);
    if (!preflight.refs.can_solve()) {
        preflight.reason = IslandConstraintSolveRejectReason::StaleConstraintRefs;
    } else if (preflight_island_sleep(island, bodies).can_skip_solve()) {
        preflight.reason = IslandConstraintSolveRejectReason::AllSleeping;
    } else if (!preflight.bodies.can_solve()) {
        preflight.reason = IslandConstraintSolveRejectReason::NoMovableBodies;
    preflight.reason =
        island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    preflight.skipped = preflight.reason == IslandConstraintSolveRejectReason::EmptyIsland;
    preflight.reason = diagnose_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints);
    preflight.reason = classifyIslandConstraintSolveRejectReason(preflight);
        islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints);
    preflight.emptyIsland = preflight.reason == IslandConstraintSolveRejectReason::EmptyIsland;
    preflight.staleConstraintRefs = preflight.reason == IslandConstraintSolveRejectReason::StaleConstraintRefs;
    preflight.noMovableBodies = preflight.reason == IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    preflight.reason = classifyIslandConstraintSolveReject(preflight);
    if (preflight.refs.reason != IslandSolveRejectReason::None) {
        preflight.reason = preflight.refs.reason;
    } else if (preflight.bodies.reason != IslandSolveRejectReason::None) {
        preflight.reason = preflight.bodies.reason;
    preflight.reason = islandConstraintSolveRejectReason(preflight);
        preflight.reason = IslandSolveRejectReason::StaleConstraintRefs;
        preflight.reason = IslandSolveRejectReason::NoMovableBodies;
        preflight.skipped = true;
        return preflight;
    if (!preflight.bodies.can_solve()) {
    preflight.reason = island_solve_reject_reason(island, bodies, contacts, distanceConstraints, 1.f);
    }
    return preflight;

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    case IslandConstraintSolveRejectReason::AllSleeping:
        return "AllSleeping";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    return "Unknown";

        preflight.reason = IslandConstraintSolveRejectReason::NoInRangeRefs;

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints).reason;

bool island_constraint_solve_rejects_for_reason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    IslandConstraintSolvePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    return preflight_island_constraint_solve(
        graph.island(islandIndex), bodies, contacts, distanceConstraints);
    f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandConstraintSolveRejectReason::InvalidDt;
    }

    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints, dt) == expected;

bool solve_island_job_with_preflight(RigidBodySoA& bodies,
                                     SolverWorkBuffers& workBuffers,
                                     f32 contactCompliance,
                                     const std::function<f32(const RigidBodySoA&, u32)>& invMassFn,
                                     IslandConstraintSolvePreflight& outPreflight) {
    outPreflight = preflight_island_constraint_solve(
        island, bodies, workBuffers.contactManifolds(), distanceConstraints);
        outPreflight.reason = IslandConstraintSolveRejectReason::InvalidDt;
    if (!outPreflight.can_solve() || !is_finite_island_solve_dt(dt)) {
        return false;
    return solve_island_job(bodies,
                            island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);
}

IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolvePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        preflight.reason = IslandConstraintSolveRejectReason::EmptyIsland;
        return preflight;
    }
    return preflight_island_constraint_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);
}

bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                         const RigidBodySoA& bodies,
                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                         const std::vector<DistanceConstraint>& distanceConstraints) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) !=
           IslandConstraintSolveRejectReason::None;
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::StaleRefs:
        return "StaleRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    case IslandConstraintSolveRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
IslandSolveDispatchPreflight preflight_island_solve_dispatch(
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolvePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    return preflight_island_constraint_solve(
        graph.island(islandIndex), bodies, contacts, distanceConstraints);

IslandConstraintSolveGraphStats compute_island_constraint_solve_stats(
    IslandConstraintSolveGraphStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve(
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.can_solve()) {
            ++stats.solveableCount;
        } else if (!preflight.refs.can_solve()) {
            ++stats.blockedByRefsCount;
        } else {
            ++stats.blockedByBodiesCount;
    return stats;

IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(
    IslandConstraintSolveGraphPreflight preflight{};
    preflight.stats =
        compute_island_constraint_solve_stats(graph, bodies, contacts, distanceConstraints);
    preflight.skipped = !preflight.has_solveable_islands();

bool should_skip_island_constraint_solve_graph(
    return preflight_island_constraint_solve_graph(graph, bodies, contacts, distanceConstraints).skipped;


bool should_skip_island_constraint_solve_by_index(const ContactIslandGraph& graph,
    return !preflight_island_constraint_solve_by_index(
               graph, islandIndex, bodies, contacts, distanceConstraints)
                .can_solve();

IslandSolvePipelinePreflight preflight_island_solve_pipeline(
    const ContactIslandGraph::Island& island,
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;

    const IslandConstraintRefsPreflight refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    if (!refs.can_solve()) {
        return IslandConstraintSolveRejectReason::StaleRefs;

    const IslandSleepPreflight sleep = preflight_island_sleep(island, bodies);
    if (sleep.can_skip_solve()) {
        return IslandConstraintSolveRejectReason::AllSleeping;

    const IslandSolveBodiesPreflight bodyPreflight = preflight_island_solve_bodies(island, bodies);
    if (!bodyPreflight.can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;

    return IslandConstraintSolveRejectReason::None;

bool island_constraint_solve_reject_reason_is(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,
    return !should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints);

IslandConstraintSolveJobPreflight preflight_solve_island_job_with_bodies(
    const IslandSolveJob& job,
    f32 dt) {
    IslandConstraintSolveJobPreflight preflight{};
    preflight.job = preflight_solve_island_job(job, dt);
    if (should_skip_island_solve_job(job)) {
    preflight.solve = preflight_island_constraint_solve(*job.island, bodies, contacts, distanceConstraints);

bool should_skip_solve_island_job_with_bodies(const IslandSolveJob& job,
    return !preflight_solve_island_job_with_bodies(job, bodies, contacts, distanceConstraints, dt).can_solve();
    return preflight_island_constraint_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);

    return !preflight_island_constraint_solve_by_index(graph, islandIndex, bodies, contacts, distanceConstraints)





    return !preflight_island_constraint_solve_by_index(graph,
                                                       islandIndex,
                                                       bodies,
                                                       contacts,
                                                       distanceConstraints)



        const IslandConstraintSolvePreflight preflight =
            preflight_island_constraint_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
            preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);
            ++stats.staleRefsCount;
        } else if (preflight.bodies.sleepingCount > 0u && preflight.bodies.movableCount == 0u) {
            ++stats.allSleepingCount;



    preflight.skipped = preflight.stats.solveableCount == 0u;


std::vector<u32> collect_solveable_island_indices(

    preflight.stats = compute_island_constraint_solve_stats(graph, bodies, contacts, distanceConstraints);
    preflight.skipped = !preflight.can_solve_graph();




    IslandSolveDispatchPreflight preflight{};

    preflight.constraint = preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);
    preflight.sleep = preflight_island_sleep(island, bodies);

IslandSolveDispatchPreflight preflight_island_solve_dispatch_by_index(
    return preflight_island_solve_dispatch(graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_island_solve_dispatch(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_dispatch(island, bodies, contacts, distanceConstraints).can_dispatch();

IslandSolveableStats compute_island_solveable_stats(

    IslandSolveableStats stats{};
        const IslandSolveDispatchPreflight preflight =
            preflight_island_solve_dispatch(graph.island(islandIndex), bodies, contacts, distanceConstraints);
        } else if (preflight.sleep.can_skip_solve()) {
        } else if (preflight.can_dispatch()) {
            ++stats.blockedCount;

u32 count_solveable_islands(const ContactIslandGraph& graph,
    return compute_island_solveable_stats(graph, bodies, contacts, distanceConstraints).solveableCount;

bool has_solveable_islands(const ContactIslandGraph& graph,
    return count_solveable_islands(graph, bodies, contacts, distanceConstraints) > 0u;

    IslandSolvePipelinePreflight preflight{};

    preflight.wake = preflight_island_wake(island, bodies);
    preflight.constraintSolve =

IslandSolvePipelinePreflight preflight_island_solve_pipeline_by_index(
    return preflight_island_solve_pipeline(

bool should_skip_island_solve_pipeline(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_pipeline(island, bodies, contacts, distanceConstraints).can_solve();

bool should_skip_island_solve_pipeline_by_index(const ContactIslandGraph& graph,
    return !preflight_island_solve_pipeline_by_index(

std::vector<u32> collect_constraint_solveable_island_indices(


        } else if (preflight.bodies.sleepingCount > 0u &&
                   preflight.bodies.movableCount == 0u) {
            ++stats.noMovableCount;






    return compute_island_constraint_solve_stats(graph, bodies, contacts, distanceConstraints).solveableCount;


            ++stats.noInRangeRefsCount;
            ++stats.noMovableBodiesCount;

        } else if (preflight_island_sleep(island, bodies).allSleeping) {


    return !preflight_island_constraint_solve_graph(graph, bodies, contacts, distanceConstraints)
                .has_solveable_islands();

    preflight.skipped = !preflight.can_solve();

bool should_skip_island_constraint_solve_graph(const ContactIslandGraph& graph,


IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
                                                                  const RigidBodySoA& bodies) {
    IslandSolveBodiesPreflight preflight{};
    return preflight_island_solve_bodies(graph.island(islandIndex), bodies);

IslandFullSolvePreflight preflight_island_full_solve(const ContactIslandGraph::Island& island,
    IslandFullSolvePreflight preflight{};
    preflight.invalidDt = !is_finite_island_solve_dt(dt);

    preflight.constraints = preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);

IslandFullSolvePreflight preflight_island_full_solve_by_index(const ContactIslandGraph& graph,
    return preflight_island_full_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints, dt);

bool should_skip_island_full_solve(const ContactIslandGraph::Island& island,
    return !preflight_island_full_solve(island, bodies, contacts, distanceConstraints, dt).can_solve();

std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,

bool should_skip_island_constraint_solve_by_index(




    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        if (preflight.can_solve()) {
            indices.push_back(islandIndex);
    return indices;

IslandSolvePassPreflight preflight_island_solve_pass(
    IslandSolvePassPreflight preflight{};


IslandSolvePassPreflight preflight_island_solve_pass_by_index(
    return preflight_island_solve_pass(graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_island_solve_pass(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_pass(island, bodies, contacts, distanceConstraints).can_solve();

bool solve_island_job_guarded(RigidBodySoA& bodies,
                              SolverWorkBuffers& workBuffers,



        if (preflight.can_dispatch()) {

IslandSleepSolveDispatchPreflight preflight_island_sleep_dispatch(const ContactIslandGraph& graph,
    IslandSleepSolveDispatchPreflight preflight{};
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.skipped = preflight.dispatch.skipped;

bool should_skip_island_sleep_dispatch(const ContactIslandGraph& graph,
    return !preflight_island_sleep_dispatch(graph, bodies, dt).can_dispatch();

        const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve_by_index(
            graph, islandIndex, bodies, contacts, distanceConstraints);

std::vector<u32> collect_pipeline_solveable_island_indices(
        const IslandSolvePipelinePreflight preflight = preflight_island_solve_pipeline_by_index(




IslandWakeAndSolvePreflight preflight_island_wake_and_solve(
    IslandWakeAndSolvePreflight preflight{};

    preflight.solve = preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);

IslandWakeAndSolvePreflight preflight_island_wake_and_solve_by_index(
    return preflight_island_wake_and_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);






bool should_skip_island_constraint_solve_index(const ContactIslandGraph& graph,

IslandConstraintSolveDispatchPreflight preflight_island_constraint_solve_dispatch(
    IslandConstraintSolveDispatchPreflight preflight{};
    preflight.skipped = preflight.solve.skipped;

IslandConstraintSolveDispatchPreflight preflight_island_constraint_solve_dispatch_by_index(
    return preflight_island_constraint_solve_dispatch(
        graph.island(islandIndex), bodies, contacts, distanceConstraints, dt);

bool should_skip_island_constraint_solve_dispatch(
    return !preflight_island_constraint_solve_dispatch(island, bodies, contacts, distanceConstraints, dt).can_solve();

        const IslandFullSolvePreflight preflight =
            preflight_island_full_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints, dt);


bool can_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
    return should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints);

    return !can_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints);


                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints)) {
    const IslandSolvePassPreflight preflight = preflight_island_solve_pass(
        island, bodies, workBuffers.contactManifolds(), distanceConstraints);
    if (!preflight.can_solve()) {
        return false;
    if (should_skip_island_sleep_solve(island, bodies)) {
    if (should_skip_island_solve_dispatch(island, bodies, contacts, distanceConstraints)) {
    if (!is_finite_island_solve_dt(dt)) {
    return solve_island_job(bodies,
                            island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);

IslandDispatchResult dispatch_solve_island_with_bodies_result(
    RigidBodySoA& bodies,
    IslandDispatchResult result{};
    result.islandIndex = islandIndex;
    const IslandSolveJob job = extract_island(graph, islandIndex);
    if (should_skip_solve_island_job_with_bodies(job, bodies, contacts, distanceConstraints, dt)) {
        result.skipped = true;
        return result;
    result.solved = solve_island_job_guarded(bodies,
                                             *job.island,
    result.skipped = !result.solved;


        if (job.island == nullptr) {
            continue;
        if (should_skip_island_constraint_solve(*job.island, bodies, contacts, distanceConstraints)) {

IslandBatchBodiesDispatchResult dispatch_all_islands_with_bodies_result(
    IslandBatchBodiesDispatchResult result{};

    const std::vector<u32> solveable = collect_solveable_island_indices(graph, bodies, contacts, distanceConstraints);
    result.solveableCount = static_cast<u32>(solveable.size());
    if (result.solveableCount == 0u) {

    for (u32 islandIndex : solveable) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_with_bodies_result(bodies,
                                                                                             graph,
        if (dispatchResult.solved) {
            ++result.solvedCount;
            ++result.skippedCount;

bool dispatch_solve_island_constraint_guarded(RigidBodySoA& bodies,
    if (!is_valid_island_solve_dt(dt)) {
    if (!should_solve_island(job) || job.island == nullptr) {
    return solve_island_job_guarded(bodies,
        if (!should_solve_island(job)) {
        const IslandSleepPreflight sleepPreflight = preflight_island_sleep(*job.island, bodies);
        if (sleepPreflight.skipped || sleepPreflight.can_skip_solve()) {

IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             u32 islandIndex) {
    IslandWakeResult result{};

    if (!wake_island_sleepers_guarded(bodies, island)) {

    result.woke = true;

bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,
    return dispatch_solve_island_with_wake_result(bodies,
                                                  invMassFn)
        .solved;

IslandWakeAndSolveResult dispatch_solve_island_with_wake_result(
    IslandWakeAndSolveResult result{};


    const IslandWakeResult wakeResult = wake_island_sleepers_result(bodies, graph, islandIndex);
    result.woke = wakeResult.woke;


u32 dispatch_all_islands_with_wake_guarded(
    return dispatch_all_islands_with_wake_result(bodies,
        .dispatch.solvedCount;

IslandWakeAndDispatchResult dispatch_all_islands_with_wake_result(
    IslandWakeAndDispatchResult result{};
    const IslandDispatchPreflight preflight = preflight_island_dispatch(graph, dt);
    result.dispatch.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.dispatch.skipped = true;
        result.dispatch.skippedCount = result.dispatch.dispatchableCount;

    result.wokeCount = wake_all_island_sleepers_guarded(bodies, graph);

    for (u32 islandIndex : collect_solveable_island_indices(graph, bodies)) {
        const IslandWakeAndSolveResult islandResult = dispatch_solve_island_with_wake_result(
        if (islandResult.solved) {
            ++result.dispatch.solvedCount;
            ++result.dispatch.skippedCount;


IslandWakeThenSolvePreflight preflight_wake_then_solve_island(
    IslandWakeThenSolvePreflight preflight{};


IslandWakeThenSolvePreflight preflight_wake_then_solve_island_by_index(
    return preflight_wake_then_solve_island(graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_wake_then_solve_island(const ContactIslandGraph::Island& island,
    return !preflight_wake_then_solve_island(island, bodies, contacts, distanceConstraints).can_wake_then_solve();

    if (!is_valid_island_solve_dt(dt) || !island_index_valid(graph, islandIndex)) {

    if (should_skip_wake_then_solve_island(island, bodies, contacts, distanceConstraints)) {

    wake_island_sleepers_guarded(bodies, island);

        if (!should_solve_island(extract_island(graph, islandIndex))) {

IslandNonsleepingDispatchResult dispatch_all_nonsleeping_islands_result(
    IslandNonsleepingDispatchResult result{};

    const std::vector<u32> solveableIndices =
        collect_solveable_island_indices(graph, bodies, contacts, distanceConstraints);
    result.solveableCount = static_cast<u32>(solveableIndices.size());
    if (solveableIndices.empty()) {

    for (u32 islandIndex : solveableIndices) {
        if (dispatch_solve_island_with_wake_guarded(bodies,
                                                    invMassFn)) {

u32 dispatch_all_nonsleeping_islands(RigidBodySoA& bodies,
    return dispatch_all_nonsleeping_islands_result(bodies,
        .solvedCount;



bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
    if (!is_finite_island_solve_dt(dt) || !should_solve_island(job) || job.island == nullptr) {


bool dispatch_solve_island_guarded(RigidBodySoA& bodies,
                                   const std::function<f32(const RigidBodySoA&, u32)>& invMassFn,
                                   bool wakeSleepers) {
    return dispatch_solve_island_guarded_result(bodies,
                                                invMassFn,
                                                wakeSleepers)

IslandDispatchResult dispatch_solve_island_guarded_result(


        preflight_island_solve_dispatch(*job.island, bodies, contacts, distanceConstraints);

    if (wakeSleepers) {
        wake_island_sleepers_guarded(bodies, *job.island);

    result.solved = solve_island_job(bodies,

IslandBatchDispatchResult dispatch_solveable_islands_result(
    IslandBatchDispatchResult result{};
    const IslandSleepSolveDispatchPreflight preflight = preflight_island_sleep_dispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
        result.skippedCount = result.dispatchableCount;

    for (u32 islandIndex : collect_solveable_island_indices(graph, bodies, contacts, distanceConstraints)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_guarded_result(bodies,
                                                                                         wakeSleepers);

u32 dispatch_solveable_islands(RigidBodySoA& bodies,
    return dispatch_solveable_islands_result(bodies,

IslandConstraintSolveResult solve_island_job_guarded_result(
    IslandConstraintSolveResult result{};


bool solve_island_job_with_wake_guarded(RigidBodySoA& bodies,
    const IslandSolvePipelinePreflight preflight =
        preflight_island_solve_pipeline(island, bodies, contacts, distanceConstraints);
    if (preflight.should_wake_sleepers()) {


IslandDispatchResult dispatch_solve_island_with_wake_result(


    result.solved = solve_island_job_with_wake_guarded(bodies,


IslandBatchWakeDispatchResult dispatch_all_islands_with_wake_result(
    IslandBatchWakeDispatchResult result{};
    const IslandDispatchPreflight dispatchPreflight = preflight_island_dispatch(graph, dt);
    result.dispatchableCount = dispatchPreflight.solve.stats.dispatchableCount;
    if (!dispatchPreflight.can_dispatch()) {

    for (u32 islandIndex : collect_pipeline_solveable_island_indices(
             graph, bodies, contacts, distanceConstraints)) {
        if (wake_island_sleepers_by_index_guarded(bodies, graph, islandIndex)) {
            ++result.wokeCount;

        const IslandConstraintSolveResult solveResult = solve_island_job_guarded_result(bodies,
        if (solveResult.solved) {

IslandDispatchWithWakePreflight preflight_dispatch_with_wake(const ContactIslandGraph& graph,
    IslandDispatchWithWakePreflight preflight{};

    preflight.wake = preflight_island_wake(graph.island(islandIndex), bodies);

bool dispatch_solve_island_wake_guarded(RigidBodySoA& bodies,
    const IslandDispatchWithWakePreflight preflight = preflight_dispatch_with_wake(graph, islandIndex, bodies, dt);

    if (preflight.should_wake_first()) {
        wake_island_sleepers_guarded(bodies, graph.island(islandIndex));

    return dispatch_solve_island_job_guarded(bodies,
                                             extract_island(graph, islandIndex),






    return dispatch_solve_island_constraint_result(bodies,

IslandDispatchResult dispatch_solve_island_constraint_result(






    const IslandWakeAndSolvePreflight preflight =
        preflight_island_wake_and_solve(island, bodies, contacts, distanceConstraints);


    return dispatch_solve_island_guarded(bodies,

bool dispatch_solve_island_job_with_wake_guarded(RigidBodySoA& bodies,

        preflight_island_wake_and_solve(*job.island, bodies, contacts, distanceConstraints);


                                             job,

u32 dispatch_all_solveable_islands_guarded(
    return dispatch_all_solveable_islands_result(bodies,

u32 dispatch_all_solveable_islands_with_wake_guarded(
    return dispatch_all_solveable_islands_with_wake_result(bodies,

IslandBatchDispatchResult dispatch_all_solveable_islands_result(
    const IslandConstraintSolveGraphPreflight preflight =
        preflight_island_constraint_solve_graph(graph,
                                                workBuffers.contactManifolds(),
                                                distanceConstraints);
    result.dispatchableCount = preflight.stats.solveableCount;
    if (!preflight.can_solve() || !is_finite_island_solve_dt(dt)) {

    for (u32 islandIndex : collect_solveable_island_indices(
             graph, bodies, workBuffers.contactManifolds(), distanceConstraints)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_guarded_result(
            bodies, graph, islandIndex, workBuffers, distanceConstraints, dt, contactCompliance, invMassFn);




bool dispatch_solve_island_after_wake_guarded(RigidBodySoA& bodies,

    return dispatch_solve_island_after_wake_result(bodies,

IslandDispatchResult dispatch_solve_island_after_wake_result(

    const IslandWakeThenSolvePreflight preflight =
        preflight_wake_then_solve_island(*job.island, bodies, contacts, distanceConstraints);

    if (preflight.can_wake()) {


        const IslandConstraintSolvePreflight solvePreflight =
        if (solvePreflight.skipped) {
        if (solvePreflight.can_solve()) {
        if (!solvePreflight.refs.can_solve()) {
        } else if (!solvePreflight.bodies.can_solve()) {
            if (preflight_island_sleep(island, bodies).allSleeping) {

IslandSolveableGraphPreflight preflight_island_solveable_graph(
    IslandSolveableGraphPreflight preflight{};
    preflight.stats = compute_island_solveable_stats(graph, bodies, contacts, distanceConstraints);

bool should_skip_island_solveable_graph(const ContactIslandGraph& graph,
    return preflight_island_solveable_graph(graph, bodies, contacts, distanceConstraints).skipped;



    const IslandSolveableGraphPreflight preflight =
        preflight_island_solveable_graph(graph,
    if (!preflight.has_solveable_islands() || !is_finite_island_solve_dt(dt)) {

    for (u32 islandIndex : collect_solveable_island_indices(graph,
                                                            distanceConstraints)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_constraint_result(

IslandBatchDispatchResult dispatch_all_solveable_islands_with_wake_result(

        const bool solved = dispatch_solve_island_job_with_wake_guarded(bodies,
        if (solved) {
    if (!is_finite_island_solve_dt(dt) || !island_index_valid(graph, islandIndex)) {

u32 dispatch_all_islands_constraint_guarded(

    return dispatch_solve_island_job_guarded_result(bodies,

IslandDispatchResult dispatch_solve_island_job_guarded_result(
    return solve_island_job_guarded_result(bodies,

        preflight_island_constraint_solve_graph(
            graph, bodies, workBuffers.contactManifolds(), distanceConstraints);
        return 0u;

    u32 solvedCount = 0u;
    for (u32 islandIndex :
         collect_constraint_solveable_island_indices(
        if (dispatch_solve_island_constraint_guarded(bodies,
            ++solvedCount;
    return solvedCount;
    result.islandIndex = job.islandIndex;
    const IslandConstraintSolveDispatchPreflight preflight = preflight_island_constraint_solve_dispatch(
        island, bodies, workBuffers.contactManifolds(), distanceConstraints, dt);


IslandConstraintSolveResult solve_island_job_by_index_guarded_result(
    result = solve_island_job_guarded_result(bodies,
                                             graph.island(islandIndex),

bool dispatch_solve_island_full_guarded(RigidBodySoA& bodies,
    return dispatch_solve_island_full_result(bodies,

IslandDispatchResult dispatch_solve_island_full_result(RigidBodySoA& bodies,

        preflight_island_full_solve(island, bodies, contacts, distanceConstraints, dt);


IslandPreSolvePreflight preflight_island_pre_solve(
    default:

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(

bool islandConstraintSolveRejectsForReason(const ContactIslandGraph::Island& island,
    return islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints) == expected;

IslandConstraintSolveRejectPreflight preflight_island_constraint_solve_reject(
    IslandPreSolvePreflight preflight{};


IslandPreSolvePreflight preflight_island_pre_solve_by_index(
    return preflight_island_pre_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_island_pre_solve(const ContactIslandGraph::Island& island,
    return !preflight_island_pre_solve(island, bodies, contacts, distanceConstraints).can_pre_solve();

IslandPreSolveGraphStats compute_island_pre_solve_stats(
    IslandPreSolveGraphStats stats{};
        const IslandPreSolvePreflight preflight =
            preflight_island_pre_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);
        } else if (preflight.can_pre_solve()) {
            ++stats.preSolveableCount;
        if (preflight.needs_wake()) {
            ++stats.wakeableCount;

IslandPreSolveGraphPreflight preflight_island_pre_solve_graph(
    IslandPreSolveGraphPreflight preflight{};
    preflight.stats = compute_island_pre_solve_stats(graph, bodies, contacts, distanceConstraints);
    preflight.skipped = preflight.stats.preSolveableCount == 0u;

std::vector<u32> collect_pre_solveable_island_indices(
        if (preflight.can_pre_solve()) {

            preflight_island_pre_solve(*job.island, bodies, contacts, distanceConstraints);

IslandConstraintSolveResult pre_solve_island_guarded_result(

u32 dispatch_all_islands_full_guarded(RigidBodySoA& bodies,
    return dispatch_all_islands_full_result(bodies,

IslandBatchConstraintSolveResult dispatch_all_islands_full_result(
    const IslandPreSolvePreflight preflight = preflight_island_pre_solve(
    if (!preflight.can_pre_solve()) {


    IslandBatchConstraintSolveResult result{};

    for (u32 islandIndex : collect_solveable_island_indices(graph, bodies, contacts, distanceConstraints, dt)) {
        ++result.solveableCount;
        const IslandDispatchResult dispatchResult = dispatch_solve_island_full_result(bodies,


bool solve_island_with_wake_guarded(RigidBodySoA& bodies,
    if (should_skip_island_full_solve(island, bodies, contacts, distanceConstraints, dt)) {

    IslandConstraintSolveRejectPreflight preflight{};
    preflight.reason = islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints);
    preflight.emptyIsland = preflight.reason == IslandConstraintSolveRejectReason::EmptyIsland;
    preflight.staleRefs = preflight.reason == IslandConstraintSolveRejectReason::StaleRefs;
    preflight.noMovableBodies = preflight.reason == IslandConstraintSolveRejectReason::NoMovableBodies;

bool canSkipIslandConstraintSolve(const ContactIslandGraph::Island& island,
    return !preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();

bool shouldRunIslandConstraintSolve(const ContactIslandGraph::Island& island,
    return preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();


    if (job.island == nullptr || job.empty) {

        preflight_island_constraint_solve(*job.island, bodies, contacts, distanceConstraints);
    preflight.sleep = preflight_island_sleep(*job.island, bodies);

        extract_island(graph, islandIndex), bodies, contacts, distanceConstraints, dt);

bool should_skip_island_solve_pipeline(const IslandSolveJob& job,
    return !preflight_island_solve_pipeline(job, bodies, contacts, distanceConstraints, dt).can_dispatch();

IslandPipelineDispatchStats compute_island_pipeline_dispatch_stats(
    IslandPipelineDispatchStats stats{};
        if (should_solve_island(job)) {
            ++stats.dispatchableCount;

        const IslandSolvePipelinePreflight pipelinePreflight =
            preflight_island_solve_pipeline(job, bodies, contacts, distanceConstraints, dt);
        if (pipelinePreflight.can_dispatch()) {
        } else if (!pipelinePreflight.skipped && pipelinePreflight.sleep.can_skip_solve()) {
            ++stats.sleepSkippedCount;
        } else if (!pipelinePreflight.skipped && !pipelinePreflight.constraintSolve.can_solve()) {
            ++stats.constraintSkippedCount;

        const IslandWakePreflight wakePreflight = preflight_island_wake(graph.island(islandIndex), bodies);
        if (wakePreflight.should_wake_sleepers()) {

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(
    IslandPipelineDispatchPreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.stats = compute_island_pipeline_dispatch_stats(graph, bodies, contacts, distanceConstraints, dt);
    preflight.skipped = !preflight.can_run() || preflight.stats.solveableCount == 0u;

bool should_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
    return !preflight_island_pipeline_dispatch(graph, bodies, contacts, distanceConstraints, dt).can_run() ||
           compute_island_pipeline_dispatch_stats(graph, bodies, contacts, distanceConstraints, dt).solveableCount ==
               0u;



    result.woke = wake_island_sleepers_guarded(bodies, island);
    result.skipped = !result.woke;

bool dispatch_solve_island_pipeline_guarded(RigidBodySoA& bodies,
    return dispatch_solve_island_pipeline_result(bodies,

IslandPipelineDispatchResult dispatch_solve_island_pipeline_result(
    IslandPipelineDispatchResult result{};


    result.woke = wake_island_sleepers_guarded(bodies, *job.island);
    result.solved = dispatch_solve_island_job(bodies,

u32 dispatch_all_islands_pipeline_guarded(RigidBodySoA& bodies,
    return dispatch_all_islands_pipeline_result(bodies,

IslandPipelineBatchDispatchResult dispatch_all_islands_pipeline_result(
    IslandPipelineBatchDispatchResult result{};
    const IslandPipelineDispatchPreflight preflight = preflight_island_pipeline_dispatch(
        graph, bodies, workBuffers.contactManifolds(), distanceConstraints, dt);
    result.solveableCount = preflight.stats.solveableCount;
        result.skippedCount = result.solveableCount;

         collect_solveable_island_indices(graph, bodies, workBuffers.contactManifolds(), distanceConstraints, dt)) {
        const IslandPipelineDispatchResult dispatchResult = dispatch_solve_island_pipeline_result(
        if (dispatchResult.woke) {
const char* islandSleepRejectReasonName(IslandSleepRejectReason reason) {
    case IslandSleepRejectReason::None:
    case IslandSleepRejectReason::EmptyIsland:
    case IslandSleepRejectReason::AllSleeping:

IslandSleepRejectReason islandSleepRejectReason(const ContactIslandGraph::Island& island,
        return IslandSleepRejectReason::EmptyIsland;

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
            ++activeDynamicCount;

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;
    return IslandSleepRejectReason::None;

bool islandSleepRejectsForReason(const ContactIslandGraph::Island& island,
                                 IslandSleepRejectReason expected) {
    return islandSleepRejectReason(island, bodies) == expected;

IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
    preflight.reason = island_sleep_reject_reason(island, bodies);
    if (preflight.reason == IslandSleepRejectReason::EmptyIsland) {
    preflight.reason = island_sleep_solve_reject_reason(island, bodies);
    if (!island_has_constraints(island)) {
    preflight.reason = islandSleepRejectReason(island, bodies);
        preflight.reason = IslandSleepRejectReason::EmptyIsland;
        preflight.skipped = true;
        preflight.reason = IslandConstraintSolveRejectReason::EmptyIsland;
        preflight.reason = IslandSleepRejectReason::EmptyIsland;
        preflight.reason = IslandSleepSolveRejectReason::EmptyIsland;
        preflight.emptyIsland = true;
        preflight.reason = classifyIslandSleepReject(preflight);
        return preflight;
    }
    return preflight_island_constraint_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);
    preflight.skipped = preflight.reason == IslandSleepRejectReason::EmptyIsland;

IslandConstraintSolveGraphStats compute_island_constraint_solve_stats(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolveGraphStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve(
            graph.island(islandIndex), bodies, contacts, distanceConstraints);
        if (preflight.skipped || preflight.reason == IslandConstraintSolveRejectReason::EmptyIsland) {
            ++stats.emptyCount;
        } else if (preflight.can_solve()) {
            ++stats.solveableCount;
        } else if (preflight.reason == IslandConstraintSolveRejectReason::StaleConstraintRefs) {
            ++stats.staleRefsCount;
        } else if (preflight.reason == IslandConstraintSolveRejectReason::AllSleeping) {
            ++stats.allSleepingCount;
        } else if (preflight.reason == IslandConstraintSolveRejectReason::NoMovableBodies) {
            ++stats.noMovableCount;
    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            ++preflight.outOfRangeBodyCount;
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            ++preflight.staticOrKinematicCount;
        if (is_body_sleeping(flags)) {
            ++preflight.sleepingCount;
        } else {
            ++preflight.activeDynamicCount;
        }
    }
    return stats;
}

u32 count_solveable_islands(const ContactIslandGraph& graph,
                            const RigidBodySoA& bodies,
                            const std::vector<narrowphase::ContactManifold>& contacts,
                            const std::vector<DistanceConstraint>& distanceConstraints) {
    return compute_island_constraint_solve_stats(graph, bodies, contacts, distanceConstraints).solveableCount;
}

bool has_solveable_islands(const ContactIslandGraph& graph,
    return count_solveable_islands(graph, bodies, contacts, distanceConstraints) > 0u;

IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(
    const ContactIslandGraph& graph,
    IslandConstraintSolveGraphPreflight preflight{};
    preflight.stats = compute_island_constraint_solve_stats(graph, bodies, contacts, distanceConstraints);
    preflight.skipped = preflight.stats.solveableCount == 0u;
    preflight.allSleeping = preflight.activeDynamicCount == 0u && preflight.sleepingCount > 0u;
    preflight.reason = island_sleep_reject_reason(island, bodies);
    preflight.skipped = preflight.reason == IslandSleepRejectReason::EmptyIsland;
    if (preflight.allSleeping) {
        preflight.reason = IslandSleepSolveRejectReason::AllSleeping;
    preflight.reason = diagnose_island_sleep_reject(island, bodies);
        preflight.reason = IslandSleepRejectReason::AllSleeping;
    preflight.allSleeping = preflight.reason == IslandSleepRejectReason::AllSleeping;
    preflight.reason = islandSleepRejectReason(island, bodies);
    preflight.emptyIsland = preflight.reason == IslandSleepRejectReason::EmptyIsland;
    }
    preflight.reason = classifyIslandSleepReject(preflight);
    if (!preflight.allSleeping) {
        preflight.reason = IslandSleepRejectReason::NotAllSleeping;
    preflight.reason = islandSleepRejectReason(preflight);
    if (!preflight.can_skip_solve()) {
    return preflight;
}

std::vector<u32> collect_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve(
            graph.island(islandIndex), bodies, contacts, distanceConstraints);
        if (preflight.can_solve()) {
            indices.push_back(islandIndex);
    return indices;

IslandDispatchDeepenPreflight preflight_island_dispatch_deepen(
    u32 islandIndex,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt) {
    IslandDispatchDeepenPreflight preflight{};
    if (preflight.allSleeping) {
        preflight.reason = IslandSleepRejectReason::AllSleeping;

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies) {
    return preflight_island_sleep(island, bodies).reason;

bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     IslandSleepRejectReason expected) {
    return island_sleep_reject_reason(island, bodies) == expected;

IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
    IslandSleepPreflight preflight{};
    preflight.reason = island_sleep_reject_reason_by_index(graph, islandIndex, bodies);
    if (preflight.reason == IslandSleepRejectReason::OutOfRangeIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        preflight.reason = IslandSleepRejectReason::OutOfRangeIndex;
        preflight.reason = IslandSleepRejectReason::OutOfRangeIslandIndex;
        preflight.reason = IslandSleepRejectReason::OutOfRangeIsland;
        preflight.skipped = true;
        preflight.reason = IslandSleepRejectReason::OutOfRangeIndex;
        preflight.reason = IslandSleepRejectReason::OutOfRangeIsland;
        preflight.reason = classifyIslandSleepReject(preflight);
        return preflight;
    }

    preflight.dispatch = preflight_island_dispatch(graph, dt);
    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    preflight.constraintSolve = preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);
    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.skipped = preflight.dispatch.skipped;
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies) {
    IslandWakePreflight preflight{};
    preflight.reason = island_wake_reject_reason(island, bodies);
    if (preflight.reason == IslandWakeRejectReason::EmptyIsland) {
    if (!island_has_constraints(island)) {
        preflight.reason = IslandWakeRejectReason::EmptyIsland;
        preflight.skipped = true;
        preflight.reason = IslandWakeRejectReason::EmptyIsland;
        return preflight;
    }
    preflight.skipped = preflight.reason == IslandWakeRejectReason::EmptyIsland;

    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            ++preflight.outOfRangeBodyCount;
            continue;
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
        if (is_body_sleeping(flags)) {
            ++preflight.sleepingCount;
        } else {
            ++preflight.activeDynamicCount;

    preflight.hasMixedSleepState =
        preflight.sleepingCount > 0u && preflight.activeDynamicCount > 0u;
    preflight.reason = island_wake_reject_reason(island, bodies);
    preflight.reason = diagnose_island_wake_reject(island, bodies);
    if (!preflight.hasMixedSleepState || preflight.activeDynamicCount == 0u) {
        preflight.reason = IslandWakeRejectReason::NoMixedSleep;
    preflight.hasMixedSleepState = preflight.reason == IslandWakeRejectReason::None;

bool solve_island_job_with_preflight(RigidBodySoA& bodies,
                                     const ContactIslandGraph::Island& island,
                                     SolverWorkBuffers& workBuffers,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     f32 dt,
                                     f32 contactCompliance,
                                     const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve(
        island, bodies, workBuffers.contactManifolds(), distanceConstraints);
    if (!preflight.can_solve()) {
        return false;
    return solve_island_job(bodies,
                            island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);

bool dispatch_solve_island_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const IslandDispatchDeepenPreflight preflight = preflight_island_dispatch_deepen(
        graph, islandIndex, bodies, workBuffers.contactManifolds(), distanceConstraints, dt);
    if (!preflight.can_dispatch()) {
    const IslandSolveJob job = extract_island(graph, islandIndex);
    if (job.island == nullptr) {
                            *job.island,

IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies) {
    IslandSleepWakePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.reason = IslandWakeRejectReason::EmptyIsland;
        preflight.skipped = true;

    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);

IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
    if (!preflight.hasMixedSleepState) {
        preflight.reason = IslandWakeRejectReason::NoMixedSleepState;
    } else if (preflight.activeDynamicCount == 0u) {
        preflight.reason = IslandWakeRejectReason::NoActiveDynamic;

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
    return preflight_island_wake(island, bodies).reason;

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;

IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
    IslandWakePreflight preflight{};
    preflight.reason = island_wake_reject_reason_by_index(graph, islandIndex, bodies);
    if (preflight.reason == IslandWakeRejectReason::OutOfRangeIndex) {
        preflight.reason = IslandWakeRejectReason::OutOfRangeIndex;
    return preflight_island_sleep_wake(graph.island(islandIndex), bodies);



    if (!island_index_valid(graph, islandIndex)) {
        preflight.reason = IslandSleepRejectReason::OutOfRangeIsland;

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    return "Unknown";

IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,
        return IslandWakeRejectReason::EmptyIsland;

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
            ++sleepingCount;
            ++activeDynamicCount;

    if (sleepingCount > 0u && activeDynamicCount > 0u) {
        return IslandWakeRejectReason::None;
    return IslandWakeRejectReason::NoMixedSleepState;

bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
    return islandWakeRejectReason(island, bodies) == expected;

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
    preflight.reason = islandWakeRejectReason(island, bodies);
    if (preflight.reason == IslandWakeRejectReason::EmptyIsland) {
        preflight.emptyIsland = true;
        preflight.reason = classifyIslandWakeReject(preflight);
        preflight.reason = IslandWakeRejectReason::EmptyIsland;
        return preflight;
    }

    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);
    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
        if (is_body_sleeping(flags)) {
            ++preflight.sleepingCount;
        } else {
            ++preflight.activeDynamicCount;

    preflight.reason = islandWakeRejectReason(island, bodies);
    preflight.hasMixedSleepState = preflight.reason == IslandWakeRejectReason::None;
    preflight.emptyIsland = preflight.reason == IslandWakeRejectReason::EmptyIsland;
    preflight.hasMixedSleepState =
        preflight.sleepingCount > 0u && preflight.activeDynamicCount > 0u;
    if (!preflight.should_wake_sleepers()) {
        preflight.reason = IslandWakeRejectReason::NoMixedSleepState;
    }
    preflight.reason = island_wake_reject_reason(island, bodies);
    preflight.skipped = preflight.reason == IslandWakeRejectReason::EmptyIsland;
    preflight.reason = classifyIslandWakeReject(preflight);
    if (!preflight.hasMixedSleepState || preflight.activeDynamicCount == 0u) {
    preflight.reason = islandWakeRejectReason(preflight);
        preflight.reason = IslandWakeRejectReason::UniformSleepState;
    if (!preflight.hasMixedSleepState) {
        preflight.reason = IslandWakeRejectReason::NoWakeTarget;
    return preflight;
}

IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const RigidBodySoA& bodies) {
    IslandSleepWakePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.reason = IslandWakeRejectReason::OutOfRangeIsland;
        preflight.skipped = true;
        preflight.reason = classifyIslandWakeReject(preflight);
        return preflight;
    }
    return preflight_island_sleep_wake(graph.island(islandIndex), bodies);

IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
                                                     const RigidBodySoA& bodies) {
    IslandSleepWakePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.reason = IslandWakeRejectReason::OutOfRangeIslandIndex;
        preflight.reason = IslandWakeRejectReason::OutOfRangeIndex;

    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);

IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
    if (!island_index_valid(graph, islandIndex)) {



        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_sleep_wake(graph.island(islandIndex), bodies);
}

IslandSleepGraphStats compute_island_sleep_stats(const ContactIslandGraph& graph,
    IslandSleepGraphStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandSleepPreflight preflight = preflight_island_sleep(graph.island(islandIndex), bodies);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.allSleeping) {
            ++stats.allSleepingCount;
        } else if (preflight.sleepingCount > 0u) {
            ++stats.mixedSleepCount;
            ++stats.fullyActiveCount;
    return stats;

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::EmptyGraph:
        return "EmptyGraph";
    case IslandSleepGraphRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    }
    return "Unknown";
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    if (graph.islandCount() == 0u) {
        return IslandSleepGraphRejectReason::EmptyGraph;
    }

    const IslandSleepGraphStats stats = compute_island_sleep_stats(graph, bodies);
    if (stats.fullyActiveCount == 0u && stats.mixedSleepCount == 0u) {
        return IslandSleepGraphRejectReason::AllIslandsSleeping;
    }
    return IslandSleepGraphRejectReason::None;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::NoSolveableIslands:
        return "NoSolveableIslands";
    }
    return "Unknown";
}

IslandSleepGraphRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,
                                                          const RigidBodySoA& bodies) {
    const IslandSleepGraphStats stats = compute_island_sleep_stats(graph, bodies);
    if (stats.fullyActiveCount > 0u || stats.mixedSleepCount > 0u) {
        return IslandSleepGraphRejectReason::None;
    }
    return IslandSleepGraphRejectReason::NoSolveableIslands;
}

bool islandSleepGraphRejectsForReason(const ContactIslandGraph& graph,
                                      const RigidBodySoA& bodies,
                                      IslandSleepGraphRejectReason expected) {
    return islandSleepGraphRejectReason(graph, bodies) == expected;
}

IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
    IslandSleepGraphPreflight preflight{};
    preflight.reason = island_sleep_graph_reject_reason(graph, bodies);
    preflight.stats = compute_island_sleep_stats(graph, bodies);
    preflight.skipped = !preflight.has_solveable_islands();
    preflight.reason = island_sleep_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandSleepGraphRejectReason::None;
    preflight.skipped = preflight.reason == IslandSleepGraphRejectReason::AllIslandsSleeping;
    preflight.reason = islandSleepGraphRejectReason(graph, bodies);
    preflight.noSolveableIslands = preflight.reason == IslandSleepGraphRejectReason::NoSolveableIslands;
    preflight.skipped = preflight.noSolveableIslands;
    preflight.skipped = preflight.reason != IslandSleepRejectReason::None;
    if (preflight.skipped) {
        preflight.reason = IslandSleepGraphRejectReason::NoSolveableIslands;
    }
    preflight.reason = classifyIslandSleepGraphReject(preflight);
    if (preflight.stats.fullyActiveCount == 0u && preflight.stats.mixedSleepCount == 0u) {
        preflight.skipped = true;
    preflight.reason = islandSleepGraphRejectReason(preflight);
        preflight.reason = IslandSleepRejectReason::NotAllSleeping;
    if (!preflight.has_solveable_islands()) {
        preflight.reason = IslandSleepRejectReason::AllSleeping;
        preflight.reason = IslandSleepRejectReason::NoSolveableIslands;
    preflight.reason = preflight.skipped ? IslandSleepRejectReason::NoSolveableIslands
                                         : IslandSleepRejectReason::None;
    return preflight;
}

bool should_skip_island_sleep_solve_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph(graph, bodies).skipped;

bool should_skip_island_sleep_solve(const ContactIslandGraph::Island& island,
    return preflight_island_sleep(island, bodies).can_skip_solve();
                                    const RigidBodySoA& bodies) {
    return island_sleep_reject_reason(island, bodies) == IslandSleepRejectReason::AllSleeping;
}

bool can_skip_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return should_skip_island_sleep_solve(island, bodies);
}

bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !can_skip_island_sleep_solve(island, bodies);
}

IslandWakeGraphStats compute_island_wake_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    IslandWakeGraphStats stats{};
        const IslandWakePreflight preflight = preflight_island_wake(graph.island(islandIndex), bodies);
        } else if (preflight.should_wake_sleepers()) {
            ++stats.wakeableCount;

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::EmptyGraph:
        return "EmptyGraph";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    if (graph.islandCount() == 0u) {
        return IslandWakeGraphRejectReason::EmptyGraph;
    }

    const IslandWakeGraphStats stats = compute_island_wake_stats(graph, bodies);
    if (stats.wakeableCount == 0u) {
        return IslandWakeGraphRejectReason::NoWakeableIslands;
    }
    return IslandWakeGraphRejectReason::None;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    }
    return "Unknown";
}

IslandWakeGraphRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,
                                                        const RigidBodySoA& bodies) {
    const IslandWakeGraphStats stats = compute_island_wake_stats(graph, bodies);
    if (stats.wakeableCount > 0u) {
        return IslandWakeGraphRejectReason::None;
    }
    return IslandWakeGraphRejectReason::NoWakeableIslands;
}

bool islandWakeGraphRejectsForReason(const ContactIslandGraph& graph,
                                     const RigidBodySoA& bodies,
                                     IslandWakeGraphRejectReason expected) {
    return islandWakeGraphRejectReason(graph, bodies) == expected;
}

IslandWakeGraphPreflight preflight_island_wake_graph(const ContactIslandGraph& graph,
    IslandWakeGraphPreflight preflight{};
    preflight.reason = island_wake_graph_reject_reason(graph, bodies);
    preflight.stats = compute_island_wake_stats(graph, bodies);
    preflight.skipped = !preflight.can_wake();
    preflight.reason = island_wake_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandWakeGraphRejectReason::None;
    preflight.skipped = preflight.reason == IslandWakeGraphRejectReason::NoWakeableIslands;
    preflight.reason = islandWakeGraphRejectReason(graph, bodies);
    preflight.noWakeableIslands = preflight.reason == IslandWakeGraphRejectReason::NoWakeableIslands;
    preflight.skipped = preflight.noWakeableIslands;
    preflight.skipped = preflight.reason != IslandWakeRejectReason::None;
    if (preflight.skipped) {
        preflight.reason = IslandWakeGraphRejectReason::NoWakeableIslands;
    }
    preflight.reason = classifyIslandWakeGraphReject(preflight);
    if (preflight.stats.wakeableCount == 0u) {
        preflight.skipped = true;
    preflight.reason = islandWakeGraphRejectReason(preflight);
        preflight.reason = IslandWakeRejectReason::UniformSleepState;
    if (!preflight.can_wake()) {
        preflight.reason = IslandWakeRejectReason::NoWakeTarget;
        preflight.reason = IslandWakeRejectReason::NoWakeableIslands;
    preflight.reason = preflight.skipped ? IslandWakeRejectReason::NoWakeableIslands
                                         : IslandWakeRejectReason::None;
    return preflight;
}

bool should_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_wake_graph(graph, bodies).can_wake();

bool should_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_wake(island, bodies).should_wake_sleepers();

bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !should_skip_island_wake(island, bodies);
}

bool should_run_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !should_skip_island_wake_graph(graph, bodies);



IslandSolvePassPreflight preflight_island_solve_pass(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandSolvePassPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;

    preflight.constraint = preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);
    preflight.sleepState = preflight_island_sleep(island, bodies);
    preflight.wakeState = preflight_island_wake(island, bodies);

IslandSolvePassPreflight preflight_island_solve_pass_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    if (!island_index_valid(graph, islandIndex)) {
    return preflight_island_solve_pass(
        graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_island_solve_pass(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_pass(island, bodies, contacts, distanceConstraints).can_solve();

IslandNonsleepingDispatchPreflight preflight_nonsleeping_island_dispatch(const ContactIslandGraph& graph,
                                                                        f32 dt) {
    IslandNonsleepingDispatchPreflight preflight{};
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.sleepGraph = preflight_island_sleep_graph(graph, bodies);
    preflight.skipped = !preflight.can_dispatch();

bool should_skip_nonsleeping_island_dispatch(const ContactIslandGraph& graph,
    return !preflight_nonsleeping_island_dispatch(graph, bodies, dt).can_dispatch();

bool solve_island_job_guarded(RigidBodySoA& bodies,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const IslandSolvePassPreflight preflight = preflight_island_solve_pass(
        island, bodies, workBuffers.contactManifolds(), distanceConstraints);
    if (!preflight.can_solve()) {
        return false;
    return solve_island_job(bodies,
                            island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);

bool solve_island_job_with_wake_guarded(RigidBodySoA& bodies,
    if (preflight.should_wake_first()) {
        wake_island_sleepers_guarded(bodies, island);

bool dispatch_solve_island_guarded(RigidBodySoA& bodies,
    if (!is_finite_island_solve_dt(dt) || !island_index_valid(graph, islandIndex)) {
    return solve_island_job_guarded(bodies,
                                    graph.island(islandIndex),

bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,
    return solve_island_job_with_wake_guarded(bodies,

IslandBatchDispatchResult dispatch_nonsleeping_islands_result(
    RigidBodySoA& bodies,
    IslandBatchDispatchResult result{};
    const IslandNonsleepingDispatchPreflight preflight = preflight_nonsleeping_island_dispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;

    for (u32 islandIndex : collect_nonsleeping_island_indices(graph, bodies)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_result(bodies,
                                                                                 graph,
                                                                                 islandIndex,
        if (dispatchResult.solved) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;

u32 dispatch_nonsleeping_islands_guarded(RigidBodySoA& bodies,
    return dispatch_nonsleeping_islands_result(bodies,
                                               invMassFn)
        .solvedCount;
    return island_wake_reject_reason(island, bodies) != IslandWakeRejectReason::None;
}

bool can_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return should_skip_island_wake(island, bodies);
}

bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !can_skip_island_wake(island, bodies);
}

bool solve_island_with_wake_guarded(RigidBodySoA& bodies,
                                    const ContactIslandGraph::Island& island,
                                    SolverWorkBuffers& workBuffers,
                                    const std::vector<DistanceConstraint>& distanceConstraints,
                                    f32 dt,
                                    f32 contactCompliance,
                                    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (should_skip_island_sleep_solve(island, bodies)) {
        return false;
    }
    wake_island_sleepers_guarded(bodies, island);
    return solve_island_job_guarded(bodies,
                                    island,
                                    workBuffers,
                                    distanceConstraints,
                                    dt,
                                    contactCompliance,
                                    invMassFn);
}

std::vector<u32> collect_nonsleeping_island_indices(const ContactIslandGraph& graph,
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        if (preflight.skipped || preflight.can_skip_solve()) {
        indices.push_back(islandIndex);
    return indices;

std::vector<u32> collect_wakeable_island_indices(const ContactIslandGraph& graph,
        if (preflight.should_wake_sleepers()) {

IslandSolveableStats compute_island_solveable_stats(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandSolveableStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!should_solve_island(job) || job.island == nullptr) {
            ++stats.emptyCount;
            continue;
        }

        const IslandConstraintSolvePreflight constraintPreflight = preflight_island_constraint_solve(
            *job.island, bodies, contacts, distanceConstraints);
        if (!constraintPreflight.refs.can_solve()) {
            ++stats.staleRefsCount;
            continue;
        }

        const IslandSleepPreflight sleepPreflight = preflight_island_sleep(*job.island, bodies);
        if (sleepPreflight.can_skip_solve()) {
            ++stats.allSleepingCount;
            continue;
        }
        if (!constraintPreflight.bodies.can_solve()) {
            ++stats.noMovableBodiesCount;
            continue;
        }

        ++stats.solveableCount;
    }
    return stats;
}

IslandSolveableGraphPreflight preflight_island_solveable_graph(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt) {
    IslandSolveableGraphPreflight preflight{};
    preflight.invalidDt = !is_finite_island_solve_dt(dt);
    preflight.stats = compute_island_solveable_stats(graph, bodies, contacts, distanceConstraints);
    preflight.skipped = preflight.stats.solveableCount == 0u;
    return preflight;
}

bool should_skip_island_solveable_graph(const ContactIslandGraph& graph,
                                        const RigidBodySoA& bodies,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints,
                                        f32 dt) {
    return !preflight_island_solveable_graph(graph, bodies, contacts, distanceConstraints, dt).can_dispatch();
}

std::vector<u32> collect_solveable_island_indices(
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!should_solve_island(job) || job.island == nullptr) {
            continue;
        }
        if (should_skip_island_constraint_solve(*job.island, bodies, contacts, distanceConstraints)) {
            continue;
        }
        if (should_skip_island_sleep_solve(*job.island, bodies)) {
            continue;
        }
        indices.push_back(islandIndex);
    }
    return indices;
}

bool wake_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies);
    if (!preflight.should_wake_sleepers()) {

        if (!is_body_sleeping(bodies.flags[bodyIndex])) {
        bodies.flags[bodyIndex] &= ~RB_SLEEPING;
        bodies.sleepTimers[bodyIndex] = 0.f;

bool wake_island_sleepers_by_index_guarded(RigidBodySoA& bodies,
                                           u32 islandIndex) {
    return wake_island_sleepers_guarded(bodies, graph.island(islandIndex));

u32 wake_all_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return wake_all_island_sleepers_result(bodies, graph).wokeCount;
}

IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    IslandWakeResult result{};
        result.skipped = true;
        return result;
    IslandWakePreflight preflight{};
    return wake_island_sleepers_with_preflight(bodies, island, preflight);

bool wake_island_sleepers_with_preflight(RigidBodySoA& bodies,
                                         const ContactIslandGraph::Island& island,
                                         IslandWakePreflight& outPreflight) {
    outPreflight = preflight_island_wake(island, bodies);
    if (!outPreflight.should_wake_sleepers()) {
        return false;
std::vector<u32> collect_wakeable_island_indices(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandWakePreflight preflight = preflight_island_wake(graph.island(islandIndex), bodies);
        if (preflight.should_wake_sleepers()) {
            indices.push_back(islandIndex);
    return indices;

IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
    result.islandIndex = islandIndex;
    }

    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (!is_body_sleeping(bodies.flags[bodyIndex])) {
            continue;
        }
        bodies.flags[bodyIndex] &= ~RB_SLEEPING;
        bodies.sleepTimers[bodyIndex] = 0.f;
        ++result.sleepersWoken;
    }

    result.woke = result.sleepersWoken > 0u;
    return result;
        ++result.sleeperCount;
    result.woke = result.sleeperCount > 0u;

bool wake_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    return wake_island_sleepers_result(bodies, island).woke;
}

IslandWakeResult wake_island_sleepers_by_index_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph,
                                                      u32 islandIndex) {
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
    IslandWakeResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;
    }

    const IslandWakeResult islandResult = wake_island_sleepers_result(bodies, graph.island(islandIndex));
    result.woke = islandResult.woke;
    result.skipped = islandResult.skipped;
    result.sleepersWoken = islandResult.sleepersWoken;
    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies);
    if (!preflight.should_wake_sleepers()) {

    result.woke = wake_island_sleepers_guarded(bodies, island);
    result.skipped = !result.woke;
    result.woke = wake_island_sleepers_guarded(bodies, graph.island(islandIndex));
bool island_sleep_graph_rejects_all(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph(graph, bodies).skipped;

bool island_wake_graph_rejects_all(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_wake_graph(graph, bodies).can_wake();

bool wake_island_sleepers_by_index_guarded(RigidBodySoA& bodies,
                                           const ContactIslandGraph& graph,
                                           u32 islandIndex) {
    return wake_island_sleepers_result(bodies, graph, islandIndex).woke;
}

IslandWakeBatchResult wake_all_island_sleepers_result(RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    IslandWakeBatchResult result{};
IslandWakeResult wake_island_sleepers_by_index_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph,
                                                      u32 islandIndex) {
    IslandWakeResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;
    }
    return wake_island_sleepers_result(bodies, graph.island(islandIndex), islandIndex);

u32 wake_all_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return wake_all_island_sleepers_result(bodies, graph).wokeCount;
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    default:
        return "Unknown";
    }

const char* island_solve_reject_reason_name(IslandSolveRejectReason reason) {
    case IslandSolveRejectReason::None:
    case IslandSolveRejectReason::InvalidDt:
    case IslandSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    case IslandSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    case IslandSolveRejectReason::AllSleeping:
        return "AllSleeping";

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph,
                                                         const RigidBodySoA& bodies,
                                                         f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    if (should_skip_island_sleep_solve_graph(graph, bodies)) {
        return IslandDispatchRejectReason::AllIslandsSleeping;
    return IslandDispatchRejectReason::None;

IslandSolveRejectReason island_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
        return IslandSolveRejectReason::InvalidDt;
    if (!island_has_constraints(island)) {
        return IslandSolveRejectReason::EmptyIsland;
    if (should_skip_island_constraint_refs(island, contacts, distanceConstraints)) {
        return IslandSolveRejectReason::StaleConstraintRefs;
    if (should_skip_island_sleep_solve(island, bodies)) {
        return IslandSolveRejectReason::AllSleeping;
    if (should_skip_island_solve_bodies(island, bodies)) {
        return IslandSolveRejectReason::NoMovableBodies;
    return IslandSolveRejectReason::None;

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, bodies, dt) == expected;

bool island_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     IslandSolveRejectReason expected) {
    return island_solve_reject_reason(island, bodies, contacts, distanceConstraints, dt) == expected;

IslandSolvePipelinePreflight preflight_island_solve_pipeline(
    u32 bodyCount,
    const ContactIslandGraph& graph,
    IslandSolvePipelinePreflight preflight{};
    preflight.build = preflight_island_build(bodyCount, contacts, distanceConstraints);
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.rejectReason = island_dispatch_reject_reason(graph, bodies, dt);
    preflight.skipped = !preflight.build.can_build() || !preflight.dispatch.can_dispatch() ||
                        preflight.rejectReason != IslandDispatchRejectReason::None;
    return preflight;

bool should_skip_island_solve_pipeline(u32 bodyCount,
    return !preflight_island_solve_pipeline(bodyCount, contacts, distanceConstraints, graph, bodies, dt)
        .can_run();

IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    u32 islandIndex,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolvePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
    return preflight_island_constraint_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_island_constraint_solve_index(const ContactIslandGraph& graph,
    return !preflight_island_constraint_solve_by_index(graph, islandIndex, bodies, contacts, distanceConstraints)
        .can_solve();

std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
                                                  const RigidBodySoA& bodies) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!should_solve_island(job)) {
            continue;
        }
        if (should_skip_island_sleep_solve(*job.island, bodies)) {
        indices.push_back(islandIndex);
    return indices;

u32 count_solveable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return static_cast<u32>(collect_solveable_island_indices(graph, bodies).size());

bool has_solveable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_solveable_islands(graph, bodies) > 0u;

IslandSleepWakeDispatchPreflight preflight_island_sleep_wake_dispatch(const ContactIslandGraph& graph,
                                                                      const RigidBodySoA& bodies,
                                                                      f32 dt) {
    IslandSleepWakeDispatchPreflight preflight{};
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.skipped = !preflight.can_dispatch();
    return preflight;

bool should_skip_island_sleep_wake_dispatch(const ContactIslandGraph& graph,
    return !preflight_island_sleep_wake_dispatch(graph, bodies, dt).can_dispatch();
        if (should_skip_island_sleep_solve(graph.island(islandIndex), bodies)) {
            continue;
        }

bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (!is_finite_island_solve_dt(dt)) {
        return false;
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints)) {
    }
    if (should_skip_island_sleep_solve(island, bodies)) {
    return solve_island_job(bodies,
                            island,
                            workBuffers,
                            distanceConstraints,
                            dt,
                            contactCompliance,
                            invMassFn);
}

IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex) {
IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    IslandWakeResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies);
    if (!preflight.should_wake_sleepers()) {
        result.skipped = true;
        return result;

    result.woke = wake_island_sleepers_guarded(bodies, island);
    result.skipped = !result.woke;

IslandWakeResult wake_island_sleepers_by_index_result(RigidBodySoA& bodies,
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {

    result.woke = wake_island_sleepers_guarded(bodies, graph.island(islandIndex));

IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph) {

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    if (!wake_island_sleepers_guarded(bodies, island)) {

    result.woke = true;




    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
        if (!is_body_sleeping(bodies.flags[bodyIndex])) {
        bodies.flags[bodyIndex] &= ~RB_SLEEPING;
        bodies.sleepTimers[bodyIndex] = 0.f;
        ++result.bodiesWoken;

    result.woke = result.bodiesWoken > 0u;




    return wake_all_island_sleepers_result(bodies, graph).wokeIslandCount;


        ++result.wokeBodyCount;

    result.woke = result.wokeBodyCount > 0u;

    return wake_island_sleepers_result(bodies, graph.island(islandIndex), islandIndex);









    IslandBatchWakeResult result{};
IslandWakeBatchResult wake_all_island_sleepers_result(RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    IslandWakeBatchResult result{};




    result = wake_island_sleepers_result(bodies, graph.island(islandIndex));

IslandBatchWakeResult wake_all_island_sleepers_result(RigidBodySoA& bodies, const ContactIslandGraph& graph) {

    if (!preflight_island_wake(island, bodies).should_wake_sleepers()) {


const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyInputs:
        return "EmptyInputs";
    case IslandBuildRejectReason::OutOfRangeContactBodies:
        return "OutOfRangeContactBodies";
    case IslandBuildRejectReason::OutOfRangeDistanceBodies:
        return "OutOfRangeDistanceBodies";
    default:
        return "Unknown";

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build_deepen(bodyCount, contacts, distanceConstraints).reason;

bool island_build_rejects_for_reason(
    IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;

IslandBuildDeepenPreflight preflight_island_build_deepen(
    IslandBuildDeepenPreflight deepen{};
    deepen.base = preflight_island_build(bodyCount, contacts, distanceConstraints);

    if (deepen.base.skipped) {
        deepen.reason = IslandBuildRejectReason::EmptyInputs;
        deepen.rejected = true;
        return deepen;

    if (deepen.base.stats.outOfRangeContactBodyCount > 0u) {
        deepen.reason = IslandBuildRejectReason::OutOfRangeContactBodies;

    if (deepen.base.stats.outOfRangeDistanceBodyCount > 0u) {
        deepen.reason = IslandBuildRejectReason::OutOfRangeDistanceBodies;


bool should_skip_island_build_deepen(u32 bodyCount,
    return !preflight_island_build_deepen(bodyCount, contacts, distanceConstraints).can_build();

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    case IslandConstraintSolveRejectReason::None:
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    return preflight_island_constraint_solve_deepen(island, bodies, contacts, distanceConstraints).reason;

bool island_constraint_solve_rejects_for_reason(
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;

IslandConstraintSolveDeepenPreflight preflight_island_constraint_solve_deepen(
    IslandConstraintSolveDeepenPreflight deepen{};
    deepen.base = preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);

        deepen.reason = IslandConstraintSolveRejectReason::EmptyIsland;

    if (!deepen.base.refs.can_solve()) {
        deepen.reason = IslandConstraintSolveRejectReason::StaleConstraintRefs;

    if (!deepen.base.bodies.can_solve()) {
        deepen.reason = IslandConstraintSolveRejectReason::NoMovableBodies;


bool should_skip_island_constraint_solve_deepen(const ContactIslandGraph::Island& island,
    return !preflight_island_constraint_solve_deepen(island, bodies, contacts, distanceConstraints).can_solve();

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason) {
    case IslandSleepSolveRejectReason::None:
    case IslandSleepSolveRejectReason::EmptyIsland:
    case IslandSleepSolveRejectReason::OutOfRangeIsland:
        return "OutOfRangeIsland";
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
    return preflight_island_sleep_solve_deepen(island, bodies).reason;

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           IslandSleepSolveRejectReason expected) {
    return island_sleep_solve_reject_reason(island, bodies) == expected;

IslandSleepSolveDeepenPreflight preflight_island_sleep_solve_deepen(const ContactIslandGraph::Island& island,
    IslandSleepSolveDeepenPreflight deepen{};
    if (!island_has_constraints(island)) {
        deepen.reason = IslandSleepSolveRejectReason::EmptyIsland;

    deepen.base = preflight_island_sleep(island, bodies);
    if (deepen.base.can_skip_solve()) {
        deepen.reason = IslandSleepSolveRejectReason::AllSleeping;

bool should_skip_island_sleep_solve_deepen(const ContactIslandGraph::Island& island,
    return !preflight_island_sleep_solve_deepen(island, bodies).can_solve();

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    case IslandWakeRejectReason::None:
    case IslandWakeRejectReason::EmptyIsland:
    case IslandWakeRejectReason::OutOfRangeIsland:
    case IslandWakeRejectReason::NoWakeTarget:
        return "NoWakeTarget";

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
    return preflight_island_wake_deepen(island, bodies).reason;

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;

IslandWakeDeepenPreflight preflight_island_wake_deepen(const ContactIslandGraph::Island& island,
    IslandWakeDeepenPreflight deepen{};
        deepen.reason = IslandWakeRejectReason::EmptyIsland;

    deepen.base = preflight_island_wake(island, bodies);
    if (!deepen.base.should_wake_sleepers()) {
        deepen.reason = IslandWakeRejectReason::NoWakeTarget;

bool should_skip_island_wake_deepen(const ContactIslandGraph::Island& island,
    return !preflight_island_wake_deepen(island, bodies).can_wake();

IslandSolvePipelinePreflight preflight_island_solve_pipeline(const ContactIslandGraph& graph,
                                                             u32 islandIndex,
    IslandSolvePipelinePreflight pipeline{};
        pipeline.skipped = true;
        pipeline.sleep.reason = IslandSleepSolveRejectReason::OutOfRangeIsland;
        pipeline.sleep.rejected = true;
        pipeline.wake.reason = IslandWakeRejectReason::OutOfRangeIsland;
        pipeline.wake.rejected = true;
        pipeline.solve.reason = IslandConstraintSolveRejectReason::EmptyIsland;
        pipeline.solve.rejected = true;
        return pipeline;

    pipeline.wake = preflight_island_wake_deepen(island, bodies);
    pipeline.sleep = preflight_island_sleep_solve_deepen(island, bodies);
    pipeline.solve = preflight_island_constraint_solve_deepen(island, bodies, contacts, distanceConstraints);
    pipeline.dispatch = preflight_island_dispatch(graph, dt);
    pipeline.skipped = !pipeline.can_dispatch();

    }

    return result;



    const IslandWakeGraphPreflight preflight = preflight_island_wake_graph(graph, bodies);
    result.wakeableCount = preflight.stats.wakeableCount;
    if (!preflight.can_wake()) {
        result.skipped = true;
        result.skippedCount = result.wakeableCount;
        return result;
    }

    for (u32 islandIndex : collect_wakeable_island_indices(graph, bodies)) {
        if (wake_island_sleepers_by_index_guarded(bodies, graph, islandIndex)) {
            ++wokeCount;
    return wokeCount;

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";

IslandDispatchRejectReason islandDispatchRejectReason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    return IslandDispatchRejectReason::None;

bool islandDispatchRejectsForReason(const ContactIslandGraph& graph,
                                    IslandDispatchRejectReason expected) {
    return islandDispatchRejectReason(graph, dt) == expected;

IslandDispatchRejectPreflight preflightIslandDispatchReject(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchRejectPreflight preflight{};
    preflight.reason = islandDispatchRejectReason(graph, dt);
    preflight.solve = preflight_island_solve(graph);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.skipped = preflight.reason != IslandDispatchRejectReason::None;

bool canSkipIslandDispatch(const ContactIslandGraph& graph, f32 dt) {
    return !preflightIslandDispatchReject(graph, dt).can_dispatch();

bool shouldRunIslandDispatch(const ContactIslandGraph& graph, f32 dt) {
    return preflightIslandDispatchReject(graph, dt).can_dispatch();

IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandDispatchRejectPreflight preflight = preflightIslandDispatchReject(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skippedCount = result.dispatchableCount;
    return dispatch_all_islands_result(bodies,
                                     workBuffers,
                                     distanceConstraints,
                                     contactCompliance,
                                     invMassFn);

const char* islandSolveJobRejectReasonName(IslandSolveJobRejectReason reason) {
    case IslandSolveJobRejectReason::None:
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::NullIsland:
        return "NullIsland";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    case IslandSolveJobRejectReason::InvalidDt:
    case IslandSolveJobRejectReason::NonFiniteDt:

IslandSolveJobRejectReason islandSolveJobRejectReason(const IslandSolveJob& job, f32 dt) {
        return IslandSolveJobRejectReason::InvalidDt;
        return IslandSolveJobRejectReason::NonFiniteDt;
    if (job.empty) {
        return IslandSolveJobRejectReason::EmptyJob;
    if (job.island == nullptr) {
        return IslandSolveJobRejectReason::NullIsland;
    if (job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    return IslandSolveJobRejectReason::None;

bool islandSolveJobRejectsForReason(const IslandSolveJob& job,
                                    IslandSolveJobRejectReason expected) {
    return islandSolveJobRejectReason(job, dt) == expected;

IslandSolveJobRejectPreflight preflightIslandSolveJobReject(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobRejectPreflight preflight{};
    preflight.reason = islandSolveJobRejectReason(job, dt);
    preflight.invalidDt = preflight.reason == IslandSolveJobRejectReason::InvalidDt ||
                          preflight.reason == IslandSolveJobRejectReason::NonFiniteDt;
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = preflight.reason != IslandSolveJobRejectReason::None;

bool canSkipIslandSolveJob(const IslandSolveJob& job, f32 dt) {
    return !preflightIslandSolveJobReject(job, dt).can_dispatch();

bool shouldRunIslandSolveJob(const IslandSolveJob& job, f32 dt) {
    return preflightIslandSolveJobReject(job, dt).can_dispatch();

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason) {
    case IslandConstraintSolveRejectReason::None:
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
        return IslandConstraintSolveRejectReason::EmptyIsland;
    if (!preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    if (!preflight_island_solve_bodies(island, bodies).can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    return IslandConstraintSolveRejectReason::None;

bool islandConstraintSolveRejectsForReason(
    IslandConstraintSolveRejectReason expected) {
    return islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints) == expected;

IslandConstraintSolveRejectPreflight preflightIslandConstraintSolveReject(
    IslandConstraintSolveRejectPreflight preflight{};
    preflight.reason =
        islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints);
    preflight.skipped = preflight.reason != IslandConstraintSolveRejectReason::None;

bool canSkipIslandConstraintSolve(const ContactIslandGraph::Island& island,
    return !preflightIslandConstraintSolveReject(island, bodies, contacts, distanceConstraints).can_solve();

bool shouldRunIslandConstraintSolve(const ContactIslandGraph::Island& island,
    return preflightIslandConstraintSolveReject(island, bodies, contacts, distanceConstraints).can_solve();

const char* islandSleepSolveRejectReasonName(IslandSleepSolveRejectReason reason) {
    case IslandSleepSolveRejectReason::None:
    case IslandSleepSolveRejectReason::EmptyIsland:
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";

IslandSleepSolveRejectReason islandSleepSolveRejectReason(const ContactIslandGraph::Island& island,
        return IslandSleepSolveRejectReason::EmptyIsland;
    if (preflight_island_sleep(island, bodies).can_skip_solve()) {
        return IslandSleepSolveRejectReason::AllSleeping;
    return IslandSleepSolveRejectReason::None;

bool islandSleepSolveRejectsForReason(const ContactIslandGraph::Island& island,
                                      IslandSleepSolveRejectReason expected) {
    return islandSleepSolveRejectReason(island, bodies) == expected;

IslandSleepSolveRejectPreflight preflightIslandSleepSolveReject(const ContactIslandGraph::Island& island,
    IslandSleepSolveRejectPreflight preflight{};
    preflight.reason = islandSleepSolveRejectReason(island, bodies);
    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.skipped = preflight.reason != IslandSleepSolveRejectReason::None;

bool canSkipIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflightIslandSleepSolveReject(island, bodies).can_solve();

bool shouldRunIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflightIslandSleepSolveReject(island, bodies).can_solve();

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason) {
    case IslandWakeRejectReason::None:
    case IslandWakeRejectReason::EmptyIsland:
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";

IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,
        return IslandWakeRejectReason::EmptyIsland;
    const IslandWakePreflight wake = preflight_island_wake(island, bodies);
    if (!wake.hasMixedSleepState) {
        return IslandWakeRejectReason::NoMixedSleepState;
    if (wake.activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;
    return IslandWakeRejectReason::None;

bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
                                IslandWakeRejectReason expected) {
    return islandWakeRejectReason(island, bodies) == expected;

IslandWakeRejectPreflight preflightIslandWakeReject(const ContactIslandGraph::Island& island,
    IslandWakeRejectPreflight preflight{};
    preflight.reason = islandWakeRejectReason(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);
    preflight.skipped = preflight.reason != IslandWakeRejectReason::None;

bool canSkipIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflightIslandWakeReject(island, bodies).can_wake();

bool shouldRunIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflightIslandWakeReject(island, bodies).can_wake();

const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason) {
    case IslandSleepGraphRejectReason::None:
    case IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty:
        return "AllIslandsSleepingOrEmpty";

IslandSleepGraphRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,
    if (preflight_island_sleep_graph(graph, bodies).has_solveable_islands()) {
        return IslandSleepGraphRejectReason::None;
    return IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty;

bool islandSleepGraphRejectsForReason(const ContactIslandGraph& graph,
                                      IslandSleepGraphRejectReason expected) {
    return islandSleepGraphRejectReason(graph, bodies) == expected;

IslandSleepGraphRejectPreflight preflightIslandSleepGraphReject(const ContactIslandGraph& graph,
    IslandSleepGraphRejectPreflight preflight{};
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.reason = islandSleepGraphRejectReason(graph, bodies);
    preflight.skipped = preflight.reason != IslandSleepGraphRejectReason::None;

bool canSkipIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflightIslandSleepGraphReject(graph, bodies).has_solveable_islands();

bool shouldRunIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflightIslandSleepGraphReject(graph, bodies).has_solveable_islands();

const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason) {
    case IslandWakeGraphRejectReason::None:
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";

IslandWakeGraphRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,
    if (preflight_island_wake_graph(graph, bodies).can_wake()) {
        return IslandWakeGraphRejectReason::None;
    return IslandWakeGraphRejectReason::NoWakeableIslands;

bool islandWakeGraphRejectsForReason(const ContactIslandGraph& graph,
                                     IslandWakeGraphRejectReason expected) {
    return islandWakeGraphRejectReason(graph, bodies) == expected;

IslandWakeGraphRejectPreflight preflightIslandWakeGraphReject(const ContactIslandGraph& graph,
    IslandWakeGraphRejectPreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.reason = islandWakeGraphRejectReason(graph, bodies);
    preflight.skipped = preflight.reason != IslandWakeGraphRejectReason::None;

bool canSkipIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflightIslandWakeGraphReject(graph, bodies).can_wake();

bool shouldRunIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflightIslandWakeGraphReject(graph, bodies).can_wake();

const char* islandPipelineDispatchRejectReasonName(IslandPipelineDispatchRejectReason reason) {
    case IslandPipelineDispatchRejectReason::None:
    case IslandPipelineDispatchRejectReason::InvalidDt:
    case IslandPipelineDispatchRejectReason::NonFiniteDt:
    case IslandPipelineDispatchRejectReason::NoDispatchableIslands:
    case IslandPipelineDispatchRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";

IslandPipelineDispatchRejectReason islandPipelineDispatchRejectReason(const ContactIslandGraph& graph,
    const IslandDispatchRejectReason dispatchReason = islandDispatchRejectReason(graph, dt);
    if (dispatchReason == IslandDispatchRejectReason::InvalidDt) {
        return IslandPipelineDispatchRejectReason::InvalidDt;
    if (dispatchReason == IslandDispatchRejectReason::NonFiniteDt) {
        return IslandPipelineDispatchRejectReason::NonFiniteDt;
    if (dispatchReason == IslandDispatchRejectReason::NoDispatchableIslands) {
        return IslandPipelineDispatchRejectReason::NoDispatchableIslands;
    if (islandSleepGraphRejectReason(graph, bodies) != IslandSleepGraphRejectReason::None) {
        return IslandPipelineDispatchRejectReason::AllIslandsSleeping;
    return IslandPipelineDispatchRejectReason::None;

bool islandPipelineDispatchRejectsForReason(const ContactIslandGraph& graph,
                                            IslandPipelineDispatchRejectReason expected) {
    return islandPipelineDispatchRejectReason(graph, bodies, dt) == expected;

IslandPipelineDispatchPreflight preflightIslandPipelineDispatch(const ContactIslandGraph& graph,
    IslandPipelineDispatchPreflight preflight{};
    preflight.wake = preflightIslandWakeGraphReject(graph, bodies);
    preflight.sleep = preflightIslandSleepGraphReject(graph, bodies);
    preflight.dispatch = preflightIslandDispatchReject(graph, dt);
    preflight.reason = islandPipelineDispatchRejectReason(graph, bodies, dt);
    preflight.skipped = preflight.reason != IslandPipelineDispatchRejectReason::None;

bool canSkipIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt) {
    return !preflightIslandPipelineDispatch(graph, bodies, dt).can_dispatch();

bool shouldRunIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt) {
    return preflightIslandPipelineDispatch(graph, bodies, dt).can_dispatch();

IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    const IslandPipelineDispatchPreflight preflight = preflightIslandPipelineDispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;

    wake_all_island_sleepers_guarded(bodies, graph);
bool preflight_warm_start_island(const ContactIslandGraph::Island& island,
                                 const std::vector<f32>& priorContactLambdas,
                                 IslandWarmStartPreflight& out) {
    out = {};

        if (distanceIndex >= priorDistanceLambdas.size()) {
        ++out.distanceSlotCount;
        if (priorDistanceLambdas[distanceIndex] != 0.f) {
            out.hasDistanceLambdas = true;

        ++out.contactSlotCount;
        if (priorContactLambdas[contactIndex] != 0.f) {
            out.hasContactLambdas = true;
        if (contactIndex < contacts.size() && contacts[contactIndex].warmNormalImpulse != 0.f && dt > 0.f) {
            out.hasContactImpulses = true;

    return out.hasDistanceLambdas || out.hasContactLambdas || out.hasContactImpulses;

void warm_start_island_lambdas_guarded(SolverWorkBuffers& workBuffers,
                                       const IslandWarmStartPreflight& preflight) {
    if (!preflight.hasDistanceLambdas && !preflight.hasContactLambdas) {
    warm_start_island_lambdas(workBuffers, island, priorDistanceLambdas, priorContactLambdas);

void warm_start_island_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
    if (!preflight.hasContactImpulses || dt <= 0.f) {

bool preflight_and_warm_start_island(SolverWorkBuffers& workBuffers,
    IslandWarmStartPreflight preflight{};
    if (!preflight_warm_start_island(island,
                                     priorContactLambdas,
                                     preflight)) {

    warm_start_island_lambdas_guarded(workBuffers,
                                      preflight);
    warm_start_island_contact_impulses_guarded(workBuffers, island, contacts, dt, preflight);

IslandWarmStartStats compute_island_warm_start_stats(const ContactIslandGraph& graph,
    IslandWarmStartStats stats{};
        const IslandWarmStartPreflight preflight =
            preflight_warm_start_island(graph.island(islandIndex), priorDistanceLambdas, priorContactLambdas);
            ++stats.skippedCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;

bool should_skip_frame_warm_start(const ContactIslandGraph& graph,
    return compute_island_warm_start_stats(graph, priorDistanceLambdas, priorContactLambdas)
               .warmStartableCount == 0u;

std::vector<u32> collect_warm_startable_island_indices(const ContactIslandGraph& graph,
        if (preflight.can_warm_start()) {

u32 warm_start_all_islands_guarded(SolverWorkBuffers& workBuffers,
    if (should_skip_frame_warm_start(graph, priorDistanceLambdas, priorContactLambdas)) {
        return 0u;

    u32 seededCount = 0u;
    for (u32 islandIndex : collect_warm_startable_island_indices(graph,
        if (warm_start_island_lambdas_guarded(workBuffers,
                                              graph.island(islandIndex),
            ++seededCount;
    return seededCount;


    if (should_skip_warm_start_island(island)) {


IslandWarmStartResult warm_start_island_contact_impulses_result(




std::vector<u32> collect_contact_impulse_warm_startable_island_indices(
    const std::vector<narrowphase::ContactManifold>& contacts) {
            preflight_contact_impulse_warm_start_island(graph.island(islandIndex), contacts);

u32 warm_start_all_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
    u32 warmedCount = 0u;
    for (u32 islandIndex : collect_contact_impulse_warm_startable_island_indices(graph, contacts)) {
        const IslandWarmStartResult result =
        if (result.warmed) {
            ++warmedCount;

    const IslandCombinedWarmStartGraphPreflight preflight =
        preflight_warm_start_combined_graph(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas);
    result.warmStartableCount = preflight.stats.warmStartableCount;

    for (u32 islandIndex :
         collect_warm_startable_combined_island_indices(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas)) {
        const IslandWakeResult wakeResult = wake_island_sleepers_by_index_result(bodies, graph, islandIndex);
        const IslandWakeResult wakeResult = wake_island_sleepers_result(bodies, graph, islandIndex);
        if (wakeResult.woke) {
            ++result.wokeCount;
            result.bodiesWoken += wakeResult.bodiesWoken;
            ++result.wokeIslandCount;
        } else {
            ++result.skippedCount;
        }
    }
    return result;
}

IslandWarmStartResult warm_start_island_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandWarmStartResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    const IslandContactImpulseWarmStartPreflight preflight =
        preflight_contact_impulse_warm_start_island(island, contacts);
    if (!preflight.can_warm_start()) {

    warm_start_island_contact_impulses(workBuffers, island, contacts, dt);
    result.warmed = true;

bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,
    return warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt).warmed;

std::vector<u32> collect_contact_impulse_warm_startable_island_indices(
    const std::vector<narrowphase::ContactManifold>& contacts) {
std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
            preflight_contact_impulse_warm_start_island(graph.island(islandIndex), contacts);
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
    return indices;

u32 warm_start_all_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
    return warm_start_all_contact_impulses_result(workBuffers, graph, contacts, dt).warmedCount;

IslandContactImpulseWarmStartBatchResult warm_start_all_contact_impulses_result(
    IslandContactImpulseWarmStartBatchResult result{};
    const IslandContactImpulseDispatchPreflight preflight = preflight_contact_impulse_dispatch(graph, contacts, dt);
    result.warmStartableCount = preflight.impulses.stats.warmStartableCount;
                                            f32 dt,
                                            const std::vector<f32>& priorDistanceLambdas,
                                            const std::vector<f32>& priorContactLambdas) {
    return warm_start_all_islands_combined_result(workBuffers,
                                                   graph,
                                                   contacts,
                                                   dt,
                                                   priorDistanceLambdas,
                                                   priorContactLambdas)
        .warmedCount;

IslandBatchWarmStartResult warm_start_all_islands_combined_result(
    IslandBatchWarmStartResult result{};
    const IslandCombinedWarmStartGraphPreflight preflight =
        preflight_warm_start_combined_graph(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas);
    result.warmStartableCount = preflight.stats.warmStartableCount;
        result.skippedCount = result.warmStartableCount;

    for (u32 islandIndex : collect_contact_impulse_warm_startable_island_indices(graph, contacts)) {
        const IslandWarmStartResult islandResult =
            warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt);
        if (islandResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;

u32 warm_start_graph_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
    return warm_start_all_contact_impulses_guarded(workBuffers, graph, contacts, dt);

namespace {

u32 count_island_prior_impulse_coverage(const ContactIslandGraph::Island& island,
    u32 coverage = 0u;
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        if (contacts[contactIndex].warmNormalImpulse != 0.f) {
            ++coverage;
    return coverage;

} // namespace

IslandContactImpulseWarmStartPreflight preflight_warm_start_island_contact_impulses(
    const ContactIslandGraph::Island& island,
    IslandContactImpulseWarmStartPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.priorImpulseCoverage = count_island_prior_impulse_coverage(island, contacts);

IslandContactImpulseWarmStartPreflight preflight_warm_start_island_contact_impulses_by_index(
    return preflight_warm_start_island_contact_impulses(graph.island(islandIndex), contacts, dt);

IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(
    IslandContactImpulseWarmStartStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
            preflight_warm_start_island_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;
            ++stats.noImpulseDataCount;
    return stats;

u32 count_contact_impulse_warm_startable_islands(const ContactIslandGraph& graph,
    return compute_island_contact_impulse_warm_start_stats(graph, contacts, dt).warmStartableCount;

bool has_contact_impulse_warm_startable_islands(const ContactIslandGraph& graph,
    return count_contact_impulse_warm_startable_islands(graph, contacts, dt) > 0u;

IslandContactImpulseWarmStartGraphPreflight preflight_warm_start_contact_impulses_graph(
    IslandContactImpulseWarmStartGraphPreflight preflight{};
    preflight.stats = compute_island_contact_impulse_warm_start_stats(graph, contacts, dt);
    preflight.skipped = preflight.stats.warmStartableCount == 0u;

bool should_skip_warm_start_contact_impulses_graph(const ContactIslandGraph& graph,
    return !has_contact_impulse_warm_startable_islands(graph, contacts, dt) ||
           !is_valid_island_solve_dt(dt);


bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island,
    return !preflight_warm_start_island_contact_impulses(island, contacts, dt).can_warm_start();

bool should_skip_warm_start_contact_impulses_index(const ContactIslandGraph& graph,
    return !preflight_warm_start_island_contact_impulses_by_index(graph, islandIndex, contacts, dt)
                .can_warm_start();

IslandContactImpulseWarmStartResult warm_start_island_contact_impulses_result(
    IslandContactImpulseWarmStartResult result{};

        preflight_warm_start_island_contact_impulses_by_index(graph, islandIndex, contacts, dt);

    warm_start_island_contact_impulses(workBuffers, graph.island(islandIndex), contacts, dt);


u32 warm_start_all_island_contact_impulses_guarded(
    const IslandContactImpulseWarmStartGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
        return 0u;

    u32 warmedCount = 0u;
    for (u32 islandIndex : collect_contact_impulse_warm_startable_island_indices(graph, contacts, dt)) {
        const IslandContactImpulseWarmStartResult result =
        if (result.warmed) {
            ++warmedCount;

    for (u32 islandIndex :
         collect_warm_startable_combined_island_indices(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas)) {
        const IslandWarmStartResult warmResult = warm_start_island_combined_result(workBuffers,
                                                                                   islandIndex,
                                                                                   priorContactLambdas);
        if (warmResult.warmed) {

IslandCombinedWarmStartPreflight preflight_warm_start_island_combined(
    IslandCombinedWarmStartPreflight preflight{};
        preflight.lambda.skipped = true;
        preflight.impulse.skipped = true;

    preflight.lambda = preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    preflight.impulse = preflight_warm_start_island_contact_impulses(island, contacts, dt);

bool is_valid_contact_impulse_warm_start_dt(f32 dt) {
    return is_valid_island_solve_dt(dt);

bool island_contact_indices_in_range(const ContactIslandGraph::Island& island, u32 contactCount) {
        if (contactIndex >= contactCount) {
            return false;
    return true;

bool island_distance_indices_in_range(const ContactIslandGraph::Island& island, u32 distanceCount) {
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceCount) {

IslandSolveInputsPreflight preflight_island_solve_inputs(const ContactIslandGraph::Island& island,
                                                         u32 contactCount,
                                                         u32 distanceCount) {
    IslandSolveInputsPreflight preflight{};

    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());

        if (contactIndex < contactCount) {
            ++preflight.inRangeContactCount;
        if (distanceIndex < distanceCount) {
            ++preflight.inRangeDistanceCount;

    preflight.contactsInRange = preflight.inRangeContactCount == preflight.ownedContactCount;
    preflight.distancesInRange = preflight.inRangeDistanceCount == preflight.ownedDistanceCount;

IslandSolveInputsPreflight preflight_island_solve_inputs_by_index(const ContactIslandGraph& graph,
    return preflight_island_solve_inputs(graph.island(islandIndex), contactCount, distanceCount);

bool should_skip_island_solve_inputs(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_inputs(island, contactCount, distanceCount).can_solve();

IslandContactImpulsePreflight preflight_warm_start_contact_impulses(
    IslandContactImpulsePreflight preflight{};

    preflight.invalidDt = !is_valid_contact_impulse_warm_start_dt(dt);

            ++preflight.nonZeroImpulseCount;

IslandContactImpulsePreflight preflight_warm_start_contact_impulses_by_index(
    return preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);

    return !preflight_warm_start_contact_impulses(island, contacts, dt).can_warm_start();

bool should_skip_contact_impulse_warm_start_island_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_warm_start_island(graph.island(islandIndex));


    preflight.impulse = preflight_warm_start_contact_impulses(island, contacts, dt);

bool should_skip_warm_start_island_combined(const ContactIslandGraph::Island& island,
    return !preflight_warm_start_island_combined(island, contacts, dt, priorDistanceLambdas, priorContactLambdas)

    const bool invalidDt = !is_valid_contact_impulse_warm_start_dt(dt);

        const IslandContactImpulsePreflight preflight =
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        } else if (invalidDt) {
            ++stats.invalidDtCount;
            ++stats.impulseSeedableCount;

u32 count_impulse_warm_startable_islands(const ContactIslandGraph& graph,
    return compute_island_contact_impulse_warm_start_stats(graph, contacts, dt).impulseSeedableCount;

bool has_impulse_warm_startable_islands(const ContactIslandGraph& graph,
    return count_impulse_warm_startable_islands(graph, contacts, dt) > 0u;

    preflight.skipped = preflight.stats.impulseSeedableCount == 0u;

    return !has_impulse_warm_startable_islands(graph, contacts, dt);

std::vector<u32> collect_impulse_warm_startable_island_indices(


    const IslandContactImpulsePreflight preflight = preflight_warm_start_contact_impulses(island, contacts, dt);


u32 warm_start_all_islands_contact_impulses_guarded(

    for (u32 islandIndex : collect_impulse_warm_startable_island_indices(graph, contacts, dt)) {


         collect_combined_warm_startable_island_indices(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas)) {


IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses(
    if (should_skip_warm_start_island(island)) {

            ++preflight.seedableContactCount;

IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses_by_index(

IslandContactImpulseStats compute_island_contact_impulse_stats(
    IslandContactImpulseStats stats{};
            ++stats.seedableCount;
            ++stats.noImpulseCount;

u32 count_seedable_contact_impulse_islands(const ContactIslandGraph& graph,
    return compute_island_contact_impulse_stats(graph, contacts, dt).seedableCount;

bool has_seedable_contact_impulse_islands(const ContactIslandGraph& graph,
    return count_seedable_contact_impulse_islands(graph, contacts, dt) > 0u;

IslandContactImpulseGraphPreflight preflight_warm_start_contact_impulses_graph(
    IslandContactImpulseGraphPreflight preflight{};
    preflight.stats = compute_island_contact_impulse_stats(graph, contacts, dt);
    preflight.skipped = preflight.stats.seedableCount == 0u;

bool should_skip_contact_impulse_warm_start_graph(const ContactIslandGraph& graph,
    return !has_seedable_contact_impulse_islands(graph, contacts, dt) ||
           !is_valid_contact_impulse_warm_start_dt(dt);

std::vector<u32> collect_contact_impulse_seedable_island_indices(

bool should_skip_contact_impulse_warm_start_island(const ContactIslandGraph::Island& island) {
    return should_skip_warm_start_island(island);

bool should_skip_contact_impulse_warm_start_island_index(const ContactIslandGraph& graph,
    if (preflight.skipped || preflight.invalidDt) {
    return preflight.ownedContactCount == 0u;

    return warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt)
        .warmed;



u32 warm_start_all_islands_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
    return warm_start_all_islands_contact_impulses_result(workBuffers, graph, contacts, dt).warmedCount;

IslandContactImpulseBatchResult warm_start_all_islands_contact_impulses_result(
    IslandContactImpulseBatchResult result{};
    const IslandContactImpulseGraphPreflight preflight =
    result.seedableCount = preflight.stats.seedableCount;
        result.skippedCount = result.seedableCount;

    for (u32 islandIndex : collect_contact_impulse_seedable_island_indices(graph, contacts, dt)) {
        const IslandContactImpulseWarmStartResult warmResult =


IslandContactImpulsePreflight preflight_island_contact_impulses(
    if (!is_valid_contact_impulse_warm_start_dt(dt)) {
        preflight.invalidDt = true;

        if (contactIndex < contacts.size() && contacts[contactIndex].warmNormalImpulse != 0.f) {
            ++preflight.impulseCoverage;
    preflight.skipped = preflight.impulseCoverage == 0u;

IslandContactImpulsePreflight preflight_island_contact_impulses_by_index(
    return preflight_island_contact_impulses(graph.island(islandIndex), contacts, dt);

        stats.emptyCount = stats.totalIslands;

            preflight_island_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (!island_has_constraints(graph.island(islandIndex))) {

u32 count_contact_impulse_warm_startable_islands(
    return compute_island_contact_impulse_stats(graph, contacts, dt).warmStartableCount;


IslandContactImpulseGraphPreflight preflight_contact_impulse_graph(
    preflight.skipped = preflight.invalidDt || preflight.stats.warmStartableCount == 0u;

bool should_skip_contact_impulse_graph(const ContactIslandGraph& graph,



bool should_skip_contact_impulse_island(const ContactIslandGraph::Island& island,
    return !preflight_island_contact_impulses(island, contacts, dt).can_warm_start();

bool should_skip_contact_impulse_island_index(const ContactIslandGraph& graph,
    return !preflight_island_contact_impulses_by_index(graph, islandIndex, contacts, dt).can_warm_start();

        preflight_island_contact_impulses_by_index(graph, islandIndex, contacts, dt);


bool warm_start_island_contact_impulses_by_index_guarded(

    return warm_start_all_island_contact_impulses_result(workBuffers, graph, contacts, dt).warmedCount;

IslandBatchContactImpulseWarmStartResult warm_start_all_island_contact_impulses_result(
    IslandBatchContactImpulseWarmStartResult result{};
    const IslandContactImpulseGraphPreflight preflight = preflight_contact_impulse_graph(graph, contacts, dt);



bool contact_has_warm_impulse(const narrowphase::ContactManifold& contact) {
    return contact.warmNormalImpulse != 0.f;

u32 island_impulse_coverage(const ContactIslandGraph::Island& island,
        if (contact_has_warm_impulse(contacts[contactIndex])) {


    preflight.impulseCoverage = island_impulse_coverage(island, contacts);


            preflight_warm_start_island_contact_impulses(graph.island(islandIndex), contacts, 1.f);
        } else if (preflight.impulseCoverage > 0u) {

    return compute_island_contact_impulse_stats(graph, contacts).warmStartableCount;

    return count_impulse_warm_startable_islands(graph, contacts) > 0u;

    preflight.stats = compute_island_contact_impulse_stats(graph, contacts);

    return !preflight_warm_start_contact_impulses_graph(graph, contacts, dt).can_warm_start();

bool should_skip_warm_start_contact_impulses_island(const ContactIslandGraph::Island& island) {

bool should_skip_warm_start_contact_impulses_island_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_warm_start_island_index(graph, islandIndex);


IslandContactImpulseResult warm_start_island_contact_impulses_result(
    IslandContactImpulseResult result{};





        const IslandContactImpulseResult impulseResult =
        if (impulseResult.warmed) {

bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
    return (bodies.flags[bodyIndex] & RB_SLEEPING) != 0u;

bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex) {
    const u32 flags = bodies.flags[bodyIndex];
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;

bool island_all_bodies_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    if (island.bodyIndices.empty()) {
    for (u32 bodyIndex : island.bodyIndices) {
        if (!is_body_sleeping(bodies, bodyIndex)) {

bool island_has_awake_body(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
        if (is_body_static_or_kinematic(bodies, bodyIndex)) {

bool island_needs_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    return island_has_awake_body(bodies, island);

IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island) {
    IslandSleepPreflight preflight{};

    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
        if (is_body_sleeping(bodies, bodyIndex)) {
            ++preflight.sleepingBodyCount;
            ++preflight.awakeBodyCount;
    preflight.allSleeping = preflight.awakeBodyCount == 0u && preflight.sleepingBodyCount > 0u;

IslandSleepPreflight preflight_island_sleep_by_index(const RigidBodySoA& bodies,
                                                     u32 islandIndex) {
    return preflight_island_sleep(bodies, graph.island(islandIndex));

IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
    IslandWakePreflight preflight{};

    preflight.constraintCount = island_constraint_count(island);
            ++preflight.awakeDynamicCount;
    preflight.needsWake = preflight.awakeDynamicCount > 0u;

IslandWakePreflight preflight_island_wake_by_index(const RigidBodySoA& bodies,
    return preflight_island_wake(bodies, graph.island(islandIndex));

IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    IslandSleepStats stats{};
        if (island.isEmpty()) {

        const IslandSleepPreflight sleepPreflight = preflight_island_sleep(bodies, island);
        if (sleepPreflight.can_skip_solve()) {
            ++stats.allSleepingCount;
            ++stats.awakeCount;

        if (preflight_island_wake(bodies, island).should_wake()) {
            ++stats.wakeRequiredCount;

u32 count_all_sleeping_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return compute_island_sleep_stats(bodies, graph).allSleepingCount;

u32 count_wake_required_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return compute_island_sleep_stats(bodies, graph).wakeRequiredCount;

bool should_skip_island_solve_for_sleep(const RigidBodySoA& bodies,
    return preflight_island_sleep(bodies, island).can_skip_solve();

bool should_skip_island_solve_for_sleep_index(const RigidBodySoA& bodies,
    return should_skip_island_solve_for_sleep(bodies, graph.island(islandIndex));

bool should_skip_island_solve_job_for_sleep(const RigidBodySoA& bodies, const IslandSolveJob& job) {
    if (job.island == nullptr || job.empty) {
    return should_skip_island_solve_for_sleep(bodies, *job.island);

IslandConstraintSolvePreflight preflight_island_constraint_solve(
    u32 contactManifoldCount,
    u32 distanceConstraintCount,
    IslandConstraintSolvePreflight preflight{};


        if (contactIndex >= contactManifoldCount) {
            ++preflight.outOfRangeContactCount;
        if (distanceIndex >= distanceConstraintCount) {
            ++preflight.outOfRangeDistanceCount;

    const u32 inRangeContacts = preflight.ownedContactCount - preflight.outOfRangeContactCount;
    const u32 inRangeDistances = preflight.ownedDistanceCount - preflight.outOfRangeDistanceCount;
    preflight.resolvableConstraintCount = inRangeContacts + inRangeDistances;

IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    return preflight_island_constraint_solve(graph.island(islandIndex),
                                             contactManifoldCount,
                                             distanceConstraintCount,
                                             dt);

IslandSolveSleepPreflight preflight_island_solve_sleep(const RigidBodySoA& bodies,
    IslandSolveSleepPreflight preflight{};

    preflight.sleep = preflight_island_sleep(bodies, island);
    preflight.wake = preflight_island_wake(bodies, island);
    preflight.constraints =
        preflight_island_constraint_solve(island, contactManifoldCount, distanceConstraintCount, dt);

IslandSolveSleepPreflight preflight_island_solve_sleep_by_index(const RigidBodySoA& bodies,
    return preflight_island_solve_sleep(bodies,
                                        graph.island(islandIndex),

IslandDispatchSleepPreflight preflight_island_dispatch_sleep(const RigidBodySoA& bodies,
    IslandDispatchSleepPreflight preflight{};
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.sleepStats = compute_island_sleep_stats(bodies, graph);
    preflight.skipped = preflight.dispatch.skipped;

    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!should_solve_island(job) || job.island == nullptr) {
        if (should_skip_island_solve_for_sleep(bodies, *job.island)) {
        ++preflight.solvableCount;


bool should_skip_island_dispatch_sleep(const RigidBodySoA& bodies,
    return !preflight_island_dispatch_sleep(bodies, graph, dt).can_dispatch();

std::vector<u32> collect_solvable_island_indices(const RigidBodySoA& bodies,
                                                 const ContactIslandGraph& graph) {

bool has_solvable_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return !collect_solvable_island_indices(bodies, graph).empty();

bool dispatch_solve_island_sleep_guarded(RigidBodySoA& bodies,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         f32 contactCompliance,
                                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_solve_island_sleep_result(bodies,
                                              workBuffers,
                                              distanceConstraints,
                                              contactCompliance,
                                              invMassFn)
        .solved;

IslandDispatchResult dispatch_solve_island_sleep_result(RigidBodySoA& bodies,
    IslandDispatchResult result{};

    const IslandSolveSleepPreflight preflight = preflight_island_solve_sleep_by_index(
        bodies,
        static_cast<u32>(workBuffers.contactManifolds().size()),
        static_cast<u32>(distanceConstraints.size()),
    if (!preflight.can_solve()) {

    result.solved = dispatch_solve_island(bodies,
                                          invMassFn);
    result.skipped = !result.solved;

u32 dispatch_all_islands_sleep_guarded(RigidBodySoA& bodies,
    return dispatch_all_islands_sleep_result(bodies,
        .dispatch.solvedCount;

IslandBatchSleepDispatchResult dispatch_all_islands_sleep_result(
    RigidBodySoA& bodies,
    IslandBatchSleepDispatchResult result{};
    const IslandDispatchSleepPreflight preflight = preflight_island_dispatch_sleep(bodies, graph, dt);
    result.dispatch.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    result.wakeRequiredCount = preflight.sleepStats.wakeRequiredCount;

    if (!preflight.can_dispatch()) {
        result.dispatch.skipped = true;

    for (u32 islandIndex : collect_solvable_island_indices(bodies, graph)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_sleep_result(
        if (dispatchResult.solved) {
            ++result.dispatch.solvedCount;
        } else if (should_skip_island_solve_for_sleep_index(bodies, graph, islandIndex)) {
            ++result.sleepSkippedCount;
            ++result.dispatch.skippedCount;

bool is_body_sleeping(u32 bodyFlags) {
    return (bodyFlags & RB_SLEEPING) != 0u;

bool is_body_static_or_kinematic(u32 bodyFlags) {
    return (bodyFlags & RB_STATIC) != 0u || (bodyFlags & RB_KINEMATIC) != 0u;


constexpr f32 kWakeForceEpsilon = 1e-6f;

bool is_dynamic_body(u32 bodyFlags) {
    return !is_body_static_or_kinematic(bodyFlags);

void accumulate_island_body_sleep_stats(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies,
                                        IslandSleepPreflight& preflight) {
        if (!is_dynamic_body(bodies.flags[bodyIndex])) {
        ++preflight.dynamicBodyCount;
        if (is_body_sleeping(bodies.flags[bodyIndex])) {
    preflight.allSleeping =
        preflight.dynamicBodyCount > 0u && preflight.awakeBodyCount == 0u;


bool is_island_all_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep(island, bodies).allSleeping;

bool island_has_awake_dynamic_body(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    const IslandSleepPreflight preflight = preflight_island_sleep(island, bodies);
    return !preflight.skipped && preflight.awakeBodyCount > 0u;

IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies) {

    accumulate_island_body_sleep_stats(island, bodies, preflight);

IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
    return preflight_island_sleep(graph.island(islandIndex), bodies);

IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
        const IslandSleepPreflight preflight = preflight_island_sleep(graph.island(islandIndex), bodies);
        } else if (preflight.allSleeping) {

u32 count_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return compute_island_sleep_stats(graph, bodies).awakeCount;

bool has_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_awake_islands(graph, bodies) > 0u;

IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
    IslandSleepGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_stats(graph, bodies);
    preflight.skipped = preflight.stats.awakeCount == 0u;

bool should_skip_awake_island_dispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_graph(graph, bodies).can_dispatch_awake();

std::vector<u32> collect_awake_island_indices(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
        if (island_has_awake_dynamic_body(graph.island(islandIndex), bodies)) {

bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep(island, bodies).can_skip_solve();

bool should_skip_sleeping_island_solve_index(const ContactIslandGraph& graph,
    return should_skip_sleeping_island_solve(graph.island(islandIndex), bodies);

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
    if (should_skip_island_wake(island)) {

            if (bodies.forces[bodyIndex].dot(bodies.forces[bodyIndex]) > kWakeForceEpsilon) {
                ++preflight.wakeCandidateCount;

        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        const bool sleepingA =
            contact.bodyA < bodies.count() && is_body_sleeping(bodies.flags[contact.bodyA]);
        const bool sleepingB =
            contact.bodyB < bodies.count() && is_body_sleeping(bodies.flags[contact.bodyB]);
        const bool awakeA =
            contact.bodyA < bodies.count() && is_dynamic_body(bodies.flags[contact.bodyA]) &&
            !is_body_sleeping(bodies.flags[contact.bodyA]);
        const bool awakeB =
            contact.bodyB < bodies.count() && is_dynamic_body(bodies.flags[contact.bodyB]) &&
            !is_body_sleeping(bodies.flags[contact.bodyB]);

        if ((sleepingA && awakeB) || (sleepingB && awakeA)) {
            preflight.hasAwakeNeighborContact = true;
        if (contact.warmNormalImpulse != 0.f && (sleepingA || sleepingB)) {

    if (preflight.wakeCandidateCount == 0u && preflight.hasAwakeNeighborContact) {
        preflight.wakeCandidateCount = preflight.sleepingBodyCount;


IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
    return preflight_island_wake(graph.island(islandIndex), bodies, contacts);

bool should_skip_island_wake(const ContactIslandGraph::Island& island) {
    return !island_has_constraints(island);

bool should_wake_island(const ContactIslandGraph::Island& island,
    return preflight_island_wake(island, bodies, contacts).should_wake();

u32 wake_island_bodies_guarded(RigidBodySoA& bodies,
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies, contacts);
    if (!preflight.should_wake()) {

    u32 wokenCount = 0u;
        if (!is_dynamic_body(bodies.flags[bodyIndex]) || !is_body_sleeping(bodies.flags[bodyIndex])) {

        bool shouldWakeBody = bodies.forces[bodyIndex].dot(bodies.forces[bodyIndex]) > kWakeForceEpsilon;
        if (!shouldWakeBody) {
                if (contact.bodyA != bodyIndex && contact.bodyB != bodyIndex) {
                const u32 otherBody = contact.bodyA == bodyIndex ? contact.bodyB : contact.bodyA;
                if (otherBody < bodies.count() && is_dynamic_body(bodies.flags[otherBody]) &&
                    !is_body_sleeping(bodies.flags[otherBody])) {
                    shouldWakeBody = true;
                    break;
                if (contact.warmNormalImpulse != 0.f) {


        bodies.flags[bodyIndex] &= ~RB_SLEEPING;
        bodies.sleepTimers[bodyIndex] = 0.f;
        ++wokenCount;
    return wokenCount;

u32 wake_island_bodies_by_index_guarded(RigidBodySoA& bodies,
    return wake_island_bodies_guarded(bodies, graph.island(islandIndex), contacts);

IslandConstraintSolvePreflight preflight_solve_island(const ContactIslandGraph::Island& island,
                                                      const SolverWorkBuffers& workBuffers,
        preflight.empty = true;

    preflight.sleep = preflight_island_sleep(island, bodies);

    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
        if (distanceIndex >= distanceConstraints.size()) {

IslandConstraintSolvePreflight preflight_solve_island_by_index(const ContactIslandGraph& graph,
    return preflight_solve_island(graph.island(islandIndex),

bool should_skip_solve_island_preflight(const ContactIslandGraph::Island& island,
    return !preflight_solve_island(island, bodies, distanceConstraints, workBuffers, dt).can_solve();

bool solve_island_job_guarded(RigidBodySoA& bodies,
    if (should_skip_solve_island_preflight(island, bodies, distanceConstraints, workBuffers, dt)) {
    return solve_island_job(bodies,
                            island,

bool dispatch_solve_awake_island(RigidBodySoA& bodies,
    if (!is_valid_island_solve_dt(dt)) {
    return dispatch_solve_awake_island_job(bodies,
                                           job,

bool dispatch_solve_awake_island_job(RigidBodySoA& bodies,
                                     const IslandSolveJob& job,
    if (!is_valid_island_solve_dt(dt) || !should_solve_island(job) || job.island == nullptr) {
    if (should_skip_sleeping_island_solve(*job.island, bodies)) {
                            *job.island,

bool is_sleeping_body(const RigidBodySoA& bodies, u32 bodyIndex) {

bool is_static_or_kinematic_body(const RigidBodySoA& bodies, u32 bodyIndex) {

bool island_body_indices_valid(const ContactIslandGraph::Island& island, u32 bodyCount) {
        if (bodyIndex >= bodyCount) {

bool has_out_of_range_island_body_indices(const ContactIslandGraph::Island& island, u32 bodyCount) {
    return !island_body_indices_valid(island, bodyCount);

IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph::Island& island,
    stats.totalBodies = static_cast<u32>(island.bodyIndices.size());
        if (is_static_or_kinematic_body(bodies, bodyIndex)) {
            ++stats.staticOrKinematicCount;
        if (is_sleeping_body(bodies, bodyIndex)) {
            ++stats.sleepingCount;
            ++stats.awakeDynamicCount;

bool island_all_dynamic_bodies_sleeping(const ContactIslandGraph::Island& island,
    const IslandSleepStats stats = compute_island_sleep_stats(island, bodies);
    return stats.awakeDynamicCount == 0u && stats.sleepingCount > 0u;

bool island_has_awake_dynamic_body(const ContactIslandGraph::Island& island,
    return compute_island_sleep_stats(island, bodies).awakeDynamicCount > 0u;

IslandSleepSolvePreflight preflight_island_sleep_solve(const ContactIslandGraph::Island& island,
    IslandSleepSolvePreflight preflight{};

    preflight.stats = compute_island_sleep_stats(island, bodies);
    preflight.allDynamicSleeping = island_all_dynamic_bodies_sleeping(island, bodies);

IslandSleepSolvePreflight preflight_island_sleep_solve_by_index(const ContactIslandGraph& graph,
    return preflight_island_sleep_solve(graph.island(islandIndex), bodies);

bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island,
    const IslandSleepSolvePreflight preflight = preflight_island_sleep_solve(island, bodies);
    return preflight.skipped || !preflight.can_solve();


                                          f32 linearThreshold,
                                          f32 angularThreshold) {


        ++preflight.eligibleDynamicCount;
        const f32 linearSpeed = bodies.linearVelocities[bodyIndex].length();
        const f32 angularSpeed = bodies.angularVelocities[bodyIndex].length();
        if (linearSpeed < linearThreshold && angularSpeed < angularThreshold) {
            ++preflight.belowThresholdCount;
            ++preflight.aboveThresholdCount;

    if (preflight.eligibleDynamicCount == 0u) {

    return preflight_island_wake(graph.island(islandIndex), bodies, linearThreshold, angularThreshold);

bool should_skip_island_sleep_detection(const ContactIslandGraph::Island& island,
        if (!is_static_or_kinematic_body(bodies, bodyIndex)) {

bool should_skip_island_sleep_detection_index(const ContactIslandGraph& graph,
    return should_skip_island_sleep_detection(graph.island(islandIndex), bodies);

IslandSolveJobPreflight preflight_solve_island_job(const ContactIslandGraph::Island& island,
    IslandSolveJobPreflight preflight{};
    preflight.emptyIsland = !island_has_constraints(island);
    preflight.outOfRangeBodyIndices = has_out_of_range_island_body_indices(island, bodies.count());
    preflight.sleep = preflight_island_sleep_solve(island, bodies);
    preflight.skipped = preflight.emptyIsland;

IslandSolveJobPreflight preflight_solve_island_job_by_index(const ContactIslandGraph& graph,
    return preflight_solve_island_job(graph.island(islandIndex), bodies, dt);

bool should_skip_solve_island_job_preflight(const ContactIslandGraph::Island& island,
    return !preflight_solve_island_job(island, bodies, dt).can_solve();

    const IslandSolveJobPreflight preflight = preflight_solve_island_job(island, bodies, dt);

IslandSolveJobResult solve_island_job_result(RigidBodySoA& bodies,
    IslandSolveJobResult result{};


    result.solved = solve_island_job(bodies,



bool is_awake_dynamic_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    return !is_static_or_kinematic_body(bodies, bodyIndex) && !is_sleeping_body(bodies, bodyIndex);

bool island_all_dynamic_bodies_sleeping(const RigidBodySoA& bodies,
    const IslandSleepPreflight preflight = preflight_island_sleep(bodies, island);
    return !preflight.skipped && preflight.allDynamicSleeping;

bool island_has_awake_dynamic_bodies(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {

bool body_has_wake_impetus(const RigidBodySoA& bodies, u32 bodyIndex, f32 velocityThreshold) {
    if (bodyIndex >= bodies.count() || is_static_or_kinematic_body(bodies, bodyIndex)) {
    if (!is_sleeping_body(bodies, bodyIndex)) {

    if (bodies.forces[bodyIndex].dot(bodies.forces[bodyIndex]) > 0.f ||
        bodies.torques[bodyIndex].dot(bodies.torques[bodyIndex]) > 0.f) {

    return linearSpeed > velocityThreshold || angularSpeed > velocityThreshold;



    preflight.allDynamicSleeping =


        const IslandSleepPreflight preflight = preflight_island_sleep(bodies, graph.island(islandIndex));
        } else if (preflight.allDynamicSleeping) {
            ++stats.hasAwakeCount;


bool has_awake_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return compute_island_sleep_stats(bodies, graph).hasAwakeCount > 0u;

IslandSleepGraphPreflight preflight_island_sleep_graph(const RigidBodySoA& bodies,
    preflight.stats = compute_island_sleep_stats(bodies, graph);
    preflight.skipped = preflight.stats.totalIslands == 0u;

bool should_skip_island_solve_sleeping(const RigidBodySoA& bodies,

bool should_skip_island_solve_sleeping_index(const ContactIslandGraph& graph,
    return should_skip_island_solve_sleeping(bodies, graph.island(islandIndex));

std::vector<u32> collect_awake_dispatchable_island_indices(const RigidBodySoA& bodies,
        if (!should_solve_island(job)) {
        if (should_skip_island_solve_sleeping(bodies, *job.island)) {

                                          f32 velocityThreshold) {
    if (should_skip_island_wake_check(island)) {

        if (!is_sleeping_body(bodies, bodyIndex) || is_static_or_kinematic_body(bodies, bodyIndex)) {
        if (body_has_wake_impetus(bodies, bodyIndex, velocityThreshold)) {

    return preflight_island_wake(bodies, graph.island(islandIndex), velocityThreshold);

bool should_skip_island_wake_check(const ContactIslandGraph::Island& island) {

IslandConstraintSolvePreflight preflight_island_constraint_solve(const RigidBodySoA& bodies,

    preflight.solve = preflight_island_solve(graph);

bool should_skip_island_constraint_solve(const RigidBodySoA& bodies,
    return !preflight_island_constraint_solve(bodies, graph, islandIndex, dt).can_solve();

    return dispatch_solve_island_sleep_guarded_result(bodies,

IslandDispatchResult dispatch_solve_island_sleep_guarded_result(

    if (!should_solve_island(extract_island(graph, islandIndex))) {
    if (should_skip_island_solve_sleeping(bodies, island)) {


u32 dispatch_awake_islands(RigidBodySoA& bodies,
    return dispatch_awake_islands_result(bodies,
        .solvedCount;

IslandBatchDispatchResult dispatch_awake_islands_result(
    IslandBatchDispatchResult result{};
    const IslandDispatchPreflight preflight = preflight_island_dispatch(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
        result.skippedCount = result.dispatchableCount;

    for (u32 islandIndex : collect_awake_dispatchable_island_indices(bodies, graph)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_sleep_guarded_result(
            ++result.solvedCount;


bool bodyIndexValid(const RigidBodySoA& bodies, u32 bodyIndex) {
    return bodyIndex < bodies.count();

bool hasNonZeroForce(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (!bodyIndexValid(bodies, bodyIndex)) {
    return bodies.forces[bodyIndex].dot(bodies.forces[bodyIndex]) > 0.f;

bool constraintPairHasEffectiveMass(const RigidBodySoA& bodies, u32 bodyA, u32 bodyB) {
    return island_effective_inv_mass(bodies, bodyA) + island_effective_inv_mass(bodies, bodyB) > 0.f;


bool body_is_sleeping(const RigidBodySoA& bodies, u32 bodyIndex) {

bool body_is_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex) {

bool body_is_active_dynamic(const RigidBodySoA& bodies, u32 bodyIndex) {
    return !body_is_static_or_kinematic(bodies, bodyIndex) && !body_is_sleeping(bodies, bodyIndex);

f32 island_effective_inv_mass(const RigidBodySoA& bodies, u32 bodyIndex) {
        return 0.f;
    if (body_is_static_or_kinematic(bodies, bodyIndex) || body_is_sleeping(bodies, bodyIndex)) {
    return bodies.invMasses[bodyIndex];

IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies,
    stats.bodyCount = static_cast<u32>(island.bodyIndices.size());
        if (body_is_static_or_kinematic(bodies, bodyIndex)) {
        } else if (body_is_sleeping(bodies, bodyIndex)) {
            ++stats.activeDynamicCount;


    preflight.stats = compute_island_sleep_stats(bodies, island);
        preflight.stats.activeDynamicCount == 0u && preflight.stats.sleepingCount > 0u;
    preflight.allStaticOrSleeping = preflight.stats.activeDynamicCount == 0u;


    return !preflight.skipped && preflight.allSleeping;

bool should_skip_solve_sleeping_island(const RigidBodySoA& bodies,
    return preflight.skipped || preflight.allStaticOrSleeping;


    const IslandSleepStats stats = compute_island_sleep_stats(bodies, island);
    preflight.sleepingBodyCount = stats.sleepingCount;
    if (preflight.sleepingBodyCount == 0u) {

    const bool hasActiveDynamic = stats.activeDynamicCount > 0u;
        if (!bodyIndexValid(bodies, bodyIndex) || !body_is_sleeping(bodies, bodyIndex)) {
        if (hasNonZeroForce(bodies, bodyIndex)) {
            ++preflight.forceWakeCount;
        } else if (hasActiveDynamic) {
            ++preflight.contactWakeCount;


bool should_skip_island_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandWakePreflight preflight = preflight_island_wake(bodies, island);
    return preflight.skipped || !preflight.can_wake();

bool should_wake_island(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    return !should_skip_island_wake(bodies, island);

u32 wake_island_bodies_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    if (!preflight.can_wake()) {

        const bool forceWake = hasNonZeroForce(bodies, bodyIndex);
        const bool contactWake = hasActiveDynamic && !forceWake;
        if (!forceWake && !contactWake) {

    return wake_island_bodies_guarded(bodies, graph.island(islandIndex));

u32 count_resolvable_island_constraints(const RigidBodySoA& bodies,
    u32 resolvableCount = 0u;
        if (constraintPairHasEffectiveMass(bodies, contact.bodyA, contact.bodyB)) {
            ++resolvableCount;
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        if (constraintPairHasEffectiveMass(bodies, constraint.bodyA, constraint.bodyB)) {
    return resolvableCount;


    preflight.ownedConstraintCount = island_constraint_count(island);
    preflight.resolvableConstraintCount =
        count_resolvable_island_constraints(bodies, island, distanceConstraints, contacts);

    return preflight_island_constraint_solve(bodies,

    return !preflight_island_constraint_solve(bodies, island, distanceConstraints, contacts, dt).can_solve();

    const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve(
        workBuffers.contactManifolds(),

bool is_static_or_kinematic_body(u32 flags) {

bool is_sleeping_body(u32 flags) {
    return (flags & RB_SLEEPING) != 0u;

bool is_below_sleep_threshold(const RigidBodySoA& bodies, u32 bodyIndex, const IslandSleepParams& params) {
    return linearSpeed < params.linearThreshold && angularSpeed < params.angularThreshold;

bool is_above_wake_threshold(const RigidBodySoA& bodies, u32 bodyIndex, const IslandSleepParams& params) {
    return linearSpeed >= params.linearThreshold || angularSpeed >= params.angularThreshold;

IslandSolveBodyStats compute_island_body_stats(const RigidBodySoA& bodies,
    IslandSolveBodyStats stats{};
        if (is_static_or_kinematic_body(flags)) {
        ++stats.dynamicCount;
        if (is_sleeping_body(flags)) {

bool is_island_all_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandSolveBodyStats stats = compute_island_body_stats(bodies, island);
    return stats.dynamicCount > 0u && stats.sleepingCount == stats.dynamicCount;

bool is_island_all_static_or_kinematic(const RigidBodySoA& bodies,
    return stats.bodyCount > 0u && stats.staticOrKinematicCount == stats.bodyCount;

IslandSolveBodyPreflight preflight_island_solve_bodies(const RigidBodySoA& bodies,
    IslandSolveBodyPreflight preflight{};

    preflight.stats = compute_island_body_stats(bodies, island);
    preflight.allStaticOrKinematic = is_island_all_static_or_kinematic(bodies, island);
    preflight.allSleeping = is_island_all_sleeping(bodies, island);

IslandSolveBodyPreflight preflight_island_solve_bodies_by_index(const RigidBodySoA& bodies,
    return preflight_island_solve_bodies(bodies, graph.island(islandIndex));

    return !preflight_island_solve_bodies(bodies, island).can_solve();

    return !preflight_island_solve_bodies_by_index(bodies, graph, islandIndex).can_solve();

bool should_solve_island_with_bodies(const IslandSolveJob& job,
    return should_solve_island(job) && preflight_island_solve_bodies(bodies, island).can_solve();

bool should_skip_island_solve_job_for_sleep(const IslandSolveJob& job,
    return !should_solve_island_with_bodies(job, bodies, island);

bool dispatch_solve_island_with_body_guards(RigidBodySoA& bodies,
    return dispatch_solve_island_with_body_guards_result(bodies,

IslandDispatchResult dispatch_solve_island_with_body_guards_result(

    if (!should_solve_island_with_bodies(job, bodies, island)) {


                                            const IslandSleepParams& params,

        ++preflight.dynamicCount;
            ++preflight.alreadySleepingCount;
        if (is_below_sleep_threshold(bodies, bodyIndex, params)) {

    preflight.allStaticOrKinematic = preflight.dynamicCount == 0u;

    return preflight_island_sleep(bodies, graph.island(islandIndex), params, dt);

                                          const IslandSleepParams& params) {

            ++preflight.sleepingCount;
        if (is_above_wake_threshold(bodies, bodyIndex, params)) {

    return preflight_island_wake(bodies, graph.island(islandIndex), params);

bool should_skip_island_sleep(const ContactIslandGraph::Island& island, f32 dt) {
    return !is_valid_island_solve_dt(dt) || should_skip_warm_start_island(island);

bool should_skip_island_sleep_index(const ContactIslandGraph& graph, u32 islandIndex, f32 dt) {
    return should_skip_island_sleep(graph.island(islandIndex), dt);


bool should_skip_island_wake_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_island_wake(graph.island(islandIndex));

IslandSleepGraphStats compute_island_sleep_graph_stats(const RigidBodySoA& bodies,
    IslandSleepGraphStats stats{};
        const IslandSleepPreflight preflight =
            preflight_island_sleep(bodies, graph.island(islandIndex), params, dt);
        } else if (preflight.allStaticOrKinematic) {
            ++stats.staticOnlyCount;
        } else if (preflight.can_sleep()) {
            ++stats.sleepableCount;

    preflight.stats = compute_island_sleep_graph_stats(bodies, graph, params, dt);
    preflight.skipped = preflight.invalidDt || preflight.stats.sleepableCount == 0u;

IslandWakeGraphStats compute_island_wake_graph_stats(const RigidBodySoA& bodies,
    IslandWakeGraphStats stats{};
        const IslandWakePreflight preflight =
            preflight_island_wake(bodies, graph.island(islandIndex), params);
        } else if (preflight.should_wake()) {
            ++stats.wakeableCount;

IslandWakeGraphPreflight preflight_island_wake_graph(const RigidBodySoA& bodies,
    IslandWakeGraphPreflight preflight{};
    preflight.stats = compute_island_wake_graph_stats(bodies, graph, params);
    preflight.skipped = preflight.stats.wakeableCount == 0u;

std::vector<u32> collect_sleepable_island_indices(const RigidBodySoA& bodies,
        if (preflight.can_sleep()) {

std::vector<u32> collect_wakeable_island_indices(const RigidBodySoA& bodies,
        if (preflight.should_wake()) {

bool sleep_island_bodies_guarded(RigidBodySoA& bodies,
    const IslandSleepPreflight preflight = preflight_island_sleep(bodies, island, params, dt);
    if (!preflight.can_sleep()) {

        if (is_static_or_kinematic_body(bodies.flags[bodyIndex])) {
        bodies.sleepTimers[bodyIndex] += dt;
        if (bodies.sleepTimers[bodyIndex] >= params.timeRequired) {
            bodies.flags[bodyIndex] |= RB_SLEEPING;
            bodies.linearVelocities[bodyIndex] = {};
            bodies.angularVelocities[bodyIndex] = {};

bool wake_island_bodies_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandWakePreflight preflight = preflight_island_wake(bodies, island, IslandSleepParams{});

    bool wokeAny = false;
        if (is_sleeping_body(bodies.flags[bodyIndex])) {
            wokeAny = true;
    return wokeAny;

bool sleep_island_bodies_by_index_guarded(RigidBodySoA& bodies,
    return sleep_island_bodies_guarded(bodies, graph.island(islandIndex), params, dt);

bool wake_island_bodies_by_index_guarded(RigidBodySoA& bodies,

u32 sleep_all_islands_guarded(RigidBodySoA& bodies,
    const IslandSleepGraphPreflight preflight = preflight_island_sleep_graph(bodies, graph, params, dt);
    if (!preflight.can_sleep_any()) {

    u32 sleptCount = 0u;
    for (u32 islandIndex : collect_sleepable_island_indices(bodies, graph, params, dt)) {
        if (sleep_island_bodies_by_index_guarded(bodies, graph, islandIndex, params, dt)) {
            ++sleptCount;
    return sleptCount;

u32 wake_all_islands_guarded(RigidBodySoA& bodies,
    const IslandWakeGraphPreflight preflight = preflight_island_wake_graph(bodies, graph, params);
    if (!preflight.should_wake_any()) {

    u32 wokeCount = 0u;
    for (u32 islandIndex : collect_wakeable_island_indices(bodies, graph, params)) {
        if (wake_island_bodies_by_index_guarded(bodies, graph, islandIndex)) {
            ++wokeCount;
    return wokeCount;





























































bool is_island_body_sleeping(u32 bodyFlags) {

bool is_island_body_static_or_kinematic(u32 bodyFlags) {


bool island_body_participates_in_solve(u32 bodyFlags, f32 invMass) {
    if (is_island_body_static_or_kinematic(bodyFlags) || is_island_body_sleeping(bodyFlags)) {
    return invMass > 0.f;


    if (!island_has_constraints(island) && island.bodyIndices.empty()) {




        const u32 bodyFlags = bodies.flags[bodyIndex];
        if (is_island_body_static_or_kinematic(bodyFlags)) {
            ++preflight.staticOrKinematicCount;
        } else if (is_island_body_sleeping(bodyFlags)) {















        if (is_island_body_sleeping(bodies.flags[bodyIndex])) {
        } else if (!is_island_body_static_or_kinematic(bodies.flags[bodyIndex])) {










    return preflight_island_wake(graph.island(islandIndex), bodies);

IslandSleepWakeStats compute_island_sleep_wake_stats(const ContactIslandGraph& graph,
    IslandSleepWakeStats stats{};

        const IslandSleepPreflight sleepPreflight = preflight_island_sleep(island, bodies);
        const IslandWakePreflight wakePreflight = preflight_island_wake(island, bodies);
        if (sleepPreflight.all_dynamic_sleeping()) {
        if (wakePreflight.should_wake()) {
            ++stats.wakeCandidateCount;




IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,
    IslandSleepWakeGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_wake_stats(graph, bodies);

bool should_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_wake_graph(graph, bodies).has_wake_candidates();

std::vector<u32> collect_wake_candidate_island_indices(const ContactIslandGraph& graph,

        const IslandWakePreflight preflight = preflight_island_wake(graph.island(islandIndex), bodies);
        const IslandSleepPreflight sleepPreflight = preflight_island_sleep(*job.island, bodies);


bool should_skip_island_solve_all_sleeping(const ContactIslandGraph::Island& island,
    return !preflight.skipped && preflight.all_dynamic_sleeping();

IslandSolveParticipationPreflight preflight_island_solve_participation(
    IslandSolveParticipationPreflight preflight{};

    preflight.ownedBodyCount = static_cast<u32>(island.bodyIndices.size());








        if (is_island_body_sleeping(bodyFlags)) {
            ++preflight.staticOrKinematicBodyCount;
        if (island_body_participates_in_solve(bodyFlags, bodies.invMasses[bodyIndex])) {
            ++preflight.participatingBodyCount;

bool should_skip_island_solve_no_participation(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_participation(island, bodies).can_solve();

    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_constraint_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_island_constraint_solve_by_index(const ContactIslandGraph& graph,
    return !preflight_island_constraint_solve_by_index(graph, islandIndex, bodies, contacts, distanceConstraints)
                .can_solve();

IslandConstraintSolveGraphStats compute_island_constraint_solve_stats(
    IslandConstraintSolveGraphStats stats{};
        const IslandConstraintSolvePreflight preflight =
            preflight_island_constraint_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);
        } else if (preflight.can_solve()) {
            ++stats.solveableCount;
        } else if (!preflight.refs.can_solve()) {
            ++stats.blockedByRefsCount;
        } else if (!preflight.bodies.can_solve()) {
            const IslandSleepPreflight sleepPreflight = preflight_island_sleep(graph.island(islandIndex), bodies);
                ++stats.blockedByBodiesCount;

u32 count_constraint_solveable_islands(const ContactIslandGraph& graph,
    return compute_island_constraint_solve_stats(graph, bodies, contacts, distanceConstraints).solveableCount;

bool has_constraint_solveable_islands(const ContactIslandGraph& graph,
    return count_constraint_solveable_islands(graph, bodies, contacts, distanceConstraints) > 0u;

IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(
    IslandConstraintSolveGraphPreflight preflight{};
    preflight.stats = compute_island_constraint_solve_stats(graph, bodies, contacts, distanceConstraints);
    preflight.skipped = !preflight.can_solve_any();

bool should_skip_island_constraint_solve_graph(
    return preflight_island_constraint_solve_graph(graph, bodies, contacts, distanceConstraints).skipped;

std::vector<u32> collect_constraint_solveable_island_indices(
        if (preflight.can_solve()) {

IslandSolvePipelinePreflight preflight_island_solve_pipeline(
    IslandSolvePipelinePreflight preflight{};

    preflight.wake = preflight_island_wake(island, bodies);
    preflight.solve = preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);

IslandSolvePipelinePreflight preflight_island_solve_pipeline_by_index(
    return preflight_island_solve_pipeline(graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_island_solve_pipeline(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_pipeline(island, bodies, contacts, distanceConstraints).can_solve();

    if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints)) {

bool solve_island_job_with_wake_guarded(RigidBodySoA& bodies,
    const IslandSolvePipelinePreflight preflight =
        preflight_island_solve_pipeline(island, bodies, workBuffers.contactManifolds(), distanceConstraints);
    if (preflight.should_wake_first()) {
        wake_island_sleepers_guarded(bodies, island);
    return solve_island_job_guarded(bodies,

bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,

bool dispatch_solve_island_with_wake_guarded(RigidBodySoA& bodies,
    if (!is_valid_island_solve_dt(dt) || !island_index_valid(graph, islandIndex)) {
    return solve_island_job_with_wake_guarded(bodies,

u32 dispatch_all_islands_with_wake_guarded(RigidBodySoA& bodies,
u32 dispatch_all_islands_skipping_sleepers(RigidBodySoA& bodies,
    return dispatch_all_islands_with_wake_result(bodies,

IslandBatchDispatchResult dispatch_all_islands_with_wake_result(
    return dispatch_all_islands_skipping_sleepers_result(bodies,

IslandBatchDispatchResult dispatch_all_islands_skipping_sleepers_result(
    const IslandDispatchPreflight dispatchPreflight = preflight_island_dispatch(graph, dt);
    result.dispatchableCount = dispatchPreflight.solve.stats.dispatchableCount;
    if (!dispatchPreflight.can_dispatch()) {

    wake_all_island_sleepers_guarded(bodies, graph);

    for (u32 islandIndex : collect_constraint_solveable_island_indices(graph, bodies, contacts, distanceConstraints)) {
        if (solve_island_job_guarded(bodies,
                                     invMassFn)) {

    const std::vector<u32> solveableIndices = collect_solveable_island_indices(graph, bodies);
    result.dispatchableCount = static_cast<u32>(solveableIndices.size());
    if (solveableIndices.empty()) {

    for (u32 islandIndex : solveableIndices) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_with_solve_guards_result(


    preflight.constraint = preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);



bool solve_island_job_pipeline_guarded(RigidBodySoA& bodies,

bool solve_island_job_pipeline_by_index_guarded(RigidBodySoA& bodies,
    return solve_island_job_pipeline_guarded(bodies,

IslandDispatchBodiesPreflight preflight_island_dispatch_with_bodies(const ContactIslandGraph& graph,
    IslandDispatchBodiesPreflight preflight{};
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.wake = preflight_island_wake_graph(graph, bodies);

bool should_skip_island_dispatch_with_bodies(const ContactIslandGraph& graph,
    return !preflight_island_dispatch_with_bodies(graph, bodies, dt).can_dispatch();

IslandDispatchResult dispatch_solve_island_pipeline_result(RigidBodySoA& bodies,
    if (!is_finite_island_solve_dt(dt)) {



IslandPipelineBatchDispatchResult dispatch_all_islands_pipeline_result(
IslandSleepAwareDispatchPreflight preflight_island_sleep_aware_dispatch(const ContactIslandGraph& graph,
    IslandSleepAwareDispatchPreflight preflight{};
    preflight.skipped = preflight.dispatch.skipped || preflight.sleep.skipped;

bool should_skip_island_sleep_aware_dispatch(const ContactIslandGraph& graph,
    return !preflight_island_sleep_aware_dispatch(graph, bodies, dt).can_dispatch();

        if (should_skip_island_sleep_solve(graph.island(islandIndex), bodies)) {
            graph.island(islandIndex), bodies, contacts, distanceConstraints);


    wake_island_sleepers_guarded(bodies, graph.island(islandIndex));
    return dispatch_solve_island_constraint_guarded(bodies,

IslandSleepAwareBatchDispatchResult dispatch_all_islands_sleep_aware_result(
    IslandPipelineBatchDispatchResult result{};
    const IslandDispatchBodiesPreflight preflight = preflight_island_dispatch_with_bodies(graph, bodies, dt);
    IslandSleepAwareBatchDispatchResult result{};
    const IslandSleepAwareDispatchPreflight preflight =
        preflight_island_sleep_aware_dispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;

    result.wakeCount = wake_all_island_sleepers_guarded(bodies, graph);

        const IslandSolvePipelinePreflight islandPreflight = preflight_island_solve_pipeline(
            graph.island(islandIndex), bodies, workBuffers.contactManifolds(), distanceConstraints);
        if (!islandPreflight.can_solve()) {
            if (!islandPreflight.skipped && islandPreflight.sleep.can_skip_solve()) {
                ++result.sleepingSkippedCount;

        const IslandDispatchResult dispatchResult = dispatch_solve_island_pipeline_result(bodies,
         collect_constraint_solveable_island_indices(graph, bodies, contacts, distanceConstraints)) {
        const IslandWakeResult wakeResult = wake_island_sleepers_result(bodies, graph, islandIndex);
        if (wakeResult.woke) {
            ++result.wokeCount;

        if (dispatch_solve_island_constraint_guarded(bodies,

u32 dispatch_all_islands_pipeline_guarded(RigidBodySoA& bodies,
    return dispatch_all_islands_pipeline_result(bodies,
u32 dispatch_all_islands_sleep_aware_guarded(RigidBodySoA& bodies,
    return dispatch_all_islands_sleep_aware_result(bodies,

const char* islandBuildRejectReasonName(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::EmptyInputs:
        return "EmptyInputs";
    case IslandBuildRejectReason::OutOfRangeContactBodies:
        return "OutOfRangeContactBodies";
    case IslandBuildRejectReason::OutOfRangeDistanceBodies:
        return "OutOfRangeDistanceBodies";
    return "Unknown";

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
        return IslandBuildRejectReason::EmptyInputs;
    if (preflight.stats.outOfRangeContactBodyCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeContactBodies;
    if (preflight.stats.outOfRangeDistanceBodyCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeDistanceBodies;
    return IslandBuildRejectReason::None;

bool island_build_rejects_for_reason(
    IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;

IslandBuildDeepenPreflight preflight_island_build_deepen(
    IslandBuildDeepenPreflight deepen{};
    deepen.stats = preflight_island_build(bodyCount, contacts, distanceConstraints).stats;
    deepen.reason = island_build_reject_reason(bodyCount, contacts, distanceConstraints);
    deepen.rejected = deepen.reason != IslandBuildRejectReason::None;
    return deepen;

bool should_skip_island_build_deepen(
    return !preflight_island_build_deepen(bodyCount, contacts, distanceConstraints).can_build();

bool should_run_island_build(
    return !should_skip_island_build_deepen(bodyCount, contacts, distanceConstraints);

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason) {
    case IslandConstraintSolveRejectReason::None:
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeConstraints:
        return "NoInRangeConstraints";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
        return IslandConstraintSolveRejectReason::EmptyIsland;
    const IslandConstraintRefsPreflight refs =
        preflight_island_constraint_refs(island, contacts, distanceConstraints);
    if (!refs.can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeConstraints;
    const IslandSolveBodiesPreflight bodyPreflight = preflight_island_solve_bodies(island, bodies);
    if (!bodyPreflight.can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    return IslandConstraintSolveRejectReason::None;

bool island_constraint_solve_rejects_for_reason(
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;

IslandConstraintSolveDeepenPreflight preflight_island_constraint_solve_deepen(
    IslandConstraintSolveDeepenPreflight deepen{};
    deepen.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    deepen.bodies = preflight_island_solve_bodies(island, bodies);
    deepen.reason = island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    deepen.rejected = deepen.reason != IslandConstraintSolveRejectReason::None;

bool should_skip_island_constraint_solve_deepen(
    return !preflight_island_constraint_solve_deepen(island, bodies, contacts, distanceConstraints).can_solve();

bool should_run_island_constraint_solve(
    return !should_skip_island_constraint_solve_deepen(island, bodies, contacts, distanceConstraints);

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason) {
    case IslandDispatchRejectReason::None:
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
        return IslandDispatchRejectReason::InvalidDt;
        return IslandDispatchRejectReason::NonFiniteDt;
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    return IslandDispatchRejectReason::None;

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;

IslandDispatchDeepenPreflight preflight_island_dispatch_deepen(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchDeepenPreflight deepen{};
    deepen.stats = compute_island_solve_stats(graph);
    deepen.reason = island_dispatch_reject_reason(graph, dt);
    deepen.rejected = deepen.reason != IslandDispatchRejectReason::None;

bool should_skip_island_dispatch_deepen(const ContactIslandGraph& graph, f32 dt) {
    return !preflight_island_dispatch_deepen(graph, dt).can_dispatch();

bool should_run_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return !should_skip_island_dispatch_deepen(graph, dt);

const char* islandSolveJobRejectReasonName(IslandSolveJobRejectReason reason) {
    case IslandSolveJobRejectReason::None:
    case IslandSolveJobRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandSolveJobRejectReason::EmptyIsland:
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    case IslandSolveJobRejectReason::InvalidDt:
    case IslandSolveJobRejectReason::NonFiniteDt:

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (job.island == nullptr) {
        return IslandSolveJobRejectReason::OutOfRangeIndex;
        return IslandSolveJobRejectReason::InvalidDt;
        return IslandSolveJobRejectReason::NonFiniteDt;
    if (job.empty) {
        return IslandSolveJobRejectReason::EmptyIsland;
    if (job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    return IslandSolveJobRejectReason::None;

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                           IslandSolveJobRejectReason expected) {
    return island_solve_job_reject_reason(job, dt) == expected;

IslandSolveJobDeepenPreflight preflight_solve_island_job_deepen(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobDeepenPreflight deepen{};
    deepen.constraintCount = job.constraintCount;
    deepen.reason = island_solve_job_reject_reason(job, dt);
    deepen.rejected = deepen.reason != IslandSolveJobRejectReason::None;

bool should_skip_solve_island_job_deepen(const IslandSolveJob& job, f32 dt) {
    return !preflight_solve_island_job_deepen(job, dt).can_dispatch();

bool should_run_solve_island_job(const IslandSolveJob& job, f32 dt) {
    return !should_skip_solve_island_job_deepen(job, dt);

const char* islandSleepSolveRejectReasonName(IslandSleepSolveRejectReason reason) {
    case IslandSleepSolveRejectReason::None:
    case IslandSleepSolveRejectReason::EmptyIsland:
    case IslandSleepSolveRejectReason::OutOfRangeIndex:
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
        return IslandSleepSolveRejectReason::EmptyIsland;
    if (preflight.can_skip_solve()) {
        return IslandSleepSolveRejectReason::AllSleeping;
    return IslandSleepSolveRejectReason::None;

IslandSleepSolveRejectReason island_sleep_solve_reject_reason_by_index(const ContactIslandGraph& graph,
        return IslandSleepSolveRejectReason::OutOfRangeIndex;
    return island_sleep_solve_reject_reason(graph.island(islandIndex), bodies);

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           IslandSleepSolveRejectReason expected) {
    return island_sleep_solve_reject_reason(island, bodies) == expected;

IslandSleepSolveDeepenPreflight preflight_island_sleep_solve_deepen(const ContactIslandGraph::Island& island,
    IslandSleepSolveDeepenPreflight deepen{};
    deepen.bodyCount = preflight.bodyCount;
    deepen.sleepingCount = preflight.sleepingCount;
    deepen.activeDynamicCount = preflight.activeDynamicCount;
    deepen.reason = island_sleep_solve_reject_reason(island, bodies);
    deepen.rejected = deepen.reason != IslandSleepSolveRejectReason::None;

IslandSleepSolveDeepenPreflight preflight_island_sleep_solve_deepen_by_index(const ContactIslandGraph& graph,
        deepen.reason = IslandSleepSolveRejectReason::OutOfRangeIndex;
        deepen.rejected = true;
    return preflight_island_sleep_solve_deepen(graph.island(islandIndex), bodies);

bool should_skip_island_sleep_solve_deepen(const ContactIslandGraph::Island& island,
    return !preflight_island_sleep_solve_deepen(island, bodies).can_solve();

bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !should_skip_island_sleep_solve_deepen(island, bodies);

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason) {
    case IslandWakeRejectReason::None:
    case IslandWakeRejectReason::EmptyIsland:
    case IslandWakeRejectReason::OutOfRangeIndex:
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
        return IslandWakeRejectReason::EmptyIsland;
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies);
    if (!preflight.should_wake_sleepers()) {
        return IslandWakeRejectReason::NoMixedSleepState;
    return IslandWakeRejectReason::None;

IslandWakeRejectReason island_wake_reject_reason_by_index(const ContactIslandGraph& graph,
        return IslandWakeRejectReason::OutOfRangeIndex;
    return island_wake_reject_reason(graph.island(islandIndex), bodies);

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;

IslandWakeDeepenPreflight preflight_island_wake_deepen(const ContactIslandGraph::Island& island,
    IslandWakeDeepenPreflight deepen{};
    deepen.reason = island_wake_reject_reason(island, bodies);
    deepen.rejected = deepen.reason != IslandWakeRejectReason::None;

IslandWakeDeepenPreflight preflight_island_wake_deepen_by_index(const ContactIslandGraph& graph,
        deepen.reason = IslandWakeRejectReason::OutOfRangeIndex;
    return preflight_island_wake_deepen(graph.island(islandIndex), bodies);

bool should_skip_island_wake_deepen(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_wake_deepen(island, bodies).can_wake();

bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !should_skip_island_wake_deepen(island, bodies);

IslandWakeAndSolvePreflight preflight_island_wake_and_solve(
    IslandWakeAndSolvePreflight preflight{};


IslandWakeAndSolvePreflight preflight_island_wake_and_solve_by_index(
    return preflight_island_wake_and_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_island_wake_and_solve(const ContactIslandGraph::Island& island,
    return !preflight_island_wake_and_solve(island, bodies, contacts, distanceConstraints).can_solve();

IslandWakeAndSolveGraphPreflight preflight_island_wake_and_solve_graph(const ContactIslandGraph& graph,
    IslandWakeAndSolveGraphPreflight preflight{};
    preflight.skipped = !is_finite_island_solve_dt(dt) || !preflight.solve.can_dispatch() ||
                        !preflight.sleep.has_solveable_islands();

bool should_skip_island_wake_and_solve_graph(const ContactIslandGraph& graph,
    return !preflight_island_wake_and_solve_graph(graph, bodies, dt).can_dispatch();

std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
        const IslandWakeAndSolvePreflight preflight =
            preflight_island_wake_and_solve(graph.island(islandIndex), bodies, contacts, distanceConstraints);

IslandWakeAndSolveResult dispatch_solve_island_with_wake_result(
    IslandWakeAndSolveResult result{};
    if (!island_index_valid(graph, islandIndex) || !is_finite_island_solve_dt(dt)) {

    const IslandWakeAndSolvePreflight preflight = preflight_island_wake_and_solve(
        island, bodies, workBuffers.contactManifolds(), distanceConstraints);

        result.woke = wake_island_sleepers_guarded(bodies, island);

    result.solved = solve_island_job_guarded(bodies,

    return dispatch_solve_island_with_wake_result(bodies,

IslandBatchDispatchResult dispatch_all_nonsleeping_islands_result(

    for (u32 islandIndex : collect_solveable_island_indices(graph, bodies, contacts, distanceConstraints)) {
        if (dispatch_solve_island_guarded(bodies,

IslandBatchWakeAndSolveResult dispatch_all_islands_with_wake_result(
    IslandBatchWakeAndSolveResult result{};
    const IslandWakeAndSolveGraphPreflight preflight = preflight_island_wake_and_solve_graph(graph, bodies, dt);
    result.solveableCount = preflight.solve.stats.dispatchableCount;
        result.skippedCount = result.solveableCount;

        const IslandWakeAndSolveResult dispatchResult = dispatch_solve_island_with_wake_result(
        if (dispatchResult.woke) {

const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {



    IslandBuildDeepenPreflight preflight{};
    preflight.stats = preflight_island_build(bodyCount, contacts, distanceConstraints).stats;
    preflight.reason = island_build_reject_reason(bodyCount, contacts, distanceConstraints);
    preflight.skipped = preflight.reason != IslandBuildRejectReason::None;

bool can_skip_island_build_deepen(

bool build_island_graph_with_preflight(ContactIslandGraph& graph,
    const IslandBuildDeepenPreflight preflight =
        preflight_island_build_deepen(bodyCount, contacts, distanceConstraints);
    if (!preflight.can_build()) {
        graph.clear();
    graph.build(bodyCount, contacts, distanceConstraints);

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";

        preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);
    if (!preflight.refs.can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    if (!preflight.bodies.can_solve()) {


    IslandConstraintSolveDeepenPreflight preflight{};
    preflight.reason = island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    preflight.skipped = preflight.reason != IslandConstraintSolveRejectReason::None;


IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
    IslandSolveBodiesPreflight preflight{};
    return preflight_island_solve_bodies(graph.island(islandIndex), bodies);

    return !preflight_island_constraint_solve_by_index(graph,
                                                       distanceConstraints)

bool can_skip_island_constraint_solve_deepen(

bool solve_island_job_with_preflight(RigidBodySoA& bodies,
    if (can_skip_island_constraint_solve_deepen(island, bodies, contacts, distanceConstraints)) {

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    case IslandSleepRejectReason::None:
    case IslandSleepRejectReason::EmptyIsland:
    case IslandSleepRejectReason::OutOfRangeIndex:
    case IslandSleepRejectReason::NotAllSleeping:
        return "NotAllSleeping";

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
        return IslandSleepRejectReason::EmptyIsland;
    if (!preflight.can_skip_solve()) {
        return IslandSleepRejectReason::NotAllSleeping;
    return IslandSleepRejectReason::None;

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";

    if (!preflight.hasMixedSleepState) {
    if (preflight.activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;

IslandWakeResult wake_island_sleepers_result(RigidBodySoA& bodies,
    IslandWakeResult result{};
IslandSleepDispatchPreflight preflight_island_sleep_dispatch(const ContactIslandGraph& graph,
    IslandSleepDispatchPreflight preflight{};

bool should_skip_island_sleep_dispatch(const ContactIslandGraph& graph,
    return !preflight_island_sleep_dispatch(graph, bodies, dt).can_dispatch();

IslandBatchSleepDispatchResult dispatch_all_islands_wake_and_solve_result(
    if (should_skip_island_sleep_dispatch(graph, bodies, dt)) {

    if (island_wake_reject_reason(island, bodies) != IslandWakeRejectReason::None) {


IslandFullDispatchPreflight preflight_dispatch_solve_island(
IslandDispatchSolveablePreflight preflight_island_dispatch_solveable(
    IslandFullDispatchPreflight preflight{};

    preflight.job = preflight_solve_island_job(job, dt);
    preflight.constraintSolve =
        preflight_island_constraint_solve_deepen(*job.island, bodies, contacts, distanceConstraints);
    preflight.wake = preflight_island_wake(*job.island, bodies);
    preflight.skipped = !preflight.can_dispatch();

bool dispatch_solve_island_with_preflight(RigidBodySoA& bodies,
    return dispatch_solve_island_with_preflight_result(bodies,

IslandDispatchResult dispatch_solve_island_with_preflight_result(

    const IslandFullDispatchPreflight preflight = preflight_dispatch_solve_island(
        graph, islandIndex, bodies, workBuffers.contactManifolds(), distanceConstraints, dt);

    if (preflight.wake.should_wake_sleepers()) {

    result.solved = solve_island_job_with_preflight(bodies,

IslandBatchDispatchResult dispatch_all_islands_with_preflight_result(

    for (u32 islandIndex : collect_nonsleeping_island_indices(graph, bodies)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_with_preflight_result(


u32 dispatch_all_islands_with_wake_guarded(

    const IslandSleepDispatchPreflight preflight = preflight_island_sleep_dispatch(graph, bodies, dt);


        const IslandDispatchResult dispatchResult = dispatch_solve_island_result(bodies,


    return preflight_island_constraint_solve(

IslandSleepAwareDispatchPreflight preflight_island_sleep_aware_dispatch(

    IslandSolveJob job{};
    job.island = &island;
    job.constraintCount = island_constraint_count(island);
    job.empty = job.constraintCount == 0u;

IslandSleepAwareDispatchPreflight preflight_island_sleep_aware_dispatch_by_index(
    return preflight_island_sleep_aware_dispatch(
        graph.island(islandIndex), bodies, contacts, distanceConstraints, dt);

IslandSleepAwareDispatchPreflight preflight_island_sleep_aware_dispatch_job(
    preflight = preflight_island_sleep_aware_dispatch(
        *job.island, bodies, contacts, distanceConstraints, dt);

IslandSleepAwareGraphPreflight preflight_island_sleep_aware_graph(const ContactIslandGraph& graph,
    IslandSleepAwareGraphPreflight preflight{};

bool should_skip_island_sleep_aware_dispatch(const ContactIslandGraph::Island& island,
    return !preflight_island_sleep_aware_dispatch(island, bodies, contacts, distanceConstraints, dt)
                .can_dispatch();

std::vector<u32> collect_solveable_island_indices(


    return dispatch_solve_island(bodies,

    return dispatch_solve_island_sleep_guarded(bodies,

bool dispatch_solve_island_sleep_aware_guarded(RigidBodySoA& bodies,

    if (should_skip_island_sleep_aware_dispatch(island, bodies, contacts, distanceConstraints, dt)) {


IslandDispatchResult dispatch_solve_island_sleep_aware_result(RigidBodySoA& bodies,

        preflight_island_sleep_aware_dispatch(island, bodies, contacts, distanceConstraints, dt);

    result.solved = dispatch_solve_island_sleep_aware_guarded(bodies,

u32 dispatch_nonsleeping_islands_guarded(RigidBodySoA& bodies,
    return dispatch_nonsleeping_islands_result(bodies,

IslandBatchSleepAwareDispatchResult dispatch_nonsleeping_islands_result(
    IslandBatchSleepAwareDispatchResult result{};
    const IslandSleepAwareGraphPreflight preflight = preflight_island_sleep_aware_graph(graph, bodies, dt);
        result.skippedDispatchCount = result.dispatchableCount;

        const IslandDispatchResult dispatchResult = dispatch_solve_island_sleep_aware_result(bodies,

        if (should_skip_island_sleep_solve(island, bodies)) {
            ++result.skippedSleepCount;
        } else if (should_skip_island_constraint_solve(
                       island, bodies, contacts, distanceConstraints)) {
            ++result.skippedConstraintCount;
            ++result.skippedDispatchCount;
    const IslandBatchWakeResult wakeResult = wake_all_island_sleepers_result(bodies, graph);
    result.wokeCount = wakeResult.wokeCount;
    result.bodiesWoken = wakeResult.bodiesWoken;

    const IslandBatchBodiesDispatchResult dispatchResult = dispatch_all_islands_with_bodies_result(
        bodies, graph, workBuffers, distanceConstraints, dt, contactCompliance, invMassFn);
    result.solvedCount = dispatchResult.solvedCount;
    result.skippedCount = dispatchResult.skippedCount;
    result.solveableCount = dispatchResult.solveableCount;
    result.skipped = dispatchResult.skipped && result.wokeCount == 0u;
    IslandDispatchSolveablePreflight preflight{};
        preflight_island_constraint_solve_graph(graph, bodies, contacts, distanceConstraints);
    preflight.skipped = preflight.dispatch.skipped || preflight.constraintSolve.skipped;

bool should_skip_island_dispatch_solveable(const ContactIslandGraph& graph,
    return !preflight_island_dispatch_solveable(graph, bodies, contacts, distanceConstraints, dt)

u32 dispatch_solveable_islands_guarded(RigidBodySoA& bodies,
    return dispatch_solveable_islands_result(bodies,

IslandBatchDispatchSolveableResult dispatch_solveable_islands_result(
    IslandBatchDispatchSolveableResult result{};
    const IslandDispatchSolveablePreflight preflight =
        preflight_island_dispatch_solveable(graph, bodies, contacts, distanceConstraints, dt);
    result.solveableCount = preflight.constraintSolve.stats.solveableCount;


IslandSleepWakeDispatchResult dispatch_solve_island_with_sleep_wake_result(
    IslandSleepWakeDispatchResult result{};



    if (should_skip_island_sleep_solve(*job.island, bodies)) {

    result.wokeSleepers = wakeResult.woke;




bool dispatch_solve_island_with_sleep_wake_guards(RigidBodySoA& bodies,
    return dispatch_solve_island_with_sleep_wake_result(bodies,

IslandBatchSleepWakeDispatchResult dispatch_all_islands_with_sleep_wake_result(
    IslandBatchSleepWakeDispatchResult result{};
    const IslandSleepWakeDispatchPreflight preflight = preflight_island_sleep_wake_dispatch(graph, bodies, dt);
    result.solveableCount = count_solveable_islands(graph, bodies);



}




    const IslandBatchWakeResult wakeBatch = wake_all_island_sleepers_result(bodies, graph);
    result.wokeCount = wakeBatch.wokeCount;

    for (u32 islandIndex : collect_solveable_island_indices(graph, bodies)) {
        const IslandSleepWakeDispatchResult dispatchResult = dispatch_solve_island_with_sleep_wake_result(

u32 dispatch_all_islands_with_sleep_wake_guards(RigidBodySoA& bodies,
    return dispatch_all_islands_with_sleep_wake_result(bodies,

IslandSolveDispatchPreflight preflight_island_solve_dispatch(
    IslandSolveDispatchPreflight preflight{};


IslandSolveDispatchPreflight preflight_island_solve_dispatch_by_index(
    return preflight_island_solve_dispatch(graph.island(islandIndex), bodies, contacts, distanceConstraints);

bool should_skip_island_solve_dispatch(const ContactIslandGraph::Island& island,
    return !preflight_island_solve_dispatch(island, bodies, contacts, distanceConstraints).can_solve();

    if (should_skip_island_solve_dispatch(island, bodies, contacts, distanceConstraints)) {

bool dispatch_solve_island_guarded(RigidBodySoA& bodies,
    return dispatch_solve_island_guarded_result(bodies,

IslandGuardedDispatchResult dispatch_solve_island_guarded_result(
    IslandGuardedDispatchResult result{};
    if (!is_finite_island_solve_dt(dt) || !island_index_valid(graph, islandIndex)) {

    const IslandSolveDispatchPreflight preflight = preflight_island_solve_dispatch(

        result.wokeSleepers = wake_island_sleepers_guarded(bodies, island);


        const IslandSolveDispatchPreflight preflight =
            preflight_island_solve_dispatch(graph.island(islandIndex), bodies, contacts, distanceConstraints);

u32 dispatch_all_islands_guarded(RigidBodySoA& bodies,
    return dispatch_all_islands_guarded_result(bodies,

IslandBatchGuardedDispatchResult dispatch_all_islands_guarded_result(
    IslandBatchGuardedDispatchResult result{};

    const std::vector<u32> solveableIndices =
        collect_solveable_island_indices(graph, bodies, contacts, distanceConstraints);

        const IslandGuardedDispatchResult dispatchResult = dispatch_solve_island_guarded_result(
            bodies, graph, islandIndex, workBuffers, distanceConstraints, dt, contactCompliance, invMassFn);
        if (dispatchResult.wokeSleepers) {

IslandFullSolvePreflight preflight_solve_island_job_full(
    IslandFullSolvePreflight preflight{};
    if (job.island == nullptr || should_skip_island_solve_job(job)) {
    preflight.constraint =
        preflight_island_constraint_solve(*job.island, bodies, contacts, distanceConstraints);

bool should_skip_solve_island_job_full(const IslandSolveJob& job,
    return !preflight_solve_island_job_full(job, bodies, contacts, distanceConstraints, dt).can_solve();


IslandConstraintSolveResult solve_island_job_guarded_result(
IslandDispatchAfterWakePreflight preflight_island_dispatch_after_wake(const ContactIslandGraph& graph,
    IslandDispatchAfterWakePreflight preflight{};

bool should_skip_island_dispatch_after_wake(const ContactIslandGraph& graph,
    return !preflight_island_dispatch_after_wake(graph, bodies, dt).can_dispatch();

bool dispatch_solve_island_after_wake_guarded(RigidBodySoA& bodies,
    return dispatch_solve_island_after_wake_result(bodies,

IslandDispatchResult dispatch_solve_island_after_wake_result(
    IslandConstraintSolveResult result{};
    if (!island_index_valid(graph, islandIndex) || !is_valid_island_solve_dt(dt)) {

    wake_island_sleepers_result(bodies, island, islandIndex);



IslandSleepWakeSolvePreflight preflight_island_sleep_wake_solve(
    IslandSleepWakeSolvePreflight preflight{};


IslandSleepWakeSolvePreflight preflight_island_sleep_wake_solve_by_index(
    return preflight_island_sleep_wake_solve(

bool should_skip_island_sleep_wake_solve(const ContactIslandGraph::Island& island,
    const IslandSleepWakeSolvePreflight preflight =
        preflight_island_sleep_wake_solve(island, bodies, contacts, distanceConstraints);
    return preflight.skipped || preflight.can_skip_entirely() || !preflight.can_solve();

IslandSleepWakeSolveGraphStats compute_island_sleep_wake_solve_stats(
    IslandSleepWakeSolveGraphStats stats{};
        const IslandSleepWakeSolvePreflight preflight = preflight_island_sleep_wake_solve(
        } else if (preflight.can_skip_entirely()) {
            if (preflight.needs_wake()) {

IslandSleepWakeSolveGraphPreflight preflight_island_sleep_wake_solve_graph(
    IslandSleepWakeSolveGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_wake_solve_stats(graph, bodies, contacts, distanceConstraints);
    preflight.skipped = preflight.stats.solveableCount == 0u;

bool should_skip_island_sleep_wake_solve_graph(
    return preflight_island_sleep_wake_solve_graph(graph, bodies, contacts, distanceConstraints).skipped;

std::vector<u32> collect_sleep_wake_solveable_island_indices(
        if (preflight.skipped || preflight.can_skip_entirely() || !preflight.can_solve()) {

bool wake_and_solve_island_guarded(RigidBodySoA& bodies,



bool wake_and_solve_island_by_index_guarded(RigidBodySoA& bodies,
    return wake_and_solve_island_guarded(bodies,

u32 dispatch_all_islands_sleep_wake_guarded(
u32 dispatch_all_islands_after_wake_guarded(
    return dispatch_all_islands_sleep_wake_result(bodies,
    return dispatch_all_islands_after_wake_result(bodies,

IslandSleepWakeSolveBatchResult dispatch_all_islands_sleep_wake_result(
IslandBatchDispatchResult dispatch_all_islands_after_wake_result(
    IslandSleepWakeSolveBatchResult result{};
    const IslandSleepWakeSolveGraphPreflight preflight = preflight_island_sleep_wake_solve_graph(
        graph, bodies, workBuffers.contactManifolds(), distanceConstraints);
    result.solveableCount = preflight.stats.solveableCount;
    if (!preflight.can_dispatch() || !is_valid_island_solve_dt(dt)) {

    for (u32 islandIndex : collect_sleep_wake_solveable_island_indices(
             graph, bodies, workBuffers.contactManifolds(), distanceConstraints)) {
        const IslandSleepWakeSolvePreflight islandPreflight = preflight_island_sleep_wake_solve(
        if (islandPreflight.needs_wake() &&
            wake_island_sleepers_by_index_guarded(bodies, graph, islandIndex)) {

        if (wake_and_solve_island_by_index_guarded(bodies,
    const IslandDispatchAfterWakePreflight preflight = preflight_island_dispatch_after_wake(graph, bodies, dt);

        wake_all_island_sleepers_result(bodies, graph);

        const IslandDispatchResult dispatchResult = dispatch_solve_island_after_wake_result(



std::vector<u32> collect_sleep_aware_dispatchable_island_indices(const ContactIslandGraph& graph,



u32 wake_and_dispatch_all_islands_guarded(
    const IslandSleepAwareDispatchPreflight preflight = preflight_island_sleep_aware_dispatch(graph, bodies, dt);

    u32 solvedCount = 0u;
    for (u32 islandIndex : collect_sleep_aware_dispatchable_island_indices(graph, bodies)) {
        if (wake_and_solve_island_guarded(bodies,
            ++solvedCount;
    return solvedCount;

    case IslandBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandBuildRejectReason::OutOfRangeContact:
        return "OutOfRangeContact";
    case IslandBuildRejectReason::OutOfRangeDistance:
        return "OutOfRangeDistance";
    default:

    const IslandBuildInputCoverage coverage =
        ContactIslandGraph::scanBuildInputs(bodyCount, contacts, distanceConstraints);
    if (coverage.isEmptyInput()) {
        return IslandBuildRejectReason::EmptyInput;
    if (coverage.outOfRangeContactCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeContact;
    if (coverage.outOfRangeDistanceCount > 0u) {
        return IslandBuildRejectReason::OutOfRangeDistance;

bool island_build_rejects_for_reason(u32 bodyCount,

bool can_skip_island_build_for_reason(u32 bodyCount,
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) !=
           IslandBuildRejectReason::None;

bool should_run_island_build(u32 bodyCount,
    return !can_skip_island_build_for_reason(bodyCount, contacts, distanceConstraints);

    case IslandConstraintSolveRejectReason::StaleRefs:
        return "StaleRefs";

        return IslandConstraintSolveRejectReason::StaleRefs;


bool can_skip_island_constraint_solve_for_reason(
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) !=
           IslandConstraintSolveRejectReason::None;

    return !can_skip_island_constraint_solve_for_reason(island, bodies, contacts, distanceConstraints);

    case IslandSleepRejectReason::AllSleeping:


    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
        if (is_body_static_or_kinematic(flags)) {
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
            ++activeDynamicCount;

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;

bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     IslandSleepRejectReason expected) {
    return island_sleep_reject_reason(island, bodies) == expected;

bool can_skip_island_sleep_solve_for_reason(const ContactIslandGraph::Island& island,
    return island_sleep_reject_reason(island, bodies) != IslandSleepRejectReason::None;

    return !can_skip_island_sleep_solve_for_reason(island, bodies);




    if (sleepingCount == 0u || activeDynamicCount == 0u) {


bool can_skip_island_wake_for_reason(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return island_wake_reject_reason(island, bodies) != IslandWakeRejectReason::None;

    return !can_skip_island_wake_for_reason(island, bodies);

bool dispatch_solve_island_with_sleep_guard(RigidBodySoA& bodies,



IslandBatchSleepDispatchResult dispatch_all_islands_with_sleep_guard_result(

    for (u32 islandIndex : collect_dispatchable_island_indices(graph)) {

        if (dispatch_solve_island(bodies,

IslandJobSolvePreflight preflight_island_job_solve(const IslandSolveJob& job,
    IslandJobSolvePreflight preflight{};

    preflight.sleep = preflight_island_sleep(*job.island, bodies);

bool should_skip_island_job_solve(const IslandSolveJob& job,
    return !preflight_island_job_solve(job, bodies, contacts, distanceConstraints, dt).can_solve();

IslandGraphSolveStats compute_island_graph_solve_stats(
    IslandGraphSolveStats stats{};
        const IslandJobSolvePreflight preflight =
            preflight_island_job_solve(job, bodies, contacts, distanceConstraints, dt);
        if (preflight.skipped || job.empty) {
        if (!preflight.job.can_dispatch()) {
            ++stats.skippedDispatchCount;
        if (preflight.sleep.can_skip_solve()) {
            ++stats.skippedSleepCount;
        if (!preflight.constraint.can_solve()) {
            ++stats.skippedConstraintCount;

u32 count_solveable_islands(const ContactIslandGraph& graph,
    return compute_island_graph_solve_stats(graph, bodies, contacts, distanceConstraints, dt).solveableCount;

bool has_solveable_islands(const ContactIslandGraph& graph,
    return count_solveable_islands(graph, bodies, contacts, distanceConstraints, dt) > 0u;

IslandGraphSolvePreflight preflight_island_graph_solve(const ContactIslandGraph& graph,
    IslandGraphSolvePreflight preflight{};
    preflight.solveStats =
        compute_island_graph_solve_stats(graph, bodies, contacts, distanceConstraints, dt);
    preflight.skipped = !preflight.dispatch.can_dispatch() || preflight.solveStats.solveableCount == 0u;

bool should_skip_island_graph_solve(const ContactIslandGraph& graph,
    return !preflight_island_graph_solve(graph, bodies, contacts, distanceConstraints, dt).can_solve();


std::vector<IslandSolveJob> filter_solveable_jobs(const std::vector<IslandSolveJob>& jobs,
    std::vector<IslandSolveJob> solveable;
    solveable.reserve(jobs.size());
    for (const IslandSolveJob& job : jobs) {
        if (!should_skip_island_job_solve(job, bodies, contacts, distanceConstraints, dt)) {
            solveable.push_back(job);
    return solveable;


    if (should_skip_island_job_solve(job, bodies, contacts, distanceConstraints, dt) || job.island == nullptr) {

IslandDispatchResult dispatch_solve_island_guarded_result(RigidBodySoA& bodies,
    if (should_skip_island_job_solve(job, bodies, contacts, distanceConstraints, dt)) {
    result.solved = dispatch_solve_island_job_guarded(bodies,

u32 dispatch_all_solveable_islands(RigidBodySoA& bodies,
    return dispatch_all_solveable_islands_result(bodies,

IslandBatchSolveResult dispatch_all_solveable_islands_result(
    IslandBatchSolveResult result{};
    const IslandGraphSolvePreflight preflight = preflight_island_graph_solve(
        graph, bodies, workBuffers.contactManifolds(), distanceConstraints, dt);
    result.solveableCount = preflight.solveStats.solveableCount;

         collect_solveable_island_indices(graph, bodies, workBuffers.contactManifolds(), distanceConstraints, dt)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_guarded_result(bodies,

    return dispatch_solve_island_guarded(bodies,

IslandBatchSolveResult dispatch_all_islands_with_wake_result(
    result.wokeCount = wake_all_island_sleepers_guarded(bodies, graph);

    const IslandGraphSolvePreflight preflight =
        preflight_island_graph_solve(graph, bodies, workBuffers.contactManifolds(), distanceConstraints, dt);


const char* islandSleepRejectReasonName(IslandSleepRejectReason reason) {
    case IslandSleepRejectReason::HasActiveDynamics:
        return "HasActiveDynamics";

IslandSleepRejectReason islandSleepRejectReason(const ContactIslandGraph::Island& island,
    const IslandSleepPreflight sleep = preflight_island_sleep(island, bodies);
    if (!sleep.can_skip_solve()) {
        return IslandSleepRejectReason::HasActiveDynamics;

bool islandSleepRejectsForReason(const ContactIslandGraph::Island& island,
    return islandSleepRejectReason(island, bodies) == expected;

IslandSleepRejectPreflight preflight_island_sleep_reject(const ContactIslandGraph::Island& island,
    IslandSleepRejectPreflight preflight{};
    preflight.reason = islandSleepRejectReason(island, bodies);
    preflight.emptyIsland = preflight.reason == IslandSleepRejectReason::EmptyIsland;
    preflight.hasActiveDynamics = preflight.reason == IslandSleepRejectReason::HasActiveDynamics;

IslandSleepRejectPreflight preflight_island_sleep_reject_by_index(const ContactIslandGraph& graph,
        preflight.reason = IslandSleepRejectReason::OutOfRangeIndex;
        preflight.sleep.skipped = true;
    return preflight_island_sleep_reject(graph.island(islandIndex), bodies);


IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,
    const IslandWakePreflight wake = preflight_island_wake(island, bodies);
    if (!wake.hasMixedSleepState) {
    if (wake.activeDynamicCount == 0u) {

bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
    return islandWakeRejectReason(island, bodies) == expected;

IslandWakeRejectPreflight preflight_island_wake_reject(const ContactIslandGraph::Island& island,
    IslandWakeRejectPreflight preflight{};
    preflight.reason = islandWakeRejectReason(island, bodies);
    preflight.emptyIsland = preflight.reason == IslandWakeRejectReason::EmptyIsland;
    preflight.noMixedSleepState = preflight.reason == IslandWakeRejectReason::NoMixedSleepState;
    preflight.noActiveDynamic = preflight.reason == IslandWakeRejectReason::NoActiveDynamic;

IslandWakeRejectPreflight preflight_island_wake_reject_by_index(const ContactIslandGraph& graph,
        preflight.reason = IslandWakeRejectReason::OutOfRangeIndex;
        preflight.wake.skipped = true;
    return preflight_island_wake_reject(graph.island(islandIndex), bodies);

const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason) {
    case IslandSleepGraphRejectReason::None:
    case IslandSleepGraphRejectReason::AllSleepingOrEmpty:
        return "AllSleepingOrEmpty";

IslandSleepGraphRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,
    const IslandSleepGraphPreflight sleepGraph = preflight_island_sleep_graph(graph, bodies);
    if (sleepGraph.has_solveable_islands()) {
        return IslandSleepGraphRejectReason::None;
    return IslandSleepGraphRejectReason::AllSleepingOrEmpty;

bool islandSleepGraphRejectsForReason(const ContactIslandGraph& graph,
                                      IslandSleepGraphRejectReason expected) {
    return islandSleepGraphRejectReason(graph, bodies) == expected;

IslandSleepGraphRejectPreflight preflight_island_sleep_graph_reject(const ContactIslandGraph& graph,
    IslandSleepGraphRejectPreflight preflight{};
    preflight.reason = islandSleepGraphRejectReason(graph, bodies);
    preflight.allSleepingOrEmpty = preflight.reason == IslandSleepGraphRejectReason::AllSleepingOrEmpty;

bool canSkipIslandSleepSolveGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();

bool shouldRunIslandSleepSolveGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();

const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason) {
    case IslandWakeGraphRejectReason::None:
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";

IslandWakeGraphRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,
    const IslandWakeGraphPreflight wakeGraph = preflight_island_wake_graph(graph, bodies);
    if (wakeGraph.can_wake()) {
        return IslandWakeGraphRejectReason::None;
    return IslandWakeGraphRejectReason::NoWakeableIslands;

bool islandWakeGraphRejectsForReason(const ContactIslandGraph& graph,
                                     IslandWakeGraphRejectReason expected) {
    return islandWakeGraphRejectReason(graph, bodies) == expected;

IslandWakeGraphRejectPreflight preflight_island_wake_graph_reject(const ContactIslandGraph& graph,
    IslandWakeGraphRejectPreflight preflight{};
    preflight.reason = islandWakeGraphRejectReason(graph, bodies);
    preflight.noWakeableIslands = preflight.reason == IslandWakeGraphRejectReason::NoWakeableIslands;

bool canSkipIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_wake_graph_reject(graph, bodies).can_wake();

bool shouldRunIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_wake_graph_reject(graph, bodies).can_wake();
    preflight.skipped = preflight.sleep.skipped || preflight.dispatch.skipped;


IslandBatchSleepSkipDispatchResult dispatch_all_islands_skipping_sleepers_result(
    IslandBatchSleepSkipDispatchResult result{};




IslandBatchSleepSkipDispatchResult dispatch_all_islands_with_wake_result(

    case IslandConstraintSolveRejectReason::AllSleeping:



        return IslandConstraintSolveRejectReason::AllSleeping;




const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason) {






    case IslandWakeRejectReason::UniformSleepState:
        return "UniformSleepState";



    if (activeDynamicCount == 0u) {
    if (sleepingCount == 0u) {
        return IslandWakeRejectReason::UniformSleepState;



    result.reason = island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    if (result.reason != IslandConstraintSolveRejectReason::None) {


    return solve_island_job_guarded_result(bodies,

IslandConstraintSolveResult solve_island_job_with_wake_guarded(

IslandDispatchResult dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
    result.islandIndex = job.islandIndex;

    const IslandConstraintSolveResult solveResult = solve_island_job_guarded_result(bodies,
    result.solved = solveResult.solved;
    result.skipped = !solveResult.solved;
bool dispatch_solve_island_pipeline_guarded(RigidBodySoA& bodies,
    return dispatch_solve_island_pipeline_result(bodies,

IslandDispatchResult dispatch_solve_island_pipeline_result(

    const IslandSolvePipelinePreflight pipeline = preflight_island_solve_pipeline(
    if (!pipeline.can_dispatch()) {


    if (should_skip_island_sleep_solve_deepen(island, bodies) ||
        should_skip_island_constraint_solve_deepen(
            island, bodies, workBuffers.contactManifolds(), distanceConstraints)) {



IslandBatchDispatchResult dispatch_all_islands_pipeline_result(


        const IslandDispatchResult dispatchResult = dispatch_solve_island_pipeline_result(

IslandPipelineSolvePreflight preflight_pipeline_solve_island(
    IslandPipelineSolvePreflight preflight{};

    job.empty = false;

IslandPipelineSolvePreflight preflight_pipeline_solve_island_by_index(
    return preflight_pipeline_solve_island(

bool should_skip_pipeline_solve_island(const ContactIslandGraph::Island& island,
    return !preflight_pipeline_solve_island(island, bodies, contacts, distanceConstraints, dt).can_solve();

IslandPipelineDispatchStats compute_island_pipeline_dispatch_stats(
    IslandPipelineDispatchStats stats{};
        const IslandPipelineSolvePreflight pipelinePreflight =
            preflight_pipeline_solve_island(island, bodies, contacts, distanceConstraints, dt);

        if (pipelinePreflight.skipped) {
        if (wakePreflight.should_wake_sleepers()) {
        if (pipelinePreflight.sleep.can_skip_solve()) {
        if (!pipelinePreflight.constraint.refs.can_solve()) {
            ++stats.staleRefsCount;
        if (!pipelinePreflight.constraint.bodies.can_solve()) {
            ++stats.noMovableBodiesCount;
        if (pipelinePreflight.can_solve()) {
            ++stats.pipelineDispatchableCount;

u32 count_pipeline_dispatchable_islands(const ContactIslandGraph& graph,
    return compute_island_pipeline_dispatch_stats(graph, bodies, contacts, distanceConstraints, dt)
        .pipelineDispatchableCount;

bool has_pipeline_dispatchable_islands(const ContactIslandGraph& graph,
    return count_pipeline_dispatchable_islands(graph, bodies, contacts, distanceConstraints, dt) > 0u;

IslandPipelineDispatchPreflight preflight_pipeline_dispatch(
    IslandPipelineDispatchPreflight preflight{};
    preflight.stats =
        compute_island_pipeline_dispatch_stats(graph, bodies, contacts, distanceConstraints, dt);
    preflight.skipped = preflight.dispatch.skipped || preflight.stats.pipelineDispatchableCount == 0u;

bool should_skip_pipeline_dispatch(const ContactIslandGraph& graph,
    return !preflight_pipeline_dispatch(graph, bodies, contacts, distanceConstraints, dt).can_dispatch();

std::vector<u32> collect_pipeline_dispatchable_island_indices(
        const IslandPipelineSolvePreflight preflight = preflight_pipeline_solve_island(


IslandPipelineDispatchResult dispatch_solve_island_pipeline_result(
    IslandPipelineDispatchResult result{};

    const IslandPipelineSolvePreflight preflight =

    if (preflight_island_wake(island, bodies).should_wake_sleepers()) {



    const IslandPipelineDispatchPreflight preflight = preflight_pipeline_dispatch(
    result.pipelineDispatchableCount = preflight.stats.pipelineDispatchableCount;
        result.skippedCount = result.pipelineDispatchableCount;

    for (u32 islandIndex : collect_pipeline_dispatchable_island_indices(
             graph, bodies, workBuffers.contactManifolds(), distanceConstraints, dt)) {
        const IslandPipelineDispatchResult dispatchResult = dispatch_solve_island_pipeline_result(

u32 dispatch_solveable_islands(RigidBodySoA& bodies,

IslandBatchDispatchResult dispatch_solveable_islands_result(
    const IslandDispatchRejectReason rejectReason = island_dispatch_reject_reason(graph, bodies, dt);
    if (rejectReason != IslandDispatchRejectReason::None) {

        if (dispatchResult.solved) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;
        }
    }
    return result;
}

const char* island_solve_reject_reason_name(IslandSolveRejectReason reason) {
    switch (reason) {
    case IslandSolveRejectReason::None:
        return "None";
    case IslandSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSolveRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveRejectReason::StaleConstraintRefs:
        return "StaleConstraintRefs";
    case IslandSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    case IslandSolveRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";

IslandSolveRejectReason island_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt) {
    if (!island_has_constraints(island)) {
        return IslandSolveRejectReason::EmptyIsland;
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveRejectReason::InvalidDt;
    if (should_skip_island_sleep_solve(island, bodies)) {
        return IslandSolveRejectReason::AllSleeping;
    if (should_skip_island_constraint_refs(island, contacts, distanceConstraints)) {
        return IslandSolveRejectReason::StaleConstraintRefs;
    if (should_skip_island_solve_bodies(island, bodies)) {
        return IslandSolveRejectReason::NoMovableBodies;
    return IslandSolveRejectReason::None;

bool island_solve_rejects_for_reason(
    f32 dt,
    IslandSolveRejectReason expected) {
    return island_solve_reject_reason(island, bodies, contacts, distanceConstraints, dt) == expected;

IslandExtendedSolvePreflight preflight_island_extended_solve(
    IslandExtendedSolvePreflight preflight{};
    preflight.reason =
        island_solve_reject_reason(island, bodies, contacts, distanceConstraints, dt);
        preflight.skipped = true;
        return preflight;

    preflight.constraint = preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints);
    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.skipped = preflight.reason != IslandSolveRejectReason::None;

bool should_skip_island_extended_solve(
    return !preflight_island_extended_solve(island, bodies, contacts, distanceConstraints, dt).can_solve();

const char* island_pipeline_reject_reason_name(IslandPipelineRejectReason reason) {
    case IslandPipelineRejectReason::None:
    case IslandPipelineRejectReason::InvalidDt:
    case IslandPipelineRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandPipelineRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";

IslandPipelineRejectReason island_pipeline_reject_reason(const ContactIslandGraph& graph,
        return IslandPipelineRejectReason::InvalidDt;
    if (!has_dispatchable_islands(graph)) {
        return IslandPipelineRejectReason::NoDispatchableIslands;
    if (should_skip_island_sleep_solve_graph(graph, bodies)) {
        return IslandPipelineRejectReason::AllIslandsSleeping;
    return IslandPipelineRejectReason::None;

bool island_pipeline_rejects_for_reason(const ContactIslandGraph& graph,
                                        IslandPipelineRejectReason expected) {
    return island_pipeline_reject_reason(graph, bodies, dt) == expected;

IslandPipelinePreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,
    IslandPipelinePreflight preflight{};
    preflight.reason = island_pipeline_reject_reason(graph, bodies, dt);
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.skipped = preflight.reason != IslandPipelineRejectReason::None;

bool should_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
    return !preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();

std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
                                                  const RigidBodySoA& bodies) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!should_solve_island(job)) {
            continue;
        const IslandSleepPreflight sleepPreflight = preflight_island_sleep(graph.island(islandIndex), bodies);
        if (sleepPreflight.skipped || sleepPreflight.can_skip_solve()) {
        indices.push_back(islandIndex);
    return indices;

u32 count_solveable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return static_cast<u32>(collect_solveable_island_indices(graph, bodies).size());

bool has_solveable_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_solveable_islands(graph, bodies) > 0u;

IslandPipelineDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandPipelineDispatchResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;

    const ContactIslandGraph::Island& island = graph.island(islandIndex);


    result.woke = wake_island_sleepers_guarded(bodies, island);

    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    if (should_skip_island_extended_solve(island, bodies, contacts, distanceConstraints, dt)) {

    result.solved = solve_island_job(bodies,
                                     island,
                                     workBuffers,
                                     distanceConstraints,
                                     dt,
                                     contactCompliance,
                                     invMassFn);
    result.skipped = !result.solved;

u32 dispatch_all_islands_pipeline_guarded(
    return dispatch_all_islands_pipeline_result(bodies,
                                                graph,
                                                invMassFn)
        .solvedCount;

IslandBatchPipelineDispatchResult dispatch_all_islands_pipeline_result(
    IslandBatchPipelineDispatchResult result{};
    const IslandPipelinePreflight preflight = preflight_island_pipeline_dispatch(graph, bodies, dt);
    result.solveableCount = count_solveable_islands(graph, bodies);
    if (!preflight.can_dispatch()) {
        result.skippedCount = result.solveableCount;

    for (u32 islandIndex : collect_solveable_island_indices(graph, bodies)) {
        const IslandPipelineDispatchResult dispatchResult = dispatch_island_pipeline_guarded(
            bodies,
            islandIndex,
        if (dispatchResult.woke) {
            ++result.wokeCount;
        if (dispatchResult.solved) {
            ++result.solvedCount;
        } else if (dispatchResult.skipped) {
            ++result.skippedCount;

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(
    IslandPipelineDispatchPreflight preflight{};

    return preflight_island_pipeline_dispatch_job(job, bodies, contacts, distanceConstraints, dt);

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch_job(
    const IslandSolveJob& job,
    preflight.job = preflight_solve_island_job(job, dt);
    if (job.island == nullptr || job.empty) {
        preflight.skipped = preflight.job.skipped;

    preflight.sleep = preflight_island_sleep(*job.island, bodies);
    preflight.wake = preflight_island_wake(*job.island, bodies);
    preflight.constraintSolve =
        preflight_island_constraint_solve(*job.island, bodies, contacts, distanceConstraints);

IslandPipelineGraphPreflight preflight_island_pipeline_dispatch_graph(const ContactIslandGraph& graph,
    IslandPipelineGraphPreflight preflight{};
    preflight.skipped = !preflight.can_dispatch();

    return !preflight_island_pipeline_dispatch(graph,
                                               contacts,
                                               dt)
        .can_dispatch();

bool should_skip_island_pipeline_dispatch_graph(const ContactIslandGraph& graph,
    return !preflight_island_pipeline_dispatch_graph(graph, bodies, dt).can_dispatch();

std::vector<u32> collect_pipeline_dispatchable_island_indices(
        if (!should_skip_island_pipeline_dispatch(graph,
                                                  dt)) {

bool dispatch_solve_island_pipeline_guarded(RigidBodySoA& bodies,
    return dispatch_solve_island_pipeline_result(bodies,
        .solved;

IslandDispatchResult dispatch_solve_island_pipeline_result(RigidBodySoA& bodies,
    IslandDispatchResult result{};

    const IslandPipelineDispatchPreflight preflight =
        preflight_island_pipeline_dispatch(graph, islandIndex, bodies, contacts, distanceConstraints, dt);

    if (preflight.should_wake_first()) {
        wake_island_sleepers_guarded(bodies, island);


u32 dispatch_all_islands_pipeline_guarded(RigidBodySoA& bodies,

IslandBatchDispatchResult dispatch_all_islands_pipeline_result(
IslandBatchDispatchResult dispatch_island_solve_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandPipelineGraphPreflight preflight = preflight_island_pipeline_dispatch_graph(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    if (preflight.wake.can_wake()) {
        wake_all_island_sleepers_guarded(bodies, graph);

    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    for (u32 islandIndex :
         collect_pipeline_dispatchable_island_indices(graph, bodies, contacts, distanceConstraints, dt)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_pipeline_result(bodies,
                                                                                        graph,
                                                                                        islandIndex,
                                                                                        workBuffers,
                                                                                        distanceConstraints,
                                                                                        dt,
                                                                                        contactCompliance,
                                                                                        invMassFn);
        if (dispatchResult.solved) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;
    if (should_skip_island_solve_pipeline(bodies.count(),
                                          workBuffers.contactManifolds(),
                                          bodies,
                                          dt)) {

    return dispatch_solveable_islands_result(bodies,
}

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                                                   const RigidBodySoA& bodies,
                                                                   f32 dt) {
    IslandPipelineDispatchPreflight preflight{};
    preflight.dispatchReason = diagnose_island_dispatch_reject(graph, dt);
    preflight.solve = preflight_island_dispatch(graph, dt);
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.skipped = preflight.dispatchReason != IslandDispatchRejectReason::None;
    return preflight;
}

bool should_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          f32 dt) {
    return !preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

IslandPipelineDispatchResult dispatch_solve_island_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandPipelineDispatchResult result{};
    result.islandIndex = islandIndex;

    const IslandSolveJob job = extract_island(graph, islandIndex);
    result.dispatchReason = diagnose_island_solve_job_reject(job, dt);
    if (result.dispatchReason != IslandSolveJobRejectReason::None) {
        result.skipped = true;
        return result;
    }

    const ContactIslandGraph::Island& island = *job.island;
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();

    result.constraintReason =
        diagnose_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints);
    if (result.constraintReason != IslandConstraintSolveRejectReason::None) {
        result.skipped = true;
        return result;
    }

    result.wakeReason = diagnose_island_wake_reject(island, bodies);
    if (result.wakeReason == IslandWakeRejectReason::None) {
        result.wokeSleepers = wake_island_sleepers_guarded(bodies, island);
    }

    if (diagnose_island_sleep_reject(island, bodies) == IslandSleepRejectReason::AllSleeping) {
        result.skipped = true;
        return result;
    }

    const IslandDispatchResult dispatchResult = dispatch_solve_island_result(bodies,
                                                                             graph,
                                                                             islandIndex,
                                                                             workBuffers,
                                                                             distanceConstraints,
                                                                             dt,
                                                                             contactCompliance,
                                                                             invMassFn);
    result.solved = dispatchResult.solved;
    result.skipped = !dispatchResult.solved;
    return result;
}

IslandBatchPipelineDispatchResult dispatch_all_islands_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchPipelineDispatchResult result{};
    const IslandPipelineDispatchPreflight preflight = preflight_island_pipeline_dispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.solve.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    for (u32 islandIndex : collect_nonsleeping_island_indices(graph, bodies)) {
        const IslandPipelineDispatchResult pipelineResult = dispatch_solve_island_pipeline_result(
            bodies,
            graph,
            islandIndex,
            workBuffers,
            distanceConstraints,
            dt,
            contactCompliance,
            invMassFn);
        if (pipelineResult.wokeSleepers) {
            ++result.wokeCount;
        }
        if (pipelineResult.solved) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;
        }
    }
    return result;
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    }
    return "Unknown";
}

const char* island_job_dispatch_reject_reason_name(IslandJobDispatchRejectReason reason) {
    switch (reason) {
    case IslandJobDispatchRejectReason::None:
        return "None";
    case IslandJobDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandJobDispatchRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandJobDispatchRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    }
    return "Unknown";
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::StaleRefs:
        return "StaleRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    }
    return "Unknown";
}

const char* island_sleep_reject_reason_name(IslandSleepRejectReason reason) {
    switch (reason) {
    case IslandSleepRejectReason::None:
        return "None";
    case IslandSleepRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandSleepRejectReason::AllSleeping:
        return "AllSleeping";
    }
    return "Unknown";
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::OutOfRangeIndex:
        return "OutOfRangeIndex";
    case IslandWakeRejectReason::NoMixedSleep:
        return "NoMixedSleep";
    }
    return "Unknown";
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

IslandJobDispatchRejectReason island_job_dispatch_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandJobDispatchRejectReason::InvalidDt;
    }
    if (job.island == nullptr && job.islandIndex == ContactIslandGraph::invalidIsland) {
        return IslandJobDispatchRejectReason::OutOfRangeIndex;
    }
    if (should_skip_island_solve_job(job)) {
        return IslandJobDispatchRejectReason::EmptyIsland;
    }
    return IslandJobDispatchRejectReason::None;
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (should_skip_island_constraint_refs(island, contacts, distanceConstraints)) {
        return IslandConstraintSolveRejectReason::StaleRefs;
    }
    if (should_skip_island_solve_bodies(island, bodies)) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandSleepRejectReason island_sleep_reject_reason_by_index(const ContactIslandGraph& graph,
                                                            u32 islandIndex,
                                                            const RigidBodySoA& bodies) {
    if (!island_index_valid(graph, islandIndex)) {
        return IslandSleepRejectReason::OutOfRangeIndex;
    }
    return island_sleep_reject_reason(graph.island(islandIndex), bodies);
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (sleepingCount == 0u || activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoMixedSleep;
    }
    return IslandWakeRejectReason::None;
}

IslandWakeRejectReason island_wake_reject_reason_by_index(const ContactIslandGraph& graph,
                                                          u32 islandIndex,
                                                          const RigidBodySoA& bodies) {
    if (!island_index_valid(graph, islandIndex)) {
        return IslandWakeRejectReason::OutOfRangeIndex;
    }
    return island_wake_reject_reason(graph.island(islandIndex), bodies);
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

bool island_job_dispatch_rejects_for_reason(const IslandSolveJob& job,
                                            f32 dt,
                                            IslandJobDispatchRejectReason expected) {
    return island_job_dispatch_reject_reason(job, dt) == expected;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

bool island_sleep_rejects_for_reason(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies,
                                     IslandSleepRejectReason expected) {
    return island_sleep_reject_reason(island, bodies) == expected;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

IslandSolvePipelinePreflight preflight_island_solve_pipeline(const ContactIslandGraph& graph,
                                                             const RigidBodySoA& bodies,
                                                             f32 dt) {
    IslandSolvePipelinePreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.reason = island_dispatch_reject_reason(graph, dt);
    preflight.skipped = preflight.dispatch.skipped;
    return preflight;
}

bool should_skip_island_solve_pipeline(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt) {
    return !preflight_island_solve_pipeline(graph, bodies, dt).can_dispatch();
}

IslandSolvePipelineResult dispatch_island_solve_pipeline(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandSolvePipelineResult result{};
    const IslandSolvePipelinePreflight preflight = preflight_island_solve_pipeline(graph, bodies, dt);
    result.reason = preflight.reason;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        return result;
    }

    result.wokeCount = wake_all_island_sleepers_guarded(bodies, graph);
    result.dispatch = dispatch_all_islands_result(bodies,
                                                  graph,
                                                  workBuffers,
                                                  distanceConstraints,
                                                  dt,
                                                  contactCompliance,
                                                  invMassFn);
    result.skipped = result.dispatch.skipped;
    return result;
}

IslandDispatchJobPreflight preflight_dispatch_island_job(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt) {
    IslandDispatchJobPreflight preflight{};
    preflight.job = preflight_solve_island_job(job, dt);
    if (dt <= 0.f) {
        preflight.reason = IslandDispatchRejectReason::InvalidDt;
        preflight.skipped = true;
        return preflight;
    }
    if (!std::isfinite(dt)) {
        preflight.reason = IslandDispatchRejectReason::NonFiniteDt;
        preflight.skipped = true;
        return preflight;
    }
    if (preflight.job.skipped || !should_solve_island(job) || job.island == nullptr) {
        preflight.reason = job.constraintCount == 0u ? IslandDispatchRejectReason::NoConstraints
                                                     : IslandDispatchRejectReason::EmptyJob;
        preflight.skipped = true;
        return preflight;
    }

    preflight.constraint =
        preflight_island_constraint_solve(*job.island, bodies, contacts, distanceConstraints);
    preflight.sleep = preflight_island_sleep(*job.island, bodies);

    if (!preflight.constraint.can_solve()) {
        switch (preflight.constraint.reason) {
        case IslandConstraintSolveRejectReason::StaleContactRefs:
        case IslandConstraintSolveRejectReason::StaleDistanceRefs:
            preflight.reason = IslandDispatchRejectReason::StaleConstraintRefs;
            break;
        case IslandConstraintSolveRejectReason::AllSleeping:
            preflight.reason = IslandDispatchRejectReason::AllSleeping;
            break;
        case IslandConstraintSolveRejectReason::NoMovableBodies:
            preflight.reason = IslandDispatchRejectReason::NoMovableBodies;
            break;
        default:
            preflight.reason = IslandDispatchRejectReason::NoConstraints;
            break;
        }
        preflight.skipped = true;
        return preflight;
    }

    if (preflight.sleep.can_skip_solve()) {
        preflight.reason = IslandDispatchRejectReason::AllSleeping;
        preflight.skipped = true;
        return preflight;
    }

    return preflight;
}

bool should_skip_dispatch_island_job(const IslandSolveJob& job,
                                     const RigidBodySoA& bodies,
                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     f32 dt) {
    return !preflight_dispatch_island_job(job, bodies, contacts, distanceConstraints, dt).can_dispatch();
}

std::vector<u32> collect_solveable_island_indices(const ContactIslandGraph& graph,
                                                const RigidBodySoA& bodies) {
    const std::vector<u32> dispatchable = collect_dispatchable_island_indices(graph);
    const std::vector<u32> nonsleeping = collect_nonsleeping_island_indices(graph, bodies);
    std::vector<u32> solveable;
    solveable.reserve(dispatchable.size());
    for (u32 islandIndex : dispatchable) {
        for (u32 nonsleepingIndex : nonsleeping) {
            if (islandIndex == nonsleepingIndex) {
                solveable.push_back(islandIndex);
                break;
            }
        }
    }
    return solveable;
}

bool dispatch_solve_island_job_guarded(RigidBodySoA& bodies,
                                       const IslandSolveJob& job,
                                       SolverWorkBuffers& workBuffers,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt,
                                       f32 contactCompliance,
                                       const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_solve_island_job_guarded_result(bodies,
                                                    job,
                                                    workBuffers,
                                                    distanceConstraints,
                                                    dt,
                                                    contactCompliance,
                                                    invMassFn)
        .solved;
}

IslandDispatchResult dispatch_solve_island_job_guarded_result(
    RigidBodySoA& bodies,
    const IslandSolveJob& job,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandDispatchResult result{};
    result.islandIndex = job.islandIndex;
    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    const IslandDispatchJobPreflight preflight =
        preflight_dispatch_island_job(job, bodies, contacts, distanceConstraints, dt);
    result.reason = classifyIslandDispatchRejectReason(preflight);
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        return result;
    }

    result.solved = solve_island_job(bodies,
                                     *job.island,
                                     workBuffers,
                                     distanceConstraints,
                                     dt,
                                     contactCompliance,
                                     invMassFn);
    result.skipped = !result.solved;
    return result;
}

IslandBuildRejectReason islandBuildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_build(bodyCount, contacts, distanceConstraints).reason;
}

bool islandBuildRejectsForReason(u32 bodyCount,
                                 const std::vector<narrowphase::ContactManifold>& contacts,
                                 const std::vector<DistanceConstraint>& distanceConstraints,
                                 IslandBuildRejectReason expected) {
    return islandBuildRejectReason(bodyCount, contacts, distanceConstraints) == expected;
}

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints).reason;
}

bool islandConstraintSolveRejectsForReason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           const std::vector<narrowphase::ContactManifold>& contacts,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                           IslandConstraintSolveRejectReason expected) {
    return islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints) == expected;
}

IslandDispatchRejectReason islandDispatchRejectReason(
    const IslandSolveJob& job,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt) {
    return preflight_dispatch_island_job(job, bodies, contacts, distanceConstraints, dt).reason;
}

bool islandDispatchRejectsForReason(const IslandSolveJob& job,
                                    f32 dt,
                                    IslandDispatchRejectReason expected) {
    return islandDispatchRejectReason(job, dt) == expected;
}

bool islandDispatchRejectsForReason(const IslandSolveJob& job,
                                    const RigidBodySoA& bodies,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints,
                                    f32 dt,
                                    IslandDispatchRejectReason expected) {
    return islandDispatchRejectReason(job, bodies, contacts, distanceConstraints, dt) == expected;
}

IslandWarmStartRejectReason islandWarmStartRejectReason(const ContactIslandGraph::Island& island,
                                                        const std::vector<f32>& priorDistanceLambdas,
                                                        const std::vector<f32>& priorContactLambdas) {
    const IslandWarmStartPreflight preflight =
        preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    if (preflight.skipped) {
        return IslandWarmStartRejectReason::EmptyIsland;
    }
    if (!preflight.can_warm_start()) {
        return IslandWarmStartRejectReason::NoPriorData;
    }
    return IslandWarmStartRejectReason::None;
}

IslandWarmStartRejectReason islandWarmStartRejectReason(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    const IslandContactImpulseWarmStartPreflight preflight =
        preflight_warm_start_contact_impulses(island, contacts, dt);
    if (preflight.skipped) {
        return IslandWarmStartRejectReason::EmptyIsland;
    }
    if (preflight.invalidDt) {
        return IslandWarmStartRejectReason::InvalidDt;
    }
    if (preflight.ownedContactCount > 0u && preflight.nonZeroImpulseCount == 0u) {
        return IslandWarmStartRejectReason::NoImpulses;
    }
    if (!preflight.can_warm_start()) {
        return IslandWarmStartRejectReason::NoImpulses;
    }
    return IslandWarmStartRejectReason::None;
}

bool islandWarmStartRejectsForReason(const ContactIslandGraph::Island& island,
                                     const std::vector<f32>& priorDistanceLambdas,
                                     const std::vector<f32>& priorContactLambdas,
                                     IslandWarmStartRejectReason expected) {
    return islandWarmStartRejectReason(island, priorDistanceLambdas, priorContactLambdas) == expected;
}

IslandDispatchRejectReason islandDispatchRejectReason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool islandDispatchRejectsForReason(const ContactIslandGraph& graph,
                                    f32 dt,
                                    IslandDispatchRejectReason expected) {
    return islandDispatchRejectReason(graph, dt) == expected;
}

IslandSolveRejectReason islandSolveJobRejectReason(const IslandSolveJob& job, f32 dt) {
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveRejectReason::InvalidDt;
    }
    if (should_skip_island_solve_job(job)) {
        return IslandSolveRejectReason::EmptyIsland;
    }
    return IslandSolveRejectReason::None;
}

IslandSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandSolveRejectReason::EmptyIsland;
    }
    if (!preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve()) {
        return IslandSolveRejectReason::NoInRangeRefs;
    }
    if (!preflight_island_solve_bodies(island, bodies).can_solve()) {
        return IslandSolveRejectReason::NoMovableBodies;
    }
    return IslandSolveRejectReason::None;
}

bool islandSolveRejectsForReason(const IslandSolveJob& job,
                                 f32 dt,
                                 IslandSolveRejectReason expected) {
    return islandSolveJobRejectReason(job, dt) == expected;
}

bool islandConstraintSolveRejectsForReason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           const std::vector<narrowphase::ContactManifold>& contacts,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                           IslandSolveRejectReason expected) {
    return islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints) == expected;
}

IslandSleepRejectReason islandSleepRejectReason(const ContactIslandGraph::Island& island,
                                                const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepRejectReason::AllSleeping;
    }
    return IslandSleepRejectReason::None;
}

IslandSleepRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies) {
    const IslandSleepGraphStats stats = compute_island_sleep_stats(graph, bodies);
    if (stats.fullyActiveCount == 0u && stats.mixedSleepCount == 0u) {
        return IslandSleepRejectReason::NoSolveableIslands;
    }
    return IslandSleepRejectReason::None;
}

bool islandSleepRejectsForReason(const ContactIslandGraph::Island& island,
                                 const RigidBodySoA& bodies,
                                 IslandSleepRejectReason expected) {
    return islandSleepRejectReason(island, bodies) == expected;
}

IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,
                                              const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (sleepingCount == 0u || activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoWakeTarget;
    }
    return IslandWakeRejectReason::None;
}

IslandWakeRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,
                                                   const RigidBodySoA& bodies) {
    if (compute_island_wake_stats(graph, bodies).wakeableCount == 0u) {
        return IslandWakeRejectReason::NoWakeableIslands;
    }
    return IslandWakeRejectReason::None;
}

bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies,
                                IslandWakeRejectReason expected) {
    return islandWakeRejectReason(island, bodies) == expected;
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::NullIsland:
        return "NullIsland";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (!std::isfinite(dt)) {
        return IslandSolveJobRejectReason::NonFiniteDt;
    }
    if (job.empty) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    if (job.island == nullptr) {
        return IslandSolveJobRejectReason::NullIsland;
    }
    if (job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    }
    return IslandSolveJobRejectReason::None;
}

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                         f32 dt,
                                         IslandSolveJobRejectReason expected) {
    return island_solve_job_reject_reason(job, dt) == expected;
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!std::isfinite(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::StaleRefs:
        return "StaleRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    case IslandConstraintSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }

    const IslandConstraintRefsPreflight refs =
        preflight_island_constraint_refs(island, contacts, distanceConstraints);
    if (!refs.can_solve()) {
        return IslandConstraintSolveRejectReason::StaleRefs;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }
    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandConstraintSolveRejectReason::AllSleeping;
    }

    const IslandSolveBodiesPreflight bodyPreflight = preflight_island_solve_bodies(island, bodies);
    if (!bodyPreflight.can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }

    return IslandConstraintSolveRejectReason::None;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) ==
           expected;
}

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason) {
    switch (reason) {
    case IslandSleepSolveRejectReason::None:
        return "None";
    case IslandSleepSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepSolveRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u && sleepingCount > 0u) {
        return IslandSleepSolveRejectReason::AllSleeping;
    }

    return IslandSleepSolveRejectReason::None;
}

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected) {
    return island_sleep_solve_reject_reason(island, bodies) == expected;
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";
    case IslandWakeRejectReason::UniformSleepState:
        return "UniformSleepState";
    default:
        return "Unknown";
    }
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }

    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++sleepingCount;
        } else {
            ++activeDynamicCount;
        }
    }

    if (activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;
    }
    if (sleepingCount == 0u) {
        return IslandWakeRejectReason::UniformSleepState;
    }

    return IslandWakeRejectReason::None;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::NoSolveableIslands:
        return "NoSolveableIslands";
    default:
        return "Unknown";
    }
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    const IslandSleepGraphStats stats = compute_island_sleep_stats(graph, bodies);
    if (stats.fullyActiveCount == 0u && stats.mixedSleepCount == 0u) {
        return IslandSleepGraphRejectReason::NoSolveableIslands;
    }
    return IslandSleepGraphRejectReason::None;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    default:
        return "Unknown";
    }
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    const IslandWakeGraphStats stats = compute_island_wake_stats(graph, bodies);
    if (stats.wakeableCount == 0u) {
        return IslandWakeGraphRejectReason::NoWakeableIslands;
    }
    return IslandWakeGraphRejectReason::None;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

const char* island_pipeline_dispatch_reject_reason_name(IslandPipelineDispatchRejectReason reason) {
    switch (reason) {
    case IslandPipelineDispatchRejectReason::None:
        return "None";
    case IslandPipelineDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandPipelineDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandPipelineDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandPipelineDispatchRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    default:
        return "Unknown";
    }
}

IslandPipelineDispatchRejectReason island_pipeline_dispatch_reject_reason(const ContactIslandGraph& graph,
                                                                        const RigidBodySoA& bodies,
                                                                        f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandPipelineDispatchRejectReason::InvalidDt;
    }
    if (!std::isfinite(dt)) {
        return IslandPipelineDispatchRejectReason::NonFiniteDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandPipelineDispatchRejectReason::NoDispatchableIslands;
    }
    if (should_skip_island_sleep_solve_graph(graph, bodies)) {
        return IslandPipelineDispatchRejectReason::AllIslandsSleeping;
    }
    return IslandPipelineDispatchRejectReason::None;
}

bool island_pipeline_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies,
                                                 f32 dt,
                                                 IslandPipelineDispatchRejectReason expected) {
    return island_pipeline_dispatch_reject_reason(graph, bodies, dt) == expected;
}

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                                                   const RigidBodySoA& bodies,
                                                                   f32 dt) {
    IslandPipelineDispatchPreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.reason = island_pipeline_dispatch_reject_reason(graph, bodies, dt);
    preflight.skipped = preflight.reason != IslandPipelineDispatchRejectReason::None;
    return preflight;
}

bool should_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          f32 dt) {
    return !preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

bool should_run_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                         const RigidBodySoA& bodies,
                                         f32 dt) {
    return preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandPipelineDispatchPreflight preflight = preflight_island_pipeline_dispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    wake_all_island_sleepers_guarded(bodies, graph);

    for (u32 islandIndex : collect_dispatchable_island_indices(graph)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_result(bodies,
                                                                                 graph,
                                                                                 islandIndex,
                                                                                 workBuffers,
                                                                                 distanceConstraints,
                                                                                 dt,
                                                                                 contactCompliance,
                                                                                 invMassFn);
        if (dispatchResult.solved) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;
        }
    }
    return result;
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    return preflight_island_dispatch(graph, dt).reason;
}

IslandSolveRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    return preflight_solve_island_job(job, dt).reason;
}

IslandSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints).reason;
}

IslandSleepRejectReason island_sleep_reject_reason(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies) {
    return preflight_island_sleep(island, bodies).reason;
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    return preflight_island_wake(island, bodies).reason;
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph(graph, bodies).reason;
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    return preflight_island_wake_graph(graph, bodies).reason;
}

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandDispatchRejectReason islandDispatchRejectReason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool islandDispatchRejectsForReason(const ContactIslandGraph& graph,
                                    f32 dt,
                                    IslandDispatchRejectReason expected) {
    return islandDispatchRejectReason(graph, dt) == expected;
}

IslandDispatchRejectPreflight preflightIslandDispatchReject(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchRejectPreflight preflight{};
    preflight.reason = islandDispatchRejectReason(graph, dt);
    preflight.solve = preflight_island_solve(graph);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.skipped = preflight.reason != IslandDispatchRejectReason::None;
    return preflight;
}

bool canSkipIslandDispatch(const ContactIslandGraph& graph, f32 dt) {
    return !preflightIslandDispatchReject(graph, dt).can_dispatch();
}

bool shouldRunIslandDispatch(const ContactIslandGraph& graph, f32 dt) {
    return preflightIslandDispatchReject(graph, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandDispatchRejectPreflight preflight = preflightIslandDispatchReject(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }
    return dispatch_all_islands_result(bodies,
                                     graph,
                                     workBuffers,
                                     distanceConstraints,
                                     dt,
                                     contactCompliance,
                                     invMassFn);
}

const char* islandSolveJobRejectReasonName(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::NullIsland:
        return "NullIsland";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandSolveJobRejectReason islandSolveJobRejectReason(const IslandSolveJob& job, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::NonFiniteDt;
    }
    if (job.empty) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    if (job.island == nullptr) {
        return IslandSolveJobRejectReason::NullIsland;
    }
    if (job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    }
    return IslandSolveJobRejectReason::None;
}

bool islandSolveJobRejectsForReason(const IslandSolveJob& job,
                                    f32 dt,
                                    IslandSolveJobRejectReason expected) {
    return islandSolveJobRejectReason(job, dt) == expected;
}

IslandSolveJobRejectPreflight preflightIslandSolveJobReject(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobRejectPreflight preflight{};
    preflight.reason = islandSolveJobRejectReason(job, dt);
    preflight.invalidDt = preflight.reason == IslandSolveJobRejectReason::InvalidDt ||
                          preflight.reason == IslandSolveJobRejectReason::NonFiniteDt;
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = preflight.reason != IslandSolveJobRejectReason::None;
    return preflight;
}

bool canSkipIslandSolveJob(const IslandSolveJob& job, f32 dt) {
    return !preflightIslandSolveJobReject(job, dt).can_dispatch();
}

bool shouldRunIslandSolveJob(const IslandSolveJob& job, f32 dt) {
    return preflightIslandSolveJobReject(job, dt).can_dispatch();
}

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    default:
        return "Unknown";
    }
}

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (!preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }
    if (!preflight_island_solve_bodies(island, bodies).can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

bool islandConstraintSolveRejectsForReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints) == expected;
}

IslandConstraintSolveRejectPreflight preflightIslandConstraintSolveReject(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolveRejectPreflight preflight{};
    preflight.reason =
        islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints);
    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(island, bodies);
    preflight.skipped = preflight.reason != IslandConstraintSolveRejectReason::None;
    return preflight;
}

bool canSkipIslandConstraintSolve(const ContactIslandGraph::Island& island,
                                  const RigidBodySoA& bodies,
                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                  const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflightIslandConstraintSolveReject(island, bodies, contacts, distanceConstraints).can_solve();
}

bool shouldRunIslandConstraintSolve(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflightIslandConstraintSolveReject(island, bodies, contacts, distanceConstraints).can_solve();
}

const char* islandSleepSolveRejectReasonName(IslandSleepSolveRejectReason reason) {
    switch (reason) {
    case IslandSleepSolveRejectReason::None:
        return "None";
    case IslandSleepSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

IslandSleepSolveRejectReason islandSleepSolveRejectReason(const ContactIslandGraph::Island& island,
                                                          const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepSolveRejectReason::EmptyIsland;
    }
    if (preflight_island_sleep(island, bodies).can_skip_solve()) {
        return IslandSleepSolveRejectReason::AllSleeping;
    }
    return IslandSleepSolveRejectReason::None;
}

bool islandSleepSolveRejectsForReason(const ContactIslandGraph::Island& island,
                                      const RigidBodySoA& bodies,
                                      IslandSleepSolveRejectReason expected) {
    return islandSleepSolveRejectReason(island, bodies) == expected;
}

IslandSleepSolveRejectPreflight preflightIslandSleepSolveReject(const ContactIslandGraph::Island& island,
                                                                const RigidBodySoA& bodies) {
    IslandSleepSolveRejectPreflight preflight{};
    preflight.reason = islandSleepSolveRejectReason(island, bodies);
    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.skipped = preflight.reason != IslandSleepSolveRejectReason::None;
    return preflight;
}

bool canSkipIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflightIslandSleepSolveReject(island, bodies).can_solve();
}

bool shouldRunIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflightIslandSleepSolveReject(island, bodies).can_solve();
}

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";
    default:
        return "Unknown";
    }
}

IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,
                                              const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }
    const IslandWakePreflight wake = preflight_island_wake(island, bodies);
    if (!wake.hasMixedSleepState) {
        return IslandWakeRejectReason::NoMixedSleepState;
    }
    if (wake.activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;
    }
    return IslandWakeRejectReason::None;
}

bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies,
                                IslandWakeRejectReason expected) {
    return islandWakeRejectReason(island, bodies) == expected;
}

IslandWakeRejectPreflight preflightIslandWakeReject(const ContactIslandGraph::Island& island,
                                                    const RigidBodySoA& bodies) {
    IslandWakeRejectPreflight preflight{};
    preflight.reason = islandWakeRejectReason(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);
    preflight.skipped = preflight.reason != IslandWakeRejectReason::None;
    return preflight;
}

bool canSkipIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflightIslandWakeReject(island, bodies).can_wake();
}

bool shouldRunIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflightIslandWakeReject(island, bodies).can_wake();
}

const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty:
        return "AllIslandsSleepingOrEmpty";
    default:
        return "Unknown";
    }
}

IslandSleepGraphRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,
                                                          const RigidBodySoA& bodies) {
    if (preflight_island_sleep_graph(graph, bodies).has_solveable_islands()) {
        return IslandSleepGraphRejectReason::None;
    }
    return IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty;
}

bool islandSleepGraphRejectsForReason(const ContactIslandGraph& graph,
                                      const RigidBodySoA& bodies,
                                      IslandSleepGraphRejectReason expected) {
    return islandSleepGraphRejectReason(graph, bodies) == expected;
}

IslandSleepGraphRejectPreflight preflightIslandSleepGraphReject(const ContactIslandGraph& graph,
                                                                const RigidBodySoA& bodies) {
    IslandSleepGraphRejectPreflight preflight{};
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.reason = islandSleepGraphRejectReason(graph, bodies);
    preflight.skipped = preflight.reason != IslandSleepGraphRejectReason::None;
    return preflight;
}

bool canSkipIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflightIslandSleepGraphReject(graph, bodies).has_solveable_islands();
}

bool shouldRunIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflightIslandSleepGraphReject(graph, bodies).has_solveable_islands();
}

const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    default:
        return "Unknown";
    }
}

IslandWakeGraphRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,
                                                        const RigidBodySoA& bodies) {
    if (preflight_island_wake_graph(graph, bodies).can_wake()) {
        return IslandWakeGraphRejectReason::None;
    }
    return IslandWakeGraphRejectReason::NoWakeableIslands;
}

bool islandWakeGraphRejectsForReason(const ContactIslandGraph& graph,
                                     const RigidBodySoA& bodies,
                                     IslandWakeGraphRejectReason expected) {
    return islandWakeGraphRejectReason(graph, bodies) == expected;
}

IslandWakeGraphRejectPreflight preflightIslandWakeGraphReject(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    IslandWakeGraphRejectPreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.reason = islandWakeGraphRejectReason(graph, bodies);
    preflight.skipped = preflight.reason != IslandWakeGraphRejectReason::None;
    return preflight;
}

bool canSkipIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflightIslandWakeGraphReject(graph, bodies).can_wake();
}

bool shouldRunIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflightIslandWakeGraphReject(graph, bodies).can_wake();
}

const char* islandPipelineDispatchRejectReasonName(IslandPipelineDispatchRejectReason reason) {
    switch (reason) {
    case IslandPipelineDispatchRejectReason::None:
        return "None";
    case IslandPipelineDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandPipelineDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandPipelineDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandPipelineDispatchRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    default:
        return "Unknown";
    }
}

IslandPipelineDispatchRejectReason islandPipelineDispatchRejectReason(const ContactIslandGraph& graph,
                                                                      const RigidBodySoA& bodies,
                                                                      f32 dt) {
    const IslandDispatchRejectReason dispatchReason = islandDispatchRejectReason(graph, dt);
    if (dispatchReason == IslandDispatchRejectReason::InvalidDt) {
        return IslandPipelineDispatchRejectReason::InvalidDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NonFiniteDt) {
        return IslandPipelineDispatchRejectReason::NonFiniteDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NoDispatchableIslands) {
        return IslandPipelineDispatchRejectReason::NoDispatchableIslands;
    }
    if (islandSleepGraphRejectReason(graph, bodies) != IslandSleepGraphRejectReason::None) {
        return IslandPipelineDispatchRejectReason::AllIslandsSleeping;
    }
    return IslandPipelineDispatchRejectReason::None;
}

bool islandPipelineDispatchRejectsForReason(const ContactIslandGraph& graph,
                                            const RigidBodySoA& bodies,
                                            f32 dt,
                                            IslandPipelineDispatchRejectReason expected) {
    return islandPipelineDispatchRejectReason(graph, bodies, dt) == expected;
}

IslandPipelineDispatchPreflight preflightIslandPipelineDispatch(const ContactIslandGraph& graph,
                                                                const RigidBodySoA& bodies,
                                                                f32 dt) {
    IslandPipelineDispatchPreflight preflight{};
    preflight.wake = preflightIslandWakeGraphReject(graph, bodies);
    preflight.sleep = preflightIslandSleepGraphReject(graph, bodies);
    preflight.dispatch = preflightIslandDispatchReject(graph, dt);
    preflight.reason = islandPipelineDispatchRejectReason(graph, bodies, dt);
    preflight.skipped = preflight.reason != IslandPipelineDispatchRejectReason::None;
    return preflight;
}

bool canSkipIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt) {
    return !preflightIslandPipelineDispatch(graph, bodies, dt).can_dispatch();
}

bool shouldRunIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt) {
    return preflightIslandPipelineDispatch(graph, bodies, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandPipelineDispatchPreflight preflight = preflightIslandPipelineDispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    wake_all_island_sleepers_guarded(bodies, graph);
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandDispatchRejectReason islandDispatchRejectReason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool islandDispatchRejectsForReason(const ContactIslandGraph& graph,
                                    f32 dt,
                                    IslandDispatchRejectReason expected) {
    return islandDispatchRejectReason(graph, dt) == expected;
}

IslandDispatchRejectPreflight preflightIslandDispatchReject(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchRejectPreflight preflight{};
    preflight.reason = islandDispatchRejectReason(graph, dt);
    preflight.solve = preflight_island_solve(graph);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.skipped = preflight.reason != IslandDispatchRejectReason::None;
    return preflight;
}

bool canSkipIslandDispatch(const ContactIslandGraph& graph, f32 dt) {
    return !preflightIslandDispatchReject(graph, dt).can_dispatch();
}

bool shouldRunIslandDispatch(const ContactIslandGraph& graph, f32 dt) {
    return preflightIslandDispatchReject(graph, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandDispatchRejectPreflight preflight = preflightIslandDispatchReject(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }
    return dispatch_all_islands_result(bodies,
                                     graph,
                                     workBuffers,
                                     distanceConstraints,
                                     dt,
                                     contactCompliance,
                                     invMassFn);
}

const char* islandSolveJobRejectReasonName(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::NullIsland:
        return "NullIsland";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandSolveJobRejectReason islandSolveJobRejectReason(const IslandSolveJob& job, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::NonFiniteDt;
    }
    if (job.empty) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    if (job.island == nullptr) {
        return IslandSolveJobRejectReason::NullIsland;
    }
    if (job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    }
    return IslandSolveJobRejectReason::None;
}

bool islandSolveJobRejectsForReason(const IslandSolveJob& job,
                                    f32 dt,
                                    IslandSolveJobRejectReason expected) {
    return islandSolveJobRejectReason(job, dt) == expected;
}

IslandSolveJobRejectPreflight preflightIslandSolveJobReject(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobRejectPreflight preflight{};
    preflight.reason = islandSolveJobRejectReason(job, dt);
    preflight.invalidDt = preflight.reason == IslandSolveJobRejectReason::InvalidDt ||
                          preflight.reason == IslandSolveJobRejectReason::NonFiniteDt;
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = preflight.reason != IslandSolveJobRejectReason::None;
    return preflight;
}

bool canSkipIslandSolveJob(const IslandSolveJob& job, f32 dt) {
    return !preflightIslandSolveJobReject(job, dt).can_dispatch();
}

bool shouldRunIslandSolveJob(const IslandSolveJob& job, f32 dt) {
    return preflightIslandSolveJobReject(job, dt).can_dispatch();
}

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    default:
        return "Unknown";
    }
}

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (!preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }
    if (!preflight_island_solve_bodies(island, bodies).can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

bool islandConstraintSolveRejectsForReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints) == expected;
}

IslandConstraintSolveRejectPreflight preflightIslandConstraintSolveReject(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolveRejectPreflight preflight{};
    preflight.reason =
        islandConstraintSolveRejectReason(island, bodies, contacts, distanceConstraints);
    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(island, bodies);
    preflight.skipped = preflight.reason != IslandConstraintSolveRejectReason::None;
    return preflight;
}

bool canSkipIslandConstraintSolve(const ContactIslandGraph::Island& island,
                                  const RigidBodySoA& bodies,
                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                  const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflightIslandConstraintSolveReject(island, bodies, contacts, distanceConstraints).can_solve();
}

bool shouldRunIslandConstraintSolve(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflightIslandConstraintSolveReject(island, bodies, contacts, distanceConstraints).can_solve();
}

const char* islandSleepSolveRejectReasonName(IslandSleepSolveRejectReason reason) {
    switch (reason) {
    case IslandSleepSolveRejectReason::None:
        return "None";
    case IslandSleepSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

IslandSleepSolveRejectReason islandSleepSolveRejectReason(const ContactIslandGraph::Island& island,
                                                          const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepSolveRejectReason::EmptyIsland;
    }
    if (preflight_island_sleep(island, bodies).can_skip_solve()) {
        return IslandSleepSolveRejectReason::AllSleeping;
    }
    return IslandSleepSolveRejectReason::None;
}

bool islandSleepSolveRejectsForReason(const ContactIslandGraph::Island& island,
                                      const RigidBodySoA& bodies,
                                      IslandSleepSolveRejectReason expected) {
    return islandSleepSolveRejectReason(island, bodies) == expected;
}

IslandSleepSolveRejectPreflight preflightIslandSleepSolveReject(const ContactIslandGraph::Island& island,
                                                                const RigidBodySoA& bodies) {
    IslandSleepSolveRejectPreflight preflight{};
    preflight.reason = islandSleepSolveRejectReason(island, bodies);
    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.skipped = preflight.reason != IslandSleepSolveRejectReason::None;
    return preflight;
}

bool canSkipIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflightIslandSleepSolveReject(island, bodies).can_solve();
}

bool shouldRunIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflightIslandSleepSolveReject(island, bodies).can_solve();
}

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";
    default:
        return "Unknown";
    }
}

IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,
                                              const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }
    const IslandWakePreflight wake = preflight_island_wake(island, bodies);
    if (!wake.hasMixedSleepState) {
        return IslandWakeRejectReason::NoMixedSleepState;
    }
    if (wake.activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;
    }
    return IslandWakeRejectReason::None;
}

bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies,
                                IslandWakeRejectReason expected) {
    return islandWakeRejectReason(island, bodies) == expected;
}

IslandWakeRejectPreflight preflightIslandWakeReject(const ContactIslandGraph::Island& island,
                                                    const RigidBodySoA& bodies) {
    IslandWakeRejectPreflight preflight{};
    preflight.reason = islandWakeRejectReason(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);
    preflight.skipped = preflight.reason != IslandWakeRejectReason::None;
    return preflight;
}

bool canSkipIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflightIslandWakeReject(island, bodies).can_wake();
}

bool shouldRunIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflightIslandWakeReject(island, bodies).can_wake();
}

const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty:
        return "AllIslandsSleepingOrEmpty";
    default:
        return "Unknown";
    }
}

IslandSleepGraphRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,
                                                          const RigidBodySoA& bodies) {
    if (preflight_island_sleep_graph(graph, bodies).has_solveable_islands()) {
        return IslandSleepGraphRejectReason::None;
    }
    return IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty;
}

bool islandSleepGraphRejectsForReason(const ContactIslandGraph& graph,
                                      const RigidBodySoA& bodies,
                                      IslandSleepGraphRejectReason expected) {
    return islandSleepGraphRejectReason(graph, bodies) == expected;
}

IslandSleepGraphRejectPreflight preflightIslandSleepGraphReject(const ContactIslandGraph& graph,
                                                                const RigidBodySoA& bodies) {
    IslandSleepGraphRejectPreflight preflight{};
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.reason = islandSleepGraphRejectReason(graph, bodies);
    preflight.skipped = preflight.reason != IslandSleepGraphRejectReason::None;
    return preflight;
}

bool canSkipIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflightIslandSleepGraphReject(graph, bodies).has_solveable_islands();
}

bool shouldRunIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflightIslandSleepGraphReject(graph, bodies).has_solveable_islands();
}

const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    default:
        return "Unknown";
    }
}

IslandWakeGraphRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,
                                                        const RigidBodySoA& bodies) {
    if (preflight_island_wake_graph(graph, bodies).can_wake()) {
        return IslandWakeGraphRejectReason::None;
    }
    return IslandWakeGraphRejectReason::NoWakeableIslands;
}

bool islandWakeGraphRejectsForReason(const ContactIslandGraph& graph,
                                     const RigidBodySoA& bodies,
                                     IslandWakeGraphRejectReason expected) {
    return islandWakeGraphRejectReason(graph, bodies) == expected;
}

IslandWakeGraphRejectPreflight preflightIslandWakeGraphReject(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    IslandWakeGraphRejectPreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.reason = islandWakeGraphRejectReason(graph, bodies);
    preflight.skipped = preflight.reason != IslandWakeGraphRejectReason::None;
    return preflight;
}

bool canSkipIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflightIslandWakeGraphReject(graph, bodies).can_wake();
}

bool shouldRunIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflightIslandWakeGraphReject(graph, bodies).can_wake();
}

const char* islandPipelineDispatchRejectReasonName(IslandPipelineDispatchRejectReason reason) {
    switch (reason) {
    case IslandPipelineDispatchRejectReason::None:
        return "None";
    case IslandPipelineDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandPipelineDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandPipelineDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandPipelineDispatchRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    default:
        return "Unknown";
    }
}

IslandPipelineDispatchRejectReason islandPipelineDispatchRejectReason(const ContactIslandGraph& graph,
                                                                      const RigidBodySoA& bodies,
                                                                      f32 dt) {
    const IslandDispatchRejectReason dispatchReason = islandDispatchRejectReason(graph, dt);
    if (dispatchReason == IslandDispatchRejectReason::InvalidDt) {
        return IslandPipelineDispatchRejectReason::InvalidDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NonFiniteDt) {
        return IslandPipelineDispatchRejectReason::NonFiniteDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NoDispatchableIslands) {
        return IslandPipelineDispatchRejectReason::NoDispatchableIslands;
    }
    if (islandSleepGraphRejectReason(graph, bodies) != IslandSleepGraphRejectReason::None) {
        return IslandPipelineDispatchRejectReason::AllIslandsSleeping;
    }
    return IslandPipelineDispatchRejectReason::None;
}

bool islandPipelineDispatchRejectsForReason(const ContactIslandGraph& graph,
                                            const RigidBodySoA& bodies,
                                            f32 dt,
                                            IslandPipelineDispatchRejectReason expected) {
    return islandPipelineDispatchRejectReason(graph, bodies, dt) == expected;
}

IslandPipelineDispatchPreflight preflightIslandPipelineDispatch(const ContactIslandGraph& graph,
                                                                const RigidBodySoA& bodies,
                                                                f32 dt) {
    IslandPipelineDispatchPreflight preflight{};
    preflight.wake = preflightIslandWakeGraphReject(graph, bodies);
    preflight.sleep = preflightIslandSleepGraphReject(graph, bodies);
    preflight.dispatch = preflightIslandDispatchReject(graph, dt);
    preflight.reason = islandPipelineDispatchRejectReason(graph, bodies, dt);
    preflight.skipped = preflight.reason != IslandPipelineDispatchRejectReason::None;
    return preflight;
}

bool canSkipIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt) {
    return !preflightIslandPipelineDispatch(graph, bodies, dt).can_dispatch();
}

bool shouldRunIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt) {
    return preflightIslandPipelineDispatch(graph, bodies, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandPipelineDispatchPreflight preflight = preflightIslandPipelineDispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    wake_all_island_sleepers_guarded(bodies, graph);
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

IslandDispatchRejectPreflight preflight_island_dispatch_reject(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchRejectPreflight preflight{};
    preflight.reason = island_dispatch_reject_reason(graph, dt);
    preflight.solve = preflight_island_solve(graph);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.skipped = preflight.reason != IslandDispatchRejectReason::None;
    return preflight;
}

bool can_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return !preflight_island_dispatch_reject(graph, dt).can_dispatch();
}

bool should_run_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return preflight_island_dispatch_reject(graph, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandDispatchRejectPreflight preflight = preflight_island_dispatch_reject(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::NullIsland:
        return "NullIsland";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::NonFiniteDt;
    }
    if (job.empty) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    if (job.island == nullptr) {
        return IslandSolveJobRejectReason::NullIsland;
    }
    if (job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    }
    return IslandSolveJobRejectReason::None;
}

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                         f32 dt,
                                         IslandSolveJobRejectReason expected) {
    return island_solve_job_reject_reason(job, dt) == expected;
}

IslandSolveJobRejectPreflight preflight_island_solve_job_reject(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobRejectPreflight preflight{};
    preflight.reason = island_solve_job_reject_reason(job, dt);
    preflight.invalidDt = preflight.reason == IslandSolveJobRejectReason::InvalidDt ||
                          preflight.reason == IslandSolveJobRejectReason::NonFiniteDt;
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = preflight.reason != IslandSolveJobRejectReason::None;
    return preflight;
}

bool can_skip_island_solve_job(const IslandSolveJob& job, f32 dt) {
    return !preflight_island_solve_job_reject(job, dt).can_dispatch();
}

bool should_run_island_solve_job(const IslandSolveJob& job, f32 dt) {
    return preflight_island_solve_job_reject(job, dt).can_dispatch();
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    default:
        return "Unknown";
    }
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (!preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }
    if (!preflight_island_solve_bodies(island, bodies).can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

IslandConstraintSolveRejectPreflight preflight_island_constraint_solve_reject(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolveRejectPreflight preflight{};
    preflight.reason = island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(island, bodies);
    preflight.skipped = preflight.reason != IslandConstraintSolveRejectReason::None;
    return preflight;
}

bool can_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                      const RigidBodySoA& bodies,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();
}

bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();
}

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason) {
    switch (reason) {
    case IslandSleepSolveRejectReason::None:
        return "None";
    case IslandSleepSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepSolveRejectReason::EmptyIsland;
    }
    if (preflight_island_sleep(island, bodies).can_skip_solve()) {
        return IslandSleepSolveRejectReason::AllSleeping;
    }
    return IslandSleepSolveRejectReason::None;
}

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected) {
    return island_sleep_solve_reject_reason(island, bodies) == expected;
}

IslandSleepSolveRejectPreflight preflight_island_sleep_solve_reject(const ContactIslandGraph::Island& island,
                                                                    const RigidBodySoA& bodies) {
    IslandSleepSolveRejectPreflight preflight{};
    preflight.reason = island_sleep_solve_reject_reason(island, bodies);
    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.skipped = preflight.reason != IslandSleepSolveRejectReason::None;
    return preflight;
}

bool can_skip_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_solve_reject(island, bodies).can_solve();
}

bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep_solve_reject(island, bodies).can_solve();
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";
    default:
        return "Unknown";
    }
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }
    const IslandWakePreflight wake = preflight_island_wake(island, bodies);
    if (!wake.hasMixedSleepState) {
        return IslandWakeRejectReason::NoMixedSleepState;
    }
    if (wake.activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;
    }
    return IslandWakeRejectReason::None;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

IslandWakeRejectPreflight preflight_island_wake_reject(const ContactIslandGraph::Island& island,
                                                       const RigidBodySoA& bodies) {
    IslandWakeRejectPreflight preflight{};
    preflight.reason = island_wake_reject_reason(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);
    preflight.skipped = preflight.reason != IslandWakeRejectReason::None;
    return preflight;
}

bool can_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_wake_reject(island, bodies).can_wake();
}

bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_wake_reject(island, bodies).can_wake();
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty:
        return "AllIslandsSleepingOrEmpty";
    default:
        return "Unknown";
    }
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    if (preflight_island_sleep_graph(graph, bodies).has_solveable_islands()) {
        return IslandSleepGraphRejectReason::None;
    }
    return IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

IslandSleepGraphRejectPreflight preflight_island_sleep_graph_reject(const ContactIslandGraph& graph,
                                                                    const RigidBodySoA& bodies) {
    IslandSleepGraphRejectPreflight preflight{};
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.reason = island_sleep_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandSleepGraphRejectReason::None;
    return preflight;
}

bool can_skip_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();
}

bool should_run_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    default:
        return "Unknown";
    }
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    if (preflight_island_wake_graph(graph, bodies).can_wake()) {
        return IslandWakeGraphRejectReason::None;
    }
    return IslandWakeGraphRejectReason::NoWakeableIslands;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

IslandWakeGraphRejectPreflight preflight_island_wake_graph_reject(const ContactIslandGraph& graph,
                                                                  const RigidBodySoA& bodies) {
    IslandWakeGraphRejectPreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.reason = island_wake_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandWakeGraphRejectReason::None;
    return preflight;
}

bool can_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_wake_graph_reject(graph, bodies).can_wake();
}

bool should_run_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_wake_graph_reject(graph, bodies).can_wake();
}

const char* island_pipeline_dispatch_reject_reason_name(IslandPipelineDispatchRejectReason reason) {
    switch (reason) {
    case IslandPipelineDispatchRejectReason::None:
        return "None";
    case IslandPipelineDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandPipelineDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandPipelineDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandPipelineDispatchRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    default:
        return "Unknown";
    }
}

IslandPipelineDispatchRejectReason island_pipeline_dispatch_reject_reason(const ContactIslandGraph& graph,
                                                                          const RigidBodySoA& bodies,
                                                                          f32 dt) {
    const IslandDispatchRejectReason dispatchReason = island_dispatch_reject_reason(graph, dt);
    if (dispatchReason == IslandDispatchRejectReason::InvalidDt) {
        return IslandPipelineDispatchRejectReason::InvalidDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NonFiniteDt) {
        return IslandPipelineDispatchRejectReason::NonFiniteDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NoDispatchableIslands) {
        return IslandPipelineDispatchRejectReason::NoDispatchableIslands;
    }
    if (island_sleep_graph_reject_reason(graph, bodies) != IslandSleepGraphRejectReason::None) {
        return IslandPipelineDispatchRejectReason::AllIslandsSleeping;
    }
    return IslandPipelineDispatchRejectReason::None;
}

bool island_pipeline_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies,
                                                 f32 dt,
                                                 IslandPipelineDispatchRejectReason expected) {
    return island_pipeline_dispatch_reject_reason(graph, bodies, dt) == expected;
}

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                                                   const RigidBodySoA& bodies,
                                                                   f32 dt) {
    IslandPipelineDispatchPreflight preflight{};
    preflight.wake = preflight_island_wake_graph_reject(graph, bodies);
    preflight.sleep = preflight_island_sleep_graph_reject(graph, bodies);
    preflight.dispatch = preflight_island_dispatch_reject(graph, dt);
    preflight.reason = island_pipeline_dispatch_reject_reason(graph, bodies, dt);
    preflight.skipped = preflight.reason != IslandPipelineDispatchRejectReason::None;
    return preflight;
}

bool can_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt) {
    return !preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

bool should_run_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                         const RigidBodySoA& bodies,
                                         f32 dt) {
    return preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandPipelineDispatchPreflight preflight = preflight_island_pipeline_dispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    wake_all_island_sleepers_guarded(bodies, graph);
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

IslandDispatchRejectPreflight preflight_island_dispatch_reject(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchRejectPreflight preflight{};
    preflight.reason = island_dispatch_reject_reason(graph, dt);
    preflight.solve = preflight_island_solve(graph);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.skipped = preflight.reason != IslandDispatchRejectReason::None;
    return preflight;
}

bool can_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return !preflight_island_dispatch_reject(graph, dt).can_dispatch();
}

bool should_run_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return preflight_island_dispatch_reject(graph, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandDispatchRejectPreflight preflight = preflight_island_dispatch_reject(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::NullIsland:
        return "NullIsland";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::NonFiniteDt;
    }
    if (job.empty) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    if (job.island == nullptr) {
        return IslandSolveJobRejectReason::NullIsland;
    }
    if (job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    }
    return IslandSolveJobRejectReason::None;
}

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                         f32 dt,
                                         IslandSolveJobRejectReason expected) {
    return island_solve_job_reject_reason(job, dt) == expected;
}

IslandSolveJobRejectPreflight preflight_island_solve_job_reject(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobRejectPreflight preflight{};
    preflight.reason = island_solve_job_reject_reason(job, dt);
    preflight.invalidDt = preflight.reason == IslandSolveJobRejectReason::InvalidDt ||
                          preflight.reason == IslandSolveJobRejectReason::NonFiniteDt;
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = preflight.reason != IslandSolveJobRejectReason::None;
    return preflight;
}

bool can_skip_island_solve_job(const IslandSolveJob& job, f32 dt) {
    return !preflight_island_solve_job_reject(job, dt).can_dispatch();
}

bool should_run_island_solve_job(const IslandSolveJob& job, f32 dt) {
    return preflight_island_solve_job_reject(job, dt).can_dispatch();
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    default:
        return "Unknown";
    }
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (!preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }
    if (!preflight_island_solve_bodies(island, bodies).can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

IslandConstraintSolveRejectPreflight preflight_island_constraint_solve_reject(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolveRejectPreflight preflight{};
    preflight.reason = island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(island, bodies);
    preflight.skipped = preflight.reason != IslandConstraintSolveRejectReason::None;
    return preflight;
}

bool can_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                      const RigidBodySoA& bodies,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();
}

bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();
}

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason) {
    switch (reason) {
    case IslandSleepSolveRejectReason::None:
        return "None";
    case IslandSleepSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepSolveRejectReason::EmptyIsland;
    }
    if (preflight_island_sleep(island, bodies).can_skip_solve()) {
        return IslandSleepSolveRejectReason::AllSleeping;
    }
    return IslandSleepSolveRejectReason::None;
}

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected) {
    return island_sleep_solve_reject_reason(island, bodies) == expected;
}

IslandSleepSolveRejectPreflight preflight_island_sleep_solve_reject(const ContactIslandGraph::Island& island,
                                                                    const RigidBodySoA& bodies) {
    IslandSleepSolveRejectPreflight preflight{};
    preflight.reason = island_sleep_solve_reject_reason(island, bodies);
    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.skipped = preflight.reason != IslandSleepSolveRejectReason::None;
    return preflight;
}

bool can_skip_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_solve_reject(island, bodies).can_solve();
}

bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep_solve_reject(island, bodies).can_solve();
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";
    default:
        return "Unknown";
    }
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }
    const IslandWakePreflight wake = preflight_island_wake(island, bodies);
    if (!wake.hasMixedSleepState) {
        return IslandWakeRejectReason::NoMixedSleepState;
    }
    if (wake.activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;
    }
    return IslandWakeRejectReason::None;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

IslandWakeRejectPreflight preflight_island_wake_reject(const ContactIslandGraph::Island& island,
                                                       const RigidBodySoA& bodies) {
    IslandWakeRejectPreflight preflight{};
    preflight.reason = island_wake_reject_reason(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);
    preflight.skipped = preflight.reason != IslandWakeRejectReason::None;
    return preflight;
}

bool can_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_wake_reject(island, bodies).can_wake();
}

bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_wake_reject(island, bodies).can_wake();
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty:
        return "AllIslandsSleepingOrEmpty";
    default:
        return "Unknown";
    }
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    if (preflight_island_sleep_graph(graph, bodies).has_solveable_islands()) {
        return IslandSleepGraphRejectReason::None;
    }
    return IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

IslandSleepGraphRejectPreflight preflight_island_sleep_graph_reject(const ContactIslandGraph& graph,
                                                                    const RigidBodySoA& bodies) {
    IslandSleepGraphRejectPreflight preflight{};
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.reason = island_sleep_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandSleepGraphRejectReason::None;
    return preflight;
}

bool can_skip_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();
}

bool should_run_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    default:
        return "Unknown";
    }
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    if (preflight_island_wake_graph(graph, bodies).can_wake()) {
        return IslandWakeGraphRejectReason::None;
    }
    return IslandWakeGraphRejectReason::NoWakeableIslands;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

IslandWakeGraphRejectPreflight preflight_island_wake_graph_reject(const ContactIslandGraph& graph,
                                                                  const RigidBodySoA& bodies) {
    IslandWakeGraphRejectPreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.reason = island_wake_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandWakeGraphRejectReason::None;
    return preflight;
}

bool can_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_wake_graph_reject(graph, bodies).can_wake();
}

bool should_run_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_wake_graph_reject(graph, bodies).can_wake();
}

const char* island_pipeline_dispatch_reject_reason_name(IslandPipelineDispatchRejectReason reason) {
    switch (reason) {
    case IslandPipelineDispatchRejectReason::None:
        return "None";
    case IslandPipelineDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandPipelineDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandPipelineDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandPipelineDispatchRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    default:
        return "Unknown";
    }
}

IslandPipelineDispatchRejectReason island_pipeline_dispatch_reject_reason(const ContactIslandGraph& graph,
                                                                          const RigidBodySoA& bodies,
                                                                          f32 dt) {
    const IslandDispatchRejectReason dispatchReason = island_dispatch_reject_reason(graph, dt);
    if (dispatchReason == IslandDispatchRejectReason::InvalidDt) {
        return IslandPipelineDispatchRejectReason::InvalidDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NonFiniteDt) {
        return IslandPipelineDispatchRejectReason::NonFiniteDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NoDispatchableIslands) {
        return IslandPipelineDispatchRejectReason::NoDispatchableIslands;
    }
    if (island_sleep_graph_reject_reason(graph, bodies) != IslandSleepGraphRejectReason::None) {
        return IslandPipelineDispatchRejectReason::AllIslandsSleeping;
    }
    return IslandPipelineDispatchRejectReason::None;
}

bool island_pipeline_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies,
                                                 f32 dt,
                                                 IslandPipelineDispatchRejectReason expected) {
    return island_pipeline_dispatch_reject_reason(graph, bodies, dt) == expected;
}

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                                                   const RigidBodySoA& bodies,
                                                                   f32 dt) {
    IslandPipelineDispatchPreflight preflight{};
    preflight.wake = preflight_island_wake_graph_reject(graph, bodies);
    preflight.sleep = preflight_island_sleep_graph_reject(graph, bodies);
    preflight.dispatch = preflight_island_dispatch_reject(graph, dt);
    preflight.reason = island_pipeline_dispatch_reject_reason(graph, bodies, dt);
    preflight.skipped = preflight.reason != IslandPipelineDispatchRejectReason::None;
    return preflight;
}

bool can_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt) {
    return !preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

bool should_run_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                         const RigidBodySoA& bodies,
                                         f32 dt) {
    return preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandPipelineDispatchPreflight preflight = preflight_island_pipeline_dispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    wake_all_island_sleepers_guarded(bodies, graph);
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

IslandSolvePipelinePreflight preflight_island_solve_pipeline(const ContactIslandGraph& graph,
                                                               const RigidBodySoA& bodies,
                                                               f32 dt) {
    IslandSolvePipelinePreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.reason = preflight.dispatch.reason;
    preflight.skipped = !preflight.dispatch.can_dispatch();
    return preflight;
}

bool should_skip_island_solve_pipeline(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt) {
    return !preflight_island_solve_pipeline(graph, bodies, dt).can_run();
}

u32 dispatch_island_solve_pipeline_guarded(RigidBodySoA& bodies,
                                           const ContactIslandGraph& graph,
                                           SolverWorkBuffers& workBuffers,
                                           const std::vector<DistanceConstraint>& distanceConstraints,
                                           f32 dt,
                                           f32 contactCompliance,
                                           const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_island_solve_pipeline_result(bodies,
                                                 graph,
                                                 workBuffers,
                                                 distanceConstraints,
                                                 dt,
                                                 contactCompliance,
                                                 invMassFn)
        .dispatch.solvedCount;
}

IslandSolvePipelineResult dispatch_island_solve_pipeline_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandSolvePipelineResult result{};
    const IslandSolvePipelinePreflight preflight = preflight_island_solve_pipeline(graph, bodies, dt);
    if (!preflight.can_run()) {
        result.skipped = true;
        return result;
    }

    result.wokeCount = wake_all_island_sleepers_guarded(bodies, graph);
    result.dispatch = dispatch_all_islands_result(bodies,
                                                  graph,
                                                  workBuffers,
                                                  distanceConstraints,
                                                  dt,
                                                  contactCompliance,
                                                  invMassFn);
    result.skipped = result.dispatch.skipped;
    return result;
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

IslandDispatchRejectPreflight preflight_island_dispatch_reject(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchRejectPreflight preflight{};
    preflight.reason = island_dispatch_reject_reason(graph, dt);
    preflight.solve = preflight_island_solve(graph);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.skipped = preflight.reason != IslandDispatchRejectReason::None;
    return preflight;
}

bool can_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return !preflight_island_dispatch_reject(graph, dt).can_dispatch();
}

bool should_run_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return preflight_island_dispatch_reject(graph, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandDispatchRejectPreflight preflight = preflight_island_dispatch_reject(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::NullIsland:
        return "NullIsland";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::NonFiniteDt;
    }
    if (job.empty) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    if (job.island == nullptr) {
        return IslandSolveJobRejectReason::NullIsland;
    }
    if (job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    }
    return IslandSolveJobRejectReason::None;
}

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                         f32 dt,
                                         IslandSolveJobRejectReason expected) {
    return island_solve_job_reject_reason(job, dt) == expected;
}

IslandSolveJobRejectPreflight preflight_island_solve_job_reject(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobRejectPreflight preflight{};
    preflight.reason = island_solve_job_reject_reason(job, dt);
    preflight.invalidDt = preflight.reason == IslandSolveJobRejectReason::InvalidDt ||
                          preflight.reason == IslandSolveJobRejectReason::NonFiniteDt;
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = preflight.reason != IslandSolveJobRejectReason::None;
    return preflight;
}

bool can_skip_island_solve_job(const IslandSolveJob& job, f32 dt) {
    return !preflight_island_solve_job_reject(job, dt).can_dispatch();
}

bool should_run_island_solve_job(const IslandSolveJob& job, f32 dt) {
    return preflight_island_solve_job_reject(job, dt).can_dispatch();
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    default:
        return "Unknown";
    }
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (!preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }
    if (!preflight_island_solve_bodies(island, bodies).can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

IslandConstraintSolveRejectPreflight preflight_island_constraint_solve_reject(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolveRejectPreflight preflight{};
    preflight.reason = island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(island, bodies);
    preflight.skipped = preflight.reason != IslandConstraintSolveRejectReason::None;
    return preflight;
}

bool can_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                      const RigidBodySoA& bodies,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();
}

bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();
}

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason) {
    switch (reason) {
    case IslandSleepSolveRejectReason::None:
        return "None";
    case IslandSleepSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepSolveRejectReason::EmptyIsland;
    }
    if (preflight_island_sleep(island, bodies).can_skip_solve()) {
        return IslandSleepSolveRejectReason::AllSleeping;
    }
    return IslandSleepSolveRejectReason::None;
}

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected) {
    return island_sleep_solve_reject_reason(island, bodies) == expected;
}

IslandSleepSolveRejectPreflight preflight_island_sleep_solve_reject(const ContactIslandGraph::Island& island,
                                                                    const RigidBodySoA& bodies) {
    IslandSleepSolveRejectPreflight preflight{};
    preflight.reason = island_sleep_solve_reject_reason(island, bodies);
    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.skipped = preflight.reason != IslandSleepSolveRejectReason::None;
    return preflight;
}

bool can_skip_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_solve_reject(island, bodies).can_solve();
}

bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep_solve_reject(island, bodies).can_solve();
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";
    default:
        return "Unknown";
    }
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }
    const IslandWakePreflight wake = preflight_island_wake(island, bodies);
    if (!wake.hasMixedSleepState) {
        return IslandWakeRejectReason::NoMixedSleepState;
    }
    if (wake.activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;
    }
    return IslandWakeRejectReason::None;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

IslandWakeRejectPreflight preflight_island_wake_reject(const ContactIslandGraph::Island& island,
                                                       const RigidBodySoA& bodies) {
    IslandWakeRejectPreflight preflight{};
    preflight.reason = island_wake_reject_reason(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);
    preflight.skipped = preflight.reason != IslandWakeRejectReason::None;
    return preflight;
}

bool can_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_wake_reject(island, bodies).can_wake();
}

bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_wake_reject(island, bodies).can_wake();
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty:
        return "AllIslandsSleepingOrEmpty";
    default:
        return "Unknown";
    }
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    if (preflight_island_sleep_graph(graph, bodies).has_solveable_islands()) {
        return IslandSleepGraphRejectReason::None;
    }
    return IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

IslandSleepGraphRejectPreflight preflight_island_sleep_graph_reject(const ContactIslandGraph& graph,
                                                                    const RigidBodySoA& bodies) {
    IslandSleepGraphRejectPreflight preflight{};
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.reason = island_sleep_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandSleepGraphRejectReason::None;
    return preflight;
}

bool can_skip_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();
}

bool should_run_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    default:
        return "Unknown";
    }
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    if (preflight_island_wake_graph(graph, bodies).can_wake()) {
        return IslandWakeGraphRejectReason::None;
    }
    return IslandWakeGraphRejectReason::NoWakeableIslands;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

IslandWakeGraphRejectPreflight preflight_island_wake_graph_reject(const ContactIslandGraph& graph,
                                                                  const RigidBodySoA& bodies) {
    IslandWakeGraphRejectPreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.reason = island_wake_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandWakeGraphRejectReason::None;
    return preflight;
}

bool can_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_wake_graph_reject(graph, bodies).can_wake();
}

bool should_run_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_wake_graph_reject(graph, bodies).can_wake();
}

const char* island_pipeline_dispatch_reject_reason_name(IslandPipelineDispatchRejectReason reason) {
    switch (reason) {
    case IslandPipelineDispatchRejectReason::None:
        return "None";
    case IslandPipelineDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandPipelineDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandPipelineDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandPipelineDispatchRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    default:
        return "Unknown";
    }
}

IslandPipelineDispatchRejectReason island_pipeline_dispatch_reject_reason(const ContactIslandGraph& graph,
                                                                          const RigidBodySoA& bodies,
                                                                          f32 dt) {
    const IslandDispatchRejectReason dispatchReason = island_dispatch_reject_reason(graph, dt);
    if (dispatchReason == IslandDispatchRejectReason::InvalidDt) {
        return IslandPipelineDispatchRejectReason::InvalidDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NonFiniteDt) {
        return IslandPipelineDispatchRejectReason::NonFiniteDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NoDispatchableIslands) {
        return IslandPipelineDispatchRejectReason::NoDispatchableIslands;
    }
    if (island_sleep_graph_reject_reason(graph, bodies) != IslandSleepGraphRejectReason::None) {
        return IslandPipelineDispatchRejectReason::AllIslandsSleeping;
    }
    return IslandPipelineDispatchRejectReason::None;
}

bool island_pipeline_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies,
                                                 f32 dt,
                                                 IslandPipelineDispatchRejectReason expected) {
    return island_pipeline_dispatch_reject_reason(graph, bodies, dt) == expected;
}

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                                                   const RigidBodySoA& bodies,
                                                                   f32 dt) {
    IslandPipelineDispatchPreflight preflight{};
    preflight.wake = preflight_island_wake_graph_reject(graph, bodies);
    preflight.sleep = preflight_island_sleep_graph_reject(graph, bodies);
    preflight.dispatch = preflight_island_dispatch_reject(graph, dt);
    preflight.reason = island_pipeline_dispatch_reject_reason(graph, bodies, dt);
    preflight.skipped = preflight.reason != IslandPipelineDispatchRejectReason::None;
    return preflight;
}

bool can_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt) {
    return !preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

bool should_run_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                         const RigidBodySoA& bodies,
                                         f32 dt) {
    return preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandPipelineDispatchPreflight preflight = preflight_island_pipeline_dispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    wake_all_island_sleepers_guarded(bodies, graph);
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

IslandDispatchRejectPreflight preflight_island_dispatch_reject(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchRejectPreflight preflight{};
    preflight.reason = island_dispatch_reject_reason(graph, dt);
    preflight.solve = preflight_island_solve(graph);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.skipped = preflight.reason != IslandDispatchRejectReason::None;
    return preflight;
}

bool can_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return !preflight_island_dispatch_reject(graph, dt).can_dispatch();
}

bool should_run_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return preflight_island_dispatch_reject(graph, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandDispatchRejectPreflight preflight = preflight_island_dispatch_reject(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::NullIsland:
        return "NullIsland";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::NonFiniteDt;
    }
    if (job.empty) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    if (job.island == nullptr) {
        return IslandSolveJobRejectReason::NullIsland;
    }
    if (job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    }
    return IslandSolveJobRejectReason::None;
}

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                         f32 dt,
                                         IslandSolveJobRejectReason expected) {
    return island_solve_job_reject_reason(job, dt) == expected;
}

IslandSolveJobRejectPreflight preflight_island_solve_job_reject(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobRejectPreflight preflight{};
    preflight.reason = island_solve_job_reject_reason(job, dt);
    preflight.invalidDt = preflight.reason == IslandSolveJobRejectReason::InvalidDt ||
                          preflight.reason == IslandSolveJobRejectReason::NonFiniteDt;
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = preflight.reason != IslandSolveJobRejectReason::None;
    return preflight;
}

bool can_skip_island_solve_job(const IslandSolveJob& job, f32 dt) {
    return !preflight_island_solve_job_reject(job, dt).can_dispatch();
}

bool should_run_island_solve_job(const IslandSolveJob& job, f32 dt) {
    return preflight_island_solve_job_reject(job, dt).can_dispatch();
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    default:
        return "Unknown";
    }
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (!preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }
    if (!preflight_island_solve_bodies(island, bodies).can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

IslandConstraintSolveRejectPreflight preflight_island_constraint_solve_reject(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolveRejectPreflight preflight{};
    preflight.reason = island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(island, bodies);
    preflight.skipped = preflight.reason != IslandConstraintSolveRejectReason::None;
    return preflight;
}

bool can_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                      const RigidBodySoA& bodies,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();
}

bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();
}

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason) {
    switch (reason) {
    case IslandSleepSolveRejectReason::None:
        return "None";
    case IslandSleepSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepSolveRejectReason::EmptyIsland;
    }
    if (preflight_island_sleep(island, bodies).can_skip_solve()) {
        return IslandSleepSolveRejectReason::AllSleeping;
    }
    return IslandSleepSolveRejectReason::None;
}

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected) {
    return island_sleep_solve_reject_reason(island, bodies) == expected;
}

IslandSleepSolveRejectPreflight preflight_island_sleep_solve_reject(const ContactIslandGraph::Island& island,
                                                                    const RigidBodySoA& bodies) {
    IslandSleepSolveRejectPreflight preflight{};
    preflight.reason = island_sleep_solve_reject_reason(island, bodies);
    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.skipped = preflight.reason != IslandSleepSolveRejectReason::None;
    return preflight;
}

bool can_skip_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_solve_reject(island, bodies).can_solve();
}

bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep_solve_reject(island, bodies).can_solve();
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";
    default:
        return "Unknown";
    }
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }
    const IslandWakePreflight wake = preflight_island_wake(island, bodies);
    if (!wake.hasMixedSleepState) {
        return IslandWakeRejectReason::NoMixedSleepState;
    }
    if (wake.activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;
    }
    return IslandWakeRejectReason::None;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

IslandWakeRejectPreflight preflight_island_wake_reject(const ContactIslandGraph::Island& island,
                                                       const RigidBodySoA& bodies) {
    IslandWakeRejectPreflight preflight{};
    preflight.reason = island_wake_reject_reason(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);
    preflight.skipped = preflight.reason != IslandWakeRejectReason::None;
    return preflight;
}

bool can_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_wake_reject(island, bodies).can_wake();
}

bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_wake_reject(island, bodies).can_wake();
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty:
        return "AllIslandsSleepingOrEmpty";
    default:
        return "Unknown";
    }
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    if (preflight_island_sleep_graph(graph, bodies).has_solveable_islands()) {
        return IslandSleepGraphRejectReason::None;
    }
    return IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

IslandSleepGraphRejectPreflight preflight_island_sleep_graph_reject(const ContactIslandGraph& graph,
                                                                    const RigidBodySoA& bodies) {
    IslandSleepGraphRejectPreflight preflight{};
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.reason = island_sleep_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandSleepGraphRejectReason::None;
    return preflight;
}

bool can_skip_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();
}

bool should_run_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    default:
        return "Unknown";
    }
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    if (preflight_island_wake_graph(graph, bodies).can_wake()) {
        return IslandWakeGraphRejectReason::None;
    }
    return IslandWakeGraphRejectReason::NoWakeableIslands;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

IslandWakeGraphRejectPreflight preflight_island_wake_graph_reject(const ContactIslandGraph& graph,
                                                                  const RigidBodySoA& bodies) {
    IslandWakeGraphRejectPreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.reason = island_wake_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandWakeGraphRejectReason::None;
    return preflight;
}

bool can_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_wake_graph_reject(graph, bodies).can_wake();
}

bool should_run_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_wake_graph_reject(graph, bodies).can_wake();
}

const char* island_pipeline_dispatch_reject_reason_name(IslandPipelineDispatchRejectReason reason) {
    switch (reason) {
    case IslandPipelineDispatchRejectReason::None:
        return "None";
    case IslandPipelineDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandPipelineDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandPipelineDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandPipelineDispatchRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    default:
        return "Unknown";
    }
}

IslandPipelineDispatchRejectReason island_pipeline_dispatch_reject_reason(const ContactIslandGraph& graph,
                                                                          const RigidBodySoA& bodies,
                                                                          f32 dt) {
    const IslandDispatchRejectReason dispatchReason = island_dispatch_reject_reason(graph, dt);
    if (dispatchReason == IslandDispatchRejectReason::InvalidDt) {
        return IslandPipelineDispatchRejectReason::InvalidDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NonFiniteDt) {
        return IslandPipelineDispatchRejectReason::NonFiniteDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NoDispatchableIslands) {
        return IslandPipelineDispatchRejectReason::NoDispatchableIslands;
    }
    if (island_sleep_graph_reject_reason(graph, bodies) != IslandSleepGraphRejectReason::None) {
        return IslandPipelineDispatchRejectReason::AllIslandsSleeping;
    }
    return IslandPipelineDispatchRejectReason::None;
}

bool island_pipeline_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies,
                                                 f32 dt,
                                                 IslandPipelineDispatchRejectReason expected) {
    return island_pipeline_dispatch_reject_reason(graph, bodies, dt) == expected;
}

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                                                   const RigidBodySoA& bodies,
                                                                   f32 dt) {
    IslandPipelineDispatchPreflight preflight{};
    preflight.wake = preflight_island_wake_graph_reject(graph, bodies);
    preflight.sleep = preflight_island_sleep_graph_reject(graph, bodies);
    preflight.dispatch = preflight_island_dispatch_reject(graph, dt);
    preflight.reason = island_pipeline_dispatch_reject_reason(graph, bodies, dt);
    preflight.skipped = preflight.reason != IslandPipelineDispatchRejectReason::None;
    return preflight;
}

bool can_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt) {
    return !preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

bool should_run_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                         const RigidBodySoA& bodies,
                                         f32 dt) {
    return preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandPipelineDispatchPreflight preflight = preflight_island_pipeline_dispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    wake_all_island_sleepers_guarded(bodies, graph);
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

IslandSleepRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                         const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph(graph, bodies).reason;
}

IslandWakeRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies) {
    return preflight_island_wake_graph(graph, bodies).reason;
}

IslandPipelineDispatchPreflight preflight_pipeline_island_dispatch(
    u32 bodyCount,
    const ContactIslandGraph& graph,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt) {
    IslandPipelineDispatchPreflight preflight{};
    preflight.buildReason = island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints);
    preflight.wakeReason = island_wake_graph_reject_reason(graph, bodies);
    preflight.dispatchReason = island_dispatch_reject_reason(graph, dt);
    return preflight;
}

bool pipeline_build_islands_guarded(ContactIslandGraph& graph,
                                    u32 bodyCount,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints) {
    return graph.build_guarded(bodyCount, contacts, distanceConstraints);
}

u32 pipeline_wake_islands_guarded(RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return wake_all_island_sleepers_guarded(bodies, graph);
}

u32 pipeline_dispatch_islands_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_all_islands(bodies,
                                graph,
                                workBuffers,
                                distanceConstraints,
                                dt,
                                contactCompliance,
                                invMassFn);
}

u32 pipeline_wake_and_dispatch_islands_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    pipeline_wake_islands_guarded(bodies, graph);
    return pipeline_dispatch_islands_guarded(bodies,
                                             graph,
                                             workBuffers,
                                             distanceConstraints,
                                             dt,
                                             contactCompliance,
                                             invMassFn);
}

u32 pipeline_build_wake_and_dispatch_islands_guarded(
    RigidBodySoA& bodies,
    ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (!pipeline_build_islands_guarded(graph, bodyCount, contacts, distanceConstraints)) {
        return 0u;
    }
    return pipeline_wake_and_dispatch_islands_guarded(bodies,
                                                      graph,
                                                      workBuffers,
                                                      distanceConstraints,
                                                      dt,
                                                      contactCompliance,
                                                      invMassFn);
}

const char* island_dispatch_reject_reason_name(IslandDispatchRejectReason reason) {
    switch (reason) {
    case IslandDispatchRejectReason::None:
        return "None";
    case IslandDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandDispatchRejectReason island_dispatch_reject_reason(const ContactIslandGraph& graph, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandDispatchRejectReason::NonFiniteDt;
    }
    if (!has_dispatchable_islands(graph)) {
        return IslandDispatchRejectReason::NoDispatchableIslands;
    }
    return IslandDispatchRejectReason::None;
}

bool island_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                        f32 dt,
                                        IslandDispatchRejectReason expected) {
    return island_dispatch_reject_reason(graph, dt) == expected;
}

IslandDispatchRejectPreflight preflight_island_dispatch_reject(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchRejectPreflight preflight{};
    preflight.reason = island_dispatch_reject_reason(graph, dt);
    preflight.solve = preflight_island_solve(graph);
    preflight.invalidDt = preflight.reason == IslandDispatchRejectReason::InvalidDt ||
                          preflight.reason == IslandDispatchRejectReason::NonFiniteDt;
    preflight.skipped = preflight.reason != IslandDispatchRejectReason::None;
    return preflight;
}

bool can_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return !preflight_island_dispatch_reject(graph, dt).can_dispatch();
}

bool should_run_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    return preflight_island_dispatch_reject(graph, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandDispatchRejectPreflight preflight = preflight_island_dispatch_reject(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

const char* island_solve_job_reject_reason_name(IslandSolveJobRejectReason reason) {
    switch (reason) {
    case IslandSolveJobRejectReason::None:
        return "None";
    case IslandSolveJobRejectReason::EmptyJob:
        return "EmptyJob";
    case IslandSolveJobRejectReason::NullIsland:
        return "NullIsland";
    case IslandSolveJobRejectReason::ZeroConstraints:
        return "ZeroConstraints";
    case IslandSolveJobRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandSolveJobRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    default:
        return "Unknown";
    }
}

IslandSolveJobRejectReason island_solve_job_reject_reason(const IslandSolveJob& job, f32 dt) {
    if (!is_valid_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::InvalidDt;
    }
    if (!is_finite_island_solve_dt(dt)) {
        return IslandSolveJobRejectReason::NonFiniteDt;
    }
    if (job.empty) {
        return IslandSolveJobRejectReason::EmptyJob;
    }
    if (job.island == nullptr) {
        return IslandSolveJobRejectReason::NullIsland;
    }
    if (job.constraintCount == 0u) {
        return IslandSolveJobRejectReason::ZeroConstraints;
    }
    return IslandSolveJobRejectReason::None;
}

bool island_solve_job_rejects_for_reason(const IslandSolveJob& job,
                                         f32 dt,
                                         IslandSolveJobRejectReason expected) {
    return island_solve_job_reject_reason(job, dt) == expected;
}

IslandSolveJobRejectPreflight preflight_island_solve_job_reject(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobRejectPreflight preflight{};
    preflight.reason = island_solve_job_reject_reason(job, dt);
    preflight.invalidDt = preflight.reason == IslandSolveJobRejectReason::InvalidDt ||
                          preflight.reason == IslandSolveJobRejectReason::NonFiniteDt;
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = preflight.reason != IslandSolveJobRejectReason::None;
    return preflight;
}

bool can_skip_island_solve_job(const IslandSolveJob& job, f32 dt) {
    return !preflight_island_solve_job_reject(job, dt).can_dispatch();
}

bool should_run_island_solve_job(const IslandSolveJob& job, f32 dt) {
    return preflight_island_solve_job_reject(job, dt).can_dispatch();
}

const char* island_constraint_solve_reject_reason_name(IslandConstraintSolveRejectReason reason) {
    switch (reason) {
    case IslandConstraintSolveRejectReason::None:
        return "None";
    case IslandConstraintSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandConstraintSolveRejectReason::NoInRangeRefs:
        return "NoInRangeRefs";
    case IslandConstraintSolveRejectReason::NoMovableBodies:
        return "NoMovableBodies";
    default:
        return "Unknown";
    }
}

IslandConstraintSolveRejectReason island_constraint_solve_reject_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (!island_has_constraints(island)) {
        return IslandConstraintSolveRejectReason::EmptyIsland;
    }
    if (!preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve()) {
        return IslandConstraintSolveRejectReason::NoInRangeRefs;
    }
    if (!preflight_island_solve_bodies(island, bodies).can_solve()) {
        return IslandConstraintSolveRejectReason::NoMovableBodies;
    }
    return IslandConstraintSolveRejectReason::None;
}

bool island_constraint_solve_rejects_for_reason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected) {
    return island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints) == expected;
}

IslandConstraintSolveRejectPreflight preflight_island_constraint_solve_reject(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolveRejectPreflight preflight{};
    preflight.reason = island_constraint_solve_reject_reason(island, bodies, contacts, distanceConstraints);
    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(island, bodies);
    preflight.skipped = preflight.reason != IslandConstraintSolveRejectReason::None;
    return preflight;
}

bool can_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                      const RigidBodySoA& bodies,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();
}

bool should_run_island_constraint_solve(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflight_island_constraint_solve_reject(island, bodies, contacts, distanceConstraints).can_solve();
}

const char* island_sleep_solve_reject_reason_name(IslandSleepSolveRejectReason reason) {
    switch (reason) {
    case IslandSleepSolveRejectReason::None:
        return "None";
    case IslandSleepSolveRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandSleepSolveRejectReason::AllSleeping:
        return "AllSleeping";
    default:
        return "Unknown";
    }
}

IslandSleepSolveRejectReason island_sleep_solve_reject_reason(const ContactIslandGraph::Island& island,
                                                              const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandSleepSolveRejectReason::EmptyIsland;
    }
    if (preflight_island_sleep(island, bodies).can_skip_solve()) {
        return IslandSleepSolveRejectReason::AllSleeping;
    }
    return IslandSleepSolveRejectReason::None;
}

bool island_sleep_solve_rejects_for_reason(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies,
                                           IslandSleepSolveRejectReason expected) {
    return island_sleep_solve_reject_reason(island, bodies) == expected;
}

IslandSleepSolveRejectPreflight preflight_island_sleep_solve_reject(const ContactIslandGraph::Island& island,
                                                                    const RigidBodySoA& bodies) {
    IslandSleepSolveRejectPreflight preflight{};
    preflight.reason = island_sleep_solve_reject_reason(island, bodies);
    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.skipped = preflight.reason != IslandSleepSolveRejectReason::None;
    return preflight;
}

bool can_skip_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_solve_reject(island, bodies).can_solve();
}

bool should_run_island_sleep_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep_solve_reject(island, bodies).can_solve();
}

const char* island_wake_reject_reason_name(IslandWakeRejectReason reason) {
    switch (reason) {
    case IslandWakeRejectReason::None:
        return "None";
    case IslandWakeRejectReason::EmptyIsland:
        return "EmptyIsland";
    case IslandWakeRejectReason::NoMixedSleepState:
        return "NoMixedSleepState";
    case IslandWakeRejectReason::NoActiveDynamic:
        return "NoActiveDynamic";
    default:
        return "Unknown";
    }
}

IslandWakeRejectReason island_wake_reject_reason(const ContactIslandGraph::Island& island,
                                                 const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return IslandWakeRejectReason::EmptyIsland;
    }
    const IslandWakePreflight wake = preflight_island_wake(island, bodies);
    if (!wake.hasMixedSleepState) {
        return IslandWakeRejectReason::NoMixedSleepState;
    }
    if (wake.activeDynamicCount == 0u) {
        return IslandWakeRejectReason::NoActiveDynamic;
    }
    return IslandWakeRejectReason::None;
}

bool island_wake_rejects_for_reason(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    IslandWakeRejectReason expected) {
    return island_wake_reject_reason(island, bodies) == expected;
}

IslandWakeRejectPreflight preflight_island_wake_reject(const ContactIslandGraph::Island& island,
                                                       const RigidBodySoA& bodies) {
    IslandWakeRejectPreflight preflight{};
    preflight.reason = island_wake_reject_reason(island, bodies);
    preflight.wake = preflight_island_wake(island, bodies);
    preflight.skipped = preflight.reason != IslandWakeRejectReason::None;
    return preflight;
}

bool can_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_wake_reject(island, bodies).can_wake();
}

bool should_run_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_wake_reject(island, bodies).can_wake();
}

const char* island_sleep_graph_reject_reason_name(IslandSleepGraphRejectReason reason) {
    switch (reason) {
    case IslandSleepGraphRejectReason::None:
        return "None";
    case IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty:
        return "AllIslandsSleepingOrEmpty";
    default:
        return "Unknown";
    }
}

IslandSleepGraphRejectReason island_sleep_graph_reject_reason(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies) {
    if (preflight_island_sleep_graph(graph, bodies).has_solveable_islands()) {
        return IslandSleepGraphRejectReason::None;
    }
    return IslandSleepGraphRejectReason::AllIslandsSleepingOrEmpty;
}

bool island_sleep_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                           const RigidBodySoA& bodies,
                                           IslandSleepGraphRejectReason expected) {
    return island_sleep_graph_reject_reason(graph, bodies) == expected;
}

IslandSleepGraphRejectPreflight preflight_island_sleep_graph_reject(const ContactIslandGraph& graph,
                                                                    const RigidBodySoA& bodies) {
    IslandSleepGraphRejectPreflight preflight{};
    preflight.sleep = preflight_island_sleep_graph(graph, bodies);
    preflight.reason = island_sleep_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandSleepGraphRejectReason::None;
    return preflight;
}

bool can_skip_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();
}

bool should_run_island_sleep_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph_reject(graph, bodies).has_solveable_islands();
}

const char* island_wake_graph_reject_reason_name(IslandWakeGraphRejectReason reason) {
    switch (reason) {
    case IslandWakeGraphRejectReason::None:
        return "None";
    case IslandWakeGraphRejectReason::NoWakeableIslands:
        return "NoWakeableIslands";
    default:
        return "Unknown";
    }
}

IslandWakeGraphRejectReason island_wake_graph_reject_reason(const ContactIslandGraph& graph,
                                                            const RigidBodySoA& bodies) {
    if (preflight_island_wake_graph(graph, bodies).can_wake()) {
        return IslandWakeGraphRejectReason::None;
    }
    return IslandWakeGraphRejectReason::NoWakeableIslands;
}

bool island_wake_graph_rejects_for_reason(const ContactIslandGraph& graph,
                                          const RigidBodySoA& bodies,
                                          IslandWakeGraphRejectReason expected) {
    return island_wake_graph_reject_reason(graph, bodies) == expected;
}

IslandWakeGraphRejectPreflight preflight_island_wake_graph_reject(const ContactIslandGraph& graph,
                                                                  const RigidBodySoA& bodies) {
    IslandWakeGraphRejectPreflight preflight{};
    preflight.wake = preflight_island_wake_graph(graph, bodies);
    preflight.reason = island_wake_graph_reject_reason(graph, bodies);
    preflight.skipped = preflight.reason != IslandWakeGraphRejectReason::None;
    return preflight;
}

bool can_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_wake_graph_reject(graph, bodies).can_wake();
}

bool should_run_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_wake_graph_reject(graph, bodies).can_wake();
}

const char* island_pipeline_dispatch_reject_reason_name(IslandPipelineDispatchRejectReason reason) {
    switch (reason) {
    case IslandPipelineDispatchRejectReason::None:
        return "None";
    case IslandPipelineDispatchRejectReason::InvalidDt:
        return "InvalidDt";
    case IslandPipelineDispatchRejectReason::NonFiniteDt:
        return "NonFiniteDt";
    case IslandPipelineDispatchRejectReason::NoDispatchableIslands:
        return "NoDispatchableIslands";
    case IslandPipelineDispatchRejectReason::AllIslandsSleeping:
        return "AllIslandsSleeping";
    default:
        return "Unknown";
    }
}

IslandPipelineDispatchRejectReason island_pipeline_dispatch_reject_reason(const ContactIslandGraph& graph,
                                                                          const RigidBodySoA& bodies,
                                                                          f32 dt) {
    const IslandDispatchRejectReason dispatchReason = island_dispatch_reject_reason(graph, dt);
    if (dispatchReason == IslandDispatchRejectReason::InvalidDt) {
        return IslandPipelineDispatchRejectReason::InvalidDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NonFiniteDt) {
        return IslandPipelineDispatchRejectReason::NonFiniteDt;
    }
    if (dispatchReason == IslandDispatchRejectReason::NoDispatchableIslands) {
        return IslandPipelineDispatchRejectReason::NoDispatchableIslands;
    }
    if (island_sleep_graph_reject_reason(graph, bodies) != IslandSleepGraphRejectReason::None) {
        return IslandPipelineDispatchRejectReason::AllIslandsSleeping;
    }
    return IslandPipelineDispatchRejectReason::None;
}

bool island_pipeline_dispatch_rejects_for_reason(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies,
                                                 f32 dt,
                                                 IslandPipelineDispatchRejectReason expected) {
    return island_pipeline_dispatch_reject_reason(graph, bodies, dt) == expected;
}

IslandPipelineDispatchPreflight preflight_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                                                   const RigidBodySoA& bodies,
                                                                   f32 dt) {
    IslandPipelineDispatchPreflight preflight{};
    preflight.wake = preflight_island_wake_graph_reject(graph, bodies);
    preflight.sleep = preflight_island_sleep_graph_reject(graph, bodies);
    preflight.dispatch = preflight_island_dispatch_reject(graph, dt);
    preflight.reason = island_pipeline_dispatch_reject_reason(graph, bodies, dt);
    preflight.skipped = preflight.reason != IslandPipelineDispatchRejectReason::None;
    return preflight;
}

bool can_skip_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                       const RigidBodySoA& bodies,
                                       f32 dt) {
    return !preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

bool should_run_island_pipeline_dispatch(const ContactIslandGraph& graph,
                                         const RigidBodySoA& bodies,
                                         f32 dt) {
    return preflight_island_pipeline_dispatch(graph, bodies, dt).can_dispatch();
}

IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchDispatchResult result{};
    const IslandPipelineDispatchPreflight preflight = preflight_island_pipeline_dispatch(graph, bodies, dt);
    result.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    if (!preflight.can_dispatch()) {
        result.skipped = true;
        result.skippedCount = result.dispatchableCount;
        return result;
    }

    wake_all_island_sleepers_guarded(bodies, graph);
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

} // namespace fuse::physics
