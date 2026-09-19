#include <fuse/physics/solver/pbd_island_solve.hpp>

#include <fuse/physics/solver/constraint_accumulation.hpp>
#include <fuse/physics/solver/pbd_solver.hpp>

#include <cmath>

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
}

bool graph_has_constrained_islands(const ContactIslandGraph& graph) {
    return graph.constrainedIslandCount() > 0u;
}

bool is_valid_island_solve_dt(f32 dt) {
    return dt > 0.f;

bool is_valid_warm_start_dt(f32 dt) {
    return is_valid_island_solve_dt(dt);

bool is_finite_island_solve_dt(f32 dt) {
    return is_valid_island_solve_dt(dt) && std::isfinite(dt);

bool is_finite_warm_start_dt(f32 dt) {
    return is_finite_island_solve_dt(dt);

IslandSolveJobPreflight preflight_solve_island_job(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobPreflight preflight{};
    preflight.invalidDt = !is_finite_island_solve_dt(dt);
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = should_skip_island_solve_job(job);
    return preflight;

bool should_skip_solve_island_job(const IslandSolveJob& job, f32 dt) {
    return !preflight_solve_island_job(job, dt).can_dispatch();

IslandConstraintRefsPreflight preflight_island_constraint_refs(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintRefsPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        ++preflight.inRangeContactCount;
        if (contacts[contactIndex].valid) {
            ++preflight.validContactCount;

    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex < distanceConstraints.size()) {
            ++preflight.inRangeDistanceCount;


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
}

IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island) {
    IslandSleepStats stats{};
    stats.totalBodies = static_cast<u32>(island.bodyIndices.size());

    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            ++stats.staticCount;
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++stats.sleepingCount;
        } else {
            ++stats.activeCount;
        }
    }
    return stats;
}

bool is_island_fully_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    if (!island_has_constraints(island)) {
        return false;
    }

    const IslandSleepStats stats = compute_island_sleep_stats(bodies, island);
    return stats.activeCount == 0u && stats.sleepingCount > 0u;
}

IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island) {
    IslandSleepPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.stats = compute_island_sleep_stats(bodies, island);
    preflight.fullySleeping = is_island_fully_sleeping(bodies, island);
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
}

bool should_skip_solve_sleeping_island(const RigidBodySoA& bodies,
                                       const ContactIslandGraph::Island& island) {
    return !preflight_island_sleep(bodies, island).can_solve();
}

bool should_skip_solve_sleeping_island_index(const RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    return should_skip_solve_sleeping_island(bodies, graph.island(islandIndex));
}

IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
                                          const ContactIslandGraph::Island& island,
                                          f32 linearWakeThreshold,
                                          f32 angularWakeThreshold) {
    IslandWakePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (is_body_static_or_kinematic(bodies.flags[bodyIndex])) {
            continue;
        }

        if (!is_body_sleeping(bodies.flags[bodyIndex])) {
            continue;
        }

        const vec3 force = bodies.forces[bodyIndex];
        const vec3 torque = bodies.torques[bodyIndex];
        if (force.dot(force) > 0.f || torque.dot(torque) > 0.f) {
            preflight.hasExternalForce = true;
        }

        const f32 linearSpeed = bodies.linearVelocities[bodyIndex].length();
        const f32 angularSpeed = bodies.angularVelocities[bodyIndex].length();
        if (linearSpeed >= linearWakeThreshold || angularSpeed >= angularWakeThreshold) {
            preflight.hasVelocityWake = true;
        }
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
        }
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
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.skipped = preflight.stats.dispatchableCount == 0u;
    return preflight;

bool should_skip_island_dispatch_from_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt) {
    return !preflight_island_dispatch_from_jobs(jobs, dt).can_dispatch();
}

