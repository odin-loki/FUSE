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

/// Aggregate warm-start coverage for parallel island seeding (B4.4 deepen).
struct IslandWarmStartStats {
    u32 totalIslands = 0;
    u32 skippedCount = 0;
    u32 warmStartableCount = 0;
};

/// Bundle preflight diagnostics and dispatch indices for one iteration pass (B4.4 deepen).
struct IslandSolveDispatchPlan {
    IslandSolvePreflight preflight{};
    std::vector<u32> dispatchIndices{};

    bool can_dispatch() const { return preflight.can_dispatch(); }
};

/// True when `islandIndex` is in range for `extract_island`.
bool island_index_valid(const ContactIslandGraph& graph, u32 islandIndex);

/// Returns true when the island carries at least one contact or distance constraint.
bool island_has_constraints(const ContactIslandGraph::Island& island);

/// Contact + distance constraint count for dispatch sizing stubs.
u32 island_constraint_count(const ContactIslandGraph::Island& island);

/// True when a job should run `solve_island_job` (non-empty, in-range, bound island).
bool should_solve_island(const IslandSolveJob& job);

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

/// Preflight island solve dispatch; sets `skipped` when no islands need work.
IslandSolvePreflight preflight_island_solve(const ContactIslandGraph& graph);

/// Early-out guard for the island solve loop when nothing is dispatchable.
bool should_skip_island_solve(const ContactIslandGraph& graph);

/// Collect island indices that pass `should_solve_island` (parallel dispatch prep).
std::vector<u32> collect_dispatchable_island_indices(const ContactIslandGraph& graph);

/// True when a single island index would be dispatched this substep.
bool island_index_dispatchable(const ContactIslandGraph& graph, u32 islandIndex);

/// Build dispatch plan from graph (preflight + collected indices).
IslandSolveDispatchPlan build_island_solve_dispatch_plan(const ContactIslandGraph& graph);

/// Dispatch using a pre-built plan; returns count of islands actually solved.
u32 dispatch_islands_from_plan(RigidBodySoA& bodies,
                               const ContactIslandGraph& graph,
                               const IslandSolveDispatchPlan& plan,
                               SolverWorkBuffers& workBuffers,
                               const std::vector<DistanceConstraint>& distanceConstraints,
                               f32 dt,
                               f32 contactCompliance,
                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch entry: skips out-of-range, empty, and null-island jobs.
bool dispatch_solve_island(RigidBodySoA& bodies,
                           const ContactIslandGraph& graph,
                           u32 islandIndex,
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

/// Warm-start only the lambda slots referenced by one island (parallel-safe selective seed).
void warm_start_island_lambdas(SolverWorkBuffers& workBuffers,
                               const ContactIslandGraph::Island& island,
                               const std::vector<f32>& priorDistanceLambdas,
                               const std::vector<f32>& priorContactLambdas = {});

/// Preflight warm-start for one island; sets `skipped` for empty islands.
IslandWarmStartPreflight preflight_warm_start_island(const ContactIslandGraph::Island& island,
                                                     const std::vector<f32>& priorDistanceLambdas,
                                                     const std::vector<f32>& priorContactLambdas = {});

/// Early-out guard for per-island lambda warm-start on empty islands.
bool should_skip_warm_start_island(const ContactIslandGraph::Island& island);

/// Guarded warm-start; returns false when the island is empty or has no prior data.
bool warm_start_island_lambdas_guarded(SolverWorkBuffers& workBuffers,
                                       const ContactIslandGraph::Island& island,
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

/// Summarize warm-start coverage across all islands for parallel seed prep.
IslandWarmStartStats compute_island_warm_start_stats(const ContactIslandGraph& graph,
                                                     const std::vector<f32>& priorDistanceLambdas,
                                                     const std::vector<f32>& priorContactLambdas = {});

/// Early-out guard when no island has prior lambda data to seed.
bool should_skip_frame_warm_start(const ContactIslandGraph& graph,
                                  const std::vector<f32>& priorDistanceLambdas,
                                  const std::vector<f32>& priorContactLambdas = {});

/// Collect island indices that pass warm-start preflight (parallel seed prep).
std::vector<u32> collect_warm_startable_island_indices(const ContactIslandGraph& graph,
                                                       const std::vector<f32>& priorDistanceLambdas,
                                                       const std::vector<f32>& priorContactLambdas = {});

/// Guarded per-island warm-start across all warm-startable islands; returns count seeded.
u32 warm_start_all_islands_guarded(SolverWorkBuffers& workBuffers,
                                   const ContactIslandGraph& graph,
                                   const std::vector<f32>& priorDistanceLambdas,
                                   const std::vector<f32>& priorContactLambdas = {});

} // namespace fuse::physics
