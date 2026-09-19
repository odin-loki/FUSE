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
    u32 islandIndex = ContactIslandGraph::invalidIsland;

/// Per-island warm-start outcome (skip vs seed) for parallel batch stubs.
struct IslandWarmStartResult {
    bool warmed = false;

/// Aggregate warm-start counts for graph-level batch guards.
struct IslandWarmStartStats {
    u32 totalIslands = 0;
    u32 warmStartableCount = 0;
    u32 emptyCount = 0;
    u32 noPriorDataCount = 0;

/// Graph-level warm-start preflight for selective per-island seeding.
struct IslandWarmStartGraphPreflight {
    IslandWarmStartStats stats{};

    bool can_warm_start() const { return !skipped && stats.warmStartableCount > 0u; }

/// Aggregate counts from a batch island dispatch pass (B4.4 deepen pass).
struct IslandDispatchBatchResult {
    u32 solvedCount = 0;
    u32 skippedCount = 0;

    u32 attemptedCount() const { return solvedCount + skippedCount; }
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


/// Combined dispatch preflight (graph + timestep guard).
struct IslandDispatchPreflight {
    IslandSolvePreflight solve{};
    bool invalidDt = false;

    bool can_dispatch() const { return !skipped && !invalidDt && solve.can_dispatch(); }

/// Preflight island dispatch from pre-extracted jobs (parallel batch guard).
struct IslandDispatchJobPreflight {
    IslandSolveStats stats{};

    bool can_dispatch() const { return !skipped && !invalidDt && stats.dispatchableCount > 0u; }

/// Batch dispatch summary for parallel iteration stubs.
struct IslandBatchDispatchResult {
    u32 solvedCount = 0;
    u32 skippedCount = 0;
    u32 dispatchableCount = 0;

    bool any_solved() const { return solvedCount > 0u; }

/// Preflight for pre-filtered dispatchable island jobs (B4.4 deepen follow-up).
struct IslandDispatchJobBatchPreflight {
    IslandDispatchPreflight graph{};
    u32 jobCount = 0;
    bool skipped = false;

    bool can_dispatch() const { return !skipped && graph.can_dispatch() && jobCount > 0u; }
};

/// Batch warm-start summary for parallel iteration stubs.
/// Batch warm-start summary for parallel lambda seeding stubs.
struct IslandWarmStartBatchResult {
    u32 warmedCount = 0;
    u32 skippedCount = 0;
    u32 warmStartableCount = 0;
    bool skipped = false;

    bool any_warmed() const { return warmedCount > 0u; }
};

/// Contact impulse warm-start preflight for one island.
struct IslandContactImpulseWarmStartPreflight {
    u32 ownedContactCount = 0;
    u32 nonZeroImpulseCoverage = 0;
    bool skipped = false;

    bool can_warm_start() const { return !skipped && nonZeroImpulseCoverage > 0u; }
};

/// Aggregate contact-impulse warm-start counts for graph-level batch guards.
struct IslandContactImpulseWarmStartStats {
/// Per-island contact-impulse warm-start outcome for parallel batch stubs.
struct IslandContactImpulseResult {
    bool warmed = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;

/// Per-island contact-impulse warm-start preflight.
struct IslandContactImpulsePreflight {
    u32 impulseCoverage = 0;
    bool invalidDt = false;

    bool can_warm_start() const { return !skipped && !invalidDt && impulseCoverage > 0u; }

struct IslandContactImpulseStats {
    u32 totalIslands = 0;
    u32 warmStartableCount = 0;
    u32 emptyCount = 0;
    u32 noImpulseDataCount = 0;
};

/// Graph-level contact-impulse warm-start preflight for selective per-island seeding.
struct IslandContactImpulseWarmStartGraphPreflight {
    IslandContactImpulseWarmStartStats stats{};
    bool skipped = false;

    bool can_warm_start() const { return !skipped && stats.warmStartableCount > 0u; }

/// Batch contact-impulse warm-start summary for parallel iteration stubs.
struct IslandContactImpulseWarmStartBatchResult {
    u32 warmedCount = 0;
    u32 skippedCount = 0;

    bool any_warmed() const { return warmedCount > 0u; }

/// Combined contact-impulse dispatch preflight (graph + timestep guard).
struct IslandContactImpulseDispatchPreflight {
    IslandContactImpulseWarmStartGraphPreflight impulses{};
    bool invalidDt = false;

    bool can_warm_start() const { return !skipped && !invalidDt && impulses.can_warm_start(); }
/// Graph-level contact-impulse warm-start preflight.
struct IslandContactImpulseGraphPreflight {
    IslandContactImpulseStats stats{};

    bool can_warm_start() const { return !skipped && !invalidDt && stats.warmStartableCount > 0u; }
};

struct IslandContactImpulseBatchResult {
    u32 warmStartableCount = 0;


/// Warm-start preflight for per-island lambda seeding (parallel-safe selective seed).
struct IslandWarmStartPreflight {
    u32 ownedDistanceCount = 0;
    u32 ownedContactCount = 0;
    u32 priorDistanceCoverage = 0;
    u32 priorContactCoverage = 0;

    bool can_warm_start() const {
        return !skipped && (priorDistanceCoverage > 0u || priorContactCoverage > 0u);
    }

/// Combined lambda + contact-impulse warm-start preflight for one island.
struct IslandCombinedWarmStartPreflight {
    IslandWarmStartPreflight lambdas{};
    IslandContactImpulseWarmStartPreflight impulses{};
    bool skipped = false;

    bool can_warm_start() const {
        return !skipped && (lambdas.can_warm_start() || impulses.can_warm_start());
    }
};

/// Aggregate contact-impulse warm-start counts for graph-level batch guards.
struct IslandContactImpulseWarmStartStats {
    u32 totalIslands = 0;
    u32 warmStartableCount = 0;
    u32 emptyCount = 0;
    u32 zeroImpulseCount = 0;
};

/// Graph-level contact-impulse warm-start preflight for selective per-island seeding.
struct IslandContactImpulseWarmStartGraphPreflight {
    IslandContactImpulseWarmStartStats stats{};
    bool invalidDt = false;
    bool skipped = false;

    bool can_warm_start() const {
        return !skipped && !invalidDt && stats.warmStartableCount > 0u;
    }
};

/// Sleep-pass diagnostics for one frame/substep (B4.4 deepen).
struct SleepPassStats {
    u32 totalBodies = 0;
    u32 staticCount = 0;
    u32 sleepingCount = 0;
    u32 activeCount = 0;
    u32 sleepCandidateCount = 0;
};

/// Preflight for the sleep detection pass; sets `skipped` when no dynamic bodies remain.
struct SleepPassPreflight {
    SleepPassStats stats{};
    bool skipped = false;

    bool can_sleep_pass() const { return !skipped && stats.activeCount > 0u; }
};

/// Wake-candidate diagnostics for sleeping bodies above velocity thresholds (B4.4 deepen).
struct WakeCandidateStats {
    u32 sleepingCount = 0;
    u32 wakeCandidateCount = 0;
    u32 restingSleepingCount = 0;
};

/// Preflight for bodies that would wake on the next sleep pass evaluation.
struct WakePreflight {
    WakeCandidateStats stats{};
    bool skipped = false;

    bool has_wake_candidates() const { return !skipped && stats.wakeCandidateCount > 0u; }
};

/// Work-buffer capacity preflight before island constraint solve (B4.4 deepen).
struct IslandSolveWorkPreflight {
    u32 bodyCount = 0;
    u32 bufferCapacity = 0;
    bool insufficientBufferCapacity = false;
    bool skipped = false;

    bool can_solve() const { return !skipped && !insufficientBufferCapacity; }
};

/// Per-island constraint index and body-reference preflight (B4.4 deepen).
struct IslandConstraintIndexPreflight {
    u32 ownedContactCount = 0;
    u32 ownedDistanceCount = 0;
    u32 oobContactIndexCount = 0;
    u32 oobDistanceIndexCount = 0;
    u32 oobBodyRefCount = 0;
    bool skipped = false;

    bool indices_valid() const {
        return oobContactIndexCount == 0u && oobDistanceIndexCount == 0u && oobBodyRefCount == 0u;
    }
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

/// Combined lambda + contact-impulse warm-start preflight for one island.
struct IslandCombinedWarmStartPreflight {
    IslandWarmStartPreflight lambdas{};
    IslandContactImpulseWarmStartPreflight impulses{};
    bool skipped = false;

    bool can_warm_start() const {
        return !skipped && (lambdas.can_warm_start() || impulses.can_warm_start());
    }
};

/// Frame-level warm-start preflight for first-substep lambda seeding.
struct FrameWarmStartPreflight {
    u32 distanceSlotCount = 0;
    u32 contactSlotCount = 0;


/// Combined dispatch preflight (graph + timestep guard).
struct IslandDispatchPreflight {
    IslandSolvePreflight solve{};
    bool invalidDt = false;

    bool can_dispatch() const { return !skipped && !invalidDt && solve.can_dispatch(); }

/// Batch dispatch summary for parallel iteration stubs.
struct IslandBatchDispatchResult {
    u32 solvedCount = 0;
    u32 skippedCount = 0;
    u32 dispatchableCount = 0;
    bool skipped = false;

    bool any_solved() const { return solvedCount > 0u; }

/// Batch warm-start summary for parallel iteration stubs.
struct IslandBatchWarmStartResult {
    u32 warmedCount = 0;

    bool any_warmed() const { return warmedCount > 0u; }

/// Per-island contact-impulse warm-start preflight (dt + empty-island guards).
struct IslandContactImpulseWarmStartPreflight {
    u32 nonZeroImpulseCount = 0;

        return !skipped && !invalidDt && ownedContactCount > 0u && nonZeroImpulseCount > 0u;

/// Combined lambda + contact-impulse warm-start preflight for one island.
struct IslandCombinedWarmStartPreflight {
    IslandWarmStartPreflight lambdas{};
    IslandContactImpulseWarmStartPreflight impulses{};

        return !skipped && (lambdas.can_warm_start() || impulses.can_warm_start());

/// Per-job island dispatch preflight (dt + empty/null job guards).
struct IslandSolveJobPreflight {
    u32 constraintCount = 0;