bool should_skip_island_solve(const ContactIslandGraph& graph) {
    return !has_dispatchable_islands(graph);

bool should_skip_island_solve_index(const ContactIslandGraph& graph, u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    return should_skip_island_solve_job(extract_island(graph, islandIndex));
}

bool should_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    const IslandDispatchPreflight preflight = preflight_island_dispatch(graph, dt);
    return !preflight.can_dispatch();

IslandDispatchJobPreflight preflight_dispatch_island_job(const IslandSolveJob& job, f32 dt) {
    IslandDispatchJobPreflight preflight{};
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.emptyJob = should_skip_island_solve_job(job);
    preflight.skipped = preflight.invalidDt || preflight.emptyJob;
    return preflight;
}

bool should_skip_dispatch_island_job(const IslandSolveJob& job, f32 dt) {
    return !preflight_dispatch_island_job(job, dt).can_dispatch();
}

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
        if (should_solve_island(job)) {
            indices.push_back(islandIndex);
    return indices;

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
    if (!is_valid_island_solve_dt(dt)) {
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

IslandDispatchResult dispatch_solve_island_job_result(RigidBodySoA& bodies,
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
        result.skipped = true;
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
                                      invMassFn)
        .solvedCount;
}

IslandDispatchBatchResult dispatch_all_islands_batch(RigidBodySoA& bodies,
                                                     const ContactIslandGraph& graph,
                                                     SolverWorkBuffers& workBuffers,
                                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                                     f32 dt,
                                                     f32 contactCompliance,
                                                     const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandDispatchBatchResult batch{};
    const IslandSolvePreflight preflight = preflight_island_solve(graph);
IslandBatchDispatchResult dispatch_all_islands_result(
    RigidBodySoA& bodies,
    const std::vector<IslandSolveJob> jobs = extract_island_jobs(graph);
    return dispatch_island_jobs_result(bodies,
                                       graph,
                                       jobs,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn);
}

IslandBatchDispatchResult dispatch_island_jobs_result(
    const std::vector<IslandSolveJob>& jobs,
    IslandBatchDispatchResult result{};
    const IslandDispatchPreflight preflight = preflight_island_dispatch(graph, dt);
    result.dispatchableCount = preflight.solve.stats.dispatchableCount;
    result.invalidDt = preflight.invalidDt;
    if (!preflight.can_dispatch()) {
        return batch;

    for (u32 islandIndex : collect_dispatchable_island_indices(graph)) {
        const IslandDispatchResult result = dispatch_solve_island_result(bodies,
                                                                         graph,
                                                                         islandIndex,
                                                                         workBuffers,
                                                                         distanceConstraints,
                                                                         dt,
                                                                         contactCompliance,
        if (result.solved) {
            ++batch.solvedCount;
        } else if (result.skipped) {
            ++batch.skippedCount;
    const std::vector<IslandSolveJob> dispatchableJobs = filter_dispatchable_jobs(jobs);
    for (const IslandSolveJob& job : dispatchableJobs) {
        if (dispatch_solve_island_job(bodies,
                                      job,
                                      invMassFn)) {
            ++result.solvedCount;
        } else {
            ++result.skippedCount;
        }
    return result;
}

IslandBatchDispatchResult dispatch_islands_at_indices_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    const std::vector<u32>& islandIndices,
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
        }
        ++result.dispatchableCount;
        if (dispatch_solve_island_job(bodies,
                                      job,
                                      workBuffers,
                                      distanceConstraints,
                                      dt,
                                      contactCompliance,
                                      invMassFn)) {
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
}

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

bool solve_island_job(RigidBodySoA& bodies,
                      const ContactIslandGraph::Island& island,
                      SolverWorkBuffers& workBuffers,
                      const std::vector<DistanceConstraint>& distanceConstraints,
                      f32 dt,
                      f32 contactCompliance,
                      const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (!is_valid_island_solve_dt(dt) || !island_has_constraints(island)) {
        return false;
    }

    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    if (should_skip_island_constraint_refs(island, contacts, distanceConstraints)) {
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

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    const ContactIslandGraphBuildPreflight graphPreflight =
        preflightContactIslandGraphBuild(bodyCount, contacts, distanceConstraints);
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
    return preflight;

bool should_skip_island_build(u32 bodyCount,
    return canSkipIslandBuild(bodyCount, contacts, distanceConstraints);

bool build_island_graph_guarded(ContactIslandGraph& graph,
    return graph.buildGuarded(bodyCount, contacts, distanceConstraints);

bool canSkipIslandBuild(u32 bodyCount,
    return canSkipContactIslandGraphBuild(bodyCount, contacts, distanceConstraints);

bool shouldRunIslandBuild(u32 bodyCount,
    return shouldRunContactIslandGraphBuild(bodyCount, contacts, distanceConstraints);

IslandSolveBodiesPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
                                                         const RigidBodySoA& bodies) {
    IslandSolveBodiesPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;

    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
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

IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
    IslandSleepPreflight preflight{};

            ++preflight.activeDynamicCount;

    preflight.allSleeping = preflight.activeDynamicCount == 0u && preflight.sleepingCount > 0u;

IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
    return preflight_island_sleep(graph.island(islandIndex), bodies);

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
    IslandWakePreflight preflight{};


    preflight.hasMixedSleepState =
        preflight.sleepingCount > 0u && preflight.activeDynamicCount > 0u;

IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
    return preflight_island_wake(graph.island(islandIndex), bodies);

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

IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
    IslandSleepGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_stats(graph, bodies);
    preflight.skipped = !preflight.has_solveable_islands();

bool should_skip_island_sleep_solve_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph(graph, bodies).skipped;

bool should_skip_island_sleep_solve(const ContactIslandGraph::Island& island,
    return preflight_island_sleep(island, bodies).can_skip_solve();

IslandWakeGraphStats compute_island_wake_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    IslandWakeGraphStats stats{};
        const IslandWakePreflight preflight = preflight_island_wake(graph.island(islandIndex), bodies);
        } else if (preflight.should_wake_sleepers()) {
            ++stats.wakeableCount;

IslandWakeGraphPreflight preflight_island_wake_graph(const ContactIslandGraph& graph,
    IslandWakeGraphPreflight preflight{};
    preflight.stats = compute_island_wake_stats(graph, bodies);
    preflight.skipped = !preflight.can_wake();

bool should_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_wake_graph(graph, bodies).can_wake();

bool should_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_wake(island, bodies).should_wake_sleepers();

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
    u32 wokeCount = 0;
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
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    const IslandContactImpulseWarmStartPreflight preflight =
        preflight_contact_impulse_warm_start_island(island, contacts);
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        return result;
    }

    warm_start_island_contact_impulses(workBuffers, island, contacts, dt);
    result.warmed = true;
    return result;
}

bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,
                                                         const ContactIslandGraph& graph,
                                                         u32 islandIndex,
                                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                                         f32 dt) {
    return warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt).warmed;
}

std::vector<u32> collect_contact_impulse_warm_startable_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandContactImpulseWarmStartPreflight preflight =
            preflight_contact_impulse_warm_start_island(graph.island(islandIndex), contacts);
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

u32 warm_start_all_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
                                            const ContactIslandGraph& graph,
                                            const std::vector<narrowphase::ContactManifold>& contacts,
                                            f32 dt) {
    return warm_start_all_contact_impulses_result(workBuffers, graph, contacts, dt).warmedCount;
}

IslandContactImpulseWarmStartBatchResult warm_start_all_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
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
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        result.skippedCount = result.warmStartableCount;
        return result;
    }

    for (u32 islandIndex : collect_contact_impulse_warm_startable_island_indices(graph, contacts)) {
        const IslandWarmStartResult islandResult =
            warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt);
        if (islandResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;
    return result;

u32 warm_start_graph_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
                                              const ContactIslandGraph& graph,
                                              const std::vector<narrowphase::ContactManifold>& contacts,
                                              f32 dt) {
    return warm_start_all_contact_impulses_guarded(workBuffers, graph, contacts, dt);

namespace {

u32 count_island_prior_impulse_coverage(const ContactIslandGraph::Island& island,
                                        const std::vector<narrowphase::ContactManifold>& contacts) {
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
    u32 islandIndex,
    if (!island_index_valid(graph, islandIndex)) {
    return preflight_warm_start_island_contact_impulses(graph.island(islandIndex), contacts, dt);

IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(
    IslandContactImpulseWarmStartStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandContactImpulseWarmStartPreflight preflight =
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

std::vector<u32> collect_contact_impulse_warm_startable_island_indices(
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
    return indices;

bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island,
    return !preflight_warm_start_island_contact_impulses(island, contacts, dt).can_warm_start();

bool should_skip_warm_start_contact_impulses_index(const ContactIslandGraph& graph,
    return !preflight_warm_start_island_contact_impulses_by_index(graph, islandIndex, contacts, dt)
                .can_warm_start();

IslandContactImpulseWarmStartResult warm_start_island_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    IslandContactImpulseWarmStartResult result{};
    result.islandIndex = islandIndex;

        preflight_warm_start_island_contact_impulses_by_index(graph, islandIndex, contacts, dt);
    if (!preflight.can_warm_start()) {
        result.skipped = true;

    warm_start_island_contact_impulses(workBuffers, graph.island(islandIndex), contacts, dt);
    result.warmed = true;

bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,
    return warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt).warmed;

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
                                                                                   graph,
                                                                                   islandIndex,
                                                                                   contacts,
                                                                                   dt,
                                                                                   priorDistanceLambdas,
                                                                                   priorContactLambdas);
        if (warmResult.warmed) {
        }
    }
    return result;
}

IslandCombinedWarmStartPreflight preflight_warm_start_island_combined(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    IslandCombinedWarmStartPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        preflight.lambda.skipped = true;
        preflight.impulse.skipped = true;
        return preflight;
    }

    preflight.lambda = preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    preflight.impulse = preflight_warm_start_island_contact_impulses(island, contacts, dt);
    return preflight;
}

bool is_valid_contact_impulse_warm_start_dt(f32 dt) {
    return is_valid_island_solve_dt(dt);
}

bool island_contact_indices_in_range(const ContactIslandGraph::Island& island, u32 contactCount) {
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contactCount) {
            return false;
        }
    }
    return true;
}

bool island_distance_indices_in_range(const ContactIslandGraph::Island& island, u32 distanceCount) {
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceCount) {
            return false;
        }
    }
    return true;
}

IslandSolveInputsPreflight preflight_island_solve_inputs(const ContactIslandGraph::Island& island,
                                                         u32 contactCount,
                                                         u32 distanceCount) {
    IslandSolveInputsPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex < contactCount) {
            ++preflight.inRangeContactCount;
        }
    }
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex < distanceCount) {
            ++preflight.inRangeDistanceCount;
        }
    }

    preflight.contactsInRange = preflight.inRangeContactCount == preflight.ownedContactCount;
    preflight.distancesInRange = preflight.inRangeDistanceCount == preflight.ownedDistanceCount;
    return preflight;
}

IslandSolveInputsPreflight preflight_island_solve_inputs_by_index(const ContactIslandGraph& graph,
                                                                  u32 islandIndex,
                                                                  u32 contactCount,
                                                                  u32 distanceCount) {
    IslandSolveInputsPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_solve_inputs(graph.island(islandIndex), contactCount, distanceCount);
}

bool should_skip_island_solve_inputs(const ContactIslandGraph::Island& island,
                                     u32 contactCount,
                                     u32 distanceCount) {
    return !preflight_island_solve_inputs(island, contactCount, distanceCount).can_solve();
}

IslandContactImpulsePreflight preflight_warm_start_contact_impulses(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulsePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.invalidDt = !is_valid_contact_impulse_warm_start_dt(dt);
    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        ++preflight.inRangeContactCount;
        if (contacts[contactIndex].warmNormalImpulse != 0.f) {
            ++preflight.nonZeroImpulseCount;
        }
    }
    return preflight;
}

IslandContactImpulsePreflight preflight_warm_start_contact_impulses_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulsePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
}

bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island,
                                             const std::vector<narrowphase::ContactManifold>& contacts,
                                             f32 dt) {
    return !preflight_warm_start_contact_impulses(island, contacts, dt).can_warm_start();
}

bool should_skip_contact_impulse_warm_start_island_index(const ContactIslandGraph& graph, u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    return should_skip_warm_start_island(graph.island(islandIndex));
}

IslandCombinedWarmStartPreflight preflight_warm_start_island_combined(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    IslandCombinedWarmStartPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.lambda = preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    preflight.impulse = preflight_warm_start_contact_impulses(island, contacts, dt);
    return preflight;
}

bool should_skip_warm_start_island_combined(const ContactIslandGraph::Island& island,
                                            const std::vector<narrowphase::ContactManifold>& contacts,
                                            f32 dt,
                                            const std::vector<f32>& priorDistanceLambdas,
                                            const std::vector<f32>& priorContactLambdas) {
    return !preflight_warm_start_island_combined(island, contacts, dt, priorDistanceLambdas, priorContactLambdas)
                .can_warm_start();
}

IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartStats stats{};
    stats.totalIslands = graph.islandCount();
    const bool invalidDt = !is_valid_contact_impulse_warm_start_dt(dt);

    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandContactImpulsePreflight preflight =
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (invalidDt) {
            ++stats.invalidDtCount;
        } else if (preflight.can_warm_start()) {
            ++stats.impulseSeedableCount;
        } else {
            ++stats.noImpulseDataCount;
    return stats;

u32 count_impulse_warm_startable_islands(const ContactIslandGraph& graph,
    return compute_island_contact_impulse_warm_start_stats(graph, contacts, dt).impulseSeedableCount;

bool has_impulse_warm_startable_islands(const ContactIslandGraph& graph,
    return count_impulse_warm_startable_islands(graph, contacts, dt) > 0u;

IslandContactImpulseWarmStartGraphPreflight preflight_warm_start_contact_impulses_graph(
    IslandContactImpulseWarmStartGraphPreflight preflight{};
    preflight.stats = compute_island_contact_impulse_warm_start_stats(graph, contacts, dt);
    preflight.skipped = preflight.stats.impulseSeedableCount == 0u;
    return preflight;

bool should_skip_warm_start_contact_impulses_graph(const ContactIslandGraph& graph,
    return !has_impulse_warm_startable_islands(graph, contacts, dt);

std::vector<u32> collect_impulse_warm_startable_island_indices(
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
    return indices;

IslandContactImpulseWarmStartResult warm_start_island_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    u32 islandIndex,
    IslandContactImpulseWarmStartResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    const IslandContactImpulsePreflight preflight = preflight_warm_start_contact_impulses(island, contacts, dt);
    if (!preflight.can_warm_start()) {

    warm_start_island_contact_impulses(workBuffers, island, contacts, dt);
    result.warmed = true;

u32 warm_start_all_islands_contact_impulses_guarded(
    const IslandContactImpulseWarmStartGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
        return 0u;

    u32 warmedCount = 0u;
    for (u32 islandIndex : collect_impulse_warm_startable_island_indices(graph, contacts, dt)) {
        const IslandContactImpulseWarmStartResult result =
            warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt);
        if (result.warmed) {
            ++warmedCount;
    return warm_start_all_islands_combined_result(workBuffers,
                                                  graph,
                                                  contacts,
                                                  dt,
                                                  priorDistanceLambdas,
                                                  priorContactLambdas)
        .warmedCount;

IslandBatchWarmStartResult warm_start_all_islands_combined_result(
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    IslandBatchWarmStartResult result{};
    const IslandCombinedWarmStartGraphPreflight preflight =
        preflight_warm_start_combined_graph(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas);
    result.warmStartableCount = preflight.stats.warmStartableCount;
        result.skippedCount = result.warmStartableCount;

    for (u32 islandIndex :
         collect_combined_warm_startable_island_indices(graph, contacts, dt, priorDistanceLambdas, priorContactLambdas)) {
        const IslandWarmStartResult warmResult = warm_start_island_combined_result(workBuffers,
                                                                                   islandIndex,
                                                                                   priorContactLambdas);
        if (warmResult.warmed) {
            ++result.warmedCount;
            ++result.skippedCount;
        }
    }
    return result;
}

bool is_valid_contact_impulse_warm_start_dt(f32 dt) {
    return is_valid_island_solve_dt(dt);
}

IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulsePreflight preflight{};
    if (should_skip_warm_start_island(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.invalidDt = !is_valid_contact_impulse_warm_start_dt(dt);
    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        if (contacts[contactIndex].warmNormalImpulse != 0.f) {
            ++preflight.seedableContactCount;
        }
    }
    return preflight;
}

IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulsePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_warm_start_island_contact_impulses(graph.island(islandIndex), contacts, dt);
}

IslandContactImpulseStats compute_island_contact_impulse_stats(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandContactImpulsePreflight preflight =
            preflight_warm_start_island_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.can_warm_start()) {
            ++stats.seedableCount;
        } else {
            ++stats.noImpulseCount;
        }
    }
    return stats;
}

u32 count_seedable_contact_impulse_islands(const ContactIslandGraph& graph,
                                           const std::vector<narrowphase::ContactManifold>& contacts,
                                           f32 dt) {
    return compute_island_contact_impulse_stats(graph, contacts, dt).seedableCount;
}

bool has_seedable_contact_impulse_islands(const ContactIslandGraph& graph,
                                          const std::vector<narrowphase::ContactManifold>& contacts,
                                          f32 dt) {
    return count_seedable_contact_impulse_islands(graph, contacts, dt) > 0u;
}

IslandContactImpulseGraphPreflight preflight_warm_start_contact_impulses_graph(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseGraphPreflight preflight{};
    preflight.invalidDt = !is_valid_contact_impulse_warm_start_dt(dt);
    preflight.stats = compute_island_contact_impulse_stats(graph, contacts, dt);
    preflight.skipped = preflight.stats.seedableCount == 0u;
    return preflight;
}

bool should_skip_contact_impulse_warm_start_graph(const ContactIslandGraph& graph,
                                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                                  f32 dt) {
    return !has_seedable_contact_impulse_islands(graph, contacts, dt) ||
           !is_valid_contact_impulse_warm_start_dt(dt);
}

std::vector<u32> collect_contact_impulse_seedable_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandContactImpulsePreflight preflight =
            preflight_warm_start_island_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

bool should_skip_contact_impulse_warm_start_island(const ContactIslandGraph::Island& island) {
    return should_skip_warm_start_island(island);
}

bool should_skip_contact_impulse_warm_start_island_index(const ContactIslandGraph& graph,
                                                         u32 islandIndex,
                                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                                         f32 dt) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    const IslandContactImpulsePreflight preflight =
        preflight_warm_start_island_contact_impulses(graph.island(islandIndex), contacts, dt);
    if (preflight.skipped || preflight.invalidDt) {
        return true;
    }
    return preflight.ownedContactCount == 0u;
}

bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,
                                                         const ContactIslandGraph& graph,
                                                         u32 islandIndex,
                                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                                         f32 dt) {
    return warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt)
        .warmed;
}

IslandContactImpulseWarmStartResult warm_start_island_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartResult result{};
    result.islandIndex = islandIndex;
    const IslandContactImpulsePreflight preflight =
        preflight_warm_start_island_contact_impulses_by_index(graph, islandIndex, contacts, dt);
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        return result;
    }

    warm_start_island_contact_impulses(workBuffers, graph.island(islandIndex), contacts, dt);
    result.warmed = true;
    return result;
}

u32 warm_start_all_islands_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
                                                    const ContactIslandGraph& graph,
                                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                                    f32 dt) {
    return warm_start_all_islands_contact_impulses_result(workBuffers, graph, contacts, dt).warmedCount;
}

IslandContactImpulseBatchResult warm_start_all_islands_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseBatchResult result{};
    const IslandContactImpulseGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    result.seedableCount = preflight.stats.seedableCount;
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        result.skippedCount = result.seedableCount;
        return result;
    }

    for (u32 islandIndex : collect_contact_impulse_seedable_island_indices(graph, contacts, dt)) {
        const IslandContactImpulseWarmStartResult warmResult =
            warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt);
        if (warmResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;
        }
    }
    return result;
}

