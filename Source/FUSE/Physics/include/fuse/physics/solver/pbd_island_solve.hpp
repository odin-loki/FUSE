#pragma once

#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/solver/contact_island_graph.hpp>
#include <fuse/physics/solver/distance_constraint.hpp>
#include <fuse/physics/solver/solver_work_buffers.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <vector>

namespace fuse::physics {

/// Lightweight view for parallel island dispatch (B4.4 deepen).
struct IslandSolveJob {
    u32 islandIndex = ContactIslandGraph::invalidIsland;
    u32 constraintCount = 0;
    bool empty = true;
    const ContactIslandGraph::Island* island = nullptr;
};

/// Aggregate counts for parallel dispatch sizing and empty-island early-out stubs.
struct IslandSolveStats {
    u32 totalIslands = 0;
    u32 constrainedCount = 0;
    u32 emptyCount = 0;
    u32 dispatchableCount = 0;
};

/// Preflight diagnostics for island solve dispatch (B4.4 deepen).
struct IslandSolvePreflight {
    IslandSolveStats stats{};
    bool skipped = false;

    bool can_dispatch() const { return !skipped && stats.dispatchableCount > 0u; }
};

/// Per-island dispatch outcome (skip vs solve) for parallel batch stubs.
struct IslandDispatchResult {
    bool solved = false;
    bool skipped = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
};

/// Per-island warm-start outcome (skip vs seed) for parallel batch stubs.
struct IslandWarmStartResult {
    bool warmed = false;
    bool skipped = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;
};

/// Aggregate warm-start counts for graph-level batch guards.
struct IslandWarmStartStats {
    u32 totalIslands = 0;
    u32 warmStartableCount = 0;
    u32 emptyCount = 0;
    u32 noPriorDataCount = 0;
};

/// Graph-level warm-start preflight for selective per-island seeding.
struct IslandWarmStartGraphPreflight {
    IslandWarmStartStats stats{};
    bool skipped = false;

    bool can_warm_start() const { return !skipped && stats.warmStartableCount > 0u; }
};

/// Warm-start preflight for per-island lambda seeding (parallel-safe selective seed).
struct IslandWarmStartPreflight {
    u32 ownedDistanceCount = 0;
    u32 ownedContactCount = 0;
    u32 priorDistanceCoverage = 0;
    u32 priorContactCoverage = 0;
    bool skipped = false;

    bool can_warm_start() const {
        return !skipped && (priorDistanceCoverage > 0u || priorContactCoverage > 0u);
    }
};

/// Frame-level warm-start preflight for first-substep lambda seeding.
struct FrameWarmStartPreflight {
    u32 distanceSlotCount = 0;
    u32 contactSlotCount = 0;
    u32 priorDistanceCoverage = 0;
    u32 priorContactCoverage = 0;
    bool skipped = false;

    bool can_warm_start() const {
        return !skipped && (priorDistanceCoverage > 0u || priorContactCoverage > 0u);
    }
};

/// Combined dispatch preflight (graph + timestep guard).
struct IslandDispatchPreflight {
    IslandSolvePreflight solve{};
    bool invalidDt = false;
    bool skipped = false;

    bool can_dispatch() const { return !skipped && !invalidDt && solve.can_dispatch(); }
};

/// Batch dispatch summary for parallel iteration stubs.
struct IslandBatchDispatchResult {
    u32 solvedCount = 0;
    u32 skippedCount = 0;
    u32 dispatchableCount = 0;
    bool skipped = false;

    bool any_solved() const { return solvedCount > 0u; }
};

/// Batch warm-start summary for parallel iteration stubs.
struct IslandBatchWarmStartResult {
    u32 warmedCount = 0;
    u32 skippedCount = 0;
    u32 warmStartableCount = 0;
    bool skipped = false;

    bool any_warmed() const { return warmedCount > 0u; }
};

/// Per-island contact-impulse warm-start preflight (dt + empty-island guards).
struct IslandContactImpulseWarmStartPreflight {
    u32 ownedContactCount = 0;
    u32 nonZeroImpulseCount = 0;
    bool invalidDt = false;
    bool skipped = false;

