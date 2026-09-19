#include <fuse/physics/solver/pbd_island_solve.hpp>

#include <fuse/physics/solver/constraint_accumulation.hpp>

#include <cmath>

namespace fuse::physics {

bool island_index_valid(const ContactIslandGraph& graph, u32 islandIndex) {
    return islandIndex < graph.islandCount();
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

IslandSolvePreflight preflight_island_solve(const ContactIslandGraph& graph) {
    IslandSolvePreflight preflight{};
    preflight.stats = compute_island_solve_stats(graph);
    preflight.skipped = preflight.stats.dispatchableCount == 0u;

IslandDispatchPreflight preflight_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    IslandDispatchPreflight preflight{};
    preflight.solve = preflight_island_solve(graph);
    preflight.skipped = preflight.solve.skipped;

bool should_skip_island_solve(const ContactIslandGraph& graph) {
    return !has_dispatchable_islands(graph);

bool should_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt) {
    const IslandDispatchPreflight preflight = preflight_island_dispatch(graph, dt);
    return !preflight.can_dispatch();

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
bool dispatch_solve_island_job(RigidBodySoA& bodies,
                              const IslandSolveJob& job,
    if (!is_valid_island_solve_dt(dt) || !should_solve_island(job)) {
    return solve_island_job(bodies,
                            *job.island,

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

IslandDispatchResult dispatch_solve_island_job_result(RigidBodySoA& bodies,
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
    const IslandWarmStartGraphPreflight preflight =
        preflight_warm_start_graph(graph, priorDistanceLambdas, priorContactLambdas);
    result.warmStartableCount = preflight.stats.warmStartableCount;
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        result.skippedCount = result.warmStartableCount;
        return result;

    for (u32 islandIndex : collect_warm_startable_island_indices(graph, priorDistanceLambdas, priorContactLambdas)) {
        const IslandWarmStartResult warmResult =
            warm_start_island_lambdas_result(workBuffers, graph, islandIndex, priorDistanceLambdas, priorContactLambdas);
        if (warmResult.warmed) {
            ++result.warmedCount;
        } else {
            ++result.skippedCount;

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

IslandCombinedWarmStartPreflight preflight_warm_start_combined_island(
    f32 dt,
    IslandCombinedWarmStartPreflight preflight{};

    preflight.lambdas = preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    preflight.impulses = preflight_warm_start_contact_impulses(island, contacts, dt);
        return 0u;

    u32 warmedCount = 0u;
        const IslandWarmStartResult result =
        if (result.warmed) {
            ++warmedCount;
    return warmedCount;
}

u32 warm_start_all_islands_guarded(SolverWorkBuffers& workBuffers,
                                   const ContactIslandGraph& graph,
                                   const std::vector<f32>& priorDistanceLambdas,
                                   const std::vector<f32>& priorContactLambdas) {
    const IslandWarmStartGraphPreflight preflight =
        preflight_warm_start_graph(graph, priorDistanceLambdas, priorContactLambdas);
    if (!preflight.can_warm_start()) {
        return 0u;
    }

    u32 warmedCount = 0u;
    for (u32 islandIndex : collect_warm_startable_island_indices(graph, priorDistanceLambdas, priorContactLambdas)) {
        const IslandWarmStartResult result =
            warm_start_island_lambdas_result(workBuffers, graph, islandIndex, priorDistanceLambdas, priorContactLambdas);
        if (result.warmed) {
            ++warmedCount;
    return warmedCount;

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

        warm_start_island_lambdas(workBuffers, island, priorDistanceLambdas, priorContactLambdas);
    if (hasContactImpulses) {
        warm_start_island_contact_impulses(workBuffers, island, contacts, dt);
    }
    return true;
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
    if (!should_warm_start_island(island)) {
        return;
    }

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
            continue;
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
                                                        const std::vector<f32>& priorDistanceLambdas,
                                                        const std::vector<f32>& priorContactLambdas) {

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

    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex >= priorDistanceLambdas.size()) {
        ++out.distanceSlotCount;
        if (priorDistanceLambdas[distanceIndex] != 0.f) {
            out.hasDistanceLambdas = true;

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= priorContactLambdas.size()) {
        ++out.contactSlotCount;
        if (priorContactLambdas[contactIndex] != 0.f) {
            out.hasContactLambdas = true;
        if (contactIndex < contacts.size() && contacts[contactIndex].warmNormalImpulse != 0.f && dt > 0.f) {
            out.hasContactImpulses = true;

    return out.hasDistanceLambdas || out.hasContactLambdas || out.hasContactImpulses;

void warm_start_island_lambdas_guarded(SolverWorkBuffers& workBuffers,
                                       const IslandWarmStartPreflight& preflight) {
    if (!preflight.hasDistanceLambdas && !preflight.hasContactLambdas) {
        return;
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
            ++stats.skippedCount;
        } else if (preflight.can_warm_start()) {
            ++stats.warmStartableCount;
        }
    }
    return stats;
}

bool should_skip_frame_warm_start(const ContactIslandGraph& graph,
                                  const std::vector<f32>& priorDistanceLambdas,
                                  const std::vector<f32>& priorContactLambdas) {
    return compute_island_warm_start_stats(graph, priorDistanceLambdas, priorContactLambdas)
               .warmStartableCount == 0u;
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

u32 warm_start_all_islands_guarded(SolverWorkBuffers& workBuffers,
                                   const ContactIslandGraph& graph,
                                   const std::vector<f32>& priorDistanceLambdas,
                                   const std::vector<f32>& priorContactLambdas) {
    if (should_skip_frame_warm_start(graph, priorDistanceLambdas, priorContactLambdas)) {
        return 0u;
    }

    u32 seededCount = 0u;
    for (u32 islandIndex : collect_warm_startable_island_indices(graph,
                                                                 priorDistanceLambdas,
                                                                 priorContactLambdas)) {
        if (warm_start_island_lambdas_guarded(workBuffers,
                                              graph.island(islandIndex),
                                              priorDistanceLambdas,
                                              priorContactLambdas)) {
            ++seededCount;
        }
    }
    return seededCount;
}

} // namespace fuse::physics