bool is_valid_contact_impulse_warm_start_dt(f32 dt) {
    return is_valid_island_solve_dt(dt);
}

IslandContactImpulsePreflight preflight_island_contact_impulses(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulsePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }
    if (!is_valid_contact_impulse_warm_start_dt(dt)) {
        preflight.invalidDt = true;
        preflight.skipped = true;
        return preflight;
    }

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex < contacts.size() && contacts[contactIndex].warmNormalImpulse != 0.f) {
            ++preflight.impulseCoverage;
        }
    }
    preflight.skipped = preflight.impulseCoverage == 0u;
    return preflight;
}

IslandContactImpulsePreflight preflight_island_contact_impulses_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulsePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_contact_impulses(graph.island(islandIndex), contacts, dt);
}

IslandContactImpulseStats compute_island_contact_impulse_stats(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseStats stats{};
    stats.totalIslands = graph.islandCount();
    if (!is_valid_contact_impulse_warm_start_dt(dt)) {
        stats.emptyCount = stats.totalIslands;
        return stats;
    }

    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandContactImpulsePreflight preflight =
            preflight_island_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (!island_has_constraints(graph.island(islandIndex))) {
            ++stats.emptyCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;
        } else {
            ++stats.noImpulseCount;
        }
    }
    return stats;
}

u32 count_contact_impulse_warm_startable_islands(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    return compute_island_contact_impulse_stats(graph, contacts, dt).warmStartableCount;
}

bool has_contact_impulse_warm_startable_islands(const ContactIslandGraph& graph,
                                                const std::vector<narrowphase::ContactManifold>& contacts,
                                                f32 dt) {
    return count_contact_impulse_warm_startable_islands(graph, contacts, dt) > 0u;
}

IslandContactImpulseGraphPreflight preflight_contact_impulse_graph(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseGraphPreflight preflight{};
    preflight.invalidDt = !is_valid_contact_impulse_warm_start_dt(dt);
    preflight.stats = compute_island_contact_impulse_stats(graph, contacts, dt);
    preflight.skipped = preflight.invalidDt || preflight.stats.warmStartableCount == 0u;
    return preflight;
}

bool should_skip_contact_impulse_graph(const ContactIslandGraph& graph,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       f32 dt) {
    return !has_contact_impulse_warm_startable_islands(graph, contacts, dt) ||
           !is_valid_contact_impulse_warm_start_dt(dt);
}

std::vector<u32> collect_contact_impulse_warm_startable_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    std::vector<u32> indices;
    if (!is_valid_contact_impulse_warm_start_dt(dt)) {
        return indices;
    }

    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandContactImpulsePreflight preflight =
            preflight_island_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

bool should_skip_contact_impulse_island(const ContactIslandGraph::Island& island,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        f32 dt) {
    return !preflight_island_contact_impulses(island, contacts, dt).can_warm_start();
}