    bool can_dispatch() const { return !skipped && !invalidDt && constraintCount > 0u; }

/// Constraint index coverage for one island solve pass (stale/out-of-range guards).
struct IslandConstraintRefsPreflight {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    u32 validContactCount = 0;

    bool can_solve() const {
        return !skipped && (inRangeContactCount > 0u || inRangeDistanceCount > 0u);

/// Input coverage for island graph build (out-of-range body-index guards).
struct IslandBuildStats {
    u32 bodyCount = 0;
    u32 outOfRangeContactBodyCount = 0;
    u32 outOfRangeDistanceBodyCount = 0;

/// Preflight diagnostics for island graph build inputs (B4.4 deepen).
struct IslandBuildPreflight {
    ContactIslandGraphBuildRejectReason reason = ContactIslandGraphBuildRejectReason::None;
    IslandBuildStats stats{};

    bool has_unsafe_refs() const {
        return stats.outOfRangeContactBodyCount > 0u || stats.outOfRangeDistanceBodyCount > 0u;

    bool can_build() const { return reason == ContactIslandGraphBuildRejectReason::None; }

/// Body participation for one island constraint solve (sleep/static/movable guards).
struct IslandSolveBodiesPreflight {
    u32 inRangeBodyCount = 0;
    u32 staticOrKinematicCount = 0;
    u32 sleepingCount = 0;
    u32 movableCount = 0;

    bool can_solve() const { return !skipped && movableCount > 0u; }

/// Combined constraint-ref + body participation preflight for one island solve pass.
struct IslandConstraintSolvePreflight {
    IslandConstraintRefsPreflight refs{};
    IslandSolveBodiesPreflight bodies{};

    bool can_solve() const { return !skipped && refs.can_solve() && bodies.can_solve(); }

/// Per-island sleep state for solve early-out stubs.
struct IslandSleepPreflight {
    u32 activeDynamicCount = 0;
    bool allSleeping = false;

    bool can_skip_solve() const { return !skipped && allSleeping; }

/// Per-island wake hint when active dynamics neighbor sleeping bodies.
struct IslandWakePreflight {
    bool hasMixedSleepState = false;

    bool should_wake_sleepers() const {
        return !skipped && hasMixedSleepState && activeDynamicCount > 0u;

/// Aggregate sleep counts for graph-level batch guards.
struct IslandSleepGraphStats {
    u32 allSleepingCount = 0;
    u32 mixedSleepCount = 0;
    u32 fullyActiveCount = 0;

/// Graph-level sleep preflight for selective per-island solve skipping.
struct IslandSleepGraphPreflight {
    IslandSleepGraphStats stats{};

    bool has_solveable_islands() const {
        return !skipped && (stats.fullyActiveCount > 0u || stats.mixedSleepCount > 0u);

/// Aggregate wake counts for graph-level batch guards.
struct IslandWakeGraphStats {
    u32 wakeableCount = 0;

/// Graph-level wake preflight for selective per-island sleeper activation.
struct IslandWakeGraphPreflight {
    IslandWakeGraphStats stats{};

    bool can_wake() const { return !skipped && stats.wakeableCount > 0u; }

/// Why island solve dispatch would early-out (B4.4 deepen follow-up pass).
enum class IslandDispatchRejectReason : u8 {
    None = 0,
    NoDispatchableIslands,
    InvalidDt,
    NonFiniteDt,

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

    bool can_dispatch() const { return reason == IslandDispatchRejectReason::None; }

IslandDispatchRejectPreflight preflightIslandDispatchReject(const ContactIslandGraph& graph, f32 dt);

/// Non-mutating dispatch skip predicate — inverse of `can_dispatch` (B4.4 deepen follow-up pass).
bool canSkipIslandDispatch(const ContactIslandGraph& graph, f32 dt);

/// Non-mutating dispatch predicate — mirrors `preflightIslandDispatchReject` (B4.4 deepen follow-up pass).
bool shouldRunIslandDispatch(const ContactIslandGraph& graph, f32 dt);

/// Why one island solve job would reject dispatch (B4.4 deepen follow-up pass).
enum class IslandSolveJobRejectReason : u8 {
    EmptyJob,
    NullIsland,
    ZeroConstraints,

const char* islandSolveJobRejectReasonName(IslandSolveJobRejectReason reason);

IslandSolveJobRejectReason islandSolveJobRejectReason(const IslandSolveJob& job, f32 dt);

bool islandSolveJobRejectsForReason(const IslandSolveJob& job,
                                    IslandSolveJobRejectReason expected);

struct IslandSolveJobRejectPreflight {
    IslandSolveJobRejectReason reason = IslandSolveJobRejectReason::None;

    bool can_dispatch() const { return reason == IslandSolveJobRejectReason::None; }

IslandSolveJobRejectPreflight preflightIslandSolveJobReject(const IslandSolveJob& job, f32 dt);

bool canSkipIslandSolveJob(const IslandSolveJob& job, f32 dt);

bool shouldRunIslandSolveJob(const IslandSolveJob& job, f32 dt);

/// Why one island constraint solve would early-out (B4.4 deepen follow-up pass).
enum class IslandConstraintSolveRejectReason : u8 {
    EmptyIsland,
    NoInRangeRefs,
    NoMovableBodies,

const char* islandConstraintSolveRejectReasonName(IslandConstraintSolveRejectReason reason);

IslandConstraintSolveRejectReason islandConstraintSolveRejectReason(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints);

bool islandConstraintSolveRejectsForReason(
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandConstraintSolveRejectReason expected);

struct IslandConstraintSolveRejectPreflight {
    IslandConstraintSolveRejectReason reason = IslandConstraintSolveRejectReason::None;

    bool can_solve() const { return reason == IslandConstraintSolveRejectReason::None; }

IslandConstraintSolveRejectPreflight preflightIslandConstraintSolveReject(

bool canSkipIslandConstraintSolve(const ContactIslandGraph::Island& island,

bool shouldRunIslandConstraintSolve(const ContactIslandGraph::Island& island,

/// Why per-island sleep solve would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepSolveRejectReason : u8 {
    AllSleeping,

const char* islandSleepSolveRejectReasonName(IslandSleepSolveRejectReason reason);

IslandSleepSolveRejectReason islandSleepSolveRejectReason(const ContactIslandGraph::Island& island,
                                                          const RigidBodySoA& bodies);

bool islandSleepSolveRejectsForReason(const ContactIslandGraph::Island& island,
                                      IslandSleepSolveRejectReason expected);

struct IslandSleepSolveRejectPreflight {
    IslandSleepSolveRejectReason reason = IslandSleepSolveRejectReason::None;
    IslandSleepPreflight sleep{};

    bool can_solve() const { return reason == IslandSleepSolveRejectReason::None; }

IslandSleepSolveRejectPreflight preflightIslandSleepSolveReject(const ContactIslandGraph::Island& island,

bool canSkipIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

bool shouldRunIslandSleepSolve(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Why per-island wake would early-out (B4.4 deepen follow-up pass).
enum class IslandWakeRejectReason : u8 {
    NoMixedSleepState,
    NoActiveDynamic,

const char* islandWakeRejectReasonName(IslandWakeRejectReason reason);

IslandWakeRejectReason islandWakeRejectReason(const ContactIslandGraph::Island& island,

bool islandWakeRejectsForReason(const ContactIslandGraph::Island& island,
                                IslandWakeRejectReason expected);

struct IslandWakeRejectPreflight {
    IslandWakeRejectReason reason = IslandWakeRejectReason::None;
    IslandWakePreflight wake{};

    bool can_wake() const { return reason == IslandWakeRejectReason::None; }

IslandWakeRejectPreflight preflightIslandWakeReject(const ContactIslandGraph::Island& island,

bool canSkipIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

bool shouldRunIslandWake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Why graph-level sleep batching would early-out (B4.4 deepen follow-up pass).
enum class IslandSleepGraphRejectReason : u8 {
    AllIslandsSleepingOrEmpty,

const char* islandSleepGraphRejectReasonName(IslandSleepGraphRejectReason reason);

IslandSleepGraphRejectReason islandSleepGraphRejectReason(const ContactIslandGraph& graph,

bool islandSleepGraphRejectsForReason(const ContactIslandGraph& graph,
                                      IslandSleepGraphRejectReason expected);

struct IslandSleepGraphRejectPreflight {
    IslandSleepGraphRejectReason reason = IslandSleepGraphRejectReason::None;
    IslandSleepGraphPreflight sleep{};

    bool has_solveable_islands() const { return reason == IslandSleepGraphRejectReason::None; }

IslandSleepGraphRejectPreflight preflightIslandSleepGraphReject(const ContactIslandGraph& graph,

bool canSkipIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

bool shouldRunIslandSleepGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Why graph-level wake batching would early-out (B4.4 deepen follow-up pass).
enum class IslandWakeGraphRejectReason : u8 {
    NoWakeableIslands,

const char* islandWakeGraphRejectReasonName(IslandWakeGraphRejectReason reason);

IslandWakeGraphRejectReason islandWakeGraphRejectReason(const ContactIslandGraph& graph,

bool islandWakeGraphRejectsForReason(const ContactIslandGraph& graph,
                                     IslandWakeGraphRejectReason expected);

struct IslandWakeGraphRejectPreflight {
    IslandWakeGraphRejectReason reason = IslandWakeGraphRejectReason::None;
    IslandWakeGraphPreflight wake{};

    bool can_wake() const { return reason == IslandWakeGraphRejectReason::None; }

IslandWakeGraphRejectPreflight preflightIslandWakeGraphReject(const ContactIslandGraph& graph,

bool canSkipIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

bool shouldRunIslandWakeGraph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Why wake-then-dispatch pipeline would early-out (B4.4 deepen follow-up pass).
enum class IslandPipelineDispatchRejectReason : u8 {
    AllIslandsSleeping,

const char* islandPipelineDispatchRejectReasonName(IslandPipelineDispatchRejectReason reason);

IslandPipelineDispatchRejectReason islandPipelineDispatchRejectReason(const ContactIslandGraph& graph,
                                                                      f32 dt);

bool islandPipelineDispatchRejectsForReason(const ContactIslandGraph& graph,
                                              IslandPipelineDispatchRejectReason expected);

struct IslandPipelineDispatchPreflight {
    IslandPipelineDispatchRejectReason reason = IslandPipelineDispatchRejectReason::None;
    IslandWakeGraphRejectPreflight wake{};
    IslandSleepGraphRejectPreflight sleep{};
    IslandDispatchRejectPreflight dispatch{};

    bool can_dispatch() const { return reason == IslandPipelineDispatchRejectReason::None; }

IslandPipelineDispatchPreflight preflightIslandPipelineDispatch(const ContactIslandGraph& graph,

bool canSkipIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt);

bool shouldRunIslandPipelineDispatch(const ContactIslandGraph& graph, const RigidBodySoA& bodies, f32 dt);

/// Wake sleepers then dispatch all islands only when pipeline preflight allows (B4.4 deepen follow-up pass).
IslandBatchDispatchResult dispatch_island_pipeline_guarded(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Aggregate contact-impulse warm-start counts for graph-level batch guards.
struct IslandContactImpulseWarmStartStats {
    u32 noImpulseCount = 0;
    u32 totalIslands = 0;
    u32 warmStartableCount = 0;
    u32 emptyCount = 0;
};

/// Graph-level contact-impulse warm-start preflight for selective per-island seeding.
struct IslandContactImpulseWarmStartGraphPreflight {
    IslandContactImpulseWarmStartStats stats{};

    bool can_warm_start() const { return !skipped && !invalidDt && stats.warmStartableCount > 0u; }
/// Structured outcome for guarded island dispatch (B4.4 deepen).
struct IslandSolveDispatchResult {
    bool skipped = true;

/// Preflight summary for selective island warm-start guards (B4.4 deepen).
    bool hasDistanceLambdas = false;
    bool hasContactLambdas = false;
    bool hasContactImpulses = false;

/// Aggregate warm-start coverage for parallel island seeding (B4.4 deepen).
struct IslandWarmStartStats {

/// Bundle preflight diagnostics and dispatch indices for one iteration pass (B4.4 deepen).
struct IslandSolveDispatchPlan {
    IslandSolvePreflight preflight{};
    std::vector<u32> dispatchIndices{};

    bool can_dispatch() const { return preflight.can_dispatch(); }

/// Per-island warm-start outcome (skip vs seed) for parallel batch stubs.
struct IslandWarmStartResult {
    bool warmed = false;
    u32 islandIndex = ContactIslandGraph::invalidIsland;

/// Aggregate warm-start counts for graph-level batch guards (B4.4 deepen pass).


/// Aggregate combined warm-start counts for graph-level batch guards.
struct IslandCombinedWarmStartStats {
    u32 totalIslands = 0;
    u32 warmStartableCount = 0;
    u32 emptyCount = 0;
    u32 noPriorDataCount = 0;
};

/// Graph-level warm-start preflight for selective per-island seeding (B4.4 deepen pass).
struct IslandWarmStartGraphPreflight {
    IslandWarmStartStats stats{};
    bool skipped = false;

    bool can_warm_start() const { return !skipped && stats.warmStartableCount > 0u; }

/// Graph-level warm-start preflight for selective per-island seeding (B4.4 deepen).
struct GraphWarmStartPreflight {
    u32 dispatchableCount = 0;
    u32 skippedEmptyCount = 0;
    u32 priorDistanceCoverage = 0;
    u32 priorContactCoverage = 0;

    bool can_warm_start() const {
        return !skipped && dispatchableCount > 0u &&
               (priorDistanceCoverage > 0u || priorContactCoverage > 0u);
    }

/// Batch warm-start summary for parallel island seeding stubs.
struct IslandWarmStartBatchResult {
    u32 warmedCount = 0;
    u32 skippedNoPriorCount = 0;

    bool any_warmed() const { return warmedCount > 0u; }

/// Batch warm-start summary for parallel selective seeding stubs.
    u32 skippedCount = 0;
    u32 warmStartableCount = 0;


/// Lightweight view for parallel warm-start dispatch (B4.4 deepen).
struct IslandWarmStartJob {
    u32 islandIndex = ContactIslandGraph::invalidIsland;
    u32 ownedDistanceCount = 0;
    u32 ownedContactCount = 0;
    bool empty = true;
    bool canWarmStart = false;
    const ContactIslandGraph::Island* island = nullptr;



/// Per-island contact impulse warm-start preflight (dt + empty-island guard).
struct IslandContactImpulseWarmStartPreflight {
    bool invalidDt = false;

    bool can_warm_start() const { return !skipped && !invalidDt && ownedContactCount > 0u; }

/// Constraint index validation for island solve preflight (B4.4 deepen).
struct IslandConstraintIndexValidation {
    u32 invalidContactIndices = 0;
    u32 invalidDistanceIndices = 0;
    bool valid = true;

    bool has_invalid_indices() const { return !valid; }

/// Per-island solve preflight including constraint index checks (B4.4 deepen).
struct IslandSolveJobPreflight {
    IslandSolveJob job{};
    IslandConstraintIndexValidation indices{};

    bool can_dispatch() const {
        return !skipped && !job.empty && job.island != nullptr && job.constraintCount > 0u && indices.valid;

/// Per-island contact-impulse warm-start preflight (B4.4 deepen).
    u32 priorImpulseCoverage = 0;

        return !skipped && !invalidDt && priorImpulseCoverage > 0u;

/// Aggregate contact-impulse warm-start counts for graph-level batch guards (B4.4 deepen).
struct IslandContactImpulseWarmStartStats {
    u32 totalIslands = 0;
    u32 emptyCount = 0;
    u32 noImpulseDataCount = 0;

/// Graph-level contact-impulse warm-start preflight (B4.4 deepen).
struct IslandContactImpulseWarmStartGraphPreflight {
    IslandContactImpulseWarmStartStats stats{};

/// Graph-level combined warm-start preflight for batch guards.
struct IslandCombinedWarmStartGraphPreflight {
    IslandCombinedWarmStartStats stats{};

        return !skipped && !invalidDt && stats.warmStartableCount > 0u;

/// Per-island contact-impulse warm-start outcome (B4.4 deepen).
struct IslandContactImpulseWarmStartResult {
    bool warmed = false;

/// Combined lambda + contact-impulse warm-start preflight (B4.4 deepen).
struct IslandCombinedWarmStartPreflight {
    IslandWarmStartPreflight lambda{};
    IslandContactImpulseWarmStartPreflight impulse{};

        return !skipped && (lambda.can_warm_start() || impulse.can_warm_start());

/// Per-island solve input bounds preflight (B4.4 deepen pass).
struct IslandSolveInputsPreflight {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    bool contactsInRange = true;
    bool distancesInRange = true;

    bool can_solve() const { return !skipped && contactsInRange && distancesInRange; }

/// Contact-impulse warm-start preflight for per-island seeding (B4.4 deepen pass).
struct IslandContactImpulsePreflight {
    u32 nonZeroImpulseCount = 0;

    bool can_warm_start() const { return !skipped && !invalidDt && nonZeroImpulseCount > 0u; }

/// Combined lambda + contact-impulse warm-start preflight (B4.4 deepen pass).
    IslandContactImpulsePreflight impulse{};

    bool can_warm_start() const { return !skipped && (lambda.can_warm_start() || impulse.can_warm_start()); }

/// Per-island contact-impulse warm-start outcome (B4.4 deepen pass).

/// Graph-level contact-impulse warm-start stats (B4.4 deepen pass).
    u32 impulseSeedableCount = 0;
    u32 invalidDtCount = 0;

/// Graph-level contact-impulse warm-start preflight (B4.4 deepen pass).

    bool can_warm_start() const { return !skipped && stats.impulseSeedableCount > 0u; }

/// Per-index dispatch preflight combining job extraction and timestep validity (B4.4 deepen).
struct IslandJobDispatchPreflight {

        return !skipped && !invalidDt && !job.empty && job.island != nullptr && job.constraintCount > 0u;



/// Per-island contact-impulse warm-start outcome (skip vs seed) for parallel batch stubs.

    u32 seedableContactCount = 0;

        return !skipped && !invalidDt && seedableContactCount > 0u;

/// Aggregate contact-impulse warm-start counts for graph-level batch guards.
struct IslandContactImpulseStats {
    u32 seedableCount = 0;
    u32 noImpulseCount = 0;

/// Graph-level contact-impulse warm-start preflight for selective per-island seeding.
struct IslandContactImpulseGraphPreflight {
    IslandContactImpulseStats stats{};

    bool can_warm_start() const { return !skipped && !invalidDt && stats.seedableCount > 0u; }

/// Batch contact-impulse warm-start summary for parallel iteration stubs.
struct IslandContactImpulseBatchResult {


/// Combined lambda + contact-impulse warm-start preflight for one island.

        if (skipped) {
            return false;
        const bool hasPriorLambdas = lambda.can_warm_start();
        const bool hasContactPass = impulse.ownedContactCount > 0u && !impulse.invalidDt;
        return hasPriorLambdas || hasContactPass;

/// Per-island solve job preflight (B4.4 deepen pass).
    bool outOfRange = false;
    bool empty = false;
    bool dispatchable = false;

    bool can_dispatch() const { return !skipped && dispatchable; }




    bool can_warm_start() const { return !skipped && !invalidDt && stats.warmStartableCount > 0u; }

/// Contact-impulse warm-start preflight for per-island selective seeding.
    u32 impulseCoverage = 0;

    bool can_warm_start() const { return !skipped && !invalidDt && impulseCoverage > 0u; }

struct IslandBatchContactImpulseWarmStartResult {


/// Per-job dispatch preflight combining timestep validity and solve-job guards.
struct IslandDispatchJobPreflight {
    bool emptyJob = false;

    bool can_dispatch() const { return !skipped && !invalidDt && !emptyJob; }
    u32 noDataCount = 0;

/// Graph-level combined warm-start preflight for selective per-island seeding.


/// Preflight for island solve job index validity against contact/distance buffers.
    u32 validContactCount = 0;
    u32 staleContactCount = 0;
    u32 validDistanceCount = 0;
    u32 staleDistanceCount = 0;
    bool hasStaleIndices = false;

    bool can_solve() const {
        return !skipped && !hasStaleIndices && (validContactCount > 0u || validDistanceCount > 0u);

/// Dispatch preflight for one island index (job extraction + timestep guard).
struct IslandDispatchIndexPreflight {


};

/// True when `islandIndex` is in range for `extract_island`.
bool island_index_valid(const ContactIslandGraph& graph, u32 islandIndex);

/// True when `islandIndex` is in range and the island carries constraints.
bool is_dispatchable_island_index(const ContactIslandGraph& graph, u32 islandIndex);

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
bool should_skip_island_solve(const IslandSolveJob& job);

/// Extract one island solve job; out-of-range and empty islands are flagged for early skip.
IslandSolveJob extract_island(const ContactIslandGraph& graph, u32 islandIndex);

/// Batch extract all island jobs (parallel dispatch prep stub).
std::vector<IslandSolveJob> extract_island_jobs(const ContactIslandGraph& graph);

/// Keep only jobs that pass `should_solve_island` (parallel dispatch prep stub).
std::vector<IslandSolveJob> filter_dispatchable_jobs(const std::vector<IslandSolveJob>& jobs);

/// Summarize constrained vs empty islands for dispatch prep and early-out guards.
IslandSolveStats compute_island_solve_stats(const ContactIslandGraph& graph);

/// Count islands that pass `should_solve_island` (non-empty, bound, in-range).
u32 count_dispatchable_islands(const ContactIslandGraph& graph);

/// True when at least one island would be dispatched this substep.
bool has_dispatchable_islands(const ContactIslandGraph& graph);

/// True when every island is constraint-free (all empty).
bool all_islands_empty(const ContactIslandGraph& graph);

/// Count islands with no contacts or distance constraints.
u32 empty_island_count(const ContactIslandGraph& graph);

/// True when at least one island is constraint-free (lone body stub).
bool graph_has_empty_islands(const ContactIslandGraph& graph);

/// True when at least one island carries constraints.
bool graph_has_constrained_islands(const ContactIslandGraph& graph);
/// True when `flags` include `RB_SLEEPING`.
bool is_body_sleeping(u32 flags);

/// True when `flags` include static or kinematic bits.
bool is_body_static_or_kinematic(u32 flags);

/// True when a dynamic body is below sleep velocity thresholds.
bool is_sleep_candidate_body(const RigidBodySoA& bodies,
                             u32 index,
                             f32 sleepLinearThreshold,
                             f32 sleepAngularThreshold);

/// True when a sleeping body exceeds wake velocity thresholds.
bool is_wake_candidate_body(const RigidBodySoA& bodies,

/// Count bodies currently marked `RB_SLEEPING`.
u32 count_sleeping_bodies(const RigidBodySoA& bodies);

/// Preflight sleep detection pass; sets `skipped` when no active dynamic bodies exist.
SleepPassPreflight preflight_sleep_pass(const RigidBodySoA& bodies,

/// Early-out guard for sleep detection when all dynamic bodies are static or sleeping.
bool should_skip_sleep_pass(const RigidBodySoA& bodies,

/// Preflight wake candidates among sleeping bodies above velocity thresholds.
WakePreflight preflight_wake_candidates(const RigidBodySoA& bodies,

/// True when both body indices are in range for constraint accumulation.
bool is_valid_constraint_body_pair(const RigidBodySoA& bodies, u32 bodyA, u32 bodyB);

/// True when every body in the island is sleeping or static/kinematic.
bool island_all_bodies_inactive(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// True when the island has at least one awake dynamic body.
bool island_has_active_bodies(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Optional early-out guard when all island members are inactive (sleeping/static).
bool should_skip_island_solve_all_inactive(const RigidBodySoA& bodies,
                                           const ContactIslandGraph::Island& island);

/// Preflight work-buffer capacity for island constraint solve.
IslandSolveWorkPreflight preflight_island_solve_work(const RigidBodySoA& bodies,
                                                     const SolverWorkBuffers& workBuffers);

/// Preflight island constraint indices and body references before solve.
IslandConstraintIndexPreflight preflight_island_constraint_indices(
    const ContactIslandGraph::Island& island,
    const RigidBodySoA& bodies,
    u32 contactCount,
    u32 distanceCount);

/// Preflight island constraint indices by island index; out-of-range indices are marked skipped.
IslandConstraintIndexPreflight preflight_island_constraint_indices_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,

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

/// True when dt is positive for contact impulse warm-start seeding.

/// Preflight island solve dispatch; sets `skipped` when no islands need work.
IslandSolvePreflight preflight_island_solve(const ContactIslandGraph& graph);

/// Preflight island dispatch including timestep validity.
IslandDispatchPreflight preflight_island_dispatch(const ContactIslandGraph& graph, f32 dt);

/// Preflight one island index for guarded dispatch (job extract + dt validity).
IslandJobDispatchPreflight preflight_dispatch_island_index(const ContactIslandGraph& graph,
                                                           u32 islandIndex,
                                                           f32 dt);

/// Early-out guard for per-index dispatch (empty, out-of-range, or invalid dt).
bool should_skip_dispatch_island_index(const ContactIslandGraph& graph, u32 islandIndex, f32 dt);
/// Summarize constrained vs empty jobs for dispatch prep without graph rescan.
IslandSolveStats compute_island_solve_stats_from_jobs(const std::vector<IslandSolveJob>& jobs);

/// Count jobs that pass `should_solve_island`.
u32 count_dispatchable_jobs(const std::vector<IslandSolveJob>& jobs);

/// True when at least one pre-extracted job would be dispatched.
bool has_dispatchable_jobs(const std::vector<IslandSolveJob>& jobs);

/// Preflight dispatch from pre-extracted jobs including timestep validity.
IslandDispatchJobPreflight preflight_island_dispatch_from_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt);

/// Early-out guard for dispatch loop over pre-extracted jobs.
bool should_skip_island_dispatch_from_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt);

/// Early-out guard for the island solve loop when nothing is dispatchable.
bool should_skip_island_solve(const ContactIslandGraph& graph);

/// Early-out guard for solve by island index (empty or out-of-range).
bool should_skip_island_solve_index(const ContactIslandGraph& graph, u32 islandIndex);

/// Early-out guard combining graph preflight and timestep validity.
bool should_skip_island_dispatch(const ContactIslandGraph& graph, f32 dt);
/// True when every island in the graph is constraint-free (empty-island fast path).

/// True when stats report zero dispatchable islands (early-out before parallel_for).
bool island_solve_stats_has_work(const IslandSolveStats& stats);

/// True when the graph has no islands worth dispatching this substep.
bool should_skip_all_island_solves(const ContactIslandGraph& graph);

/// Preflight one dispatch job including timestep validity.
IslandDispatchJobPreflight preflight_dispatch_island_job(const IslandSolveJob& job, f32 dt);

/// Early-out guard for a single dispatch job (empty job or invalid dt).
bool should_skip_dispatch_island_job(const IslandSolveJob& job, f32 dt);

/// Collect island indices that pass `should_solve_island` (parallel dispatch prep).
std::vector<u32> collect_dispatchable_island_indices(const ContactIslandGraph& graph);

/// Collect only dispatchable island jobs (filters `extract_island_jobs`).
std::vector<IslandSolveJob> collect_dispatchable_island_jobs(const ContactIslandGraph& graph);
/// Lightweight preflight before per-island solve dispatch (index + non-empty guards).
bool preflight_island_solve(const ContactIslandGraph& graph, u32 islandIndex);

/// Keep only jobs that pass `should_solve_island` (parallel dispatch prep stub).
std::vector<IslandSolveJob> filter_dispatchable_jobs(const std::vector<IslandSolveJob>& jobs);

/// Guarded serial dispatch over all islands; returns count of islands actually solved.
u32 dispatch_solve_all_islands(RigidBodySoA& bodies,
                               const ContactIslandGraph& graph,
/// True when a single island index would be dispatched this substep.
bool island_index_dispatchable(const ContactIslandGraph& graph, u32 islandIndex);

/// Build dispatch plan from graph (preflight + collected indices).
IslandSolveDispatchPlan build_island_solve_dispatch_plan(const ContactIslandGraph& graph);

/// Dispatch using a pre-built plan; returns count of islands actually solved.
u32 dispatch_islands_from_plan(RigidBodySoA& bodies,
                               const IslandSolveDispatchPlan& plan,
/// Early-out guard for job-based dispatch (empty job or invalid dt).
bool should_skip_island_dispatch_job(const IslandSolveJob& job, f32 dt);

/// Guarded dispatch from a pre-extracted job; skips empty jobs and invalid dt.
bool dispatch_solve_island_job(RigidBodySoA& bodies,
                               const IslandSolveJob& job,
                               SolverWorkBuffers& workBuffers,
                               const std::vector<DistanceConstraint>& distanceConstraints,
                               f32 dt,
                               f32 contactCompliance,
                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// True when an island owns constraints worth warm-start seeding.
bool should_warm_start_island(const ContactIslandGraph::Island& island);

/// True when prior lambda buffers carry reusable warm-start slots.
bool has_prior_lambda_warm_start(const std::vector<f32>& priorDistanceLambdas,
                                 const std::vector<f32>& priorContactLambdas);

/// Preflight before frame-wide lambda warm-start; false when nothing can be seeded.
bool preflight_frame_lambda_warm_start(const std::vector<DistanceConstraint>& distanceConstraints,
                                       const std::vector<f32>& priorDistanceLambdas,
/// Filter pre-extracted jobs to the dispatchable subset (parallel prep stub).

/// Guarded dispatch using a pre-extracted job; skips invalid, empty, and null-island jobs.
/// Guarded job dispatch with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_job_result(RigidBodySoA& bodies,

/// Batch dispatch over pre-collected jobs; returns solve/skip counts.
IslandBatchDispatchResult dispatch_dispatchable_jobs(
    RigidBodySoA& bodies,
    const std::vector<IslandSolveJob>& jobs,
/// Validate contact/distance index ranges owned by one island.
IslandConstraintIndexValidation validate_island_constraint_indices(
    const ContactIslandGraph::Island& island,
    u32 contactCount,
    u32 distanceConstraintCount);

/// Preflight one island solve job including constraint index validation.
IslandSolveJobPreflight preflight_island_solve_job(const ContactIslandGraph& graph,
                                                   u32 islandIndex,

/// Preflight one island solve job by index; out-of-range indices are marked skipped.
IslandSolveJobPreflight preflight_island_solve_by_index(const ContactIslandGraph& graph,

/// Early-out guard when island job has out-of-range constraint indices.
bool should_skip_island_solve_invalid_indices(const IslandSolveJob& job,

/// Early-out guard for island solve by index (empty, out-of-range, or invalid indices).
bool should_skip_island_solve_index(const ContactIslandGraph& graph,
/// Preflight one island solve job without graph lookup (parallel dispatch stub).
IslandSolveJobPreflight preflight_island_solve_job(const IslandSolveJob& job);
/// Preflight island solve job index validity against contact/distance buffers.
IslandSolveJobPreflight preflight_solve_island_job(const IslandSolveJob& job,
                                                   const std::vector<narrowphase::ContactManifold>& contacts,
                                                   const std::vector<DistanceConstraint>& distanceConstraints);

/// Preflight island solve job by island index; out-of-range indices are marked skipped.
IslandSolveJobPreflight preflight_solve_island_job_by_index(const ContactIslandGraph& graph,

/// True when the job references contact or distance indices outside the provided buffers.
bool has_stale_island_indices(const IslandSolveJob& job,

/// Early-out guard for solve jobs with stale contact/distance indices.
bool should_skip_solve_island_job_stale(const IslandSolveJob& job,

/// Preflight dispatch for one island index (job extraction + timestep guard).
IslandDispatchIndexPreflight preflight_dispatch_island_by_index(const ContactIslandGraph& graph,
                                                                f32 dt);

/// Early-out guard for dispatch by island index (empty, out-of-range, or invalid dt).
bool should_skip_dispatch_island_index(const ContactIslandGraph& graph, u32 islandIndex, f32 dt);
/// Preflight dispatch using pre-collected dispatchable jobs (B4.4 deepen follow-up).
IslandDispatchJobBatchPreflight preflight_dispatchable_island_jobs(const ContactIslandGraph& graph,

/// Early-out when pre-collected jobs are empty or dispatch is blocked (B4.4 deepen follow-up).
bool should_skip_dispatchable_island_jobs(const std::vector<IslandSolveJob>& jobs, f32 dt);

/// Batch guarded dispatch over pre-filtered jobs; returns count of islands actually solved.
u32 dispatch_dispatchable_island_jobs(RigidBodySoA& bodies,

/// Batch guarded dispatch over pre-filtered jobs with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_dispatchable_island_jobs_result(

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
/// Guarded dispatch from an extracted job view (parallel worker stub).
bool dispatch_solve_island_job(RigidBodySoA& bodies,
                               const IslandSolveJob& job,
                               SolverWorkBuffers& workBuffers,
                               const std::vector<DistanceConstraint>& distanceConstraints,
                               f32 dt,
                               f32 contactCompliance,
                               const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Guarded dispatch from a pre-extracted job with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_job_result(RigidBodySoA& bodies,

/// Guarded dispatch with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_result(RigidBodySoA& bodies,
                                                  const ContactIslandGraph& graph,
                                                  u32 islandIndex,

/// Batch guarded dispatch over all islands; returns count of islands actually solved.
u32 dispatch_all_islands(RigidBodySoA& bodies,

/// Batch guarded dispatch from pre-extracted jobs (parallel worker stub).
IslandBatchDispatchResult dispatch_island_jobs_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    const std::vector<IslandSolveJob>& jobs,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Batch guarded dispatch with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_islands_result(
    RigidBodySoA& bodies,
/// Guarded dispatch with structured skip/solve outcome.
IslandSolveDispatchResult dispatch_solve_island_result(RigidBodySoA& bodies,

/// Batch guarded dispatch with per-outcome solved/skipped counts.
IslandDispatchBatchResult dispatch_all_islands_batch(RigidBodySoA& bodies,
                                                    const ContactIslandGraph& graph,
                                                    SolverWorkBuffers& workBuffers,
                                                    const std::vector<DistanceConstraint>& distanceConstraints,
                                                    f32 dt,
                                                    f32 contactCompliance,
                                                    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);
/// Guarded dispatch over an explicit island index list (parallel worker batch stub).
u32 dispatch_islands_at_indices(RigidBodySoA& bodies,
                                const std::vector<u32>& islandIndices,

/// Guarded dispatch over explicit indices with skip/solve counts.
IslandBatchDispatchResult dispatch_islands_at_indices_result(
/// Batch guarded dispatch over pre-filtered island jobs; returns count of islands actually solved.
u32 dispatch_island_jobs(RigidBodySoA& bodies,
                         const std::vector<IslandSolveJob>& jobs,
/// Guarded dispatch from an extracted solve job; skips empty jobs and invalid dt.
bool dispatch_solve_island_job(RigidBodySoA& bodies,
                               const IslandSolveJob& job,

/// Guarded dispatch from a solve job with explicit skip/solve outcome.
IslandDispatchResult dispatch_solve_island_job_result(RigidBodySoA& bodies,
/// Batch guarded dispatch over pre-collected island jobs; returns count of islands actually solved.
u32 dispatch_all_island_jobs(RigidBodySoA& bodies,

/// Batch guarded dispatch over pre-collected island jobs with explicit skip/solve counts.
IslandBatchDispatchResult dispatch_all_island_jobs_result(

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

/// Extract one warm-start job; out-of-range and empty islands are flagged for early skip.
IslandWarmStartJob extract_warm_start_job(const ContactIslandGraph& graph,
                                          u32 islandIndex,
                                          const std::vector<f32>& priorDistanceLambdas,
                                          const std::vector<f32>& priorContactLambdas = {});

/// Batch extract warm-start jobs (parallel seeding prep stub).
std::vector<IslandWarmStartJob> extract_warm_start_jobs(const ContactIslandGraph& graph,
                                                        const std::vector<f32>& priorDistanceLambdas,
                                                        const std::vector<f32>& priorContactLambdas = {});

/// True when a warm-start job should seed lambda slots.
bool should_warm_start_island(const IslandWarmStartJob& job);

/// Inverse of `should_warm_start_island` — empty, null-island, or no-prior fast path.
bool should_skip_warm_start_island_job(const IslandWarmStartJob& job);

/// Keep only jobs that pass `should_warm_start_island`.
std::vector<IslandWarmStartJob> filter_warm_startable_jobs(const std::vector<IslandWarmStartJob>& jobs);

/// Early-out guard for per-island lambda warm-start on empty islands.
bool should_skip_warm_start_island(const ContactIslandGraph::Island& island);

/// Early-out guard for warm-start by island index (empty or out-of-range).
bool should_skip_warm_start_island_index(const ContactIslandGraph& graph, u32 islandIndex);

/// Guarded warm-start; returns false when the island is empty or has no prior data.
bool warm_start_island_lambdas_guarded(SolverWorkBuffers& workBuffers,
                                       const ContactIslandGraph::Island& island,
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
/// Preflight graph warm-start; sets `skipped` when no prior lambda data exists.
GraphWarmStartPreflight preflight_graph_warm_start(const ContactIslandGraph& graph,

/// Early-out guard for graph-level warm-start when no prior data exists.
bool should_skip_graph_warm_start(const std::vector<f32>& priorDistanceLambdas,

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

/// Batch guarded warm-start over warm-startable islands; returns count seeded.
/// Batch guarded warm-start over all warm-startable islands; returns count seeded.
u32 warm_start_all_islands_guarded(SolverWorkBuffers& workBuffers,

/// Batch guarded warm-start with explicit skip/seed counts.
IslandBatchWarmStartResult warm_start_all_islands_result(SolverWorkBuffers& workBuffers,

/// Preflight contact-impulse warm-start for one island; sets `skipped` for empty islands.
IslandContactImpulseWarmStartPreflight preflight_warm_start_contact_impulses(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Preflight contact-impulse warm-start by island index; out-of-range indices are marked skipped.
IslandContactImpulseWarmStartPreflight preflight_warm_start_contact_impulses_by_index(

/// Summarize warm-startable vs empty/no-impulse islands for graph-level impulse guards.
IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(

/// Count islands that pass per-island contact-impulse warm-start preflight.
u32 count_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,

/// True when at least one island can seed from non-zero contact impulses.
bool has_warm_startable_contact_impulse_islands(const ContactIslandGraph& graph,

/// Graph-level contact-impulse warm-start preflight; sets `skipped` when nothing can seed.
IslandContactImpulseWarmStartGraphPreflight preflight_warm_start_contact_impulses_graph(

/// Early-out guard for graph-level contact-impulse warm-start batching.
bool should_skip_warm_start_contact_impulses_graph(const ContactIslandGraph& graph,

/// Collect island indices that pass per-island contact-impulse warm-start preflight.
std::vector<u32> collect_warm_startable_contact_impulse_island_indices(

/// Summarize warm-startable vs empty/zero-impulse islands for contact-impulse batch guards.
IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Count islands that pass contact-impulse warm-start preflight.
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

/// Collect island indices that pass contact-impulse warm-start preflight.
std::vector<u32> collect_warm_startable_contact_impulse_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Summarize contact-impulse warm-startable vs empty/no-impulse islands for batch guards.
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
std::vector<u32> collect_contact_impulse_warm_startable_island_indices(
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Early-out guard for per-island contact-impulse warm-start (empty island or invalid dt).
bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island, f32 dt);

/// Early-out guard using contact data (empty island, invalid dt, or no non-zero impulses).
bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island,

/// Early-out guard for contact-impulse warm-start by island index (empty, out-of-range, or invalid dt).
bool should_skip_warm_start_contact_impulses_index(const ContactIslandGraph& graph,

/// Summarize contact-impulse warm-startable vs empty/no-impulse islands for batch guards.
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

/// Preflight combined lambda + contact-impulse warm-start for one island.
IslandCombinedWarmStartPreflight preflight_warm_start_combined_island(
    f32 dt,

/// Batch guarded warm-start with explicit skip/seed counts.
IslandWarmStartBatchResult warm_start_all_islands_result(SolverWorkBuffers& workBuffers,
                                                           const ContactIslandGraph& graph,
                                                           const std::vector<f32>& priorDistanceLambdas,
                                                           const std::vector<f32>& priorContactLambdas = {});

/// Guarded warm-start from an extracted warm-start job view.
bool warm_start_island_job_guarded(SolverWorkBuffers& workBuffers,
                                   const IslandWarmStartJob& job,
                                   const std::vector<f32>& priorDistanceLambdas,
                                   const std::vector<f32>& priorContactLambdas = {});

/// Batch guarded warm-start with explicit warmed/skipped counts.
/// Batch guarded warm-start with explicit skip/seed counts.
IslandWarmStartBatchResult warm_start_all_islands_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas = {});

/// Preflight contact impulse warm-start for one island; sets `skipped` for empty islands.
IslandContactImpulseWarmStartPreflight preflight_contact_impulse_warm_start_island(
    const ContactIslandGraph::Island& island,
    const std::vector<narrowphase::ContactManifold>& contacts);

/// Preflight contact impulse warm-start by island index; out-of-range indices are marked skipped.
IslandContactImpulseWarmStartPreflight preflight_contact_impulse_warm_start_island_by_index(
    u32 islandIndex,

/// Summarize impulse warm-startable vs empty/no-impulse islands for batch guards.
IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(

/// Count islands that pass per-island contact-impulse warm-start preflight.
u32 count_contact_impulse_warm_startable_islands(

/// True when at least one island can seed from non-zero contact impulses.
bool has_contact_impulse_warm_startable_islands(

/// Graph-level contact-impulse warm-start preflight; sets `skipped` when nothing can seed.
IslandContactImpulseWarmStartGraphPreflight preflight_contact_impulse_warm_start_graph(

/// Preflight contact-impulse warm-start including timestep validity.
IslandContactImpulseDispatchPreflight preflight_contact_impulse_dispatch(
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt);

/// Early-out guard for graph-level contact-impulse warm-start batching.
bool should_skip_contact_impulse_warm_start_graph(

/// Early-out guard combining graph preflight and timestep validity.
bool should_skip_contact_impulse_dispatch(const ContactIslandGraph& graph,

/// Early-out guard for per-island contact impulse warm-start on empty islands.
bool should_skip_contact_impulse_warm_start_island(const ContactIslandGraph::Island& island);

/// Early-out guard for contact impulse warm-start by island index (empty or out-of-range).
bool should_skip_contact_impulse_warm_start_island_index(const ContactIslandGraph& graph, u32 islandIndex);

/// Combined lambda + impulse warm-start preflight for one island.
IslandCombinedWarmStartPreflight preflight_warm_start_island_combined(
IslandWarmStartBatchResult warm_start_all_islands_result(SolverWorkBuffers& workBuffers,

IslandContactImpulseWarmStartPreflight preflight_warm_start_contact_impulses(

/// Early-out guard for contact impulse warm-start (empty island or invalid dt).
bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island, f32 dt);

/// Guarded contact impulse warm-start by island index.
bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,

/// Graph-level guarded contact impulse warm-start; returns islands actually seeded.
u32 warm_start_graph_contact_impulses_guarded(SolverWorkBuffers& workBuffers,

/// Combined warm-start preflight by island index; out-of-range indices are marked skipped.
IslandCombinedWarmStartPreflight preflight_warm_start_island_combined_by_index(

/// Combined lambda + contact-impulse warm-start preflight for one island.
    f32 dt,
/// Preflight combined warm-start by island index; out-of-range indices are marked skipped.
IslandCombinedWarmStartPreflight preflight_warm_start_combined_island_by_index(
    const ContactIslandGraph& graph,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas = {});

/// Summarize combined warm-startable vs empty/no-prior islands for batch guards.
IslandCombinedWarmStartStats compute_island_combined_warm_start_stats(

/// Count islands that pass per-island combined warm-start preflight.
u32 count_combined_warm_startable_islands(const ContactIslandGraph& graph,

/// True when at least one island can seed from prior lambda or contact-impulse data.
bool has_combined_warm_startable_islands(const ContactIslandGraph& graph,

/// Graph-level combined warm-start preflight; sets `skipped` when nothing can seed.
IslandCombinedWarmStartGraphPreflight preflight_warm_start_combined_graph(

/// Early-out guard for graph-level combined warm-start batching.
bool should_skip_warm_start_combined_graph(const ContactIslandGraph& graph,

/// Collect island indices that pass per-island combined warm-start preflight.
std::vector<u32> collect_combined_warm_startable_island_indices(
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas = {});

/// Early-out guard for combined warm-start by island index (empty, out-of-range, or invalid dt).
bool should_skip_warm_start_combined_island_index(const ContactIslandGraph& graph,

/// Summarize warm-startable vs empty/no-prior islands for combined batch guards.
IslandCombinedWarmStartStats compute_island_combined_warm_start_stats(

/// Count islands that pass combined warm-start preflight.
u32 count_warm_startable_combined_islands(const ContactIslandGraph& graph,

/// True when at least one island can combined warm-start from prior data or impulses.
bool has_warm_startable_combined_islands(const ContactIslandGraph& graph,

/// Graph-level combined warm-start preflight; sets `skipped` when nothing can seed.
IslandCombinedWarmStartGraphPreflight preflight_warm_start_combined_graph(

/// Early-out guard for graph-level combined warm-start batching.
bool should_skip_warm_start_combined_graph(const ContactIslandGraph& graph,

/// Collect island indices that pass combined warm-start preflight.
std::vector<u32> collect_warm_startable_combined_island_indices(

/// Summarize combined warm-startable vs empty/no-data islands for batch guards.

/// Count islands that pass per-island combined warm-start preflight.

/// True when at least one island can seed from prior lambdas or contact impulses.



/// Collect island indices that pass per-island combined warm-start preflight.
                                                  u32 islandIndex,
                                                  f32 dt);

/// Guarded combined lambda + contact-impulse warm-start for one island.
bool warm_start_island_combined_guarded(SolverWorkBuffers& workBuffers,
/// Batch warm-start with explicit skip/warm counts (parallel prep stub).
IslandWarmStartBatchResult warm_start_all_islands_result(
    SolverWorkBuffers& workBuffers,

/// Warm-start only dispatchable (constrained) islands; skips empty islands.
u32 warm_start_dispatchable_islands_guarded(SolverWorkBuffers& workBuffers,

/// Guarded combined warm-start with explicit skip/seed outcome.
IslandWarmStartResult warm_start_island_combined_result(SolverWorkBuffers& workBuffers,
                                                        const ContactIslandGraph& graph,
                                                        u32 islandIndex,
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

/// Guarded contact-impulse warm-start by island index.
bool warm_start_island_contact_impulses_by_index_guarded(SolverWorkBuffers& workBuffers,

/// Batch guarded contact-impulse warm-start over islands with non-zero impulses.
u32 warm_start_all_islands_contact_impulses_guarded(SolverWorkBuffers& workBuffers,

/// Batch guarded contact-impulse warm-start with explicit skip/seed counts.
IslandBatchWarmStartResult warm_start_all_islands_contact_impulses_result(
    SolverWorkBuffers& workBuffers,

/// Guarded combined warm-start with explicit skip/seed outcome.
IslandWarmStartResult warm_start_island_combined_result(SolverWorkBuffers& workBuffers,
                                                        f32 dt,
                                                        const std::vector<f32>& priorDistanceLambdas,
                                                        const std::vector<f32>& priorContactLambdas = {});

/// Batch guarded combined warm-start over all islands that can seed.
u32 warm_start_all_islands_combined_guarded(SolverWorkBuffers& workBuffers,

/// Batch guarded combined warm-start with explicit skip/seed counts.
IslandBatchWarmStartResult warm_start_all_islands_combined_result(

/// True when `flags` carries `RB_SLEEPING`.
bool is_body_sleeping(u32 flags);

/// True when `flags` carries `RB_STATIC` or `RB_KINEMATIC`.
bool is_body_static_or_kinematic(u32 flags);

/// True when a body can receive constraint corrections (dynamic and awake).
bool is_body_movable(const RigidBodySoA& bodies, u32 bodyIndex);

/// Preflight island graph build inputs; sets `skipped` when nothing can partition.
IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<DistanceConstraint>& distanceConstraints);

/// Early-out guard when build inputs cannot form any constrained partition.
bool should_skip_island_build(u32 bodyCount,

/// Guarded island graph build; returns false when preflight skips build.
bool build_island_graph_guarded(ContactIslandGraph& graph,

/// Non-mutating island build skip predicate — inverse of `can_build` (B4.4 deepen follow-up pass).
bool canSkipIslandBuild(u32 bodyCount,

/// Non-mutating island build predicate — mirrors `preflight_island_build` (B4.4 deepen follow-up pass).
bool shouldRunIslandBuild(u32 bodyCount,

/// Batch guarded dispatch only when `preflightIslandDispatchReject` allows (B4.4 deepen follow-up pass).
IslandBatchDispatchResult dispatch_all_islands_with_preflight(
    RigidBodySoA& bodies,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

/// Preflight body participation for one island solve pass.
IslandSolveBodiesPreflight preflight_island_solve_bodies(const ContactIslandGraph::Island& island,
                                                         const RigidBodySoA& bodies);

/// Early-out guard when an island has no movable bodies to solve.
bool should_skip_island_solve_bodies(const ContactIslandGraph::Island& island,

/// Combined constraint-ref + body participation preflight for one island solve pass.
IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const RigidBodySoA& bodies,

/// Early-out guard when refs or body participation block island constraint solve.
bool should_skip_island_constraint_solve(const ContactIslandGraph::Island& island,

/// Preflight sleep state for one island; sets `skipped` for empty islands.
IslandSleepPreflight preflight_island_sleep(const ContactIslandGraph::Island& island,

/// Preflight sleep state by island index; out-of-range indices are marked skipped.
IslandSleepPreflight preflight_island_sleep_by_index(const ContactIslandGraph& graph,

/// Preflight wake hints for one island; sets `skipped` for empty islands.
IslandWakePreflight preflight_island_wake(const ContactIslandGraph::Island& island,

/// Preflight wake hints by island index; out-of-range indices are marked skipped.
IslandWakePreflight preflight_island_wake_by_index(const ContactIslandGraph& graph,

/// Summarize all-sleeping vs mixed vs fully-active islands for batch guards.
IslandSleepGraphStats compute_island_sleep_stats(const ContactIslandGraph& graph,

/// Graph-level sleep preflight; sets `skipped` when every island is all-sleeping or empty.
IslandSleepGraphPreflight preflight_island_sleep_graph(const ContactIslandGraph& graph,

/// Early-out guard when every constrained island is all-sleeping.
bool should_skip_island_sleep_solve_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Early-out guard when one island is all-sleeping.
bool should_skip_island_sleep_solve(const ContactIslandGraph::Island& island,

/// Summarize wakeable islands for batch guards.
IslandWakeGraphStats compute_island_wake_stats(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Graph-level wake preflight; sets `skipped` when no island needs sleeper activation.
IslandWakeGraphPreflight preflight_island_wake_graph(const ContactIslandGraph& graph,

/// Early-out guard for graph-level wake batching.
bool should_skip_island_wake_graph(const ContactIslandGraph& graph, const RigidBodySoA& bodies);

/// Early-out guard when an island does not need sleeper activation.
bool should_skip_island_wake(const ContactIslandGraph::Island& island, const RigidBodySoA& bodies);

/// Collect island indices that are not all-sleeping (parallel solve prep stub).
std::vector<u32> collect_nonsleeping_island_indices(const ContactIslandGraph& graph,

/// Collect island indices that should wake sleeping neighbors before solve.
std::vector<u32> collect_wakeable_island_indices(const ContactIslandGraph& graph,

/// Guarded wake for sleeping bodies in one island; returns false when wake is unnecessary.
bool wake_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Guarded wake by island index; returns false for out-of-range or non-wakeable islands.
bool wake_island_sleepers_by_index_guarded(RigidBodySoA& bodies,
                                           u32 islandIndex);

/// Batch guarded wake across wakeable islands; returns count of islands activated.
u32 wake_all_island_sleepers_guarded(RigidBodySoA& bodies, const ContactIslandGraph& graph);
/// Preflight warm-start for one island; returns false when island is empty or has no seed data.
bool preflight_warm_start_island(const ContactIslandGraph::Island& island,
                                 const std::vector<f32>& priorContactLambdas,
                                 IslandWarmStartPreflight& out);

/// Apply lambda warm-start only when preflight reports owned slots (guarded selective seed).
void warm_start_island_lambdas_guarded(SolverWorkBuffers& workBuffers,
                                       const IslandWarmStartPreflight& preflight);

/// Apply contact impulse warm-start only when preflight reports non-zero impulses.
void warm_start_island_contact_impulses_guarded(SolverWorkBuffers& workBuffers,

/// Preflight + guarded lambda/contact warm-start for one island (returns false when skipped).
bool preflight_and_warm_start_island(SolverWorkBuffers& workBuffers,
/// Summarize warm-start coverage across all islands for parallel seed prep.
IslandWarmStartStats compute_island_warm_start_stats(const ContactIslandGraph& graph,

/// Early-out guard when no island has prior lambda data to seed.
bool should_skip_frame_warm_start(const ContactIslandGraph& graph,

/// Collect island indices that pass warm-start preflight (parallel seed prep).
std::vector<u32> collect_warm_startable_island_indices(const ContactIslandGraph& graph,

/// Guarded per-island warm-start across all warm-startable islands; returns count seeded.
u32 warm_start_all_islands_guarded(SolverWorkBuffers& workBuffers,
/// Guarded contact impulse warm-start by island index with explicit skip/seed outcome.
                                                                const std::vector<narrowphase::ContactManifold>& contacts,
                                                                f32 dt);
/// Guarded contact impulse warm-start with explicit skip/seed outcome.
IslandWarmStartResult warm_start_island_contact_impulses_result(

/// Guarded contact impulse warm-start by island index.

/// Collect island indices with non-zero contact impulse warm-start coverage.
std::vector<u32> collect_contact_impulse_warm_startable_island_indices(


    const std::vector<narrowphase::ContactManifold>& contacts);

/// Graph-level guarded contact impulse warm-start; returns islands actually seeded.
u32 warm_start_all_contact_impulses_guarded(SolverWorkBuffers& workBuffers,

/// Batch guarded contact-impulse warm-start with explicit warmed/skipped counts.
IslandContactImpulseWarmStartBatchResult warm_start_all_contact_impulses_result(

/// Graph-level guarded contact impulse warm-start alias.
u32 warm_start_graph_contact_impulses_guarded(SolverWorkBuffers& workBuffers,
/// Preflight contact-impulse warm-start for one island; sets `skipped` for empty islands.
IslandContactImpulseWarmStartPreflight preflight_warm_start_island_contact_impulses(
    const ContactIslandGraph::Island& island,

/// Preflight contact-impulse warm-start by island index; out-of-range indices are marked skipped.
IslandContactImpulseWarmStartPreflight preflight_warm_start_island_contact_impulses_by_index(

/// Summarize contact-impulse warm-startable vs empty/no-impulse islands for batch guards.
IslandContactImpulseWarmStartStats compute_island_contact_impulse_warm_start_stats(

/// Count islands that pass per-island contact-impulse warm-start preflight.
u32 count_contact_impulse_warm_startable_islands(const ContactIslandGraph& graph,

/// True when at least one island can seed from prior contact impulses.
bool has_contact_impulse_warm_startable_islands(const ContactIslandGraph& graph,

/// Graph-level contact-impulse warm-start preflight; sets `skipped` when nothing can seed.
IslandContactImpulseWarmStartGraphPreflight preflight_warm_start_contact_impulses_graph(

/// Early-out guard for graph-level contact-impulse warm-start batching.
bool should_skip_warm_start_contact_impulses_graph(const ContactIslandGraph& graph,

/// Collect island indices that pass per-island contact-impulse warm-start preflight.

/// Early-out guard for per-island contact-impulse warm-start (empty, invalid dt, or no impulses).
bool should_skip_warm_start_contact_impulses(const ContactIslandGraph::Island& island,

/// Early-out guard for contact-impulse warm-start by island index.
bool should_skip_warm_start_contact_impulses_index(const ContactIslandGraph& graph,

IslandContactImpulseWarmStartResult warm_start_island_contact_impulses_result(

/// Guarded contact-impulse warm-start by island index; returns false for skipped islands.
bool warm_start_island_contact_impulses_by_index_guarded(

/// Graph-level guarded contact-impulse warm-start; returns islands actually seeded.
u32 warm_start_all_island_contact_impulses_guarded(

/// Preflight combined lambda + contact-impulse warm-start for one island.
IslandCombinedWarmStartPreflight preflight_warm_start_island_combined(
/// True when dt is positive for contact-impulse warm-start (aliases island solve dt guard).
bool is_valid_contact_impulse_warm_start_dt(f32 dt);

/// True when every contact index owned by the island is in range for `contacts`.
bool island_contact_indices_in_range(const ContactIslandGraph::Island& island, u32 contactCount);

/// True when every distance index owned by the island is in range for `distanceConstraints`.
bool island_distance_indices_in_range(const ContactIslandGraph::Island& island, u32 distanceCount);

/// Preflight island solve inputs; sets `skipped` for empty islands.
IslandSolveInputsPreflight preflight_island_solve_inputs(const ContactIslandGraph::Island& island,
                                                         u32 contactCount,
                                                         u32 distanceCount);

/// Preflight island solve inputs by index; out-of-range indices are marked skipped.
IslandSolveInputsPreflight preflight_island_solve_inputs_by_index(const ContactIslandGraph& graph,

/// Early-out guard when island constraint indices are out of range for solve buffers.
bool should_skip_island_solve_inputs(const ContactIslandGraph::Island& island,

IslandContactImpulsePreflight preflight_warm_start_contact_impulses(

IslandContactImpulsePreflight preflight_warm_start_contact_impulses_by_index(

/// Early-out guard for per-island contact-impulse warm-start when nothing can seed.

/// Early-out guard for contact-impulse warm-start by island index (empty or out-of-range).
bool should_skip_contact_impulse_warm_start_island_index(const ContactIslandGraph& graph, u32 islandIndex);

/// Combined lambda + contact-impulse warm-start preflight for one island.

/// Early-out guard for combined warm-start when neither path can seed.
bool should_skip_warm_start_island_combined(const ContactIslandGraph::Island& island,

/// Summarize impulse-seedable vs empty/no-impulse islands for batch guards.

u32 count_impulse_warm_startable_islands(const ContactIslandGraph& graph,

/// True when at least one island can seed from non-zero contact impulses.
bool has_impulse_warm_startable_islands(const ContactIslandGraph& graph,



std::vector<u32> collect_impulse_warm_startable_island_indices(


/// Batch guarded contact-impulse warm-start over seedable islands; returns count seeded.
u32 warm_start_all_islands_contact_impulses_guarded(
/// True when dt is positive for contact-impulse warm-start seeding.

IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses(

IslandContactImpulsePreflight preflight_warm_start_island_contact_impulses_by_index(

/// Summarize seedable vs empty/no-impulse islands for batch guards.
IslandContactImpulseStats compute_island_contact_impulse_stats(

u32 count_seedable_contact_impulse_islands(const ContactIslandGraph& graph,

/// True when at least one island can seed from non-zero warm-start impulses.
bool has_seedable_contact_impulse_islands(const ContactIslandGraph& graph,

IslandContactImpulseGraphPreflight preflight_warm_start_contact_impulses_graph(

bool should_skip_contact_impulse_warm_start_graph(const ContactIslandGraph& graph,

std::vector<u32> collect_contact_impulse_seedable_island_indices(

/// Early-out guard for per-island contact-impulse warm-start on empty islands.
bool should_skip_contact_impulse_warm_start_island(const ContactIslandGraph::Island& island);

/// Early-out guard for contact-impulse warm-start by island index (empty, out-of-range, or invalid dt).
bool should_skip_contact_impulse_warm_start_island_index(const ContactIslandGraph& graph,




IslandContactImpulseBatchResult warm_start_all_islands_contact_impulses_result(

/// Preflight contact-impulse warm-start for one island; sets `skipped` for empty or zero-impulse islands.
IslandContactImpulsePreflight preflight_island_contact_impulses(

IslandContactImpulsePreflight preflight_island_contact_impulses_by_index(


u32 count_contact_impulse_warm_startable_islands(


IslandContactImpulseGraphPreflight preflight_contact_impulse_graph(

bool should_skip_contact_impulse_graph(const ContactIslandGraph& graph,


/// Early-out guard for per-island contact-impulse warm-start (empty, zero impulse, or invalid dt).
bool should_skip_contact_impulse_island(const ContactIslandGraph::Island& island,

bool should_skip_contact_impulse_island_index(const ContactIslandGraph& graph,


/// Guarded contact-impulse warm-start by island index; returns false for out-of-range, empty, or no-impulse islands.

/// Batch guarded contact-impulse warm-start over warm-startable islands; returns count seeded.

/// Batch guarded contact-impulse warm-start with explicit skip/warm counts.
IslandBatchContactImpulseWarmStartResult warm_start_all_island_contact_impulses_result(

/// True when a contact carries a non-zero warm-start normal impulse.
bool contact_has_warm_impulse(const narrowphase::ContactManifold& contact);

/// Count owned contacts with non-zero warm-start impulse for one island.
u32 island_impulse_coverage(const ContactIslandGraph::Island& island,



/// Summarize impulse-warm-startable vs empty/no-impulse islands for batch guards.


/// True when at least one island can seed from warm-start impulses.



bool should_skip_warm_start_contact_impulses_island(const ContactIslandGraph::Island& island);

bool should_skip_warm_start_contact_impulses_island_index(const ContactIslandGraph& graph, u32 islandIndex);


IslandContactImpulseResult warm_start_island_contact_impulses_result(




/// Batch guarded combined warm-start with explicit skip/seed counts.
IslandBatchWarmStartResult warm_start_all_islands_combined_result(
    SolverWorkBuffers& workBuffers,
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

/// Batch guarded combined warm-start with explicit skip/seed counts.
IslandBatchWarmStartResult warm_start_all_islands_combined_result(
    SolverWorkBuffers& workBuffers,
    const ContactIslandGraph& graph,
    const std::vector<narrowphase::ContactManifold>& contacts,
    f32 dt,
    const std::vector<f32>& priorDistanceLambdas,
    const std::vector<f32>& priorContactLambdas = {});

/// True when a body carries `RB_SLEEPING`.
bool is_body_sleeping(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when a body is static or kinematic (non-dynamic for solve).
bool is_body_static_or_kinematic(const RigidBodySoA& bodies, u32 bodyIndex);

/// True when every body in the island carries `RB_SLEEPING`.
bool island_all_bodies_sleeping(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// True when at least one non-static body in the island is awake.
bool island_has_awake_body(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// True when a constrained island has an awake dynamic body that would receive corrections.
bool island_needs_wake(const RigidBodySoA& bodies, const ContactIslandGraph::Island& island);

/// Per-island sleep preflight for solve skip guards (B4.4 deepen follow-up).
struct IslandSleepPreflight {
    u32 bodyCount = 0;
    u32 sleepingBodyCount = 0;
    u32 awakeBodyCount = 0;
    bool allSleeping = false;
    bool skipped = false;

    bool can_skip_solve() const { return !skipped && allSleeping; }
};

/// Per-island wake preflight for selective wake-before-solve stubs (B4.4 deepen follow-up).
struct IslandWakePreflight {
    u32 constraintCount = 0;
    u32 awakeDynamicCount = 0;
    bool needsWake = false;
    bool skipped = false;

    bool should_wake() const { return !skipped && needsWake; }
};

/// Graph-level sleep/wake counts for batch guards (B4.4 deepen follow-up).
struct IslandSleepStats {
    u32 totalIslands = 0;
    u32 allSleepingCount = 0;
    u32 awakeCount = 0;
    u32 emptyCount = 0;
    u32 wakeRequiredCount = 0;
};

/// Combined constraint-solve preflight (index bounds + dt + empty island) (B4.4 deepen follow-up).
struct IslandConstraintSolvePreflight {
    u32 ownedContactCount = 0;
    u32 ownedDistanceCount = 0;
    u32 outOfRangeContactCount = 0;
    u32 outOfRangeDistanceCount = 0;
    u32 resolvableConstraintCount = 0;
    bool invalidDt = false;
    bool skipped = false;

    bool can_solve() const {
        return !skipped && !invalidDt && resolvableConstraintCount > 0u;
    }
};

/// Dispatch preflight combining graph stats, dt validity, and sleep skip (B4.4 deepen follow-up).
struct IslandDispatchSleepPreflight {
    IslandDispatchPreflight dispatch{};
    IslandSleepStats sleepStats{};
    u32 solvableCount = 0;
    bool skipped = false;

    bool can_dispatch() const { return !skipped && dispatch.can_dispatch() && solvableCount > 0u; }
};

/// Per-island solve preflight combining sleep skip and constraint bounds (B4.4 deepen follow-up).
struct IslandSolveSleepPreflight {
    IslandSleepPreflight sleep{};
    IslandWakePreflight wake{};
    IslandConstraintSolvePreflight constraints{};
    bool skipped = false;

    bool can_solve() const {
        return !skipped && constraints.can_solve() && !sleep.can_skip_solve();
    }
};

/// Batch dispatch summary including sleep-skipped islands (B4.4 deepen follow-up).
struct IslandBatchSleepDispatchResult {
    IslandBatchDispatchResult dispatch{};
    u32 sleepSkippedCount = 0;
    u32 wakeRequiredCount = 0;
};

IslandSleepPreflight preflight_island_sleep(const RigidBodySoA& bodies,
                                            const ContactIslandGraph::Island& island);

IslandSleepPreflight preflight_island_sleep_by_index(const RigidBodySoA& bodies,
                                                     const ContactIslandGraph& graph,
                                                     u32 islandIndex);

IslandWakePreflight preflight_island_wake(const RigidBodySoA& bodies,
                                          const ContactIslandGraph::Island& island);

IslandWakePreflight preflight_island_wake_by_index(const RigidBodySoA& bodies,
                                                   const ContactIslandGraph& graph,
                                                   u32 islandIndex);

IslandSleepStats compute_island_sleep_stats(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

u32 count_all_sleeping_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

u32 count_wake_required_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

bool has_solvable_islands(const RigidBodySoA& bodies, const ContactIslandGraph& graph);

bool should_skip_island_solve_for_sleep(const RigidBodySoA& bodies,
                                        const ContactIslandGraph::Island& island);

bool should_skip_island_solve_for_sleep_index(const RigidBodySoA& bodies,
                                              const ContactIslandGraph& graph,
                                              u32 islandIndex);

bool should_skip_island_solve_job_for_sleep(const RigidBodySoA& bodies, const IslandSolveJob& job);

IslandConstraintSolvePreflight preflight_island_constraint_solve(
    const ContactIslandGraph::Island& island,
    u32 contactManifoldCount,
    u32 distanceConstraintCount,
    f32 dt);

IslandConstraintSolvePreflight preflight_island_constraint_solve_by_index(
    const ContactIslandGraph& graph,
    u32 islandIndex,
    u32 contactManifoldCount,
    u32 distanceConstraintCount,
    f32 dt);

IslandSolveSleepPreflight preflight_island_solve_sleep(const RigidBodySoA& bodies,
                                                       const ContactIslandGraph::Island& island,
                                                       u32 contactManifoldCount,
                                                       u32 distanceConstraintCount,
                                                       f32 dt);

IslandSolveSleepPreflight preflight_island_solve_sleep_by_index(const RigidBodySoA& bodies,
                                                                const ContactIslandGraph& graph,
                                                                u32 islandIndex,
                                                                u32 contactManifoldCount,
                                                                u32 distanceConstraintCount,
                                                                f32 dt);

IslandDispatchSleepPreflight preflight_island_dispatch_sleep(const RigidBodySoA& bodies,
                                                             const ContactIslandGraph& graph,
                                                             f32 dt);

bool should_skip_island_dispatch_sleep(const RigidBodySoA& bodies,
                                       const ContactIslandGraph& graph,
                                       f32 dt);

std::vector<u32> collect_solvable_island_indices(const RigidBodySoA& bodies,
                                                 const ContactIslandGraph& graph);

bool dispatch_solve_island_sleep_guarded(RigidBodySoA& bodies,
                                         const ContactIslandGraph& graph,
                                         u32 islandIndex,
                                         SolverWorkBuffers& workBuffers,
                                         const std::vector<DistanceConstraint>& distanceConstraints,
                                         f32 dt,
                                         f32 contactCompliance,
                                         const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

IslandDispatchResult dispatch_solve_island_sleep_result(RigidBodySoA& bodies,
                                                        const ContactIslandGraph& graph,
                                                        u32 islandIndex,
                                                        SolverWorkBuffers& workBuffers,
                                                        const std::vector<DistanceConstraint>& distanceConstraints,
                                                        f32 dt,
                                                        f32 contactCompliance,
                                                        const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

u32 dispatch_all_islands_sleep_guarded(RigidBodySoA& bodies,
                                       const ContactIslandGraph& graph,
                                       SolverWorkBuffers& workBuffers,
                                       const std::vector<DistanceConstraint>& distanceConstraints,
                                       f32 dt,
                                       f32 contactCompliance,
                                       const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

IslandBatchSleepDispatchResult dispatch_all_islands_sleep_result(
    RigidBodySoA& bodies,
    const ContactIslandGraph& graph,
    SolverWorkBuffers& workBuffers,
    const std::vector<DistanceConstraint>& distanceConstraints,
    f32 dt,
    f32 contactCompliance,
    const std::function<f32(const RigidBodySoA&, u32)>& invMassFn);

} // namespace fuse::physics
