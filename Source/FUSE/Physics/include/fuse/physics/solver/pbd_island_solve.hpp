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

/// Input coverage for island graph build (out-of-range body-index guards).
struct IslandBuildStats {
    u32 bodyCount = 0;
    u32 contactSlotCount = 0;
    u32 distanceSlotCount = 0;
    u32 validContactCount = 0;
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;
};

/// Preflight diagnostics for island graph build inputs (B4.4 deepen).
struct IslandBuildPreflight {
    ContactIslandGraphBuildRejectReason reason = ContactIslandGraphBuildRejectReason::None;
    IslandBuildStats stats{};
    bool skipped = false;

    bool has_unsafe_refs() const {
        return stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u;
    }

    bool can_build() const { return reason == ContactIslandGraphBuildRejectReason::None; }
};

/// Body participation for one island constraint solve (sleep/static/movable guards).
struct IslandSolveBodiesPreflight {
    u32 bodyCount = 0;
    u32 inRangeBodyCount = 0;
    u32 staticOrKinematicCount = 0;
    u32 sleepingCount = 0;
    u32 movableCount = 0;
    bool skipped = false;

    bool can_solve() const { return !skipped && movableCount > 0u; }
};

/// Combined constraint-ref + body participation preflight for one island solve pass.
struct IslandConstraintSolvePreflight {
    IslandConstraintRefsPreflight refs{};
    IslandSolveBodiesPreflight bodies{};
    bool skipped = false;

    bool can_solve() const { return !skipped && refs.can_solve() && bodies.can_solve(); }
};

/// Per-island sleep state for solve early-out stubs.
struct IslandSleepPreflight {
    u32 bodyCount = 0;
    u32 sleepingCount = 0;
    u32 staticOrKinematicCount = 0;
    u32 activeDynamicCount = 0;
    bool allSleeping = false;
    bool skipped = false;

    bool can_skip_solve() const { return !skipped && allSleeping; }
};

/// Per-island wake hint when active dynamics neighbor sleeping bodies.
struct IslandWakePreflight {
    u32 bodyCount = 0;
    u32 sleepingCount = 0;
    u32 activeDynamicCount = 0;
    bool hasMixedSleepState = false;
    bool skipped = false;

    bool should_wake_sleepers() const {
        return !skipped && hasMixedSleepState && activeDynamicCount > 0u;
    }
};

/// Aggregate sleep counts for graph-level batch guards.
struct IslandSleepGraphStats {
    u32 totalIslands = 0;
    u32 allSleepingCount = 0;
    u32 mixedSleepCount = 0;
    u32 fullyActiveCount = 0;
    u32 emptyCount = 0;
};

/// Graph-level sleep preflight for selective per-island solve skipping.
struct IslandSleepGraphPreflight {
    IslandSleepGraphStats stats{};
    bool skipped = false;

    bool has_solveable_islands() const {
        return !skipped && (stats.fullyActiveCount > 0u || stats.mixedSleepCount > 0u);
    }
};

/// Aggregate wake counts for graph-level batch guards.
struct IslandWakeGraphStats {
    u32 totalIslands = 0;
    u32 wakeableCount = 0;
    u32 emptyCount = 0;
};

/// Graph-level wake preflight for selective per-island sleeper activation.
struct IslandWakeGraphPreflight {
    IslandWakeGraphStats stats{};
    bool skipped = false;

    bool can_wake() const { return !skipped && stats.wakeableCount > 0u; }
};

/// Why island solve dispatch would early-out (B4.4 deepen follow-up pass).
enum class IslandDispatchRejectReason : u8 {
    None = 0,
    NoDispatchableIslands,
    InvalidDt,
    NonFiniteDt,
};

/// Human-readable label for island dispatch reject reasons (logging / tests).
const char* islandDispatchRejectReasonName(IslandDispatchRejectReason reason);

/// Diagnose why dispatch would skip; vacuously succeeds when dispatch may proceed.
IslandDispatchRejectReason islandDispatchRejectReason(const ContactIslandGraph& graph, f32 dt);

/// Returns true when `islandDispatchRejectReason` matches `expected` (B4.4 deepen follow-up pass).
bool islandDispatchRejectsForReason(const ContactIslandGraph& graph,
                                    f32 dt,
                                    IslandDispatchRejectReason expected);

/// Read-only island dispatch diagnostics — no mutation (B4.4 deepen follow-up pass).
struct IslandDispatchRejectPreflight {
    IslandDispatchRejectReason reason = IslandDispatchRejectReason::None;
    IslandSolvePreflight solve{};
    bool invalidDt = false;
    bool skipped = false;

    bool can_dispatch() const { return reason == IslandDispatchRejectReason::None; }
};

IslandDispatchRejectPreflight preflightIslandDispatchReject(const ContactIslandGraph& graph, f32 dt);