bool should_skip_contact_impulse_island_index(const ContactIslandGraph& graph,
                                              u32 islandIndex,
                                              const std::vector<narrowphase::ContactManifold>& contacts,
                                              f32 dt) {
    return !preflight_island_contact_impulses_by_index(graph, islandIndex, contacts, dt).can_warm_start();
}

IslandContactImpulseWarmStartResult warm_start_island_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartResult result{};
    result.islandIndex = islandIndex;
    const IslandContactImpulsePreflight preflight =
        preflight_island_contact_impulses_by_index(graph, islandIndex, contacts, dt);
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        return result;
    }

    warm_start_island_contact_impulses(workBuffers, graph.island(islandIndex), contacts, dt);
    result.warmed = true;
    return result;
}

bool warm_start_island_contact_impulses_by_index_guarded(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    return warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt).warmed;
}

u32 warm_start_all_island_contact_impulses_guarded(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    return warm_start_all_island_contact_impulses_result(workBuffers, graph, contacts, dt).warmedCount;
}

IslandBatchContactImpulseWarmStartResult warm_start_all_island_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandBatchContactImpulseWarmStartResult result{};
    const IslandContactImpulseGraphPreflight preflight = preflight_contact_impulse_graph(graph, contacts, dt);
    result.warmStartableCount = preflight.stats.warmStartableCount;
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        result.skippedCount = result.warmStartableCount;
        return result;
    }

    for (u32 islandIndex : collect_contact_impulse_warm_startable_island_indices(graph, contacts, dt)) {
        const IslandContactImpulseWarmStartResult warmResult =
            warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt);
        if (warmResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;
        }
    }
    return result;
}

bool is_valid_contact_impulse_warm_start_dt(f32 dt) {
    return is_valid_island_solve_dt(dt);
}

bool contact_has_warm_impulse(const narrowphase::ContactManifold& contact) {
    return contact.warmNormalImpulse != 0.f;
}

u32 island_impulse_coverage(const ContactIslandGraph::Island& island,
                            const std::vector<narrowphase::ContactManifold>& contacts) {
    u32 coverage = 0u;
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        if (contact_has_warm_impulse(contacts[contactIndex])) {
            ++coverage;
        }
    }
    return coverage;
}

IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulsePreflight preflight{};
    preflight.invalidDt = !is_valid_contact_impulse_warm_start_dt(dt);
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.impulseCoverage = island_impulse_coverage(island, contacts);
    return preflight;
}

IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulsePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_warm_start_island_contact_impulses(graph.island(islandIndex), contacts, dt);
}

IslandContactImpulseStats compute_island_contact_impulse_stats(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts) {
    IslandContactImpulseStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandContactImpulsePreflight preflight =
            preflight_warm_start_island_contact_impulses(graph.island(islandIndex), contacts, 1.f);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.impulseCoverage > 0u) {
            ++stats.warmStartableCount;
        } else {
            ++stats.noImpulseDataCount;
        }
    }
    return stats;
}

u32 count_impulse_warm_startable_islands(const ContactIslandGraph& graph,
                                         const std::vector<narrowphase::ContactManifold>& contacts) {
    return compute_island_contact_impulse_stats(graph, contacts).warmStartableCount;
}

bool has_impulse_warm_startable_islands(const ContactIslandGraph& graph,
                                        const std::vector<narrowphase::ContactManifold>& contacts) {
    return count_impulse_warm_startable_islands(graph, contacts) > 0u;
}

IslandContactImpulseGraphPreflight preflight_warm_start_contact_impulses_graph(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseGraphPreflight preflight{};
    preflight.invalidDt = !is_valid_contact_impulse_warm_start_dt(dt);
    preflight.stats = compute_island_contact_impulse_stats(graph, contacts);
    preflight.skipped = preflight.stats.warmStartableCount == 0u;
    return preflight;
}

bool should_skip_warm_start_contact_impulses_graph(const ContactIslandGraph& graph,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   f32 dt) {
    return !preflight_warm_start_contact_impulses_graph(graph, contacts, dt).can_warm_start();
}

bool should_skip_warm_start_contact_impulses_island(const ContactIslandGraph::Island& island) {
    return should_skip_warm_start_island(island);
}

bool should_skip_warm_start_contact_impulses_island_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_warm_start_island_index(graph, islandIndex);
}

std::vector<u32> collect_impulse_warm_startable_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandContactImpulsePreflight preflight =
            preflight_warm_start_island_contact_impulses_by_index(graph, islandIndex, contacts, dt);
        if (preflight.can_warm_start()) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

IslandContactImpulseResult warm_start_island_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseResult result{};
    result.islandIndex = islandIndex;
    const IslandContactImpulsePreflight preflight =
        preflight_warm_start_island_contact_impulses_by_index(graph, islandIndex, contacts, dt);
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        return result;
    }

    warm_start_island_contact_impulses(workBuffers, graph.island(islandIndex), contacts, dt);
    result.warmed = true;
    return result;
}

bool warm_start_island_contact_impulses_by_index_guarded(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    return warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt).warmed;
}

u32 warm_start_all_islands_contact_impulses_guarded(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    return warm_start_all_islands_contact_impulses_result(workBuffers, graph, contacts, dt).warmedCount;
}

IslandContactImpulseBatchResult warm_start_all_islands_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseBatchResult result{};
    const IslandContactImpulseGraphPreflight preflight =
        preflight_warm_start_contact_impulses_graph(graph, contacts, dt);
    result.warmStartableCount = preflight.stats.warmStartableCount;
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        result.skippedCount = result.warmStartableCount;
        return result;
    }

    for (u32 islandIndex : collect_impulse_warm_startable_island_indices(graph, contacts, dt)) {
        const IslandContactImpulseResult impulseResult =
            warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt);
        if (impulseResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;
        }
    }
    return result;
}

bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    }
    return (bodies.flags[bodyIndex] & RB_SLEEPING) != 0u;
}

bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return true;
    }
    const u32 flags = bodies.flags[bodyIndex];
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;
}

bool island_all_bodies_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    if (island.bodyIndices.empty()) {
        return false;
    }
    for (u32 bodyIndex : island.bodyIndices) {
        if (!is_body_sleeping(bodies, bodyIndex)) {
            return false;
        }
    }
    return true;
}

bool island_has_awake_body(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    for (u32 bodyIndex : island.bodyIndices) {
        if (is_body_static_or_kinematic(bodies, bodyIndex)) {
            continue;
        }
        if (!is_body_sleeping(bodies, bodyIndex)) {
            return true;
        }
    }
    return false;
}

bool island_needs_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    if (!island_has_constraints(island)) {
        return false;
    }
    return island_has_awake_body(bodies, island);
}

IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island) {
    IslandSleepPreflight preflight{};
    if (island.bodyIndices.empty()) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (is_body_sleeping(bodies, bodyIndex)) {
            ++preflight.sleepingBodyCount;
        } else {
            ++preflight.awakeBodyCount;
        }
    }
    preflight.allSleeping = preflight.awakeBodyCount == 0u && preflight.sleepingBodyCount > 0u;
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
}

IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
                                         const ContactIslandGraph::Island& island) {
    IslandWakePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.constraintCount = island_constraint_count(island);
    for (u32 bodyIndex : island.bodyIndices) {
        if (is_body_static_or_kinematic(bodies, bodyIndex)) {
            continue;
        }
        if (!is_body_sleeping(bodies, bodyIndex)) {
            ++preflight.awakeDynamicCount;
        }
    }
    preflight.needsWake = preflight.awakeDynamicCount > 0u;
    return preflight;
}

IslandWakePreflight preflight_island_wake_by_index(const RigidBodySoA& bodies,
                                                   const ContactIslandGraph& graph,
                                                   u32 islandIndex) {
    IslandWakePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_wake(bodies, graph.island(islandIndex));
}

IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    IslandSleepStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (island.isEmpty()) {
            ++stats.emptyCount;
            continue;
        }

        const IslandSleepPreflight sleepPreflight = preflight_island_sleep(bodies, island);
        if (sleepPreflight.can_skip_solve()) {
            ++stats.allSleepingCount;
        } else {
            ++stats.awakeCount;
        }

        if (preflight_island_wake(bodies, island).should_wake()) {
            ++stats.wakeRequiredCount;
        }
    }
    return stats;
}

u32 count_all_sleeping_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return compute_island_sleep_stats(bodies, graph).allSleepingCount;
}

u32 count_wake_required_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return compute_island_sleep_stats(bodies, graph).wakeRequiredCount;
}

bool should_skip_island_solve_for_sleep(const RigidBodySoA& bodies,
                                        const ContactIslandGraph::Island& island) {
    if (!island_has_constraints(island)) {
        return false;
    }
    return preflight_island_sleep(bodies, island).can_skip_solve();
}