    bool can_warm_start() const {
        return !skipped && !invalidDt && ownedContactCount > 0u && nonZeroImpulseCount > 0u;
    }
};

/// Combined lambda + contact-impulse warm-start preflight for one island.
struct IslandCombinedWarmStartPreflight {
    IslandWarmStartPreflight lambdas{};
    IslandContactImpulseWarmStartPreflight impulses{};
    bool skipped = false;

    bool can_warm_start() const {
        return !skipped && (lambdas.can_warm_start() || impulses.can_warm_start());
    }
};

/// Per-job island dispatch preflight (dt + empty/null job guards).
struct IslandSolveJobPreflight {
    bool invalidDt = false;
    bool skipped = false;
    u32 constraintCount = 0;

    bool can_dispatch() const { return !skipped && !invalidDt && constraintCount > 0u; }
};

/// Constraint index coverage for one island solve pass (stale/out-of-range guards).
struct IslandConstraintRefsPreflight {
    u32 ownedContactCount = 0;
    u32 ownedDistanceCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 validContactCount = 0;
    bool skipped = false;

    bool can_solve() const {
        return !skipped && (inRangeContactCount > 0u || inRangeDistanceCount > 0u);
    }
};

/// Body index coverage for one island solve pass (stale/out-of-range guards).
struct IslandBodyRefsPreflight {
    u32 ownedBodyCount = 0;
    u32 inRangeBodyCount = 0;
    u32 dynamicBodyCount = 0;
    bool skipped = false;

    bool can_solve() const {
        return !skipped && ownedBodyCount > 0u && inRangeBodyCount == ownedBodyCount;
    }
};

/// Combined constraint + body coverage for one island solve pass.
struct IslandSolveRefsPreflight {
    IslandConstraintRefsPreflight constraints{};
    IslandBodyRefsPreflight bodies{};
    bool skipped = false;

    bool can_solve() const { return !skipped && constraints.can_solve() && bodies.can_solve(); }
};

/// Per-island sleep aggregation for selective solve skip (B4.4 deepen follow-up).
struct IslandSleepPreflight {
    u32 ownedBodyCount = 0;
    u32 sleepingCount = 0;
    u32 staticOrKinematicCount = 0;
    u32 dynamicAwakeCount = 0;
    bool skipped = false;

    bool all_dynamic_sleeping() const {
        return !skipped && dynamicAwakeCount == 0u && ownedBodyCount > staticOrKinematicCount;
    }

    bool can_skip_solve() const { return all_dynamic_sleeping(); }
};

/// Per-island wake candidate scan for sleep/wake preflight (B4.4 deepen follow-up).
struct IslandWakePreflight {
    u32 ownedBodyCount = 0;
    u32 wakeCandidateCount = 0;
    u32 externalForceCount = 0;
    u32 penetratingContactCount = 0;
    bool skipped = false;

    bool should_wake() const {
        return !skipped &&
               (wakeCandidateCount > 0u || externalForceCount > 0u || penetratingContactCount > 0u);
    }
};

/// Graph-level sleep aggregation for batch solve skip guards.
struct IslandSleepGraphStats {
    u32 totalIslands = 0;
    u32 fullySleepingCount = 0;
    u32 partiallyAwakeCount = 0;
    u32 emptyCount = 0;
};

/// Graph-level sleep preflight for selective island solve skip.
struct IslandSleepGraphPreflight {
    IslandSleepGraphStats stats{};
    bool skipped = false;

    bool all_fully_sleeping() const {
        return !skipped && stats.fullySleepingCount == stats.totalIslands && stats.totalIslands > 0u;
    }

    bool can_skip_all() const { return all_fully_sleeping(); }
};

/// Graph-level wake preflight for batch wake guards.
struct IslandWakeGraphPreflight {
    u32 wakeableIslandCount = 0;
    u32 totalIslands = 0;
    bool skipped = false;

    bool any_should_wake() const { return !skipped && wakeableIslandCount > 0u; }
};

/// Aggregate contact-impulse warm-start counts for graph-level batch guards.
struct IslandContactImpulseWarmStartStats {
    u32 totalIslands = 0;
    u32 warmStartableCount = 0;
    u32 emptyCount = 0;
    u32 noImpulseCount = 0;
};

/// Graph-level contact-impulse warm-start preflight for selective per-island seeding.
struct IslandContactImpulseWarmStartGraphPreflight {
    IslandContactImpulseWarmStartStats stats{};
    bool invalidDt = false;
    bool skipped = false;