/// Non-mutating dispatch skip predicate — inverse of `can_dispatch` (B4.4 deepen follow-up pass).
bool canSkipIslandDispatch(const ContactIslandGraph& graph, f32 dt);

/// Non-mutating dispatch predicate — mirrors `preflightIslandDispatchReject` (B4.4 deepen follow-up pass).
bool shouldRunIslandDispatch(const ContactIslandGraph& graph, f32 dt);

/// Why one island solve job would reject dispatch (B4.4 deepen follow-up pass).
enum class IslandSolveJobRejectReason : u8 {
    None = 0,
    EmptyJob,
    NullIsland,
    ZeroConstraints,
    InvalidDt,
    NonFiniteDt,
};

const char* islandSolveJobRejectReasonName(IslandSolveJobRejectReason reason);

IslandSolveJobRejectReason islandSolveJobRejectReason(const IslandSolveJob& job, f32 dt);

bool islandSolveJobRejectsForReason(const IslandSolveJob& job,
                                    f32 dt,
                                    IslandSolveJobRejectReason expected);

struct IslandSolveJobRejectPreflight {
    IslandSolveJobRejectReason reason = IslandSolveJobRejectReason::None;
    bool invalidDt = false;
    bool skipped = false;
    u32 constraintCount = 0;

    bool can_dispatch() const { return reason == IslandSolveJobRejectReason::None; }
};

IslandSolveJobRejectPreflight preflightIslandSolveJobReject(const IslandSolveJob& job, f32 dt);

bool canSkipIslandSolveJob(const IslandSolveJob& job, f32 dt);

bool shouldRunIslandSolveJob(const IslandSolveJob& job, f32 dt);

/// Why one island constraint solve would early-out (B4.4 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    NoInRangeRefs,
    NoMovableBodies,
};

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason);

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

bool islandConstraintSolveRejectsForReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected);

struct IslandConstraintSolveRejectPreflight {
    IslandConstraintSolveRejectReason reason = IslandConstraintSolveRejectReason::None;
    IslandConstraintRefsPreflight refs{};
    IslandSolveBodiesPreflight bodies{};
    bool skipped = false;

    bool can_solve() const { return reason == IslandConstraintSolveRejectReason::None; }
};

IslandConstraintSolveRejectPreflight preflightIslandConstraintSolveReject(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

bool canSkipIslandConstraintSolve(const ContactIslandGraph::Island& island,
                                  const RigidBodySoA& bodies,
                                  const std::vector<narrowphase::ContactManifold>& contacts,
                                  const std::vector<DistanceConstraint>& distanceConstraints);

bool shouldRunIslandConstraintSolve(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints);

/// Why per-island sleep solve would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepSolveRejectReason : u8 {
    None = 0,
    EmptyIsland,
    AllSleeping,
};

const char* islandSleepSolveRejectReasonName(IslandSleepSolveRejectReason reason);

IslandSleepSolveRejectReason islandSleepSolveRejectReason(const ContactIslandGraph::Island& island,
                                                          const RigidBodySoA& bodies);

bool islandSleepSolveRejectsForReason(const ContactIslandGraph::Island& island,
                                      const RigidBodySoA& bodies,
                                      IslandSleepSolveRejectReason expected);

struct IslandSleepSolveRejectPreflight {
    IslandSleepSolveRejectReason reason = IslandSleepSolveRejectReason::None;
    IslandSleepPreflight sleep{};
    bool skipped = false;

    bool can_solve() const { return reason == IslandSleepSolveRejectReason::None; }
};

IslandSleepSolveRejectPreflight preflightIslandSleepSolveReject(const ContactIslandGraph::Island& island,
                                                                const RigidBodySoA& bodies);

bool canSkipIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

bool shouldRunIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Why per-island wake would early-out (B4.4 deepen follow-up pass).
enum class IslandWakeRejectReason : u8 {
    None = 0,
    EmptyIsland,
    NoMixedSleepState,
    NoActiveDynamic,
};

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason);

IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,
                                              const RigidBodySoA& bodies);

bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
                                const RigidBodySoA& bodies,
                                IslandWakeRejectReason expected);

struct IslandWakeRejectPreflight {
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;
    IslandWakePreflight wake{};
    bool skipped = false;

    bool can_wake() const { return reason == IslandWakeRejectReason::None; }
};

IslandWakeRejectPreflight preflightIslandWakeReject(const ContactIslandGraph::Island& island,
                                                    const RigidBodySoA& bodies);

bool canSkipIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

bool shouldRunIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Why graph-level sleep batching would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepGraphRejectReason : u8 {
    None = 0,
    AllIslandsSleepingOrEmpty,
};

const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason);

IslandSleepGraphRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,
                                                          const RigidBodySoA& bodies);

bool islandSleepGraphRejectsForReason(const ContactIslandGraph& graph,
                                      const RigidBodySoA& bodies,
                                      IslandSleepGraphRejectReason expected);

