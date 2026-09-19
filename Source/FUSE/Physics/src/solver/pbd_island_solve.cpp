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

bool is_finite_island_solve_dt(f32 dt) {
    return is_valid_island_solve_dt(dt) && std::isfinite(dt);
}

bool is_finite_warm_start_dt(f32 dt) {
    return is_finite_island_solve_dt(dt);
}

IslandSolveJobPreflight preflight_solve_island_job(const IslandSolveJob& job, f32 dt) {
    IslandSolveJobPreflight preflight{};
    preflight.invalidDt = !is_finite_island_solve_dt(dt);
    preflight.constraintCount = job.constraintCount;
    preflight.skipped = should_skip_island_solve_job(job);
    return preflight;
}

bool should_skip_solve_island_job(const IslandSolveJob& job, f32 dt) {
    return !preflight_solve_island_job(job, dt).can_dispatch();
}

IslandConstraintRefsPreflight preflight_island_constraint_refs(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintRefsPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.ownedContactCount = static_cast<u32>(island.contactIndices.size());
    preflight.ownedDistanceCount = static_cast<u32>(island.distanceIndices.size());

    for (u32 contactIndex : island.contactIndices) {
        if (contactIndex >= contacts.size()) {
            continue;
        }
        ++preflight.inRangeContactCount;
        if (contacts[contactIndex].valid) {
            ++preflight.validContactCount;
        }
    }

    for (u32 distanceIndex : island.distanceIndices) {
        if (distanceIndex < distanceConstraints.size()) {
            ++preflight.inRangeDistanceCount;
        }
    }

    return preflight;
}

bool should_skip_island_constraint_refs(const ContactIslandGraph::Island& island,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_constraint_refs(island, contacts, distanceConstraints).can_solve();
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
    preflight.invalidDt = !is_finite_island_solve_dt(dt);
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

bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island,
                                             const std::vector<narrowphase::ContactManifold>& contacts,
                                             f32 dt) {
    return !preflight_warm_start_contact_impulses(island, contacts, dt).can_warm_start();
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
    return warm_start_all_islands_combined_result(workBuffers,
                                                  graph,
                                                  contacts,
                                                  dt,
                                                  priorDistanceLambdas,
                                                  priorContactLambdas)
        .warmedCount;
}

IslandBatchWarmStartResult warm_start_all_islands_combined_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas) {
    IslandBatchWarmStartResult result{};
    if (!is_valid_warm_start_dt(dt)) {
        result.skipped = true;
        return result;
    }

    for (u32 islandIndex = 0; islandIndex < graph.islandCount(); ++islandIndex) {
        const IslandCombinedWarmStartPreflight preflight = preflight_warm_start_combined_island(
            graph.island(islandIndex), contacts, dt, priorDistanceLambdas, priorContactLambdas);
        if (!preflight.can_warm_start()) {
            continue;
        }
        ++result.warmStartableCount;
        const IslandWarmStartResult warmResult = warm_start_island_combined_result(workBuffers,
                                                                                   graph,
                                                                                   islandIndex,
                                                                                   contacts,
                                                                                   dt,
                                                                                   priorDistanceLambdas,
                                                                                   priorContactLambdas);
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

bool is_body_sleeping(u32 flags) {
    return (flags & RB_SLEEPING) != 0u;
}

bool is_body_static_or_kinematic(u32 flags) {
    return (flags & RB_STATIC) != 0u || (flags & RB_KINEMATIC) != 0u;
}

bool is_body_movable(const RigidBodySoA& bodies, u32 bodyIndex) {
    if (bodyIndex >= bodies.count()) {
        return false;
    }
    if (is_body_static_or_kinematic(bodies.flags[bodyIndex])) {
        return false;
    }
    if (is_body_sleeping(bodies.flags[bodyIndex])) {
        return false;
    }
    return bodies.invMasses[bodyIndex] > 0.f;
}

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
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
}

bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    return canSkipIslandBuild(bodyCount, contacts, distanceConstraints);
}

bool build_island_graph_guarded(ContactIslandGraph& graph,
                                u32 bodyCount,
                                const std::vector<narrowphase::ContactManifold>& contacts,
                                const std::vector<DistanceConstraint>& distanceConstraints) {
    return graph.buildGuarded(bodyCount, contacts, distanceConstraints);
}

bool canSkipIslandBuild(u32 bodyCount,
                        const std::vector<narrowphase::ContactManifold>& contacts,
                        const std::vector<DistanceConstraint>& distanceConstraints) {
    return canSkipContactIslandGraphBuild(bodyCount, contacts, distanceConstraints);
}

bool shouldRunIslandBuild(u32 bodyCount,
                          const std::vector<narrowphase::ContactManifold>& contacts,
                          const std::vector<DistanceConstraint>& distanceConstraints) {
    return shouldRunContactIslandGraphBuild(bodyCount, contacts, distanceConstraints);
}

IslandSolveBodiesPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
                                                         const RigidBodySoA& bodies) {
    IslandSolveBodiesPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        ++preflight.inRangeBodyCount;
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            ++preflight.staticOrKinematicCount;
        }
        if (is_body_sleeping(flags)) {
            ++preflight.sleepingCount;
        }
        if (is_body_movable(bodies, bodyIndex)) {
            ++preflight.movableCount;
        }
    }
    return preflight;
}

bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies) {
    return !preflight_island_solve_bodies(island, bodies).can_solve();
}

IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandConstraintSolvePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.refs = preflight_island_constraint_refs(island, contacts, distanceConstraints);
    preflight.bodies = preflight_island_solve_bodies(island, bodies);
    return preflight;
}

bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                         const RigidBodySoA& bodies,
                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                         const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_constraint_solve(island, bodies, contacts, distanceConstraints).can_solve();
}

IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies) {
    IslandSleepPreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            ++preflight.staticOrKinematicCount;
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++preflight.sleepingCount;
        } else {
            ++preflight.activeDynamicCount;
        }
    }

    preflight.allSleeping = preflight.activeDynamicCount == 0u && preflight.sleepingCount > 0u;
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

IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies) {
    IslandWakePreflight preflight{};
    if (!island_has_constraints(island)) {
        preflight.skipped = true;
        return preflight;
    }

    preflight.bodyCount = static_cast<u32>(island.bodyIndices.size());
    for (u32 bodyIndex : island.bodyIndices) {
        if (bodyIndex >= bodies.count()) {
            continue;
        }
        const u32 flags = bodies.flags[bodyIndex];
        if (is_body_static_or_kinematic(flags)) {
            continue;
        }
        if (is_body_sleeping(flags)) {
            ++preflight.sleepingCount;
        } else {
            ++preflight.activeDynamicCount;
        }
    }

    preflight.hasMixedSleepState =
        preflight.sleepingCount > 0u && preflight.activeDynamicCount > 0u;
    return preflight;
}

IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   const RigidBodySoA& bodies) {
    IslandWakePreflight preflight{};
    if (!island_index_valid(graph, islandIndex)) {
        preflight.skipped = true;
        return preflight;
    }
    return preflight_island_wake(graph.island(islandIndex), bodies);
}

IslandSleepGraphStats compute_island_sleep_stats(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies) {
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
        } else {
            ++stats.fullyActiveCount;
        }
    }
    return stats;
}

IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies) {
    IslandSleepGraphPreflight preflight{};
    preflight.stats = compute_island_sleep_stats(graph, bodies);
    preflight.skipped = !preflight.has_solveable_islands();
    return preflight;
}

bool should_skip_island_sleep_solve_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return preflight_island_sleep_graph(graph, bodies).skipped;
}

bool should_skip_island_sleep_solve(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies) {
    return preflight_island_sleep(island, bodies).can_skip_solve();
}

IslandWakeGraphStats compute_island_wake_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    IslandWakeGraphStats stats{};
    stats.totalIslands = graph.islandCount();
    for (u32 islandIndex = 0; islandIndex < stats.totalIslands; ++islandIndex) {
        const IslandWakePreflight preflight = preflight_island_wake(graph.island(islandIndex), bodies);
        if (preflight.skipped) {
            ++stats.emptyCount;
        } else if (preflight.should_wake_sleepers()) {
            ++stats.wakeableCount;
        }
    }
    return stats;
}

IslandWakeGraphPreflight preflight_island_wake_graph(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies) {
    IslandWakeGraphPreflight preflight{};
    preflight.stats = compute_island_wake_stats(graph, bodies);
    preflight.skipped = !preflight.can_wake();
    return preflight;
}

bool should_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    return !preflight_island_wake_graph(graph, bodies).can_wake();
}

bool should_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    return !preflight_island_wake(island, bodies).should_wake_sleepers();
}

std::vector<u32> collect_nonsleeping_island_indices(const ContactIslandGraph& graph,
                                                    const RigidBodySoA& bodies) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandSleepPreflight preflight = preflight_island_sleep(graph.island(islandIndex), bodies);
        if (preflight.skipped || preflight.can_skip_solve()) {
            continue;
        }
        indices.push_back(islandIndex);
    }
    return indices;
}

std::vector<u32> collect_wakeable_island_indices(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies) {
    std::vector<u32> indices;
    const u32 count = graph.islandCount();
    indices.reserve(count);
    for (u32 islandIndex = 0; islandIndex < count; ++islandIndex) {
        const IslandWakePreflight preflight = preflight_island_wake(graph.island(islandIndex), bodies);
        if (preflight.should_wake_sleepers()) {
            indices.push_back(islandIndex);
        }
    }
    return indices;
}

