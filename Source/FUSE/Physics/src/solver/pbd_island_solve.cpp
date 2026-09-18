#include <fuse/physics/solver/pbd_island_solve.hpp>

#include <fuse/physics/solver/constraint_accumulation.hpp>

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
    return dt > 0.f;
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

bool should_skip_island_dispatch_job(const IslandSolveJob& job, f32 dt) {
    return !is_valid_island_solve_dt(dt) || should_skip_island_solve_job(job);
}

bool dispatch_solve_island_job(RigidBodySoA& bodies,
                               const IslandSolveJob& job,
                               SolverWorkBuffers& workBuffers,
                               const std::vector<DistanceConstraint>& distanceConstraints,
                               f32 dt,
                               f32 contactCompliance,
                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn) {
    if (should_skip_island_dispatch_job(job, dt)) {
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
    if (should_skip_island_dispatch_job(job, dt)) {
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

IslandBatchDispatchResult dispatch_dispatchable_jobs(
    RigidBodySoA& bodies,
    const std::vector<IslandSolveJob>& jobs,
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

    const std::vector<IslandSolveJob> dispatchable = filter_dispatchable_jobs(jobs);
    result.dispatchableCount = static_cast<u32>(dispatchable.size());
    if (dispatchable.empty()) {
        result.skipped = true;
        return result;
    }

    for (const IslandSolveJob& job : dispatchable) {
        const IslandDispatchResult dispatchResult = dispatch_solve_island_job_result(bodies,
                                                                                     job,
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

IslandWarmStartBatchResult warm_start_all_islands_result(SolverWorkBuffers& workBuffers,
                                                         const ContactIslandGraph& graph,
                                                         const std::vector<f32>& priorDistanceLambdas,
                                                         const std::vector<f32>& priorContactLambdas) {
    IslandWarmStartBatchResult result{};
    const IslandWarmStartGraphPreflight preflight =
        preflight_warm_start_graph(graph, priorDistanceLambdas, priorContactLambdas);
    result.warmStartableCount = preflight.stats.warmStartableCount;
    if (!preflight.can_warm_start()) {
        result.skipped = true;
        result.skippedCount = preflight.stats.totalIslands;
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
    return preflight;
}

bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island, f32 dt) {
    return preflight_warm_start_contact_impulses(island, {}, dt).skipped ||
           !is_valid_warm_start_dt(dt);
}

bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,
                                                         const ContactIslandGraph& graph,
                                                         u32 islandIndex,
                                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                                         f32 dt) {
    if (!island_index_valid(graph, islandIndex)) {
        return false;
    }
    return warm_start_island_contact_impulses_guarded(workBuffers, graph.island(islandIndex), contacts, dt);
}

u32 warm_start_graph_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
                                              const ContactIslandGraph& graph,
                                              const std::vector<narrowphase::ContactManifold>& contacts,
                                              f32 dt) {
    if (!is_valid_warm_start_dt(dt)) {
        return 0u;
    }

    u32 warmedCount = 0u;
    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const ContactIslandGraph::Island& island = graph.island(islandIndex);
        if (warm_start_island_contact_impulses_guarded(workBuffers, island, contacts, dt)) {
            ++warmedCount;
        }
    }
    return warmedCount;
}

bool warm_start_island_combined_guarded(SolverWorkBuffers& workBuffers,
                                        const ContactIslandGraph::Island& island,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        f32 dt,
                                        const std::vector<f32>& priorDistanceLambdas,
                                        const std::vector<f32>& priorContactLambdas) {
    if (should_skip_warm_start_island(island)) {
        return false;
    }

    const IslandWarmStartPreflight preflight =
        preflight_warm_start_island(island, priorDistanceLambdas, priorContactLambdas);
    const bool hasPriorLambdas = preflight.can_warm_start();
    const bool hasContactImpulses = !island.contactIndices.empty();
    if (!hasPriorLambdas && !hasContactImpulses) {
        return false;
    }

    if (hasPriorLambdas) {
        warm_start_island_lambdas(workBuffers, island, priorDistanceLambdas, priorContactLambdas);
    }
    if (hasContactImpulses) {
        warm_start_island_contact_impulses(workBuffers, island, contacts, dt);
    }
    return true;
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

} // namespace fuse::physics