bool should_skip_island_solve_for_sleep_index(const RigidBodySoA& bodies,
                                              const ContactIslandGraph& graph,
                                              u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    return should_skip_island_solve_for_sleep(bodies, graph.island(islandIndex));
}

bool should_skip_island_solve_job_for_sleep(const RigidBodySoA& bodies, const IslandSolveJob& job) {
    if (job.island == nullptr || job.empty) {
        return false;
    }
    return should_skip_island_solve_for_sleep(bodies, *job.island);
}

IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const ContactIslandGraph::Island& island,
    u32 contactManifoldCount,
    u32 distanceConstraintCount,
    f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contactManifoldCount) {
            ++preflight.outOfRangeContactCount;
        }
    }
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceConstraintCount) {
            ++preflight.outOfRangeDistanceCount;
        }
    }

    const u32 inRangeContacts = preflight.ownedContactCount - preflight.outOfRangeContactCount;
    const u32 inRangeDistances = preflight.ownedDistanceCount - preflight.outOfRangeDistanceCount;
    preflight.resolvableConstraintCount = inRangeContacts + inRangeDistances;
    return preflight;
}

IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    u32 contactManifoldCount,
    u32 distanceConstraintCount,
    f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_constraint_solve(graph.island(islandIndex),
                                             contactManifoldCount,
                                             distanceConstraintCount,
                                             dt);
}

IslandSolveSleepPreflight preflight_island_solve_sleep(const RigidBodySoA& bodies,
                                                       const ContactIslandGraph::Island& island,
                                                       u32 contactManifoldCount,
                                                       u32 distanceConstraintCount,
                                                       f32 dt) {
    IslandSolveSleepPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.sleep = preflight_island_sleep(bodies, island);
    preflight.wake = preflight_island_wake(bodies, island);
    preflight.constraints =
        preflight_island_constraint_solve(island, contactManifoldCount, distanceConstraintCount, dt);
    return preflight;
}

IslandSolveSleepPreflight preflight_island_solve_sleep_by_index(const RigidBodySoA& bodies,
                                                                const ContactIslandGraph& graph,
                                                                u32 islandIndex,
                                                                u32 contactManifoldCount,
                                                                u32 distanceConstraintCount,
                                                                f32 dt) {
    IslandSolveSleepPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_solve_sleep(bodies,
                                        graph.island(islandIndex),
                                        contactManifoldCount,
                                        distanceConstraintCount,
                                        dt);
}

IslandDispatchSleepPreflight preflight_island_dispatch_sleep(const RigidBodySoA& bodies,
                                                             const ContactIslandGraph& graph,
                                                             f32 dt) {
    IslandDispatchSleepPreflight preflight{};
    preflight.dispatch = preflight_island_dispatch(graph, dt);
    preflight.sleepStats = compute_island_sleep_stats(bodies, graph);
    preflight.skipped = preflight.dispatch.skipped;

    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!should_solve_island(job) || job.island == nullptr) {
            continue;
        }
        if (should_skip_island_solve_for_sleep(bodies, *job.island)) {
            continue;
        }
        ++preflight.solvableCount;
    }

    return preflight;
}

bool should_skip_island_dispatch_sleep(const RigidBodySoA& bodies,
                                       const ContactIslandGraph& graph,
                                       f32 dt) {
    return !preflight_island_dispatch_sleep(bodies, graph, dt).can_dispatch();
}

std::vector<u32> collect_solvable_island_indices(const RigidBodySoA& bodies,
                                                 const ContactIslandGraph& graph) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!should_solve_island(job) || job.island == nullptr) {
            continue;
        }
        if (should_skip_island_solve_for_sleep(bodies, *job.island)) {
            continue;
        }
        indices.push_back(islandIndex);
    }
    return indices;
}

bool has_solvable_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return !collect_solvable_island_indices(bodies, graph).empty();
}

bool dispatch_solve_island_sleep_guarded(RigidBodySoA& bodies,
                                         const ContactIslandGraph& graph,
                                         u32 islandIndex,
                                         SolverWorkBuffers& workBuffers,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         f32 dt,
                                         f32 contactCompliance,
                                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_solve_island_sleep_result(bodies,
                                              graph,
                                              islandIndex,
                                              workBuffers,
                                              distanceConstraints,
                                              dt,
                                              contactCompliance,
                                              invMassFn)
        .solved;
}

IslandDispatchResult dispatch_solve_island_sleep_result(RigidBodySoA& bodies,
                                                      const ContactIslandGraph& graph,
                                                      u32 islandIndex,
                                                      SolverWorkBuffers& workBuffers,
                                                      const std::vector<DistanceConstraint>& distanceConstraints,
                                                      f32 dt,
                                                      f32 contactCompliance,
                                                      const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandDispatchResult result{};
    result.islandIndex = islandIndex;

    const IslandSolveSleepPreflight preflight = preflight_island_solve_sleep_by_index(
        bodies,
        graph,
        islandIndex,
        static_cast<u32>(workBuffers.contactManifolds().size()),
        static_cast<u32>(distanceConstraints.size()),
        dt);
    if (!preflight.can_solve()) {
        result.skipped = true;
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

u32 dispatch_all_islands_sleep_guarded(RigidBodySoA& bodies,
                                       const ContactIslandGraph& graph,
                                       SolverWorkBuffers& workBuffers,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt,
                                       f32 contactCompliance,
                                       const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_all_islands_sleep_result(bodies,
                                             graph,
                                             workBuffers,
                                             distanceConstraints,
                                             dt,
                                             contactCompliance,
                                             invMassFn)
        .dispatch.solvedCount;
}

IslandBatchSleepDispatchResult dispatch_all_islands_sleep_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandBatchSleepDispatchResult result{};
    const IslandDispatchSleepPreflight preflight = preflight_island_dispatch_sleep(bodies, graph, dt);
    result.dispatch.dispatchableCount = preflight.dispatch.solve.stats.dispatchableCount;
    result.wakeRequiredCount = preflight.sleepStats.wakeRequiredCount;

    if (!preflight.can_dispatch()) {
        result.dispatch.skipped = true;
        return result;
    }

    for (u32 islandIndex : collect_solvable_island_indices(bodies, graph)) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_sleep_result(
            bodies,
            graph,
            islandIndex,
            workBuffers,
            distanceConstraints,
            dt,
            contactCompliance,
            invMassFn);
        if (dispatchResult.solved) {
            ++result.dispatch.solvedCount;
        } else if (should_skip_island_solve_for_sleep_index(bodies, graph, islandIndex)) {
            ++result.sleepSkippedCount;
            ++result.dispatch.skippedCount;
        } else {
            ++result.dispatch.skippedCount;
        }
    }
    return result;
}

bool is_body_sleeping(u32 bodyFlags) {
    return (bodyFlags & RB_SLEEPING) != 0u;
}

bool is_body_static_or_kinematic(u32 bodyFlags) {
    return (bodyFlags & RB_STATIC) != 0u || (bodyFlags & RB_KINEMATIC) != 0u;
}

namespace {

constexpr f32 kWakeForceEpsilon = 1e-6f;

bool is_dynamic_body(u32 bodyFlags) {
    return !is_body_static_or_kinematic(bodyFlags);
}

void accumulate_island_body_sleep_stats(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies,
                                        IslandSleepPreflight& preflight) {
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (!is_dynamic_body(bodies.flags[bodyIndex])) {
            continue;
        }
        ++preflight.dynamicBodyCount;
        if (is_body_sleeping(bodies.flags[bodyIndex])) {
            ++preflight.sleepingBodyCount;
        } else {
            ++preflight.awakeBodyCount;
        }
    }
    preflight.allSleeping =
        preflight.dynamicBodyCount > 0u && preflight.awakeBodyCount == 0u;
}

} // namespace

bool is_island_all_sleeping(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep(island, bodies).allSleeping;
}

bool island_has_awake_dynamic_body(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    const IslandSleepPreflight preflight = preflight_island_sleep(island, bodies);
    return !preflight.skipped && preflight.awakeBodyCount > 0u;
}

IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    accumulate_island_body_sleep_stats(island, bodies, preflight);
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
        const IslandSleepPreflight preflight = preflight_island_sleep(graph.island(islandIndex), bodies);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.allSleeping) {
            ++stats.allSleepingCount;
        } else {
            ++stats.awakeCount;
        }
    }
    return stats;
}

u32 count_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return compute_island_sleep_stats(graph, bodies).awakeCount;
}

bool has_awake_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return count_awake_islands(graph, bodies) > 0u;
}

IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies) {
    IslandSleepGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_stats(graph, bodies);
    preflight.skipped = preflight.stats.awakeCount == 0u;
    return preflight;
}

bool should_skip_awake_island_dispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_sleep_graph(graph, bodies).can_dispatch_awake();
}

std::vector<u32> collect_awake_island_indices(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        if (island_has_awake_dynamic_body(graph.island(islandIndex), bodies)) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return preflight_island_sleep(island, bodies).can_skip_solve();
}

