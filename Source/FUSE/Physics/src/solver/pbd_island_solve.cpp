#include <fuse/physics/solver/pbd_island_solve.hpp>

#include <fuse/physics/solver/constraint_accumulation.hpp>

namespace fuse::physics {

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
}

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

std::vector<IslandSolveJob> extract_island_jobs(const ContactIslandGraph& graph) {
    const u32 count = graph.islandCount();
    std::vector<IslandSolveJob> jobs;
    jobs.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        jobs.push_back(extract_island(graph, islandIndex));
    }
    return jobs;
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

bool is_valid_island_solve_dt(f32 dt) {
    return dt > 0.f;
}

bool is_valid_warm_start_dt(f32 dt) {
    return is_valid_island_solve_dt(dt);
}

IslandSolvePreflight preflight_island_solve(const ContactIslandGraph& graph) {
    IslandSolvePreflight preflight{};
    preflight.stats = compute_island_solve_stats(graph);
    preflight.skipped = preflight.stats.dispatchableCount == 0u;
    return preflight;
}

IslandDispatchPreflight preflight_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchPreflight preflight{};
    preflight.solve = preflight_island_solve(graph);
    preflight.invalidDt = !is_valid_island_solve_dt(dt);
    preflight.skipped = preflight.solve.skipped;
    return preflight;
}

bool should_skip_island_solve(const ContactIslandGraph& graph) {
    return !has_dispatchable_islands(graph);
}

bool should_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    const IslandDispatchPreflight preflight = preflight_island_dispatch(graph, dt);
    return !preflight.can_dispatch();
}

std::vector<u32> collect_dispatchable_island_indices(const ContactIslandGraph& graph) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandSolveJob job = extract_island(graph, islandIndex);
        if (should_solve_island(job)) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

std::vector<IslandSolveJob> collect_dispatchable_island_jobs(const ContactIslandGraph& graph) {
    return filter_dispatchable_jobs(extract_island_jobs(graph));
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
    if (!is_valid_island_solve_dt(dt)) {
        result.skipped = true;
        return result;
    }
    if (!should_solve_island(job) || job.island == nullptr) {
        result.skipped = true;
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
    return result;
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
    result.islandIndex = islandIndex;
    if (!is_valid_island_solve_dt(dt)) {
        result.skipped = true;
        return result;
    }
    const IslandSolveJob job = extract_island(graph, islandIndex);
    if (!should_solve_island(job)) {
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

u32 dispatch_all_islands(RigidBodySoA& bodies,
                         const ContactIslandGraph& graph,
                         SolverWorkBuffers& workBuffers,
                         const std::vector<DistanceConstraint>& distanceConstraints,
                         f32 dt,
                         f32 contactCompliance,
                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    return dispatch_all_islands_result(bodies,
                                       graph,
                                       workBuffers,
                                       distanceConstraints,
                                       dt,
                                       contactCompliance,
                                       invMassFn)
        .solvedCount;
}

IslandBatchDispatchResult dispatch_all_islands_result(
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
    if (!island_has_constraints(island)) {
        return false;
    }

    workBuffers.clearPositionDeltasForIslandBodies(island.bodyIndices);

    const std::vector<narrowphase::ContactManifold>& contacts = workBuffers.contactManifolds();

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
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
    const IslandWarmStartGraphPreflight preflight =
        preflight_warm_start_graph(graph, priorDistanceLambdas, priorContactLambdas);
    result.warmStartableCount = preflight.stats.warmStartableCount;
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        result.skippedCount = result.warmStartableCount;
        return result;
    }

    for (u32 islandIndex : collect_warm_startable_island_indices(graph, priorDistanceLambdas, priorContactLambdas)) {
        const IslandWarmStartResult warmResult =
            warm_start_island_lambdas_result(workBuffers, graph, islandIndex, priorDistanceLambdas, priorContactLambdas);
        if (warmResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;
        }
    }
    return result;
}

IslandContactImpulseWarmStartPreflight preflight_warm_start_contact_impulses(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartPreflight preflight{};
    preflight.invalidDt = !is_valid_warm_start_dt(dt);
    if (should_skip_warm_start_island(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        if (contacts[contactIndex].warmNormalImpulse != 0.f) {
            ++preflight.nonZeroImpulseCount;
        }
    }
    return preflight;
}

IslandContactImpulseWarmStartPreflight preflight_warm_start_contact_impulses_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandContactImpulseWarmStartPreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
}

bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island, f32 dt) {
    if (!is_valid_warm_start_dt(dt) || should_skip_warm_start_island(island)) {
        return true;
    }
    return island.contactIndices.empty();
}

bool should_skip_warm_start_contact_impulses_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   f32 dt) {
    if (!island_index_valid(graph, islandIndex)) {
        return true;
    }
    return should_skip_warm_start_contact_impulses(graph.island(islandIndex), dt);
}