bool wake_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies);
    if (!preflight.should_wake_sleepers()) {
        return false;
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
    }
    return true;
}

bool wake_island_sleepers_by_index_guarded(RigidBodySoA& bodies,
                                           const ContactIslandGraph& graph,
                                           u32 islandIndex) {
    if (!island_index_valid(graph, islandIndex)) {
        return false;
    }
    return wake_island_sleepers_guarded(bodies, graph.island(islandIndex));
}

u32 wake_all_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph& graph) {
    u32 wokeCount = 0;
    for (u32 islandIndex : collect_wakeable_island_indices(graph, bodies)) {
        if (wake_island_sleepers_by_index_guarded(bodies, graph, islandIndex)) {
            ++wokeCount;
        }
    }
    return wokeCount;
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

} // namespace fuse::physics

// --- deepen additive from deepen-b4-pbd-island-solve-guards-848c ---
bool should_skip_island_solve(const IslandSolveJob& job) {
bool should_skip_all_island_solves(const ContactIslandGraph& graph) {
    if (should_skip_island_solve(job)) {
                                 IslandWarmStartPreflight& out) {
                                       const IslandWarmStartPreflight& preflight) {

// --- deepen additive from deepen-b4-pbd-island-preflight-warmstart-4254 ---
bool should_skip_frame_warm_start(const ContactIslandGraph& graph,
    if (should_skip_frame_warm_start(graph, priorDistanceLambdas, priorContactLambdas)) {

// --- deepen additive from deepen-b4-pbd-island-preflight-warmstart-8200 ---
    const IslandSolvePreflight preflight = preflight_island_solve(graph);

// --- deepen additive from deepen-b4-pbd-island-dispatch-warmstart-guards-e8ca ---
IslandContactImpulseWarmStartPreflight preflight_contact_impulse_warm_start_island(
IslandContactImpulseWarmStartPreflight preflight_contact_impulse_warm_start_island_by_index(
bool should_skip_contact_impulse_warm_start_island(const ContactIslandGraph::Island& island) {
    return should_skip_warm_start_island(island);
bool should_skip_contact_impulse_warm_start_island_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_contact_impulse_warm_start_island(graph.island(islandIndex));
IslandCombinedWarmStartPreflight preflight_warm_start_island_combined(

// --- deepen additive from deepen-b4-pbd-island-dispatch-warmstart-guards-c488 ---
bool should_skip_island_dispatch_job(const IslandSolveJob& job, f32 dt) {
    return !is_valid_island_solve_dt(dt) || should_skip_island_solve_job(job);
    if (should_skip_island_dispatch_job(job, dt)) {

// --- deepen additive from deepen-b4-pbd-island-guards-7571 ---
IslandContactImpulseWarmStartGraphPreflight preflight_contact_impulse_warm_start_graph(
IslandContactImpulseDispatchPreflight preflight_contact_impulse_dispatch(
    IslandContactImpulseDispatchPreflight preflight{};
bool should_skip_contact_impulse_warm_start_graph(
bool should_skip_contact_impulse_dispatch(const ContactIslandGraph& graph,
    const IslandContactImpulseDispatchPreflight preflight = preflight_contact_impulse_dispatch(graph, contacts, dt);
IslandCombinedWarmStartPreflight preflight_warm_start_island_combined_by_index(

// --- deepen additive from deepen-pbd-island-guards-6957 ---
IslandSolveJobPreflight preflight_island_solve_job(const ContactIslandGraph& graph,
IslandSolveJobPreflight preflight_island_solve_by_index(const ContactIslandGraph& graph,
bool should_skip_island_solve_invalid_indices(const IslandSolveJob& job,
bool should_skip_island_solve_index(const ContactIslandGraph& graph,
    const IslandSolveJobPreflight preflight =
IslandContactImpulseWarmStartPreflight preflight_warm_start_island_contact_impulses(
IslandContactImpulseWarmStartPreflight preflight_warm_start_island_contact_impulses_by_index(
    const IslandContactImpulseWarmStartGraphPreflight preflight =

// --- deepen additive from pbd-island-guards-deepen-1f2e ---
IslandSolveInputsPreflight preflight_island_solve_inputs(const ContactIslandGraph::Island& island,
    IslandSolveInputsPreflight preflight{};
IslandSolveInputsPreflight preflight_island_solve_inputs_by_index(const ContactIslandGraph& graph,
bool should_skip_island_solve_inputs(const ContactIslandGraph::Island& island,
IslandContactImpulsePreflight preflight_warm_start_contact_impulses(
    IslandContactImpulsePreflight preflight{};
IslandContactImpulsePreflight preflight_warm_start_contact_impulses_by_index(
bool should_skip_warm_start_island_combined(const ContactIslandGraph::Island& island,
        const IslandContactImpulsePreflight preflight =
    const IslandContactImpulsePreflight preflight = preflight_warm_start_contact_impulses(island, contacts, dt);

// --- deepen additive from deepen-pbd-island-guards-426b ---
IslandJobDispatchPreflight preflight_dispatch_island_index(const ContactIslandGraph& graph,
    IslandJobDispatchPreflight preflight{};
bool should_skip_dispatch_island_index(const ContactIslandGraph& graph, u32 islandIndex, f32 dt) {
    const IslandJobDispatchPreflight preflight = preflight_dispatch_island_index(graph, islandIndex, dt);
IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses(
IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses_by_index(
IslandContactImpulseGraphPreflight preflight_warm_start_contact_impulses_graph(
    IslandContactImpulseGraphPreflight preflight{};
bool should_skip_contact_impulse_warm_start_graph(const ContactIslandGraph& graph,
    const IslandContactImpulseGraphPreflight preflight =

// --- deepen additive from deepen-b4-pbd-island-solver-ac66 ---
IslandSolveJobPreflight preflight_island_solve_job(const IslandSolveJob& job) {
IslandContactImpulsePreflight preflight_island_contact_impulses(
IslandContactImpulsePreflight preflight_island_contact_impulses_by_index(
IslandContactImpulseGraphPreflight preflight_contact_impulse_graph(
bool should_skip_contact_impulse_graph(const ContactIslandGraph& graph,
bool should_skip_contact_impulse_island(const ContactIslandGraph::Island& island,
bool should_skip_contact_impulse_island_index(const ContactIslandGraph& graph,
    const IslandContactImpulseGraphPreflight preflight = preflight_contact_impulse_graph(graph, contacts, dt);

// --- deepen additive from deepen-pbd-island-guards-f8cf ---
IslandDispatchJobPreflight preflight_dispatch_island_job(const IslandSolveJob& job, f32 dt) {
    IslandDispatchJobPreflight preflight{};
    preflight.emptyJob = should_skip_island_solve_job(job);
bool should_skip_dispatch_island_job(const IslandSolveJob& job, f32 dt) {
    const IslandDispatchJobPreflight preflight = preflight_dispatch_island_job(job, dt);
bool should_skip_warm_start_contact_impulses_island(const ContactIslandGraph::Island& island) {
bool should_skip_warm_start_contact_impulses_island_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_warm_start_island_index(graph, islandIndex);

// --- deepen additive from deepen-pbd-island-guards-b61c ---
IslandDispatchJobPreflight preflight_island_dispatch_from_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt) {
bool should_skip_island_dispatch_from_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt) {
IslandCombinedWarmStartPreflight preflight_warm_start_combined_island_by_index(
bool should_skip_warm_start_combined_island_index(const ContactIslandGraph& graph,
IslandCombinedWarmStartGraphPreflight preflight_warm_start_combined_graph(
    IslandCombinedWarmStartGraphPreflight preflight{};
bool should_skip_warm_start_combined_graph(const ContactIslandGraph& graph,
    const IslandCombinedWarmStartGraphPreflight preflight =

// --- deepen additive from pbd-island-guards-deepen-0fe3 ---
IslandSolveJobPreflight preflight_solve_island_job_by_index(const ContactIslandGraph& graph,
bool should_skip_solve_island_job_stale(const IslandSolveJob& job,
    const IslandSolveJobPreflight preflight = preflight_solve_island_job(job, contacts, distanceConstraints);
IslandDispatchIndexPreflight preflight_dispatch_island_by_index(const ContactIslandGraph& graph,
    IslandDispatchIndexPreflight preflight{};

// --- deepen additive from deepen-pbd-island-guards-faad ---
SleepPassPreflight preflight_sleep_pass(const RigidBodySoA& bodies,
    SleepPassPreflight preflight{};
bool should_skip_sleep_pass(const RigidBodySoA& bodies,
WakePreflight preflight_wake_candidates(const RigidBodySoA& bodies,
bool should_skip_island_solve_all_inactive(const RigidBodySoA& bodies,
IslandSolveWorkPreflight preflight_island_solve_work(const RigidBodySoA& bodies,
    IslandSolveWorkPreflight preflight{};
IslandConstraintIndexPreflight preflight_island_constraint_indices(
    IslandConstraintIndexPreflight preflight{};
IslandConstraintIndexPreflight preflight_island_constraint_indices_by_index(

// --- deepen additive from deepen-pbd-island-guards-88d5 ---
IslandDispatchJobBatchPreflight preflight_dispatchable_island_jobs(const ContactIslandGraph& graph,
    IslandDispatchJobBatchPreflight preflight{};
bool should_skip_dispatchable_island_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt) {
    if (should_skip_dispatchable_island_jobs(jobs, dt)) {

// --- deepen additive from pbd-island-sleep-build-preflights-cb2c ---
IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
IslandSleepPreflight preflight_island_sleep_by_index(const RigidBodySoA& bodies,
IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
IslandWakePreflight preflight_island_wake_by_index(const RigidBodySoA& bodies,
        const IslandSleepPreflight sleepPreflight = preflight_island_sleep(bodies, island);
        if (sleepPreflight.can_skip_solve()) {
bool should_skip_island_solve_for_sleep(const RigidBodySoA& bodies,
bool should_skip_island_solve_for_sleep_index(const RigidBodySoA& bodies,
    return should_skip_island_solve_for_sleep(bodies, graph.island(islandIndex));
bool should_skip_island_solve_job_for_sleep(const RigidBodySoA& bodies, const IslandSolveJob& job) {
    return should_skip_island_solve_for_sleep(bodies, *job.island);
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
IslandSolveSleepPreflight preflight_island_solve_sleep(const RigidBodySoA& bodies,
    IslandSolveSleepPreflight preflight{};
IslandSolveSleepPreflight preflight_island_solve_sleep_by_index(const RigidBodySoA& bodies,
IslandDispatchSleepPreflight preflight_island_dispatch_sleep(const RigidBodySoA& bodies,
    IslandDispatchSleepPreflight preflight{};
        if (should_skip_island_solve_for_sleep(bodies, *job.island)) {
bool should_skip_island_dispatch_sleep(const RigidBodySoA& bodies,
    const IslandSolveSleepPreflight preflight = preflight_island_solve_sleep_by_index(
    const IslandDispatchSleepPreflight preflight = preflight_island_dispatch_sleep(bodies, graph, dt);
        } else if (should_skip_island_solve_for_sleep_index(bodies, graph, islandIndex)) {

// --- deepen additive from deepen-pbd-island-guards-a261 ---
bool should_skip_solve_island_all_sleeping(const ContactIslandGraph::Island& island,
    const IslandSleepPreflight sleepPreflight = preflight_island_sleep_state(island, bodies);
    return !sleepPreflight.skipped && sleepPreflight.all_dynamic_sleeping();
bool should_skip_solve_island_all_static(const ContactIslandGraph::Island& island,
    return !sleepPreflight.skipped && sleepPreflight.all_static();
bool should_skip_solve_island_job_preflight(const IslandSolveJobPreflight& preflight) {
IslandSleepPreflight preflight_island_sleep_state(const ContactIslandGraph::Island& island,
IslandSleepPreflight preflight_island_sleep_state_by_index(const ContactIslandGraph& graph,
bool should_skip_sleep_detection_for_body(const RigidBodySoA& bodies, u32 bodyIndex) {
IslandSleepWakePreflight preflight_island_sleep_wake(
    IslandSleepWakePreflight preflight{};
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(

// --- deepen additive from deepen-pbd-island-sleep-build-guards-9e33 ---
                                        IslandSleepPreflight& preflight) {
    const IslandSleepPreflight preflight = preflight_island_sleep(island, bodies);
bool should_skip_awake_island_dispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
bool should_skip_sleeping_island_solve_index(const ContactIslandGraph& graph,
    return should_skip_sleeping_island_solve(graph.island(islandIndex), bodies);
    if (should_skip_island_wake(island)) {
bool should_skip_island_wake(const ContactIslandGraph::Island& island) {
    const IslandWakePreflight preflight = preflight_island_wake(island, bodies, contacts);
IslandConstraintSolvePreflight preflight_solve_island(const ContactIslandGraph::Island& island,
IslandConstraintSolvePreflight preflight_solve_island_by_index(const ContactIslandGraph& graph,
bool should_skip_solve_island_preflight(const ContactIslandGraph::Island& island,
    if (should_skip_solve_island_preflight(island, bodies, distanceConstraints, workBuffers, dt)) {
    if (should_skip_sleeping_island_solve(*job.island, bodies)) {

// --- deepen additive from pbd-island-guards-deepen-5934 ---
IslandSleepPreflight preflight_sleeping_island(const ContactIslandGraph::Island& island,
IslandSleepPreflight preflight_sleeping_island_by_index(const ContactIslandGraph& graph,
    const IslandSleepPreflight preflight = preflight_sleeping_island(island, bodies);
bool should_skip_sleeping_island_solve_job(const IslandSolveJob& job,
    if (should_skip_island_solve_job(job)) {
    return should_skip_sleeping_island_solve(*job.island, bodies);
WakeOnImpulsePreflight preflight_wake_on_impulse(const RigidBodySoA& bodies,
    WakeOnImpulsePreflight preflight{};
IslandBodyPartitionPreflight preflight_island_body_partition(const ContactIslandGraph& graph) {
    IslandBodyPartitionPreflight preflight{};
ConstraintIterationPreflight preflight_constraint_iterations(const SolverParams& params) {
    ConstraintIterationPreflight preflight{};
bool should_skip_constraint_iterations(const SolverParams& params) {
    if (should_skip_sleeping_island_solve_job(job, bodies)) {

// --- deepen additive from deepen-pbd-island-guards-5425 ---
IslandSleepSolvePreflight preflight_island_sleep_solve(const ContactIslandGraph::Island& island,
    IslandSleepSolvePreflight preflight{};
IslandSleepSolvePreflight preflight_island_sleep_solve_by_index(const ContactIslandGraph& graph,
    const IslandSleepSolvePreflight preflight = preflight_island_sleep_solve(island, bodies);
bool should_skip_island_sleep_detection(const ContactIslandGraph::Island& island,
bool should_skip_island_sleep_detection_index(const ContactIslandGraph& graph,
    return should_skip_island_sleep_detection(graph.island(islandIndex), bodies);
IslandSolveJobPreflight preflight_solve_island_job(const ContactIslandGraph::Island& island,
bool should_skip_solve_island_job_preflight(const ContactIslandGraph::Island& island,
    const IslandSolveJobPreflight preflight = preflight_solve_island_job(island, bodies, dt);

// --- deepen additive from deepen-pbd-island-guards-ac8e ---
bool should_skip_solve_sleeping_island(const RigidBodySoA& bodies,
bool should_skip_solve_sleeping_island_index(const RigidBodySoA& bodies,
    return should_skip_solve_sleeping_island(bodies, graph.island(islandIndex));
IslandConstraintSolvePreflight preflight_island_constraint_solve(const RigidBodySoA& bodies,
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(const RigidBodySoA& bodies,
bool should_skip_island_constraint_solve(const RigidBodySoA& bodies,
IslandSleepGraphPreflight preflight_island_sleep_graph(const RigidBodySoA& bodies,
        const IslandSleepPreflight islandPreflight = preflight_island_sleep(bodies, island);
        if (islandPreflight.fullySleeping) {
        } else if (islandPreflight.can_solve()) {
bool should_skip_island_sleep_dispatch(const RigidBodySoA& bodies, const ContactIslandGraph& graph) {
        if (!should_skip_solve_sleeping_island(bodies, island)) {
    const IslandConstraintSolvePreflight preflight = preflight_island_constraint_solve(bodies, island, dt);

// --- deepen additive from deepen-pbd-island-build-sleep-wake-cc0e ---
    const IslandSleepPreflight preflight = preflight_island_sleep(bodies, island);
        const IslandSleepPreflight preflight = preflight_island_sleep(bodies, graph.island(islandIndex));
bool should_skip_island_solve_sleeping(const RigidBodySoA& bodies,
bool should_skip_island_solve_sleeping_index(const ContactIslandGraph& graph,
    return should_skip_island_solve_sleeping(bodies, graph.island(islandIndex));
        if (should_skip_island_solve_sleeping(bodies, *job.island)) {
    if (should_skip_island_wake_check(island)) {
bool should_skip_island_wake_check(const ContactIslandGraph::Island& island) {
    if (should_skip_island_solve_sleeping(bodies, island)) {

// --- deepen additive from deepen-pbd-island-guards-2fdd ---
bool should_skip_island_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    const IslandWakePreflight preflight = preflight_island_wake(bodies, island);
    return !should_skip_island_wake(bodies, island);

// --- deepen additive from deepen-pbd-island-guards-fd7c ---
IslandSolveBodyPreflight preflight_island_solve_bodies(const RigidBodySoA& bodies,
    IslandSolveBodyPreflight preflight{};
IslandSolveBodyPreflight preflight_island_solve_bodies_by_index(const RigidBodySoA& bodies,
bool should_skip_island_solve_job_for_sleep(const IslandSolveJob& job,
bool should_skip_island_sleep(const ContactIslandGraph::Island& island, f32 dt) {
    return !is_valid_island_solve_dt(dt) || should_skip_warm_start_island(island);
bool should_skip_island_sleep_index(const ContactIslandGraph& graph, u32 islandIndex, f32 dt) {
    return should_skip_island_sleep(graph.island(islandIndex), dt);
bool should_skip_island_wake_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_island_wake(graph.island(islandIndex));
IslandWakeGraphPreflight preflight_island_wake_graph(const RigidBodySoA& bodies,
    const IslandSleepPreflight preflight = preflight_island_sleep(bodies, island, params, dt);
    const IslandWakePreflight preflight = preflight_island_wake(bodies, island, IslandSleepParams{});
    const IslandSleepGraphPreflight preflight = preflight_island_sleep_graph(bodies, graph, params, dt);
    const IslandWakeGraphPreflight preflight = preflight_island_wake_graph(bodies, graph, params);

// --- deepen additive from deepen-pbd-island-guards-bda2 ---
bool should_skip_island_constraint_solve_index(const RigidBodySoA& bodies,
    return should_skip_island_constraint_solve(bodies, graph.island(islandIndex), dt);
bool should_skip_island_sleep_check(const ContactIslandGraph::Island& island) {
bool should_skip_island_sleep_index(const ContactIslandGraph& graph, u32 islandIndex) {
    return should_skip_island_sleep_check(graph.island(islandIndex));
IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island) {
    return should_skip_island_wake_check(graph.island(islandIndex));
        if (sleepPreflight.can_consider_sleep()) {
        if (sleepPreflight.all_dynamic_sleeping()) {
        const IslandWakePreflight wakePreflight = preflight_island_wake(bodies, island);
        if (wakePreflight.should_wake()) {

// --- deepen additive from pbd-island-guards-f0a5 ---
bool should_skip_island_solve_sleeping(const ContactIslandGraph::Island& island,
IslandSolveJobSleepPreflight preflight_solve_island_job_with_sleep(const IslandSolveJob& job,
    IslandSolveJobSleepPreflight preflight{};
bool should_skip_solve_island_job_with_sleep(const IslandSolveJob& job,
IslandSleepDispatchPreflight preflight_island_dispatch_with_sleep(const ContactIslandGraph& graph,
    IslandSleepDispatchPreflight preflight{};
bool should_skip_island_dispatch_with_sleep(const ContactIslandGraph& graph,
    if (should_skip_island_solve_sleeping(island, bodies)) {

// --- deepen additive from pbd-island-guards-deepen-bcee ---
IslandSleepPreflight preflight_island_sleep_for_solve(const ContactIslandGraph::Island& island,
IslandSleepPreflight preflight_island_sleep_for_solve_by_index(const ContactIslandGraph& graph,
        const IslandSleepPreflight preflight = preflight_island_sleep_for_solve(island, bodies);
bool should_skip_island_solve_for_sleep(const ContactIslandGraph& graph,
bool should_skip_solve_sleeping_island(const ContactIslandGraph::Island& island,
bool should_skip_island_wake_check(const ContactIslandGraph::Island& island,
IslandSolveCombinedPreflight preflight_island_solve_combined(
    IslandSolveCombinedPreflight preflight{};
bool should_skip_island_solve_combined(const IslandSolveJob& job,
    const IslandSolveCombinedPreflight preflight = preflight_island_solve_combined(

// --- deepen additive from deepen-pbd-island-guards-3045 ---
IslandBodyRefsPreflight preflight_island_body_refs(const ContactIslandGraph::Island& island,
    IslandBodyRefsPreflight preflight{};
bool should_skip_island_body_refs(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    const IslandSleepPreflight preflight = preflight_island_sleep_state(island, bodies);
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
    if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints, dt)) {

// --- deepen additive from pbd-island-sleep-wake-guards-28a6 ---
bool should_skip_island_solve_all_sleeping(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    const IslandSleepGraphPreflight preflight = preflight_island_sleep_graph(graph, bodies);
bool should_skip_sleeping_island(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
bool should_skip_sleeping_island_index(const ContactIslandGraph& graph,
    const IslandSleepPreflight preflight = preflight_island_sleep_by_index(graph, islandIndex, bodies);
        if (!should_skip_sleeping_island(*job.island, bodies)) {
    if (should_skip_sleeping_island(*job.island, bodies)) {

// --- deepen additive from deepen-pbd-island-guards-6182 ---
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
        const IslandSleepWakePreflight preflight = preflight_island_sleep_wake(graph.island(islandIndex), bodies);
IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,
    IslandSleepWakeGraphPreflight preflight{};
bool should_skip_island_solve_sleep_wake(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
    if (should_skip_island_constraint_solve(island, bodies, contacts, distanceConstraints)) {

// --- deepen additive from deepen-pbd-island-guards-a489 ---
IslandSolveBodyRefsPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
    IslandSolveBodyRefsPreflight preflight{};
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
bool should_skip_island_solve_for_sleep(const ContactIslandGraph::Island& island,
bool should_skip_island_solve_for_sleep_index(const ContactIslandGraph& graph,
    return should_skip_island_solve_for_sleep(graph.island(islandIndex), bodies);
bool should_skip_island_sleep_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
bool should_skip_island_constraint_solve(const IslandSolveJob& job,

// --- deepen additive from pbd-island-guards-deepen-77cf ---
IslandSolveRefsPreflight preflight_island_solve_refs(
    IslandSolveRefsPreflight preflight{};
bool should_skip_island_solve_refs(const ContactIslandGraph::Island& island,
        const IslandWakePreflight islandPreflight =
        if (islandPreflight.should_wake()) {
    if (should_skip_island_body_refs(island, bodies)) {
    if (should_skip_island_solve_for_sleep(island, bodies)) {

// --- deepen additive from deepen-pbd-island-guards-2105 ---
IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
        const IslandSleepPreflight islandSleep = preflight_island_sleep(island, bodies);
        if (!should_skip_sleeping_island_solve(island, bodies)) {
IslandSolvePreflightCombined preflight_island_solve_combined(
    IslandSolvePreflightCombined preflight{};
    if (should_skip_sleeping_island_solve(island, bodies) ||
        should_skip_island_solve_bodies(island, bodies)) {

// --- deepen additive from deepen-pbd-island-guards-358e ---
        const IslandSleepPreflight sleepPreflight = preflight_island_sleep(island, bodies);
        const IslandWakePreflight wakePreflight = preflight_island_wake(island, bodies);
bool should_skip_island_solve_all_sleeping(const ContactIslandGraph::Island& island,
IslandSolveParticipationPreflight preflight_island_solve_participation(
    IslandSolveParticipationPreflight preflight{};
bool should_skip_island_solve_no_participation(const ContactIslandGraph::Island& island,

// --- deepen additive from deepen-pbd-island-guards-a375 ---
    const IslandSleepPreflight sleep = preflight_island_sleep_state(island, bodies);
bool should_skip_island_dispatch_for_sleep(const ContactIslandGraph& graph,
IslandSolvePassPreflight preflight_island_solve_pass(
    IslandSolvePassPreflight preflight{};
bool should_skip_island_solve_pass(const ContactIslandGraph::Island& island,
    if (should_skip_island_solve_pass(island, bodies, contacts, distanceConstraints)) {
    if (should_skip_solve_sleeping_island(*job.island, bodies)) {

// --- deepen additive from deepen-pbd-island-guards-9b4f ---
bool should_skip_solve_fully_sleeping_island(const RigidBodySoA& bodies,
IslandSolveSleepPreflight preflight_solve_island_with_sleep(
bool should_skip_solve_island_with_sleep(const RigidBodySoA& bodies,
    if (should_skip_solve_island_with_sleep(bodies, island, contacts, distanceConstraints)) {

// --- deepen additive from pbd-island-sleep-wake-guards-c801 ---
        const IslandSleepWakePreflight preflight = preflight_island_sleep_wake(island, bodies);
bool should_skip_sleeping_island_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {

// --- deepen additive from deepen-pbd-island-guards-12c0 ---
        const IslandSleepSolvePreflight sleepPreflight = preflight_island_sleep_solve(island, bodies);
        if (sleepPreflight.awakeBodyCount > 0u) {
        } else if (sleepPreflight.dynamicBodyCount > 0u) {
bool should_skip_solve_all_sleeping_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {
bool should_skip_wake_island(const ContactIslandGraph::Island& island,
IslandSolveBodyPreflight preflight_solve_island_with_bodies(
bool should_skip_solve_island_with_bodies(const ContactIslandGraph::Island& island,
    if (should_skip_solve_sleeping_island(island, bodies)) {

// --- deepen additive from pbd-island-sleep-wake-preflights-1600 ---
bool should_skip_inactive_island_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies) {
bool should_skip_inactive_island_solve_index(const ContactIslandGraph& graph,
    return should_skip_inactive_island_solve(graph.island(islandIndex), bodies);
IslandConstraintRefsPreflight preflight_island_solvable_constraint_refs(
    IslandConstraintRefsPreflight preflight = preflight_island_constraint_refs(island, contacts, distanceConstraints);
bool should_skip_island_solvable_constraint_refs(const ContactIslandGraph::Island& island,
IslandDispatchBodiesPreflight preflight_island_dispatch_with_bodies(const ContactIslandGraph& graph,
    IslandDispatchBodiesPreflight preflight{};
bool should_skip_island_dispatch_with_bodies(const ContactIslandGraph& graph,

// --- deepen additive from deepen-pbd-island-sleep-build-guards-e836 ---
bool should_skip_island_dispatch_for_sleep(const ContactIslandGraph& graph, const RigidBodySoA& bodies) {

// --- deepen additive from deepen-pbd-island-guards-9f8d ---
    if (should_skip_island_build(bodyCount, contacts, distanceConstraints)) {
bool should_skip_island_constraint_solve_by_index(const ContactIslandGraph& graph,
            const IslandSleepPreflight sleepPreflight = preflight_island_sleep(graph.island(islandIndex), bodies);
IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(
    IslandConstraintSolveGraphPreflight preflight{};
bool should_skip_island_constraint_solve_graph(
IslandSolvePipelinePreflight preflight_island_solve_pipeline(
    IslandSolvePipelinePreflight preflight{};
IslandSolvePipelinePreflight preflight_island_solve_pipeline_by_index(
bool should_skip_island_solve_pipeline(const ContactIslandGraph::Island& island,
    const IslandSolvePipelinePreflight preflight =
    const IslandDispatchPreflight dispatchPreflight = preflight_island_dispatch(graph, dt);
    result.dispatchableCount = dispatchPreflight.solve.stats.dispatchableCount;
    if (!dispatchPreflight.can_dispatch()) {