bool should_skip_sleeping_island_solve_index(const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             const RigidBodySoA& bodies) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    return should_skip_sleeping_island_solve(graph.island(islandIndex), bodies);
}

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts) {
    IslandWakePreflight preflight{};
    if (should_skip_island_wake(island)) {
        preflight.skipped = true;
        return preflight;
    }

    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (!is_dynamic_body(bodies.flags[bodyIndex])) {
            continue;
        }
        if (is_body_sleeping(bodies.flags[bodyIndex])) {
            ++preflight.sleepingBodyCount;
            if (bodies.forces[bodyIndex].dot(bodies.forces[bodyIndex]) > kWakeForceEpsilon) {
                ++preflight.wakeCandidateCount;
            }
        } else {
            ++preflight.awakeBodyCount;
        }
    }

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
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
        }
        if (contact.warmNormalImpulse != 0.f && (sleepingA || sleepingB)) {
            ++preflight.wakeCandidateCount;
        }
    }

    if (preflight.wakeCandidateCount == 0u && preflight.hasAwakeNeighborContact) {
        preflight.wakeCandidateCount = preflight.sleepingBodyCount;
    }

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

bool should_skip_island_wake(const ContactIslandGraph::Island& island) {
    return !island_has_constraints(island);
}

bool should_wake_island(const ContactIslandGraph::Island& island,
                        const RigidBodySoA& bodies,
                        const std::vector<narrowphase::ContactManifold>& contacts) {
    return preflight_island_wake(island, bodies, contacts).should_wake();
}

u32 wake_island_bodies_guarded(RigidBodySoA& bodies,
                               const ContactIslandGraph::Island& island,
                               const std::vector<narrowphase::ContactManifold>& contacts) {
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies, contacts);
    if (!preflight.should_wake()) {
        return 0u;
    }

    u32 wokenCount = 0u;
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (!is_dynamic_body(bodies.flags[bodyIndex]) || !is_body_sleeping(bodies.flags[bodyIndex])) {
            continue;
        }

        bool shouldWakeBody = bodies.forces[bodyIndex].dot(bodies.forces[bodyIndex]) > kWakeForceEpsilon;
        if (!shouldWakeBody) {
            for (u32 contactIndex : island.contactIndices) {
                if (contactIndex >= contacts.size()) {
                    continue;
                }
                const narrowphase::ContactManifold& contact = contacts[contactIndex];
                if (contact.bodyA != bodyIndex && contact.bodyB != bodyIndex) {
                    continue;
                }
                const u32 otherBody = contact.bodyA == bodyIndex ? contact.bodyB : contact.bodyA;
                if (otherBody < bodies.count() && is_dynamic_body(bodies.flags[otherBody]) &&
                    !is_body_sleeping(bodies.flags[otherBody])) {
                    shouldWakeBody = true;
                    break;
                }
                if (contact.warmNormalImpulse != 0.f) {
                    shouldWakeBody = true;
                    break;
                }
            }
        }

        if (!shouldWakeBody) {
            continue;
        }

        bodies.flags[bodyIndex] &= ~RB_SLEEPING;
        bodies.sleepTimers[bodyIndex] = 0.f;
        ++wokenCount;
    }
    return wokenCount;
}

u32 wake_island_bodies_by_index_guarded(RigidBodySoA& bodies,
                                        const ContactIslandGraph& graph,
                                        u32 islandIndex,
                                        const std::vector<narrowphase::ContactManifold>& contacts) {
    if (!island_index_valid(graph, islandIndex)) {
        return 0u;
    }
    return wake_island_bodies_guarded(bodies, graph.island(islandIndex), contacts);
}

IslandConstraintSolvePreflight preflight_solve_island(const ContactIslandGraph::Island& island,
                                                      const RigidBodySoA& bodies,
                                                      const std::vector<DistanceConstraint>& distanceConstraints,
                                                      const SolverWorkBuffers& workBuffers,
                                                      f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    if (!island_has_constraints(island)) {
        preflight.empty = true;
        preflight.skipped = true;
        return preflight;
    }

    preflight.sleep = preflight_island_sleep(island, bodies);
    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());

    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            ++preflight.outOfRangeContactCount;
        }
    }
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceConstraints.size()) {
            ++preflight.outOfRangeDistanceCount;
        }
    }
    return preflight;
}

IslandConstraintSolvePreflight preflight_solve_island_by_index(const ContactIslandGraph& graph,
                                                               u32 islandIndex,
                                                               const RigidBodySoA& bodies,
                                                               const std::vector<DistanceConstraint>& distanceConstraints,
                                                               const SolverWorkBuffers& workBuffers,
                                                               f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_solve_island(graph.island(islandIndex),
                                  bodies,
                                  distanceConstraints,
                                  workBuffers,
                                  dt);
}

bool should_skip_solve_island_preflight(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies,
                                        const std::vector<DistanceConstraint>& distanceConstraints,
                                        const SolverWorkBuffers& workBuffers,
                                        f32 dt) {
    return !preflight_solve_island(island, bodies, distanceConstraints, workBuffers, dt).can_solve();
}

bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (should_skip_solve_island_preflight(island, bodies, distanceConstraints, workBuffers, dt)) {
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

bool dispatch_solve_awake_island(RigidBodySoA& bodies,
                                 const ContactIslandGraph& graph,
                                 u32 islandIndex,
                                 SolverWorkBuffers& workBuffers,
                                 const std::vector<DistanceConstraint>& distanceConstraints,
                                 f32 dt,
                                 f32 contactCompliance,
                                 const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (!is_valid_island_solve_dt(dt)) {
        return false;
    }
    const IslandSolveJob job = extract_island(graph, islandIndex);
    return dispatch_solve_awake_island_job(bodies,
                                           job,
                                           workBuffers,
                                           distanceConstraints,
                                           dt,
                                           contactCompliance,
                                           invMassFn);
}

bool dispatch_solve_awake_island_job(RigidBodySoA& bodies,
                                     const IslandSolveJob& job,
                                     SolverWorkBuffers& workBuffers,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     f32 dt,
                                     f32 contactCompliance,
                                     const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (!is_valid_island_solve_dt(dt) || !should_solve_island(job) || job.island == nullptr) {
        return false;
    }
    if (should_skip_sleeping_island_solve(*job.island, bodies)) {
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

bool is_sleeping_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    }
    return (bodies.flags[bodyIndex] & RB_SLEEPING) != 0u;
}

bool is_static_or_kinematic_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    }
    const u32 flags = bodies.flags[bodyIndex];
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;
}

bool island_body_indices_valid(const ContactIslandGraph::Island& island, u32 bodyCount) {
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodyCount) {
            return false;
        }
    }
    return true;
}

bool has_out_of_range_island_body_indices(const ContactIslandGraph::Island& island, u32 bodyCount) {
    return !island_body_indices_valid(island, bodyCount);
}

IslandSleepStats compute_island_sleep_stats(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies) {
    IslandSleepStats stats{};
    stats.totalBodies = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (is_static_or_kinematic_body(bodies, bodyIndex)) {
            ++stats.staticOrKinematicCount;
            continue;
        }
        if (is_sleeping_body(bodies, bodyIndex)) {
            ++stats.sleepingCount;
        } else {
            ++stats.awakeDynamicCount;
        }
    }
    return stats;
}

bool island_all_dynamic_bodies_sleeping(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies) {
    const IslandSleepStats stats = compute_island_sleep_stats(island, bodies);
    return stats.awakeDynamicCount == 0u && stats.sleepingCount > 0u;
}

bool island_has_awake_dynamic_body(const ContactIslandGraph::Island& island,
                                   const RigidBodySoA& bodies) {
    return compute_island_sleep_stats(island, bodies).awakeDynamicCount > 0u;
}

IslandSleepSolvePreflight preflight_island_sleep_solve(const ContactIslandGraph::Island& island,
                                                       const RigidBodySoA& bodies) {
    IslandSleepSolvePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.stats = compute_island_sleep_stats(island, bodies);
    preflight.allDynamicSleeping = island_all_dynamic_bodies_sleeping(island, bodies);
    return preflight;
}

IslandSleepSolvePreflight preflight_island_sleep_solve_by_index(const ContactIslandGraph& graph,
                                                                u32 islandIndex,
                                                                const RigidBodySoA& bodies) {
    IslandSleepSolvePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_sleep_solve(graph.island(islandIndex), bodies);
}

bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island,
                                       const RigidBodySoA& bodies) {
    const IslandSleepSolvePreflight preflight = preflight_island_sleep_solve(island, bodies);
    return preflight.skipped || !preflight.can_solve();
}

