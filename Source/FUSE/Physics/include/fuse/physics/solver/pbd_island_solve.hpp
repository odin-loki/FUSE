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

/// Per-island constraint solve preflight (OOB indices, immovable pairs) (B4.4 deepen).
struct IslandSolveJobPreflight {
    u32 contactCount = 0;
    u32 distanceCount = 0;
    u32 outOfRangeContacts = 0;
    u32 outOfRangeDistances = 0;
    u32 immovableConstraintPairs = 0;
    u32 solvableConstraintPairs = 0;
    bool skipped = false;

    bool can_solve() const { return !skipped && solvableConstraintPairs > 0u; }
};

/// Per-island body sleep state summary (B4.4 deepen).
struct IslandSleepPreflight {
    u32 dynamicBodyCount = 0;
    u32 sleepingBodyCount = 0;
    u32 staticBodyCount = 0;
    u32 bodiesWithForces = 0;
    u32 bodiesWithCcd = 0;
    bool skipped = false;

    bool all_dynamic_sleeping() const {
        return dynamicBodyCount > 0u && sleepingBodyCount == dynamicBodyCount;
    }

    bool all_static() const {
        return dynamicBodyCount == 0u && staticBodyCount > 0u;
    }
};

/// Combined sleep/wake preflight for one island (B4.4 deepen).
struct IslandSleepWakePreflight {
    IslandSleepPreflight sleep{};
    u32 activeContactCount = 0;
    u32 contactsTouchingSleepingBody = 0;
    bool should_wake = false;
    bool skipped = false;
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

/// True when the body carries `RB_SLEEPING`.
bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when the body carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when the body is dynamic (not static or kinematic).
bool is_body_dynamic(const RigidBodySoA& bodies, u32 bodyIndex);

/// Preflight constraint solve for one island; sets `skipped` for empty islands.
IslandSolveJobPreflight preflight_solve_island_job(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    const RigidBodySoA& bodies,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Preflight constraint solve by island index; out-of-range indices are marked skipped.
IslandSolveJobPreflight preflight_solve_island_job_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    const RigidBodySoA& bodies,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Early-out guard when every dynamic body in the island is sleeping.
bool should_skip_solve_island_all_sleeping(const ContactIslandGraph::Island& island,
                                           const RigidBodySoA& bodies);

/// Early-out guard when the island has no dynamic bodies.
bool should_skip_solve_island_all_static(const ContactIslandGraph::Island& island,
                                         const RigidBodySoA& bodies);

/// Early-out guard when preflight reports no solvable constraint pairs.
bool should_skip_solve_island_job_preflight(const IslandSolveJobPreflight& preflight);

/// Preflight per-island sleep state; sets `skipped` for empty body lists.
IslandSleepPreflight preflight_island_sleep_state(const ContactIslandGraph::Island& island,
                                                  const RigidBodySoA& bodies);

/// Preflight per-island sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_state_by_index(const ContactIslandGraph& graph,
                                                           u32 islandIndex,
                                                           const RigidBodySoA& bodies);

/// Early-out guard for sleep detection on bodies with pending forces or `RB_CCD`.
bool should_skip_sleep_detection_for_body(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when a sleeping body in the island should wake due to active contacts.
bool should_wake_island_on_contact(const ContactIslandGraph::Island& island,
                                   const RigidBodySoA& bodies,
                                   const std::vector<narrowphase::ContactManifold>& contacts);

/// Combined sleep/wake preflight for one island.
IslandSleepWakePreflight preflight_island_sleep_wake(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts);

/// Combined sleep/wake preflight by island index; out-of-range indices are marked skipped.
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts);

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

/// Early-out guard for per-island contact-impulse warm-start (empty island or invalid dt).
bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island, f32 dt);

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

} // namespace fuse::physics