struct IslandSleepGraphRejectPreflight {
    IslandSleepGraphRejectReason reason = IslandSleepGraphRejectReason::None;
    IslandSleepGraphPreflight sleep{};
    bool skipped = false;

    bool has_solveable_islands() const { return reason == IslandSleepGraphRejectReason::None; }
};

IslandSleepGraphRejectPreflight preflightIslandSleepGraphReject(const ContactIslandGraph& graph,
                                                                const RigidBodySoA& bodies);

bool canSkipIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

bool shouldRunIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Why graph-level wake batching would early-out (B4.4 deepen follow-up pass).
enum class IslandWakeGraphRejectReason : u8 {
    None = 0,
    NoWakeableIslands,
};

const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason);

IslandWakeGraphRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,
                                                        const RigidBodySoA& bodies);

bool islandWakeGraphRejectsForReason(const ContactIslandGraph& graph,
                                     const RigidBodySoA& bodies,
                                     IslandWakeGraphRejectReason expected);

struct IslandWakeGraphRejectPreflight {
    IslandWakeGraphRejectReason reason = IslandWakeGraphRejectReason::None;
    IslandWakeGraphPreflight wake{};
    bool skipped = false;

    bool can_wake() const { return reason == IslandWakeGraphRejectReason::None; }
};

IslandWakeGraphRejectPreflight preflightIslandWakeGraphReject(const ContactIslandGraph& graph,
                                                              const RigidBodySoA& bodies);

bool canSkipIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

bool shouldRunIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Why wake-then-dispatch pipeline would early-out (B4.4 deepen follow-up pass).
enum class IslandPipelineDispatchRejectReason : u8 {
    None = 0,
    InvalidDt,
    NonFiniteDt,
    NoDispatchableIslands,
    AllIslandsSleeping,
};

const char* islandPipelineDispatchRejectReasonName(IslandPipelineDispatchRejectReason reason);

IslandPipelineDispatchRejectReason islandPipelineDispatchRejectReason(const ContactIslandGraph& graph,
                                                                      const RigidBodySoA& bodies,
                                                                      f32 dt);

bool islandPipelineDispatchRejectsForReason(const ContactIslandGraph& graph,
                                              const RigidBodySoA& bodies,
                                              f32 dt,
                                              IslandPipelineDispatchRejectReason expected);

struct IslandPipelineDispatchPreflight {
    IslandPipelineDispatchRejectReason reason = IslandPipelineDispatchRejectReason::None;
    IslandWakeGraphRejectPreflight wake{};
    IslandSleepGraphRejectPreflight sleep{};
    IslandDispatchRejectPreflight dispatch{};
    bool skipped = false;

    bool can_dispatch() const { return reason == IslandPipelineDispatchRejectReason::None; }
};

IslandPipelineDispatchPreflight preflightIslandPipelineDispatch(const ContactIslandGraph& graph,
                                                                const RigidBodySoA& bodies,
                                                                f32 dt);

bool canSkipIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt);

bool shouldRunIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt);

/// Wake sleepers then dispatch all islands only when pipeline preflight allows (B4.4 deepen follow-up pass).
IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

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

/// True when `flags` carries `RB_SLEEPING`.
bool is_body_sleeping(u32 flags);

/// True when `flags` carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_body_static_or_kinematic(u32 flags);

/// True when a body can receive constraint corrections (dynamic and awake).
bool is_body_movable(const RigidBodySoA& bodies, u32 bodyIndex);

/// Preflight island graph build inputs; sets `skipped` when nothing can partition.
IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when build inputs cannot form any constrained partition.
bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints);

/// Guarded island graph build; returns false when preflight skips build.
bool build_island_graph_guarded(ContactIslandGraph& graph,
                                u32 bodyCount,
                                const std::vector<narrowphase::ContactManifold>& contacts,
                                const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating island build skip predicate — inverse of `can_build` (B4.4 deepen follow-up pass).
bool canSkipIslandBuild(u32 bodyCount,
                        const std::vector<narrowphase::ContactManifold>& contacts,
                        const std::vector<DistanceConstraint>& distanceConstraints);

/// Non-mutating island build predicate — mirrors `preflight_island_build` (B4.4 deepen follow-up pass).
bool shouldRunIslandBuild(u32 bodyCount,
                          const std::vector<narrowphase::ContactManifold>& contacts,
                          const std::vector<DistanceConstraint>& distanceConstraints);

/// Batch guarded dispatch only when `preflightIslandDispatchReject` allows (B4.4 deepen follow-up pass).
IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Preflight body participation for one island solve pass.
IslandSolveBodiesPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
                                                         const RigidBodySoA& bodies);

/// Early-out guard when an island has no movable bodies to solve.
bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,
                                     const RigidBodySoA& bodies);