bool should_skip_sleeping_island_solve_index(const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             const RigidBodySoA& bodies) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    return should_skip_sleeping_island_solve(graph.island(islandIndex), bodies);
}

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          f32 linearThreshold,
                                          f32 angularThreshold) {
    IslandWakePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (is_static_or_kinematic_body(bodies, bodyIndex)) {
            continue;
        }

        ++preflight.eligibleDynamicCount;
        const f32 linearSpeed = bodies.linearVelocities[bodyIndex].length();
        const f32 angularSpeed = bodies.angularVelocities[bodyIndex].length();
        if (linearSpeed < linearThreshold && angularSpeed < angularThreshold) {
            ++preflight.belowThresholdCount;
        } else {
            ++preflight.aboveThresholdCount;
        }
    }

    if (preflight.eligibleDynamicCount == 0u) {
        preflight.skipped = true;
    }
    return preflight;
}

IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   const RigidBodySoA& bodies,
                                                   f32 linearThreshold,
                                                   f32 angularThreshold) {
    IslandWakePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_wake(graph.island(islandIndex), bodies, linearThreshold, angularThreshold);
}

bool should_skip_island_sleep_detection(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies) {
    if (!island_has_constraints(island)) {
        return true;
    }
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        if (!is_static_or_kinematic_body(bodies, bodyIndex)) {
            return false;
        }
    }
    return true;
}

bool should_skip_island_sleep_detection_index(const ContactIslandGraph& graph,
                                              u32 islandIndex,
                                              const RigidBodySoA& bodies) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    return should_skip_island_sleep_detection(graph.island(islandIndex), bodies);
}

IslandSolveJobPreflight preflight_solve_island_job(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies,
                                                   f32 dt) {
    IslandSolveJobPreflight preflight{};
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.emptyIsland = !island_has_constraints(island);
    preflight.outOfRangeBodyIndices = has_out_of_range_island_body_indices(island, bodies.count());
    preflight.sleep = preflight_island_sleep_solve(island, bodies);
    preflight.skipped = preflight.emptyIsland;
    return preflight;
}

IslandSolveJobPreflight preflight_solve_island_job_by_index(const ContactIslandGraph& graph,
                                                            u32 islandIndex,
                                                            const RigidBodySoA& bodies,
                                                            f32 dt) {
    IslandSolveJobPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_solve_island_job(graph.island(islandIndex), bodies, dt);
}

bool should_skip_solve_island_job_preflight(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies,
                                            f32 dt) {
    return !preflight_solve_island_job(island, bodies, dt).can_solve();
}

bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const IslandSolveJobPreflight preflight = preflight_solve_island_job(island, bodies, dt);
    if (!preflight.can_solve()) {
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

IslandSolveJobResult solve_island_job_result(RigidBodySoA& bodies,
                                             const ContactIslandGraph& graph,
                                             u32 islandIndex,
                                             SolverWorkBuffers& workBuffers,
                                             const std::vector<DistanceConstraint>& distanceConstraints,
                                             f32 dt,
                                             f32 contactCompliance,
                                             const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandSolveJobResult result{};
    result.islandIndex = islandIndex;
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    const IslandSolveJobPreflight preflight = preflight_solve_island_job(island, bodies, dt);
    if (!preflight.can_solve()) {
        result.skipped = true;
        return result;
    }

    result.solved = solve_island_job(bodies,
                                     island,
                                     workBuffers,
                                     distanceConstraints,
                                     dt,
                                     contactCompliance,
                                     invMassFn);
    result.skipped = !result.solved;
    return result;
}

bool is_static_or_kinematic_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return true;
    }
    const u32 flags = bodies.flags[bodyIndex];
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;
}

bool is_sleeping_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    }
    return (bodies.flags[bodyIndex] & RB_SLEEPING) != 0u;
}

bool is_awake_dynamic_body(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    }
    return !is_static_or_kinematic_body(bodies, bodyIndex) && !is_sleeping_body(bodies, bodyIndex);
}

bool island_all_dynamic_bodies_sleeping(const RigidBodySoA& bodies,
                                        const ContactIslandGraph::Island& island) {
    const IslandSleepPreflight preflight = preflight_island_sleep(bodies, island);
    return !preflight.skipped && preflight.allDynamicSleeping;
}

bool island_has_awake_dynamic_bodies(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandSleepPreflight preflight = preflight_island_sleep(bodies, island);
    return !preflight.skipped && preflight.awakeBodyCount > 0u;
}

bool body_has_wake_impetus(const RigidBodySoA& bodies, u32 bodyIndex, f32 velocityThreshold) {
    if (bodyIndex >= bodies.count() || is_static_or_kinematic_body(bodies, bodyIndex)) {
        return false;
    }
    if (!is_sleeping_body(bodies, bodyIndex)) {
        return false;
    }

    if (bodies.forces[bodyIndex].dot(bodies.forces[bodyIndex]) > 0.f ||
        bodies.torques[bodyIndex].dot(bodies.torques[bodyIndex]) > 0.f) {
        return true;
    }

    const f32 linearSpeed = bodies.linearVelocities[bodyIndex].length();
    const f32 angularSpeed = bodies.angularVelocities[bodyIndex].length();
    return linearSpeed > velocityThreshold || angularSpeed > velocityThreshold;
}

IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island) {
    IslandSleepPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    for (u32 bodyIndex : island.bodyIndices) {
        if (is_static_or_kinematic_body(bodies, bodyIndex)) {
            continue;
        }
        ++preflight.dynamicBodyCount;
        if (is_sleeping_body(bodies, bodyIndex)) {
            ++preflight.sleepingBodyCount;
        } else {
            ++preflight.awakeBodyCount;
        }
    }

    preflight.allDynamicSleeping =
        preflight.dynamicBodyCount > 0u && preflight.awakeBodyCount == 0u;
    return preflight;
}

IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies,
                                                     u32 islandIndex) {
    IslandSleepPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_sleep(bodies, graph.island(islandIndex));
}

IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    IslandSleepStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandSleepPreflight preflight = preflight_island_sleep(bodies, graph.island(islandIndex));
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.allDynamicSleeping) {
            ++stats.allSleepingCount;
        } else {
            ++stats.hasAwakeCount;
        }
    }
    return stats;
}

u32 count_all_sleeping_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return compute_island_sleep_stats(bodies, graph).allSleepingCount;
}

bool has_awake_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    return compute_island_sleep_stats(bodies, graph).hasAwakeCount > 0u;
}

IslandSleepGraphPreflight preflight_island_sleep_graph(const RigidBodySoA& bodies,
                                                       const ContactIslandGraph& graph) {
    IslandSleepGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_stats(bodies, graph);
    preflight.skipped = preflight.stats.totalIslands == 0u;
    return preflight;
}

bool should_skip_island_solve_sleeping(const RigidBodySoA& bodies,
                                       const ContactIslandGraph::Island& island) {
    const IslandSleepPreflight preflight = preflight_island_sleep(bodies, island);
    return preflight.skipped || !preflight.can_solve();
}

bool should_skip_island_solve_sleeping_index(const ContactIslandGraph& graph,
                                             const RigidBodySoA& bodies,
                                             u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    return should_skip_island_solve_sleeping(bodies, graph.island(islandIndex));
}

std::vector<u32> collect_awake_dispatchable_island_indices(const RigidBodySoA& bodies,
                                                           const ContactIslandGraph& graph) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (!should_solve_island(job)) {
            continue;
        }
        if (should_skip_island_solve_sleeping(bodies, *job.island)) {
            continue;
        }
        indices.push_back(islandIndex);
    }
    return indices;
}

IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
                                          const ContactIslandGraph::Island& island,
                                          f32 velocityThreshold) {
    IslandWakePreflight preflight{};
    if (should_skip_island_wake_check(island)) {
        preflight.skipped = true;
        return preflight;
    }

    for (u32 bodyIndex : island.bodyIndices) {
        if (!is_sleeping_body(bodies, bodyIndex) || is_static_or_kinematic_body(bodies, bodyIndex)) {
            continue;
        }
        ++preflight.sleepingBodyCount;
        if (body_has_wake_impetus(bodies, bodyIndex, velocityThreshold)) {
            ++preflight.wakeCandidateCount;
        }
    }
    return preflight;
}

IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
                                                   const RigidBodySoA& bodies,
                                                   u32 islandIndex,
                                                   f32 velocityThreshold) {
    IslandWakePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_wake(bodies, graph.island(islandIndex), velocityThreshold);
}

bool should_skip_island_wake_check(const ContactIslandGraph::Island& island) {
    return !island_has_constraints(island);
}

IslandConstraintSolvePreflight preflight_island_constraint_solve(const RigidBodySoA& bodies,
                                                                 const ContactIslandGraph& graph,
                                                                 u32 islandIndex,
                                                                 f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    preflight.solve = preflight_island_solve(graph);
    preflight.sleep = preflight_island_sleep(bodies, island);
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
    }
    return preflight;
}