IslandCombinedWarmStartPreflight preflight_warm_start_combined_island(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    IslandCombinedWarmStartPreflight preflight{};
    if (should_skip_warm_start_island(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.lambdas = preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    preflight.impulses = preflight_warm_start_contact_impulses(island, contacts, dt);
    return preflight;
}

bool warm_start_island_combined_guarded(SolverWorkBuffers& workBuffers,
                                        const ContactIslandGraph::Island& island,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        f32 dt,
                                        const std::vector<f32>& priorDistanceLambdas,
                                        const std::vector<f32>& priorContactLambdas) {
    const IslandCombinedWarmStartPreflight preflight =
        preflight_warm_start_combined_island(island, contacts, dt, priorDistanceLambdas, priorContactLambdas);
    if (!preflight.can_warm_start()) {
        return false;
    }

    if (preflight.lambdas.can_warm_start()) {
        warm_start_island_lambdas(workBuffers, island, priorDistanceLambdas, priorContactLambdas);
    }
    if (preflight.impulses.can_warm_start()) {
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
    }
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= priorContactLambdas.size()) {
            continue;
        }
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

void warm_start_island_contact_impulses(SolverWorkBuffers& workBuffers,
                                        const ContactIslandGraph::Island& island,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        f32 dt) {
    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        workBuffers.seedContactLambdaFromImpulse(contactIndex,
                                                 contacts[contactIndex].warmNormalImpulse,
                                                 dt);
    }
}

bool warm_start_island_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
                                                const ContactIslandGraph::Island& island,
                                                const std::vector<narrowphase::ContactManifold>& contacts,
                                                f32 dt) {
    const IslandContactImpulseWarmStartPreflight preflight =
        preflight_warm_start_contact_impulses(island, contacts, dt);
    if (!preflight.can_warm_start()) {
        return false;
    }
    warm_start_island_contact_impulses(workBuffers, island, contacts, dt);
    return true;
}

IslandWarmStartResult warm_start_island_contact_impulses_result(SolverWorkBuffers& workBuffers,
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
        preflight_warm_start_contact_impulses(island, contacts, dt);
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

u32 warm_start_all_islands_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
                                                    const ContactIslandGraph& graph,
                                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                                    f32 dt) {
    return warm_start_all_islands_contact_impulses_result(workBuffers, graph, contacts, dt).warmedCount;
}

IslandBatchWarmStartResult warm_start_all_islands_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt) {
    IslandBatchWarmStartResult result{};
    if (!is_valid_warm_start_dt(dt)) {
        result.skipped = true;
        return result;
    }

    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandContactImpulseWarmStartPreflight preflight =
            preflight_warm_start_contact_impulses(graph.island(islandIndex), contacts, dt);
        if (!preflight.can_warm_start()) {
            continue;
        }
        ++result.warmStartableCount;
        const IslandWarmStartResult warmResult =
            warm_start_island_contact_impulses_result(workBuffers, graph, islandIndex, contacts, dt);
        if (warmResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;
        }
    }

    if (result.warmStartableCount == 0u) {
        result.skipped = true;
    }
    return result;
}

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

u32 warm_start_all_islands_combined_guarded(SolverWorkBuffers& workBuffers,
                                            const ContactIslandGraph& graph,
                                            const std::vector<narrowphase::ContactManifold>& contacts,
                                            f32 dt,
                                            const std::vector<f32>& priorDistanceLambdas,
                                            const std::vector<f32>& priorContactLambdas) {
    if (!is_valid_warm_start_dt(dt)) {
        return 0u;
    }

    u32 warmedCount = 0u;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandWarmStartResult result = warm_start_island_combined_result(workBuffers,
                                                                               graph,
                                                                               islandIndex,
                                                                               contacts,
                                                                               dt,
                                                                               priorDistanceLambdas,
                                                                               priorContactLambdas);
        if (result.warmed) {
            ++warmedCount;
        }
    }
    return warmedCount;
}

} // namespace fuse::physics