/// Combined constraint-ref + body participation preflight for one island solve pass.
IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when refs or body participation block island constraint solve.
bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,
                                         const RigidBodySoA& bodies,
                                         const std::vector<narrowphase::ContactManifold>& contacts,
                                         const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight sleep state for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,
                                            const RigidBodySoA& bodies);

/// Preflight sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,
                                                     u32 islandIndex,
                                                     const RigidBodySoA& bodies);

/// Preflight wake hints for one island; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,
                                          const RigidBodySoA& bodies);

/// Preflight wake hints by island index; out-of-range indices are marked skipped.
IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,
                                                   u32 islandIndex,
                                                   const RigidBodySoA& bodies);

/// Summarize all-sleeping vs mixed vs fully-active islands for batch guards.
IslandSleepGraphStats compute_island_sleep_stats(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies);

/// Graph-level sleep preflight; sets `skipped` when every island is all-sleeping or empty.
IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,
                                                       const RigidBodySoA& bodies);

/// Early-out guard when every constrained island is all-sleeping.
bool should_skip_island_sleep_solve_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Early-out guard when one island is all-sleeping.
bool should_skip_island_sleep_solve(const ContactIslandGraph::Island& island,
                                    const RigidBodySoA& bodies);

/// Summarize wakeable islands for batch guards.
IslandWakeGraphStats compute_island_wake_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Graph-level wake preflight; sets `skipped` when no island needs sleeper activation.
IslandWakeGraphPreflight preflight_island_wake_graph(const ContactIslandGraph& graph,
                                                     const RigidBodySoA& bodies);

/// Early-out guard for graph-level wake batching.
bool should_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Early-out guard when an island does not need sleeper activation.
bool should_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Collect island indices that are not all-sleeping (parallel solve prep stub).
std::vector<u32> collect_nonsleeping_island_indices(const ContactIslandGraph& graph,
                                                    const RigidBodySoA& bodies);

/// Collect island indices that should wake sleeping neighbors before solve.
std::vector<u32> collect_wakeable_island_indices(const ContactIslandGraph& graph,
                                                 const RigidBodySoA& bodies);

/// Guarded wake for sleeping bodies in one island; returns false when wake is unnecessary.
bool wake_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Guarded wake by island index; returns false for out-of-range or non-wakeable islands.
bool wake_island_sleepers_by_index_guarded(RigidBodySoA& bodies,
                                           const ContactIslandGraph& graph,
                                           u32 islandIndex);

/// Batch guarded wake across wakeable islands; returns count of islands activated.
u32 wake_all_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph& graph);

} // namespace fuse::physics

// --- deepen additive from deepen-b4-pbd-island-solve-guards-848c ---
bool should_skip_island_solve(const IslandSolveJob& job);
bool should_skip_all_island_solves(const ContactIslandGraph& graph);
                                 IslandWarmStartPreflight& out);
                                       const IslandWarmStartPreflight& preflight);

// --- deepen additive from deepen-b4-pbd-island-preflight-warmstart-4254 ---
    IslandSolvePreflight preflight{};