bool should_skip_island_constraint_solve(const RigidBodySoA& bodies,
                                         const ContactIslandGraph& graph,
                                         u32 islandIndex,
                                         f32 dt) {
    return !preflight_island_constraint_solve(bodies, graph, islandIndex, dt).can_solve();
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
    const ContactIslandGraph& graph,
    u32 islandIndex,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    IslandDispatchResult result{};
    result.islandIndex = islandIndex;
    if (!is_valid_island_solve_dt(dt)) {
        result.skipped = true;
        return result;
    }
    if (!island_index_valid(graph, islandIndex)) {
        result.skipped = true;
        return result;
    }

    const ContactIslandGraph::Island& island = graph.island(islandIndex);
    if (!should_solve_island(extract_island(graph, islandIndex))) {
        result.skipped = true;
        return result;
    }
    if (should_skip_island_solve_sleeping(bodies, island)) {
        result.skipped = true;
        return result;
    }

    result.solved = solve_island_job(bodies,
                                     island,
                                     workBuffers,
                                     distanceConstraints,
                                     dt,
                                     contactCompliance,
                                     invMassFn);
    result.skipped = !result.solved;
    return result;
}

u32 dispatch_awake_islands(RigidBodySoA& bodies,
                           const ContactIslandGraph& graph,
                           SolverWorkBuffers& workBuffers,
                           const std::vector<DistanceConstraint>& distanceConstraints,
                           f32 dt,
                           f32 contactCompliance,
                           const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_awake_islands_result(bodies,
                                         graph,
                                         workBuffers,
                                         distanceConstraints,
                                         dt,
                                         contactCompliance,
                                         invMassFn)
        .solvedCount;
}

IslandBatchDispatchResult dispatch_awake_islands_result(
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

    for (u32 islandIndex : collect_awake_dispatchable_island_indices(bodies, graph)) {
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

namespace {

bool bodyIndexValid(const RigidBodySoA& bodies, u32 bodyIndex) {
    return bodyIndex < bodies.count();
}

bool hasNonZeroForce(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (!bodyIndexValid(bodies, bodyIndex)) {
        return false;
    }
    return bodies.forces[bodyIndex].dot(bodies.forces[bodyIndex]) > 0.f;
}

bool constraintPairHasEffectiveMass(const RigidBodySoA& bodies, u32 bodyA, u32 bodyB) {
    return island_effective_inv_mass(bodies, bodyA) + island_effective_inv_mass(bodies, bodyB) > 0.f;
}

} // namespace

bool body_is_sleeping(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (!bodyIndexValid(bodies, bodyIndex)) {
        return false;
    }
    return (bodies.flags[bodyIndex] & RB_SLEEPING) != 0u;
}

bool body_is_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (!bodyIndexValid(bodies, bodyIndex)) {
        return false;
    }
    const u32 flags = bodies.flags[bodyIndex];
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;
}

bool body_is_active_dynamic(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (!bodyIndexValid(bodies, bodyIndex)) {
        return false;
    }
    return !body_is_static_or_kinematic(bodies, bodyIndex) && !body_is_sleeping(bodies, bodyIndex);
}

f32 island_effective_inv_mass(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (!bodyIndexValid(bodies, bodyIndex)) {
        return 0.f;
    }
    if (body_is_static_or_kinematic(bodies, bodyIndex) || body_is_sleeping(bodies, bodyIndex)) {
        return 0.f;
    }
    return bodies.invMasses[bodyIndex];
}

IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island) {
    IslandSleepStats stats{};
    stats.bodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (!bodyIndexValid(bodies, bodyIndex)) {
            continue;
        }
        if (body_is_static_or_kinematic(bodies, bodyIndex)) {
            ++stats.staticOrKinematicCount;
        } else if (body_is_sleeping(bodies, bodyIndex)) {
            ++stats.sleepingCount;
        } else {
            ++stats.activeDynamicCount;
        }
    }
    return stats;
}

IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island) {
    IslandSleepPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.stats = compute_island_sleep_stats(bodies, island);
    preflight.allSleeping =
        preflight.stats.activeDynamicCount == 0u && preflight.stats.sleepingCount > 0u;
    preflight.allStaticOrSleeping = preflight.stats.activeDynamicCount == 0u;
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
}

bool island_all_bodies_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandSleepPreflight preflight = preflight_island_sleep(bodies, island);
    return !preflight.skipped && preflight.allSleeping;
}

bool should_skip_solve_sleeping_island(const RigidBodySoA& bodies,
                                       const ContactIslandGraph::Island& island) {
    const IslandSleepPreflight preflight = preflight_island_sleep(bodies, island);
    return preflight.skipped || preflight.allStaticOrSleeping;
}

IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
                                          const ContactIslandGraph::Island& island) {
    IslandWakePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    const IslandSleepStats stats = compute_island_sleep_stats(bodies, island);
    preflight.sleepingBodyCount = stats.sleepingCount;
    if (preflight.sleepingBodyCount == 0u) {
        return preflight;
    }

    const bool hasActiveDynamic = stats.activeDynamicCount > 0u;
    for (u32 bodyIndex : island.bodyIndices) {
        if (!bodyIndexValid(bodies, bodyIndex) || !body_is_sleeping(bodies, bodyIndex)) {
            continue;
        }
        if (hasNonZeroForce(bodies, bodyIndex)) {
            ++preflight.forceWakeCount;
        } else if (hasActiveDynamic) {
            ++preflight.contactWakeCount;
        }
    }
    return preflight;
}

IslandWakePreflight preflight_island_wake_by_index(const RigidBodySoA& bodies,
                                                   const ContactIslandGraph& graph,
                                                   u32 islandIndex) {
    IslandWakePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_wake(bodies, graph.island(islandIndex));
}

bool should_skip_island_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandWakePreflight preflight = preflight_island_wake(bodies, island);
    return preflight.skipped || !preflight.can_wake();
}

bool should_wake_island(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    return !should_skip_island_wake(bodies, island);
}

u32 wake_island_bodies_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandWakePreflight preflight = preflight_island_wake(bodies, island);
    if (!preflight.can_wake()) {
        return 0u;
    }

    u32 wokenCount = 0u;
    const IslandSleepStats stats = compute_island_sleep_stats(bodies, island);
    const bool hasActiveDynamic = stats.activeDynamicCount > 0u;
    for (u32 bodyIndex : island.bodyIndices) {
        if (!bodyIndexValid(bodies, bodyIndex) || !body_is_sleeping(bodies, bodyIndex)) {
            continue;
        }
        const bool forceWake = hasNonZeroForce(bodies, bodyIndex);
        const bool contactWake = hasActiveDynamic && !forceWake;
        if (!forceWake && !contactWake) {
            continue;
        }
        bodies.flags[bodyIndex] &= ~RB_SLEEPING;
        bodies.sleepTimers[bodyIndex] = 0.f;
        ++wokenCount;
    }
    return wokenCount;
}

u32 wake_island_bodies_by_index_guarded(RigidBodySoA& bodies,
                                        const ContactIslandGraph& graph,
                                        u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return 0u;
    }
    return wake_island_bodies_guarded(bodies, graph.island(islandIndex));
}

u32 count_resolvable_island_constraints(const RigidBodySoA& bodies,
                                          const ContactIslandGraph::Island& island,
                                          const std::vector<DistanceConstraint>& distanceConstraints,
                                          const std::vector<narrowphase::ContactManifold>& contacts) {
    u32 resolvableCount = 0u;
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (constraintPairHasEffectiveMass(bodies, contact.bodyA, contact.bodyB)) {
            ++resolvableCount;
        }
    }
    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= distanceConstraints.size()) {
            continue;
        }
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        if (constraintPairHasEffectiveMass(bodies, constraint.bodyA, constraint.bodyB)) {
            ++resolvableCount;
        }
    }
    return resolvableCount;
}

IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const RigidBodySoA& bodies,
    const ContactIslandGraph::Island& island,
    const std::vector<DistanceConstraint>& distanceConstraints,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.sleep = preflight_island_sleep(bodies, island);
    preflight.ownedConstraintCount = island_constraint_count(island);
    preflight.resolvableConstraintCount =
        count_resolvable_island_constraints(bodies, island, distanceConstraints, contacts);
    return preflight;
}

IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<DistanceConstraint>& distanceConstraints,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandConstraintSolvePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_constraint_solve(bodies,
                                             graph.island(islandIndex),
                                             distanceConstraints,
                                             contacts,
                                             dt);
}

bool should_skip_island_constraint_solve(const RigidBodySoA& bodies,
                                         const ContactIslandGraph::Island& island,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                         f32 dt) {
    return !preflight_island_constraint_solve(bodies, island, distanceConstraints, contacts, dt).can_solve();
}

bool solve_island_job_guarded(RigidBodySoA& bodies,
                              const ContactIslandGraph::Island& island,
                              SolverWorkBuffers& workBuffers,
                              const std::vector<DistanceConstraint>& distanceConstraints,
                              f32 dt,
                              f32 contactCompliance,
                              const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve(
        bodies,
        island,
        distanceConstraints,
        workBuffers.contactManifolds(),
        dt);
    if (!preflight.can_solve()) {
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

} // namespace fuse::physics