    bool can_warm_start() const { return !skipped && !invalidDt && stats.warmStartableCount > 0u; }
};

/// True when `islandIndex` is in range for `extract_island`.
bool island_index_valid(const ContactIslandGraph& graph, u32 islandIndex);

/// Returns true when the island carries at least one contact or distance constraint.
bool island_has_constraints(const ContactIslandGraph::Island& island);

/// Contact + distance constraint count for dispatch sizing stubs.
u32 island_constraint_count(const ContactIslandGraph::Island& island);

/// True when a job should run `solve_island_job` (non-empty, in-range, bound island).
bool should_solve_island(const IslandSolveJob& job);

/// Inverse of `should_solve_island` — empty, null-island, or zero-constraint fast path.
bool should_skip_island_solve_job(const IslandSolveJob& job);

/// True when `islandIndex` extracts a dispatchable solve job.
bool should_dispatch_island_index(const ContactIslandGraph& graph, u32 islandIndex);

/// Extract one island solve job; out-of-range and empty islands are flagged for early skip.
IslandSolveJob extract_island(const ContactIslandGraph& graph, u32 islandIndex);

/// Batch extract all island jobs (parallel dispatch prep stub).
std::vector<IslandSolveJob> extract_island_jobs(const ContactIslandGraph& graph);

/// Summarize constrained vs empty islands for dispatch prep and early-out guards.
IslandSolveStats compute_island_solve_stats(const ContactIslandGraph& graph);

/// Count islands that pass `should_solve_island` (non-empty, bound, in-range).
u32 count_dispatchable_islands(const ContactIslandGraph& graph);

/// True when at least one island would be dispatched this substep.
bool has_dispatchable_islands(const ContactIslandGraph& graph);

/// True when every island is constraint-free (all empty).
bool all_islands_empty(const ContactIslandGraph& graph);

/// True when dt is positive for island constraint solve passes.
bool is_valid_island_solve_dt(f32 dt);

/// True when dt is positive for contact-impulse warm-start seeding.
bool is_valid_warm_start_dt(f32 dt);

/// True when dt is positive and finite for island constraint solve passes.
bool is_finite_island_solve_dt(f32 dt);

/// True when dt is positive and finite for contact-impulse warm-start seeding.
bool is_finite_warm_start_dt(f32 dt);

/// Preflight one extracted island job including timestep validity.
IslandSolveJobPreflight preflight_solve_island_job(const IslandSolveJob& job, f32 dt);

/// Early-out guard for job-level island dispatch (empty/null job or invalid dt).
bool should_skip_solve_island_job(const IslandSolveJob& job, f32 dt);

/// Preflight constraint index coverage for one island; sets `skipped` for empty islands.
IslandConstraintRefsPreflight preflight_island_constraint_refs(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when an island has no in-range constraint references to solve.
bool should_skip_island_constraint_refs(const ContactIslandGraph::Island& island,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight body index coverage for one island; sets `skipped` for empty islands.
IslandBodyRefsPreflight preflight_island_body_refs(const ContactIslandGraph::Island& island,
                                                   const RigidBodySoA& bodies);

/// Early-out guard when an island has no in-range body references to solve.
bool should_skip_island_body_refs(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Combined constraint + body preflight for one island solve pass.
IslandSolveRefsPreflight preflight_island_solve_refs(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when combined refs preflight rejects the island.
bool should_skip_island_solve_refs(const ContactIslandGraph::Island& island,
                                   const RigidBodySoA& bodies,
                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                   const std::vector<DistanceConstraint>& distanceConstraints);

/// True when a body carries `RB_SLEEPING`.
bool is_sleeping_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when a body carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_static_or_kinematic_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when a body is dynamic and not sleeping.
bool is_dynamic_awake_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when a dynamic body exceeds linear/angular speed thresholds.
bool body_exceeds_wake_threshold(const RigidBodySoA& bodies,
                                 u32 bodyIndex,
                                 f32 linearThreshold,
                                 f32 angularThreshold);

/// Preflight sleep aggregation for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies);

/// Preflight sleep by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
                                                     u32 islandIndex,
                                                     const RigidBodySoA& bodies);

/// Preflight wake candidates for one island; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies,
                                          const std::vector<narrowphase::ContactManifold>& contacts,
                                          f32 linearThreshold,
                                          f32 angularThreshold);

/// Preflight wake by island index; out-of-range indices are marked skipped.
IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   const RigidBodySoA& bodies,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   f32 linearThreshold,
                                                   f32 angularThreshold);

/// Summarize fully-sleeping vs partially-awake islands for batch guards.
IslandSleepGraphStats compute_island_sleep_graph_stats(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// Graph-level sleep preflight; sets `skipped` when graph has no islands.
IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// Graph-level wake preflight for batch wake guards.
IslandWakeGraphPreflight preflight_island_wake_graph(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies,
                                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                                     f32 linearThreshold,
                                                     f32 angularThreshold);

/// Early-out guard when every dynamic body in the island is sleeping.
bool should_skip_island_solve_for_sleep(const ContactIslandGraph::Island& island,
                                        const RigidBodySoA& bodies);

/// Early-out guard when island index is out of range or island is fully sleeping.
bool should_skip_island_solve_for_sleep_index(const ContactIslandGraph& graph,
                                              u32 islandIndex,
                                              const RigidBodySoA& bodies);

/// True when an island has wake candidates (motion, forces, or penetrating contacts).
bool should_wake_island(const ContactIslandGraph::Island& island,
                        const RigidBodySoA& bodies,
                        const std::vector<narrowphase::ContactManifold>& contacts,
                        f32 linearThreshold,
                        f32 angularThreshold);

/// Collect island indices whose dynamic bodies are all sleeping.
std::vector<u32> collect_fully_sleeping_island_indices(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// Collect island indices that pass wake preflight.
std::vector<u32> collect_wakeable_island_indices(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies,
                                                 const std::vector<narrowphase::ContactManifold>& contacts,
                                                 f32 linearThreshold,
                                                 f32 angularThreshold);

/// Preflight island solve dispatch; sets `skipped` when no islands need work.
IslandSolvePreflight preflight_island_solve(const ContactIslandGraph& graph);

/// Preflight island dispatch including timestep validity.
IslandDispatchPreflight preflight_island_dispatch(const ContactIslandGraph& graph, f32 dt);

/// Early-out guard for the island solve loop when nothing is dispatchable.
bool should_skip_island_solve(const ContactIslandGraph& graph);

/// Early-out guard combining graph preflight and timestep validity.
bool should_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt);

/// Collect island indices that pass `should_solve_island` (parallel dispatch prep).
std::vector<u32> collect_dispatchable_island_indices(const ContactIslandGraph& graph);

/// Collect only dispatchable island jobs (filters `extract_island_jobs`).
std::vector<IslandSolveJob> collect_dispatchable_island_jobs(const ContactIslandGraph& graph);

/// Keep only jobs that pass `should_solve_island` (parallel dispatch prep stub).
std::vector<IslandSolveJob> filter_dispatchable_jobs(const std::vector<IslandSolveJob>& jobs);

/// Guarded dispatch entry: skips out-of-range, empty, and null-island jobs.
bool dispatch_solve_island(RigidBodySoA& bodies,
                           const ContactIslandGraph& graph,
                           u32 islandIndex,
                           SolverWorkBuffers& workBuffers,
                           const std::vector<DistanceConstraint>& distanceConstraints,
                           f32 dt,
                           f32 contactCompliance,
                           const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch from a pre-extracted job; skips empty/null jobs and invalid dt.
bool dispatch_solve_island_job(RigidBodySoA& bodies,
                               const IslandSolveJob& job,
                               SolverWorkBuffers& workBuffers,
                               const std::vector<DistanceConstraint>& distanceConstraints,
                               f32 dt,
                               f32 contactCompliance,
                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch from a pre-extracted job with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_job_result(RigidBodySoA& bodies,
                                                      const IslandSolveJob& job,
                                                      SolverWorkBuffers& workBuffers,
                                                      const std::vector<DistanceConstraint>& distanceConstraints,
                                                      f32 dt,
                                                      f32 contactCompliance,
                                                      const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_result(RigidBodySoA& bodies,
                                                  const ContactIslandGraph& graph,
                                                  u32 islandIndex,
                                                  SolverWorkBuffers& workBuffers,
                                                  const std::vector<DistanceConstraint>& distanceConstraints,
                                                  f32 dt,
                                                  f32 contactCompliance,
                                                  const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch over all islands; returns count of islands actually solved.
u32 dispatch_all_islands(RigidBodySoA& bodies,
                         const ContactIslandGraph& graph,
                         SolverWorkBuffers& workBuffers,
                         const std::vector<DistanceConstraint>& distanceConstraints,
                         f32 dt,
                         f32 contactCompliance,
                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_islands_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Per-constraint-pair delta clear → accumulate → apply (job-safe across parallel islands).
void per_pair_delta_application(RigidBodySoA& bodies,
                                SolverWorkBuffers& workBuffers,
                                u32 bodyA,
                                u32 bodyB,
                                f32 invMassA,
                                f32 invMassB,
                                const narrowphase::ContactManifold& contact,
                                f32 dt,
                                f32 contactCompliance,
                                f32& lambda);

void per_pair_delta_application(RigidBodySoA& bodies,
                                SolverWorkBuffers& workBuffers,
                                u32 bodyA,
                                u32 bodyB,
                                f32 invMassA,
                                f32 invMassB,
                                const DistanceConstraint& constraint,
                                f32 dt,
                                f32& lambda);

/// Resolve all constraints in one island (Gauss-Seidel stub). Returns false for empty islands.
bool solve_island_job(RigidBodySoA& bodies,
                      const ContactIslandGraph::Island& island,
                      SolverWorkBuffers& workBuffers,
                      const std::vector<DistanceConstraint>& distanceConstraints,
                      f32 dt,
                      f32 contactCompliance,
                      const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Clear lambdas and warm-start distance/contact slots from the prior frame (first substep only).
void frame_lambda_warm_start(SolverWorkBuffers& workBuffers,
                             const std::vector<DistanceConstraint>& distanceConstraints,
                             const std::vector<f32>& priorDistanceLambdas,
                             const std::vector<f32>& priorContactLambdas = {});

/// Preflight frame warm-start; sets `skipped` when no prior lambda data exists.
FrameWarmStartPreflight preflight_frame_lambda_warm_start(
    const std::vector<DistanceConstraint>& distanceConstraints,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas = {});

/// Early-out guard for frame lambda warm-start when no prior data exists.
bool should_skip_frame_warm_start(const std::vector<f32>& priorDistanceLambdas,
                                  const std::vector<f32>& priorContactLambdas = {});

/// Guarded frame warm-start; clears lambdas and returns false when no prior data exists.
bool frame_lambda_warm_start_guarded(SolverWorkBuffers& workBuffers,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     const std::vector<f32>& priorDistanceLambdas,
                                     const std::vector<f32>& priorContactLambdas = {});

/// Warm-start only the lambda slots referenced by one island (parallel-safe selective seed).
void warm_start_island_lambdas(SolverWorkBuffers& workBuffers,
                               const ContactIslandGraph::Island& island,
                               const std::vector<f32>& priorDistanceLambdas,
                               const std::vector<f32>& priorContactLambdas = {});

/// Preflight warm-start for one island; sets `skipped` for empty islands.
IslandWarmStartPreflight preflight_warm_start_island(const ContactIslandGraph::Island& island,
                                                     const std::vector<f32>& priorDistanceLambdas,
                                                     const std::vector<f32>& priorContactLambdas = {});

/// Preflight warm-start by island index; out-of-range indices are marked skipped.
IslandWarmStartPreflight preflight_warm_start_island_by_index(const ContactIslandGraph& graph,
                                                              u32 islandIndex,
                                                              const std::vector<f32>& priorDistanceLambdas,
                                                              const std::vector<f32>& priorContactLambdas = {});

/// Summarize warm-startable vs empty/no-prior islands for batch guards.
IslandWarmStartStats compute_island_warm_start_stats(const ContactIslandGraph& graph,
                                                     const std::vector<f32>& priorDistanceLambdas,
                                                     const std::vector<f32>& priorContactLambdas = {});

/// Count islands that pass per-island warm-start preflight.
u32 count_warm_startable_islands(const ContactIslandGraph& graph,
                                 const std::vector<f32>& priorDistanceLambdas,
                                 const std::vector<f32>& priorContactLambdas = {});

/// True when at least one island can seed from prior lambda data.
bool has_warm_startable_islands(const ContactIslandGraph& graph,
                                const std::vector<f32>& priorDistanceLambdas,
                                const std::vector<f32>& priorContactLambdas = {});

/// Graph-level warm-start preflight; sets `skipped` when nothing can seed.
IslandWarmStartGraphPreflight preflight_warm_start_graph(const ContactIslandGraph& graph,
                                                         const std::vector<f32>& priorDistanceLambdas,
                                                         const std::vector<f32>& priorContactLambdas = {});

/// Early-out guard for graph-level warm-start batching.
bool should_skip_warm_start_graph(const ContactIslandGraph& graph,
                                  const std::vector<f32>& priorDistanceLambdas,
                                  const std::vector<f32>& priorContactLambdas = {});

/// Collect island indices that pass per-island warm-start preflight.
std::vector<u32> collect_warm_startable_island_indices(const ContactIslandGraph& graph,
                                                       const std::vector<f32>& priorDistanceLambdas,
                                                       const std::vector<f32>& priorContactLambdas = {});

/// Early-out guard for per-island lambda warm-start on empty islands.
bool should_skip_warm_start_island(const ContactIslandGraph::Island& island);

/// Early-out guard for warm-start by island index (empty or out-of-range).
bool should_skip_warm_start_island_index(const ContactIslandGraph& graph, u32 islandIndex);

/// Guarded warm-start; returns false when the island is empty or has no prior data.
bool warm_start_island_lambdas_guarded(SolverWorkBuffers& workBuffers,
                                       const ContactIslandGraph::Island& island,
                                       const std::vector<f32>& priorDistanceLambdas,
                                       const std::vector<f32>& priorContactLambdas = {});

/// Graph-level guarded warm-start across all islands; returns islands actually seeded.
u32 warm_start_graph_lambdas_guarded(SolverWorkBuffers& workBuffers,
                                     const ContactIslandGraph& graph,
                                     const std::vector<f32>& priorDistanceLambdas,
                                     const std::vector<f32>& priorContactLambdas = {});

/// Guarded warm-start by island index; returns false for out-of-range, empty, or no-prior islands.
bool warm_start_island_lambdas_by_index_guarded(SolverWorkBuffers& workBuffers,
                                                const ContactIslandGraph& graph,
                                                u32 islandIndex,
                                                const std::vector<f32>& priorDistanceLambdas,
                                                const std::vector<f32>& priorContactLambdas = {});

/// Guarded warm-start with explicit skip/seed outcome.
IslandWarmStartResult warm_start_island_lambdas_result(SolverWorkBuffers& workBuffers,
                                                       const ContactIslandGraph& graph,
                                                       u32 islandIndex,
                                                       const std::vector<f32>& priorDistanceLambdas,
                                                       const std::vector<f32>& priorContactLambdas = {});

/// Batch guarded warm-start over warm-startable islands; returns count seeded.
u32 warm_start_all_islands_guarded(SolverWorkBuffers& workBuffers,
                                   const ContactIslandGraph& graph,
                                   const std::vector<f32>& priorDistanceLambdas,
                                   const std::vector<f32>& priorContactLambdas = {});

/// Batch guarded warm-start with explicit skip/seed counts.
IslandBatchWarmStartResult warm_start_all_islands_result(SolverWorkBuffers& workBuffers,
                                                         const ContactIslandGraph& graph,
                                                         const std::vector<f32>& priorDistanceLambdas,
                                                         const std::vector<f32>& priorContactLambdas = {});

/// Preflight contact-impulse warm-start for one island; sets `skipped` for empty islands.
IslandContactImpulseWarmStartPreflight preflight_warm_start_contact_impulses(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Preflight contact-impulse warm-start by island index; out-of-range indices are marked skipped.
IslandContactImpulseWarmStartPreflight preflight_warm_start_contact_impulses_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Summarize warm-startable vs empty/no-impulse islands for graph-level impulse guards.
IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Count islands that pass per-island contact-impulse warm-start preflight.
u32 count_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,
                                                 const std::vector<narrowphase::ContactManifold>& contacts,
                                                 f32 dt);

/// True when at least one island can seed from non-zero contact impulses.
bool has_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,
                                                const std::vector<narrowphase::ContactManifold>& contacts,
                                                f32 dt);

/// Graph-level contact-impulse warm-start preflight; sets `skipped` when nothing can seed.
IslandContactImpulseWarmStartGraphPreflight preflight_warm_start_contact_impulses_graph(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Early-out guard for graph-level contact-impulse warm-start batching.
bool should_skip_warm_start_contact_impulses_graph(const ContactIslandGraph& graph,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   f32 dt);

/// Collect island indices that pass per-island contact-impulse warm-start preflight.
std::vector<u32> collect_warm_startable_contact_impulse_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Early-out guard for per-island contact-impulse warm-start (empty island or invalid dt).
bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island, f32 dt);

/// Early-out guard using contact data (empty island, invalid dt, or no non-zero impulses).
bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island,
                                             const std::vector<narrowphase::ContactManifold>& contacts,
                                             f32 dt);

/// Early-out guard for contact-impulse warm-start by island index (empty, out-of-range, or invalid dt).
bool should_skip_warm_start_contact_impulses_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   f32 dt);

/// Preflight combined lambda + contact-impulse warm-start for one island.
IslandCombinedWarmStartPreflight preflight_warm_start_combined_island(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas = {});

/// Guarded combined lambda + contact-impulse warm-start for one island.
bool warm_start_island_combined_guarded(SolverWorkBuffers& workBuffers,
                                        const ContactIslandGraph::Island& island,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        f32 dt,
                                        const std::vector<f32>& priorDistanceLambdas,
                                        const std::vector<f32>& priorContactLambdas = {});

/// Seed contact impulse warm-start for contacts owned by one island (per-substep stub).
void warm_start_island_contact_impulses(SolverWorkBuffers& workBuffers,
                                        const ContactIslandGraph::Island& island,
                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                        f32 dt);

/// Guarded contact impulse warm-start; returns false for empty islands.
bool warm_start_island_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
                                                const ContactIslandGraph::Island& island,
                                                const std::vector<narrowphase::ContactManifold>& contacts,
                                                f32 dt);

/// Guarded contact-impulse warm-start with explicit skip/seed outcome.
IslandWarmStartResult warm_start_island_contact_impulses_result(SolverWorkBuffers& workBuffers,
                                                                const ContactIslandGraph& graph,
                                                                u32 islandIndex,
                                                                const std::vector<narrowphase::ContactManifold>& contacts,
                                                                f32 dt);

/// Guarded contact-impulse warm-start by island index.
bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,
                                                         const ContactIslandGraph& graph,
                                                         u32 islandIndex,
                                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                                         f32 dt);

/// Batch guarded contact-impulse warm-start over islands with non-zero impulses.
u32 warm_start_all_islands_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
                                                    const ContactIslandGraph& graph,
                                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                                    f32 dt);

/// Batch guarded contact-impulse warm-start with explicit skip/seed counts.
IslandBatchWarmStartResult warm_start_all_islands_contact_impulses_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Guarded combined warm-start with explicit skip/seed outcome.
IslandWarmStartResult warm_start_island_combined_result(SolverWorkBuffers& workBuffers,
                                                        const ContactIslandGraph& graph,
                                                        u32 islandIndex,
                                                        const std::vector<narrowphase::ContactManifold>& contacts,
                                                        f32 dt,
                                                        const std::vector<f32>& priorDistanceLambdas,
                                                        const std::vector<f32>& priorContactLambdas = {});

/// Batch guarded combined warm-start over all islands that can seed.
u32 warm_start_all_islands_combined_guarded(SolverWorkBuffers& workBuffers,
                                            const ContactIslandGraph& graph,
                                            const std::vector<narrowphase::ContactManifold>& contacts,
                                            f32 dt,
                                            const std::vector<f32>& priorDistanceLambdas,
                                            const std::vector<f32>& priorContactLambdas = {});

/// Batch guarded combined warm-start with explicit skip/seed counts.
IslandBatchWarmStartResult warm_start_all_islands_combined_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas = {});

} // namespace fuse::physics