bool should_skip_frame_warm_start(const ContactIslandGraph& graph,

// --- deepen additive from deepen-b4-pbd-island-dispatch-warmstart-guards-e8ca ---
IslandContactImpulseWarmStartPreflight preflight_contact_impulse_warm_start_island(
IslandContactImpulseWarmStartPreflight preflight_contact_impulse_warm_start_island_by_index(
bool should_skip_contact_impulse_warm_start_island(const ContactIslandGraph::Island& island);
bool should_skip_contact_impulse_warm_start_island_index(const ContactIslandGraph& graph, u32 islandIndex);
IslandCombinedWarmStartPreflight preflight_warm_start_island_combined(

// --- deepen additive from deepen-b4-pbd-island-dispatch-warmstart-guards-c488 ---
bool should_skip_island_dispatch_job(const IslandSolveJob& job, f32 dt);

// --- deepen additive from deepen-b4-pbd-island-guards-7571 ---
struct IslandContactImpulseDispatchPreflight {
    IslandContactImpulseWarmStartGraphPreflight impulses{};
IslandContactImpulseWarmStartGraphPreflight preflight_contact_impulse_warm_start_graph(
IslandContactImpulseDispatchPreflight preflight_contact_impulse_dispatch(
bool should_skip_contact_impulse_warm_start_graph(
bool should_skip_contact_impulse_dispatch(const ContactIslandGraph& graph,
IslandCombinedWarmStartPreflight preflight_warm_start_island_combined_by_index(

// --- deepen additive from deepen-pbd-island-guards-6957 ---
    IslandWarmStartPreflight lambda{};
    IslandContactImpulseWarmStartPreflight impulse{};
IslandSolveJobPreflight preflight_island_solve_job(const ContactIslandGraph& graph,
IslandSolveJobPreflight preflight_island_solve_by_index(const ContactIslandGraph& graph,
bool should_skip_island_solve_invalid_indices(const IslandSolveJob& job,
bool should_skip_island_solve_index(const ContactIslandGraph& graph,
IslandContactImpulseWarmStartPreflight preflight_warm_start_island_contact_impulses(
IslandContactImpulseWarmStartPreflight preflight_warm_start_island_contact_impulses_by_index(

// --- deepen additive from pbd-island-guards-deepen-1f2e ---
struct IslandSolveInputsPreflight {
struct IslandContactImpulsePreflight {
    IslandContactImpulsePreflight impulse{};
IslandSolveInputsPreflight preflight_island_solve_inputs(const ContactIslandGraph::Island& island,
IslandSolveInputsPreflight preflight_island_solve_inputs_by_index(const ContactIslandGraph& graph,
bool should_skip_island_solve_inputs(const ContactIslandGraph::Island& island,
IslandContactImpulsePreflight preflight_warm_start_contact_impulses(
IslandContactImpulsePreflight preflight_warm_start_contact_impulses_by_index(
bool should_skip_warm_start_island_combined(const ContactIslandGraph::Island& island,

// --- deepen additive from deepen-pbd-island-guards-426b ---
struct IslandJobDispatchPreflight {
struct IslandContactImpulseGraphPreflight {
IslandJobDispatchPreflight preflight_dispatch_island_index(const ContactIslandGraph& graph,
bool should_skip_dispatch_island_index(const ContactIslandGraph& graph, u32 islandIndex, f32 dt);
IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses(
IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses_by_index(
IslandContactImpulseGraphPreflight preflight_warm_start_contact_impulses_graph(
bool should_skip_contact_impulse_warm_start_graph(const ContactIslandGraph& graph,

// --- deepen additive from deepen-b4-pbd-island-solver-ac66 ---
IslandSolveJobPreflight preflight_island_solve_job(const IslandSolveJob& job);
IslandContactImpulsePreflight preflight_island_contact_impulses(
IslandContactImpulsePreflight preflight_island_contact_impulses_by_index(
IslandContactImpulseGraphPreflight preflight_contact_impulse_graph(
bool should_skip_contact_impulse_graph(const ContactIslandGraph& graph,
bool should_skip_contact_impulse_island(const ContactIslandGraph::Island& island,
bool should_skip_contact_impulse_island_index(const ContactIslandGraph& graph,

// --- deepen additive from deepen-pbd-island-guards-f8cf ---
struct IslandDispatchJobPreflight {
IslandDispatchJobPreflight preflight_dispatch_island_job(const IslandSolveJob& job, f32 dt);
bool should_skip_dispatch_island_job(const IslandSolveJob& job, f32 dt);
bool should_skip_warm_start_contact_impulses_island(const ContactIslandGraph::Island& island);
bool should_skip_warm_start_contact_impulses_island_index(const ContactIslandGraph& graph, u32 islandIndex);

// --- deepen additive from deepen-pbd-island-guards-b61c ---
struct IslandCombinedWarmStartGraphPreflight {
IslandDispatchJobPreflight preflight_island_dispatch_from_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt);
bool should_skip_island_dispatch_from_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt);
IslandCombinedWarmStartPreflight preflight_warm_start_combined_island_by_index(
bool should_skip_warm_start_combined_island_index(const ContactIslandGraph& graph,
IslandCombinedWarmStartGraphPreflight preflight_warm_start_combined_graph(
bool should_skip_warm_start_combined_graph(const ContactIslandGraph& graph,

// --- deepen additive from pbd-island-guards-deepen-0fe3 ---
struct IslandDispatchIndexPreflight {
IslandSolveJobPreflight preflight_solve_island_job_by_index(const ContactIslandGraph& graph,
bool should_skip_solve_island_job_stale(const IslandSolveJob& job,
IslandDispatchIndexPreflight preflight_dispatch_island_by_index(const ContactIslandGraph& graph,

// --- deepen additive from deepen-pbd-island-guards-faad ---
struct SleepPassPreflight {
struct WakePreflight {
struct IslandSolveWorkPreflight {
struct IslandConstraintIndexPreflight {
SleepPassPreflight preflight_sleep_pass(const RigidBodySoA& bodies,
bool should_skip_sleep_pass(const RigidBodySoA& bodies,
WakePreflight preflight_wake_candidates(const RigidBodySoA& bodies,
bool should_skip_island_solve_all_inactive(const RigidBodySoA& bodies,
IslandSolveWorkPreflight preflight_island_solve_work(const RigidBodySoA& bodies,
IslandConstraintIndexPreflight preflight_island_constraint_indices(
IslandConstraintIndexPreflight preflight_island_constraint_indices_by_index(

// --- deepen additive from deepen-pbd-island-guards-88d5 ---
struct IslandDispatchJobBatchPreflight {
    IslandDispatchPreflight graph{};
IslandDispatchJobBatchPreflight preflight_dispatchable_island_jobs(const ContactIslandGraph& graph,
bool should_skip_dispatchable_island_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt);

// --- deepen additive from pbd-island-sleep-build-preflights-cb2c ---
struct IslandDispatchSleepPreflight {
    IslandDispatchPreflight dispatch{};
struct IslandSolveSleepPreflight {
    IslandConstraintSolvePreflight constraints{};
IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
IslandSleepPreflight preflight_island_sleep_by_index(const RigidBodySoA& bodies,
IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
IslandWakePreflight preflight_island_wake_by_index(const RigidBodySoA& bodies,
bool should_skip_island_solve_for_sleep(const RigidBodySoA& bodies,
bool should_skip_island_solve_for_sleep_index(const RigidBodySoA& bodies,
bool should_skip_island_solve_job_for_sleep(const RigidBodySoA& bodies, const IslandSolveJob& job);
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
IslandSolveSleepPreflight preflight_island_solve_sleep(const RigidBodySoA& bodies,
IslandSolveSleepPreflight preflight_island_solve_sleep_by_index(const RigidBodySoA& bodies,
IslandDispatchSleepPreflight preflight_island_dispatch_sleep(const RigidBodySoA& bodies,
bool should_skip_island_dispatch_sleep(const RigidBodySoA& bodies,

// --- deepen additive from deepen-pbd-island-guards-a261 ---
struct IslandSleepWakePreflight {
bool should_skip_solve_island_all_sleeping(const ContactIslandGraph::Island& island,
bool should_skip_solve_island_all_static(const ContactIslandGraph::Island& island,
bool should_skip_solve_island_job_preflight(const IslandSolveJobPreflight& preflight);
IslandSleepPreflight preflight_island_sleep_state(const ContactIslandGraph::Island& island,
IslandSleepPreflight preflight_island_sleep_state_by_index(const ContactIslandGraph& graph,
bool should_skip_sleep_detection_for_body(const RigidBodySoA& bodies, u32 bodyIndex);
IslandSleepWakePreflight preflight_island_sleep_wake(
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(

// --- deepen additive from deepen-pbd-island-sleep-build-guards-9e33 ---
bool should_skip_awake_island_dispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies);
bool should_skip_sleeping_island_solve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);
bool should_skip_sleeping_island_solve_index(const ContactIslandGraph& graph,
bool should_skip_island_wake(const ContactIslandGraph::Island& island);
IslandConstraintSolvePreflight preflight_solve_island(const ContactIslandGraph::Island& island,
IslandConstraintSolvePreflight preflight_solve_island_by_index(const ContactIslandGraph& graph,
bool should_skip_solve_island_preflight(const ContactIslandGraph::Island& island,

// --- deepen additive from pbd-island-guards-deepen-5934 ---
struct WakeOnImpulsePreflight {
struct IslandBodyPartitionPreflight {
struct ConstraintIterationPreflight {
IslandSleepPreflight preflight_sleeping_island(const ContactIslandGraph::Island& island,
IslandSleepPreflight preflight_sleeping_island_by_index(const ContactIslandGraph& graph,
bool should_skip_sleeping_island_solve_job(const IslandSolveJob& job,
WakeOnImpulsePreflight preflight_wake_on_impulse(const RigidBodySoA& bodies,
IslandBodyPartitionPreflight preflight_island_body_partition(const ContactIslandGraph& graph);
ConstraintIterationPreflight preflight_constraint_iterations(const SolverParams& params);
bool should_skip_constraint_iterations(const SolverParams& params);

// --- deepen additive from deepen-pbd-island-guards-5425 ---
struct IslandSleepSolvePreflight {
    IslandSleepSolvePreflight sleep{};
IslandSleepSolvePreflight preflight_island_sleep_solve(const ContactIslandGraph::Island& island,
IslandSleepSolvePreflight preflight_island_sleep_solve_by_index(const ContactIslandGraph& graph,
bool should_skip_island_sleep_detection(const ContactIslandGraph::Island& island,
bool should_skip_island_sleep_detection_index(const ContactIslandGraph& graph,
IslandSolveJobPreflight preflight_solve_island_job(const ContactIslandGraph::Island& island,
bool should_skip_solve_island_job_preflight(const ContactIslandGraph::Island& island,

// --- deepen additive from deepen-pbd-island-guards-ac8e ---
bool should_skip_solve_sleeping_island(const RigidBodySoA& bodies,
bool should_skip_solve_sleeping_island_index(const RigidBodySoA& bodies,
IslandConstraintSolvePreflight preflight_island_constraint_solve(const RigidBodySoA& bodies,
IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(const RigidBodySoA& bodies,
bool should_skip_island_constraint_solve(const RigidBodySoA& bodies,
IslandSleepGraphPreflight preflight_island_sleep_graph(const RigidBodySoA& bodies,
bool should_skip_island_sleep_dispatch(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

// --- deepen additive from deepen-pbd-island-build-sleep-wake-cc0e ---
IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);
bool should_skip_island_solve_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);
bool should_skip_island_solve_sleeping_index(const ContactIslandGraph& graph,
bool should_skip_island_wake_check(const ContactIslandGraph::Island& island);

// --- deepen additive from deepen-pbd-island-guards-2fdd ---
bool should_skip_island_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

// --- deepen additive from deepen-pbd-island-guards-fd7c ---
struct IslandSolveBodyPreflight {
IslandSolveBodyPreflight preflight_island_solve_bodies(const RigidBodySoA& bodies,
IslandSolveBodyPreflight preflight_island_solve_bodies_by_index(const RigidBodySoA& bodies,
bool should_skip_island_solve_job_for_sleep(const IslandSolveJob& job,
bool should_skip_island_sleep(const ContactIslandGraph::Island& island, f32 dt);
bool should_skip_island_sleep_index(const ContactIslandGraph& graph, u32 islandIndex, f32 dt);
bool should_skip_island_wake_index(const ContactIslandGraph& graph, u32 islandIndex);
IslandWakeGraphPreflight preflight_island_wake_graph(const RigidBodySoA& bodies,

// --- deepen additive from deepen-pbd-island-guards-bda2 ---
bool should_skip_island_constraint_solve_index(const RigidBodySoA& bodies,
bool should_skip_island_sleep_check(const ContactIslandGraph::Island& island);
bool should_skip_island_sleep_index(const ContactIslandGraph& graph, u32 islandIndex);
IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

// --- deepen additive from pbd-island-guards-f0a5 ---
struct IslandSolveJobSleepPreflight {
    IslandSolveJobPreflight job{};
struct IslandSleepDispatchPreflight {
bool should_skip_island_solve_sleeping(const ContactIslandGraph::Island& island,
IslandSolveJobSleepPreflight preflight_solve_island_job_with_sleep(const IslandSolveJob& job,
bool should_skip_solve_island_job_with_sleep(const IslandSolveJob& job,
IslandSleepDispatchPreflight preflight_island_dispatch_with_sleep(const ContactIslandGraph& graph,
bool should_skip_island_dispatch_with_sleep(const ContactIslandGraph& graph,

// --- deepen additive from pbd-island-guards-deepen-bcee ---
struct IslandSolveCombinedPreflight {
    IslandConstraintRefsPreflight constraintRefs{};
IslandSleepPreflight preflight_island_sleep_for_solve(const ContactIslandGraph::Island& island,
IslandSleepPreflight preflight_island_sleep_for_solve_by_index(const ContactIslandGraph& graph,
bool should_skip_island_solve_for_sleep(const ContactIslandGraph& graph,
bool should_skip_solve_sleeping_island(const ContactIslandGraph::Island& island,
bool should_skip_island_wake_check(const ContactIslandGraph::Island& island,
IslandSolveCombinedPreflight preflight_island_solve_combined(
bool should_skip_island_solve_combined(const IslandSolveJob& job,

// --- deepen additive from deepen-pbd-island-guards-3045 ---
struct IslandBodyRefsPreflight {
    IslandBodyRefsPreflight bodies{};
IslandBodyRefsPreflight preflight_island_body_refs(const ContactIslandGraph::Island& island,
bool should_skip_island_body_refs(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

// --- deepen additive from pbd-island-sleep-wake-guards-28a6 ---
bool should_skip_island_solve_all_sleeping(const ContactIslandGraph& graph,
bool should_skip_sleeping_island(const ContactIslandGraph::Island& island,
bool should_skip_sleeping_island_index(const ContactIslandGraph& graph,

// --- deepen additive from deepen-pbd-island-guards-6182 ---
    IslandSleepWakePreflight sleepWake{};
struct IslandSleepWakeGraphPreflight {
IslandSleepWakePreflight preflight_island_sleep_wake(const ContactIslandGraph::Island& island,
IslandSleepWakeGraphPreflight preflight_island_sleep_wake_graph(const ContactIslandGraph& graph,
bool should_skip_island_solve_sleep_wake(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

// --- deepen additive from deepen-pbd-island-guards-a489 ---
struct IslandSolveBodyRefsPreflight {
    IslandSolveBodyRefsPreflight bodies{};
IslandSolveBodyRefsPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
IslandSleepWakePreflight preflight_island_sleep_wake_by_index(const ContactIslandGraph& graph,
bool should_skip_island_solve_for_sleep(const ContactIslandGraph::Island& island,
bool should_skip_island_solve_for_sleep_index(const ContactIslandGraph& graph,
bool should_skip_island_sleep_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);
bool should_skip_island_constraint_solve(const IslandSolveJob& job,

// --- deepen additive from pbd-island-guards-deepen-77cf ---
struct IslandSolveRefsPreflight {
    IslandConstraintRefsPreflight constraints{};
IslandSolveRefsPreflight preflight_island_solve_refs(
bool should_skip_island_solve_refs(const ContactIslandGraph::Island& island,

// --- deepen additive from deepen-pbd-island-guards-2105 ---
IslandSolveBodiesPreflight preflight_island_solve_bodies_by_index(const ContactIslandGraph& graph,
struct IslandSolvePreflightCombined {
IslandSolvePreflightCombined preflight_island_solve_combined(

// --- deepen additive from deepen-pbd-island-guards-358e ---
struct IslandSolveParticipationPreflight {
bool should_skip_island_solve_all_sleeping(const ContactIslandGraph::Island& island,
bool should_skip_island_solve_no_participation(const ContactIslandGraph::Island& island,
IslandSolveParticipationPreflight preflight_island_solve_participation(

// --- deepen additive from deepen-pbd-island-guards-a375 ---
struct IslandSolvePassPreflight {
    IslandBodyRefsPreflight bodyRefs{};
bool should_skip_island_dispatch_for_sleep(const ContactIslandGraph& graph,
IslandSolvePassPreflight preflight_island_solve_pass(
bool should_skip_island_solve_pass(const ContactIslandGraph::Island& island,

// --- deepen additive from deepen-pbd-island-guards-9b4f ---
bool should_skip_solve_fully_sleeping_island(const RigidBodySoA& bodies,
IslandSolveSleepPreflight preflight_solve_island_with_sleep(
bool should_skip_solve_island_with_sleep(const RigidBodySoA& bodies,

// --- deepen additive from pbd-island-sleep-wake-guards-c801 ---
bool should_skip_sleeping_island_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

// --- deepen additive from deepen-pbd-island-guards-12c0 ---
bool should_skip_solve_all_sleeping_islands(const ContactIslandGraph& graph, const RigidBodySoA& bodies);
bool should_skip_wake_island(const ContactIslandGraph::Island& island,
IslandSolveBodyPreflight preflight_solve_island_with_bodies(
bool should_skip_solve_island_with_bodies(const ContactIslandGraph::Island& island,

// --- deepen additive from pbd-island-sleep-wake-preflights-1600 ---
struct IslandDispatchBodiesPreflight {
    IslandSleepWakeGraphPreflight sleepWake{};
bool should_skip_inactive_island_solve(const ContactIslandGraph::Island& island,
bool should_skip_inactive_island_solve_index(const ContactIslandGraph& graph,
IslandConstraintRefsPreflight preflight_island_solvable_constraint_refs(
bool should_skip_island_solvable_constraint_refs(const ContactIslandGraph::Island& island,
IslandDispatchBodiesPreflight preflight_island_dispatch_with_bodies(const ContactIslandGraph& graph,
bool should_skip_island_dispatch_with_bodies(const ContactIslandGraph& graph,

// --- deepen additive from deepen-pbd-island-sleep-build-guards-e836 ---
bool should_skip_island_dispatch_for_sleep(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

// --- deepen additive from deepen-pbd-island-guards-9f8d ---
struct IslandConstraintSolveGraphPreflight {
struct IslandSolvePipelinePreflight {
    IslandConstraintSolvePreflight solve{};
bool should_skip_island_constraint_solve_by_index(const ContactIslandGraph& graph,
IslandConstraintSolveGraphPreflight preflight_island_constraint_solve_graph(
bool should_skip_island_constraint_solve_graph(
IslandSolvePipelinePreflight preflight_island_solve_pipeline(
IslandSolvePipelinePreflight preflight_island_solve_pipeline_by_index(
bool should_skip_island_solve_pipeline(const ContactIslandGraph::Island& island,

// --- deepen additive from deepen-pbd-island-guards-73b7 ---
    IslandBuildRejectReason reason = IslandBuildRejectReason::None;
    bool can_build() const { return reason == IslandBuildRejectReason::None && !skipped && !has_unsafe_refs(); }
    IslandConstraintSolvePreflight constraintSolve{};
bool should_skip_island_constraint_solve_index(const ContactIslandGraph& graph,
IslandSolvePipelinePreflight preflight_island_solve_pipeline(const ContactIslandGraph& graph,

// --- deepen additive from deepen-pbd-island-guards-e84e ---
    IslandConstraintSolvePreflight constraint{};

// --- deepen additive from deepen-pbd-island-guards-0f38 ---
struct IslandSleepAwareDispatchPreflight {
IslandSleepAwareDispatchPreflight preflight_island_sleep_aware_dispatch(const ContactIslandGraph& graph,
bool should_skip_island_sleep_aware_dispatch(const ContactIslandGraph& graph,
